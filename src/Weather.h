// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace RE
{
	class Sky;
}

namespace Weather
{
	// Fallout 4's Sky singleton through a verified address, or nullptr.
	RE::Sky* Sky();


	void Update();

	void Reset();

	float Level();

	float Precipitation();

	float DepthScale();
	float DecayScale();

	float RaiseScale();
	float SnowDepth();

	float FillPerSecond();

	const char* StateLine();

	// How hard snow is falling outside now, 0 to 1 (blended across a weather change): a snow
	// weather, or under WinterMode a rain one. 0 indoors. Read whether or not EnableWeather is on.
	float Snowfall();
}
