// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"
#include "ShaderState.h"

#include "StampShapes.h"

#include "Clipmap.h"
#include "DdsLoader.h"
#include "ClipmapUpdateCS.h"
#include "StampShapeAnalysisCS.h"
#include "Globals.h"
#include "Settings.h"

#include <array>
#include <cstring>

#include <filesystem>

namespace StampShapes
{
	namespace
	{
		ID3D11ShaderResourceView* g_view{ nullptr };
		bool                      g_failed{ false };
		bool                      g_analysisOwed{ false };
		ID3D11Buffer* g_pendingReadback{};
		float g_pendingDepth{}, g_pendingRimHeight{};
		void PollAnalysis(ID3D11DeviceContext* context);
		void DiscardPending()
		{
			if (g_pendingReadback) {
				g_pendingReadback->Release();
				g_pendingReadback = nullptr;
			}
		}

		enum class Band
		{
			kFlat,
			kHeap,
			kHole,
		};

		Band BandFor(float a_height, float a_depth)
		{
			const float threshold = std::max(a_depth, 1.0f) * 0.02f;
			if (a_height > threshold) {
				return Band::kHeap;
			}
			if (a_height < -threshold) {
				return Band::kHole;
			}
			return Band::kFlat;
		}

		constexpr auto kPath = L"Data/Textures/NMN_DeformableTerrain/FootprintHeight.dds";

		void Report(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_shader,
			ID3D11UnorderedAccessView* a_uav, ID3D11Buffer* a_params,
			ID3D11SamplerState* a_sampler, ID3D11Buffer* a_counts, ID3D11Buffer* a_staging);
	}

	bool Initialize()
	{
		if (g_view) {
			return true;
		}
		if (g_failed || !globals::d3d::device) {
			return false;
		}

		g_failed = true;

		const HRESULT hr = Dds::LoadTexture(globals::d3d::device, kPath, &g_view);

		if (FAILED(hr) || !g_view) {
			logger::warn(
				"Stamp shape not loaded (0x{:08X}) - marks will stay circular. Expected at "
				"Data/Textures/NMN_DeformableTerrain/FootprintHeight.dds",
				static_cast<uint32_t>(hr));
			g_view = nullptr;
			return false;
		}

		g_failed = false;
		logger::info("Stamp shape loaded");
		return true;
	}

	void Retry()
	{
		g_failed = false;
	}

	void RequestAnalysis()
	{
		DiscardPending();
		g_analysisOwed = true;
	}

	void Analyse()
	{
		if (g_pendingReadback) {
			PollAnalysis(globals::d3d::context);
			if (g_pendingReadback) { return; }
		}
		if (!g_analysisOwed) {
			return;
		}
		g_analysisOwed = false;

		if (!g_view || !Settings::logStampShape) {
			return;
		}

		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context) {
			return;
		}

		ID3D11ComputeShader* shader = nullptr;
		ID3D11Buffer*        counts = nullptr;
		ID3D11Buffer*        staging = nullptr;
		ID3D11Buffer*        params = nullptr;
		ID3D11SamplerState*  sampler = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;

		const auto cleanup = [&]() {
			if (uav) uav->Release();
			if (sampler) sampler->Release();
			if (params) params->Release();
			if (staging) staging->Release();
			if (counts) counts->Release();
			if (shader) shader->Release();
		};

		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			const HRESULT hr = ::D3DCompile(kAnalysisShader, sizeof(kAnalysisShader) - 1,
				"StampShapeAnalysis", nullptr, nullptr, "main", "cs_5_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &code, &errors);

			if (FAILED(hr) || !code) {
				logger::warn("Stamp shape analysis did not compile: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				if (errors) errors->Release();
				if (code) code->Release();
				return;
			}
			if (errors) errors->Release();

			const HRESULT created = device->CreateComputeShader(
				code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
			code->Release();
			if (FAILED(created)) {
				return;
			}
		}

		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.ByteWidth = kBuckets * sizeof(uint32_t);
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = sizeof(uint32_t);

		const std::array<uint32_t, kBuckets> zeros{};
		D3D11_SUBRESOURCE_DATA zeroed{};
		zeroed.pSysMem = zeros.data();

		if (FAILED(device->CreateBuffer(&bufferDesc, &zeroed, &counts))) {
			cleanup();
			return;
		}

		D3D11_BUFFER_DESC stagingDesc = bufferDesc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, &staging))) {
			cleanup();
			return;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kBuckets;
		if (FAILED(device->CreateUnorderedAccessView(counts, &uavDesc, &uav))) {
			cleanup();
			return;
		}

		const float grid[4] = { static_cast<float>(kGrid), static_cast<float>(kGrid),
			static_cast<float>(kBuckets), 0.0f };

		D3D11_BUFFER_DESC paramsDesc{};
		paramsDesc.ByteWidth = sizeof(grid);
		paramsDesc.Usage = D3D11_USAGE_IMMUTABLE;
		paramsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA paramsData{};
		paramsData.pSysMem = grid;
		if (FAILED(device->CreateBuffer(&paramsDesc, &paramsData, &params))) {
			cleanup();
			return;
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&samplerDesc, &sampler))) {
			cleanup();
			return;
		}

		Report(context, shader, uav, params, sampler, counts, staging);
		cleanup();
	}

	namespace
	{
		void Report(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_shader,
			ID3D11UnorderedAccessView* a_uav, ID3D11Buffer* a_params,
			ID3D11SamplerState* a_sampler, ID3D11Buffer* a_counts, ID3D11Buffer* a_staging)
		{

			ShaderState::Compute       savedShader;
			ID3D11ShaderResourceView*  savedSrv = nullptr;
			ID3D11SamplerState*        savedSampler = nullptr;
			ID3D11Buffer*              savedCb = nullptr;
			ID3D11UnorderedAccessView* savedUav = nullptr;

			savedShader.Capture(a_context);
			a_context->CSGetShaderResources(0, 1, &savedSrv);
			a_context->CSGetSamplers(0, 1, &savedSampler);
			a_context->CSGetConstantBuffers(0, 1, &savedCb);
			a_context->CSGetUnorderedAccessViews(0, 1, &savedUav);

			const UINT noOffset = static_cast<UINT>(-1);

			a_context->CSSetShader(a_shader, nullptr, 0);
			a_context->CSSetShaderResources(0, 1, &g_view);
			a_context->CSSetSamplers(0, 1, &a_sampler);
			a_context->CSSetConstantBuffers(0, 1, &a_params);
			a_context->CSSetUnorderedAccessViews(0, 1, &a_uav, &noOffset);

			a_context->Dispatch(kGrid / 8, kGrid / 8, 1);

			ID3D11UnorderedAccessView* nullUav = nullptr;
			a_context->CSSetUnorderedAccessViews(0, 1, &nullUav, &noOffset);

			savedShader.Restore(a_context);
			a_context->CSSetShaderResources(0, 1, &savedSrv);
			a_context->CSSetSamplers(0, 1, &savedSampler);
			a_context->CSSetConstantBuffers(0, 1, &savedCb);
			a_context->CSSetUnorderedAccessViews(0, 1, &savedUav, &noOffset);

			if (savedSrv) savedSrv->Release();
			if (savedSampler) savedSampler->Release();
			if (savedCb) savedCb->Release();
			if (savedUav) savedUav->Release();

			a_context->CopyResource(a_staging, a_counts);

			DiscardPending();
			g_pendingReadback = a_staging;
			g_pendingReadback->AddRef();
			g_pendingDepth = Settings::stampDepth;
			g_pendingRimHeight = Settings::stampRimHeight;
		}

		void PollAnalysis(ID3D11DeviceContext* context)
		{
			if (!context || !g_pendingReadback) { return; }
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const auto hr = context->Map(g_pendingReadback, 0, D3D11_MAP_READ,
				D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { return; }
			if (FAILED(hr)) {
				logger::warn("Stamp shape analysis could not read its result back");
				DiscardPending();
				return;
			}
			std::array<uint32_t, kBuckets> buckets{};
			std::memcpy(buckets.data(), mapped.pData, sizeof(buckets));
			context->Unmap(g_pendingReadback, 0);
			DiscardPending();

			const float total = static_cast<float>(kGrid) * static_cast<float>(kGrid);
			const float depth = g_pendingDepth;
			const float rim = depth * g_pendingRimHeight;

			float share[3]{};
			float deepest = 0.0f;
			float highest = 0.0f;
			float lowestMask = 1.0f;
			float peakMask = 0.0f;

			std::string histogram;
			for (uint32_t i = 0; i < kBuckets; ++i) {
				const float mask = (static_cast<float>(i) + 0.5f) / static_cast<float>(kBuckets);
				const float height = Clipmap::PrintHeightFor(mask, depth, rim);
				const auto  band = BandFor(height, depth);

				share[static_cast<size_t>(band)] +=
					100.0f * static_cast<float>(buckets[i]) / total;

				if (buckets[i] > 0) {
					deepest = std::min(deepest, height);
					highest = std::max(highest, height);
					lowestMask = std::min(lowestMask, mask);
					peakMask = std::max(peakMask, mask);
				}

				histogram += buckets[i] == 0 ? '.' :
				             band == Band::kHeap ? '+' :
				             band == Band::kHole ? '#' :
				                                   '-';
			}

			logger::info("Stamp shape: at StampDepth {} and StampRimHeight {} (rim {} units)",
				depth, g_pendingRimHeight, rim);
			logger::info("  mask 0.0 {} 1.0     . unused   - flat   + heap   # hole",
				histogram);
			logger::info("  {:.1f}% of the image is flat, {:.1f}% heap, {:.1f}% hole | "
						 "deepest {:.2f}, highest {:+.2f} world units",
				share[static_cast<size_t>(Band::kFlat)],
				share[static_cast<size_t>(Band::kHeap)],
				share[static_cast<size_t>(Band::kHole)], deepest, highest);

			logger::info("  the image uses mask {:.2f} to {:.2f}", lowestMask, peakMask);

			if (peakMask < 0.85f) {
				logger::warn("  the mask never gets brighter than {:.2f}. White is the "
							 "DEEPEST part of a print, so nothing in this image carves at "
							 "full depth, and {:.2f} still sits inside the rim's falloff - "
							 "the middle of the sole is being read as the outer edge of a "
							 "heap. Paint the sole solid white and keep the soft falloff "
							 "outside it.",
					peakMask, peakMask);
			}

			const float heap = share[static_cast<size_t>(Band::kHeap)];
			const float hole = share[static_cast<size_t>(Band::kHole)];

			if (rim > 0.0f && hole > 1.0f && heap < 0.5f * hole) {
				logger::warn("  the heap band is starved - only {:.1f}% of the image against "
							 "{:.1f}% hole. A hard-edged mask has no mid greys to bank from. "
							 "Blur the silhouette outward so it passes through the greys "
							 "marked + above before reaching black.",
					heap, hole);
			}

			if (hole > 0.5f && heap > 2.0f * hole) {
				logger::warn("  the heap outweighs the hole {:.1f}x. StampRimHeight is a "
							 "MULTIPLIER ON DEPTH - at {} the bank is taller than the print "
							 "is deep, which no texture can look right through. Around 0.45 "
							 "is the shipping value.",
					heap / std::max(hole, 0.01f), g_pendingRimHeight);
			}

			if (hole <= 0.5f) {
				logger::warn("  almost none of the image is dark enough to carve. White is "
							 "the DEEPEST part of a print, not the background.");
			}
		}
	}

	void Release()
	{
		DiscardPending();
		g_analysisOwed = false;
		if (g_view) {
			g_view->Release();
			g_view = nullptr;
		}
		g_failed = false;
	}

	bool Ready()
	{
		return g_view != nullptr;
	}

	ID3D11ShaderResourceView* View()
	{
		return g_view;
	}
}
