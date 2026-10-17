// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ImpactPatterns
{
	struct Stroke
	{
		float x{}, y{}, motionX{}, motionY{}, radius{}, strength{};
	};
	struct Pattern
	{
		std::array<Stroke, 9> strokes{};
		size_t count{};
	};

	inline float BoundedRadius(float radius, float scale, float limit)
	{
		if (!std::isfinite(radius) || !std::isfinite(scale) || !std::isfinite(limit)) { return 0.0f; }
		return std::clamp(radius * scale, 0.0f, std::max(limit, 0.0f));
	}

	inline float StampRadius(float outerRadius, float surfaceScale, float span, float lean)
	{
		const float support = 1.0f + std::max(span, 0.0f) * (1.0f + std::clamp(lean, 0.0f, 1.0f));
		return outerRadius * std::clamp(surfaceScale, 0.0f, 1.0f) / support;
	}

	inline Pattern Explosion(float reach)
	{
		Pattern out{};
		if (!(reach > 0.0f) || !std::isfinite(reach)) { return out; }
		out.strokes[out.count++] = { 0, 0, 0, 0, reach * 0.48f, 1.0f };
		for (int i = 0; i < 6; ++i) {
			const float angle = i * 1.04719755f + 0.17f;
			const float x = std::cos(angle), y = std::sin(angle);
			out.strokes[out.count++] = { x * reach * 0.76f, y * reach * 0.76f,
				x * reach * 0.42f, y * reach * 0.42f, reach * 0.18f,
				(i % 2 ? 0.36f : 0.48f) };
		}
		return out;
	}

	inline Pattern Directional(float reach, float width, float forwardX, float forwardY, bool shout)
	{
		Pattern out{};
		if (!(reach > 0.0f) || !(width > 0.0f) || !std::isfinite(reach) || !std::isfinite(width)) { return out; }
		const float length = std::hypot(forwardX, forwardY);
		if (!(length > 0.001f) || !std::isfinite(length)) { return out; }
		const float fx = forwardX / length, fy = forwardY / length;
		const int lanes = shout ? 3 : 1;
		for (int lane = 0; lane < lanes; ++lane) {
			const float side = shout ? static_cast<float>(lane - 1) : 0.0f;
			for (int step = 0; step < 3; ++step) {
				const float t0 = step / 3.0f, t1 = (step + 1) / 3.0f;
				const float lateral0 = side * width * t0 * 0.52f;
				const float lateral1 = side * width * t1 * 0.52f;
				const float x0 = fx * reach * t0 - fy * lateral0;
				const float y0 = fy * reach * t0 + fx * lateral0;
				const float x1 = fx * reach * t1 - fy * lateral1;
				const float y1 = fy * reach * t1 + fx * lateral1;
				out.strokes[out.count++] = { x1, y1, x1 - x0, y1 - y0,
					width * (shout ? (0.14f + 0.13f * t1) : 0.5f),
					(1.0f - 0.5f * t1) * (side == 0 ? 1.0f : 0.65f) };
			}
		}
		return out;
	}
}
