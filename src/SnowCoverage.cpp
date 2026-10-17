// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include <chrono>

#include "Globals.h"
#include "GameLayouts.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "ShelterTransition.h"
#include "SnowCoverage.h"
#include "SurfaceTypes.h"
#include "SurfaceProfiles.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <vector>

namespace SnowCoverage
{
	namespace
	{
		using MapFilter::CellRect;

		constexpr uint32_t kMask = kTexels - 1;
		static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the "
											  "toroidal index to be a bitmask");

		constexpr uint32_t kTotal = kTexels * kTexels;
		constexpr int32_t  kHalf = static_cast<int32_t>(kTexels / 2);
		constexpr int32_t  kUnfilled = -0x40000000;

		// Ground with no loaded land under it (worldspace edge, a cell still loading) is
		// asked again after this long instead of every frame.
		constexpr float kNoLandRetrySeconds = 2.0f;

		// Ground read while its cell was not attached yet may still carry the default
		// texture; it is read again once the cell has had time to finish.
		constexpr float kLoadingRetrySeconds = 1.0f;

		// Every texel is read again about this often, so anything misread corrects itself.
		constexpr float kAgeSeconds = 120.0f;

		// The exterior grid cell size (TES::GetGridCellCell works in these).
		constexpr float kGridCellUnits = 4096.0f;

		// Dirty regions beyond this many are merged into one.
		constexpr size_t kMaxDirtyRects = 32;

		ID3D11Texture2D*          g_texture{ nullptr };
		ID3D11ShaderResourceView* g_srv{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		bool                      g_failed{ false };

		// 255 where the ground is snow, else 0; smoothed, then opened under roofs into
		// what the GPU samples (g_upload).
		std::vector<uint8_t> g_coverage;

		std::vector<uint8_t>  g_smooth;
		std::vector<uint32_t> g_rowSums;

		std::vector<uint8_t> g_upload;
		bool                 g_uploadPending{ false };

		// The cell each texel was classified for, or kUnfilled while it waits for a probe.
		std::vector<int32_t> g_filledX;
		std::vector<int32_t> g_filledY;

		// Seconds (Now()) after which a texel is read again; 0 when no retry is pending.
		std::vector<float>    g_retryAt;
		std::vector<uint32_t> g_retries;

		// After a reset every texel is read once, nearest-first around the centre.
		// Afterwards only the texels that need it are read: those that entered the window
		// or are due a retry (in the order they were queued), then the slow re-read.
		std::vector<int16_t> g_order;
		uint32_t             g_cursor{ 0 };
		bool                 g_fullPass{ true };
		std::deque<uint32_t> g_queue;

		uint32_t g_ageCursor{ 0 };
		float    g_ageCarry{ 0.0f };

		int32_t g_centreCellX{ 0 };
		int32_t g_centreCellY{ 0 };
		bool    g_haveCentre{ false };

		// Snow texels in the window; AnySnow() is this being non-zero.
		uint32_t g_snowCount{ 0 };
		// Read by queued game-thread probes; the maps and D3D resource pointers stay on
		// the frame thread. Publish only whether there is snow after an update.
		std::atomic<bool> g_anySnow{ false };

		// Cells whose classification changed (to re-smooth), and cells to recombine with
		// the shelter.
		std::vector<CellRect> g_dirty;
		std::vector<CellRect> g_combine;
		bool                  g_combineAll{ false };

		std::vector<CellRect> g_shelterChanges;

		bool g_everFilled{ false };
		bool g_reportedComplete{ false };

		bool Fail(const char* a_why)
		{
			logger::error("SnowCoverage: {}", a_why);
			g_failed = true;
			Shutdown();
			return false;
		}

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

		void CellOf(size_t a_index, int32_t& a_cellX, int32_t& a_cellY)
		{
			const int32_t  baseX = g_centreCellX - kHalf;
			const int32_t  baseY = g_centreCellY - kHalf;
			const uint32_t tx = static_cast<uint32_t>(a_index) & kMask;
			const uint32_t ty = static_cast<uint32_t>(a_index) / kTexels;
			a_cellX = baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
			a_cellY = baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);
		}

		void SetCoverage(size_t a_index, int32_t a_cellX, int32_t a_cellY, uint8_t a_value)
		{
			const uint8_t before = g_coverage[a_index];
			if (before == a_value) {
				return;
			}
			if (before == 255) {
				--g_snowCount;
			}
			if (a_value == 255) {
				++g_snowCount;
			}
			g_coverage[a_index] = a_value;
			MapFilter::AddRect(g_dirty, CellRect{ a_cellX, a_cellY, a_cellX, a_cellY }, kMaxDirtyRects);
		}

		void ForgetAll()
		{
			g_anySnow.store(false, std::memory_order_relaxed);
			std::fill(g_coverage.begin(), g_coverage.end(), static_cast<uint8_t>(0));
			std::fill(g_smooth.begin(), g_smooth.end(), static_cast<uint8_t>(0));
			std::fill(g_upload.begin(), g_upload.end(), static_cast<uint8_t>(0));
			std::fill(g_filledX.begin(), g_filledX.end(), kUnfilled);
			std::fill(g_filledY.begin(), g_filledY.end(), kUnfilled);
			std::fill(g_retryAt.begin(), g_retryAt.end(), 0.0f);
			g_retries.clear();
			g_queue.clear();
			g_dirty.clear();
			g_combine.clear();
			g_combineAll = false;
			g_snowCount = 0;
			g_cursor = 0;
			g_fullPass = true;
			g_uploadPending = true;
			g_everFilled = false;
			g_reportedComplete = false;
		}

		// The player moved by whole texels: the cells that left the window take the texels
		// of the cells that entered, which start over as bare and unread.
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
			if (dx > 0) {
				entering[0] = { before.x1 + 1, after.y0, after.x1, after.y1 };
			} else if (dx < 0) {
				entering[0] = { after.x0, after.y0, before.x0 - 1, after.y1 };
			}
			// The second strip leaves out the corner the first already covers.
			const int32_t keptX0 = std::max(after.x0, before.x0);
			const int32_t keptX1 = std::min(after.x1, before.x1);
			if (dy > 0) {
				entering[1] = { keptX0, before.y1 + 1, keptX1, after.y1 };
			} else if (dy < 0) {
				entering[1] = { keptX0, after.y0, keptX1, before.y0 - 1 };
			}

			// The nearest-first pass orders its texels around the centre: when the centre
			// moves before it is done, the offsets it has passed now name other cells, so it
			// starts over (texels already read are skipped at no cost to the budget).
			if (g_fullPass) {
				g_cursor = 0;
			}

			for (const auto& strip : entering) {
				if (strip.Empty()) {
					continue;
				}
				for (int32_t y = strip.y0; y <= strip.y1; ++y) {
					for (int32_t x = strip.x0; x <= strip.x1; ++x) {
						const size_t index = IndexOf(x, y);
						SetCoverage(index, x, y, 0);
						g_filledX[index] = kUnfilled;
						g_filledY[index] = kUnfilled;
						g_retryAt[index] = 0.0f;
						g_queue.push_back(static_cast<uint32_t>(index));
					}
				}
				// The shelter factor of these cells changed with them.
				MapFilter::AddRect(g_combine, strip, kMaxDirtyRects);
			}
		}

		// Texels owed a re-read; paid from what the frame's budget leaves over.
		void Age(float a_dt)
		{
			if (g_fullPass) {
				g_ageCarry = 0.0f;
				return;
			}
			const float perSecond = static_cast<float>(kTotal) / kAgeSeconds;
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

		void ScheduleRetry(size_t a_index, float a_at)
		{
			g_retryAt[a_index] = a_at;
			g_retries.push_back(static_cast<uint32_t>(a_index));
		}

		void Combine(const CellRect& a_rect)
		{
			for (int32_t y = a_rect.y0; y <= a_rect.y1; ++y) {
				for (int32_t x = a_rect.x0; x <= a_rect.x1; ++x) {
					const size_t index = IndexOf(x, y);
					const float  open = 1.0f - Shelter::AtCell(x, y);
					g_upload[index] =
						static_cast<uint8_t>(static_cast<float>(g_smooth[index]) * open);
				}
			}
			g_uploadPending = true;
		}

		// Whether the exterior cell under a point has finished attaching. Asked once per
		// grid cell per frame.
		// What one update needs from the grid cells its texels fall in: whether each is
		// attached yet, and the height of its water surface.
		class GridCellCache
		{
		public:
			struct Entry
			{
				int32_t x;
				int32_t y;
				bool    attached;
				float   water;  // NaN where the cell has no water
			};

			Entry At(RE::TES* a_tes, float a_worldX, float a_worldY)
			{
				const auto gx = static_cast<int32_t>(std::floor(a_worldX / kGridCellUnits));
				const auto gy = static_cast<int32_t>(std::floor(a_worldY / kGridCellUnits));
				for (const auto& entry : _entries) {
					if (entry.x == gx && entry.y == gy) {
						return entry;
					}
				}

				const auto*  cell = a_tes->GetGridCellCell(gx, gy);
				const Entry entry{ gx, gy, !cell || GameLayouts::CellAttached(cell),
					GameLayouts::CellWaterHeight(cell) };
				if (_entries.size() < 16) {
					_entries.push_back(entry);
				}
				return entry;
			}

		private:
			std::vector<Entry> _entries;
		};
	}

	bool Ready()
	{
		return g_srv && g_sampler;
	}

	bool Initialize()
	{
		if (g_failed || Ready()) {
			return Ready();
		}

		auto* device = globals::d3d::device;
		if (!device) {
			return Fail("no device");
		}

		g_coverage.assign(kTotal, 0);
		g_smooth.assign(kTotal, 0);
		g_upload.assign(kTotal, 0);
		g_rowSums.clear();
		g_filledX.assign(kTotal, kUnfilled);
		g_filledY.assign(kTotal, kUnfilled);
		g_retryAt.assign(kTotal, 0.0f);
		if (g_order.empty()) {
			g_order = MapFilter::NearestFirst(kTexels);
		}
		ForgetAll();
		g_haveCentre = false;

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

		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = g_upload.data();
		initial.SysMemPitch = kTexels * sizeof(uint8_t);

		if (FAILED(device->CreateTexture2D(&desc, &initial, &g_texture))) {
			return Fail("CreateTexture2D failed");
		}
		if (FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_srv))) {
			return Fail("CreateShaderResourceView failed");
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return Fail("CreateSamplerState failed");
		}

		g_uploadPending = false;

		logger::info("SnowCoverage ready: {}x{} texels over {} world units ({} per texel); profile-aware raise",
			kTexels, kTexels, kWorldSize, kTexelSize);
		return true;
	}

	void Shutdown()
	{
		g_anySnow.store(false, std::memory_order_relaxed);
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};
		drop(g_sampler);
		drop(g_srv);
		drop(g_texture);
	}

	void Reset()
	{
		if (g_coverage.empty()) {
			return;
		}

		ForgetAll();
		g_haveCentre = false;
		g_ageCursor = 0;
		g_ageCarry = 0.0f;
	}

	void Reread()
	{
		if (g_coverage.empty()) {
			return;
		}
		std::fill(g_filledX.begin(), g_filledX.end(), kUnfilled);
		std::fill(g_filledY.begin(), g_filledY.end(), kUnfilled);
		g_cursor = 0;
		g_fullPass = true;
		g_reportedComplete = false;
	}

	void Update()
	{
		if (!Settings::enableSnowRaise) {
			return;
		}

		if (!Ready() && !Initialize()) {
			return;
		}

		auto* player = globals::game::player;
		auto* context = globals::d3d::context;
		auto* tes = globals::TES();
		if (!player || !context || !tes || !globals::PlayerInExterior()) {
			return;
		}

		const int64_t started = Profiler::Ticks();

		const RE::NiPoint3 position = player->GetPosition();

		const int32_t wasX = g_centreCellX;
		const int32_t wasY = g_centreCellY;
		g_centreCellX = static_cast<int32_t>(std::floor(position.x / kTexelSize));
		g_centreCellY = static_cast<int32_t>(std::floor(position.y / kTexelSize));

		if (!g_haveCentre) {
			g_haveCentre = true;
			g_combineAll = true;
		} else if (wasX != g_centreCellX || wasY != g_centreCellY) {
			Recentre(wasX, wasY);
		}

		const float now = Now();
		Age(globals::FrameDelta());
		Retry(now);

		const uint32_t budget = static_cast<uint32_t>(
			std::max(Settings::snowCoverageBudget, 1));
		uint32_t queries = 0;

		GridCellCache cells;

		const auto read = [&](size_t a_index, int32_t a_cellX, int32_t a_cellY) {
			++queries;
			g_filledX[a_index] = a_cellX;
			g_filledY[a_index] = a_cellY;
			g_retryAt[a_index] = 0.0f;

			RE::NiPoint3 probe{
				(static_cast<float>(a_cellX) + 0.5f) * kTexelSize,
				(static_cast<float>(a_cellY) + 0.5f) * kTexelSize,
				position.z
			};

			float landZ = 0.0f;
			if (!tes->GetLandHeight(probe, landZ)) {
				// No land here: bare, and asked again later instead of every frame.
				SetCoverage(a_index, a_cellX, a_cellY, 0);
				ScheduleRetry(a_index, now + kNoLandRetrySeconds);
				return;
			}

			const auto cell = cells.At(tes, probe.x, probe.y);
			if (!cell.attached) {
				ScheduleRetry(a_index, now + kLoadingRetrySeconds);
			}

			// Land under water keeps no blanket: raised, it would break through the surface
			// of a river or lake as a bank of snow standing in the water.
			if (Settings::snowSkipUnderwater && landZ < cell.water) {
				SetCoverage(a_index, a_cellX, a_cellY, 0);
				return;
			}

			probe.z = landZ;
			const auto ground = Surfaces::GroundAt(probe, landZ);
			SetCoverage(a_index, a_cellX, a_cellY, ground.type == Surfaces::Type::kSnow ? 255 : 0);
		};

		// The pass after a reset, nearest-first.
		while (g_fullPass && queries < budget) {
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
			read(index, cellX, cellY);
		}

		// Entered and retried texels, for whatever cell they stand for now.
		while (!g_queue.empty() && queries < budget) {
			const uint32_t index = g_queue.front();
			g_queue.pop_front();

			int32_t cellX = 0;
			int32_t cellY = 0;
			CellOf(index, cellX, cellY);
			read(index, cellX, cellY);
		}

		// The slow re-read of everything, with what is left.
		while (g_ageCarry >= 1.0f && queries < budget) {
			g_ageCarry -= 1.0f;
			const uint32_t index = g_ageCursor;
			g_ageCursor = (g_ageCursor + 1) % kTotal;

			// An unfilled texel the passes missed is read here too, for the cell it
			// stands for now.
			int32_t cellX = 0;
			int32_t cellY = 0;
			CellOf(index, cellX, cellY);
			read(index, cellX, cellY);
		}

		const CellRect window = WindowAround(g_centreCellX, g_centreCellY);
		const int      r = std::clamp(Settings::snowSeamTexels, 0, kHalf / 2);

		// Re-smooth around what changed; the smoothed cells then need recombining.
		for (const auto& rect : g_dirty) {
			const CellRect reach = rect.Grown(r).Clipped(window);
			MapFilter::BoxRect(g_coverage, g_smooth, kTexels, r, reach, g_rowSums);
			MapFilter::AddRect(g_combine, reach, kMaxDirtyRects);
		}
		g_dirty.clear();

		bool shelterAll = false;
		if (Shelter::TakeChanges(g_shelterChanges, shelterAll)) {
			if (shelterAll) {
				g_combineAll = true;
			}
			for (const auto& rect : g_shelterChanges) {
				MapFilter::AddRect(g_combine, rect.Clipped(window), kMaxDirtyRects);
			}
		}

		if (g_combineAll) {
			Combine(window);
		} else {
			for (const auto& rect : g_combine) {
				Combine(rect.Clipped(window));
			}
		}
		g_combine.clear();
		g_combineAll = false;

		if (g_uploadPending) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(g_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (uint32_t y = 0; y < kTexels; ++y) {
					std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
						g_upload.data() + static_cast<size_t>(y) * kTexels, kTexels);
				}
				context->Unmap(g_texture, 0);
				g_uploadPending = false;
			}
		}

		// The whole window has been read once and is on the GPU.
		const bool complete = !g_fullPass && !g_uploadPending;
		const bool firstFill = complete && !g_everFilled;
		if (complete) {
			g_everFilled = true;
		}

		Profiler::AddCpuTicks(Profiler::CpuScope::kSnowCoverage,
			Profiler::Ticks() - started);
		g_anySnow.store(g_snowCount > 0, std::memory_order_relaxed);

		if (!Settings::logSnowCoverage) {
			return;
		}

		using Clock = std::chrono::steady_clock;
		static Clock::time_point backlogSince{};
		static bool              warned{};
		const bool               caughtUp = complete && g_queue.empty();
		if (caughtUp) {
			if (firstFill || warned || !g_reportedComplete) {
				logger::info("SnowCoverage: swept clean at {} units a texel ({} snow texels, {} retries pending)",
					kTexelSize, g_snowCount, g_retries.size());
			}
			g_reportedComplete = true;
			backlogSince = {};
			warned = false;
		} else {
			g_reportedComplete = false;
			const auto clockNow = Clock::now();
			if (backlogSince == Clock::time_point{}) {
				backlogSince = clockNow;
			}
			if (!warned && clockNow - backlogSince > std::chrono::seconds(2)) {
				warned = true;
				logger::info("SnowCoverage: still catching up after 2 seconds (budget {}, {} queued)",
					budget, g_queue.size());
			}
		}
	}

	float At(float a_worldX, float a_worldY)
	{
		if (!Settings::enableSnowRaise) {
			return 1.0f;
		}

		// What the GPU samples: before anything is read (or without a map at all) that is
		// no snow, so the CPU never assumes a blanket the terrain does not show.
		if (g_upload.empty()) {
			return 0.0f;
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

		const auto texel = [](uint32_t a_x, uint32_t a_y) {
			return static_cast<float>(
					   g_upload[(static_cast<size_t>(a_y) * kTexels) + a_x]) /
			       255.0f;
		};

		const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
		const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
		return top + (bottom - top) * fy;
	}

	float GroundAt(float a_worldX, float a_worldY)
	{
		if (!Settings::enableSnowRaise || g_smooth.empty()) {
			return 0.0f;
		}

		// Nearest texel of the smoothed snow-ground map (the same wrapped layout as At's).
		const auto x = static_cast<int32_t>(std::floor(a_worldX / kTexelSize));
		const auto y = static_cast<int32_t>(std::floor(a_worldY / kTexelSize));
		return static_cast<float>(g_smooth[IndexOf(x, y)]) / 255.0f;
	}

	bool AnySnow()
	{
		return g_anySnow.load(std::memory_order_relaxed);
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
		ID3D11SamplerState*       nullSampler = nullptr;
		a_context->DSSetShaderResources(kSlot, 1, &nullSRV);
		a_context->DSSetSamplers(kSamplerSlot, 1, &nullSampler);
	}
}
