// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

struct ID3D11DeviceContext;

// Snow on the tops of static objects that stand above the blanket - rock tops, roofs, car
// roofs, porches - where the ground around them is snow (SnowOnObjects). Rays cast straight
// down on the static collision layers find the topmost surface over each 16-unit square around
// the player; after the game's G-buffer pass a full-screen pass whitens the albedo of every
// upward face at that height (ObjectSnowRule, ObjectSnowShader.h). Flat screens only: VR
// queues its G-buffer pass for a submission thread.
namespace ObjectSnow
{
	// Once per exterior frame, before the G-buffer pass: queues the next batch of top probes
	// (an F4SE game-thread task, like the shelter's) and refreshes the grid the pass reads.
	void Update();

	// From a routed landscape draw in the G-buffer pass: remembers the albedo target, the depth
	// and the viewport bound for it, once a frame.
	void NoteGBuffer(ID3D11DeviceContext* a_context);

	// After DrawWorld::DeferredPrePass: the pass itself.
	void Render();

	// Forgets every probed top (a new world, a reload of the settings).
	void Reset();

	// Whether a point (a foot on a step) lies on a top the pass whitens: a static's top above
	// the snow on its square, with snow ground around it. Floor prints count it as snow.
	bool SnowyTopAt(float a_x, float a_y, float a_z);

	void Shutdown();
}
