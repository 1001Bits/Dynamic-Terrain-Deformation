// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace ShaderRegistry
{
	// Patches Fallout4.exe's import of D3D11CreateDeviceAndSwapChain so the device can be
	// instrumented before the game creates a single shader. Must run from plugin load.
	bool InstallEarly();

	// Hooks CreateVertexShader and deferred-context creation on the device, and indexed
	// draws on its immediate context and every subsequently created deferred context.
	// Safe to call more than once.
	bool Install(ID3D11Device* a_device);

	bool DrawHookInstalled();

	// The output signature chunk recorded for a vertex shader the device created (see
	// Reflection::ParseSignatureChunk), or empty. Only that chunk is kept, not the shader.
	std::vector<uint8_t> OutputSignature(ID3D11VertexShader* a_shader);

	size_t Count();

	// Bytes of signature data held.
	size_t Bytes();
}
