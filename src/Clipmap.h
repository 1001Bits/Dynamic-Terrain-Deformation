// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "HiddenMarks.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Clipmap
{
	inline constexpr uint32_t kTexels = 2048;
	inline constexpr float    kWorldSize = 1536.0f;
	inline constexpr float    kCellSize = kWorldSize / static_cast<float>(kTexels);

	// The coarse level is kLevelScale times wider than the fine one: 7680 units at 3.75 a
	// texel, so marks reach about 53 m around the player (FootprintMetres sets how far).
	inline constexpr uint32_t kMaxLevels = 2;
	inline constexpr float    kLevelScale = 5.0f;

	constexpr float WorldSizeFor(uint32_t a_level)
	{
		float size = kWorldSize;
		for (uint32_t i = 0; i < a_level; ++i) {
			size *= kLevelScale;
		}
		return size;
	}

	constexpr float CellSizeFor(uint32_t a_level)
	{
		return WorldSizeFor(a_level) / static_cast<float>(kTexels);
	}

	inline uint32_t LevelCount()
	{
		return std::clamp(Settings::clipmapLevels, 1u, kMaxLevels);
	}

	inline constexpr uint32_t kActivityTexels = 32;
	inline constexpr uint32_t kActivityRatio = kTexels / kActivityTexels;
	static_assert(kTexels % kActivityTexels == 0);

	// Hull-stage slots of each level's activity (the largest |height| near each cell).
	inline constexpr uint32_t kActivitySlot = 0;
	inline constexpr uint32_t kActivity1Slot = 1;

	inline constexpr uint32_t kLevel1Slot = 2;
	// Level 0's metadata in the domain stage, for the scorch.
	inline constexpr uint32_t kScorchSlot = 4;

	// The rate a mark recovers at, as the fraction of its depth left after one second.
	// StampDecayPerSecond is the base; every scale on it (a surface's Decay column, a
	// profile's Decay, the weather's decay scales) stretches the mark's lifetime: 2 makes a
	// mark last twice as long, 0.5 half as long. Multiplying the rate itself instead would
	// push 0.99 past 1 for any scale above 1.0101 - a mark that never recovers.
	inline float RecoveryRate(float a_perSecond, float a_lifetimeScale)
	{
		const float base = std::clamp(a_perSecond, 0.0f, 0.9999f);
		const float scale = std::max(a_lifetimeScale, 1e-3f);
		return std::clamp(std::pow(base, 1.0f / scale), 0.0f, 0.9999f);
	}

	inline constexpr uint32_t kMaxStamps = 64;

	// Stamp sources may append up to this many per frame. The gatherer keeps one-shot
	// stamps (impacts) first, then the nearest, trims to kMaxStamps and logs what it cut.
	inline constexpr uint32_t kGatherCap = kMaxStamps * 2;

	static_assert((kTexels & (kTexels - 1)) == 0, "kTexels must be a power of two");

	static_assert(kTexels % 8 == 0, "kTexels must divide evenly by the 8x8 thread group");

	struct Stamp
	{

		enum class Kind
		{

			kPress,

			kMelt,

			kPrint,
		};

		float x{ 0.0f };
		float y{ 0.0f };
		float radius{ 40.0f };
		float depth{ 15.0f };

		Kind kind{ Kind::kPress };

		float shoulder{ 0.3f };

		float decay{ 0.92f };

		float rim{ 0.0f };

		float forwardX{ 0.0f };
		float forwardY{ 1.0f };
		float halfWidth{ 0.0f };

		float mirror{ 1.0f };

		// A press under a resting object: its rim rises only this many world units
		// outside the footprint, over a band as wide as the footprint is narrow, so the
		// soil pushed aside never covers the object lying there. Negative (feet,
		// impacts): the rim rises at the edge, its width a share of the radius.
		float rimMargin{ -1.0f };

		float motionX{ 0.0f };
		float motionY{ 0.0f };
		bool snow{ false };

		// How black the stamp burns the ground under it, 0 to 1 (a Mr. Handy's jet). The
		// field keeps the darkest burn per texel and lets it fade; the terrain's vertex
		// colour is darkened by it.
		float scorch{ 0.0f };

		// A melt by heat (a fire, a jet) rather than a body's hollow: where it takes the snow
		// down, the terrain shows the ground's own colour (MeltShowsGround).
		bool heat{ false };
	};

	bool Initialize();

	// The levels in use: level 0 and each coarser one that was created, up to LevelCount()
	// (0 before Initialize). The hull and domain stages are generated for this many; a
	// coarse level that failed to allocate drops the generated stages to one level.
	uint32_t ActiveLevels();

	void ResetDiagnostics();
	void Release();
	bool Ready();

	void Update(float a_deltaSeconds);

	// Inside (no landscape, no field): only what the actors' feet do on the floors, for the
	// floor prints (IndoorPrints). Outside, Update does it with the rest.
	void UpdateFloorFeet(float a_deltaSeconds);

	// The player's camera for hiding first-person marks while they form (flat: the world
	// camera read back from the G-buffer pass, FrameCamera; VR: the engine's camera block;
	// valid only while it is the player's).
	HiddenMarks::View PlayerView(const RE::NiPoint3& a_player);

	// Clears both levels and forgets the window and every tracked actor, for a save load
	// or a change of worldspace.
	void ResetWorld();

	inline constexpr uint32_t kParamsSlot = 13;

	// Binds both levels' fields to the domain stage and their activity to the hull stage,
	// with the field's linear wrap sampler at s0 of both.
	void BindDomain(ID3D11DeviceContext* a_context);

	ID3D11SamplerState* Sampler();

	bool GetWindow(float& a_centreX, float& a_centreY, float& a_halfExtent);
}
