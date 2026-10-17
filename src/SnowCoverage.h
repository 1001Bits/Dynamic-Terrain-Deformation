// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace SnowCoverage
{

	inline constexpr uint32_t kTexels = 256;
	inline constexpr float    kWorldSize = 16384.0f;
	inline constexpr float    kTexelSize = kWorldSize / static_cast<float>(kTexels);

	inline constexpr uint32_t kSlot = 1;
	inline constexpr uint32_t kSamplerSlot = 1;

	bool Initialize();
	void Shutdown();
	bool Ready();

	void Update();

	void Reset();

	// Reads every texel again, nearest first, keeping what is known meanwhile (no flicker):
	// for when what the ground is has changed under it.
	void Reread();

	float At(float a_worldX, float a_worldY);

	// The share of snow ground around a point before any roof is taken out: what lies under
	// a house is still snow ground, so the house's roof can carry snow (ObjectSnow).
	float GroundAt(float a_worldX, float a_worldY);

	// True when any texel of the current window is snow ground. The tessellation only
	// extends past the field's window (for the blanket raise) while this holds.
	bool AnySnow();

	void BindDomain(ID3D11DeviceContext* a_context);
	void UnbindDomain(ID3D11DeviceContext* a_context);

	ID3D11ShaderResourceView* View();
}
