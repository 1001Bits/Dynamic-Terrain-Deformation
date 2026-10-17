// SPDX-License-Identifier: GPL-3.0-only
// Exercise the production profile parser/index and texture classification. Including
// the source also lets these fixtures inspect parsed overrides without a game TES.
#include "PCH.h"
#include "SurfaceProfiles.cpp"
#include "WinterLook.h"
#include <chrono>
#include <stdexcept>
#include <thread>

// CommonLib's inline runtime helpers otherwise inspect this test executable as
// Fallout4.exe. No relocation is used: supply only its inert OG type/layout flag.
namespace { std::atomic<bool> fixturesStarted{false}; }
namespace REL {
Module::Module() : _segments{}, _version{1, 10, 163, 0}, _runtime{Runtime::F4} {}
// CommonLib registers unused engine metadata during static initialization. It
// may hold inert zero offsets then; any relocation during a fixture must fail.
IDDB::IDDB() {}
std::size_t IDDB::id2offset(std::uint64_t) const {
	if (fixturesStarted.load()) throw std::runtime_error("Unexpected engine relocation");
	return 0;
}
}

// Engine queries and field-dependent response functions must never run in this test.
namespace globals { RE::TES* TES() { throw std::runtime_error("Unexpected engine query"); } }
namespace RE {
bool TES::GetLandHeight(const NiPoint3&, float&) { throw std::runtime_error("Unexpected height query"); }
TESLandTexture* TES::GetLandTexture(const NiPoint3&) { throw std::runtime_error("Unexpected texture query"); }
}
// Snow-mark fixtures turn these on with set values; anything else must not query them.
namespace { bool snowFixture = false; float fixtureBlanket = 0.0f; float fixtureBuried = 0.0f; }
namespace SnowCoverage { float At(float, float) { if (!snowFixture) throw std::runtime_error("Unexpected coverage query"); return 1.0f; } }
namespace SnowSurface { float BlanketFor(float a_snowy, float, float) { if (!snowFixture) throw std::runtime_error("Unexpected blanket query"); return fixtureBlanket * a_snowy; } }
namespace Shelter { float BuriedAt(float, float) { if (!snowFixture) throw std::runtime_error("Unexpected buried mesh query"); return fixtureBuried; } }
namespace Weather { float DepthScale() { throw std::runtime_error("Unexpected weather query"); } }

namespace {
namespace fs = std::filesystem;
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void Near(float value, float expected, const char* why) { Require(std::abs(value - expected) < 1e-5f, why); }
void Write(const fs::path& file, std::string_view body) {
	fs::create_directories(file.parent_path());
	std::ofstream out(file, std::ios::binary | std::ios::trunc); out << body;
	Require(static_cast<bool>(out), "Write profile fixture");
}
const Surfaces::Profile& Find(std::string_view path) {
	int index = -1;
	const auto* profile = Surfaces::ProfileFor(nullptr, Surfaces::TextureKey(path), index);
	Require(profile && index >= 0, "Texture must select a profile");
	return *profile;
}
}

int main() {
	std::setbuf(stdout, nullptr);
	fixturesStarted.store(true);
	const auto original = fs::current_path();
	const auto temporary = fs::temp_directory_path() /
		("nmn-profiles-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	try {
		fs::create_directories(temporary); fs::current_path(temporary);
		const fs::path ini = "Data/F4SE/Plugins/NMN_DeformableTerrain.ini";
		const fs::path root = "Data/F4SE/Plugins/NMN_DeformableTerrain";
		Write(ini, "EnableLiveReload=0\n"); Settings::Load();
		Surfaces::LoadProfiles();
		Require(Surfaces::ProfileCount() == 0, "Missing profile tree is valid");
		Require(Surfaces::TypeFor(nullptr, "textures/landscape/dirt01.dds") == Surfaces::Type::kDirt, "Unprofiled name fallback");
		Require(Surfaces::TextureKey("Textures\\Landscape\\DIRT01.DDS") == "dirt01", "Windows path key");
		std::puts("PASS missing profiles and texture key/name fallback");

		// Create in reverse order so enumeration order cannot accidentally satisfy precedence.
		Write(root / "zModule/Surfaces.ini", "[Later]\nTextures=dirt01\nType=mud\nDepth=2\n");
		Write(root / "AModule/Surfaces.ini", "[Earlier]\nTexture=Textures\\Landscape\\DIRT01.DDS\nType=sand\nDepth=3\n");
		Surfaces::LoadProfiles();
		Require(Find("dir/dirt01.dds").name == "Later", "Case-insensitive module ordering");
		Require(Surfaces::TypeFor(nullptr, "dirt01.dds") == Surfaces::Type::kMud, "Explicit profile type");
		Write(root / "Patches/aPatch/Surfaces.ini", "[Patch]\nTextures=dirt01\nType=stone\nDepth=0\n");
		Surfaces::LoadProfiles();
		Require(Find("dirt01").name == "Patch", "Patches override modules");
		Require(Surfaces::TypeFor(nullptr, "dirt01.dds") == Surfaces::Type::kStone, "Patch type takes effect");
		std::puts("PASS module order, patch precedence and explicit classification");

		Write(root / "zModule/Surfaces.ini", "[Overrides]\nTextures=custom, other\nLandTextures=0xAB72C,000AB72D,xyz,FFFFFFFFF\n"
			"Depth=-2\nRadius=100\nShoulder=100\nDecay=0\nRim=100\nPrint=-1\nClearance=1000\n"
			"[Invalid]\nTexture=invalid\nType=typo\nDepth=nan\nRadius=+-1\n[Empty]\nType=snow\n");
		Surfaces::LoadProfiles();
		const auto& o = Find("custom").overrides;
		Near(*o.depth, 0, "Depth bound"); Near(*o.radius, 8, "Radius bound");
		Near(*o.shoulder, .95f, "Shoulder bound"); Near(*o.decay, .05f, "Lifetime bound");
		Near(*o.rim, 4, "Rim bound"); Near(*o.print, 0, "Print bound"); Near(*o.clearance, 256, "Clearance bound");
		Require(Find("other").forms.size() == 2 && Surfaces::g_byForm.contains(0xAB72C) && Surfaces::g_byForm.contains(0xAB72D), "Strict hexadecimal form parsing");
		Require(!Find("invalid").type && !Find("invalid").overrides.depth && !Find("invalid").overrides.radius, "Invalid values remain unset");
		Require(Surfaces::ProfileCount() == 4, "Profile without matches is discarded");
		std::puts("PASS response bounds, finite numbers, form IDs, aliases and empty profiles");

		Write(root / "zModule/Surfaces.ini", "[Untyped]\nTextures=snowyroad\nDepth=2\n[Typed]\nTextures=typed\nType=dirt\n");
		Surfaces::LoadProfiles();
		Require(Surfaces::TypeFor(nullptr, "snowyroad.dds") != Surfaces::Type::kSnow, "Untyped road profile never becomes snow by name");
		Settings::winterMode = 1;
		Require(Surfaces::TypeFor(nullptr, "plain_dirt.dds") == Surfaces::Type::kSnow, "Winter converts ordinary dirt");
		Require(Surfaces::TypeFor(nullptr, "dirt01.dds") == Surfaces::Type::kStone, "Explicit stone profile stays stone");
		Require(Surfaces::TypeFor(nullptr, "typed.dds") == Surfaces::Type::kDirt, "Explicit type overrides winter conversion");
		Settings::winterMode = 2;
		Require(Surfaces::TypeFor(nullptr, "road.dds") == Surfaces::Type::kSnow, "Winter mode 2 includes roads");
		std::puts("PASS untyped snow suppression and winter/type precedence");

		// WinterMode -1: the ground's looks decide, once a draw has had a texture measured.
		Settings::winterMode = -1;
		Require(WinterLook::IsSnow({ 0.8f, 0.8f, 0.8f, 0.6f }) && !WinterLook::IsSnow({ 0.8f, 0.8f, 0.8f, 0.5f }),
			"A texture is snow when most of it is white");
		Require(Surfaces::TypeFor(nullptr, "whitegrass01.dds") == Surfaces::Type::kGrass, "Unmeasured ground keeps its name's type");
		const uint64_t white = WinterLook::Register("Textures\\Landscape\\WhiteGrass01.dds");
		const uint64_t green = WinterLook::Register("textures/landscape/grass02.dds");
		const uint64_t patched = WinterLook::Register("dirt01.dds");
		Require(white && white == WinterLook::KeyOf("whitegrass01.dds") && WinterLook::Waiting(), "Registered textures wait by key");
		const uint64_t drawn[4]{ white, green, patched, 0 };
		Require(WinterLook::Seen(drawn, 10) == 0b111, "A draw claims each waiting layer");
		Require(WinterLook::Seen(drawn, 11) == 0 && !WinterLook::Waiting(), "A claimed layer is not claimed twice");
		Require(WinterLook::Report(white, { 0.82f, 0.85f, 0.88f, 0.93f }), "White ground turns into snow");
		Require(!WinterLook::Report(green, { 0.30f, 0.35f, 0.15f, 0.0f }), "Green ground changes nothing");
		Require(WinterLook::Report(patched, { 0.9f, 0.9f, 0.9f, 1.0f }), "A profiled texture is measured too");
		Require(Surfaces::TypeFor(nullptr, "whitegrass01.dds") == Surfaces::Type::kSnow, "Measured white grass is snow");
		Require(Surfaces::TypeFor(nullptr, "grass02.dds") == Surfaces::Type::kGrass, "Measured green grass stays grass");
		Require(Surfaces::TypeFor(nullptr, "dirt01.dds") == Surfaces::Type::kStone, "A profile's type wins over the looks");
		Require(!WinterLook::MostlySnowy() && WinterLook::Reevaluate(20) && WinterLook::MostlySnowy(),
			"Mostly white ground drawn lately makes a winter world");
		Require(!WinterLook::Reevaluate(5000) && WinterLook::MostlySnowy(), "Nothing drawn lately (an interior) keeps the verdict");
		const uint64_t rock = WinterLook::Register("rock01.dds");
		const uint64_t soil = WinterLook::Register("soil02.dds");
		const uint64_t later[4]{ green, rock, soil, 0 };
		Require(WinterLook::Seen(later, 6000) == 0b110, "Only the new layers are claimed");
		WinterLook::Report(rock, { 0.4f, 0.4f, 0.4f, 0.0f });
		WinterLook::Abandon(soil);
		Require(WinterLook::Waiting() && WinterLook::Seen(later, 6001) == 0b100, "A layer without a texture is tried again");
		WinterLook::Report(soil, { 0.3f, 0.25f, 0.2f, 0.0f });
		Require(WinterLook::Reevaluate(6002) && !WinterLook::MostlySnowy(), "Green and grey ground ends the winter world");
		Settings::winterMode = 0;
		Require(Surfaces::TypeFor(nullptr, "whitegrass01.dds") == Surfaces::Type::kGrass, "WinterMode 0 ignores the looks");
		WinterLook::Forget();
		std::puts("PASS winter looks: measured white ground is snow, profiles win, winter world with hysteresis");

		Write(root / "zModule/Surfaces.ini", "[StampDefaults]\nStampRimSpan=4\nSnowStampChurn=99\nSnowStampReposeRate=-1\n");
		Write(root / "Patches/zPatch/Surfaces.ini", "[StampDefaults]\nStampRimSpan=6\n");
		Settings::Load(); Surfaces::LoadProfiles();
		Near(Settings::stampRimSpan, 6, "Patch stamp default overrides module and main INI");
		Near(Settings::snowStampChurn, 8, "Snow default clamps");
		Near(Settings::snowStampReposeRate, -1, "Snow inheritance sentinel retained");
		fs::remove(root / "Patches/zPatch/Surfaces.ini");
		Write(root / "zModule/Surfaces.ini", "[StampDefaults]\nStampRimSpan=nan\n");
		Settings::Load(); Surfaces::LoadProfiles();
		Near(Settings::stampRimSpan, Settings::Values{}.stampRimSpan, "Deleted/invalid profile default resets via normal settings reload");
		std::puts("PASS stamp default precedence, bounds, inheritance and deletion reset");

		Write(root / fs::path(L"\u6e2c\u8a66") / "Surfaces.ini", "[Unicode]\nTexture=unicode_test\nType=ash\n");
		Surfaces::LoadProfiles();
		Require(Surfaces::TypeFor(nullptr, "unicode_test.dds") == Surfaces::Type::kAsh, "Unicode module folder");
		Require(Surfaces::ProfileName(-1) == nullptr && Surfaces::ProfileName(9999) == nullptr, "Invalid profile lookup");
		std::puts("PASS Unicode folder and invalid profile lookup");

		// A future timestamp in one module used to mask all edits/deletions in older
		// modules. Preserve a file's timestamp while changing its size as a second case.
		fs::last_write_time(root / "AModule/Surfaces.ini", fs::file_time_type::clock::now() + std::chrono::hours(24 * 365));
		const auto beforeRevision = Surfaces::ProfilesRevision();
		const auto patchPath = root / "Patches/aPatch/Surfaces.ini";
		const auto patchTime = fs::last_write_time(patchPath);
		Write(patchPath, "[Resized]\nTexture=dirt01\nType=mud\nDepth=3\n");
		fs::last_write_time(patchPath, patchTime);
		const auto resizedRevision = Surfaces::ProfilesRevision();
		Require(resizedRevision != beforeRevision, "A newer module cannot mask an older profile size change");
		fs::remove(patchPath);
		Require(Surfaces::ProfilesRevision() != resizedRevision, "Profile removal changes revision even under a newer timestamp");
		Write(patchPath, "[Restored]\nTexture=dirt01\nType=stone\n");
		std::puts("PASS independent profile revisions: newer-file masking, preserved-time size change and deletion");

		Write(ini, "EnableLiveReload=1\nLiveReloadInterval=0.05\n"); Settings::Load();
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		Write(root / "Patches/aPatch/Surfaces.ini", "[Changed]\nTexture=dirt01\nType=gravel\n");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		bool changed = false;
		while (std::chrono::steady_clock::now() < deadline && !changed) {
			changed = Settings::PollForChanges(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Require(changed, "Actual profile edit must trigger the settings watcher");
		Surfaces::LoadProfiles();
		Require(Surfaces::TypeFor(nullptr, "dirt01.dds") == Surfaces::Type::kGravel, "Watched profile reload applies");
		Surfaces::ReleaseProfiles(); Require(Surfaces::ProfileCount() == 0, "Release clears profiles");
		std::puts("PASS actual profile file watcher, reload and release");

		// Marks in the snow blanket: half its depth as a trench, never below a buried mesh's
		// top (a rock stays under the snow), shallower on thin snow over stone and gravel.
		Settings::Load();
		snowFixture = true;
		fixtureBlanket = 35.0f;
		fixtureBuried = 0.0f;
		Near(Surfaces::MarkDepth(Surfaces::Type::kSnow, 7.2f, 1.0f, 0, 0), 17.5f, "A snow trench is half the blanket");
		Near(Surfaces::MarkDepth(Surfaces::Type::kSnow, 7.2f, 1.6f, 0, 0), 28.0f, "A deathclaw's trench goes deeper");
		Near(Surfaces::MarkDepth(Surfaces::Type::kDirt, 4.0f, 1.0f, 0, 0), 4.0f, "Other ground keeps its own depth");
		fixtureBuried = 25.0f;
		Near(Surfaces::MarkDepth(Surfaces::Type::kSnow, 7.2f, 1.0f, 0, 0), 8.0f, "A mark stops over a buried rock's top");
		fixtureBuried = 34.0f;
		Near(Surfaces::MarkDepth(Surfaces::Type::kSnow, 7.2f, 1.0f, 0, 0), 0.0f, "Snow just over a mesh takes no mark");
		fixtureBuried = 0.0f;
		Near(Surfaces::MarkDepth(Surfaces::Type::kSnow, 7.2f * 0.3f, 0.3f, 0, 0), 5.25f, "Thin snow over gravel takes a shallow mark");
		snowFixture = false;
		std::puts("PASS snow marks: trench depth, buried meshes kept covered, thin snow over stone and gravel");
		fs::current_path(original); fs::remove_all(temporary);
		std::puts("ALL PROFILE TESTS PASS"); return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		fs::current_path(original); fs::remove_all(temporary); return 1;
	}
}
