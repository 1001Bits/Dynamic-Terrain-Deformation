// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <algorithm>

namespace SnowSurface
{
	// Mesh caps store fixed geometry heights normalized to the configured blanket,
	// independent of the weather-scaled lift. This is the domain shader's cap rule.
	inline float ApplyMeshCap(float a_lift, float a_baseHeight, float a_cap)
	{
		return std::min(a_lift, a_baseHeight * a_cap);
	}

	float BlanketFor(float a_snowy, float a_worldX, float a_worldY);

	float LiftAt(float a_worldX, float a_worldY);
}
