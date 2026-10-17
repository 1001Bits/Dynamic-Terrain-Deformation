// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace RE
{
	class TES;
	class TESObjectCELL;
	class TESWorldSpace;
}

namespace globals
{
	namespace d3d
	{
		inline ID3D11Device*        device{ nullptr };
		inline ID3D11DeviceContext* context{ nullptr };
	}

	namespace game
	{
		inline RE::PlayerCharacter* player{ nullptr };
		inline RE::UI*              ui{ nullptr };
	}

	bool Initialize();

	bool Ready();

	// Seconds since the previous rendered world frame, clamped to [0, 0.25]; 0 while the
	// game is paused (menus, loading) so marks neither fade nor grow in a menu.
	float FrameDelta();
	bool  GamePaused();

	// The same clock without the pause gate or the stall cut-off (capped at one second),
	// for timers that must keep running in menus and at low frame rates (the settings
	// poll, the profiler).
	float RawFrameDelta();

	// Advances the frame clock; called once per rendered world frame.
	void BeginFrame();

	// Counts rendered world frames (BeginFrame calls).
	uint64_t FrameIndex();

	// The engine's camera block (BSGraphics::State::cameraState), or nullptr.
	const RE::BSGraphics::CameraStateData* Camera();

	RE::TES* TES();

	// The player's parent cell, or nullptr while none is attached.
	RE::TESObjectCELL* PlayerCell();

	// True only while the player stands in an exterior cell, the only place with
	// landscape to deform. Interiors skip every per-frame ground query and dispatch.
	bool PlayerInExterior();

	// The exterior worldspace the player is in, or nullptr in an interior or with no cell.
	RE::TESWorldSpace* PlayerWorldspace();

	// A reference's loaded (third-person) scene root, read from its loaded data rather
	// than through the Get3D virtual so the call does not depend on a runtime's vtable.
	RE::NiAVObject* Loaded3D(const RE::TESObjectREFR* a_ref);

	// True when any keyword on the actor's base race has an editor id in a_list (comma
	// separated, case-insensitive). The base race is read from the NPC form, whose layout
	// is the same on every runtime.
	bool RaceHasAnyKeyword(const RE::Actor* a_actor, std::string_view a_list);

	// The form id of the actor's base race (0 when it has none), read the same way.
	uint32_t RaceFormID(const RE::Actor* a_actor);

	// Drops every cache keyed by a form pointer; a data reload frees the forms.
	void ResetFormCaches();
}

namespace RE
{
	// Fallout 4's TES is not described by CommonLibF4; the plugin only needs these
	// three queries, called through the verified addresses in Offsets.h.
	class TES
	{
	public:
		[[nodiscard]] bool           GetLandHeight(const NiPoint3& a_position, float& a_height);
		[[nodiscard]] TESLandTexture* GetLandTexture(const NiPoint3& a_position);
		[[nodiscard]] TESObjectCELL* GetGridCellCell(std::int32_t a_x, std::int32_t a_y);
	};
}
