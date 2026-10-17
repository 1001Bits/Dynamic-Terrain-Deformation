// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Where snow lies on a static object standing above the blanket: on a face that points up,
// at the height of the topmost static surface over that spot (a roof, a rock top, a car
// roof), where the ground around is snow. The top heights come from rays cast straight down
// on the static collision layers, one per kTexelSize square around the player; a pixel is on
// a top when its height matches one of the four nearest. Only tops that stand above the snow
// count (a road or a slab the blanket buries keeps the blanket's own look), and nothing at or
// under the snow surface - the land, the blanket, a trench - is touched: the landscape shows
// its own texture (white with a winter texture mod). Anything under a roof sits at another
// height. An actor (living, dead, the player) standing, lying or draped over a top is within
// reach of its height, so inside an actor's body (capsules along its skeleton) only the top's
// own surface counts (kActorTopAbove): a body on a wall keeps its own colour. ObjectSnow's
// pixel shader repeats this; the offline checks run it on the CPU.
namespace ObjectSnowRule
{
	// The probe grid: wrapped storage around the player, a top height (world z) per texel.
	inline constexpr uint32_t kTexels = 256;
	inline constexpr uint32_t kMask = kTexels - 1;
	inline constexpr float    kTexelSize = 16.0f;
	inline constexpr float    kWindowSize = kTexelSize * static_cast<float>(kTexels);

	static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the wrapped grid");

	// A texel with no snowy top (no static under the ray, not snow ground, not probed yet), or
	// no land height.
	inline constexpr float kNoTop = -1.0e30f;

	// Collision is not the drawn surface (a roof's collision can sit some units over or under
	// its shingles): a top within this of the drawn height still counts.
	inline constexpr float kTopTolerance = 12.0f;

	// A top counts only this far above the snow surface (land plus the blanket depth): a
	// mesh the blanket buries, or one flush with it, keeps the blanket's own look.
	inline constexpr float kAboveSnow = 6.0f;

	// A pixel this close over the snow surface, or under it, is the ground or the blanket
	// itself and is never whitened.
	inline constexpr float kSurfaceBand = 3.0f;

	// Inside an actor's body a pixel is the top only within this of its height; a body, a
	// boot or a limb resting on or hanging over the top lies off it. Up to kMaxActorCapsules
	// capsules of the nearest actors, each widened by kActorMargin for clothes and armour.
	inline constexpr float    kActorTopAbove = 2.0f;
	inline constexpr uint32_t kMaxActorCapsules = 64;
	inline constexpr float    kActorMargin = 4.0f;
	// A fire (burning barrel, campfire, brazier) takes no snow at all within its solid
	// bound widened by this; the snow around it melts (HeatSources). Up to kMaxFires.
	inline constexpr float    kFireMargin = 8.0f;
	inline constexpr uint32_t kMaxFires = 8;

	// From MinUp to MinUp + kFadeUp the snow fades in.
	inline constexpr float kFadeUp = 0.3f;

	// The furthest probed square must lie inside the wrapped window with room to spare.
	inline constexpr float kMaxDistance = kWindowSize * 0.5f - 2.0f * kTexelSize;

	inline float Smoothstep(float a_lo, float a_hi, float a_x)
	{
		if (!(a_hi > a_lo)) {
			return a_x >= a_hi ? 1.0f : 0.0f;
		}
		const float t = std::clamp((a_x - a_lo) / (a_hi - a_lo), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// Whether a probed top stands above the snow on its square (and so may carry snow).
	inline bool AboveSnow(float a_top, float a_land, float a_snowDepth)
	{
		return a_top > kNoTop * 0.5f && a_land > kNoTop * 0.5f &&
		       a_top - a_land >= std::max(a_snowDepth, 0.0f) + kAboveSnow;
	}

	// How much a face whose normal points up by a_up (its z component) takes.
	inline float UpShare(float a_up, float a_minUp)
	{
		const float lo = std::clamp(a_minUp, 0.0f, 0.99f);
		return Smoothstep(lo, std::min(lo + kFadeUp, 1.0f), a_up);
	}

	// A sloped top spans heights across one texel: the tolerance grows with the slope.
	inline float Tolerance(float a_up)
	{
		const float up = std::clamp(a_up, 0.2f, 1.0f);
		const float slope = std::sqrt(std::max(1.0f - up * up, 0.0f)) / up;
		return kTopTolerance + kTexelSize * slope;
	}

	// 1 when a point at a_z lies on one of the given tops, fading to 0 just past the tolerance.
	inline float OnTop(float a_z, const float (&a_tops)[4], float a_tolerance)
	{
		float nearest = 1.0e30f;
		for (const float top : a_tops) {
			if (top > kNoTop * 0.5f) {
				nearest = std::min(nearest, std::fabs(a_z - top));
			}
		}
		return 1.0f - Smoothstep(a_tolerance * 0.6f, a_tolerance, nearest);
	}

	// Whether a pixel a_overTop over (or under) its nearest top may take snow: anywhere outside
	// an actor's body, inside one only on the top's own surface.
	inline bool ActorAllows(float a_overTop, bool a_insideActor)
	{
		return !a_insideActor || std::fabs(a_overTop) <= kActorTopAbove;
	}

	// 1 when a point lies clear of the snow surface (a_land is the land height under it, from
	// the four nearest squares), 0 at or under it.
	inline float AboveSurface(float a_z, float a_land, float a_snowDepth)
	{
		if (!(a_land > kNoTop * 0.5f)) {
			return 0.0f;
		}
		const float over = a_z - (a_land + std::max(a_snowDepth, 0.0f));
		return Smoothstep(kSurfaceBand, kSurfaceBand * 2.0f, over);
	}

	// The snow cover on a pixel, 0 to a_strength.
	inline float Amount(float a_up, float a_z, const float (&a_tops)[4], float a_land, float a_snowDepth,
		float a_minUp, float a_strength)
	{
		return std::clamp(a_strength, 0.0f, 1.0f) * UpShare(a_up, a_minUp) *
		       OnTop(a_z, a_tops, Tolerance(a_up)) * AboveSurface(a_z, a_land, a_snowDepth);
	}

	// The wrapped storage index of the texel that holds a world position.
	inline uint32_t Index(int32_t a_cellX, int32_t a_cellY)
	{
		return (static_cast<uint32_t>(a_cellY) & kMask) * kTexels + (static_cast<uint32_t>(a_cellX) & kMask);
	}

	inline int32_t CellOf(float a_world)
	{
		return static_cast<int32_t>(std::floor(a_world / kTexelSize));
	}
}
