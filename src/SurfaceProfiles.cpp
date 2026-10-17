// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "GameLayouts.h"
#include "Globals.h"
#include "Settings.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "WinterLook.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Surfaces
{
	namespace
	{

		constexpr auto kRoot = "Data/F4SE/Plugins/NMN_DeformableTerrain";
		constexpr auto kPatches = L"Patches";
		constexpr auto kFile = "Surfaces.ini";

		struct Overrides
		{
			std::optional<float> depth;
			std::optional<float> radius;
			std::optional<float> shoulder;
			std::optional<float> decay;
			std::optional<float> rim;
			std::optional<float> print;
			std::optional<float> clearance;
		};

		struct Slot
		{
			std::optional<float> value;
			std::string          source;
		};

		struct StampDefaults
		{
			Slot snowRepose, snowSpan, snowNoise, snowLean, snowChurn;
			Slot repose, span, noise, lean, churn;
		};

		constexpr auto kStampSection = "stampdefaults";

		struct Profile
		{
			std::string name;
			std::string source;

			std::optional<Type> type;

			Overrides overrides{};

			std::vector<std::string> textures;

			std::vector<uint32_t> forms;
		};

		// Rebuilt by LoadProfiles on the frame thread; read by GroundAt there and by the
		// landscape classification on the render thread (VR's submission thread), so the
		// three are swapped in together under the lock.
		std::shared_mutex                    g_profilesLock;
		std::vector<Profile>                 g_profiles;
		std::unordered_map<std::string, int> g_byTexture;
		std::unordered_map<uint32_t, int>    g_byForm;

		std::mutex                           g_cacheLock;
		std::unordered_map<uint64_t, Ground> g_cache;
		constexpr size_t                     kMaxCacheEntries = 1024;

		std::string Lowered(std::string_view a_text)
		{
			std::string out(a_text);
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return a_text.substr(first, last - first + 1);
		}

		// A folder name for the log. path::string() throws for a name outside the ANSI code
		// page; this never does.
		std::string Utf8(const std::filesystem::path& a_path)
		{
			const auto& wide = a_path.native();
			if (wide.empty()) {
				return {};
			}
			const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
				nullptr, 0, nullptr, nullptr);
			if (size <= 0) {
				return {};
			}
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
				size, nullptr, nullptr);
			return out;
		}

		bool SameName(const std::filesystem::path& a_name, const wchar_t* a_other)
		{
			return _wcsicmp(a_name.c_str(), a_other) == 0;
		}

		Type TypeFromName(std::string_view a_name)
		{
			const auto lowered = Lowered(a_name);
			for (size_t i = 0; i < static_cast<size_t>(Type::kCount); ++i) {
				const auto type = static_cast<Type>(i);
				if (lowered == Name(type)) {
					return type;
				}
			}
			return Type::kCount;
		}

		void SplitInto(std::string_view a_value, std::vector<std::string>& a_out,
			bool a_asTextureKey)
		{
			size_t start = 0;
			while (start <= a_value.size()) {
				const auto comma = a_value.find(',', start);
				const auto end = comma == std::string_view::npos ? a_value.size() : comma;

				const auto item = Trim(a_value.substr(start, end - start));
				if (!item.empty()) {
					a_out.push_back(a_asTextureKey ? TextureKey(item) : Lowered(item));
				}

				if (comma == std::string_view::npos) {
					break;
				}
				start = comma + 1;
			}
		}

		// Form ids are always hexadecimal, with or without 0x, as xEdit and the console show
		// them (000AB72C). The whole text must be the id.
		bool ParseFormID(std::string_view a_value, uint32_t& a_out)
		{
			auto text = Trim(a_value);
			if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
				text.remove_prefix(2);
			}
			if (text.empty()) {
				return false;
			}

			uint32_t   parsed = 0;
			const auto end = text.data() + text.size();
			const auto [stop, error] = std::from_chars(text.data(), end, parsed, 16);
			if (error != std::errc{} || stop != end) {
				return false;
			}
			a_out = parsed;
			return true;
		}

		// A profile number, clamped to the range the main INI's Response column has.
		float ClampedOverride(float a_value, size_t a_field, std::string_view a_profile,
			std::string_view a_source)
		{
			const float low = kResponseLow[a_field];
			const float high = kResponseHigh[a_field];
			const float out = std::clamp(a_value, low, high);
			if (out != a_value) {
				logger::warn("Surfaces: [{}] in {} - {} {} is outside {} to {}, using {}",
					a_profile, a_source, kResponseFieldNames[a_field], a_value, low, high, out);
			}
			return out;
		}

		void ParseFile(const std::filesystem::path& a_path, std::string_view a_source,
			std::vector<Profile>& a_profiles, StampDefaults& a_stampDefaults)
		{
			std::ifstream file(a_path);
			if (!file) {
				return;
			}

			Profile     current{};
			bool        open = false;
			bool        inStamp = false;
			std::string line;

			const auto commit = [&]() {
				if (!open) {
					return;
				}
				if (current.textures.empty() && current.forms.empty()) {
					logger::warn("Surfaces: profile [{}] in {} matches nothing - it needs "
								 "Textures or LandTextures",
						current.name, a_source);
				} else {
					a_profiles.push_back(std::move(current));
				}
				current = Profile{};
				open = false;
			};

			while (std::getline(file, line)) {
				const auto trimmed = Trim(line);
				if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
					continue;
				}

				if (trimmed.front() == '[') {
					commit();
					const auto close = trimmed.find(']');
					current.name = std::string(
						Trim(trimmed.substr(1, close == std::string_view::npos ?
													std::string_view::npos :
													close - 1)));
					current.source = std::string(a_source);

					inStamp = Lowered(current.name) == kStampSection;
					open = !inStamp;
					continue;
				}

				if (inStamp) {
					const auto eq = trimmed.find('=');
					if (eq == std::string_view::npos) {
						continue;
					}
					const auto skey = Lowered(Trim(trimmed.substr(0, eq)));
					auto       sbody = trimmed.substr(eq + 1);
					if (const auto c = sbody.find_first_of(";#");
						c != std::string_view::npos) {
						sbody = sbody.substr(0, c);
					}
					float sval = 0.0f;
					if (!Settings::ParseFloat(Trim(sbody), sval)) {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a number for {}",
							current.name, a_source, Trim(sbody), skey);
						continue;
					}

					const auto take = [&](Slot& a_slot, float a_lo, float a_hi) {
						const float clamped = std::clamp(sval, a_lo, a_hi);
						if (clamped != sval) {
							logger::warn("Surfaces: {} in {} is outside {} to {}, using {}",
								skey, a_source, a_lo, a_hi, clamped);
						}
						if (a_slot.value && *a_slot.value != clamped) {
							logger::info("Surfaces: {} = {} in {} replaces {} from {}",
								skey, clamped, a_source, *a_slot.value, a_slot.source);
						}
						a_slot.value = clamped;
						a_slot.source = std::string(a_source);
					};

					if (skey == "snowstampreposerate") {
						take(a_stampDefaults.snowRepose, -1.0f, 1.0f);
					} else if (skey == "snowstamprimspan") {
						take(a_stampDefaults.snowSpan, -1.0f, 16.0f);
					} else if (skey == "snowstamprimnoise") {
						take(a_stampDefaults.snowNoise, -1.0f, 8.0f);
					} else if (skey == "snowstamprimlean") {
						take(a_stampDefaults.snowLean, -1.0f, 1.0f);
					} else if (skey == "snowstampchurn") {
						take(a_stampDefaults.snowChurn, -1.0f, 8.0f);
					} else if (skey == "stampreposerate") {
						take(a_stampDefaults.repose, 0.0f, 1.0f);
					} else if (skey == "stamprimspan") {
						take(a_stampDefaults.span, 0.0f, 16.0f);
					} else if (skey == "stamprimnoise") {
						take(a_stampDefaults.noise, 0.0f, 8.0f);
					} else if (skey == "stamprimlean") {
						take(a_stampDefaults.lean, 0.0f, 1.0f);
					} else if (skey == "stampchurn") {
						take(a_stampDefaults.churn, 0.0f, 8.0f);
					} else {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a stamp default "
									 "this build knows",
							current.name, a_source, skey);
					}
					continue;
				}

				const auto equals = trimmed.find('=');
				if (equals == std::string_view::npos || !open) {
					continue;
				}

				const auto key = Lowered(Trim(trimmed.substr(0, equals)));

				auto       body = trimmed.substr(equals + 1);
				const auto comment = body.find_first_of(";#");
				if (comment != std::string_view::npos) {
					body = body.substr(0, comment);
				}
				const auto value = Trim(body);

				if (key == "textures" || key == "texture") {
					SplitInto(value, current.textures, true);
				} else if (key == "landtextures" || key == "landtexture") {
					std::vector<std::string> items;
					SplitInto(value, items, false);
					for (const auto& item : items) {
						uint32_t form = 0;
						if (ParseFormID(item, form)) {
							current.forms.push_back(form);
						} else {
							logger::warn("Surfaces: [{}] in {} - '{}' is not a form id (hexadecimal, "
										 "like 000AB72C or 0x000AB72C)",
								current.name, a_source, item);
						}
					}
				} else if (key == "type") {
					const auto type = TypeFromName(value);
					if (type == Type::kCount) {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a surface type; "
									 "expected one of snow grass dirt mud sand ash gravel "
									 "stone. Leaving it out is fine and usually right.",
							current.name, a_source, value);
					} else {
						current.type = type;
					}
				} else {
					float parsed = 0.0f;
					if (!Settings::ParseFloat(value, parsed)) {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a number for {}",
							current.name, a_source, value, key);
						continue;
					}

					const auto clamp = [&](size_t a_field) {
						return ClampedOverride(parsed, a_field, current.name, a_source);
					};

					auto& o = current.overrides;
					if (key == "depth") {
						o.depth = clamp(0);
					} else if (key == "radius") {
						o.radius = clamp(1);
					} else if (key == "shoulder") {
						o.shoulder = clamp(2);
					} else if (key == "decay") {
						o.decay = clamp(3);
					} else if (key == "rim") {
						o.rim = clamp(4);
					} else if (key == "print") {
						o.print = clamp(5);
					} else if (key == "clearance") {
						o.clearance = clamp(6);
					} else {
						logger::warn("Surfaces: [{}] in {} - unknown key '{}'",
							current.name, a_source, key);
					}
				}
			}

			commit();
		}

		// The folders directly under a_root, in name order. Never throws: an unreadable or
		// vanishing folder (deleted mid-scan, a name outside the code page) is skipped.
		std::vector<std::filesystem::path> SortedFolders(const std::filesystem::path& a_root)
		{
			std::vector<std::filesystem::path> out;

			std::error_code ec;
			if (!std::filesystem::is_directory(a_root, ec)) {
				return out;
			}

			std::filesystem::directory_iterator it(a_root, ec);
			const std::filesystem::directory_iterator end;
			for (; !ec && it != end; it.increment(ec)) {
				std::error_code typeError;
				if (it->is_directory(typeError)) {
					out.push_back(it->path());
				}
			}

			std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
				return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
			});
			return out;
		}

		void BuildIndex(const std::vector<Profile>& a_profiles, size_t a_firstPatch,
			std::unordered_map<std::string, int>& a_byTexture,
			std::unordered_map<uint32_t, int>&    a_byForm)
		{
			a_byTexture.clear();
			a_byForm.clear();

			for (size_t i = 0; i < a_profiles.size(); ++i) {
				const auto& profile = a_profiles[i];

				for (const auto& texture : profile.textures) {
					const auto existing = a_byTexture.find(texture);
					if (existing != a_byTexture.end()) {

						logger::warn("Surfaces: '{}' is claimed by both [{}] in {} and "
									 "[{}] in {} - the {} wins",
							texture, a_profiles[existing->second].name,
							a_profiles[existing->second].source, profile.name,
							profile.source,
							i >= a_firstPatch ? "patch" : "later folder");
					}
					a_byTexture[texture] = static_cast<int>(i);
				}

				for (const auto form : profile.forms) {
					a_byForm[form] = static_cast<int>(i);
				}
			}
		}

		void ApplyStampDefaults(const StampDefaults& a_defaults)
		{
			int        n = 0;
			const auto set = [&n](const char* a_name, const Slot& a_from, float& a_to) {
				if (!a_from.value) {
					return;
				}
				a_to = *a_from.value;
				++n;
				logger::info("Surfaces:   {} = {} (from {})", a_name, a_to,
					a_from.source);
			};
			set("SnowStampReposeRate", a_defaults.snowRepose, Settings::snowStampReposeRate);
			set("SnowStampRimSpan", a_defaults.snowSpan, Settings::snowStampRimSpan);
			set("SnowStampRimNoise", a_defaults.snowNoise, Settings::snowStampRimNoise);
			set("SnowStampRimLean", a_defaults.snowLean, Settings::snowStampRimLean);
			set("SnowStampChurn", a_defaults.snowChurn, Settings::snowStampChurn);
			set("StampReposeRate", a_defaults.repose, Settings::stampReposeRate);
			set("StampRimSpan", a_defaults.span, Settings::stampRimSpan);
			set("StampRimNoise", a_defaults.noise, Settings::stampRimNoise);
			set("StampRimLean", a_defaults.lean, Settings::stampRimLean);
			set("StampChurn", a_defaults.churn, Settings::stampChurn);
			if (n > 0) {
				logger::info("Surfaces: {} stamp default(s) applied from [StampDefaults] "
							 "- these OVERRIDE the main INI; last file to set one wins",
					n);
			}
		}

		void LogProfiles(size_t a_firstPatch, size_t a_modules, size_t a_patches)
		{
			if (g_profiles.empty()) {
				logger::info("Surfaces: no per-texture profiles found under {} - running on "
							 "the main INI's response table and the game's material ids",
					kRoot);
				return;
			}

			logger::info("Surfaces: {} profile{} covering {} texture{} and {} land texture{}, "
						 "from {} module{} and {} patch{} under {}",
				g_profiles.size(), g_profiles.size() == 1 ? "" : "s",
				g_byTexture.size(), g_byTexture.size() == 1 ? "" : "s",
				g_byForm.size(), g_byForm.size() == 1 ? "" : "s",
				a_modules, a_modules == 1 ? "" : "s",
				a_patches, a_patches == 1 ? "" : "es",
				kRoot);

			for (size_t i = 0; i < g_profiles.size(); ++i) {
				const auto& profile = g_profiles[i];
				const auto& o = profile.overrides;

				const auto show = [](const std::optional<float>& a_value) {
					return a_value ? std::format("{:.2f}", *a_value) : std::string{ "-" };
				};

				std::string names;
				for (const auto& form : profile.forms) {
					names += std::format(" ltex:{:08X}", form);
				}
				for (const auto& texture : profile.textures) {
					names += " " + texture;
				}

				logger::info("  [{}] {:<20} type={:<7} depth={} radius={} shoulder={} "
							 "decay={} rim={} print={} clearance={} |{}{}",
					profile.source, profile.name,
					profile.type ? Name(*profile.type) : "(auto)",
					show(o.depth), show(o.radius), show(o.shoulder), show(o.decay),
					show(o.rim), show(o.print), show(o.clearance), names,
					i >= a_firstPatch ? "  (patch)" : "");
			}

			logger::info("  a dash means the profile left that one alone, so the main INI's "
						 "Response row for the surface decides it");
			logger::info("  decay multiplies how long a mark lasts (2 = twice as long); profiles "
						 "written for older builds, where it scaled the per-second rate, need "
						 "new values");
		}

		void LoadProfilesUnguarded()
		{
			std::vector<Profile> profiles;
			StampDefaults        stampDefaults{};

			const std::filesystem::path root{ kRoot };

			size_t modules = 0;
			for (const auto& folder : SortedFolders(root)) {
				if (SameName(folder.filename(), kPatches)) {
					continue;
				}
				const auto      file = folder / kFile;
				std::error_code ec;
				if (std::filesystem::is_regular_file(file, ec)) {
					ParseFile(file, Utf8(folder.filename()), profiles, stampDefaults);
					++modules;
				}
			}

			const auto firstPatch = profiles.size();

			size_t patches = 0;
			for (const auto& folder : SortedFolders(root / kPatches)) {
				const auto      file = folder / kFile;
				std::error_code ec;
				if (std::filesystem::is_regular_file(file, ec)) {
					ParseFile(file, "Patches/" + Utf8(folder.filename()), profiles, stampDefaults);
					++patches;
				}
			}

			std::unordered_map<std::string, int> byTexture;
			std::unordered_map<uint32_t, int>    byForm;
			BuildIndex(profiles, firstPatch, byTexture, byForm);

			{
				const std::unique_lock lock(g_profilesLock);
				g_profiles = std::move(profiles);
				g_byTexture = std::move(byTexture);
				g_byForm = std::move(byForm);
			}
			ClearGroundCache();

			ApplyStampDefaults(stampDefaults);

			LogProfiles(firstPatch, modules, patches);
		}
	}

	void LoadProfiles()
	{
		try {
			LoadProfilesUnguarded();
		} catch (const std::exception& e) {
			logger::error("Surfaces: reading the profiles under {} failed ({}); the previous "
						  "profiles stay in use",
				kRoot, e.what());
		}
	}

	void ReleaseProfiles()
	{
		{
			const std::unique_lock lock(g_profilesLock);
			g_profiles.clear();
			g_byTexture.clear();
			g_byForm.clear();
		}
		ClearGroundCache();
	}

	void ClearGroundCache()
	{
		std::lock_guard lock(g_cacheLock);
		g_cache.clear();
	}

	size_t ProfileCount()
	{
		const std::shared_lock lock(g_profilesLock);
		return g_profiles.size();
	}

	// Touches only the file system, so the settings watcher calls it from its own thread.
	std::vector<ProfileFileState> ProfilesRevision()
	{
		std::vector<ProfileFileState> revision;

		const auto note = [&revision](const std::filesystem::path& a_path) {
			std::error_code ec;
			ProfileFileState state{ a_path };
			state.modified = std::filesystem::last_write_time(a_path, ec);
			state.exists = !ec;
			if (ec) { state.modified = {}; }
			const auto size = std::filesystem::file_size(a_path, ec);
			if (!ec) { state.size = size; }
			revision.push_back(std::move(state));
		};

		const std::filesystem::path root{ kRoot };
		note(root);

		for (const auto& folder : SortedFolders(root)) {
			note(folder);
			note(folder / kFile);
		}

		const auto patches = root / kPatches;
		note(patches);
		for (const auto& folder : SortedFolders(patches)) {
			note(folder);
			note(folder / kFile);
		}

		return revision;
	}

	const char* ProfileName(int a_index)
	{
		const std::shared_lock lock(g_profilesLock);
		if (a_index < 0 || static_cast<size_t>(a_index) >= g_profiles.size()) {
			return nullptr;
		}
		return g_profiles[static_cast<size_t>(a_index)].name.c_str();
	}

	namespace
	{
		// WinterMode turns repainted vanilla ground into snow: -1 each texture that looks like
		// snow (WinterLook), 1 everything but stone (and so roads), 2 everything.
		Type WithWinterMode(Type a_type, std::string_view a_path)
		{
			if (Settings::winterMode < 0) {
				return WinterLook::LooksSnowy(a_path) ? Type::kSnow : a_type;
			}
			if (Settings::winterMode >= 2 ||
				(Settings::winterMode == 1 && a_type != Type::kStone)) {
				return Type::kSnow;
			}
			return a_type;
		}

		// The worldspace's default land texture, which no LTEX names: snow under WinterMode
		// 1 and 2, and under -1 where most of the ground drawn lately looks like snow.
		bool DefaultGroundIsSnow()
		{
			return Settings::winterMode < 0 ? WinterLook::MostlySnowy() : Settings::winterMode > 0;
		}

		// Caller holds g_profilesLock.
		const Profile* ProfileFor(const RE::TESLandTexture* a_landTexture, const std::string& a_key,
			int& a_index)
		{
			a_index = -1;
			if (a_landTexture) {
				if (const auto byForm = g_byForm.find(a_landTexture->GetFormID()); byForm != g_byForm.end()) {
					a_index = byForm->second;
				}
			}
			if (a_index < 0 && !a_key.empty()) {
				if (const auto byTexture = g_byTexture.find(a_key); byTexture != g_byTexture.end()) {
					a_index = byTexture->second;
				}
			}
			return a_index >= 0 ? &g_profiles[static_cast<size_t>(a_index)] : nullptr;
		}

		// Caller holds g_profilesLock (for a_profile). a_own is the ground's type before
		// WinterMode turned it into snow.
		Type TypeFrom(const Profile* a_profile, const RE::TESLandTexture* a_landTexture,
			std::string_view a_path, Type& a_own)
		{
			if (a_profile && a_profile->type) {
				a_own = *a_profile->type;
				return *a_profile->type;
			}

			// A profile that does not name its type is never snow by texture name alone (a
			// "snowyroad" profile tunes a road); a material type that says snow still is.
			const bool profiled = a_profile != nullptr;

			const auto fromMaterial = a_landTexture ?
			                              FromMaterialType(GameLayouts::LandMaterialType(a_landTexture)) :
			                              Type::kUnknown;
			const auto type = fromMaterial != Type::kUnknown ? fromMaterial :
			                                                   ClassifyTexturePath(a_path, profiled);

			a_own = type;
			return WithWinterMode(type, a_path);
		}

		Ground GroundFor(const RE::TESLandTexture* a_landTexture, const RE::NiPoint3& a_position)
		{
			Ground ground{};
			ground.response = ResponseFor(Type::kUnknown);

			if (!a_landTexture) {
				// The worldspace's default land texture: under WinterMode it is snow too.
				if (DefaultGroundIsSnow()) {
					ground.type = Type::kSnow;
					ground.response = ResponseFor(Type::kSnow);
				}
				return ground;
			}

			const uint32_t form = a_landTexture->GetFormID();

			const uint64_t cacheKey = static_cast<uint64_t>(form);
			const bool     cacheable = form != 0;

			if (cacheable) {
				std::lock_guard lock(g_cacheLock);
				const auto      found = g_cache.find(cacheKey);
				if (found != g_cache.end()) {
					return found->second;
				}
			}

			const char* diffuse = GameLayouts::DiffusePath(GameLayouts::LandTextureSet(a_landTexture));
			const auto  path = diffuse ? std::string_view(diffuse) : std::string_view{};
			const auto  key = TextureKey(path);

			std::string profileName;
			bool        profileTyped = false;
			{
				const std::shared_lock lock(g_profilesLock);

				const Profile* profile = ProfileFor(a_landTexture, key, ground.profile);

				Type own = Type::kUnknown;
				ground.type = TypeFrom(profile, a_landTexture, path, own);
				ground.response = ResponseFor(ground.type);
				// Stone and gravel under WinterMode's snow hold a thin layer of it only.
				if (ground.type == Type::kSnow && (own == Type::kStone || own == Type::kGravel)) {
					ground.snowSink = std::clamp(Settings::winterHardGroundDepth, 0.0f, 1.0f);
					ground.response.depthScale *= ground.snowSink;
				}

				if (profile) {
					const auto& o = profile->overrides;
					if (o.depth) {
						ground.response.depthScale = *o.depth;
					}
					if (o.radius) {
						ground.response.radiusScale = *o.radius;
					}
					if (o.shoulder) {
						ground.response.shoulder = *o.shoulder;
					}
					if (o.decay) {
						ground.response.decayScale = *o.decay;
					}
					if (o.rim) {
						ground.response.rimScale = *o.rim;
					}
					if (o.print) {
						ground.response.print = *o.print;
					}
					if (o.clearance) {
						ground.response.clearanceScale = *o.clearance;
					}

					if (Settings::logStampSurfaces) {
						profileName = profile->name;
						profileTyped = profile->type.has_value();
					}
				}
			}

			if (Settings::logStampSurfaces) {
				static std::mutex                         reportLock;
				static std::unordered_map<uint32_t, bool> reported;
				std::lock_guard                           lock(reportLock);
				if (reported.size() < 64 && reported.try_emplace(form, true).second) {
					logger::info("Ground at ({:.0f}, {:.0f}): texture '{}' ltex {:08X} -> {}{}",
						a_position.x, a_position.y, key.empty() ? "<none>" : key, form,
						ground.profile >= 0 ? std::format("profile [{}]", profileName) :
											  std::format("no profile, treated as {}", Name(ground.type)),
						ground.profile >= 0 && !profileTyped ?
							std::format(" (no Type given: its material decides, its name never "
										"makes it snow, so it is {})",
								Name(ground.type)) :
							std::string{});
				}
			}

			if (cacheable) {
				std::lock_guard lock(g_cacheLock);
				if (g_cache.size() >= kMaxCacheEntries) {
					g_cache.clear();
				}
				g_cache[cacheKey] = ground;
			}

			return ground;
		}
	}

	Ground GroundAt(const RE::NiPoint3& a_position)
	{
		if (!Settings::enableSurfaceClassification) {
			Ground ground{};
			ground.response = ResponseFor(Type::kUnknown);
			return ground;
		}

		auto* tes = globals::TES();
		float landZ = 0.0f;
		if (!tes || !tes->GetLandHeight(a_position, landZ)) {
			return GroundFor(nullptr, a_position);
		}
		return GroundAt(a_position, landZ);
	}

	Ground GroundAt(const RE::NiPoint3& a_position, float a_landZ)
	{
		if (!Settings::enableSurfaceClassification) {
			Ground ground{};
			ground.response = ResponseFor(Type::kUnknown);
			return ground;
		}

		// The land texture that dominates the ground at a position, found by the same query
		// the game uses for footstep sounds and impact effects.
		const RE::TESLandTexture* landTexture = nullptr;
		if (auto* tes = globals::TES()) {
			RE::NiPoint3 sample = a_position;
			sample.z = a_landZ + 2.0f;
			landTexture = tes->GetLandTexture(sample);
		}
		return GroundFor(landTexture, a_position);
	}

	Type TypeFor(const RE::TESLandTexture* a_landTexture, std::string_view a_texturePath)
	{
		const auto key = TextureKey(a_texturePath);

		const std::shared_lock lock(g_profilesLock);
		int                    index = -1;
		const Profile*         profile = ProfileFor(a_landTexture, key, index);
		Type                   own = Type::kUnknown;
		return TypeFrom(profile, a_landTexture, a_texturePath, own);
	}
}
