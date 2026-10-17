// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <span>

// Field accessors for engine objects CommonLibF4 does not describe (or describes with
// offsets that do not match 1.10.163). Each offset was read from the 1.10.163 code that
// uses the field; docs/FO4_PORT_NOTES.md names the function.

namespace GameLayouts
{
	// VR's attached state is 7, not the flat runtime's 8. Verified against the
	// native height query (0xFA030 accepts 3..7) and a loaded Sanctuary cell at +0x44.
	[[nodiscard]] inline bool CellAttached(const RE::TESObjectCELL* a_cell)
	{
		return a_cell && static_cast<std::uint8_t>(a_cell->cellState.get()) ==
			(REL::Module::IsVR() ? 7 : static_cast<std::uint8_t>(RE::TESObjectCELL::CELL_STATE::kAttached));
	}

	template <class T>
	[[nodiscard]] inline T Read(const void* a_object, std::ptrdiff_t a_offset)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(a_object) + a_offset);
	}

	// A node's children. VR's NiNode holds 0x40 more bytes before its child array, which
	// sits at +0x160 there and at +0x120 on the flat runtimes (F4SE VR's NiNode is 0x180
	// bytes); CommonLibF4's flat layout read other bytes as the array on VR and walked
	// memory that was no array. Only the slots before the array's first empty run are in
	// use (F4SE's own walk stops there); a slot in use can still be null.
	[[nodiscard]] inline std::span<RE::NiPointer<RE::NiAVObject>> Children(RE::NiNode* a_node)
	{
		if (!a_node) {
			return {};
		}
		const std::ptrdiff_t array = REL::Module::IsVR() ? 0x160 : 0x120;
		auto* const          data = Read<RE::NiPointer<RE::NiAVObject>*>(a_node, array + 0x08);
		const auto           capacity = Read<std::uint16_t>(a_node, array + 0x10);
		const auto           used = Read<std::uint16_t>(a_node, array + 0x12);
		if (!data) {
			return {};
		}
		return { data, std::min(used, capacity) };
	}

	// VR's headset node, whose world transform is where the player's eyes are this frame
	// (PlayerCharacter +0x7E0 on 1.2.72: the HmdNode F4VR-CommonFramework and FRIK read).
	// Null on the flat runtimes, which have none.
	[[nodiscard]] inline RE::NiAVObject* HeadsetNode(const RE::PlayerCharacter* a_player)
	{
		return a_player && REL::Module::IsVR() ? Read<RE::NiAVObject*>(a_player, 0x7E0) : nullptr;
	}

	// The water surface over an exterior cell, or NaN where it has none. The cell's own
	// height (XCLW, TESObjectCELL +0x60) when set; FLT_MAX there means its worldspace's
	// default (TESWorldSpace +0x21C: the PDB layout on 1.10.163; 1.10.163, 1.11.240 and VR
	// 1.2.72 all read the Commonwealth's 450 there live. NG 1.10.984 is unverified and takes
	// no default). Placed water objects are not included.
	[[nodiscard]] inline float CellWaterHeight(const RE::TESObjectCELL* a_cell)
	{
		constexpr float none = std::numeric_limits<float>::quiet_NaN();
		if (!a_cell || !a_cell->IsExterior() || !a_cell->HasWater()) {
			return none;
		}
		float height = a_cell->waterHeight;
		if (!(std::abs(height) < 1.0e30f)) {
			const auto* world = a_cell->worldSpace;
			const auto  version = REL::Module::get().version();
			const bool  verified = REL::Module::IsVR() || version == REL::Version{ 1, 10, 163, 0 } ||
			                      version == REL::Version{ 1, 11, 240, 0 };
			if (!world || !verified) {
				return none;
			}
			height = Read<float>(world, 0x21C);
		}
		return std::isfinite(height) && std::abs(height) < 1.0e6f ? height : none;
	}

	// TESLandTexture: CommonLibF4's textureSet (+0x20) and materialType (+0x30) match
	// TESLandTexture::GetTextureSet / GetHavokMaterialType on 1.10.163.
	[[nodiscard]] inline RE::BGSTextureSet* LandTextureSet(const RE::TESLandTexture* a_ltex)
	{
		return a_ltex ? a_ltex->textureSet : nullptr;
	}

	[[nodiscard]] inline RE::BGSMaterialType* LandMaterialType(const RE::TESLandTexture* a_ltex)
	{
		return a_ltex ? a_ltex->materialType : nullptr;
	}

	// BGSTextureSet: TESTexture textures[8] at +0x80, each { vtable, BSFixedString name }.
	[[nodiscard]] inline const char* DiffusePath(const RE::BGSTextureSet* a_set)
	{
		if (!a_set) {
			return nullptr;
		}
		const auto& name = a_set->textures[0].textureName;
		return name.empty() ? nullptr : name.c_str();
	}

	// BSLightingShaderMaterialLandscape (BSDFPrePassShader's landscape SetupMaterial):
	// layer 0's diffuse is the base material's texture at +0x48 (bound to t0), layers 1-3
	// at +0x110/+0x118/+0x120 (t1-t3); the number of extra layers is the u32 at +0x10C.
	// VR's material base is 0x10 smaller, so every one of these sits 0x10 lower there
	// (its constructors 0x14280B8B0 / 0x14280FA80).
	[[nodiscard]] inline RE::NiTexture* LandscapeLayerDiffuse(const RE::BSShaderMaterial* a_material, size_t a_layer)
	{
		if (!a_material || a_layer >= 4) {
			return nullptr;
		}
		const std::ptrdiff_t shift = REL::Module::IsVR() ? -0x10 : 0;
		if (a_layer == 0) {
			return Read<RE::NiTexture*>(a_material, 0x48 + shift);
		}
		const auto extra = Read<std::uint32_t>(a_material, 0x10C + shift);
		if (a_layer > extra) {
			return nullptr;
		}
		return Read<RE::NiTexture*>(a_material,
			0x110 + shift + 8 * static_cast<std::ptrdiff_t>(a_layer - 1));
	}

	// Sky: the weather mode (+0x36C) and the current weather's blend-in fraction (+0x360),
	// both 0x10 lower on VR (VR Sky::IsSnowing reads +0x350, Sky::Update compares +0x35C).
	// currentWeather (+0x48) and lastWeather (+0x50) are the same everywhere.
	[[nodiscard]] inline std::uint32_t SkyMode(const RE::Sky* a_sky)
	{
		return Read<std::uint32_t>(a_sky, REL::Module::IsVR() ? 0x35C : 0x36C);
	}

	[[nodiscard]] inline float SkyCurrentWeatherPct(const RE::Sky* a_sky)
	{
		return Read<float>(a_sky, REL::Module::IsVR() ? 0x350 : 0x360);
	}

	[[nodiscard]] inline std::string_view TextureName(const RE::NiTexture* a_texture)
	{
		if (!a_texture) {
			return {};
		}
		const auto& name = a_texture->name;
		return name.empty() ? std::string_view{} : std::string_view(name.c_str());
	}
}
