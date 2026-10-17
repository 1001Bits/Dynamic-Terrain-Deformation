// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "DdsLoader.h"

#include <cstring>
#include <fstream>
#include <array>
#include <new>
#include <stdexcept>
#include <vector>

namespace Dds
{
	namespace
	{
		constexpr uint32_t FourCC(char a, char b, char c, char d)
		{
			return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
		}

		struct PixelFormat
		{
			uint32_t size;
			uint32_t flags;
			uint32_t fourCC;
			uint32_t rgbBitCount;
			uint32_t rMask, gMask, bMask, aMask;
		};

		struct Header
		{
			uint32_t    size;
			uint32_t    flags;
			uint32_t    height;
			uint32_t    width;
			uint32_t    pitchOrLinearSize;
			uint32_t    depth;
			uint32_t    mipMapCount;
			uint32_t    reserved1[11];
			PixelFormat pixelFormat;
			uint32_t    caps, caps2, caps3, caps4;
			uint32_t    reserved2;
		};
		static_assert(sizeof(Header) == 124);

		struct HeaderDX10
		{
			uint32_t dxgiFormat;
			uint32_t resourceDimension;
			uint32_t miscFlag;
			uint32_t arraySize;
			uint32_t miscFlags2;
		};
		static_assert(sizeof(HeaderDX10) == 20);

		constexpr uint32_t kFourCCFlag = 0x4;
		constexpr uint32_t kRGBFlag = 0x40;
		constexpr uint32_t kCubemapFlag = 0x200;       // caps2
		constexpr uint32_t kVolumeFlag = 0x200000;     // caps2
		constexpr uint32_t kTexture2D = 3;             // D3D10_RESOURCE_DIMENSION_TEXTURE2D
		constexpr uint32_t kTextureCubeMisc = 0x4;

		DXGI_FORMAT LegacyFormat(const PixelFormat& a_pf)
		{
			if (a_pf.flags & kFourCCFlag) {
				switch (a_pf.fourCC) {
				case FourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
				case FourCC('D', 'X', 'T', '2'):
				case FourCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
				case FourCC('D', 'X', 'T', '4'):
				case FourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
				case FourCC('A', 'T', 'I', '1'):
				case FourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
				case FourCC('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
				case FourCC('A', 'T', 'I', '2'):
				case FourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
				case FourCC('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
				default:                         return DXGI_FORMAT_UNKNOWN;
				}
			}

			if ((a_pf.flags & kRGBFlag) && a_pf.rgbBitCount == 32) {
				if (a_pf.rMask == 0x000000FF && a_pf.gMask == 0x0000FF00 && a_pf.bMask == 0x00FF0000) {
					return DXGI_FORMAT_R8G8B8A8_UNORM;
				}
				if (a_pf.rMask == 0x00FF0000 && a_pf.gMask == 0x0000FF00 && a_pf.bMask == 0x000000FF) {
					return a_pf.aMask ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
				}
			}
			return DXGI_FORMAT_UNKNOWN;
		}

		// Bytes per 4x4 block for block-compressed formats, or 0 for the rest.
		uint32_t BlockBytes(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_TYPELESS:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				return 8;
			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_TYPELESS:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return 16;
			default:
				return 0;
			}
		}

		// Bytes per pixel for the uncompressed formats this reader accepts, or 0.
		uint32_t PixelBytes(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R16G16_FLOAT:
				return 4;
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R8G8_UNORM:
				return 2;
			case DXGI_FORMAT_R8_UNORM:
				return 1;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
				return 8;
			default:
				return 0;
			}
		}

		void SurfaceSize(DXGI_FORMAT a_format, uint32_t a_width, uint32_t a_height,
			uint32_t& a_rowPitch, uint32_t& a_rows)
		{
			if (const uint32_t block = BlockBytes(a_format)) {
				a_rowPitch = std::max(1u, (a_width + 3) / 4) * block;
				a_rows = std::max(1u, (a_height + 3) / 4);
			} else {
				a_rowPitch = a_width * PixelBytes(a_format);
				a_rows = a_height;
			}
		}
	}

	HRESULT LoadTexture(ID3D11Device* a_device, const wchar_t* a_path,
		ID3D11ShaderResourceView** a_view) try
	{
		if (!a_device || !a_path || !a_view) {
			return E_INVALIDARG;
		}
		*a_view = nullptr;

		std::ifstream file(a_path, std::ios::binary | std::ios::ate);
		if (!file) {
			return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		}
		const auto end = file.tellg();
		if (end < std::streampos(0)) {
			return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
		}
		const auto size = static_cast<std::uintmax_t>(end);
		file.seekg(0);

		constexpr size_t kMagic = sizeof(uint32_t);
		if (size < kMagic + sizeof(Header)) {
			return E_FAIL;
		}
		uint32_t magic = 0;
		if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic))) {
			return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
		}
		if (magic != FourCC('D', 'D', 'S', ' ')) {
			return E_FAIL;
		}

		Header header{};
		if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) {
			return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
		}
		if (header.size != sizeof(Header) || header.pixelFormat.size != sizeof(PixelFormat)) {
			return E_FAIL;
		}
		if (header.caps2 & (kCubemapFlag | kVolumeFlag)) {
			return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
		}

		size_t      offset = kMagic + sizeof(Header);
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		if ((header.pixelFormat.flags & kFourCCFlag) &&
			header.pixelFormat.fourCC == FourCC('D', 'X', '1', '0')) {
			if (size < offset + sizeof(HeaderDX10)) {
				return E_FAIL;
			}
			HeaderDX10 dx10{};
			if (!file.read(reinterpret_cast<char*>(&dx10), sizeof(dx10))) {
				return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
			}
			offset += sizeof(HeaderDX10);

			if (dx10.resourceDimension != kTexture2D || dx10.arraySize != 1 ||
				(dx10.miscFlag & kTextureCubeMisc)) {
				return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
			}
			format = static_cast<DXGI_FORMAT>(dx10.dxgiFormat);
		} else {
			format = LegacyFormat(header.pixelFormat);
		}

		if (format == DXGI_FORMAT_UNKNOWN || (BlockBytes(format) == 0 && PixelBytes(format) == 0)) {
			return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
		}
		if (header.width == 0 || header.height == 0 ||
			header.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			header.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			return E_FAIL;
		}

		const uint32_t mips = std::max(header.mipMapCount, 1u);
		uint32_t maxMips = 1;
		for (auto dimension = std::max(header.width, header.height); dimension > 1; dimension /= 2) {
			++maxMips;
		}
		if (mips > maxMips) {
			return E_FAIL;
		}

		// Validate the complete payload before allocating. Trailing data (even a huge
		// corrupt file) is never copied into memory, and valid dimensions/formats bound
		// the payload independently of the file's reported size.
		std::array<D3D11_SUBRESOURCE_DATA, D3D11_REQ_MIP_LEVELS> initial{};
		std::array<size_t, D3D11_REQ_MIP_LEVELS> mipOffsets{};
		size_t payload = 0;
		uint32_t                            width = header.width;
		uint32_t                            height = header.height;
		for (uint32_t mip = 0; mip < mips; ++mip) {
			uint32_t rowPitch = 0;
			uint32_t rows = 0;
			SurfaceSize(format, width, height, rowPitch, rows);

			const size_t bytes = static_cast<size_t>(rowPitch) * rows;
			if (offset + payload + bytes > size) {
				return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
			}
			mipOffsets[mip] = payload;
			initial[mip].SysMemPitch = rowPitch;
			initial[mip].SysMemSlicePitch = static_cast<UINT>(bytes);
			payload += bytes;

			width = std::max(1u, width / 2);
			height = std::max(1u, height / 2);
		}
		std::vector<uint8_t> data(payload);
		if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(payload))) {
			return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
		}
		for (uint32_t mip = 0; mip < mips; ++mip) {
			initial[mip].pSysMem = data.data() + mipOffsets[mip];
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = header.width;
		desc.Height = header.height;
		desc.MipLevels = mips;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		ID3D11Texture2D* texture = nullptr;
		HRESULT          hr = a_device->CreateTexture2D(&desc, initial.data(), &texture);
		if (FAILED(hr)) {
			return hr;
		}

		hr = a_device->CreateShaderResourceView(texture, nullptr, a_view);
		texture->Release();
		return hr;
	} catch (const std::bad_alloc&) {
		return E_OUTOFMEMORY;
	} catch (const std::length_error&) {
		return E_OUTOFMEMORY;
	}
}
