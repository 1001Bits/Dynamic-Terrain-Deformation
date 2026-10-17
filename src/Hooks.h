// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

struct ID3D11DeviceContext;

namespace Hooks
{
	void Install();

	// Capture G-buffer resources from the context issuing the draw, including
	// deferred contexts; never query the immediate context from a rendering worker.
	void NoteDrawContext(ID3D11DeviceContext* a_context);

	// VR only: records each landscape layer's texture and texture-set path as the game
	// builds land geometry (VR's landscape textures carry no names). Install before any
	// cell loads.
	void InstallLandscapeTextureRecorder();

	// Asks the next world frame to drop every piece of per-world state (the field, the
	// snow and shelter maps, the weather clock, tracked actors and objects). Safe from any
	// thread; the reset itself runs on the thread that advances the field.
	void RequestWorldReset();
}
