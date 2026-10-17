// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

namespace FootprintProfiles
{
	enum class Shape : uint32_t
	{
		kBoot = 1,
		kPaw = 2,
		kClaw = 3,
		kHoof = 4,
		kMechanical = 5,
		kHeavy = 6,
	};

	struct Profile
	{
		Shape shape;
		float lengthScale;
		float widthScale;
		float depthScale;
	};

	constexpr Shape Select(bool a_deathclaw, bool a_mirelurk, bool a_radstag, bool a_yaoguai,
		bool a_dog, bool a_robot, bool a_behemoth, bool a_booted)
	{
		if (a_deathclaw || a_mirelurk) {
			return Shape::kClaw;
		}
		if (a_radstag) {
			return Shape::kHoof;
		}
		if (a_robot) {
			return Shape::kMechanical;
		}
		if (a_behemoth) {
			return Shape::kHeavy;
		}
		if (a_yaoguai || a_dog) {
			return Shape::kPaw;
		}
		return a_booted ? Shape::kBoot : Shape::kPaw;
	}

	constexpr Profile For(Shape a_shape)
	{
		switch (a_shape) {
		case Shape::kClaw:
			return { a_shape, 1.25f, 1.15f, 1.2f };
		case Shape::kHoof:
			return { a_shape, 1.1f, 0.9f, 1.1f };
		case Shape::kMechanical:
			return { a_shape, 1.1f, 1.15f, 1.2f };
		case Shape::kHeavy:
			return { a_shape, 1.35f, 1.2f, 1.35f };
		case Shape::kPaw:
			return { a_shape, 1.0f, 1.1f, 1.0f };
		case Shape::kBoot:
		default:
			return { Shape::kBoot, 1.0f, 1.0f, 1.0f };
		}
	}
}
