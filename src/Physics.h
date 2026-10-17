// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace Physics
{
	// Collision layers (bits of bhkPickData::customCollideLayers).
	inline constexpr std::uint64_t kLayerStatic = 1ull << 1;
	inline constexpr std::uint64_t kLayerAnimStatic = 1ull << 2;
	inline constexpr std::uint64_t kLayerGround = 1ull << 17;

	// What the game itself uses to find the ground material under a point: world
	// geometry and terrain, never actors or clutter.
	inline constexpr std::uint64_t kWorldGeometry = kLayerStatic | kLayerAnimStatic | kLayerGround;

	bool Available();

	// Casts a ray between two world positions (game units) in the cell's physics world.
	// Returns the fraction along the ray of the first hit in [0, 1], or a negative value
	// when nothing on the given layers is in the way.
	float CastRay(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		std::uint64_t a_layers = kWorldGeometry);
}
