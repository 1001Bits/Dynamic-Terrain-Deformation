// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "FrameCamera.h"

#include "Globals.h"
#include "Settings.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>

namespace FrameCamera
{
	namespace
	{
		enum class Source : int
		{
			kLandscape,
			kAfterPass,
		};

		// Copies on their way back to the CPU. Each is mapped without waiting, so one the GPU
		// has not reached yet is tried again next frame; with all of them in flight a frame
		// sends none.
		constexpr uint32_t kReadbacks = 3;

		// A camera read back longer ago than this (a loading screen, a menu) is not used.
		constexpr uint64_t kMaxAgeFrames = 30;

		struct Readback
		{
			ID3D11Buffer* staging{ nullptr };
			UINT          size{ 0 };
			uint64_t      frame{ 0 };
			Source        source{ Source::kLandscape };
			bool          pending{ false };
		};

		// Fallout 4 gives each rendering thread its own context, so two threads can route
		// landscape at once; the first to arrive in a frame takes the copy.
		std::mutex            g_lock;
		ID3D11Buffer*         g_copy{ nullptr };
		UINT                  g_copySize{ 0 };
		std::atomic<uint64_t> g_copyFrame{ ~0ull };
		Source                g_copySource{ Source::kLandscape };

		Readback g_readbacks[kReadbacks];
		Camera   g_latest{};
		uint64_t g_latestFrame{ 0 };
		bool     g_haveLatest{ false };

		bool g_reported[2]{};
		bool g_rejectReported{ false };
		bool g_active{ false };  // immediate-context thread only

		bool Wanted()
		{
			return !REL::Module::IsVR() &&
			       (Settings::useClipmap ||
			        Settings::indoorPrints || (Settings::snowOnObjects && Settings::enableSnowRaise) || Settings::snowSparkle);
		}

		void Take(ID3D11DeviceContext* a_context, ID3D11Buffer* a_source, Source a_from)
		{
			const uint64_t frame = globals::FrameIndex();
			if (!a_context || !a_source || g_copyFrame.load(std::memory_order_acquire) == frame) {
				return;
			}

			const std::scoped_lock lock(g_lock);
			if (g_copyFrame.load(std::memory_order_relaxed) == frame) {
				return;
			}

			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			if (desc.ByteWidth < kMinFloats * sizeof(float)) {
				return;
			}

			if (!g_copy || g_copySize != desc.ByteWidth) {
				if (g_copy) {
					g_copy->Release();
					g_copy = nullptr;
				}
				g_copySize = 0;

				D3D11_BUFFER_DESC copyDesc{};
				copyDesc.ByteWidth = desc.ByteWidth;
				copyDesc.Usage = D3D11_USAGE_DEFAULT;
				copyDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				if (FAILED(globals::d3d::device->CreateBuffer(&copyDesc, nullptr, &g_copy))) {
					g_copy = nullptr;
					return;
				}
				g_copySize = desc.ByteWidth;
			}

			a_context->CopyResource(g_copy, a_source);
			g_copySource = a_from;
			g_copyFrame.store(frame, std::memory_order_release);
		}

		// Once for each place a copy is taken: where the eye was, how far from the player and
		// how wide the view, so a log shows both agree on the world camera.
		void Report(const Camera& a_camera, Source a_from)
		{
			auto& reported = g_reported[static_cast<int>(a_from)];
			if (reported) {
				return;
			}
			reported = true;
			float away = -1.0f;
			if (auto* player = globals::game::player) {
				const RE::NiPoint3 at = player->GetPosition();
				const float        dx = a_camera.eye[0] - at.x;
				const float        dy = a_camera.eye[1] - at.y;
				const float        dz = a_camera.eye[2] - at.z;
				away = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			const float across = 2.0f * std::atan(1.0f / a_camera.proj[0]) * 180.0f / std::numbers::pi_v<float>;
			logger::info("Frame camera ({}): eye ({:.0f}, {:.0f}, {:.0f}), {:.0f} units from the player, {:.0f} "
						 "degrees across",
				a_from == Source::kLandscape ? "a landscape draw" : "after the G-buffer pass", a_camera.eye[0],
				a_camera.eye[1], a_camera.eye[2], away, across);
		}

		// The copies the GPU has finished are read; this frame's copy is sent after them.
		void ReadBack(ID3D11DeviceContext* a_context, uint64_t a_frame)
		{
			const std::scoped_lock lock(g_lock);
			for (auto& readback : g_readbacks) {
				if (!readback.pending) {
					continue;
				}
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const HRESULT            hr =
					a_context->Map(readback.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
					continue;
				}
				readback.pending = false;
				if (FAILED(hr)) {
					continue;
				}
				Camera     camera{};
				const bool parsed = Parse(static_cast<const float*>(mapped.pData), readback.size / sizeof(float), camera);
				a_context->Unmap(readback.staging, 0);
				if (!parsed) {
					if (!g_rejectReported) {
						g_rejectReported = true;
						logger::warn("Frame camera: the per-frame constants do not read as a perspective camera "
									 "(view rows or position adjust moved?); nothing is hidden from the view");
					}
					continue;
				}
				if (!g_haveLatest || readback.frame >= g_latestFrame) {
					g_latest = camera;
					g_latestFrame = readback.frame;
					g_haveLatest = true;
					Report(camera, readback.source);
				}
			}

			if (!g_copy || g_copyFrame.load(std::memory_order_relaxed) != a_frame) {
				return;
			}
			for (auto& readback : g_readbacks) {
				if (readback.pending) {
					continue;
				}
				if (!readback.staging || readback.size != g_copySize) {
					if (readback.staging) {
						readback.staging->Release();
						readback.staging = nullptr;
					}
					readback.size = 0;
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = g_copySize;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, &readback.staging))) {
						readback.staging = nullptr;
						return;
					}
					readback.size = g_copySize;
				}
				a_context->CopyResource(readback.staging, g_copy);
				readback.frame = a_frame;
				readback.source = g_copySource;
				readback.pending = true;
				return;
			}
		}
	}

	void Note(ID3D11DeviceContext* a_context, ID3D11Buffer* a_source)
	{
		if (Wanted()) {
			Take(a_context, a_source, Source::kLandscape);
		}
	}

	void AfterPrePass(ID3D11DeviceContext* a_context)
	{
		if (!Wanted()) {
			if (g_active) {
				Reset();
				g_active = false;
			}
			return;
		}
		g_active = true;
		if (!a_context || !globals::d3d::device) {
			return;
		}
		const uint64_t frame = globals::FrameIndex();
		if (g_copyFrame.load(std::memory_order_acquire) != frame) {
			ID3D11Buffer* bound = nullptr;
			a_context->VSGetConstantBuffers(kSlot, 1, &bound);
			if (!bound) {
				a_context->PSGetConstantBuffers(kSlot, 1, &bound);
			}
			if (bound) {
				Take(a_context, bound, Source::kAfterPass);
				bound->Release();
			}
		}
		ReadBack(a_context, frame);
	}

	ID3D11Buffer* Constants()
	{
		return Wanted() && g_copyFrame.load(std::memory_order_acquire) == globals::FrameIndex() ? g_copy : nullptr;
	}

	bool Latest(Camera& a_out, uint64_t* a_frame)
	{
		if (!Wanted()) {
			return false;
		}
		const std::scoped_lock lock(g_lock);
		if (!g_haveLatest || globals::FrameIndex() > g_latestFrame + kMaxAgeFrames) {
			return false;
		}
		a_out = g_latest;
		if (a_frame) {
			*a_frame = g_latestFrame;
		}
		return true;
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_copyFrame.store(~0ull, std::memory_order_release);
		g_haveLatest = false;
		for (auto& readback : g_readbacks) {
			readback.pending = false;
		}
		g_reported[0] = g_reported[1] = false;
		g_rejectReported = false;
	}
}
