// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "SurfaceTypes.h"

namespace SnowSparkle
{

	// True while SnowSparkle is on in the INI and the forward-pass hook is in place; the
	// INI switch is read live, so the spray turns on and off on a reload.
	bool Enabled();
	void NoteHookInstalled(bool a_installed);

	// Compiles the flake shaders and makes the buffers and states, once (flat screen
	// only). Call when the forward-pass hook is installed so the compile happens at load,
	// not at the first kick; Render makes them itself if this was never called.
	bool Initialize();

	// a_amount: how much snow the contact throws against a person's foot (a heavy
	// creature's more, higher and further).
	void NoteContact(Surfaces::Type a_surface, const RE::NiPoint3& a_at, float a_forwardX,
		float a_forwardY, float a_velX, float a_velY, float a_radius, float a_amount = 1.0f);

	void Update(float a_deltaSeconds);

	void Render();

	// Clears the flakes (a settings reload); the D3D objects stay.
	void Reset();
	// Clears the flakes and releases every D3D object.
	void Release();
}
