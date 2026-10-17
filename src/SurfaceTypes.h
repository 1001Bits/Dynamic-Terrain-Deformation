// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <cstdint>
#include <string_view>

namespace RE
{
	class BGSMaterialType;
	class BSShaderMaterial;
	class NiPoint3;
	class TESLandTexture;
}

namespace Surfaces
{
	enum class Type : uint8_t
	{
		kUnknown = 0,
		kSnow,
		kGrass,
		kDirt,
		kMud,
		kSand,
		kAsh,
		kGravel,
		kStone,

		kCount
	};

	const char* Name(Type a_type);

	struct Response
	{
		float depthScale{ 1.0f };

		float radiusScale{ 1.0f };

		float shoulder{ 0.3f };

		float decayScale{ 1.0f };

		float rimScale{ 1.0f };

		float print{ 1.0f };

		float clearanceScale{ 1.0f };
	};

	// The accepted range of each Response column, in declaration order. The main INI's
	// Response rows and the per-texture profiles clamp to the same limits. Decay is a
	// lifetime multiplier (Clipmap::RecoveryRate), so it is kept to 0.05 - 20.
	inline constexpr size_t      kResponseFields = 7;
	inline constexpr const char* kResponseFieldNames[kResponseFields]{ "depth", "radius",
		"shoulder", "decay", "rim", "print", "clearance" };
	inline constexpr float kResponseLow[kResponseFields]{ 0.0f, 0.05f, 0.0f, 0.05f, 0.0f, 0.0f, 0.0f };
	inline constexpr float kResponseHigh[kResponseFields]{ 8.0f, 8.0f, 0.95f, 20.0f, 4.0f, 1.0f, 256.0f };

	// The same range for the weather's decay scales.
	inline constexpr float kDecayScaleLow = 0.05f;
	inline constexpr float kDecayScaleHigh = 20.0f;

	const Response& ResponseFor(Type a_type);

	float MarkDepth(Type a_type, float a_ordinaryDepth, float a_sink, float a_worldX, float a_worldY);

	float RimHeight(float a_markDepth, float a_rimScale);

	bool MatchesKeywords(std::string_view a_name, std::string_view a_keywords);

	// Lowercased file name without directory or extension: the key texture profiles and
	// the land-texture index are looked up by.
	std::string TextureKey(std::string_view a_path);

	Type ClassifyTexturePath(std::string_view a_path, bool a_skipSnow = false);

	// A land texture's material type, when it is one of Fallout4.esm's ground materials.
	Type FromMaterialType(const RE::BGSMaterialType* a_material);

	// Everything a land texture says about its ground: its material type first, then the
	// name of its diffuse texture.
	Type FromLandTexture(const RE::TESLandTexture* a_landTexture);

	// Indexes every loaded LTEX by the key of its diffuse texture so a landscape draw's
	// layer textures can be traced back to their LTEX. Call each time the game data has
	// (re)loaded, then Reset() to drop what was classified from the old forms.
	void BuildLandTextureIndex();

	// Forgets the index: call before a data reload frees the LTEX forms it points at.
	void ClearLandTextureIndex();

	const RE::TESLandTexture* LandTextureForTexture(std::string_view a_texturePath);

	// Fallout 4 blends four layers in one landscape draw.
	inline constexpr size_t kLayers = 4;

	struct Classification
	{
		Type layers[kLayers]{};

		int8_t reveal{ -1 };

		bool revealUnderSnow{ false };

		int8_t snow{ -1 };

		uint8_t used{ 0 };

		// For each snow layer, the ground its texture shows without the snow, by its name (a
		// winter mod's repainted grass is still named grass); kUnknown when the name says
		// nothing.
		Type under[kLayers]{};

		// Each layer texture's WinterLook key (0 without a texture), for the draw to have it
		// measured.
		uint64_t keys[kLayers]{};
	};

	Classification Classify(RE::BSShaderMaterial* a_material);

	// Records the diffuse path of the texture set a landscape layer texture came from, for
	// textures that carry no name of their own (VR). Thread-safe; the land geometry builder
	// calls it as each layer is assigned. Kept across resets: it describes loaded textures.
	void NoteLayerTexture(const RE::NiTexture* a_texture, const char* a_diffusePath);

	// Drops every cached classification (render side and GroundAt). Call after a settings
	// reload or a rebuilt land texture index.
	void Reset();
}
