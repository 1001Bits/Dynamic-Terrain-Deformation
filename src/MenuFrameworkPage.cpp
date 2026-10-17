// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "MenuFrameworkPage.h"

#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace MenuFrameworkPage
{
	namespace
	{
		// The framework's exports this page uses (DCCStudios/F4SEMenuFramework, the same ones
		// FO4 Cloud Shadows binds): a section, its open/close events and the cimgui widgets,
		// bound by name. Every widget draws in the framework's ImGui context; no ImGui object
		// crosses the DLL boundary.
		enum class Event : int
		{
			kNone = 0,
			kOpen = 1,
			kClose = 2,
			kBeforeRender = 3,
			kAfterRender = 4,
		};
		using RenderCallback = void(__stdcall*)();
		using EventCallback = void(__stdcall*)(Event);

		struct Api
		{
			void (*addSection)(const char*, RenderCallback){};
			std::int64_t (*registerEvent)(EventCallback){};
			bool (*checkbox)(const char*, bool*){};
			bool (*sliderFloat)(const char*, float*, float, float, const char*, int){};
			bool (*combo)(const char*, int*, const char* const[], int, int){};
			bool (*hovered)(int){};
			void (*tooltip)(const char*, ...){};
			void (*disabled)(const char*, ...){};
			// Optional: without it a slider is saved when the menu closes.
			bool (*deactivatedAfterEdit)(){};

			bool Load(HMODULE a_module)
			{
				const auto bind = [a_module](auto& a_function, const char* a_name) {
					a_function = reinterpret_cast<std::remove_reference_t<decltype(a_function)>>(
						GetProcAddress(a_module, a_name));
					return a_function != nullptr;
				};
				(void)bind(registerEvent, "RegisterEvent");
				(void)bind(deactivatedAfterEdit, "igIsItemDeactivatedAfterEdit");
				return bind(addSection, "AddSectionItem") && bind(checkbox, "igCheckbox") &&
				       bind(sliderFloat, "igSliderFloat") && bind(combo, "igCombo_Str_arr") &&
				       bind(hovered, "igIsItemHovered") && bind(tooltip, "igSetTooltip") &&
				       bind(disabled, "igTextDisabled");
			}
		};

		enum class Kind
		{
			kSwitch,
			kSlider,
			kStepper,
		};

		// One control, as MCM's config.json has it: its id (MCM key:section), text, help, kind and
		// slider range, and the live setting it shows. tools/offline/test_mcm_config.py checks
		// these rows against config.json, so keep one row per line.
		struct Control
		{
			const char* id;
			const char* text;
			const char* help;
			Kind        kind;
			float       min;
			float       max;
			float       step;
			bool*       flag;
			float*      value;
		};

		// clang-format off
		const Control kControls[]{
			{ "bEnableTessellation:General", "Deformable terrain", "Off draws the landscape as the game does, without marks.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableTessellation, nullptr },
			{ "iSnowTracks:Footprints", "Marks in snow", "Separate footprints or continuous tracks in deep snow.", Kind::kStepper, 0.0f, 0.0f, 0.0f, &Settings::snowTracks, nullptr },
			{ "fStampDepth:Footprints", "Footprint depth", "How deep footprints press into soft ground.", Kind::kSlider, 1.0f, 16.0f, 0.5f, nullptr, &Settings::stampDepth },
			{ "bEnableWeather:Footprints", "Weather softens the ground", "Rain and snow make marks deeper; clear skies firm the ground.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableWeather, nullptr },
			{ "fFootprintMetres:Footprints", "Footprint distance (metres)", "How far from you footprints are made and shown.", Kind::kSlider, 5.0f, 50.0f, 1.0f, nullptr, &Settings::footprintMetres },
			{ "bIndoorPrints:Footprints", "Shoe prints on floors", "Boots carry snow, mud or dust onto floors as fading prints.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::indoorPrints, nullptr },
			{ "fIndoorPrintOpacity:Footprints", "Shoe print strength", "How strongly shoe prints show on floors.", Kind::kSlider, 0.1f, 1.0f, 0.05f, nullptr, &Settings::indoorPrintOpacity },
			{ "bEnableSnowRaise:Snow", "Deep snow (when using a snow mod)", "Snow lies as a deep layer you wade through.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableSnowRaise, nullptr },
			{ "fSnowRaiseHeight:Snow", "Snow depth", "How deep the snow lies.", Kind::kSlider, 10.0f, 60.0f, 1.0f, nullptr, &Settings::snowRaiseHeight },
			{ "bSnowCoverLowMeshes:Snow", "Snow covers roads and paths", "Roads and paths lie under the snow and take footprints.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::snowCoverLowMeshes, nullptr },
			{ "fWinterHardGroundDepth:Snow", "Snow over stone and gravel", "How deep marks go in snow over stone and gravel.", Kind::kSlider, 0.0f, 1.0f, 0.05f, nullptr, &Settings::winterHardGroundDepth },
			{ "bSnowOnObjects:Snow", "Snow on roofs and rock tops", "Roofs, rocks and wrecks get snowy tops, at a small frame cost.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::snowOnObjects, nullptr },
			{ "fSnowOnObjectsStrength:Snow", "Snow on roofs strength", "How white the tops turn.", Kind::kSlider, 0.1f, 1.0f, 0.05f, nullptr, &Settings::snowOnObjectsStrength },
			{ "bSnowSparkle:Snow", "Kicked-up snow", "Walking and running through snow throws up small puffs.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::snowSparkle, nullptr },
			{ "fSnowSparkleRate:Snow", "Kicked-up snow amount", "How much snow your feet throw up.", Kind::kSlider, 100.0f, 4000.0f, 50.0f, nullptr, &Settings::snowSparkleRate },
			{ "fSnowSparkleLife:Snow", "Kicked-up snow time", "How long kicked-up snow hangs in the air.", Kind::kSlider, 0.3f, 4.0f, 0.1f, nullptr, &Settings::snowSparkleLife },
			{ "bEnableExplosionImpacts:World", "Explosion craters", "Explosions leave craters in soft ground and snow.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableExplosionImpacts, nullptr },
			{ "bEnableProjectileImpacts:World", "Bullet marks", "Shots leave small marks where they hit the ground.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableProjectileImpacts, nullptr },
			{ "bEnableObjectStamps:World", "Dropped objects dent the ground", "Dropped items dent soft ground and snow.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableObjectStamps, nullptr },
			{ "bEnableBloodDecals:World", "Blood follows the ground", "Blood follows dents and lies on top of the snow.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableBloodDecals, nullptr },
			{ "fBloodDecalSnowOpacity:World", "Blood on snow", "Lower values let blood soak into the snow.", Kind::kSlider, 0.2f, 1.0f, 0.05f, nullptr, &Settings::bloodDecalSnowOpacity },
			{ "bCorpseMarks:World", "Bodies press into the ground", "A body lying on soft ground or snow sinks into a hollow.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::corpseMarks, nullptr },
			{ "bCorpseBurial:World", "Snowfall buries bodies", "While it snows, a mound of snow slowly grows over bodies.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::corpseBurial, nullptr },
			{ "fCorpseBurialSeconds:World", "Time to bury a body (seconds)", "How long snowfall takes to cover a body.", Kind::kSlider, 60.0f, 1800.0f, 30.0f, nullptr, &Settings::corpseBurialSeconds },
			{ "bHeatMeltsSnow:World", "Fires melt snow", "Fires and candles melt the snow around them.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::heatMeltsSnow, nullptr },
			{ "fHeatRadiusScale:World", "Melt around fires", "How far around a fire the snow melts.", Kind::kSlider, 1.0f, 6.0f, 0.25f, nullptr, &Settings::heatRadiusScale },
			{ "bEnableScorch:World", "Burnt ground", "Fire and heat leave darkened ground that fades over time.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableScorch, nullptr },
			{ "bEnableThrusterBurn:World", "Robot jets burn the ground", "Flying robots like Codsworth burn a line along their path.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::enableThrusterBurn, nullptr },
			{ "bThrusterMeltsSnow:World", "Robot jets melt snow", "Their jet melts the snow under them.", Kind::kSwitch, 0.0f, 0.0f, 0.0f, &Settings::thrusterMeltsSnow, nullptr },
		};
		// clang-format on

		constexpr const char* kTrackOptions[]{ "Footprints", "Tracks" };

		// MCM's sections, each a page: its key in the control ids and its title.
		struct Section
		{
			const char* key;
			const char* title;
		};
		constexpr Section kSections[]{
			{ "General", "General" },
			{ "Footprints", "Footprints" },
			{ "Snow", "Snow" },
			{ "World", "Combat and the world" },
		};

		constexpr auto kMcmPath = "Data/MCM/Settings/NMN_DeformableTerrain.ini";

		Api ui;
		// The framework renders and raises its events on threads of its own.
		std::mutex g_lock;
		// What each control shows (switches as 0 or 1), read from the live settings when the
		// menu opens, and what was last written for it.
		std::vector<float> g_shown;
		std::vector<float> g_written;
		bool               g_loaded{ false };
		bool               g_saveFailed{ false };

		std::string_view KeyOf(const Control& a_control)
		{
			const std::string_view id = a_control.id;
			return id.substr(0, id.find(':'));
		}

		std::string_view SectionOf(const Control& a_control)
		{
			const std::string_view id = a_control.id;
			return id.substr(id.find(':') + 1);
		}

		float Live(const Control& a_control)
		{
			return a_control.flag ? (*a_control.flag ? 1.0f : 0.0f) : *a_control.value;
		}

		// Caller holds the lock.
		void Load()
		{
			g_shown.resize(std::size(kControls));
			for (size_t i = 0; i < std::size(kControls); ++i) {
				g_shown[i] = Live(kControls[i]);
			}
			g_written = g_shown;
			g_loaded = true;
		}

		std::string Text(float a_value, const Control& a_control)
		{
			if (a_control.kind != Kind::kSlider) {
				return a_value > 0.5f ? "1" : "0";
			}
			return std::format("{}", a_value);
		}

		// Sets one key in MCM's settings file, as MCM writes it ([Section] then key=value),
		// through a temporary file and a rename so the plugin's watcher never reads half a file.
		bool WriteKey(std::string_view a_section, std::string_view a_key, const std::string& a_value)
		{
			const auto same = [](std::string_view a_lhs, std::string_view a_rhs) {
				return a_lhs.size() == a_rhs.size() &&
				       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](char a_l, char a_r) {
						   return std::tolower(static_cast<unsigned char>(a_l)) ==
						          std::tolower(static_cast<unsigned char>(a_r));
					   });
			};
			const auto trim = [](std::string_view a_text) {
				const auto first = a_text.find_first_not_of(" \t\r");
				if (first == std::string_view::npos) {
					return std::string_view{};
				}
				return a_text.substr(first, a_text.find_last_not_of(" \t\r") - first + 1);
			};

			std::vector<std::string> lines;
			if (std::ifstream in{ kMcmPath }; in) {
				for (std::string line; std::getline(in, line);) {
					if (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					lines.push_back(std::move(line));
				}
			}

			const std::string entry = std::format("{}={}", a_key, a_value);
			bool              inSection = false;
			size_t            sectionEnd = std::string::npos;
			bool              done = false;
			for (size_t i = 0; i < lines.size() && !done; ++i) {
				const auto line = trim(lines[i]);
				if (line.starts_with('[') && line.ends_with(']')) {
					if (inSection) {
						break;
					}
					inSection = same(trim(line.substr(1, line.size() - 2)), a_section);
					sectionEnd = inSection ? i + 1 : sectionEnd;
					continue;
				}
				if (!inSection) {
					continue;
				}
				if (!line.empty() && line[0] != ';') {
					sectionEnd = i + 1;
				}
				const auto equals = line.find('=');
				if (equals != std::string_view::npos && same(trim(line.substr(0, equals)), a_key)) {
					lines[i] = entry;
					done = true;
				}
			}
			if (!done) {
				if (sectionEnd != std::string::npos) {
					lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), entry);
				} else {
					if (!lines.empty() && !lines.back().empty()) {
						lines.emplace_back();
					}
					lines.push_back(std::format("[{}]", a_section));
					lines.push_back(entry);
				}
			}

			std::error_code ec;
			const std::filesystem::path path{ kMcmPath };
			std::filesystem::create_directories(path.parent_path(), ec);
			const auto temporary = path.string() + ".menuframework.tmp";
			{
				std::ofstream out{ temporary, std::ios::binary | std::ios::trunc };
				if (!out) {
					return false;
				}
				for (const auto& line : lines) {
					out << line << "\r\n";
				}
				if (!out.flush()) {
					return false;
				}
			}
			std::filesystem::rename(temporary, path, ec);
			if (ec) {
				std::filesystem::remove(temporary, ec);
				return false;
			}
			return true;
		}

		// Caller holds the lock.
		void Save(size_t a_index)
		{
			if (g_shown[a_index] == g_written[a_index]) {
				return;
			}
			const auto& control = kControls[a_index];
			if (WriteKey(SectionOf(control), KeyOf(control), Text(g_shown[a_index], control))) {
				g_written[a_index] = g_shown[a_index];
				g_saveFailed = false;
				logger::info("Menu Framework: {} = {}", KeyOf(control), Text(g_shown[a_index], control));
			} else {
				g_saveFailed = true;
			}
		}

		const char* FormatFor(float a_step)
		{
			return a_step >= 1.0f ? "%.0f" : a_step >= 0.1f ? "%.1f" : "%.2f";
		}

		void Help(const char* a_help)
		{
			if (ui.hovered(0)) {
				ui.tooltip("%s", a_help);
			}
		}

		// Caller holds the lock.
		void Draw(size_t a_index)
		{
			const auto& control = kControls[a_index];
			float&      shown = g_shown[a_index];
			switch (control.kind) {
			case Kind::kSwitch:
				{
					bool on = shown > 0.5f;
					if (ui.checkbox(control.text, &on)) {
						shown = on ? 1.0f : 0.0f;
						Save(a_index);
					}
					Help(control.help);
					break;
				}
			case Kind::kStepper:
				{
					int choice = shown > 0.5f ? 1 : 0;
					if (ui.combo(control.text, &choice, kTrackOptions, static_cast<int>(std::size(kTrackOptions)), -1)) {
						shown = choice != 0 ? 1.0f : 0.0f;
						Save(a_index);
					}
					Help(control.help);
					break;
				}
			case Kind::kSlider:
				{
					float value = shown;
					if (ui.sliderFloat(control.text, &value, control.min, control.max, FormatFor(control.step), 0)) {
						// The slider's own steps, as MCM's; a typed value is kept in range too.
						const float steps = std::round((value - control.min) / control.step);
						shown = std::clamp(control.min + steps * control.step, control.min, control.max);
						// Without the framework's release test, every move is written.
						if (!ui.deactivatedAfterEdit) {
							Save(a_index);
						}
					}
					Help(control.help);
					if (ui.deactivatedAfterEdit && ui.deactivatedAfterEdit()) {
						Save(a_index);
					}
					break;
				}
			}
		}

		void Render(std::string_view a_section)
		{
			const std::scoped_lock lock(g_lock);
			if (!g_loaded) {
				Load();
			}
			for (size_t i = 0; i < std::size(kControls); ++i) {
				if (SectionOf(kControls[i]) == a_section) {
					Draw(i);
				}
			}
			ui.disabled("%s", "Changes apply at once. MCM shows them after a restart.");
			if (g_saveFailed) {
				ui.disabled("%s", "A change could not be saved to Data/MCM/Settings.");
			}
		}

		template <size_t N>
		void __stdcall RenderSection()
		{
			Render(kSections[N].key);
		}

		void __stdcall OnEvent(Event a_event)
		{
			if (a_event != Event::kOpen && a_event != Event::kClose) {
				return;
			}
			const std::scoped_lock lock(g_lock);
			if (a_event == Event::kOpen) {
				// Read again on opening: MCM or the INI may have changed meanwhile.
				g_loaded = false;
				return;
			}
			// A slider still held when the menu closed.
			for (size_t i = 0; g_loaded && i < g_shown.size(); ++i) {
				Save(i);
			}
		}
	}

	void Install()
	{
		static bool attempted = false;
		if (attempted || REL::Module::IsVR()) {
			return;
		}
		attempted = true;

		const auto framework = GetModuleHandleW(L"F4SEMenuFramework.dll");
		if (!framework) {
			return;
		}
		// With MCM installed the framework shows MCM's own menu (its MCM translation): a page
		// of ours beside it would list every option twice.
		if (GetModuleHandleW(L"mcm.dll")) {
			logger::info("Menu Framework: MCM is installed, so its menu is the one shown there");
			return;
		}
		Api candidate;
		if (!candidate.Load(framework)) {
			logger::warn("Menu Framework: found, but without the functions this page needs; use MCM instead");
			return;
		}
		ui = candidate;

		const RenderCallback pages[]{ RenderSection<0>, RenderSection<1>, RenderSection<2>, RenderSection<3> };
		static_assert(std::size(pages) == std::size(kSections));
		// The framework may keep the path it is given: these live as long as the plugin.
		static std::vector<std::string> paths;
		for (size_t i = 0; i < std::size(kSections); ++i) {
			paths.push_back(std::format("Dynamic Terrain Deformation/{}", kSections[i].title));
		}
		for (size_t i = 0; i < std::size(kSections); ++i) {
			ui.addSection(paths[i].c_str(), pages[i]);
		}
		const auto listener = ui.registerEvent ? ui.registerEvent(OnEvent) : -1;
		logger::info("Menu Framework: settings pages registered{}",
			listener > 0 ? "" : " (no menu events: the page reads the settings once)");
	}
}
