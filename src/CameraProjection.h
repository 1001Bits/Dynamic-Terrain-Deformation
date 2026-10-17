// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace CameraProjection
{

	enum class Read
	{
		kRead,
		kEmpty,
		kFault,
	};

	// Flat State::ApplyCameraStateData writes the selected camera (which can come
	// from cameraDataCache, without updating State::cameraState) to the current
	// thread's renderer shadow state. Verified on 1.10.163 and 1.11.240:
	// TLS block +0xB20 -> Context; shadowState +0x1B70; projMat +0x760.
	inline constexpr std::uintptr_t kThreadContext = 0xB20;
	inline constexpr std::uintptr_t kContextProjection = 0x1B70 + 0x760;

	inline Read Flat(std::uintptr_t a_tlsVector, std::uint32_t a_tlsIndex,
		std::uintptr_t a_defaultSlot, float (&a_out)[16])
	{
		__try {
			std::uintptr_t context = 0;
			if (a_tlsVector) {
				const auto block = reinterpret_cast<const std::uintptr_t*>(a_tlsVector)[a_tlsIndex];
				if (block) {
					context = *reinterpret_cast<const std::uintptr_t*>(block + kThreadContext);
				}
			}
			if (!context && a_defaultSlot) {
				context = *reinterpret_cast<const std::uintptr_t*>(a_defaultSlot);
			}
			if (!context) {
				return Read::kEmpty;
			}
			std::memcpy(a_out, reinterpret_cast<const void*>(context + kContextProjection), sizeof(a_out));
			return Read::kRead;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return Read::kFault;
		}
	}
}
