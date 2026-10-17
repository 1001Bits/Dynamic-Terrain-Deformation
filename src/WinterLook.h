// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <cstdint>
#include <string_view>

struct ID3D11DeviceContext;

// Winter mods paint the ground textures white and keep their names and ground materials, so
// neither says the ground is snow. Under WinterMode -1 (the default) every landscape layer
// texture is measured once on the GPU as a draw first shows it, and one that looks like snow
// counts as snow, whatever its name. The verdicts and their rules are apart from the GPU so
// the offline tests can check them.
namespace WinterLook
{
	// What a texture looks like: its mean colour (gamma space, 0-1) and the share of it that
	// is bright and nearly colourless.
	struct Sample
	{
		float r{ 0.0f };
		float g{ 0.0f };
		float b{ 0.0f };
		float white{ 0.0f };
	};

	// A texel counts as white at this brightness or more and this saturation or less (also
	// the measuring shader's rule); a texture is snow when this share of it is white.
	inline constexpr float kWhiteLuminance = 0.6f;
	inline constexpr float kWhiteSaturation = 0.22f;
	inline constexpr float kSnowShare = 0.55f;

	[[nodiscard]] bool IsSnow(const Sample& a_sample);

	// Whether the textures are measured at all (WinterMode -1).
	[[nodiscard]] bool Enabled();

	// The key a texture is known by: a hash of Surfaces::TextureKey.
	[[nodiscard]] uint64_t KeyOf(std::string_view a_texturePath);

	// A landscape layer texture: measured the next time a draw shows it. Thread-safe; returns
	// its key.
	uint64_t Register(std::string_view a_texturePath);

	// True once the texture was measured and looks like snow.
	[[nodiscard]] bool LooksSnowy(std::string_view a_texturePath);

	// Most of the ground textures drawn lately look like snow (a winter mod is on): the
	// worldspace's default ground counts as snow too, and rain falls as snow.
	[[nodiscard]] bool MostlySnowy();

	// For the GPU side and the offline tests. Seen() notes the layers a draw shows on a_frame
	// and returns a bit for each one still waiting to be measured, now claimed by the caller;
	// Report() records a measurement (Abandon() one that could not be taken) and returns true
	// when it turned some ground into snow. Reevaluate() updates MostlySnowy from the textures
	// drawn in the last few seconds and returns true when it changed.
	uint32_t Seen(const uint64_t (&a_keys)[4], uint64_t a_frame);
	bool     Report(uint64_t a_key, const Sample& a_sample);
	void     Abandon(uint64_t a_key);
	bool     Reevaluate(uint64_t a_frame);
	[[nodiscard]] bool Waiting();

	// Forgets every texture and verdict.
	void Forget();

	// GPU side (WinterLookGpu.cpp). Capture() runs in the landscape draw bracket, while the
	// draw's layer textures are bound to the pixel stage (t0-t3, layer by layer); Update()
	// runs once a frame on the render thread before the world is drawn, measures what was
	// captured and returns true when the ground's surfaces changed (classification caches
	// and the snow coverage then need reading again).
	void Capture(ID3D11DeviceContext* a_context, const uint64_t (&a_keys)[4]);
	bool Update();
	void Release();
}
