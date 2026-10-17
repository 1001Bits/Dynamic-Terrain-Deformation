// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace Profiler
{

	enum class Scope
	{

		kClipmap,

		kLandscape,

		kLandscapeDepth,

		kOtherRouted,

		// The snow-on-objects G-buffer pass (its own timer, like the clipmap's).
		kObjectSnow,

		// The floor-print G-buffer pass (its own timer too).
		kIndoorPrints,

		kCount
	};

	enum class Count
	{
		kLandscapeSeen,
		kLandscapeCulled,
		kLandscapeRouted,

		kDepthSeen,
		kDepthCulled,
		kDepthShadowSkipped,
		kDepthRouted,

		kActorRouted,

		kStaticSeen,
		kStaticRouted,
		kShaderBuilds,

		kDecalsSeen,
		kDecalsFitted,

		kObjectSnowRays,
		kObjectSnowPasses,

		kIndoorPrintsDrawn,
		kIndoorPrintsRecorded,

		// Corpses whose marks were stamped this frame (each is stamped again every few frames).
		kCorpsesMarked,
		kCount
	};

	bool Enabled();

	void Frame(float a_rawDeltaSeconds);

	// Immediate render context only. End only a scope whose begin returned true.
	bool GpuBegin(Scope a_scope);
	void GpuEnd();

	void Tally(Count a_what, uint32_t a_howMany = 1);

	enum class CpuScope
	{

		kDrawBracket,

		kGatherStamps,

		kSnowCoverage,

		kShelter,
		kObjectScan,
		kShaderPrepare,
		kImpacts,
		kGroundDecals,
		kObjectSnow,

		kCount
	};

	int64_t Ticks();
	void    AddCpuTicks(CpuScope a_scope, int64_t a_ticks);

	void Reset();

	void Release();
}
