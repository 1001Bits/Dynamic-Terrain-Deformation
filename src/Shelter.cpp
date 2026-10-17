// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Shelter.h"

#include "Clipmap.h"
#include "Globals.h"
#include "Physics.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShelterTransition.h"
#include "SnowCoverage.h"
#include "SnowMeshCover.h"
#include "SnowSurface.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace Shelter
{
	namespace
	{
		// The probes run as an F4SE game-thread task, the rest of the update in the frame
		// hook; every change to the maps goes through this lock.
		std::mutex        g_stateLock;
		std::atomic<bool> g_probeQueued{ false };

		// The last batches' cost, handed to the profiler and the log by the frame hook.
		uint32_t g_batchRays{ 0 };
		int64_t  g_batchTicks{ 0 };
		double   g_batchMs{ 0.0 }, g_batchMaxMs{ 0.0 };
		uint32_t g_batches{ 0 };
		using MapFilter::CellRect;

		constexpr uint32_t kMask = kTexels - 1;
		static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the "
											  "toroidal index to be a bitmask");

		constexpr uint32_t kTotal = kTexels * kTexels;
		constexpr int32_t  kHalf = static_cast<int32_t>(kTexels / 2);
		constexpr int32_t  kUnfilled = -0x40000000;

		// A column with no loaded land under it (worldspace edge, a cell still loading) is
		// asked again after this long instead of every frame.
		constexpr float kNoLandRetrySeconds = 2.0f;

		// ShelterRefresh is the number of texels re-probed per 60th of a second.
		constexpr float kRefreshReferenceRate = 60.0f;

		// More pending map changes than this are handed over as one full recombine.
		constexpr size_t kMaxQueuedChanges = 32;

		std::vector<uint8_t> g_raw;

		std::vector<uint8_t>  g_smooth;
		std::vector<uint32_t> g_rowSums;

		std::vector<uint8_t> g_cap;
		std::vector<uint8_t> g_capSmooth;
		// The visible cap eroded by the seam radius, so smoothing spreads a cap outwards.
		std::vector<uint8_t> g_capLow;
		std::vector<float>   g_roofDisplay, g_capDisplay;
		std::vector<uint8_t> g_roofVisible, g_capVisible;
		// The top of a static mesh the blanket buries (SnowMeshCover), per texel as probed.
		std::vector<uint8_t> g_buried;

		// Texels whose display value is still blending toward its target, and a flag per
		// texel so one is never listed twice.
		std::vector<uint32_t> g_blending;
		std::vector<uint8_t>  g_isBlending;

		// Texels reset for a cell nobody has probed yet; their first result shows at once
		// instead of fading in, so a load or a jump doesn't fade snow out from under roofs.
		std::vector<uint8_t> g_fresh;

		ID3D11Texture2D*          g_texture{ nullptr };
		ID3D11ShaderResourceView* g_srv{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		bool                      g_failed{ false };
		bool                      g_capUpload{ false };

		// The cell each texel was classified for, or kUnfilled while it waits for a probe.
		std::vector<int32_t> g_filledX;
		std::vector<int32_t> g_filledY;

		// Seconds (Now()) after which a texel classified over missing land is asked again;
		// 0 when it has no retry pending.
		std::vector<float>    g_retryAt;
		std::vector<uint32_t> g_retries;

		// After a reset every texel is probed once, nearest-first around the centre.
		// Afterwards only the texels that need it are probed: those that entered the window
		// or are due a retry (in the order they were queued), then the steady re-probe.
		std::vector<int16_t> g_order;
		uint32_t             g_cursor{ 0 };
		bool                 g_fullPass{ true };
		std::deque<uint32_t> g_queue;

		uint32_t g_ageCursor{ 0 };
		float    g_ageCarry{ 0.0f };

		int32_t  g_centreCellX{ 0 };
		int32_t  g_centreCellY{ 0 };
		bool     g_haveCentre{ false };
		uint32_t g_revision{ 1 };

		// Visible cells whose smoothed values are out of date.
		std::vector<CellRect> g_roofDirty;
		std::vector<CellRect> g_capDirty;

		// AtCell changes the snow coverage has not recombined yet.
		std::vector<CellRect> g_changes;
		bool                  g_changedAll{ true };

		bool g_allocated{ false };

		float Now()
		{
			static const auto start = std::chrono::steady_clock::now();
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
		}

		size_t IndexOf(int32_t a_cellX, int32_t a_cellY)
		{
			return (static_cast<size_t>(static_cast<uint32_t>(a_cellY) & kMask) * kTexels) +
			       (static_cast<uint32_t>(a_cellX) & kMask);
		}

		CellRect WindowAround(int32_t a_cellX, int32_t a_cellY)
		{
			return { a_cellX - kHalf, a_cellY - kHalf, a_cellX + kHalf - 1, a_cellY + kHalf - 1 };
		}

		// The cell a texel stands for in the current window.
		void CellOf(size_t a_index, int32_t& a_cellX, int32_t& a_cellY)
		{
			const int32_t  baseX = g_centreCellX - kHalf;
			const int32_t  baseY = g_centreCellY - kHalf;
			const uint32_t tx = static_cast<uint32_t>(a_index) & kMask;
			const uint32_t ty = static_cast<uint32_t>(a_index) / kTexels;
			a_cellX = baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
			a_cellY = baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);
		}

		void NoteChange(const CellRect& a_rect)
		{
			if (a_rect.Empty()) {
				return;
			}
			++g_revision;
			if (g_changedAll) {
				return;
			}
			if (g_changes.size() >= kMaxQueuedChanges) {
				g_changes.clear();
				g_changedAll = true;
				return;
			}
			g_changes.push_back(a_rect);
		}

		void NoteChangedAll()
		{
			++g_revision;
			g_changes.clear();
			g_changedAll = true;
		}

		// Back to "open, not probed yet" for whatever cell the texel stands for next.
		void Forget(size_t a_index)
		{
			g_filledX[a_index] = kUnfilled;
			g_filledY[a_index] = kUnfilled;
			g_retryAt[a_index] = 0.0f;
			g_fresh[a_index] = 1;

			g_raw[a_index] = 0;
			g_roofDisplay[a_index] = 0.0f;
			g_roofVisible[a_index] = 0;
			g_cap[a_index] = 255;
			g_capDisplay[a_index] = 255.0f;
			g_capVisible[a_index] = 255;
			g_buried[a_index] = 0;
			// A listed texel now sits at its target and drops out of g_blending next frame.
		}

		void ForgetRect(const CellRect& a_rect)
		{
			for (int32_t y = a_rect.y0; y <= a_rect.y1; ++y) {
				for (int32_t x = a_rect.x0; x <= a_rect.x1; ++x) {
					const size_t index = IndexOf(x, y);
					Forget(index);
					g_queue.push_back(static_cast<uint32_t>(index));
				}
			}
		}

		void ForgetAll()
		{
			std::fill(g_filledX.begin(), g_filledX.end(), kUnfilled);
			std::fill(g_filledY.begin(), g_filledY.end(), kUnfilled);
			std::fill(g_retryAt.begin(), g_retryAt.end(), 0.0f);
			std::fill(g_fresh.begin(), g_fresh.end(), static_cast<uint8_t>(1));
			std::fill(g_isBlending.begin(), g_isBlending.end(), static_cast<uint8_t>(0));
			g_retries.clear();
			g_blending.clear();
			g_queue.clear();
			g_cursor = 0;
			g_fullPass = true;

			std::fill(g_raw.begin(), g_raw.end(), static_cast<uint8_t>(0));
			std::fill(g_roofDisplay.begin(), g_roofDisplay.end(), 0.0f);
			std::fill(g_roofVisible.begin(), g_roofVisible.end(), static_cast<uint8_t>(0));
			std::fill(g_smooth.begin(), g_smooth.end(), static_cast<uint8_t>(0));

			std::fill(g_cap.begin(), g_cap.end(), static_cast<uint8_t>(255));
			std::fill(g_capDisplay.begin(), g_capDisplay.end(), 255.0f);
			std::fill(g_capVisible.begin(), g_capVisible.end(), static_cast<uint8_t>(255));
			std::fill(g_capSmooth.begin(), g_capSmooth.end(), static_cast<uint8_t>(255));
			std::fill(g_capLow.begin(), g_capLow.end(), static_cast<uint8_t>(255));
			std::fill(g_buried.begin(), g_buried.end(), static_cast<uint8_t>(0));

			g_roofDirty.clear();
			g_capDirty.clear();
			g_capUpload = true;
			NoteChangedAll();
		}

		void Allocate()
		{
			if (g_allocated) {
				return;
			}

			g_raw.assign(kTotal, 0);
			g_smooth.assign(kTotal, 0);
			g_cap.assign(kTotal, 255);
			g_capSmooth.assign(kTotal, 255);
			g_capLow.assign(kTotal, 255);
			g_roofDisplay.assign(kTotal, 0.0f);
			g_capDisplay.assign(kTotal, 255.0f);
			g_roofVisible.assign(kTotal, 0);
			g_capVisible.assign(kTotal, 255);
			g_buried.assign(kTotal, 0);
			g_isBlending.assign(kTotal, 0);
			g_fresh.assign(kTotal, 1);
			g_filledX.assign(kTotal, kUnfilled);
			g_filledY.assign(kTotal, kUnfilled);
			g_retryAt.assign(kTotal, 0.0f);
			g_rowSums.clear();
			g_blending.clear();
			g_retries.clear();
			g_queue.clear();
			if (g_order.empty()) {
				g_order = MapFilter::NearestFirst(kTexels);
			}
			g_cursor = 0;
			g_fullPass = true;
			g_ageCursor = 0;
			g_ageCarry = 0.0f;
			g_roofDirty.clear();
			g_capDirty.clear();
			g_capUpload = true;
			g_haveCentre = false;
			NoteChangedAll();
			g_allocated = true;
		}

		// The player moved by whole texels: the cells that left the window take the
		// texels of the cells that entered, which start over as open and unprobed.
		void Recentre(int32_t a_fromX, int32_t a_fromY)
		{
			const int32_t dx = g_centreCellX - a_fromX;
			const int32_t dy = g_centreCellY - a_fromY;
			if (std::abs(dx) >= static_cast<int32_t>(kTexels) ||
				std::abs(dy) >= static_cast<int32_t>(kTexels)) {
				ForgetAll();
				return;
			}

			const CellRect before = WindowAround(a_fromX, a_fromY);
			const CellRect after = WindowAround(g_centreCellX, g_centreCellY);

			CellRect entering[2]{};
			CellRect leaving[2]{};
			if (dx > 0) {
				entering[0] = { before.x1 + 1, after.y0, after.x1, after.y1 };
				leaving[0] = { before.x0, before.y0, after.x0 - 1, before.y1 };
			} else if (dx < 0) {
				entering[0] = { after.x0, after.y0, before.x0 - 1, after.y1 };
				leaving[0] = { after.x1 + 1, before.y0, before.x1, before.y1 };
			}
			// The second entering strip leaves out the corner the first already covers.
			const int32_t keptX0 = std::max(after.x0, before.x0);
			const int32_t keptX1 = std::min(after.x1, before.x1);
			if (dy > 0) {
				entering[1] = { keptX0, before.y1 + 1, keptX1, after.y1 };
				leaving[1] = { before.x0, before.y0, before.x1, after.y0 - 1 };
			} else if (dy < 0) {
				entering[1] = { keptX0, after.y0, keptX1, before.y0 - 1 };
				leaving[1] = { before.x0, after.y1 + 1, before.x1, before.y1 };
			}

			// The nearest-first pass orders its texels around the centre: when the centre
			// moves before it is done, the offsets it has passed now name other cells, so it
			// starts over (texels already read are skipped at no cost to the budget).
			if (g_fullPass) {
				g_cursor = 0;
			}

			const int r = std::clamp(Settings::shelterSeamTexels, 0, kHalf / 2);
			for (const auto& strip : entering) {
				if (strip.Empty()) {
					continue;
				}
				ForgetRect(strip);
				const CellRect reach = strip.Grown(r).Clipped(after);
				MapFilter::AddRect(g_roofDirty, reach);
				MapFilter::AddRect(g_capDirty, reach);
			}

			// AtCell reads 0 outside the window, so the cells that left changed too.
			for (const auto& strip : leaving) {
				NoteChange(strip);
			}
		}

		// Height above the land of the first world geometry between two heights over a
		// point, or -1 when the column is clear. Actors and clutter are not on the layers
		// the ray tests, so a passer-by never reads as a roof.
		float RayHeight(RE::TESObjectCELL* a_cell, float a_worldX, float a_worldY, float a_landZ,
			float a_fromAbove, float a_toAbove, std::uint64_t a_layers = Physics::kWorldGeometry)
		{
			const RE::NiPoint3 from{ a_worldX, a_worldY, a_landZ + a_fromAbove };
			const RE::NiPoint3 to{ a_worldX, a_worldY, a_landZ + a_toAbove };

			const float fraction = Physics::CastRay(a_cell, from, to, a_layers);
			if (fraction < 0.0f) {
				return -1.0f;
			}

			return a_fromAbove + (a_toAbove - a_fromAbove) * fraction;
		}

		bool Occluded(RE::TESObjectCELL* a_cell, float a_worldX, float a_worldY, float a_landZ)
		{
			const float low = std::max(Settings::shelterClearance, 1.0f);
			return RayHeight(a_cell, a_worldX, a_worldY, a_landZ, low,
					   low + std::max(Settings::shelterHeight, 1.0f)) >= 0.0f;
		}

		SnowMeshCover::Result CapFor(RE::TESObjectCELL* a_cell, float a_worldX, float a_worldY, float a_landZ)
		{
			const float raise = std::max(Settings::snowRaiseHeight, 1.0f);

			const float low = std::max(Settings::shelterFloorTolerance, 1.0f);
			const float high = raise + low;

			// Static meshes only: on the landscape's own collision layer the ray would stop at
			// the ground itself. A road or path lies nearly flush on the land (precombined
			// Sanctuary concrete sits 3-14 units up): buried under the blanket when it is low
			// enough, else the blanket stays under it (SnowMeshCover).
			const float hit = RayHeight(a_cell, a_worldX, a_worldY, a_landZ, high, low,
				Physics::kLayerStatic | Physics::kLayerAnimStatic);
			return SnowMeshCover::Classify(hit, raise, Settings::snowCoverMeshHeight,
				Settings::snowCoverLowMeshes);
		}

		uint32_t g_roofChanges{ 0 };
		uint32_t g_capChanges{ 0 };

		// Records a probe result. A fresh texel shows it at once; a re-probe that found
		// something new blends toward it.
		void Apply(size_t a_index, int32_t a_cellX, int32_t a_cellY, uint8_t a_roof, uint8_t a_cap,
			uint8_t a_buried = 0)
		{
			g_filledX[a_index] = a_cellX;
			g_filledY[a_index] = a_cellY;
			g_buried[a_index] = a_buried;

			if (g_raw[a_index] != a_roof) {
				g_raw[a_index] = a_roof;
				++g_roofChanges;
			}
			if (g_cap[a_index] != a_cap) {
				g_cap[a_index] = a_cap;
				++g_capChanges;
			}

			if (g_fresh[a_index]) {
				g_fresh[a_index] = 0;
				g_roofDisplay[a_index] = a_roof;
				g_capDisplay[a_index] = a_cap;
				if (g_roofVisible[a_index] != a_roof) {
					g_roofVisible[a_index] = a_roof;
					MapFilter::AddRect(g_roofDirty, CellRect{ a_cellX, a_cellY, a_cellX, a_cellY });
				}
				if (g_capVisible[a_index] != a_cap) {
					g_capVisible[a_index] = a_cap;
					MapFilter::AddRect(g_capDirty, CellRect{ a_cellX, a_cellY, a_cellX, a_cellY });
				}
				return;
			}

			const bool settled = g_roofDisplay[a_index] == static_cast<float>(a_roof) &&
			                     g_capDisplay[a_index] == static_cast<float>(a_cap);
			if (!settled && !g_isBlending[a_index]) {
				g_isBlending[a_index] = 1;
				g_blending.push_back(static_cast<uint32_t>(a_index));
			}
		}

		void Blend(float a_dt)
		{
			const float alpha = ShelterTransition::Alpha(a_dt);
			if (g_blending.empty() || alpha <= 0.0f) {
				return;
			}

			size_t keep = 0;
			for (const uint32_t index : g_blending) {
				const bool roofMoving = ShelterTransition::Advance(g_roofDisplay[index], g_raw[index], alpha);
				const bool capMoving = ShelterTransition::Advance(g_capDisplay[index], g_cap[index], alpha);

				const auto roof = static_cast<uint8_t>(g_roofDisplay[index] + 0.5f);
				const auto cap = static_cast<uint8_t>(g_capDisplay[index] + 0.5f);
				if (roof != g_roofVisible[index] || cap != g_capVisible[index]) {
					int32_t cellX = 0;
					int32_t cellY = 0;
					CellOf(index, cellX, cellY);
					if (roof != g_roofVisible[index]) {
						g_roofVisible[index] = roof;
						MapFilter::AddRect(g_roofDirty, CellRect{ cellX, cellY, cellX, cellY });
					}
					if (cap != g_capVisible[index]) {
						g_capVisible[index] = cap;
						MapFilter::AddRect(g_capDirty, CellRect{ cellX, cellY, cellX, cellY });
					}
				}

				if (roofMoving || capMoving) {
					g_blending[keep++] = index;
				} else {
					g_isBlending[index] = 0;
				}
			}
			g_blending.resize(keep);
		}

		// Texels owed a re-probe at a steady rate (texels per second), so a roof built or
		// removed later is noticed whatever the frame rate. Paid from what the frame's
		// budget leaves over.
		void Age(float a_dt)
		{
			if (g_fullPass) {
				g_ageCarry = 0.0f;
				return;
			}
			const float perSecond =
				static_cast<float>(std::max(Settings::shelterRefresh, 0)) * kRefreshReferenceRate;
			g_ageCarry = std::min(g_ageCarry + perSecond * std::max(a_dt, 0.0f),
				static_cast<float>(kTotal / 4));
		}

		void Retry(float a_now)
		{
			for (size_t i = 0; i < g_retries.size();) {
				const uint32_t index = g_retries[i];
				const float    due = g_retryAt[index];
				if (due != 0.0f && a_now < due) {
					++i;
					continue;
				}
				if (due != 0.0f) {
					g_retryAt[index] = 0.0f;
					g_queue.push_back(index);
				}
				g_retries[i] = g_retries.back();
				g_retries.pop_back();
			}
		}

		void UploadCap()
		{
			if (!g_capUpload || !Ready()) {
				return;
			}

			auto* context = globals::d3d::context;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (context && SUCCEEDED(context->Map(
							   g_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (uint32_t y = 0; y < kTexels; ++y) {
					std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
						g_capSmooth.data() + static_cast<size_t>(y) * kTexels, kTexels);
				}
				context->Unmap(g_texture, 0);
				g_capUpload = false;
			}
		}
	}

	bool Ready()
	{
		return g_texture && g_srv && g_sampler;
	}

	bool Initialize()
	{
		if (g_failed || Ready()) {
			return Ready();
		}

		auto* device = globals::d3d::device;
		if (!device) {
			return false;
		}

		Allocate();

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTexels;
		desc.Height = kTexels;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		// The cap may have been probed before the texture existed; smooth all of it now.
		{
			const int r = std::clamp(Settings::shelterSeamTexels, 0, kHalf / 2);
			const CellRect all{ 0, 0, static_cast<int32_t>(kTexels) - 1, static_cast<int32_t>(kTexels) - 1 };
			MapFilter::MinRect(g_capVisible, g_capLow, kTexels, r, all);
			MapFilter::BoxRect(g_capLow, g_capSmooth, kTexels, r, all, g_rowSums);
			g_capDirty.clear();
		}

		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = g_capSmooth.data();
		initial.SysMemPitch = kTexels * sizeof(uint8_t);

		if (FAILED(device->CreateTexture2D(&desc, &initial, &g_texture))) {
			g_failed = true;
			logger::warn("Shelter: CreateTexture2D failed - the mesh cap is off");
			return false;
		}

		if (FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_srv))) {
			g_failed = true;
			logger::warn("Shelter: CreateShaderResourceView failed - the mesh cap is off");
			return false;
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			g_failed = true;
			logger::warn("Shelter: CreateSamplerState failed - the mesh cap is off");
			return false;
		}

		g_capUpload = false;

		logger::info("Shelter ready: {}x{} texels over {} world units ({} per texel); 0.35s temporal response",
			kTexels, kTexels, kWorldSize, kTexelSize);
		return true;
	}

	ID3D11ShaderResourceView* View()
	{
		return Ready() ? g_srv : nullptr;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !Ready()) {
			return;
		}
		a_context->DSSetShaderResources(kSlot, 1, &g_srv);
		a_context->DSSetSamplers(kSamplerSlot, 1, &g_sampler);
	}

	void UnbindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context) {
			return;
		}
		ID3D11ShaderResourceView* nullSRV = nullptr;
		a_context->DSSetShaderResources(kSlot, 1, &nullSRV);
	}

	void Shutdown()
	{
		const std::scoped_lock lock(g_stateLock);
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};
		drop(g_srv);
		drop(g_sampler);
		drop(g_texture);
		g_failed = false;

		g_roofDisplay.clear();
		g_capDisplay.clear();
		g_roofVisible.clear();
		g_capVisible.clear();
		g_buried.clear();
		g_blending.clear();
		g_isBlending.clear();
		g_fresh.clear();
		g_cap.clear();
		g_capSmooth.clear();
		g_capLow.clear();
		g_raw.clear();
		g_smooth.clear();
		g_rowSums.clear();
		g_filledX.clear();
		g_filledY.clear();
		g_retryAt.clear();
		g_retries.clear();
		g_queue.clear();
		g_allocated = false;
		g_haveCentre = false;
		g_roofDirty.clear();
		g_capDirty.clear();
		NoteChangedAll();
	}

	void Reset()
	{
		const std::scoped_lock lock(g_stateLock);
		if (!g_allocated) {
			return;
		}

		ForgetAll();
		g_ageCursor = 0;
		g_ageCarry = 0.0f;
		g_haveCentre = false;
		g_roofChanges = 0;
		g_capChanges = 0;
	}

	uint32_t Revision()
	{
		return g_revision;
	}

	bool TakeChanges(std::vector<MapFilter::CellRect>& a_out, bool& a_all)
	{
		a_all = g_changedAll;
		a_out.assign(g_changes.begin(), g_changes.end());
		g_changes.clear();
		g_changedAll = false;
		return a_all || !a_out.empty();
	}

	float CapAt(float a_worldX, float a_worldY)
	{
		const std::scoped_lock lock(g_stateLock);
		if (!Settings::enableShelter || !Settings::shelterMeshCap || !g_allocated ||
			!g_haveCentre || g_capVisible.empty()) {
			return 1.0f;
		}

		const float u = (a_worldX / kWorldSize) * static_cast<float>(kTexels) - 0.5f;
		const float v = (a_worldY / kWorldSize) * static_cast<float>(kTexels) - 0.5f;

		const float u0f = std::floor(u);
		const float v0f = std::floor(v);

		const auto warp = [](float a_f) { return a_f * a_f * (3.0f - 2.0f * a_f); };
		const float fx = warp(u - u0f);
		const float fy = warp(v - v0f);

		const uint32_t x0 = static_cast<uint32_t>(static_cast<int32_t>(u0f)) & kMask;
		const uint32_t y0 = static_cast<uint32_t>(static_cast<int32_t>(v0f)) & kMask;
		const uint32_t x1 = (x0 + 1) & kMask;
		const uint32_t y1 = (y0 + 1) & kMask;

		// The eroded and smoothed cap the terrain shader samples, so feet and objects meet
		// the blanket where it is drawn (the raw cap before the texture exists).
		const auto& source = Ready() && g_capSmooth.size() == g_capVisible.size() ? g_capSmooth : g_capVisible;
		const auto  texel = [&](uint32_t a_x, uint32_t a_y) {
			return static_cast<float>(source[(static_cast<size_t>(a_y) * kTexels) + a_x]) / 255.0f;
		};

		const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
		const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
		float       cap = top + (bottom - top) * fy;

		float centreX = 0.0f, centreY = 0.0f, halfExtent = 0.0f;
		if (Clipmap::GetWindow(centreX, centreY, halfExtent)) {
			const float reach = std::max(std::fabs(a_worldX - centreX),
				std::fabs(a_worldY - centreY));
			const float start = kWorldSize * 0.39f;
			const float end = kWorldSize * 0.47f;
			const float release =
				std::clamp((reach - start) / std::max(end - start, 1e-3f), 0.0f, 1.0f);
			cap = cap + (1.0f - cap) * release;
		}

		return std::clamp(cap, 0.0f, 1.0f);
	}

	float BuriedAt(float a_worldX, float a_worldY)
	{
		float height = 0.0f;
		{
		const std::scoped_lock lock(g_stateLock);
		if (!Settings::enableShelter || !Settings::shelterMeshCap || !Settings::snowCoverLowMeshes ||
			!g_allocated || !g_haveCentre || g_buried.empty()) {
			return 0.0f;
		}

		// The highest of the four texels around the point: a foot or a shot near a road's
		// edge still finds the road.
		const float u = (a_worldX / kWorldSize) * static_cast<float>(kTexels) - 0.5f;
		const float v = (a_worldY / kWorldSize) * static_cast<float>(kTexels) - 0.5f;
		const uint32_t x0 = static_cast<uint32_t>(static_cast<int32_t>(std::floor(u))) & kMask;
		const uint32_t y0 = static_cast<uint32_t>(static_cast<int32_t>(std::floor(v))) & kMask;
		const uint32_t x1 = (x0 + 1) & kMask;
		const uint32_t y1 = (y0 + 1) & kMask;
		const auto     at = [](uint32_t a_x, uint32_t a_y) {
			return g_buried[(static_cast<size_t>(a_y) * kTexels) + a_x];
		};
		const uint8_t top = std::max({ at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1) });
		height = top ? SnowMeshCover::BuriedHeight(top, Settings::snowRaiseHeight) : 0.0f;
		}
		// The stored result uses the configured maximum blanket. Weather, shelter and
		// ground coverage can lower today's blanket below the mesh. LiftAt samples CapAt,
		// so release the state lock before asking it.
		return height > 0.0f && SnowSurface::LiftAt(a_worldX, a_worldY) >= height + SnowMeshCover::kMinCover ?
		           height : 0.0f;
	}

	float AtCell(int32_t a_cellX, int32_t a_cellY)
	{
		if (!Settings::enableShelter || !g_allocated || !g_haveCentre) {
			return 0.0f;
		}

		if (a_cellX < g_centreCellX - kHalf || a_cellX >= g_centreCellX + kHalf ||
			a_cellY < g_centreCellY - kHalf || a_cellY >= g_centreCellY + kHalf) {
			return 0.0f;
		}

		return static_cast<float>(g_smooth[IndexOf(a_cellX, a_cellY)]) * (1.0f / 255.0f);
	}

	namespace
	{
		// One frame's probes, on the game thread through the F4SE task queue. The frame hook
		// runs while Havok may be stepping: a pick there waits for the whole step (about
		// 3 ms per frame beside 30 settling rifles), while from the task it takes
		// microseconds, as Rain Drops FO4 and the test driver cast theirs.
		void ProbeBatch()
		{
			struct Done
			{
				~Done() { g_probeQueued.store(false, std::memory_order_release); }
			} done;

			if (!Settings::enableShelter || !Settings::enableSnowRaise) {
				return;
			}
			auto* player = globals::game::player;
			auto* tes = globals::TES();
			auto* cell = globals::PlayerCell();
			if (!player || !tes || !cell || !cell->IsExterior() || !Physics::Available()) {
				return;
			}
			// Roofs and caps only shape the blanket: with no snow ground in reach there is
			// nothing to probe for. Texels entered meanwhile wait in the queue.
			if (!SnowCoverage::AnySnow()) {
				return;
			}

			const std::scoped_lock lock(g_stateLock);
			if (!g_allocated || !g_haveCentre) {
				return;
			}

			const int64_t      started = Profiler::Ticks();
			const auto         begun = std::chrono::steady_clock::now();
			const RE::NiPoint3 position = player->GetPosition();
			const float        now = Now();

			const uint32_t budget = static_cast<uint32_t>(std::max(Settings::shelterBudget, 1));
			const uint32_t raysPerCell = Settings::shelterMeshCap && Ready() ? 2u : 1u;
			const uint32_t limit = std::max(budget, raysPerCell);
			uint32_t       rays = 0;

			// A CPU time budget on top of the ray budget; at least one texel per batch.
			const auto deadline = begun +
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<float, std::milli>(Settings::shelterTimeBudget));
			const auto mayProbe = [&] {
				return rays + raysPerCell <= limit && (rays == 0 || std::chrono::steady_clock::now() < deadline);
			};

			const auto probe = [&](size_t a_index, int32_t a_cellX, int32_t a_cellY) {
				const RE::NiPoint3 at{
					(static_cast<float>(a_cellX) + 0.5f) * kTexelSize,
					(static_cast<float>(a_cellY) + 0.5f) * kTexelSize,
					position.z
				};

				float landZ = 0.0f;
				if (!tes->GetLandHeight(at, landZ)) {
					// No land to shelter: open, and asked again later instead of every frame.
					++rays;
					Apply(a_index, a_cellX, a_cellY, 0, 255);
					g_retryAt[a_index] = now + kNoLandRetrySeconds;
					g_retries.push_back(static_cast<uint32_t>(a_index));
					return;
				}
				g_retryAt[a_index] = 0.0f;

				const bool occluded = Occluded(cell, at.x, at.y, landZ);
				++rays;

				SnowMeshCover::Result cover{};
				if (Settings::shelterMeshCap && Ready()) {
					cover = CapFor(cell, at.x, at.y, landZ);
					++rays;
				}

				Apply(a_index, a_cellX, a_cellY, occluded ? 255 : 0, cover.cap, cover.buried);
			};

			// The pass after a reset, nearest-first.
			while (g_fullPass && mayProbe()) {
				const uint32_t k = g_cursor++;
				if (g_cursor >= kTotal) {
					g_cursor = 0;
					g_fullPass = false;
				}

				const int32_t cellX = g_centreCellX + g_order[2 * static_cast<size_t>(k)];
				const int32_t cellY = g_centreCellY + g_order[2 * static_cast<size_t>(k) + 1];
				const size_t  index = IndexOf(cellX, cellY);
				if (g_filledX[index] == cellX && g_filledY[index] == cellY) {
					continue;
				}
				probe(index, cellX, cellY);
			}

			// Entered and retried texels, for whatever cell they stand for now.
			while (!g_queue.empty() && mayProbe()) {
				const uint32_t index = g_queue.front();
				g_queue.pop_front();

				int32_t cellX = 0;
				int32_t cellY = 0;
				CellOf(index, cellX, cellY);
				probe(index, cellX, cellY);
			}

			// The steady re-probe of everything, with what is left.
			while (g_ageCarry >= 1.0f && mayProbe()) {
				g_ageCarry -= 1.0f;
				const uint32_t index = g_ageCursor;
				g_ageCursor = (g_ageCursor + 1) % kTotal;

				// An unfilled texel the passes missed is read here too, for the cell it
				// stands for now.
				int32_t cellX = 0;
				int32_t cellY = 0;
				CellOf(index, cellX, cellY);
				probe(index, cellX, cellY);
			}

			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
			g_batchRays += rays;
			g_batchTicks += Profiler::Ticks() - started;
			g_batchMs += ms;
			g_batchMaxMs = std::max(g_batchMaxMs, ms);
			++g_batches;
		}
	}

	void Update()
	{
		if (!Settings::enableShelter || !Settings::enableSnowRaise) {
			return;
		}

		auto* player = globals::game::player;
		auto* tes = globals::TES();
		auto* cell = globals::PlayerCell();
		if (!player || !tes || !cell || !cell->IsExterior() || !Physics::Available()) {
			return;
		}

		const std::scoped_lock lock(g_stateLock);

		Allocate();

		if (Settings::shelterMeshCap) {
			Initialize();
		}

		const int64_t started = Profiler::Ticks();
		const auto    updateStarted = std::chrono::steady_clock::now();

		const RE::NiPoint3 position = player->GetPosition();

		const int32_t wasX = g_centreCellX;
		const int32_t wasY = g_centreCellY;
		g_centreCellX = static_cast<int32_t>(std::floor(position.x / kTexelSize));
		g_centreCellY = static_cast<int32_t>(std::floor(position.y / kTexelSize));

		if (!g_haveCentre) {
			// Every texel is already open and unprobed (Allocate or Reset).
			g_haveCentre = true;
			NoteChangedAll();
		} else if (wasX != g_centreCellX || wasY != g_centreCellY) {
			Recentre(wasX, wasY);
		}

		const float dt = globals::FrameDelta();
		const float now = Now();

		// No steady re-probe while there is no snow to shape (see ProbeBatch).
		const bool snow = SnowCoverage::AnySnow();
		if (snow) {
			Age(dt);
		}
		Retry(now);

		Blend(dt);

		const CellRect window = WindowAround(g_centreCellX, g_centreCellY);
		const int      r = std::clamp(Settings::shelterSeamTexels, 0, kHalf / 2);

		for (const auto& rect : g_roofDirty) {
			const CellRect reach = rect.Grown(r).Clipped(window);
			MapFilter::BoxRect(g_roofVisible, g_smooth, kTexels, r, reach, g_rowSums);
			NoteChange(reach);
		}
		g_roofDirty.clear();

		if (Ready()) {
			// Erode, then smooth: a static's low cap reaches its own edges, and the blanket
			// ramps back up outside it instead of over it.
			for (const auto& rect : g_capDirty) {
				MapFilter::MinRect(g_capVisible, g_capLow, kTexels, r, rect.Grown(r).Clipped(window));
				const CellRect reach = rect.Grown(2 * r).Clipped(window);
				MapFilter::BoxRect(g_capLow, g_capSmooth, kTexels, r, reach, g_rowSums);
				g_capUpload = true;
			}
		}
		g_capDirty.clear();

		UploadCap();

		// Queue the next batch of probes for the game thread (one at a time).
		if (snow && !g_probeQueued.exchange(true, std::memory_order_acq_rel)) {
			if (const auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([] { ProbeBatch(); });
			} else {
				g_probeQueued.store(false, std::memory_order_release);
			}
		}

		static uint32_t reportedRays = 0;
		static double   updateMs = 0.0;
		static uint32_t timedFrames = 0;
		reportedRays += g_batchRays;
		const int64_t batchTicks = g_batchTicks;
		g_batchRays = 0;
		g_batchTicks = 0;

		static auto reportAt = std::chrono::steady_clock::now();
		const auto  reportNow = std::chrono::steady_clock::now();
		if (reportNow - reportAt >= std::chrono::seconds(5)) {
			if (Settings::logSnowCoverage) {
				// Over the whole map: low static meshes the blanket buries, and higher ones it
				// stays under.
				uint32_t buried = 0;
				uint32_t capped = 0;
				for (size_t i = 0; i < g_buried.size() && i < g_cap.size(); ++i) {
					buried += g_buried[i] != 0 ? 1u : 0u;
					capped += g_cap[i] != 255 ? 1u : 0u;
				}
				logger::info("Snow over static meshes: {} texels buried under the blanket, {} with the "
							 "blanket kept below a higher mesh (of {})",
					buried, capped, kTotal);
				logger::info("Shelter transitions: {} rays, {} roof target changes, {} cap target changes; "
							 "blending={} queued={} retries={} | {} probe batches on the game thread, "
							 "{:.3f} ms each (max {:.3f}); frame-hook update {:.3f} ms per frame",
					reportedRays, g_roofChanges, g_capChanges, g_blending.size(), g_queue.size(),
					g_retries.size(), g_batches, g_batches ? g_batchMs / g_batches : 0.0, g_batchMaxMs,
					updateMs / std::max<double>(timedFrames, 1.0));
			}
			reportedRays = g_roofChanges = g_capChanges = 0;
			g_batchMs = g_batchMaxMs = 0.0;
			g_batches = 0;
			updateMs = 0.0;
			timedFrames = 0;
			reportAt = reportNow;
		}
		updateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - updateStarted).count();
		++timedFrames;
		// The probes' game-thread time counts toward the shelter's CPU cost here.
		Profiler::AddCpuTicks(Profiler::CpuScope::kShelter, Profiler::Ticks() - started + batchTicks);
	}
}
