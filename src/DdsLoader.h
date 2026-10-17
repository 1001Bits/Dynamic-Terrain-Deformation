// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// A small DDS reader standing in for DirectXTK's loader, enough for the plugin's own
// textures: 2D, one array slice, any mip count, in the block-compressed formats (BC1 to
// BC7, legacy DXT1/3/5 and ATI1/2 headers included) and plain 8-bit RGBA.
namespace Dds
{
	HRESULT LoadTexture(ID3D11Device* a_device, const wchar_t* a_path,
		ID3D11ShaderResourceView** a_view);
}
