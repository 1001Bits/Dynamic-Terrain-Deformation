// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// How the snow blanket meets a static mesh lying on the ground, from the height of the
// mesh's top above the land (a shelter probe straight down). A low one - a road, a path,
// a slab, low rubble - is buried: the blanket rises over it as over the land, so it is no
// longer a bare strip under the snow, and feet, steps and shots on it press the snow. A
// higher one - a big rock, a porch, a foundation, a bridge - keeps the blanket a little
// under its top, as it always did.
namespace SnowMeshCover
{
	// Collision is not the drawn surface: a blanket kept under a mesh stays this far below
	// the mesh's collision top, so it does not rise through the mesh it stops under.
	inline constexpr float kBelowMesh = 3.0f;

	// A buried mesh's top lies at least this far under the blanket's top.
	inline constexpr float kMinCover = 2.0f;

	struct Result
	{
		// The blanket's height limit over the raise height (255 = no limit).
		uint8_t cap{ 255 };
		// A buried mesh's top over the raise height (0 = nothing buried here).
		uint8_t buried{ 0 };
	};

	// The highest a mesh's top may lie and still be buried.
	inline float CoverLimit(float a_raise, float a_coverHeight)
	{
		return std::clamp(a_coverHeight, 0.0f, std::max(a_raise - kMinCover, 0.0f));
	}

	// a_hit is the height above the land of the first static surface under the blanket's
	// top, negative where the column is clear.
	inline Result Classify(float a_hit, float a_raise, float a_coverHeight, bool a_coverLow)
	{
		Result      out{};
		const float raise = std::max(a_raise, 1.0f);
		if (!(a_hit >= 0.0f)) {
			return out;
		}

		if (a_coverLow && a_hit <= CoverLimit(raise, a_coverHeight)) {
			out.buried = static_cast<uint8_t>(
				std::clamp(a_hit / raise, 1.0f / 255.0f, 1.0f) * 255.0f + 0.5f);
			return out;
		}

		out.cap = static_cast<uint8_t>(
			std::clamp((a_hit - kBelowMesh) / raise, 0.0f, 1.0f) * 255.0f + 0.5f);
		return out;
	}

	// A stored buried value back in world units above the land.
	inline float BuriedHeight(uint8_t a_buried, float a_raise)
	{
		return static_cast<float>(a_buried) / 255.0f * std::max(a_raise, 1.0f);
	}

	// A hit on a buried static is measured from its top, which can be more than the
	// ordinary landscape impact tolerance (16 units) above the land.
	inline bool HitsBuriedTop(float a_aboveLand, float a_buried)
	{
		return a_buried > 0.0f && std::isfinite(a_aboveLand) &&
		       std::abs(a_aboveLand - a_buried) <= 8.0f;
	}
}
