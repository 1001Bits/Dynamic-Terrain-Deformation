// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// Every engine address the plugin touches, per runtime. Each entry is a VariantID of
// (1.10.163 address-library id, next-gen address-library id, VR offset). A zero entry
// means "not verified for this runtime": Address() then returns 0 and the feature that
// needs it switches itself off with a log line instead of guessing.
//
// The 1.10.163 ids and the in-function deltas were derived from the PDB-named 1.10.163
// program in the Combined Ghidra project, and the 1.10.984 ones matched against them in
// the NG program; docs/FO4_PORT_NOTES.md records how each one was verified.

namespace Offsets
{
	[[nodiscard]] inline std::uintptr_t Address(const REL::VariantID& a_id)
	{
		return a_id.address();
	}

	[[nodiscard]] inline bool Is1_11_240()
	{
		return REL::Module::get().version() == REL::Version{ 1, 11, 240, 0 };
	}

	// An engine global whose address-library id differs between 1.10.984 and 1.11.240.
	// The 1.11.240 library dropped the NG ids of the whole .data singleton block
	// (2676370-2697706) and numbered those globals afresh; CommonLibF4's RelocationID has
	// no slot for a third id, and its lookup would silently land on a neighbouring global
	// that holds 0. VR resolves the OG id through its own library.
	struct GlobalID
	{
		std::uint64_t og;
		std::uint64_t ng;
		std::uint64_t ae1_11_240;
	};

	[[nodiscard]] inline std::uintptr_t Address(const GlobalID& a_id)
	{
		if (REL::Module::IsVR() || REL::Module::IsF4()) {
			return REL::ID(a_id.og).address();
		}
		const auto id = Is1_11_240() ? a_id.ae1_11_240 : a_id.ng;
		return id ? REL::ID(id).address() : 0;
	}

	template <class T>
	[[nodiscard]] inline T* ReadGlobalPointer(const GlobalID& a_id)
	{
		const auto slot = Address(a_id);
		return slot ? *reinterpret_cast<T* const*>(slot) : nullptr;
	}

	// The globals the plugin reads (pointers to the objects, or for the player a
	// NiPointer whose first member is the pointer). 1.11.240 ids were located at the same
	// instruction of functions identical to 1.10.984's; docs/FO4_PORT_NOTES.md lists them.
	inline constexpr GlobalID ProcessListsSingleton{ 1569706, 2688869, 4796160 };
	inline constexpr GlobalID CalendarSingleton{ 1444952, 2689092, 4796378 };
	inline constexpr GlobalID PlayerSingleton{ 303410, 2690919, 4798212 };
	inline constexpr GlobalID UISingleton{ 548587, 2689028, 4796314 };
	inline constexpr GlobalID DataHandlerSingleton{ 711558, 2688883, 4796135 };

	// ── renderer ────────────────────────────────────────────────────────────

	// BSDFPrePassShader::`vftable' (primary). SetupGeometry / RestoreGeometry sit at
	// 0x07 / 0x08 on flat; VR inserts two slots ahead of them.
	inline const REL::VariantID PrePassShaderVtbl{ 1091304, 1091304, 0x30B8C68 };

	[[nodiscard]] inline std::size_t PrePassSetupGeometrySlot() { return REL::Module::IsVR() ? 0x09 : 0x07; }
	[[nodiscard]] inline std::size_t PrePassRestoreGeometrySlot() { return REL::Module::IsVR() ? 0x0A : 0x08; }

	// VR queued renderer: 0x28AAB61 calls 0x28AA8D0 with {batch, blend, renderState}.
	// Its packed path bypasses SetupGeometry; a null first packet selects the native
	// RenderPassImmediately fallback for that group. 0x28AAD50 joins packet preparation.
	inline const REL::VariantID VRBatchSubmitCall{ 0, 0, 0x28AAB61 };
	inline const REL::VariantID VRBatchSubmit{ 0, 0, 0x28AA8D0 };
	inline const REL::VariantID VRBatchWait{ 0, 0, 0x28AAD50 };

	// BSShaderAccumulator::RegisterObject_Standard, and the offset of its one call to
	// BSGraphics::Renderer::RegisterForZPrePass (the non-instanced, non-alpha-tested
	// depth pre-pass registration).
	inline const REL::VariantID RegisterObjectStandard{ 289935, 2317856, 0x281C0D0 };
	[[nodiscard]] inline std::ptrdiff_t RegisterForZPrePassCall()
	{
		return REL::Module::IsVR() ? 0x171 : REL::Module::IsNG() ? 0x327 : 0x332;
	}

	// DrawWorld::Render_PreUI and its direct calls to DrawWorld::DeferredPrePass and
	// DrawWorld::Forward.
	inline const REL::VariantID RenderPreUI{ 984743, 2318321, 0x284E9E0 };
	[[nodiscard]] inline std::ptrdiff_t DeferredPrePassCall() { return REL::Module::IsNG() ? 0x2E3 : 0x17F; }
	[[nodiscard]] inline std::ptrdiff_t ForwardCall() { return REL::Module::IsNG() ? 0x3A6 : 0x1C9; }
	// VR only: Render_PreUI queues its passes for the render-submission thread. Its call to
	// BSGraphics::Renderer::Unlock hands over the renderer; before it the main thread still
	// owns the immediate context and the world camera is already set.
	[[nodiscard]] inline std::ptrdiff_t VRRendererUnlockCall() { return 0x24B; }

	// DrawWorld::RenderDeferredDecals and its one call to DrawWorld::ComputeShaderDecalsInput::
	// Set(input*, BSDeferredDecal::BSDFDecal*, bool), which copies each decal into the shader
	// input every frame (OG +0x15F; 1.10.984 and 1.11.240 +0x16F; VR +0x1BF). The setter's
	// output layout is identical on all four; GroundDecalFit.h lists it.
	inline const REL::VariantID RenderDeferredDecals{ 223495, 2318310, 0x2845CC0 };
	inline const REL::VariantID DecalInputSet{ 1422094, 2318403, 0x28643B0 };
	[[nodiscard]] inline std::ptrdiff_t DecalInputSetCall()
	{
		return REL::Module::IsVR() ? 0x1BF : REL::Module::IsNG() ? 0x16F : 0x15F;
	}

	// BSGraphics::State (the instance, not a pointer). cameraState lives at +0x160.
	inline const REL::VariantID GraphicsState{ 600795, 2704621, 0x65A2AB0 };

	// Flat draw-camera context. ApplyCameraStateData reads the executable's TLS
	// index, then TLS+0xB20 (or the default context), and copies projMat into
	// Context+0x22D0. OG: 0x141D22F20; AE: 0x14182BC80. NG not yet verified.
	inline const GlobalID FlatTLSIndex{ 842564, 0, 2787938 };
	inline const GlobalID FlatDefaultRendererContext{ 33539, 0, 2704428 };

	// VR only: the renderer context every render thread works through (a global each
	// thread entry sets to the default context; the slot before it is the fallback), and
	// its copy of the camera the queued passes render with: a BSTArray<ViewData> of one
	// entry per eye (data at +0x25D0, size at +0x25E0, 0x210 bytes each, projMat at +0x90).
	// State::ApplyCameraStateData fills it under the renderer lock, so on the submission
	// thread it is the draw's camera, while State::cameraState may already be the next
	// frame's.
	inline const REL::VariantID VRRendererContext{ 0, 0, 0x6235AC8 };
	inline const REL::VariantID VRDefaultRendererContext{ 0, 0, 0x6235AC0 };
	inline constexpr std::ptrdiff_t kVRContextViews = 0x25D0;
	inline constexpr std::ptrdiff_t kVRContextViewCount = 0x25E0;
	inline constexpr std::ptrdiff_t kVRViewDataProjection = 0x90;

	// ── world queries ───────────────────────────────────────────────────────

	// TES* (the global pointer).
	inline const REL::VariantID TESSingleton{ 1194835, 2698044, 0x5B042C0 };

	// bool TES::GetLandHeight(const NiPoint3&, float&)
	inline const REL::VariantID TESGetLandHeight{ 58595, 2192072, 0x0FA030 };
	// TESLandTexture* TES::GetLandTexture(const NiPoint3&)
	inline const REL::VariantID TESGetLandTexture{ 549652, 2192076, 0x0FA330 };
	// TESObjectCELL* TES::GetGridCellCell(int, int)  (exterior grid only)
	inline const REL::VariantID TESGetGridCellCell{ 1383837, 2192050, 0x0F8A10 };

	// ── explosions and projectile impacts ───────────────────────────────────

	// Explosion::`vftable'; Explosion::Initialize is slot 0xC6 (runs once per new
	// explosion, skipped while a save loads).
	inline const REL::VariantID ExplosionVtbl{ 1408942, 1408942, 0x2D954E8 };
	[[nodiscard]] inline std::size_t ExplosionInitializeSlot() { return 0xC6; }

	// MissileProjectile (bullets), BeamProjectile (lasers), ArrowProjectile vtables;
	// AddImpact(const ImpactCreation&) is slot 0xE4 in each.
	inline const REL::VariantID MissileProjectileVtbl{ 1432029, 1432029, 0x2D97E28 };
	inline const REL::VariantID BeamProjectileVtbl{ 700255, 700255, 0x2D93FD8 };
	inline const REL::VariantID ArrowProjectileVtbl{ 1479747, 1479747, 0x2D92B38 };
	[[nodiscard]] inline std::size_t ProjectileAddImpactSlot() { return 0xE4; }

	// Sky* Sky::GetSingleton()  (creates the instance on first use)
	inline const REL::VariantID SkyGetSingleton{ 484694, 2192448, 0x12FB50 };

	// ── landscape layer textures ────────────────────────────────────────────

	// TESObjectLAND::CreateGeometry gives each landscape layer its texture set through
	// BSLightingShaderProperty::SetLandscapeTextureSet(property, layer, BSTextureSet*,
	// BSShaderData*): layer 0 at +0xF2C, layers 1-12 in a loop at +0xF68 (the setter ignores
	// 4 and up). Offsets are OG and VR; 1.11.240 calls at +0x10F0/+0x1128. The BSTextureSet
	// is the BGSTextureSet's base at +0x68 (BGSTextureSet::GetAsShaderTextureSet, all three).
	// Only VR uses this: it creates landscape textures without names.
	inline const REL::VariantID LandCreateGeometry{ 624032, 2200097, 0x38E830 };
	inline const REL::VariantID SetLandscapeTextureSet{ 969591, 2316424, 0x27A2A00 };
	inline constexpr std::ptrdiff_t kVRLandTextureSetCalls[] = { 0xF2C, 0xF68 };
	inline constexpr std::ptrdiff_t kTextureSetBase = 0x68;

	// ── ray picks (the recipe TES::GetMaterialType uses) ────────────────────

	// bhkPickData::bhkPickData(CFilter, const NiPoint3& start, const NiPoint3& end)
	inline const REL::VariantID PickDataCtor{ 884870, 2192211, 0x109670 };
	// NiAVObject* TESObjectCELL::Pick(bhkPickData&)
	inline const REL::VariantID CellPick{ 434717, 2200263, 0x39B170 };
	// bool bhkPickData::HasHit() const
	inline const REL::VariantID PickHasHit{ 1181584, 2277770, 0x1DFB6F0 };
	// float bhkPickData::GetHitFraction() const
	inline const REL::VariantID PickGetHitFraction{ 476687, 2277771, 0x1DFB710 };
}
