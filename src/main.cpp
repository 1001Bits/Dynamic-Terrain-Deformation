// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.
//
// Deformable Terrain for Fallout 4 (1.10.163 / 1.10.984 / 1.11.240 / VR 1.2.72), ported from
// the Skyrim SKSE plugin by maglarnet (NearMidnightNow).

#include "PCH.h"
#include "Plugin.h"

#include "Clipmap.h"
#include "Globals.h"
#include "HeatSources.h"
#include "Hooks.h"
#include "Impacts.h"
#include "MenuFrameworkPage.h"
#include "Settings.h"
#include "ShaderRegistry.h"
#include "SnowCoverage.h"
#include "StampShapes.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"

using namespace std::literals;

namespace
{
	// Only the builds every address and field offset was verified on (docs/FO4_PORT_NOTES.md).
	// The address library cannot be trusted on other builds: CommonLibF4 resolves a missing
	// id to its neighbour's address instead of failing.
	constexpr REL::Version kRuntime_1_11_240{ 1, 11, 240, 0 };
	constexpr REL::Version kSupportedRuntimes[]{
		F4SE::RUNTIME_1_10_163,
		F4SE::RUNTIME_1_10_984,
		kRuntime_1_11_240,
		F4SE::RUNTIME_VR_1_2_72,
	};

	// The forms the plugin indexes or caches by pointer are freed by a data reload (the
	// main menu's Mods screen) and made again: drop everything keyed by them before, and
	// index the new ones after.
	void OnGameDataUnloading()
	{
		Surfaces::ClearLandTextureIndex();
		Surfaces::Reset();
		HeatSources::Reset();
		globals::ResetFormCaches();
	}

	void OnGameDataReady()
	{
		// kGameDataReady arrives again after every data reload; only the indexes of the
		// forms are rebuilt then, everything else is one-time setup.
		static bool once = false;
		if (once) {
			Surfaces::BuildLandTextureIndex();
			Surfaces::Reset();
			HeatSources::Reset();
			globals::ResetFormCaches();
			Hooks::RequestWorldReset();
			return;
		}
		once = true;

		Settings::Load();
		Surfaces::LoadProfiles();
		Surfaces::BuildLandTextureIndex();
		Hooks::InstallLandscapeTextureRecorder();

		if (!globals::Initialize()) {
			logger::error("D3D device/context unavailable - renderer work disabled");
			return;
		}

		logger::info("D3D ready: device={} context={}",
			static_cast<const void*>(globals::d3d::device),
			static_cast<const void*>(globals::d3d::context));

		if (!ShaderRegistry::Install(globals::d3d::device)) {
			logger::error("ShaderRegistry: CreateVertexShader not hooked - draws under a "
						  "replaced vertex shader cannot be resolved");
		}
		logger::info("ShaderRegistry: {} vertex shader(s) recorded so far, {:.1f} KB of "
					 "output signatures",
			ShaderRegistry::Count(),
			static_cast<double>(ShaderRegistry::Bytes()) / 1024.0);
		if (ShaderRegistry::Count() == 0) {
			logger::warn("ShaderRegistry: no vertex shaders were seen being created - the "
						 "device was made before this plugin could watch it, so the "
						 "landscape cannot be tessellated this session");
		}

		if (Settings::useClipmap && !Clipmap::Initialize()) {
			logger::error("Clipmap unavailable - displacement will fall back to the wave");
		}

		if (Settings::enableSnowRaise && !SnowCoverage::Initialize()) {
			logger::error("SnowCoverage unavailable - the snow raise will lift nothing");
		}

		if (Settings::enableStampShapes) {
			StampShapes::Initialize();
			StampShapes::RequestAnalysis();
		}

		Hooks::Install();
		Impacts::Install();
	}

	void F4SEAPI MessageHandler(F4SE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case F4SE::MessagingInterface::kPostPostLoad:
			// Every plugin is loaded: F4SE Menu Framework, if present, takes the pages now.
			MenuFrameworkPage::Install();
			break;
		case F4SE::MessagingInterface::kGameDataReady:
			// Sent with false before a data reload and true once the forms are in. F4SE
			// passes the bool as the data pointer's value, not through it (Buffout 4 reads
			// it the same way); dereferencing it would read address 1.
			if (!a_message->data) {
				OnGameDataUnloading();
				break;
			}
			OnGameDataReady();
			break;
		case F4SE::MessagingInterface::kPreLoadGame:
		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			// Marks, snow and shelter maps and tracked actors belong to the timeline being
			// left; the next world frame drops them.
			Impacts::Reset();
			Hooks::RequestWorldReset();
			break;
		default:
			break;
		}
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	if (!a_f4se || !a_info) {
		return false;
	}

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Plugin::NAME.data();
	a_info->version = Plugin::VERSION[0];

	if (a_f4se->IsEditor()) {
		logger::critical("Loading into editor, aborting.");
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	if (!a_f4se) {
		return false;
	}

	F4SE::Init(a_f4se);

	logger::info("{} v{}.{}.{} [{} {}] is loading", Plugin::NAME, Plugin::VERSION[0],
		Plugin::VERSION[1], Plugin::VERSION[2], __DATE__, __TIME__);

	const auto version = REL::Module::get().version();
	logger::info("Detected runtime: Fallout 4{} (v{}).", REL::Module::IsVR() ? " VR" : "",
		version.string());

	if (std::ranges::find(kSupportedRuntimes, version) == std::end(kSupportedRuntimes)) {
		logger::critical("Runtime version {} is not supported (1.10.163, 1.10.984, 1.11.240 and "
						 "VR 1.2.72 are), aborting.",
			version.string());
		return false;
	}

	F4SE::AllocTrampoline(1 << 9);

	// The listener first: a failure here unloads the plugin, which must not leave a patched
	// import pointing into it.
	const auto messaging = F4SE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(MessageHandler)) {
		logger::critical("Failed to get message interface, aborting.");
		return false;
	}

	// Device creation has to be watched from here: Fallout 4 compiles every shader it
	// will use while the renderer starts, long before the game data is ready.
	if (!ShaderRegistry::InstallEarly()) {
		logger::warn("ShaderRegistry: could not intercept device creation - shaders made "
					 "before the device is handed to us will be invisible");
	}

	logger::info("{} loaded", Plugin::NAME);
	return true;
}

// ── NG / 1.11 version declaration ───────────────────────────────────────────
// F4SE on 1.11.x only treats plugins built for its own address-library generation as
// version independent; everything else must list the exact runtime, which is what we want.
F4SE_EXPORT auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};
	data.PluginName(Plugin::NAME);
	data.PluginVersion(Plugin::VERSION);
	data.AuthorName("NearMidnightNow (Fallout 4 port)");
	data.UsesAddressLibrary(true);
	data.UsesSigScanning(false);
	data.IsLayoutDependent(true);
	data.HasNoStructUse(false);
	data.CompatibleVersions({ F4SE::RUNTIME_1_10_984, kRuntime_1_11_240 });
	return data;
}();
