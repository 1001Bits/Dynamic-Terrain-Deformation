// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Fallout 4 draws blood, scorch marks and bullet holes on the ground as deferred decals:
// each is a box, and every pixel whose depth-buffer position lies inside it takes the decal.
// The box is sized for the undeformed ground, so a decal on a dent lies partly below the
// surface the plugin draws there, and one on raised snow lies wholly under it. Here a box on
// the landscape is stretched to reach from the deepest mark below the ground to the top of
// the snow above it; the visible surface - snow, trench, crater - then takes the decal.
namespace GroundDecalFit
{
	// DrawWorld::ComputeShaderDecalsInput as DrawWorld::ComputeShaderDecalsInput::Set fills it
	// from a BSDeferredDecal::BSDFDecal (0xA0 bytes a decal; the same on 1.10.163, 1.10.984,
	// 1.11.240 and VR 1.2.72), in floats: the parent transform (0x00-0x3C), the decal's
	// rotation rows (0x40-0x6C), its origin (0x70), width / height / depth (0x7C-0x84), and
	// at 0x8C the dynamic flag (non-zero for a decal riding on a moving object).
	inline constexpr std::size_t kInputFloats = 0xA0 / 4;
	inline constexpr std::size_t kRotation22 = 0x68 / 4;
	inline constexpr std::size_t kOriginX = 0x70 / 4;
	inline constexpr std::size_t kOriginY = 0x74 / 4;
	inline constexpr std::size_t kOriginZ = 0x78 / 4;
	inline constexpr std::size_t kDepth = 0x84 / 4;
	// The decal's fade (BSDFDecal +0xC0: 1 when the decal is made). The decal pixel shader
	// multiplies the decal's opacity by it.
	inline constexpr std::size_t kFade = 0x88 / 4;
	inline constexpr std::size_t kDynamic = 0x8C / 4;

	// The box's depth axis must be this close to vertical (the cosine of its tilt) for the
	// decal to count as lying on the ground. A rotation's corner element is the vertical
	// part of that axis whichever way round the matrix is stored.
	inline constexpr float kMinVertical = 0.7f;

	// The fade a decal lying on snow is drawn with: blood soaks into snow, so the snow shows
	// through it. Only on snow (a blanket over the land); the game's own fade is kept.
	inline float SnowFade(float a_fade, float a_lift, float a_snowOpacity)
	{
		if (!(a_lift > 0.5f) || !std::isfinite(a_fade)) {
			return a_fade;
		}
		const float opacity = std::isfinite(a_snowOpacity) ? std::clamp(a_snowOpacity, 0.0f, 1.0f) : 1.0f;
		return a_fade * opacity;
	}

	struct Box
	{
		float originZ{ 0.0f };
		float depth{ 0.0f };
	};

	// Whether a decal with this depth axis (vertical component), centre height and depth lies
	// on the landscape at a_landZ, and if so the box that covers everything from a_margin
	// below the ground to a_margin above the snow's top (a_lift above the ground). The box
	// only ever grows: what the engine sized it to cover stays covered.
	inline bool Fit(float a_axisZ, float a_originZ, float a_depth, float a_landZ, float a_lift,
		float a_margin, float a_tolerance, Box& a_out)
	{
		const float vertical = std::fabs(a_axisZ);
		if (!(vertical >= kMinVertical && vertical <= 1.001f) || !std::isfinite(a_originZ) || !std::isfinite(a_depth) ||
			!std::isfinite(a_landZ) || !(a_depth >= 0.0f)) {
			return false;
		}
		if (!(std::fabs(a_originZ - a_landZ) <= std::max(a_tolerance, 0.0f))) {
			return false;
		}

		const float lift = std::isfinite(a_lift) ? std::max(a_lift, 0.0f) : 0.0f;
		const float margin = std::isfinite(a_margin) ? std::max(a_margin, 0.0f) : 0.0f;
		if (lift <= 0.0f && margin <= 0.0f) {
			return false;
		}

		// Keep the centre fixed. Moving only its world Z also moves a tilted box along
		// its width/height axes, clipping parts of the original decal. Growing the depth
		// symmetrically preserves the entire original oriented volume.
		const float halfSpan = std::max(std::fabs(a_originZ - (a_landZ - margin)),
			std::fabs(a_landZ + lift + margin - a_originZ));
		a_out.originZ = a_originZ;
		a_out.depth = std::max(a_depth, 2.0f * halfSpan / vertical);
		return std::isfinite(a_out.depth);
	}
}
