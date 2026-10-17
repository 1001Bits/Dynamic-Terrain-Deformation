// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "SurfaceTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace RE
{
	class NiPoint3;
	class TESLandTexture;
}

namespace Surfaces
{

	struct Ground
	{
		Type     type{ Type::kUnknown };
		Response response{};
		// How deep marks go into the snow here, as a share of an ordinary snow mark: below 1
		// on stone and gravel that WinterMode covers with snow, which holds only a thin layer
		// (WinterHardGroundDepth), so the rocks under it are not dug up.
		float snowSink{ 1.0f };

		int profile{ -1 };
	};

	void LoadProfiles();

	void ReleaseProfiles();

	size_t      ProfileCount();
	const char* ProfileName(int a_index);

	// Compare every watched path, not just the newest timestamp. A future-dated file
	// must not hide changes to other modules; size also detects preserved-time copies.
	struct ProfileFileState
	{
		std::filesystem::path path;
		std::filesystem::file_time_type modified{};
		std::uintmax_t size{};
		bool exists{};
		bool operator==(const ProfileFileState&) const = default;
	};
	std::vector<ProfileFileState> ProfilesRevision();

	Ground GroundAt(const RE::NiPoint3& a_position);

	// The same, for a caller that already has the land height at a_position (skips the
	// second GetLandHeight).
	Ground GroundAt(const RE::NiPoint3& a_position, float a_landZ);

	// The surface a land texture is, by the rules GroundAt follows: a profile's Type, else
	// the texture's material type, else its name (a profile without a Type is never snow
	// by name alone), then WinterMode. a_landTexture may be null for a texture the LTEX
	// index does not know; a_texturePath is its diffuse texture.
	Type TypeFor(const RE::TESLandTexture* a_landTexture, std::string_view a_texturePath);

	// Drops GroundAt's per-LTEX cache.
	void ClearGroundCache();
}
