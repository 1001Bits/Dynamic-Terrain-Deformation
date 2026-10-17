// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "GroundDecals.h"

#include "Clipmap.h"
#include "Globals.h"
#include "GroundDecalFit.h"
#include "Profiler.h"
#include "Settings.h"
#include "SnowSurface.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace GroundDecals
{
	namespace
	{
		std::atomic<int> g_budget{ 0 };

		std::atomic<uint32_t> g_logged{ 0 };
		constexpr uint32_t    kMaxLogged = 16;
	}

	void NewFrame()
	{
		g_budget.store(std::max(Settings::maxBloodDecalsPerFrame, 0), std::memory_order_relaxed);
	}

	void Fit(float* a_input)
	{
		if (!a_input || !Settings::enableBloodDecals || g_budget.load(std::memory_order_relaxed) <= 0 ||
			!Settings::enableTessellation || !Settings::useClipmap || !Clipmap::Ready() || !globals::PlayerInExterior()) {
			return;
		}

		// A decal riding on a moving object keeps the engine's box: its origin is relative to
		// that object, and no ground lies under it.
		std::uint32_t dynamic = 0;
		std::memcpy(&dynamic, a_input + GroundDecalFit::kDynamic, sizeof(dynamic));
		if (dynamic != 0) {
			return;
		}
		Profiler::Tally(Profiler::Count::kDecalsSeen);

		const RE::NiPoint3 origin{ a_input[GroundDecalFit::kOriginX], a_input[GroundDecalFit::kOriginY],
			a_input[GroundDecalFit::kOriginZ] };
		const float vertical = std::fabs(a_input[GroundDecalFit::kRotation22]);
		if (!(vertical >= GroundDecalFit::kMinVertical && vertical <= 1.001f) ||
			!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
			return;
		}

		// Marks only exist inside the field's window and the blanket only within its own
		// distance; past both the ground is the game's own.
		float centreX = 0.0f;
		float centreY = 0.0f;
		float halfExtent = 0.0f;
		if (!Clipmap::GetWindow(centreX, centreY, halfExtent)) {
			return;
		}
		const float reach = std::max(std::fabs(origin.x - centreX), std::fabs(origin.y - centreY));
		const bool  inField = reach <= halfExtent;
		const bool  inBlanket = Settings::enableSnowRaise && reach <= Settings::snowRaiseDistance;
		if (!inField && !inBlanket) {
			return;
		}
		// Bound the costly terrain queries, including rejected decals. Counting only
		// successful fits left a room full of wall decals completely unbounded.
		int remaining = g_budget.load(std::memory_order_relaxed);
		while (remaining > 0 && !g_budget.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
		}
		if (remaining <= 0) {
			return;
		}

		const int64_t start = Profiler::Ticks();
		auto* const   tes = globals::TES();
		float         landZ = origin.z;
		if (tes && tes->GetLandHeight(origin, landZ)) {
			const float lift = inBlanket ? SnowSurface::LiftAt(origin.x, origin.y) : 0.0f;
			const float margin = inField ? Settings::bloodDecalDepthMargin : 0.0f;

			GroundDecalFit::Box box{};
			if (GroundDecalFit::Fit(a_input[GroundDecalFit::kRotation22], origin.z,
					a_input[GroundDecalFit::kDepth], landZ, lift, margin,
					Settings::bloodDecalGroundTolerance, box)) {
				if (Settings::logBloodDecals &&
					(g_logged.load() < kMaxLogged && g_logged.fetch_add(1) < kMaxLogged)) {
					logger::info("Ground decal at ({:.0f}, {:.0f}): centre {:+.1f} above land, snow {:.1f} | "
								 "box depth {:.1f} -> {:.1f}, centre z {:.1f} -> {:.1f}",
						origin.x, origin.y, origin.z - landZ, lift, a_input[GroundDecalFit::kDepth], box.depth,
						origin.z, box.originZ);
				}
				a_input[GroundDecalFit::kOriginZ] = box.originZ;
				a_input[GroundDecalFit::kDepth] = box.depth;
				a_input[GroundDecalFit::kFade] =
					GroundDecalFit::SnowFade(a_input[GroundDecalFit::kFade], lift, Settings::bloodDecalSnowOpacity);
				Profiler::Tally(Profiler::Count::kDecalsFitted);
			}
		}
		Profiler::AddCpuTicks(Profiler::CpuScope::kGroundDecals, Profiler::Ticks() - start);
	}

	void Reset()
	{
		g_logged.store(0);
	}
}
