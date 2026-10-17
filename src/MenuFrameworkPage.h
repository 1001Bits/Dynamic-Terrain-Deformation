// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// The main options in F4SE Menu Framework for players without the Mod Configuration Menu: the same
// controls, texts and ranges as MCM's config.json, one page per MCM section. A change is
// written to MCM's settings file (Data/MCM/Settings/NMN_DeformableTerrain.ini), which the
// plugin already watches, so it applies within half a second, as a change in MCM does.
// Only without MCM: with it, the framework shows MCM's own menu through its MCM translation.
// Flat screens only: the framework does not run in VR.
namespace MenuFrameworkPage
{
	// Registers the pages when F4SEMenuFramework.dll is loaded. Call once at kPostPostLoad.
	void Install();
}
