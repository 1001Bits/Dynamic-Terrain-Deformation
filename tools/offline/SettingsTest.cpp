// SPDX-License-Identifier: GPL-3.0-only
// Exercises production Settings::Load/ParseFloat/PollForChanges in a unique temporary
// working directory. No game, GPU, user INI or real surface profile is touched.
#include "OfflinePCH.h"
#include "Settings.h"
#include "SurfaceProfiles.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

// The settings watcher also asks the surface module for its revision. This test
// isolates the main INI; a fixed future profile revision must not mask INI edits.
namespace Surfaces {
std::vector<ProfileFileState> ProfilesRevision() {
	return { { "future/Surfaces.ini", std::filesystem::file_time_type::max(), 1, true } };
}
}

namespace {
const std::filesystem::path kIni = "Data/F4SE/Plugins/NMN_DeformableTerrain.ini";
void Require(bool ok, const char* what) { if (!ok) { throw std::runtime_error(what); } }
void Write(std::string_view text) {
	std::ofstream file(kIni, std::ios::binary | std::ios::trunc);
	file << text;
	Require(static_cast<bool>(file), "Write test INI");
}
void Near(float actual, float expected, const char* what) { Require(std::abs(actual - expected) < 1e-5f, what); }
}

int main(int argc, char** argv)
{
	const auto original = std::filesystem::current_path();
	const auto temporary = std::filesystem::temp_directory_path() /
		("nmn-settings-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	try {
		Require(argc == 2, "Usage: SettingsTest <shipped INI>");
		const auto shipped = std::filesystem::absolute(argv[1]);
		std::filesystem::create_directories(temporary / kIni.parent_path());
		std::filesystem::current_path(temporary);
		const Settings::Values defaults;
		Require(defaults.stampFootNodes == "foot,toe,paw,hoof", "Creature skeleton keywords default");
		float value = 42;
		for (auto text : { "", " ", "nan", "NaN", "inf", "-inf", "1e999", "1.2junk", "1,2", "0x10", "++1", "+-1" }) {
			if (Settings::ParseFloat(text, value) || value != 42) {
				throw std::runtime_error(std::format("Invalid numeric input '{}' was accepted or changed output to {}", text, value));
			}
		}
		Require(Settings::ParseFloat(" \t+1.25e1\r\n", value), "Whitespace, leading plus and exponent should parse");
		Near(value, 12.5f, "Parsed value");
		Require(Settings::ParseFloat("-0.75", value), "Negative number should parse");
		Near(value, -0.75f, "Negative value");
		std::puts("PASS numeric parsing: finite, complete, locale independent, signs and unchanged invalid output");

		std::filesystem::copy_file(shipped, kIni);
		{ std::ofstream file(kIni, std::ios::app); file << "\nEnableLiveReload=0\n"; }
		Settings::Load();
		Require(Settings::stampFootNodes == defaults.stampFootNodes, "Shipped creature skeleton keywords");
		Near(Settings::stampDepth, defaults.stampDepth, "Shipped footprint depth default");
		Near(Settings::snowRaiseHeight, defaults.snowRaiseHeight, "Shipped snow height default");
		Near(Settings::stampDecayPerSecond, defaults.stampDecayPerSecond, "Shipped decay default");
		Require(Settings::clipmapLevels == 2 && Settings::enableTessellation && Settings::useClipmap, "Shipped field defaults");
		Require(Settings::snowSkipUnderwater && defaults.snowSkipUnderwater, "Shipped underwater exclusion default");
		Near(Settings::shelterTimeBudget, defaults.shelterTimeBudget, "Shipped shelter time budget");
		Near(Settings::shelterFloorTolerance, defaults.shelterFloorTolerance, "Shipped floor tolerance");
		Require(Settings::snowCoverLowMeshes && defaults.snowCoverLowMeshes, "Shipped low meshes buried under snow");
		Near(Settings::snowCoverMeshHeight, defaults.snowCoverMeshHeight, "Shipped snow cover mesh height");
		Require(!Settings::snowOnObjects && !defaults.snowOnObjects, "Shipped snow on objects off");
		Near(Settings::snowOnObjectsStrength, defaults.snowOnObjectsStrength, "Shipped snow on objects strength");
		Near(Settings::snowOnObjectsMinUp, defaults.snowOnObjectsMinUp, "Shipped snow on objects slope limit");
		Near(Settings::snowOnObjectsDistance, defaults.snowOnObjectsDistance, "Shipped snow on objects distance");
		Require(Settings::snowOnObjectsDebug == 0 && defaults.snowOnObjectsDebug == 0, "Shipped snow on objects debug colours off");
		Near(Settings::stampSyntheticStepLength, defaults.stampSyntheticStepLength, "Shipped synthetic step length");
		Near(Settings::stampSyntheticStepPerSpeed, defaults.stampSyntheticStepPerSpeed, "Shipped synthetic step growth");
		Near(Settings::stampSyntheticStepWidth, defaults.stampSyntheticStepWidth, "Shipped synthetic step width");
		Require(!Settings::snowTracks && !defaults.snowTracks, "Shipped snow leaves footprints, not tracks");
		Require(Settings::meltShowsGround && defaults.meltShowsGround, "Shipped melted snow shows the ground");
		Require(!Settings::enableThrusterBurn && !defaults.enableThrusterBurn, "Shipped jets do not burn the ground");
		Require(Settings::winterMode == -1 && defaults.winterMode == -1, "Shipped winter ground tells snow by its looks");
		Near(Settings::stampSyntheticRevealDistance, defaults.stampSyntheticRevealDistance, "Shipped reveal distance");
		Near(Settings::stampSyntheticRevealSeconds, defaults.stampSyntheticRevealSeconds, "Shipped reveal seconds");
		Near(Settings::objectStampMaxDepth, defaults.objectStampMaxDepth, "Shipped object dent cap");
		Near(Settings::objectRimMargin, defaults.objectRimMargin, "Shipped object rim margin");
		Require(Settings::enableBloodDecals && defaults.enableBloodDecals, "Shipped ground decals on");
		Near(Settings::bloodDecalDepthMargin, defaults.bloodDecalDepthMargin, "Shipped ground decal margin");
		Near(Settings::bloodDecalGroundTolerance, defaults.bloodDecalGroundTolerance, "Shipped ground decal tolerance");
		Require(Settings::maxBloodDecalsPerFrame == defaults.maxBloodDecalsPerFrame, "Shipped ground decal budget");
		Require(Settings::indoorPrints && defaults.indoorPrints, "Shipped floor prints on");
		Require(Settings::indoorPrintSteps == defaults.indoorPrintSteps, "Shipped floor print steps");
		Near(Settings::indoorPrintFadeSeconds, defaults.indoorPrintFadeSeconds, "Shipped floor print fade");
		Near(Settings::indoorPrintOpacity, defaults.indoorPrintOpacity, "Shipped floor print opacity");
		Require(Settings::maxIndoorPrints == defaults.maxIndoorPrints, "Shipped floor prints kept");
		Near(Settings::indoorPrintDistance, defaults.indoorPrintDistance, "Shipped floor print distance");
		Near(Settings::footprintMetres, defaults.footprintMetres, "Shipped footprint distance");
		Require(Settings::corpseMarks && defaults.corpseMarks && Settings::corpseBurial && defaults.corpseBurial,
			"Shipped corpse marks and burial on");
		Near(Settings::corpseBurialSeconds, defaults.corpseBurialSeconds, "Shipped corpse burial time");
		Near(Settings::corpseMoundHeight, defaults.corpseMoundHeight, "Shipped corpse mound height");
		Require(Settings::maxCorpseMarks == defaults.maxCorpseMarks, "Shipped corpses marked");
		Require(!Settings::logCorpses, "Shipped corpse log off");
		std::puts("PASS shipped INI: field, footprint, snow and decay defaults");

		Write("[anything]\nEnableLiveReload=0\nStampDepth=9 ; comment\nStampDepth=11 # later wins\n"
			"SnowSparkle=YES\nUseClipmap=false\nDebugFieldColor=2\nStampFootNodes= FOOT, Toe \n");
		Settings::Load();
		Near(Settings::stampDepth, 11, "Last duplicate wins across arbitrary sections");
		Require(Settings::snowSparkle && !Settings::useClipmap && Settings::debugFieldColour == 2, "Booleans and color alias");
		Require(Settings::stampFootNodes == "foot,toe", "Configured skeleton keywords normalize");
		std::puts("PASS INI syntax: comments, sections, duplicates, booleans, aliases and keyword normalization");

		Write("EnableLiveReload=0\nStampDepth=nan\nSnowRaiseHeight=1e999\nClipmapLevels=999\n"
			"TessellationMaxFactor=-1\nSnowStampRimSpan=-100\nStampFootRadius=0\n");
		Settings::Load();
		Near(Settings::stampDepth, defaults.stampDepth, "Invalid number retains default");
		Near(Settings::snowRaiseHeight, defaults.snowRaiseHeight, "Overflow retains default");
		Require(Settings::clipmapLevels == 2, "Field levels clamp");
		Near(Settings::tessellationMaxFactor, 1, "Tessellation factor clamps");
		Near(Settings::snowStampRimSpan, -1, "Snow inheritance sentinel clamps");
		Near(Settings::stampFootRadius, 0.5f, "Foot radius clamps");
		std::puts("PASS invalid values and bounds: defaults, finite values, levels, tessellation, snow and radius");

		Write("EnableLiveReload=0\nStampSyntheticStepLength=1\n"
			"StampSyntheticStepPerSpeed=9\nStampSyntheticStepWidth=-3\n"
			"StampSyntheticRevealDistance=99999\nStampSyntheticRevealSeconds=-5\n");
		Settings::Load();
		Near(Settings::stampSyntheticStepLength, 4, "Synthetic step length clamps");
		Near(Settings::stampSyntheticStepPerSpeed, 2, "Synthetic step growth clamps");
		Near(Settings::stampSyntheticStepWidth, 0, "Synthetic step width clamps");
		Near(Settings::stampSyntheticRevealDistance, 4096, "Reveal distance clamps");
		Near(Settings::stampSyntheticRevealSeconds, 0, "Reveal seconds clamp");
		std::puts("PASS synthetic steps: bounds");

		Write("EnableLiveReload=0\nObjectStampMaxDepth=-2\nObjectRimMargin=500\n");
		Settings::Load();
		Near(Settings::objectStampMaxDepth, 0, "Object dent cap clamps");
		Near(Settings::objectRimMargin, 64, "Object rim margin clamps");
		std::puts("PASS object footprint marks: dent cap and rim margin bounds");

		Write("EnableLiveReload=0\nSnowCoverLowMeshes=0\nSnowCoverMeshHeight=999\n");
		Settings::Load();
		Require(!Settings::snowCoverLowMeshes, "Burying low meshes can be switched off");
		Near(Settings::snowCoverMeshHeight, 256, "Snow cover mesh height clamps");
		Write("EnableLiveReload=0\nSnowCoverMeshHeight=-5\n");
		Settings::Load();
		Require(Settings::snowCoverLowMeshes, "A deleted switch returns to its default");
		Near(Settings::snowCoverMeshHeight, 0, "Snow cover mesh height clamps at the ground");
		std::puts("PASS snow over static meshes: switch and bounds");

		Write("EnableLiveReload=0\nSnowOnObjects=1\nSnowOnObjectsStrength=3\nSnowOnObjectsMinUp=2\nSnowOnObjectsDistance=1e9\nSnowOnObjectsDebug=7\n");
		Settings::Load();
		Require(Settings::snowOnObjects, "Snow on objects can be switched on");
		Near(Settings::snowOnObjectsStrength, 1.0f, "Snow on objects strength clamps");
		Near(Settings::snowOnObjectsMinUp, 0.99f, "Snow on objects slope limit clamps below straight up");
		Near(Settings::snowOnObjectsDistance, 2000.0f, "Snow on objects distance clamps");
		Require(Settings::snowOnObjectsDebug == 1, "Snow on objects debug colours clamp to on");
		Write("EnableLiveReload=0\nSnowOnObjectsDistance=10\nSnowOnObjectsStrength=-1\n");
		Settings::Load();
		Require(!Settings::snowOnObjects, "A deleted snow on objects switch returns to off");
		Near(Settings::snowOnObjectsDistance, 256.0f, "Snow on objects distance clamps at its least");
		Near(Settings::snowOnObjectsStrength, 0.0f, "Snow on objects strength clamps at none");
		Require(Settings::snowOnObjectsDebug == 0, "A deleted snow on objects debug switch returns to off");
		std::puts("PASS snow on objects: switch and bounds");

		Write("EnableLiveReload=0\nEnableBloodDecals=0\nBloodDecalDepthMargin=-4\nBloodDecalGroundTolerance=9999\nMaxBloodDecalsPerFrame=1e6\n");
		Settings::Load();
		Require(!Settings::enableBloodDecals, "Ground decals can be switched off");
		Near(Settings::bloodDecalDepthMargin, 0, "Ground decal margin clamps");
		Near(Settings::bloodDecalGroundTolerance, 256, "Ground decal tolerance clamps");
		Require(Settings::maxBloodDecalsPerFrame == 4096, "Ground decal budget clamps");
		std::puts("PASS ground decals: switch and bounds");

		Write("EnableLiveReload=0\nIndoorPrints=0\nIndoorPrintSteps=0\nIndoorPrintFadeSeconds=1e9\nIndoorPrintOpacity=4\nMaxIndoorPrints=2\nIndoorPrintDistance=1");
		Settings::Load();
		Require(!Settings::indoorPrints && defaults.indoorPrints, "Floor prints can be switched off");
		Require(Settings::indoorPrintSteps == 1, "Floor print steps clamp at one");
		Near(Settings::indoorPrintFadeSeconds, 7200.0f, "Floor print fade clamps");
		Near(Settings::indoorPrintOpacity, 1.0f, "Floor print opacity clamps");
		Require(Settings::maxIndoorPrints == 16, "Floor prints kept clamp at their least");
		Near(Settings::indoorPrintDistance, 256.0f, "Floor print distance clamps at its least");
		Write("EnableLiveReload=0\nIndoorPrintSteps=1e9\nMaxIndoorPrints=1e9\nIndoorPrintOpacity=-1\n");
		Settings::Load();
		Require(Settings::indoorPrints, "A deleted floor prints switch returns to on");
		Require(Settings::indoorPrintSteps == 200 && Settings::maxIndoorPrints == 4096, "Floor print steps and count clamp");
		Near(Settings::indoorPrintOpacity, 0.0f, "Floor print opacity clamps at none");
		std::puts("PASS floor prints: switch and bounds");

		Write("EnableLiveReload=0\nFootprintMetres=-5\n");
		Settings::Load();
		Near(Settings::footprintMetres, 1.0f, "Footprint distance clamps at a metre");
		Write("EnableLiveReload=0\nFootprintMetres=1e9\n");
		Settings::Load();
		Near(Settings::footprintMetres, 200.0f, "Footprint distance clamps");
		std::puts("PASS footprint distance: bounds");

		Write("EnableLiveReload=0\nCorpseMarks=0\nCorpseBurial=0\nCorpseBurialSeconds=1\nCorpseMoundHeight=-3\nMaxCorpseMarks=1e6\nLogCorpses=1\n");
		Settings::Load();
		Require(!Settings::corpseMarks && !Settings::corpseBurial, "Corpse marks and burial can be switched off");
		Near(Settings::corpseBurialSeconds, 10.0f, "Corpse burial time clamps at its least");
		Near(Settings::corpseMoundHeight, 0.0f, "Corpse mound height clamps at none");
		Require(Settings::maxCorpseMarks == 32 && Settings::logCorpses, "Corpses marked clamp; the log switch reads");
		Write("EnableLiveReload=0\nCorpseBurialSeconds=1e9\nCorpseMoundHeight=1e9\nMaxCorpseMarks=-4\n");
		Settings::Load();
		Require(Settings::corpseMarks && Settings::corpseBurial, "Deleted corpse switches return to on");
		Near(Settings::corpseBurialSeconds, 7200.0f, "Corpse burial time clamps");
		Near(Settings::corpseMoundHeight, 32.0f, "Corpse mound height clamps");
		Require(Settings::maxCorpseMarks == 0, "Corpses marked clamp at none");
		std::puts("PASS corpses: switches and bounds");

		Write("EnableLiveReload=0\nResponseDirt=2,,0.4,3,2,0.5,4\nResponseStone=-5,100,-1,100,100,100,1000\n");
		Settings::Load();
		const auto& dirt = Settings::surfaceResponse[static_cast<size_t>(Surfaces::Type::kDirt)];
		Near(dirt.depthScale, 2, "Surface depth column");
		Near(dirt.radiusScale, defaults.surfaceResponse[static_cast<size_t>(Surfaces::Type::kDirt)].radiusScale, "Empty column retains default");
		Near(dirt.decayScale, 3, "Surface lifetime column");
		const auto& stone = Settings::surfaceResponse[static_cast<size_t>(Surfaces::Type::kStone)];
		Near(stone.depthScale, 0, "Hard-surface depth clamps to zero");
		Near(stone.radiusScale, 8, "Surface radius cap");
		Near(stone.decayScale, 20, "Surface lifetime cap");
		Near(stone.clearanceScale, 256, "Surface clearance cap");
		std::puts("PASS surface response rows: partial columns, lifetime scales and hard-surface bounds");

		Write("EnableLiveReload=0\nStampDepth=20\nSnowStampRimSpan=-1\nSnowSparkle=0\nUseClipmap=0\nSnowSkipUnderwater=0\n");
		Settings::Load();
		Require(!Settings::snowSkipUnderwater, "Underwater exclusion can be switched off");
		Write("EnableLiveReload=0\n");
		Settings::Load();
		Near(Settings::stampDepth, defaults.stampDepth, "Deleted depth returns to default");
		Near(Settings::snowStampRimSpan, defaults.snowStampRimSpan, "Deleted snow override returns to its own default");
		Require(Settings::snowSparkle == defaults.snowSparkle && Settings::useClipmap == defaults.useClipmap &&
				Settings::snowSkipUnderwater == defaults.snowSkipUnderwater,
			"Deleted toggles restore defaults");
		std::filesystem::remove(kIni);
		Settings::Load();
		Near(Settings::stampDepth, defaults.stampDepth, "Deleted INI restores default");
		std::puts("PASS reload defaults: deleted lines, snow inheritance, toggles and deleted file");

		// The MCM menu's choices: typed keys in Data/MCM/Settings, read after the INI.
		const std::filesystem::path menu = "Data/MCM/Settings/NMN_DeformableTerrain.ini";
		std::filesystem::create_directories(menu.parent_path());
		Write("EnableLiveReload=0\nStampDepth=9\nWinterMode=0\nSnowSparkleSize=3\n");
		{
			std::ofstream file(menu, std::ios::binary | std::ios::trunc);
			file << "[Footprints]\nbSnowTracks=1\nfStampDepth=4.5\n[General]\niWinterMode=2\n[Snow]\nbSnowOnObjects=1\n";
		}
		Settings::Load();
		Require(Settings::snowTracks, "An MCM toggle applies");
		Near(Settings::stampDepth, 4.5f, "An MCM slider wins over the INI's line");
		Require(Settings::snowOnObjects, "MCM toggles apply");
		Require(Settings::winterMode == 0, "An old menu's Winter ground choice does not override the INI");
		Near(Settings::snowSparkleSize, 3.0f, "INI lines the menu does not set still apply");
		std::filesystem::remove(menu);
		Settings::Load();
		Require(!Settings::snowTracks && Settings::winterMode == 0 && !Settings::snowOnObjects, "Without the MCM file the INI rules");
		Near(Settings::stampDepth, 9.0f, "Without the MCM file the INI's depth applies");
		std::puts("PASS MCM menu: typed keys override the INI, unset ones and a missing file leave it");

		Write("EnableLiveReload=1\nLiveReloadInterval=0.05\nStampDepth=7\n");
		Settings::Load();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		Write("EnableLiveReload=1\nLiveReloadInterval=0.05\nStampDepth=13\n");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		bool reloaded = false;
		while (std::chrono::steady_clock::now() < deadline && !reloaded) {
			reloaded = Settings::PollForChanges(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Require(reloaded, "Background watcher must publish an INI change");
		Near(Settings::stampDepth, 13, "Watcher reload applied changed setting");
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		{
			std::ofstream file(menu, std::ios::binary | std::ios::trunc);
			file << "[Footprints]\nbSnowTracks=1\n";
		}
		const auto menuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		bool       menuReloaded = false;
		while (std::chrono::steady_clock::now() < menuDeadline && !menuReloaded) {
			menuReloaded = Settings::PollForChanges(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Require(menuReloaded && Settings::snowTracks, "A change made in the MCM menu reloads live");
		std::puts("PASS background watcher: INI and MCM menu edits reload even with a newer profile timestamp");
		std::filesystem::current_path(original);
		std::filesystem::remove_all(temporary);
		std::puts("ALL SETTINGS TESTS PASS");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		std::filesystem::current_path(original);
		std::filesystem::remove_all(temporary);
		return 1;
	}
}
