// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// Blood, scorch marks and bullet holes on the deformed ground (the Fallout 4 counterpart of
// the Skyrim plugin's EnableBloodDecals). Fallout 4 draws them as deferred decals; each
// frame, as DrawWorld::RenderDeferredDecals copies a decal into its shader input, a static
// decal lying on the landscape has its box stretched over the deformed surface there (see
// GroundDecalFit.h). Nothing in the game's own decal objects changes, so saves are untouched.
namespace GroundDecals
{
	// A new frame: the per-frame budget starts again. Called from the frame update.
	void NewFrame();

	// One decal's shader input, just filled by DrawWorld::ComputeShaderDecalsInput::Set.
	void Fit(float* a_input);

	// Settings were reloaded: the log lines start again.
	void Reset();
}
