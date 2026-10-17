// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "CameraProjection.h"
#include "Clipmap.h"
#include "Corpses.h"
#include "GameLayouts.h"
#include "FrameCamera.h"
#include "Globals.h"
#include "GroundDecals.h"
#include "HeatSources.h"
#include "Hooks.h"
#include "ObjectSnow.h"
#include "Impacts.h"
#include "IndoorPrints.h"
#include "ObjectStamps.h"
#include "Offsets.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShaderRegistry.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSparkle.h"
#include "StampShapes.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Tessellation.h"
#include "UtilityRouting.h"
#include "VRBatchRouting.h"
#include "Weather.h"
#include "WinterLook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <intrin.h>

namespace Hooks
{
	namespace
	{
		// BSRenderPass fields, as BSBatchRenderer::RenderPassImmediately reads them and
		// BSLightingShaderProperty::GetRenderPasses writes them.
		struct PassView
		{
			RE::BSShaderProperty* property{ nullptr };
			RE::BSGeometry*       geometry{ nullptr };
			std::uint32_t         technique{ 0 };
		};

		PassView View(const void* a_pass)
		{
			PassView view{};
			if (!a_pass) {
				return view;
			}
			const auto base = reinterpret_cast<std::uintptr_t>(a_pass);
			view.property = *reinterpret_cast<RE::BSShaderProperty* const*>(base + 0x10);
			view.geometry = *reinterpret_cast<RE::BSGeometry* const*>(base + 0x18);
			view.technique = *reinterpret_cast<const std::uint32_t*>(base + 0x48);
			return view;
		}

		// BSDFPrePassShader technique bits (BSDFPrePassShader::GetTechniqueName): "L" is
		// the landscape, "Lodl" LOD land, "Llodb" the near-to-LOD land blend.
		constexpr std::uint32_t kTechLandscape = 0x20;
		constexpr std::uint32_t kTechLODLand = 0x200;

		[[nodiscard]] bool IsNearLandscape(std::uint32_t a_technique)
		{
			return (a_technique & kTechLandscape) != 0 && (a_technique & kTechLODLand) == 0;
		}

		RE::BSShaderMaterial* MaterialOf(RE::BSShaderProperty* a_property)
		{
			if (!a_property) {
				return nullptr;
			}
			return *reinterpret_cast<RE::BSShaderMaterial* const*>(
				reinterpret_cast<std::uintptr_t>(a_property) + 0x58);
		}

		enum class VRRead
		{
			kRead,
			kEmpty,  // no context or camera yet
			kFault,  // the addresses do not hold what they should
		};

		// VR: eye 0's projection from the renderer context (see Offsets.h), read under SEH
		// so that a wrong address costs the gate its source, not the game.
		VRRead ReadVRDrawProjection(std::uintptr_t a_slot, std::uintptr_t a_fallback, float (&a_out)[16])
		{
			__try {
				auto context = *reinterpret_cast<const std::uintptr_t*>(a_slot);
				if (!context && a_fallback) {
					context = *reinterpret_cast<const std::uintptr_t*>(a_fallback);
				}
				if (!context) {
					return VRRead::kEmpty;
				}
				const auto views = *reinterpret_cast<const std::uintptr_t*>(context + Offsets::kVRContextViews);
				const auto count = *reinterpret_cast<const std::uint32_t*>(context + Offsets::kVRContextViewCount);
				if (!views || count == 0) {
					return VRRead::kEmpty;
				}
				if (count > 2) {
					return VRRead::kFault;
				}
				std::memcpy(a_out, reinterpret_cast<const void*>(views + Offsets::kVRViewDataProjection), sizeof(a_out));
				return VRRead::kRead;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return VRRead::kFault;
			}
		}

		bool DrawProjection(float (&a_out)[16], bool& a_fromContext)
		{
			a_fromContext = false;
			if (REL::Module::IsVR()) {
				static const auto slot = Offsets::Address(Offsets::VRRendererContext);
				static const auto fallback = Offsets::Address(Offsets::VRDefaultRendererContext);
				static std::atomic<bool> unreadable{ false };
				if (slot && !unreadable.load(std::memory_order_relaxed)) {
					const auto read = ReadVRDrawProjection(slot, fallback, a_out);
					if (read == VRRead::kRead) {
						a_fromContext = true;
						return true;
					}
					if (read == VRRead::kFault && !unreadable.exchange(true)) {
						logger::warn("Camera gate: the VR renderer context gave no camera - "
									 "falling back to BSGraphics::State");
					}
				}
			} else {
				static const auto tlsIndex = Offsets::Address(Offsets::FlatTLSIndex);
				static const auto fallback = Offsets::Address(Offsets::FlatDefaultRendererContext);
				static std::atomic<bool> unreadable{ false };
				if (tlsIndex && !unreadable.load(std::memory_order_relaxed)) {
					const auto read = CameraProjection::Flat(__readgsqword(0x58),
						*reinterpret_cast<const std::uint32_t*>(tlsIndex), fallback, a_out);
					if (read == CameraProjection::Read::kRead) {
						a_fromContext = true;
						return true;
					}
					if (read == CameraProjection::Read::kFault && !unreadable.exchange(true)) {
						logger::warn("Camera gate: the flat renderer context gave no camera - "
							"falling back to BSGraphics::State");
					}
				}
			}

			const auto* camera = globals::Camera();
			if (!camera) {
				return false;
			}
			std::memcpy(a_out, camera->camViewData.projMat, sizeof(a_out));
			return true;
		}

		// The local map and any other orthographic render go through the same pre-pass;
		// only perspective views are the player's.
		bool DrawnFromPlayerCamera()
		{
			if (!Settings::routeOnlyPlayerCamera) {
				return true;
			}

			float proj[16];
			bool fromContext = false;
			if (!DrawProjection(proj, fromContext)) {
				return true;
			}
			const bool perspective = UtilityRouting::IsPerspectiveProjection(proj);
			const auto* stateCamera = Settings::logDraws ? globals::Camera() : nullptr;
			const auto* stateProjection = stateCamera ? reinterpret_cast<const float*>(stateCamera->camViewData.projMat) : nullptr;
			const bool statePerspective = UtilityRouting::IsPerspectiveProjection(stateProjection);
			// Bounded, one sample per frame/type: shared camera state can differ from a
			// queued draw's camera. Keep UI/frame evidence alongside the rejection.
			static std::atomic<uint64_t> sampledFrame[2]{ UINT64_MAX, UINT64_MAX };
			static std::atomic<uint32_t> samples[2]{};
			static std::atomic<uint32_t> mismatches{ 0 };
			static std::atomic<uint64_t> mismatchFrame{ UINT64_MAX };
			const auto index = perspective ? 1 : 0;
			const auto frame = globals::FrameIndex();
			if (Settings::logDraws && samples[index].load() < 16 &&
				sampledFrame[index].exchange(frame) != frame && samples[index].fetch_add(1) < 16) {
				const auto* camera = globals::Camera();
				const auto* ui = globals::game::ui;
				logger::info("Camera sample: perspective={} frame={} thread={} exterior={} menu={} paused={} "
					"stateCamera={} projW=({:.4f},{:.4f},{:.4f}) context={} statePerspective={}",
					perspective, frame, GetCurrentThreadId(), globals::PlayerInExterior(),
					ui ? ui->menuMode : 0, globals::GamePaused(),
					camera ? static_cast<const void*>(camera->referenceCamera) : nullptr,
					proj[11], proj[14], proj[15], fromContext, statePerspective);
			}
			if (Settings::logDraws && fromContext && stateProjection && perspective != statePerspective &&
				mismatches.load() < 16 && mismatchFrame.exchange(frame) != frame && mismatches.fetch_add(1) < 16) {
				logger::info("Camera source mismatch: frame={} contextPerspective={} statePerspective={} "
					"contextW=({:.4f},{:.4f},{:.4f}) stateW=({:.4f},{:.4f},{:.4f})",
					frame, perspective, statePerspective, proj[11], proj[14], proj[15],
					stateProjection[11], stateProjection[14], stateProjection[15]);
			}

			static std::atomic<bool> reported[2]{};
			if (!reported[perspective ? 1 : 0].exchange(true, std::memory_order_relaxed)) {
				logger::info("Camera gate: {} a landscape draw, projection w-row/column = "
							 "({:.4f}, {:.4f}, {:.4f})",
					perspective ? "ROUTING" : "refusing", proj[11], proj[14], proj[15]);
			}

			return perspective;
		}

		// How far from the field's centre terrain is routed: the field's window, and the snow
		// blanket's reach while the snow coverage holds any snow (without snow ground the
		// blanket raises nothing, and distant terrain keeps its vanilla draw and pre-pass).
		float ReachFromCentre(float a_windowHalfExtent)
		{
			if (!Settings::enableSnowRaise || !Settings::useClipmap ||
				Settings::snowRaiseHeight <= 0.0f || !SnowCoverage::AnySnow()) {
				return a_windowHalfExtent;
			}
			return std::max(a_windowHalfExtent, Settings::snowRaiseDistance);
		}

		bool WithinReach(float a_x, float a_y, float a_radius)
		{
			float centreX = 0.0f;
			float centreY = 0.0f;
			float halfExtent = 0.0f;
			if (!Clipmap::GetWindow(centreX, centreY, halfExtent)) {
				return true;
			}

			const float reach = ReachFromCentre(halfExtent);
			const float dx = std::max(0.0f, std::fabs(a_x - centreX) - reach);
			const float dy = std::max(0.0f, std::fabs(a_y - centreY) - reach);
			const float slack = a_radius + Settings::cullMargin;
			return (dx * dx + dy * dy) <= (slack * slack);
		}

		// The four 2048-unit quadrants of a landscape cell share one transform, the cell's
		// centre (TESObjectLAND::CreateGeometry places their vertices at -2048..0 / 0..2048
		// from it; MakeLandTriStrips gives every quadrant GetWorldOffsetForBlock's centre),
		// and the depth pre-pass gets nothing else to tell them apart. Both hooks therefore
		// decide by this one test on the whole cell, so a quadrant is either displaced and out
		// of the pre-pass or neither. The cell's half-diagonal also covers a quadrant whose
		// transform sat at its corner.
		constexpr float kCellRadius = 2900.0f;  // 2048 * sqrt(2), rounded up

		bool RoutesLandscape(const RE::NiPoint3& a_translate)
		{
			return !Settings::cullDistantDraws || WithinReach(a_translate.x, a_translate.y, kCellRadius);
		}

		bool ShouldRoute(RE::BSGeometry* a_geometry)
		{
			return !a_geometry || RoutesLandscape(a_geometry->world.translate);
		}

		void ReportException(const char* a_where, const std::exception& a_error)
		{
			static std::atomic<uint32_t> reported{ 0 };
			if (reported.fetch_add(1) < 8) {
				logger::error("{}: {} - this frame's work was skipped", a_where, a_error.what());
			}
		}

		std::atomic<uint32_t> g_landLogged{ 0 };
		constexpr uint32_t    kMaxLandLogged = 16;

		void LogLandscapeDraw(const PassView& a_view)
		{
			if (!Settings::logDraws || g_landLogged.load() >= kMaxLandLogged ||
				g_landLogged.fetch_add(1) >= kMaxLandLogged) {
				return;
			}

			const auto* geometry = a_view.geometry;
			float centreX = 0.0f, centreY = 0.0f, halfExtent = 0.0f;
			const bool haveWindow = Clipmap::GetWindow(centreX, centreY, halfExtent);
			logger::info(
				"Landscape pass: technique {:08X} vertexDesc {:016X} bound ({:.0f}, {:.0f}, {:.0f}) r {:.0f} | "
				"window ({:.0f}, {:.0f}) half {:.0f}{}",
				a_view.technique, geometry ? geometry->vertexDesc.desc : 0,
				geometry ? geometry->worldBound.center.x : 0.0f,
				geometry ? geometry->worldBound.center.y : 0.0f,
				geometry ? geometry->worldBound.center.z : 0.0f,
				geometry ? geometry->worldBound.fRadius : 0.0f,
				centreX, centreY, halfExtent, haveWindow ? "" : " (no window yet)");
		}

		std::atomic<bool> g_routing{ false };
		std::atomic<uint64_t> g_prepassDiagnosticCalls{ 0 };
		struct VRBatchSubmit
		{
			static void thunk(void* a_batch, bool a_blend, void* a_renderState)
			{
				if (!a_batch || !g_routing || !Settings::enableTessellation) {
					func(a_batch, a_blend, a_renderState);
					return;
				}

				const auto base = reinterpret_cast<std::uintptr_t>(a_batch);
				auto* owner = *reinterpret_cast<void**>(base);
				// The packet arrays are filled by another job. Join before inspecting them,
				// just as the first call inside the native submit does. Its second join is safe.
				static const REL::Relocation<void(void*)> wait{ Offsets::Address(Offsets::VRBatchWait) };
				wait(owner);
				const auto* ends = *reinterpret_cast<const std::uint32_t* const*>(base + 0x08);
				const auto groups = *reinterpret_cast<const std::uint32_t*>(base + 0x18);
				auto** packets = *reinterpret_cast<void***>(base + 0x20);
				const auto count = *reinterpret_cast<const std::uint32_t*>(base + 0x30);
				auto* const* passes = *reinterpret_cast<void* const* const*>(reinterpret_cast<std::uintptr_t>(owner) + 0x3D0);
				const auto prepass = Offsets::Address(Offsets::PrePassShaderVtbl);

				// These arrays belong to this queued batch; only its submission thread reads
				// them after the join. Restore every changed pointer before native destruction.
				VRBatchRouting::ScopedFallback restore;
				if (ends && packets && passes && groups <= count && count <= 65536) {
					restore.Select({ ends, groups }, { packets, count }, [&](std::uint32_t i) {
						if (!passes[i]) return false;
						const auto shader = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(passes[i]) + 8);
						if (!shader || *reinterpret_cast<const std::uintptr_t*>(shader) != prepass) return false;
						const auto view = View(passes[i]);
						return IsNearLandscape(view.technique) && ShouldRoute(view.geometry);
					});
				}
				static std::atomic<unsigned> logged{ 0 };
				if (Settings::logDraws && restore.Count() && logged.load() < 8 && logged.fetch_add(1) < 8) {
					logger::info("VR queued terrain fallback: groups={} packets={} selected={}", groups, count, restore.Count());
				}
				func(a_batch, a_blend, a_renderState);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// VR passes a third argument (the thread's render state), which its SetupGeometry
		// reads at once; flat builds ignore whatever is in that register.
		struct PrePass_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, void* a_pass, void* a_renderState)
			{
				func(a_shader, a_pass, a_renderState);

				Tessellation::Disarm();

				// With tessellation off and draw scopes on, the same near-terrain draws are
				// still selected, only to be timed: vanilla against tessellated, like for like.
				const bool measureVanilla = !Settings::enableTessellation && Settings::profileDrawScopes;
				if (!g_routing || (!Settings::enableTessellation && !measureVanilla)) {
					return;
				}

				const auto view = View(a_pass);
				if (Settings::logDraws) {
					const auto call = g_prepassDiagnosticCalls.fetch_add(1);
					if (call < 32) {
						logger::info("Prepass sample: call={} thread={} technique={:08X} pass={} geometry={} property={}",
							call, GetCurrentThreadId(), view.technique, a_pass,
							static_cast<const void*>(view.geometry), static_cast<const void*>(view.property));
					}
				}
				if (!IsNearLandscape(view.technique)) {
					return;
				}

				LogLandscapeDraw(view);
				Profiler::Tally(Profiler::Count::kLandscapeSeen);

				if (!DrawnFromPlayerCamera()) {
					return;
				}

				if (!ShouldRoute(view.geometry)) {
					Profiler::Tally(Profiler::Count::kLandscapeCulled);
					return;
				}

				if (measureVanilla) {
					Tessellation::Request request{};
					request.measureOnly = true;
					Tessellation::Arm(request);
					return;
				}

				try {
					const auto surfaces = Surfaces::Classify(MaterialOf(view.property));

					Tessellation::Request request{};
					request.mode = Tessellation::Mode::kLandscape;
					request.material.revealLayer = surfaces.reveal;
					request.material.snowLayer = surfaces.snow;
					request.material.strengthScale =
						surfaces.revealUnderSnow ? Settings::snowRevealStrength : 1.0f;
					for (size_t i = 0; i < Surfaces::kLayers; ++i) {
						if (surfaces.layers[i] == Surfaces::Type::kSnow) {
							request.material.thaw[i] = static_cast<int8_t>(surfaces.under[i]);
						}
						request.layerKeys[i] = surfaces.keys[i];
					}

					Tessellation::Arm(request);
				} catch (const std::exception& e) {
					ReportException("SetupGeometry", e);
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// True while DrawWorld::DeferredPrePass (the main world's G-buffer pass) runs; the
		// same shader also draws other G-buffers (a map, a menu's 3D).
		std::atomic<bool> g_inMainPrepass{ false };

		struct PrePass_RestoreGeometry
		{
			static void thunk(RE::BSShader* a_shader, void* a_pass, void* a_renderState)
			{
				Tessellation::Disarm();
				func(a_shader, a_pass, a_renderState);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The depth pre-pass would lay down undisplaced terrain depth before the G-buffer
		// pass draws the displaced surface, and every footprint would then fail the depth
		// test. Near terrain is left out of it while routing is live; its G-buffer pass
		// keeps the default test-and-write depth mode, so it lays down its own depth.
		std::atomic<uint32_t> g_zPrePassSkipped{ 0 };

		struct RegisterForZPrePass
		{
			static void thunk(void* a_renderer, bool a_firstPerson, RE::BSGraphics::TriShape* a_shape,
				std::uint32_t a_indices, RE::NiTransform* a_world, std::uint16_t a_arg5,
				void* a_segments, std::uint16_t a_arg7)
			{
				if (a_shape && !a_firstPerson &&
					a_shape->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_LANDDATA) &&
					g_routing.load(std::memory_order_relaxed) && Settings::enableTessellation &&
					Settings::useClipmap && Clipmap::Ready() && Tessellation::RoutedRecently()) {
					// a_world is the pass geometry's own world transform (RegisterObject_Standard
					// passes &geometry->world), the one ShouldRoute reads.
					const bool isNear = !a_world || RoutesLandscape(a_world->translate);
					if (isNear) {
						if (g_zPrePassSkipped.fetch_add(1) == 0) {
							logger::info("Depth pre-pass: near terrain is drawn by the G-buffer pass only");
						}
						return;
					}
				}

				func(a_renderer, a_firstPerson, a_shape, a_indices, a_world, a_arg5, a_segments, a_arg7);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		void ApplyReloadedSettings()
		{
			Tessellation::Reset();

			if (Settings::useClipmap && !Clipmap::Ready() && !Clipmap::Initialize()) {
				logger::error("Clipmap unavailable - the terrain stays vanilla");
			}
			if (SnowSparkle::Enabled()) {
				SnowSparkle::Initialize();
			}

			Surfaces::Reset();
			Surfaces::LoadProfiles();

			SnowSparkle::Reset();
			ObjectStamps::Reset();
			Corpses::Reset();
			Impacts::Reset();

			if (Settings::enableStampShapes && !StampShapes::Ready()) {
				StampShapes::Retry();
				StampShapes::Initialize();
			}
			StampShapes::RequestAnalysis();

			Clipmap::ResetDiagnostics();
			Corpses::ResetDiagnostics();
			GroundDecals::Reset();
			Weather::Reset();

			SnowCoverage::Initialize();
			SnowCoverage::Reset();

			Shelter::Reset();
			ObjectSnow::Reset();
			IndoorPrints::ResetDiagnostics();

			Profiler::Reset();
		}

		// Per-world state (the field, the snow and shelter maps, the weather clock, tracked
		// actors and objects) belongs to one worldspace and one timeline: a save load or new
		// game asks for a reset, and a change of exterior worldspace triggers one. Interiors
		// keep it as it is, so the ground outside the door is unchanged on the way back.
		std::atomic<bool>  g_worldResetRequested{ false };
		RE::TESWorldSpace* g_lastWorldspace{ nullptr };

		void ResetWorldState(const char* a_why)
		{
			FrameCamera::Reset();
			Clipmap::ResetWorld();
			SnowCoverage::Reset();
			Shelter::Reset();
			ObjectSnow::Reset();
			IndoorPrints::ResetWorld();
			Weather::Reset();
			ObjectStamps::Reset();
			Corpses::Reset();
			Impacts::Reset();
			SnowSparkle::Reset();
			logger::info("World state reset ({})", a_why);
		}

		void TrackWorld()
		{
			auto* worldspace = globals::PlayerWorldspace();
			if (g_worldResetRequested.exchange(false)) {
				ResetWorldState("game loaded");
				g_lastWorldspace = worldspace;
			} else if (worldspace && worldspace != g_lastWorldspace) {
				if (g_lastWorldspace) {
					ResetWorldState("worldspace changed");
				}
				g_lastWorldspace = worldspace;
			}
		}

		// Once per rendered world frame, on the thread that owns the immediate context, just
		// before the G-buffer pass: advances every system and dispatches the field update the
		// terrain samples.
		void FrameUpdate()
		{
			globals::BeginFrame();
			GroundDecals::NewFrame();
			if (Settings::logDraws && REL::Module::IsVR()) {
				static uint32_t reports = 0;
				static uint64_t nextFrame = 0;
				const auto frame = globals::FrameIndex();
				if (reports < 8 && frame >= nextFrame) {
					++reports;
					nextFrame = frame + 120;
					const auto* table = reinterpret_cast<const std::uintptr_t*>(Offsets::Address(Offsets::PrePassShaderVtbl));
					logger::info("VR prepass hook health: frame={} calls={} setupOwned={} restoreOwned={} routing={}",
						frame, g_prepassDiagnosticCalls.load(),
						table && table[Offsets::PrePassSetupGeometrySlot()] == reinterpret_cast<std::uintptr_t>(PrePass_SetupGeometry::thunk),
						table && table[Offsets::PrePassRestoreGeometrySlot()] == reinterpret_cast<std::uintptr_t>(PrePass_RestoreGeometry::thunk),
						g_routing.load());
				}
			}

			try {
				const float raw = globals::RawFrameDelta();
				const float step = globals::FrameDelta();

				if (Settings::PollForChanges(raw)) {
					ApplyReloadedSettings();
				}

				Profiler::Frame(raw);
				Tessellation::PrepareFrame();

				StampShapes::Analyse();

				TrackWorld();

				// A ground texture just measured as snow (or a world that turned out to be a
				// winter one): classify again, and read the snow coverage again in place.
				if (WinterLook::Update()) {
					Surfaces::Reset();
					SnowCoverage::Reread();
				}
				Weather::Update();

				// Interiors have no landscape: no ground queries, no ray picks, no dispatch.
				if (globals::PlayerInExterior()) {
					static bool exteriorReported = false;
					if (!exteriorReported) {
						exteriorReported = true;
						logger::info("Exterior world frame: terrain={} clipmap={}",
							Settings::enableTessellation, Settings::useClipmap);
					}
					Shelter::Update();
					SnowCoverage::Update();
					ObjectSnow::Update();

					if (Settings::useClipmap) {
						Clipmap::Update(step);
					}
				} else {
					// Inside: only the feet on the floors, for the floor prints.
					Clipmap::UpdateFloorFeet(step);
				}
				IndoorPrints::Update(step);

				SnowSparkle::Update(step);
			} catch (const std::exception& e) {
				ReportException("Frame update", e);
			}
		}

		struct DeferredPrePass
		{
			static void thunk()
			{
				FrameUpdate();
				g_inMainPrepass = true;
				func();
				g_inMainPrepass = false;
				// The G-buffer is complete; use the targets retained during geometry draws,
				// since the engine may already have unbound them. Decals and lighting follow.
				// The pass ends on the world camera, so without a landscape draw (an interior) its
				// per-frame constants are taken from what is bound now.
				if (!REL::Module::IsVR()) {
					FrameCamera::AfterPrePass(globals::d3d::context);
				}
				try {
					ObjectSnow::Render();
				} catch (const std::exception& e) {
					ReportException("Snow on objects", e);
				}
				try {
					IndoorPrints::Render();
				} catch (const std::exception& e) {
					ReportException("Floor prints", e);
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Fallout 4 VR queues its passes for a render-submission thread. Render_PreUI hands
		// the renderer lock to that thread just before it queues the G-buffer pass; until
		// that call the main thread owns the immediate context, and the world camera is set.
		struct VRRendererUnlock
		{
			static void thunk(void* a_renderer)
			{
				// A frame through a scope renders the world twice: first mono through the
				// scope camera (flags 3 and 4 set), then the stereo view. The field advances
				// on the stereo one only.
				const auto* flags = static_cast<const std::uint8_t*>(a_renderer);
				if (!(flags && flags[3] && flags[4])) {
					FrameUpdate();
				}
				func(a_renderer);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Kicked snow is drawn once the forward pass is done, into the scene target and
		// depth it leaves bound.
		struct Forward
		{
			static void thunk()
			{
				func();
				SnowSparkle::Render();
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Each frame DrawWorld::RenderDeferredDecals copies every decal into its shader input;
		// right after the copy a static decal on the landscape is fitted to the deformed ground
		// there. Flat screens only: the pass runs on the frame's own thread, after the field
		// update, so the land and snow queries see the frame they were made for.
		struct DecalInputSet
		{
			static void thunk(void* a_input, void* a_decal, bool a_light)
			{
				func(a_input, a_decal, a_light);
				GroundDecals::Fit(static_cast<float*>(a_input));
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool IsCall(std::uintptr_t a_address)
		{
			return a_address && *reinterpret_cast<const std::uint8_t*>(a_address) == 0xE8;
		}

		template <class T>
		bool WriteCall(std::uintptr_t a_site, const char* a_what)
		{
			if (!IsCall(a_site)) {
				logger::critical("{}: expected a call at {:X}, found {:02X}. This build does not fit "
								 "this game version; nothing was patched there.",
					a_what, a_site, a_site ? *reinterpret_cast<const std::uint8_t*>(a_site) : 0);
				return false;
			}
			// The original target is stored before the call is patched, so a thread that
			// reaches the call the moment it changes already finds it.
			const auto displacement = *reinterpret_cast<const std::int32_t*>(a_site + 1);
			T::func = REL::Relocation<decltype(T::thunk)>{ a_site + 5 + static_cast<std::intptr_t>(displacement) };
			F4SE::GetTrampoline().write_call<5>(a_site, T::thunk);
			logger::info("Hooked {} (call site {:X})", a_what, a_site);
			return true;
		}

		std::uintptr_t CallTarget(std::uintptr_t a_site)
		{
			return a_site + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(a_site + 1));
		}

		// VR creates landscape textures without names, so the surface classifier cannot read
		// a layer's texture path from the texture. Where TESObjectLAND::CreateGeometry gives a
		// layer its texture set, record the texture the setter stored and the set's diffuse path.
		struct LandscapeTextureSet
		{
			static void thunk(RE::BSShaderProperty* a_property, std::uint32_t a_layer, void* a_textureSet,
				void* a_shaderData)
			{
				func(a_property, a_layer, a_textureSet, a_shaderData);
				if (!a_property || !a_textureSet || a_layer >= 4) {
					return;
				}
				const auto* set = reinterpret_cast<const RE::BGSTextureSet*>(
					reinterpret_cast<std::uintptr_t>(a_textureSet) - Offsets::kTextureSetBase);
				Surfaces::NoteLayerTexture(GameLayouts::LandscapeLayerDiffuse(MaterialOf(a_property), a_layer),
					GameLayouts::DiffusePath(set));
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <class T>
		void WriteVfunc(std::uintptr_t a_vtable, std::size_t a_slot)
		{
			T::func = REL::Relocation<decltype(T::thunk)>{ reinterpret_cast<const std::uintptr_t*>(a_vtable)[a_slot] };
			REL::Relocation<std::uintptr_t> table{ a_vtable };
			table.write_vfunc(a_slot, T::thunk);
		}

		bool g_installed{ false };
	}

	void RequestWorldReset()
	{
		g_worldResetRequested.store(true);
	}

	void NoteDrawContext(ID3D11DeviceContext* a_context)
	{
		if (g_inMainPrepass.load(std::memory_order_acquire)) {
			try {
				IndoorPrints::NoteGBuffer(a_context);
			} catch (const std::exception& e) {
				ReportException("Floor print target capture", e);
			}
		}
	}

	void InstallLandscapeTextureRecorder()
	{
		// Flat runtimes name every landscape texture; only VR needs the record.
		if (!REL::Module::IsVR()) {
			return;
		}
		const auto function = Offsets::Address(Offsets::LandCreateGeometry);
		const auto target = Offsets::Address(Offsets::SetLandscapeTextureSet);
		for (const auto offset : Offsets::kVRLandTextureSetCalls) {
			const auto site = function ? function + offset : 0;
			if (!function || !target || !IsCall(site) || CallTarget(site) != target) {
				logger::warn("VR landscape layers: TESObjectLAND::CreateGeometry does not call "
							 "SetLandscapeTextureSet where verified; layers stay unclassified");
				return;
			}
		}
		for (const auto offset : Offsets::kVRLandTextureSetCalls) {
			WriteCall<LandscapeTextureSet>(function + offset, "SetLandscapeTextureSet (VR layer textures)");
		}
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		g_installed = true;

		if (!ShaderRegistry::DrawHookInstalled()) {
			logger::critical("The context draw hook is not in place, so there is nothing to route "
							 "terrain through - deformation is off.");
			return;
		}

		const auto vtable = Offsets::Address(Offsets::PrePassShaderVtbl);
		const auto renderPreUI = Offsets::Address(Offsets::RenderPreUI);
		if (!vtable || !renderPreUI) {
			logger::critical("The G-buffer pass or the frame hook has no verified address on this "
							 "game version - deformation is off. Nothing was patched.");
			return;
		}

		const bool frameHooked = REL::Module::IsVR() ?
		                             WriteCall<VRRendererUnlock>(renderPreUI + Offsets::VRRendererUnlockCall(),
										 "BSGraphics::Renderer::Unlock in Render_PreUI (VR field update)") :
		                             WriteCall<DeferredPrePass>(renderPreUI + Offsets::DeferredPrePassCall(),
										 "DrawWorld::DeferredPrePass (field update)");
		if (!frameHooked) {
			return;
		}

		WriteVfunc<PrePass_SetupGeometry>(vtable, Offsets::PrePassSetupGeometrySlot());
		WriteVfunc<PrePass_RestoreGeometry>(vtable, Offsets::PrePassRestoreGeometrySlot());
		logger::info("Hooked BSDFPrePassShader::SetupGeometry/RestoreGeometry (vtable {:X})", vtable);
		if (REL::Module::IsVR()) {
			const auto site = Offsets::Address(Offsets::VRBatchSubmitCall);
			const auto expected = Offsets::Address(Offsets::VRBatchSubmit);
			if (!IsCall(site) || site + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(site + 1)) != expected) {
				logger::critical("VR queued terrain: submit call does not match the verified target; routing is off");
				return;
			}
			if (!WriteCall<VRBatchSubmit>(site, "VR queued terrain fallback")) return;
		}

		if (const auto registerObject = Offsets::Address(Offsets::RegisterObjectStandard)) {
			WriteCall<RegisterForZPrePass>(registerObject + Offsets::RegisterForZPrePassCall(),
				"BSGraphics::Renderer::RegisterForZPrePass");
		} else {
			logger::warn("The depth pre-pass registration has no verified address on this game "
						 "version; footprints may be hidden by the pre-pass depth when "
						 "bZPrePass is on.");
		}

		// VR draws the forward pass on its submission thread while the main thread runs on,
		// so the snow spray (flat screen only anyway) is not hooked there. On flat screens
		// the hook is always in place and the spray follows the SnowSparkle setting live.
		if (!REL::Module::IsVR()) {
			if (WriteCall<Forward>(renderPreUI + Offsets::ForwardCall(), "DrawWorld::Forward (kicked snow)")) {
				SnowSparkle::NoteHookInstalled(true);
				if (Settings::snowSparkle) {
					SnowSparkle::Initialize();
				}
			}
		}

		if (REL::Module::IsVR()) {
			logger::info("Snow on objects: off in VR, which queues its G-buffer pass for the submission thread");
		}

		// Blood, scorch marks and bullet holes follow the deformed ground (EnableBloodDecals,
		// read live). VR draws its decals on the render-submission thread while the main thread
		// runs on, where the land and snow queries are not safe, so they stay as the game lays them.
		if (REL::Module::IsVR()) {
			logger::info("Ground decals: VR draws decals on its submission thread; blood and other "
						 "ground decals keep the game's own placement there");
		} else {
			const auto render = Offsets::Address(Offsets::RenderDeferredDecals);
			const auto setter = Offsets::Address(Offsets::DecalInputSet);
			const auto site = render ? render + Offsets::DecalInputSetCall() : 0;
			if (render && setter && IsCall(site) && CallTarget(site) == setter) {
				WriteCall<DecalInputSet>(site, "DrawWorld::ComputeShaderDecalsInput::Set (ground decals)");
			} else {
				logger::warn("Ground decals: RenderDeferredDecals does not call the decal input setter "
							 "where verified on this game version; blood and other ground decals keep "
							 "the game's own placement");
			}
		}

		// Without any recorded vertex shader nothing can be routed, and taking near terrain
		// out of the depth pre-pass would only cost overdraw.
		if (ShaderRegistry::Count() == 0) {
			logger::error("No vertex shader bytecode was recorded - the terrain stays vanilla");
			return;
		}
		g_routing = true;
	}
}
