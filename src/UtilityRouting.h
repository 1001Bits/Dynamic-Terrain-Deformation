// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace UtilityRouting
{
	// A projection matrix's w-from-w entry ([3][3]) is 0 for a perspective projection and
	// 1 for an orthographic one, whichever way the matrix is stored, so this works on
	// Fallout 4's ViewData::projMat without assuming its convention.
	inline bool IsPerspectiveProjection(const float* a_projection)
	{
		if (!a_projection) {
			return true;
		}

		const float w = a_projection[15];
		return (w < 0.0f ? -w : w) < 0.5f;
	}
}
