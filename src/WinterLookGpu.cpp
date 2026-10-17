// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"
#include "ShaderState.h"

#include "WinterLook.h"

#include "Globals.h"
#include "WinterLookCS.h"

#include <string>

namespace WinterLook
{
	namespace
	{
		// Textures measured in one batch, one batch in flight at a time.
		constexpr uint32_t kBatch = 16;

		// More captured textures than this wait for a later draw instead.
		constexpr size_t kMaxQueued = 64;

		// A batch is read back without waiting on the GPU; one still not ready after this many
		// frames is given up (and its textures tried again).
		constexpr uint32_t kMaxWaitFrames = 30;

		struct Params
		{
			uint32_t slot;
			uint32_t srgb;
			float    whiteLuminance;
			float    whiteSaturation;
		};

		struct Captured
		{
			uint64_t                  key;
			ID3D11ShaderResourceView* view;
			bool                      srgb;
		};

		std::mutex            g_queueLock;
		std::vector<Captured> g_queue;

		ID3D11ComputeShader*       g_shader{ nullptr };
		ID3D11Buffer*              g_results{ nullptr };
		ID3D11UnorderedAccessView* g_resultsView{ nullptr };
		ID3D11Buffer*              g_staging{ nullptr };
		ID3D11Buffer*              g_params{ nullptr };
		bool                       g_failed{ false };

		std::array<uint64_t, kBatch> g_inFlight{};
		uint32_t                     g_inFlightCount{ 0 };
		uint32_t                     g_waitedFrames{ 0 };

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		bool IsSrgb(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
				return true;
			default:
				return false;
			}
		}

		// Gives every captured texture back to be taken again by a later draw.
		void DropQueue(bool a_retry)
		{
			std::vector<Captured> queue;
			{
				const std::scoped_lock lock(g_queueLock);
				queue.swap(g_queue);
			}
			for (auto& captured : queue) {
				if (a_retry) {
					Abandon(captured.key);
				}
				Drop(captured.view);
			}
		}

		bool Fail(const char* a_why)
		{
			logger::warn("Winter look: {} - ground textures are not measured, so a winter mod's white "
						 "ground counts as snow only by its name",
				a_why);
			g_failed = true;
			return false;
		}

		bool Create(ID3D11Device* a_device)
		{
			if (g_shader) {
				return true;
			}
			if (g_failed) {
				return false;
			}

			ID3DBlob*     code = nullptr;
			ID3DBlob*     errors = nullptr;
			const HRESULT compiled = ::D3DCompile(kMeasureShader, sizeof(kMeasureShader) - 1, "NMN winter look", nullptr, nullptr,
				"main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(compiled) || !code) {
				if (errors) {
					logger::warn("Winter look: {}", static_cast<const char*>(errors->GetBufferPointer()));
				}
				Drop(errors);
				Drop(code);
				return Fail("the measuring shader did not compile");
			}
			Drop(errors);
			const HRESULT created = a_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
				nullptr, &g_shader);
			Drop(code);
			if (FAILED(created)) {
				return Fail("CreateComputeShader failed");
			}

			D3D11_BUFFER_DESC results{};
			results.ByteWidth = kBatch * 4 * sizeof(float);
			results.Usage = D3D11_USAGE_DEFAULT;
			results.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			results.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			results.StructureByteStride = 4 * sizeof(float);
			if (FAILED(a_device->CreateBuffer(&results, nullptr, &g_results))) {
				return Fail("the results buffer could not be made");
			}

			D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_UNKNOWN;
			view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			view.Buffer.NumElements = kBatch;
			if (FAILED(a_device->CreateUnorderedAccessView(g_results, &view, &g_resultsView))) {
				return Fail("the results view could not be made");
			}

			D3D11_BUFFER_DESC staging = results;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.BindFlags = 0;
			staging.MiscFlags = 0;
			staging.StructureByteStride = 0;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(a_device->CreateBuffer(&staging, nullptr, &g_staging))) {
				return Fail("the read-back buffer could not be made");
			}

			D3D11_BUFFER_DESC params{};
			params.ByteWidth = sizeof(Params);
			params.Usage = D3D11_USAGE_DYNAMIC;
			params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(a_device->CreateBuffer(&params, nullptr, &g_params))) {
				return Fail("the parameter buffer could not be made");
			}
			return true;
		}

		// The read-back of the batch in flight. Returns true when it changed some ground.
		bool Collect(ID3D11DeviceContext* a_context)
		{
			if (g_inFlightCount == 0) {
				return false;
			}

			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT read = a_context->Map(g_staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (read == DXGI_ERROR_WAS_STILL_DRAWING && ++g_waitedFrames < kMaxWaitFrames) {
				return false;
			}

			bool changed = false;
			if (SUCCEEDED(read)) {
				std::array<float, kBatch * 4> values{};
				std::memcpy(values.data(), mapped.pData, sizeof(float) * 4 * g_inFlightCount);
				a_context->Unmap(g_staging, 0);
				for (uint32_t i = 0; i < g_inFlightCount; ++i) {
					const Sample sample{ values[4 * i], values[4 * i + 1], values[4 * i + 2], values[4 * i + 3] };
					changed = Report(g_inFlight[i], sample) || changed;
				}
			} else {
				for (uint32_t i = 0; i < g_inFlightCount; ++i) {
					Abandon(g_inFlight[i]);
				}
			}
			g_inFlightCount = 0;
			g_waitedFrames = 0;
			return changed;
		}

		void Measure(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
		{
			std::vector<Captured> batch;
			{
				const std::scoped_lock lock(g_queueLock);
				const size_t take = std::min<size_t>(g_queue.size(), kBatch);
				batch.assign(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(take));
				g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(take));
			}
			if (batch.empty()) {
				return;
			}
			if (!Create(a_device)) {
				for (auto& captured : batch) {
					Abandon(captured.key);
					Drop(captured.view);
				}
				DropQueue(true);
				return;
			}

			ShaderState::Compute       savedShader;
			ID3D11ShaderResourceView*  savedView = nullptr;
			ID3D11Buffer*              savedParams = nullptr;
			ID3D11UnorderedAccessView* savedOutput = nullptr;
			savedShader.Capture(a_context);
			a_context->CSGetShaderResources(0, 1, &savedView);
			a_context->CSGetConstantBuffers(0, 1, &savedParams);
			a_context->CSGetUnorderedAccessViews(0, 1, &savedOutput);

			const UINT noOffset = static_cast<UINT>(-1);
			a_context->CSSetShader(g_shader, nullptr, 0);
			a_context->CSSetConstantBuffers(0, 1, &g_params);
			a_context->CSSetUnorderedAccessViews(0, 1, &g_resultsView, &noOffset);

			uint32_t count = 0;
			for (auto& captured : batch) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(a_context->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					Abandon(captured.key);
					continue;
				}
				const Params params{ count, captured.srgb ? 1u : 0u, kWhiteLuminance, kWhiteSaturation };
				std::memcpy(mapped.pData, &params, sizeof(params));
				a_context->Unmap(g_params, 0);

				a_context->CSSetShaderResources(0, 1, &captured.view);
				a_context->Dispatch(1, 1, 1);
				g_inFlight[count++] = captured.key;
			}

			ID3D11ShaderResourceView*  noView = nullptr;
			ID3D11UnorderedAccessView* noOutput = nullptr;
			a_context->CSSetShaderResources(0, 1, &noView);
			a_context->CSSetUnorderedAccessViews(0, 1, &noOutput, &noOffset);

			savedShader.Restore(a_context);
			a_context->CSSetShaderResources(0, 1, &savedView);
			a_context->CSSetConstantBuffers(0, 1, &savedParams);
			a_context->CSSetUnorderedAccessViews(0, 1, &savedOutput, &noOffset);
			Drop(savedView);
			Drop(savedParams);
			Drop(savedOutput);

			if (count > 0) {
				a_context->CopyResource(g_staging, g_results);
			}
			g_inFlightCount = count;
			g_waitedFrames = 0;

			for (auto& captured : batch) {
				Drop(captured.view);
			}
		}
	}

	void Capture(ID3D11DeviceContext* a_context, const uint64_t (&a_keys)[4])
	{
		if (!a_context || !Enabled() || g_failed) {
			return;
		}
		const uint32_t claimed = Seen(a_keys, globals::FrameIndex());
		if (!claimed) {
			return;
		}

		ID3D11ShaderResourceView* views[4]{};
		a_context->PSGetShaderResources(0, 4, views);

		std::vector<Captured> captured;
		for (uint32_t i = 0; i < 4; ++i) {
			if (!(claimed & (1u << i))) {
				continue;
			}
			auto* view = views[i];
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			if (view) {
				view->GetDesc(&desc);
			}
			if (!view || desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
				Abandon(a_keys[i]);
				continue;
			}
			view->AddRef();
			captured.push_back({ a_keys[i], view, IsSrgb(desc.Format) });
		}
		for (auto*& view : views) {
			Drop(view);
		}

		std::vector<Captured> overflow;
		{
			const std::scoped_lock lock(g_queueLock);
			for (auto& entry : captured) {
				(g_queue.size() < kMaxQueued ? g_queue : overflow).push_back(entry);
			}
		}
		for (auto& entry : overflow) {
			Abandon(entry.key);
			Drop(entry.view);
		}
	}

	bool Update()
	{
		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!Enabled() || !device || !context) {
			DropQueue(true);
			return false;
		}

		bool changed = Collect(context);

		const uint64_t frame = globals::FrameIndex();
		if (frame % 30 == 0) {
			changed = Reevaluate(frame) || changed;
		}

		if (g_inFlightCount == 0) {
			Measure(device, context);
		}
		return changed;
	}

	void Release()
	{
		DropQueue(false);
		if (g_inFlightCount > 0) {
			for (uint32_t i = 0; i < g_inFlightCount; ++i) {
				Abandon(g_inFlight[i]);
			}
			g_inFlightCount = 0;
		}
		Drop(g_params);
		Drop(g_staging);
		Drop(g_resultsView);
		Drop(g_results);
		Drop(g_shader);
		g_failed = false;
	}
}
