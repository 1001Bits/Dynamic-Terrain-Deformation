// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Globals.h"
#include "Offsets.h"

#include <mutex>
#include <unordered_map>

namespace globals
{
	namespace
	{
		LARGE_INTEGER g_lastTick{};
		double        g_tickToSeconds{ 0.0 };

		// Written by the thread that advances the frame, read by the render threads.
		std::atomic<float>    g_delta{ 0.0f };
		std::atomic<float>    g_rawDelta{ 0.0f };
		std::atomic<bool>     g_paused{ false };
		std::atomic<uint64_t> g_frameIndex{ 0 };

		// Engine addresses resolve through a binary search of the address library; the
		// ones read every draw or frame are looked up once.
		std::uintptr_t GraphicsStateAddress()
		{
			static const auto address = Offsets::Address(Offsets::GraphicsState);
			return address;
		}

		std::uintptr_t TESSlotAddress()
		{
			static const auto address = Offsets::Address(Offsets::TESSingleton);
			return address;
		}
	}

	bool Initialize()
	{
		// The device normally arrives through the D3D11CreateDeviceAndSwapChain hook. The
		// renderer singleton is the fallback; its id is only valid on 1.10.163 and VR.
		if ((!d3d::device || !d3d::context) && !REL::Module::IsNG()) {
			if (auto* renderer = RE::BSGraphics::RendererData::GetSingleton()) {
				d3d::device = reinterpret_cast<ID3D11Device*>(renderer->device);
				d3d::context = reinterpret_cast<ID3D11DeviceContext*>(renderer->context);
			}
		}

		if (!game::player) {
			game::player = Offsets::ReadGlobalPointer<RE::PlayerCharacter>(Offsets::PlayerSingleton);
		}
		if (!game::ui) {
			game::ui = Offsets::ReadGlobalPointer<RE::UI>(Offsets::UISingleton);
		}

		if (g_tickToSeconds == 0.0) {
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			g_tickToSeconds = frequency.QuadPart ? 1.0 / static_cast<double>(frequency.QuadPart) : 0.0;
		}

		return Ready();
	}

	bool Ready()
	{
		return d3d::device != nullptr && d3d::context != nullptr;
	}

	bool GamePaused()
	{
		return g_paused.load(std::memory_order_relaxed);
	}

	float FrameDelta()
	{
		return GamePaused() ? 0.0f : g_delta.load(std::memory_order_relaxed);
	}

	float RawFrameDelta()
	{
		return g_rawDelta.load(std::memory_order_relaxed);
	}

	uint64_t FrameIndex()
	{
		return g_frameIndex.load(std::memory_order_relaxed);
	}

	void BeginFrame()
	{
		g_frameIndex.fetch_add(1, std::memory_order_relaxed);

		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);

		float raw = 0.0f;
		if (g_lastTick.QuadPart != 0 && g_tickToSeconds > 0.0) {
			raw = static_cast<float>(static_cast<double>(now.QuadPart - g_lastTick.QuadPart) * g_tickToSeconds);
		}
		g_lastTick = now;

		// A long stall (alt-tab, load) is not time the ground spent recovering.
		g_delta.store((raw > 0.0f && raw < 0.25f) ? raw : 0.0f, std::memory_order_relaxed);
		g_rawDelta.store(raw > 0.0f ? std::min(raw, 1.0f) : 0.0f, std::memory_order_relaxed);

		bool paused = false;
		if (auto* ui = game::ui) {
			paused = ui->menuMode != 0 || ui->freezeFramePause != 0;
		}
		if (auto* main = RE::Main::GetSingleton()) {
			paused = paused || main->freezeTime || !main->gameActive;
		}
		g_paused.store(paused, std::memory_order_relaxed);
	}

	const RE::BSGraphics::CameraStateData* Camera()
	{
		const auto state = GraphicsStateAddress();
		if (!state) {
			return nullptr;
		}
		return reinterpret_cast<const RE::BSGraphics::CameraStateData*>(
			state + offsetof(RE::BSGraphics::State, cameraState));
	}

	RE::TES* TES()
	{
		const auto slot = TESSlotAddress();
		return slot ? *reinterpret_cast<RE::TES**>(slot) : nullptr;
	}

	RE::TESObjectCELL* PlayerCell()
	{
		auto* player = game::player;
		return player ? player->GetParentCell() : nullptr;
	}

	bool PlayerInExterior()
	{
		const auto* cell = PlayerCell();
		return cell && cell->IsExterior();
	}

	RE::TESWorldSpace* PlayerWorldspace()
	{
		auto* cell = PlayerCell();
		return cell && cell->IsExterior() ? cell->worldSpace : nullptr;
	}

	RE::NiAVObject* Loaded3D(const RE::TESObjectREFR* a_ref)
	{
		const auto* loaded = a_ref ? a_ref->loadedData : nullptr;
		return loaded ? loaded->data3D.get() : nullptr;
	}

	namespace
	{
		// RaceHasAnyKeyword's answers per race, for the list they were asked with.
		std::mutex                                     g_raceLock;
		std::string                                    g_raceList;
		std::unordered_map<const RE::TESRace*, bool>   g_raceAnswers;
	}

	void ResetFormCaches()
	{
		const std::scoped_lock lock(g_raceLock);
		g_raceAnswers.clear();
	}

	uint32_t RaceFormID(const RE::Actor* a_actor)
	{
		auto* base = a_actor ? a_actor->GetObjectReference() : nullptr;
		auto* npc = base && base->GetFormType() == RE::ENUM_FORM_ID::kNPC_ ?
		                static_cast<RE::TESNPC*>(base) :
		                nullptr;
		auto* race = npc ? npc->GetFormRace() : nullptr;
		return race ? race->GetFormID() : 0u;
	}

	bool RaceHasAnyKeyword(const RE::Actor* a_actor, std::string_view a_list)
	{
		auto* base = a_actor ? a_actor->GetObjectReference() : nullptr;
		auto* npc = base && base->GetFormType() == RE::ENUM_FORM_ID::kNPC_ ?
		                static_cast<RE::TESNPC*>(base) :
		                nullptr;
		auto* race = npc ? npc->GetFormRace() : nullptr;
		if (!race || a_list.empty()) {
			return false;
		}

		const std::scoped_lock lock(g_raceLock);
		if (g_raceList != a_list) {
			g_raceList = a_list;
			g_raceAnswers.clear();
		}
		if (const auto it = g_raceAnswers.find(race); it != g_raceAnswers.end()) {
			return it->second;
		}

		bool found = false;
		race->ForEachKeyword([&](RE::BGSKeyword* a_keyword) {
			const std::string_view editorID = a_keyword->formEditorID.c_str();
			for (size_t start = 0; start <= a_list.size();) {
				const auto comma = a_list.find(',', start);
				const auto end = comma == std::string_view::npos ? a_list.size() : comma;
				auto       piece = a_list.substr(start, end - start);
				while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) {
					piece.remove_prefix(1);
				}
				while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) {
					piece.remove_suffix(1);
				}
				if (!piece.empty() && piece.size() == editorID.size() &&
					_strnicmp(piece.data(), editorID.data(), piece.size()) == 0) {
					found = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
				if (comma == std::string_view::npos) {
					break;
				}
				start = comma + 1;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		g_raceAnswers.emplace(race, found);
		return found;
	}
}

namespace RE
{
	bool TES::GetLandHeight(const NiPoint3& a_position, float& a_height)
	{
		using func_t = bool (*)(TES*, const NiPoint3&, float&);
		static const auto address = Offsets::Address(Offsets::TESGetLandHeight);
		if (!address) {
			return false;
		}
		return reinterpret_cast<func_t>(address)(this, a_position, a_height);
	}

	TESLandTexture* TES::GetLandTexture(const NiPoint3& a_position)
	{
		using func_t = TESLandTexture* (*)(TES*, const NiPoint3&);
		static const auto address = Offsets::Address(Offsets::TESGetLandTexture);
		if (!address) {
			return nullptr;
		}
		return reinterpret_cast<func_t>(address)(this, a_position);
	}

	TESObjectCELL* TES::GetGridCellCell(std::int32_t a_x, std::int32_t a_y)
	{
		using func_t = TESObjectCELL* (*)(TES*, std::int32_t, std::int32_t);
		static const auto address = Offsets::Address(Offsets::TESGetGridCellCell);
		if (!address) {
			return nullptr;
		}
		return reinterpret_cast<func_t>(address)(this, a_x, a_y);
	}
}
