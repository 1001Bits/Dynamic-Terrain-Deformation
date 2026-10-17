// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Offsets.h"

#include "GameLayouts.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSurface.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"
#include "WinterLook.h"

#include <iterator>
#include <shared_mutex>
#include <unordered_map>

namespace Surfaces
{
	namespace
	{
		constexpr Type kMatchOrder[] = {
			Type::kAsh,
			Type::kMud,
			Type::kSand,
			Type::kGravel,
			Type::kSnow,
			Type::kDirt,
			Type::kStone,
			Type::kGrass,
		};

		constexpr Type kSnowRevealPreference[] = {
			Type::kDirt,
			Type::kMud,
			Type::kGravel,
			Type::kAsh,
			Type::kSand,
			Type::kStone,
			Type::kGrass,
		};

		constexpr Type kRevealPreference[] = {
			Type::kDirt,
			Type::kMud,
			Type::kGravel,
			Type::kAsh,
			Type::kSand,
			Type::kStone,
		};

		// BSLightingShaderMaterialLandscape::GetFeature (vfunc 5) on 1.10.163.
		constexpr std::int32_t kLandscapeFeature = 0x13;

		// Fallout4.esm's ground material types (MATT), by form id.
		struct KnownMaterial
		{
			std::uint32_t formID;
			Type          type;
		};

		constexpr KnownMaterial kKnownMaterials[] = {
			{ 0x00012F46, Type::kGrass },   // MaterialGrass
			{ 0x00021DE5, Type::kGrass },   // MaterialGrassStairs
			{ 0x000464B1, Type::kGrass },   // MaterialGroundGrassBush
			{ 0x00012F38, Type::kDirt },    // Material_Dirt
			{ 0x00021DDE, Type::kDirt },    // MaterialDirtStairs
			{ 0x0003A51D, Type::kDirt },    // MaterialGroundDirtLeaves
			{ 0x00012F48, Type::kMud },     // MaterialDirtMud
			{ 0x00012F3E, Type::kSand },    // MaterialDirtlSand
			{ 0x00026F80, Type::kSand },    // MaterialDirtSandSandbag
			{ 0x0001C151, Type::kGravel },  // MaterialDirtGravel
			{ 0x00012F35, Type::kGravel },  // MaterialStoneBroken
			{ 0x00012F45, Type::kSnow },    // MaterialDirtSnow
			{ 0x00012F47, Type::kSnow },    // MaterialDirtSnowIce
			{ 0x00052ED0, Type::kSnow },    // MaterialDirtSnowStairs
			{ 0x00012F34, Type::kStone },   // Material_Stone
			{ 0x00012F36, Type::kStone },   // MaterialStoneHeavy
			{ 0x0001686B, Type::kStone },   // MaterialStoneAsStairs
			{ 0x0002EE2C, Type::kStone },   // MaterialStoneStairs
			{ 0x00022958, Type::kStone },   // MaterialStoneBrick
			{ 0x00055F39, Type::kStone },   // MaterialStoneConcrete
			{ 0x000223D1, Type::kStone },   // Material_Metal
			{ 0x000223D2, Type::kStone },   // Material__GenericMaster
		};

		const std::string& KeywordsFor(Type a_type)
		{
			static const std::string empty{};

			switch (a_type) {
			case Type::kSnow:   return Settings::surfaceSnow;
			case Type::kGrass:  return Settings::surfaceGrass;
			case Type::kDirt:   return Settings::surfaceDirt;
			case Type::kMud:    return Settings::surfaceMud;
			case Type::kSand:   return Settings::surfaceSand;
			case Type::kAsh:    return Settings::surfaceAsh;
			case Type::kGravel: return Settings::surfaceGravel;
			case Type::kStone:  return Settings::surfaceStone;
			default:            return empty;
			}
		}

		bool Matches(std::string_view a_name, std::string_view a_keywords)
		{
			size_t start = 0;
			while (start <= a_keywords.size()) {
				const auto comma = a_keywords.find(',', start);
				const auto end = comma == std::string_view::npos ? a_keywords.size() : comma;

				const auto keyword = a_keywords.substr(start, end - start);

				if (!keyword.empty()) {
					if (keyword.front() == '^') {
						const auto body = keyword.substr(1);
						if (!body.empty() && a_name.rfind(body, 0) == 0) {
							return true;
						}
					} else if (a_name.find(keyword) != std::string_view::npos) {
						return true;
					}
				}

				if (comma == std::string_view::npos) {
					break;
				}
				start = comma + 1;
			}
			return false;
		}

		std::string Lowered(std::string_view a_text)
		{
			std::string out(a_text);
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// ── the texture -> LTEX index ───────────────────────────────────────

		std::shared_mutex                                         g_indexLock;
		std::unordered_map<std::string, const RE::TESLandTexture*> g_byTexture;

		// ── per-material classification cache ──────────────────────────────

		std::mutex                                   g_cacheLock;
		std::unordered_map<uint64_t, Classification> g_cache;
		constexpr size_t                             kMaxCacheEntries = 512;

		// Keyed by the layer textures' names rather than their addresses: a texture that is
		// freed and whose address is reused by another must not inherit its classification.
		uint64_t CacheKey(const std::string_view (&a_names)[kLayers])
		{
			uint64_t key = 0xcbf29ce484222325ull;
			for (const auto name : a_names) {
				for (const char c : name) {
					key ^= static_cast<uint8_t>(c);
					key *= 0x100000001b3ull;
				}
				// A separator, so ("ab", "c") and ("a", "bc") differ.
				key ^= 0xFFu;
				key *= 0x100000001b3ull;
			}
			return key;
		}

		std::atomic<uint32_t> g_loggedPlain{ 0 };
		std::atomic<uint32_t> g_loggedSnow{ 0 };
		constexpr uint32_t    kMaxLogged = 16;

		// Paths of landscape layer textures that carry no name (VR), recorded as the land
		// geometry builder assigns each layer its texture set. A freed texture's address can
		// be reused, but a landscape texture reaches a material only through that builder,
		// which records it again first.
		std::mutex                                            g_layerPathLock;
		std::unordered_map<const RE::NiTexture*, std::string> g_layerPaths;
		constexpr size_t                                      kMaxLayerPaths = 4096;

		std::string RecordedLayerPath(const RE::NiTexture* a_texture)
		{
			const std::scoped_lock lock(g_layerPathLock);
			const auto             it = g_layerPaths.find(a_texture);
			return it == g_layerPaths.end() ? std::string{} : it->second;
		}

		void LogClassification(const Classification& a_result, const std::string_view (&a_names)[kLayers])
		{
			if (!Settings::logSurfaceClassification) {
				return;
			}

			auto& budget = a_result.snow >= 0 ? g_loggedSnow : g_loggedPlain;
			if (budget.fetch_add(1) >= kMaxLogged) {
				return;
			}

			std::string detail;
			for (size_t i = 0; i < kLayers; ++i) {
				const auto path = a_names[i];
				if (path.empty()) {
					continue;
				}
				const auto slash = path.find_last_of("\\/");
				const auto leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
				detail += std::format("\n      [{}] {:<8} {}", i, Name(a_result.layers[i]), leaf);
			}

			logger::info("Landscape surfaces ({} layers, snow={}, reveal={}{}):{}",
				a_result.used,
				a_result.snow < 0 ? "no" : "yes",
				a_result.reveal < 0 ? "none" : Name(a_result.layers[a_result.reveal]),
				a_result.revealUnderSnow ? " under-snow" : "",
				detail);
		}

		// A landscape material whose layers have no names cannot be classified. Record the
		// raw layout once or twice so the cause (no texture, or an unnamed one) is visible.
		std::atomic<uint32_t> g_loggedUnnamed{ 0 };

		// Copies bytes that may not be readable; false (and zeros) when they are not.
		bool GuardedCopy(void* a_to, const void* a_from, size_t a_size)
		{
			__try {
				std::memcpy(a_to, a_from, a_size);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				std::memset(a_to, 0, a_size);
				return false;
			}
		}

		// The string pool entry behind a texture's name: its header and first characters.
		std::string NameBytes(const void* a_entry)
		{
			if (!a_entry) {
				return "none";
			}
			unsigned char bytes[0x30]{};
			if (!GuardedCopy(bytes, a_entry, sizeof(bytes))) {
				return "unreadable";
			}
			std::string out;
			for (size_t i = 0; i < 0x18; ++i) {
				out += std::format("{:02X}", bytes[i]);
			}
			out += " '";
			for (size_t i = 0x18; i < sizeof(bytes) && bytes[i]; ++i) {
				out += bytes[i] >= 0x20 && bytes[i] < 0x7F ? static_cast<char>(bytes[i]) : '?';
			}
			return out + "'";
		}

		void LogUnnamedLayers(const RE::BSShaderMaterial* a_material, RE::NiTexture* const* a_textures)
		{
			if (!Settings::logSurfaceClassification || g_loggedUnnamed.fetch_add(1) >= 2) {
				return;
			}

			const auto base = REL::Module::get().base();
			const auto rva = [base](const void* a_object) {
				const auto vtable = a_object ? *reinterpret_cast<const std::uintptr_t*>(a_object) : 0;
				return vtable > base ? vtable - base : 0;
			};
			const auto words = [a_material](std::ptrdiff_t a_from, std::ptrdiff_t a_to) {
				std::string out;
				for (auto offset = a_from; offset < a_to; offset += 8) {
					out += std::format(" +{:X}={:X}", offset, GameLayouts::Read<std::uintptr_t>(a_material, offset));
				}
				return out;
			};

			std::string textures;
			for (size_t i = 0; i < kLayers; ++i) {
				const auto* texture = a_textures[i];
				const auto* entry = texture ? GameLayouts::Read<const void*>(texture, 0x10) : nullptr;
				textures += std::format("\n      [{}] {} vtable+{:X} name@{} {}", i, static_cast<const void*>(texture),
					rva(texture), entry, NameBytes(entry));
			}

			logger::info("Landscape material {} (vtable+{:X}) has no named layers:{}\n     {}\n     {}",
				static_cast<const void*>(a_material), rva(a_material), textures,
				words(0x30, 0x60), words(0xF0, 0x128));
		}
	}

	void NoteLayerTexture(const RE::NiTexture* a_texture, const char* a_diffusePath)
	{
		if (!a_texture || !a_diffusePath || !*a_diffusePath) {
			return;
		}
		const std::scoped_lock lock(g_layerPathLock);
		if (g_layerPaths.size() >= kMaxLayerPaths && !g_layerPaths.contains(a_texture)) {
			g_layerPaths.clear();
		}
		g_layerPaths[a_texture] = a_diffusePath;
	}

	std::string TextureKey(std::string_view a_path)
	{
		const auto slash = a_path.find_last_of("\\/");
		auto       leaf = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);

		const auto dot = leaf.find_last_of('.');
		if (dot != std::string_view::npos && dot > 0) {
			leaf = leaf.substr(0, dot);
		}

		auto key = Lowered(leaf);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
			key.pop_back();
		}
		return key;
	}

	Type ClassifyTexturePath(std::string_view a_path, bool a_skipSnow)
	{
		if (a_path.empty()) {
			return Type::kUnknown;
		}

		const auto slash = a_path.find_last_of("\\/");
		const auto leaf = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
		const auto lowered = Lowered(leaf);

		for (const Type candidate : kMatchOrder) {
			if (a_skipSnow && candidate == Type::kSnow) {
				continue;
			}
			if (Matches(lowered, KeywordsFor(candidate))) {
				return candidate;
			}
		}

		return Type::kUnknown;
	}

	Type FromMaterialType(const RE::BGSMaterialType* a_material)
	{
		// A mod's material usually parents to one of the vanilla ground materials.
		for (int depth = 0; a_material && depth < 8; ++depth, a_material = a_material->parentType) {
			const auto id = a_material->GetFormID();
			for (const auto& known : kKnownMaterials) {
				if (known.formID == id) {
					return known.type;
				}
			}
		}
		return Type::kUnknown;
	}

	Type FromLandTexture(const RE::TESLandTexture* a_landTexture)
	{
		if (!a_landTexture) {
			return Type::kUnknown;
		}

		const auto fromMaterial = FromMaterialType(GameLayouts::LandMaterialType(a_landTexture));
		if (fromMaterial != Type::kUnknown) {
			return fromMaterial;
		}

		const char* path = GameLayouts::DiffusePath(GameLayouts::LandTextureSet(a_landTexture));
		return ClassifyTexturePath(path ? std::string_view(path) : std::string_view{});
	}

	void ClearLandTextureIndex()
	{
		const std::unique_lock lock(g_indexLock);
		g_byTexture.clear();
	}

	void BuildLandTextureIndex()
	{
		auto* data = Offsets::ReadGlobalPointer<RE::TESDataHandler>(Offsets::DataHandlerSingleton);
		if (!data) {
			// Never keep pointers into forms a reload may have freed.
			ClearLandTextureIndex();
			return;
		}

		const auto& forms = data->formArrays[std::to_underlying(RE::ENUM_FORM_ID::kLTEX)];

		std::unordered_map<std::string, const RE::TESLandTexture*> index;
		index.reserve(forms.size());

		size_t typed = 0;
		for (auto* form : forms) {
			const auto* ltex = static_cast<const RE::TESLandTexture*>(form);
			if (!ltex) {
				continue;
			}
			const char* path = GameLayouts::DiffusePath(GameLayouts::LandTextureSet(ltex));
			if (!path) {
				continue;
			}
			const auto key = TextureKey(path);
			if (key.empty()) {
				continue;
			}
			// The first LTEX to claim a texture wins; later ones with the same texture
			// almost always share its material too.
			if (index.try_emplace(key, ltex).second &&
				FromMaterialType(GameLayouts::LandMaterialType(ltex)) != Type::kUnknown) {
				++typed;
			}
		}

		{
			const std::unique_lock lock(g_indexLock);
			g_byTexture = std::move(index);
		}

		logger::info("Surfaces: indexed {} land textures ({} with a known ground material) from {} LTEX forms",
			g_byTexture.size(), typed, forms.size());
	}

	const RE::TESLandTexture* LandTextureForTexture(std::string_view a_texturePath)
	{
		const auto            key = TextureKey(a_texturePath);
		const std::shared_lock lock(g_indexLock);
		const auto            it = g_byTexture.find(key);
		return it == g_byTexture.end() ? nullptr : it->second;
	}

	const Response& ResponseFor(Type a_type)
	{
		const auto index = static_cast<size_t>(a_type);
		return index < static_cast<size_t>(Type::kCount) ? Settings::surfaceResponse[index] :
		                                                   Settings::surfaceResponse[0];
	}

	float MarkDepth(Type a_type, float a_ordinaryDepth, float a_sink, float a_worldX, float a_worldY)
	{
		if (a_type != Type::kSnow) {
			return a_ordinaryDepth;
		}

		const float lower = std::clamp(Settings::snowTrenchCoverage, 0.5f, 0.95f);
		const float cover = SnowCoverage::At(a_worldX, a_worldY);
		const float t = std::clamp((cover - lower) / std::max(1.0f - lower, 1e-3f), 0.0f, 1.0f);
		const float snowy = t * t * (3.0f - 2.0f * t);

		const float blanket = SnowSurface::BlanketFor(snowy, a_worldX, a_worldY);
		// a_sink: the ground's share (thin snow over stone and gravel) times how heavy the
		// actor is (FootGeometry::Heavy, up to 2): a deathclaw sinks further than a person.
		const float trench = blanket * std::clamp(Settings::snowTrenchDepth, 0.0f, 4.0f) *
		                     std::clamp(a_sink, 0.0f, 2.0f);
		const float depth = std::max(a_ordinaryDepth, trench);

		// A low mesh the blanket buries (a rock, a slab, a road) stays under the snow: a mark
		// stops at its top instead of digging it up, with a little snow left over it.
		const float buried = Shelter::BuriedAt(a_worldX, a_worldY);
		if (buried > 0.0f && blanket > 0.0f) {
			constexpr float kSnowOverMesh = 2.0f;
			return std::min(depth, std::max(blanket - buried - kSnowOverMesh, 0.0f));
		}
		return depth;
	}

	float RimHeight(float a_markDepth, float a_rimScale)
	{
		const float reference = std::lerp(a_markDepth,
			std::max(Settings::stampDepth, 0.0f) * Weather::DepthScale(),
			std::clamp(Settings::stampRimIndependence, 0.0f, 1.0f));

		return reference * Settings::stampRimHeight * a_rimScale;
	}

	bool MatchesKeywords(std::string_view a_name, std::string_view a_keywords)
	{
		return Matches(a_name, a_keywords);
	}

	const char* Name(Type a_type)
	{
		switch (a_type) {
		case Type::kSnow:   return "snow";
		case Type::kGrass:  return "grass";
		case Type::kDirt:   return "dirt";
		case Type::kMud:    return "mud";
		case Type::kSand:   return "sand";
		case Type::kAsh:    return "ash";
		case Type::kGravel: return "gravel";
		case Type::kStone:  return "stone";
		default:            return "unknown";
		}
	}

	Classification Classify(RE::BSShaderMaterial* a_material)
	{
		Classification result{};

		if (!Settings::enableSurfaceClassification || !a_material) {
			return result;
		}

		if (static_cast<std::int32_t>(a_material->GetFeature()) != kLandscapeFeature) {
			static bool reported = false;
			if (!reported) {
				reported = true;
				logger::warn("A landscape draw's material reports feature {} (expected {}); its "
							 "layers are not classified",
					static_cast<std::int32_t>(a_material->GetFeature()), kLandscapeFeature);
			}
			return result;
		}

		RE::NiTexture*   textures[kLayers]{};
		std::string_view names[kLayers]{};
		std::string      recorded[kLayers];
		for (size_t i = 0; i < kLayers; ++i) {
			textures[i] = GameLayouts::LandscapeLayerDiffuse(a_material, i);
			names[i] = GameLayouts::TextureName(textures[i]);
			if (names[i].empty() && textures[i]) {
				recorded[i] = RecordedLayerPath(textures[i]);
				names[i] = recorded[i];
			}
		}

		const uint64_t key = CacheKey(names);
		{
			const std::scoped_lock lock(g_cacheLock);
			if (const auto it = g_cache.find(key); it != g_cache.end()) {
				return it->second;
			}
		}

		Type under[kLayers]{};

		for (size_t i = 0; i < kLayers; ++i) {
			const auto path = names[i];
			if (path.empty()) {
				result.layers[i] = Type::kUnknown;
				continue;
			}

			++result.used;
			result.keys[i] = WinterLook::Register(path);

			// The same rules the marks follow (GroundAt): a profile's Type, the texture's own
			// LTEX material when the index knows it, its name, then WinterMode.
			const Type type = TypeFor(LandTextureForTexture(path), path);

			result.layers[i] = type;
			if (type == Type::kSnow) {
				if (result.snow < 0) {
					result.snow = static_cast<int8_t>(i);
				}
				// What the snow texture would be without its snow, to reveal beneath it.
				under[i] = ClassifyTexturePath(path, true);
			}
		}

		std::copy(std::begin(under), std::end(under), std::begin(result.under));

		if (result.snow >= 0) {
			for (const Type wanted : kRevealPreference) {
				const auto found = std::find(std::begin(result.layers), std::end(result.layers), wanted);
				if (found != std::end(result.layers)) {
					result.reveal = static_cast<int8_t>(std::distance(std::begin(result.layers), found));
					break;
				}
			}

			if (result.reveal < 0) {
				for (const Type wanted : kSnowRevealPreference) {
					auto* found = std::find(std::begin(under), std::end(under), wanted);
					if (found != std::end(under)) {
						result.reveal = static_cast<int8_t>(std::distance(std::begin(under), found));
						result.revealUnderSnow = true;
						break;
					}
				}
			}
		}

		LogClassification(result, names);
		if (result.used == 0) {
			LogUnnamedLayers(a_material, textures);
		}

		{
			const std::scoped_lock lock(g_cacheLock);
			if (g_cache.size() >= kMaxCacheEntries) {
				g_cache.clear();
			}
			g_cache[key] = result;
		}

		return result;
	}

	void Reset()
	{
		{
			const std::scoped_lock lock(g_cacheLock);
			g_cache.clear();
		}
		g_loggedPlain.store(0);
		g_loggedSnow.store(0);
		g_loggedUnnamed.store(0);

		ClearGroundCache();
	}
}
