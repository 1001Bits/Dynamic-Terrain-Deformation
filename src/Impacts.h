// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include "Clipmap.h"

#include <vector>

// Fallout 4's stand-in for the Skyrim build's magic, shout and explosion impacts:
// explosions blast craters, and bullets and lasers that strike the ground leave small
// dents.
namespace Impacts
{
	void Install();
	void Reset();
	void Append(const RE::NiPoint3& a_anchor, std::vector<Clipmap::Stamp>& a_out);
}
