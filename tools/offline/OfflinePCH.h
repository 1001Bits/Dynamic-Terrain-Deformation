// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// Stand-in for src/PCH.h when the plugin's shader generator is compiled into the offline
// checks (NMN_OFFLINE): the Windows and D3D headers, a stderr logger, and just enough of
// the engine's names for the plugin headers to parse.

#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace RE
{
	class Actor;
	class BGSMaterialType;
	class BSShaderMaterial;
	class NiAVObject;
	class NiPoint3;
	class NiTexture;
	class PlayerCharacter;
	class TESLandTexture;
	class TESObjectCELL;
	class TESObjectREFR;
	class UI;

	namespace BSGraphics
	{
		struct CameraStateData;
	}
}

namespace logger
{
	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		std::fputs(("[info] " + std::format(a_fmt, std::forward<Args>(a_args)...) + "\n").c_str(),
			stderr);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		std::fputs(("[warn] " + std::format(a_fmt, std::forward<Args>(a_args)...) + "\n").c_str(),
			stderr);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		std::fputs(("[error] " + std::format(a_fmt, std::forward<Args>(a_args)...) + "\n").c_str(),
			stderr);
	}
}
