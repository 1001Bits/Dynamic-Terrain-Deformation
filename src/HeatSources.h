// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include "Clipmap.h"

#include <span>

namespace RE
{
	class TESObjectREFR;
}

// Fires, burning barrels and candles melt the snow around them and keep the ground
// under them from being pressed flat.
namespace HeatSources
{
	// True for a loaded reference whose model is a fire, candle or lit flare (cached per
	// base form; cheap after the first call).
	bool IsHeatSource(RE::TESObjectREFR* a_ref);

	bool StampFor(RE::TESObjectREFR* a_ref, Clipmap::Stamp& a_out);

	// Drops the per-base "is this hot" cache; call when the heat keyword lists change.
	void Reset();

	// A fire's solid parts (barrel, logs, candle) where it was last stamped.
	struct Hot
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float radius{ 0.0f };
	};

	// The fires stamped lately within a_reach of a_from (across), nearest first, at most
	// a_out.size(). Returns how many.
	size_t Nearby(float a_fromX, float a_fromY, float a_reach, std::span<Hot> a_out);
}
