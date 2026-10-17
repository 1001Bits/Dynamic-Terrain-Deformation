// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#ifdef NMN_OFFLINE
// The offline shader checks compile parts of the plugin without the game libraries.
#	include "OfflinePCH.h"
#else

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <Windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The plugin's log calls. CommonLibF4's F4SE::log formats at run time, where a format string
// that does not fit its arguments throws std::format_error - inside an engine hook, a crash.
// These check every format string against its arguments at compile time instead, and write
// through the same log file.
namespace logger
{
	template <class... Args>
	struct Format
	{
		std::format_string<Args...> text;
		std::source_location        location;

		template <class T>
		consteval Format(const T& a_text, std::source_location a_location = std::source_location::current()) :
			text(a_text),
			location(a_location)
		{}
	};

	template <class... Args>
	void trace(Format<std::type_identity_t<Args>...> a_format, Args&&... a_args)
	{
		F4SE::log::write_log(a_format.location, "TRACE", std::format(a_format.text, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void debug(Format<std::type_identity_t<Args>...> a_format, Args&&... a_args)
	{
		F4SE::log::write_log(a_format.location, "DEBUG", std::format(a_format.text, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void info(Format<std::type_identity_t<Args>...> a_format, Args&&... a_args)
	{
		F4SE::log::write_log(a_format.location, "INFO", std::format(a_format.text, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void warn(Format<std::type_identity_t<Args>...> a_format, Args&&... a_args)
	{
		F4SE::log::write_log(a_format.location, "WARN", std::format(a_format.text, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void error(Format<std::type_identity_t<Args>...> a_format, Args&&... a_args)
	{
		F4SE::log::write_log(a_format.location, "ERROR", std::format(a_format.text, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void critical(Format<std::type_identity_t<Args>...> a_format, Args&&... a_args)
	{
		F4SE::log::write_log(a_format.location, "CRIT", std::format(a_format.text, std::forward<Args>(a_args)...));
	}
}

using namespace std::literals;

#define DLLEXPORT __declspec(dllexport)

#endif
