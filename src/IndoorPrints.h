// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include "SurfaceTypes.h"

struct ID3D11DeviceContext;

// Shoe prints on floors (IndoorPrints). Boots pick up mud, snow (wet) or dust walking on the
// landscape (Clipmap reports each actor's ground); on a floor that is not landscape - every
// interior, a porch or a road outside that the snow does not bury - each planted foot leaves
// a print coloured onto the floor that fades, the boots a little cleaner with every step
// (IndoorPrintRule). Drawn after the game's G-buffer pass, before its decals and lighting.
// Flat screens only: VR queues its G-buffer pass for a submission thread.
namespace IndoorPrints
{
	// Setting on, a flat screen.
	bool Wanted();

	// Once per world frame (interiors too): the print clock, the space the player is in,
	// and the player's own hidden first-person prints that have left the view.
	void Update(float a_deltaSeconds);

	// An actor walked a_travel over landscape of this kind this frame.
	void NoteGroundWalk(RE::TESFormID a_actor, Surfaces::Type a_surface, float a_travel);

	struct FloorStep
	{
		float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };  // the middle of the sole, on the floor
		float forwardX{ 0.0f }, forwardY{ 1.0f };
		float mirror{ 1.0f };    // 1 right foot, -1 left
		float sizeScale{ 1.0f }; // the actor's size against a person's
		// The player's own synthetic step in first person: not seen forming, it waits until
		// its spot has left the view (as the terrain's hidden marks do).
		bool hidden{ false };
	};

	// A foot came down on a floor: a print if the boots carry anything.
	void NoteFloorStep(RE::TESFormID a_actor, const FloorStep& a_step);

	// From indexed draws in the main G-buffer pass, using the context issuing the draw:
	// retain the first valid albedo/depth/viewport set each frame; skip depth-only draws.
	void NoteGBuffer(ID3D11DeviceContext* a_context);

	// After DrawWorld::DeferredPrePass: the pass itself.
	void Render();

	// A settings reload: the log lines may print again.
	void ResetDiagnostics();

	// A save load or new game: every print and every boot's load are forgotten.
	void ResetWorld();

	void Shutdown();
}
