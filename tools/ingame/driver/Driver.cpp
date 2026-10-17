// Test-only native console mailbox. Commands run on the F4SE game task queue;
// no global input, focus changes, renderer hooks or automatic save writes.
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "Offsets.h"
#include "GameLayouts.h"
#include <Windows.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <thread>
#include <vector>

namespace {
	namespace fs = std::filesystem;
	const fs::path* directory = nullptr; // Process lifetime, including queued tasks.
	const F4SE::TaskInterface* tasks = nullptr;
	std::atomic<bool> pending{false}, ready{false};
	constexpr REL::Version og{1, 10, 163, 0}, ae{1, 11, 240, 0}, vr{1, 2, 72, 0};

	void Write(const fs::path& name, const std::string& body)
	{
		const auto path = *directory / name;
		const auto temporary = fs::path(path.native() + L".tmp");
		{ std::ofstream out(temporary, std::ios::binary); out << body; out.flush(); if (!out) return; }
		MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}

	std::string Escaped(std::string_view text)
	{
		std::string out;
		for (const unsigned char c : text) {
			if (c == '\\' || c == '"') out += '\\';
			if (c < 0x20) out += std::format("\\u{:04x}", c);
			else out += static_cast<char>(c);
		}
		return out;
	}

	// Releases the cast world a pick holds a reference to, as Physics::CastRay does.
	void ReleaseWorld(std::byte* a_pick)
	{
		auto* world = *reinterpret_cast<std::byte**>(a_pick + 0xC0);
		if (!world) return;
		*reinterpret_cast<std::byte**>(a_pick + 0xC0) = nullptr;
		if (*reinterpret_cast<volatile std::uint16_t*>(world + 0xA) == 0) return;
		auto* counts = reinterpret_cast<volatile LONG*>(world + 0x8);
		LONG before = 0, after = 0;
		do {
			before = *counts;
			after = static_cast<LONG>((static_cast<std::uint32_t>(before) & 0xFFFF0000u) |
				((static_cast<std::uint32_t>(before) - 1u) & 0xFFFFu));
		} while (InterlockedCompareExchange(counts, after, before) != before);
		if ((static_cast<std::uint32_t>(after) & 0xFFFFu) == 0) {
			using Delete = void (*)(void*);
			reinterpret_cast<Delete>((*reinterpret_cast<void***>(world))[3])(world);
		}
	}

	// What a vertical engine pick (TESObjectCELL::Pick, as the plugin's Physics::CastRay
	// uses it) hits between two heights on the given collision layers: the hit height and
	// the hit object's RTTI, name and owning reference. Flat runtimes only.
	std::string VerticalPick(RE::TESObjectCELL* cell, float x, float y, float fromZ, float toZ, std::uint64_t layers)
	{
		using Ctor = void* (*)(void*, std::uint32_t, const RE::NiPoint3&, const RE::NiPoint3&);
		using PickFn = RE::NiAVObject* (*)(RE::TESObjectCELL*, void*);
		using HasHit = bool (*)(const void*);
		using Fraction = float (*)(const void*);
		static const auto ctor = reinterpret_cast<Ctor>(Offsets::Address(Offsets::PickDataCtor));
		static const auto pick = reinterpret_cast<PickFn>(Offsets::Address(Offsets::CellPick));
		static const auto hasHit = reinterpret_cast<HasHit>(Offsets::Address(Offsets::PickHasHit));
		static const auto fraction = reinterpret_cast<Fraction>(Offsets::Address(Offsets::PickGetHitFraction));
		if (!cell || !ctor || !pick || !hasHit || !fraction) return "null";

		// Earlier identical picks, only to time the call (the soak found it can cost
		// milliseconds); each releases its cast world like the measured one below.
		LARGE_INTEGER frequency{}, begin{}, end{};
		QueryPerformanceFrequency(&frequency);
		double fastest = 1e30, total = 0;
		constexpr int kTimed = 4;
		for (int i = 0; i < kTimed; ++i) {
			alignas(16) std::byte probe[0x140]{};
			QueryPerformanceCounter(&begin);
			ctor(probe, 0x60000, RE::NiPoint3{ x, y, fromZ }, RE::NiPoint3{ x, y, toZ });
			*reinterpret_cast<std::uint64_t*>(probe + 0xC8) = layers;
			(void)pick(cell, probe);
			QueryPerformanceCounter(&end);
			const double micros = static_cast<double>(end.QuadPart - begin.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
			fastest = (std::min)(fastest, micros);
			total += micros;
			ReleaseWorld(probe);
		}

		// The same ray initialised as Rain Drops FO4 does: zeroed, material 0xFFFF, collector 3,
		// ends through bhkPickData::SetStartEnd, and neither charged to nor skipped by the
		// engine's per-frame pick budget (the constructor sets both).
		using SetEnds = void (*)(void*, const RE::NiPoint3&, const RE::NiPoint3&);
		const auto& module = REL::Module::get();
		const std::uintptr_t setEndsRva = REL::Module::IsVR() ? 0x27170 : module.version() == og ? 0x266C0 : 0xE23850;
		const auto* setEndsCode = reinterpret_cast<const std::uint8_t*>(module.base() + setEndsRva);
		const bool setEndsVerified = setEndsCode[0] == 0x48 && setEndsCode[1] == 0x8B && setEndsCode[2] == 0xC4 &&
			setEndsCode[3] == 0x48 && setEndsCode[4] == 0x83 && setEndsCode[5] == 0xEC;
		double rainFastest = -1, rainTotal = 0;
		if (setEndsVerified) {
			rainFastest = 1e30;
			for (int i = 0; i < kTimed; ++i) {
				alignas(16) std::byte probe[0x140]{};
				*reinterpret_cast<std::uint16_t*>(probe + 0x08) = 0xFFFF;
				*reinterpret_cast<std::uint32_t*>(probe + 0xD8) = 3;
				*reinterpret_cast<std::uint32_t*>(probe + 0x0C) = 0x60000;
				*reinterpret_cast<std::uint64_t*>(probe + 0xC8) = layers;
				QueryPerformanceCounter(&begin);
				reinterpret_cast<SetEnds>(module.base() + setEndsRva)(probe, RE::NiPoint3{ x, y, fromZ }, RE::NiPoint3{ x, y, toZ });
				(void)pick(cell, probe);
				QueryPerformanceCounter(&end);
				const double micros = static_cast<double>(end.QuadPart - begin.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
				rainFastest = (std::min)(rainFastest, micros);
				rainTotal += micros;
				ReleaseWorld(probe);
			}
		}

		alignas(16) std::byte data[0x140]{};
		ctor(data, 0x60000, RE::NiPoint3{ x, y, fromZ }, RE::NiPoint3{ x, y, toZ });
		*reinterpret_cast<std::uint64_t*>(data + 0xC8) = layers;
		// What the constructor left in the budget fields, for the record.
		const auto budgetFields = std::format("{:02X}{:02X}{:02X}", static_cast<unsigned>(data[0xDC]),
			static_cast<unsigned>(data[0xDD]), static_cast<unsigned>(data[0xD8]));
		auto* object = pick(cell, data);
		const bool skipped = data[0xDE] != std::byte{ 0 };
		const std::string timing = std::format(
			",\"pickMicrosMin\":{:.1f},\"pickMicrosAvg\":{:.1f},\"rainMicrosMin\":{:.1f},\"rainMicrosAvg\":{:.1f},"
			"\"ctorTimeFailCollector\":\"{}\",\"skipped\":{}",
			fastest, total / kTimed, rainFastest, rainFastest < 0 ? -1.0 : rainTotal / kTimed, budgetFields, skipped);
		std::string result = "{\"hit\":false" + timing + "}";
		if (hasHit(data)) {
			const float z = fromZ + (toZ - fromZ) * fraction(data);
			std::string rtti = "?", name, ref = "null";
			if (object) {
				if (const auto* type = object->GetRTTI()) rtti = *reinterpret_cast<const char* const*>(type);
				name = object->name.c_str();
				// The owning reference sits on the hit node or one of its parents.
				for (const RE::NiAVObject* node = object; node; node = node->parent) {
					if (const auto* owner = reinterpret_cast<const RE::TESObjectREFR*>(node->userData)) {
						const auto* base = owner->data.objectReference;
						ref = std::format("\"{:08X} base {:08X} type {}\"", owner->GetFormID(),
							base ? base->GetFormID() : 0, base ? static_cast<int>(base->GetFormType()) : -1);
						break;
					}
				}
			}
			result = std::format("{{\"hit\":true,\"z\":{},\"rtti\":\"{}\",\"name\":\"{}\",\"ref\":{}{}}}",
				z, Escaped(rtti), Escaped(name), ref, timing);
		}
		ReleaseWorld(data);
		return result;
	}

	std::string State()
	{
		auto* player = Offsets::ReadGlobalPointer<RE::PlayerCharacter>(Offsets::PlayerSingleton);
		auto* ui = Offsets::ReadGlobalPointer<RE::UI>(Offsets::UISingleton);
		auto* cell = player ? player->GetParentCell() : nullptr;
		auto* calendar = Offsets::ReadGlobalPointer<RE::Calendar>(Offsets::CalendarSingleton);
		const auto number = [](const RE::TESGlobal* value) {
			return value && std::isfinite(value->value) ? std::format("{}", value->value) : "null";
		};
		RE::Sky* sky = nullptr;
		if (cell) {
			using GetSky = RE::Sky* (*)();
			static const auto getSky = reinterpret_cast<GetSky>(Offsets::Address(Offsets::SkyGetSingleton));
			if (getSky) sky = getSky();
		}
		const auto* weather = sky ? sky->currentWeather : nullptr;
		const float waterHeight = GameLayouts::CellWaterHeight(cell);
		const std::string water = std::isfinite(waterHeight) ? std::format("{}", waterHeight) : "null";
		// The 1.10.163 TESWorldSpace default water height offset, read on every runtime so the
		// value can be checked against the worldspace's DNAM (Commonwealth: 450).
		const auto* world = cell && cell->IsExterior() ? cell->worldSpace : nullptr;
		const float worldWater = world ? GameLayouts::Read<float>(world, 0x21C) : std::numeric_limits<float>::quiet_NaN();
		const std::string worldDefault = std::isfinite(worldWater) ? std::format("{}", worldWater) : "null";
		std::string position = "null", angle = "null";
		std::string landHeight = "null", landTextureId = "null", diffuseTexture = "null", picks = "null";
		if (player) {
			const auto& p = player->data.location;
			if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
				position = std::format("[{},{},{}]", p.x, p.y, p.z);
			// Degrees, as setangle takes them, so a view can be restored exactly.
			const auto& a = player->data.angle;
			if (std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)) {
				constexpr float degrees = 57.29577951f;
				angle = std::format("[{},{},{}]", a.x * degrees, a.y * degrees, a.z * degrees);
			}
			if (cell && cell->IsExterior()) {
				static const auto tesSlot = Offsets::Address(Offsets::TESSingleton);
				auto* tes = tesSlot ? *reinterpret_cast<void* const*>(tesSlot) : nullptr;
				using Height = bool (*)(void*, const RE::NiPoint3&, float&);
				using Texture = RE::TESLandTexture* (*)(void*, const RE::NiPoint3&);
				static const auto height = reinterpret_cast<Height>(Offsets::Address(Offsets::TESGetLandHeight));
				static const auto texture = reinterpret_cast<Texture>(Offsets::Address(Offsets::TESGetLandTexture));
				float z = 0;
				if (tes && height && texture && height(tes, p, z) && std::isfinite(z)) {
					landHeight = std::format("{}", z);
					if (!REL::Module::IsVR()) {
						// Static+anim static, the landscape layer 0x11, and both: 200 above to 30 below.
						picks = std::format("{{\"static\":{},\"ground\":{},\"all\":{}}}",
							VerticalPick(cell, p.x, p.y, z + 200.0f, z - 30.0f, (1ull << 1) | (1ull << 2)),
							VerticalPick(cell, p.x, p.y, z + 200.0f, z - 30.0f, 1ull << 17),
							VerticalPick(cell, p.x, p.y, z + 200.0f, z - 30.0f, (1ull << 1) | (1ull << 2) | (1ull << 17)));
					}
					RE::NiPoint3 sample{p.x, p.y, z + 2.0f};
					if (const auto* ltex = texture(tes, sample)) {
						landTextureId = std::format("{}", ltex->GetFormID());
						if (const auto* path = GameLayouts::DiffusePath(GameLayouts::LandTextureSet(ltex))) {
							std::string escaped;
							for (const unsigned char c : std::string_view(path)) {
								if (c == '\\' || c == '"') escaped += '\\';
								if (c < 0x20) escaped += std::format("\\u{:04x}", c);
								else escaped += static_cast<char>(c);
							}
							diffuseTexture = '"' + escaped + '"';
						}
					}
				}
			}
		}
		return std::format("\"pid\":{},\"cellId\":{},\"exterior\":{},\"position\":{},\"angleDegrees\":{},\"menuMode\":{},"
			"\"worldspaceId\":{},\"gameHour\":{},\"timeScale\":{},\"weatherId\":{},\"weatherFlags\":{},"
			"\"landHeight\":{},\"landTextureId\":{},\"diffuseTexture\":{},\"cellWaterHeight\":{},\"worldWater21C\":{},\"picks\":{}",
			GetCurrentProcessId(), cell ? cell->GetFormID() : 0, cell && cell->IsExterior(),
			position, angle, ui ? ui->menuMode : 0,
			cell && cell->IsExterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0,
			number(calendar ? calendar->gameHour : nullptr), number(calendar ? calendar->timeScale : nullptr),
			weather ? weather->GetFormID() : 0,
			weather ? static_cast<unsigned>(static_cast<unsigned char>(weather->weatherData[11])) : 0,
			landHeight, landTextureId, diffuseTexture, water, worldDefault, picks);
	}

	void Run(std::uint64_t sequence, const std::vector<std::string>& commands)
	{
		std::size_t executed = 0;
		try {
			for (const auto& command : commands) {
				if (command != "@status") RE::Console::ExecuteCommand(command.c_str());
				++executed;
			}
			Write(std::to_string(sequence) + ".json",
				std::format("{{\"sequence\":{},\"executed\":{},\"ok\":true,{}}}", sequence, executed, State()));
		} catch (const std::exception&) {
			Write(std::to_string(sequence) + ".json", std::format(
				"{{\"sequence\":{},\"executed\":{},\"ok\":false,\"error\":\"task exception\"}}", sequence, executed));
		}
		pending.store(false, std::memory_order_release);
	}

	void Watch()
	{
		std::uint64_t last = 0;
		// Long enough for sessions past the upstream-reported 20-30 minute problem onset.
		const auto end = std::chrono::steady_clock::now() + std::chrono::minutes(120);
		while (std::chrono::steady_clock::now() < end) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (pending.load(std::memory_order_acquire)) continue;
			std::error_code error;
			const auto path = *directory / "commands.txt";
			if (fs::file_size(path, error) > 65536 || error) continue;
			std::ifstream in(path, std::ios::binary);
			std::string line;
			if (!std::getline(in, line)) continue;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::uint64_t sequence = 0;
			const auto [stop, ec] = std::from_chars(line.data(), line.data() + line.size(), sequence);
			if (ec != std::errc{} || stop != line.data() + line.size() || sequence <= last) continue;
			last = sequence;
			std::vector<std::string> commands;
			bool valid = true;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.empty()) continue;
				if (line.size() > 2048 || line.find('\0') != std::string::npos || commands.size() >= 64 ||
					(line.starts_with('@') && line != "@status")) { valid = false; break; }
				commands.push_back(line);
			}
			if (!valid || commands.empty()) {
				Write(std::to_string(sequence) + ".json", std::format(
					"{{\"sequence\":{},\"ok\":false,\"error\":\"invalid batch\"}}", sequence));
				continue;
			}
			pending.store(true, std::memory_order_release);
			tasks->AddTask([sequence, commands = std::move(commands)] { Run(sequence, commands); });
		}
	}

	void F4SEAPI Message(F4SE::MessagingInterface::Message* message)
	{
		if (message && message->type == F4SE::MessagingInterface::kGameDataReady && message->data && !ready.exchange(true)) {
			Write("ready.json", std::format("{{\"pid\":{},\"ackMeaning\":\"console call returned; verify effects independently\"}}", GetCurrentProcessId()));
			std::thread(Watch).detach();
		}
	}
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* query, F4SE::PluginInfo* info)
{
	if (!query || !info) return false;
	info->infoVersion = F4SE::PluginInfo::kVersion;
	info->name = "NMN_TerrainTestDriver";
	info->version = 1;
	return !query->IsEditor();
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* load)
{
	wchar_t folder[32768]{};
	const auto length = GetEnvironmentVariableW(L"NMN_TERRAIN_TEST_DIR", folder, std::size(folder));
	if (!load || !length || length >= std::size(folder)) return false;
	auto* path = new fs::path(folder);
	std::error_code error;
	if (!path->is_absolute() || !fs::is_directory(*path, error) || error) { delete path; return false; }
	directory = path;
	F4SE::Init(load, false);
	const auto version = REL::Module::get().version();
	if (version != og && version != ae && version != vr) return false;
	if (version == vr) {
		// VR CSV ID 1061864 and the live native disassembly both identify this
		// command-string entry. Do not run commands if the installed library disagrees.
		if (REL::ID(1061864).address() != REL::Module::get().base() + 0x12DC460) return false;
	}
	tasks = F4SE::GetTaskInterface();
	const auto* messaging = F4SE::GetMessagingInterface();
	return tasks && messaging && messaging->RegisterListener(Message);
}

F4SE_EXPORT auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};
	data.PluginName("NMN_TerrainTestDriver");
	data.PluginVersion(REL::Version{1, 0, 0, 0});
	data.AuthorName("NMN local validation");
	data.UsesAddressLibrary(true);
	data.CompatibleVersions({ae});
	return data;
}();
