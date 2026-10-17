// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Globals.h"
#include "ImpactPatterns.h"
#include "Impacts.h"
#include "Offsets.h"
#include "Physics.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowMeshCover.h"
#include "SurfaceProfiles.h"
#include "Weather.h"

#include <chrono>

namespace Impacts
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		enum class Kind
		{
			kExplosion,
			kShot,
		};

		struct Event
		{
			Kind              kind{};
			RE::NiPoint3      position{};
			RE::NiPoint3      normal{};
			RE::TESFormID     source{};
			RE::TESFormID     space{};
			float             radius{};
			Clock::time_point time{};
			// A shot that struck a static mesh rather than the landscape: it only marks the
			// snow over a road or path the blanket buries (Build).
			bool              onMesh{};
		};

		std::array<Event, 128> g_queue{};
		size_t                 g_head{};
		size_t                 g_count{};
		std::mutex             g_lock;
		std::atomic<bool>      g_explosions{ false };
		std::atomic<bool>      g_shots{ false };
		std::atomic<uint32_t>  g_logged{};
		std::atomic<uint32_t>  g_callbacksLogged{};
		std::atomic<uint32_t>  g_samplesLogged{};

		// The collision layer of Fallout 4's landscape (TESObjectCELL::Pick maps a hit
		// on it to the cell's terrain).
		constexpr std::uint32_t kTerrainLayer = 0x11;
		constexpr std::uint32_t kStaticLayer = 0x01;
		constexpr std::uint32_t kAnimStaticLayer = 0x02;

		// ImpactCreation, as Projectile::AddImpact copies it into an ImpactData on
		// 1.10.163: contact point at +0x00, normal at +0x10, collision filter at +0x28.
		struct ImpactView
		{
			RE::NiPoint3  point;
			RE::NiPoint3  normal;
			std::uint32_t layer;
		};

		ImpactView ViewImpact(const void* a_creation)
		{
			const auto* base = static_cast<const std::byte*>(a_creation);
			ImpactView  view{};
			std::memcpy(&view.point, base + 0x00, sizeof(RE::NiPoint3));
			std::memcpy(&view.normal, base + 0x10, sizeof(RE::NiPoint3));
			view.layer = *reinterpret_cast<const std::uint32_t*>(base + 0x28) & 0x7F;
			return view;
		}

		RE::TESFormID Space(RE::TESObjectREFR* a_ref)
		{
			auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell) {
				return 0;
			}
			auto* world = cell->worldSpace;
			return (cell->IsExterior() && world) ? world->GetFormID() : cell->GetFormID();
		}

		bool Finite(const RE::NiPoint3& a_point)
		{
			return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
		}

		void Queue(Event a_event)
		{
			if (!a_event.space || !Finite(a_event.position)) {
				return;
			}
			a_event.time = Clock::now();

			const std::scoped_lock lock(g_lock);

			// One burst of the same weapon at the same spot is one mark.
			for (size_t i = 0; i < g_count; ++i) {
				const auto& previous = g_queue[(g_head + i) % g_queue.size()];
				if (previous.kind == a_event.kind && previous.source == a_event.source &&
					a_event.time - previous.time < std::chrono::milliseconds(50) &&
					previous.position.GetSquaredDistance(a_event.position) < 4.0f) {
					return;
				}
			}
			if (g_count == g_queue.size()) {
				return;
			}
			g_queue[(g_head + g_count++) % g_queue.size()] = a_event;
		}

		struct ExplosionInitialize
		{
			static void thunk(RE::TESObjectREFR* a_explosion)
			{
				func(a_explosion);

				if (!g_explosions.load(std::memory_order_relaxed) || !a_explosion) {
					return;
				}

				auto* base = a_explosion->GetObjectReference();
				if (!base || !base->Is(RE::ENUM_FORM_ID::kEXPL)) {
					return;
				}
				const auto* explosion = static_cast<const RE::BGSExplosion*>(base);
				const float radius = std::max(explosion->data.outerRadius, explosion->data.innerRadius);
				if (!(radius > 1.0f)) {
					return;
				}

				Queue({ Kind::kExplosion, a_explosion->data.location, RE::NiPoint3{ 0.0f, 0.0f, 1.0f },
					base->GetFormID(), Space(a_explosion), radius });
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <std::size_t Tag>
		struct AddImpact
		{
			static std::uint32_t thunk(RE::Projectile* a_projectile, const void* a_creation)
			{
				// Log delivery before terrain filtering. A missing stamp alone cannot tell a
				// missed hook from a hit on a static mesh or a point above the landscape.
				if (Settings::logImpacts && a_projectile && a_creation &&
					g_callbacksLogged.load() < 32 && g_callbacksLogged.fetch_add(1) < 32) {
					const auto impact = ViewImpact(a_creation);
					auto* base = a_projectile->GetObjectReference();
					logger::info("Impact callback tag={} projectile={:08X} source={:08X} space={:08X} "
						"layer={:02X} point=({:.1f},{:.1f},{:.1f}) normal=({:.2f},{:.2f},{:.2f}) enabled={}",
						Tag, a_projectile->GetFormID(), base ? base->GetFormID() : 0, Space(a_projectile),
						impact.layer, impact.point.x, impact.point.y, impact.point.z,
						impact.normal.x, impact.normal.y, impact.normal.z, g_shots.load());
				}
				const auto result = func(a_projectile, a_creation);

				if (g_shots.load(std::memory_order_relaxed) && a_projectile && a_creation) {
					const auto impact = ViewImpact(a_creation);
					const bool onMesh = impact.layer == kStaticLayer || impact.layer == kAnimStaticLayer;
					if (impact.layer == kTerrainLayer || (onMesh && Settings::snowCoverLowMeshes)) {
						auto* base = a_projectile->GetObjectReference();
						Event event{ Kind::kShot, impact.point, impact.normal, base ? base->GetFormID() : 0,
							Space(a_projectile), 0.0f };
						event.onMesh = onMesh;
						Queue(event);
					}
				}

				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool ClearPath(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			if (!a_cell) {
				return true;
			}
			const float hit = Physics::CastRay(a_cell, a_from, a_to);
			return hit < 0.0f || hit >= 0.98f;
		}

		void Build(const Event& a_event, const RE::NiPoint3& a_anchor, std::vector<Clipmap::Stamp>& a_out)
		{
			auto* tes = globals::TES();
			auto* player = globals::game::player;
			if (!tes || !player || a_event.space != Space(player) ||
				a_anchor.GetSquaredDistance(a_event.position) > 2800.0f * 2800.0f) {
				return;
			}

			float landZ{};
			if (!tes->GetLandHeight(a_event.position, landZ)) {
				return;
			}

			const float above = a_event.position.z - landZ;
			if (Settings::logImpacts && g_samplesLogged.load() < 32 && g_samplesLogged.fetch_add(1) < 32) {
				const auto ground = Surfaces::GroundAt(a_event.position, landZ);
				logger::info("Impact ground sample: kind={} source={:08X} point=({:.1f},{:.1f},{:.1f}) "
					"landZ={:.1f} above={:.1f} surface={} depthScale={:.2f}",
					a_event.kind == Kind::kExplosion ? "explosion" : "shot", a_event.source,
					a_event.position.x, a_event.position.y, a_event.position.z, landZ, above,
					Surfaces::Name(ground.type), ground.response.depthScale);
			}

			float                   reach = 0.0f;
			float                   depth = 0.0f;
			ImpactPatterns::Pattern pattern{};

			if (a_event.kind == Kind::kExplosion) {
				reach = ImpactPatterns::BoundedRadius(a_event.radius, Settings::explosionImpactRadiusScale,
					Settings::explosionImpactMaxRadius);
				if (!(reach > 1.0f) || above < -16.0f || above >= reach) {
					return;
				}
				// A blast above the ground reaches it with a smaller circle.
				reach = std::sqrt(std::max(0.0f, reach * reach - above * above));
				depth = Settings::explosionImpactDepth;
				pattern = ImpactPatterns::Explosion(reach);
			} else {
				if (!a_event.onMesh && (above < -16.0f || above > 16.0f)) {
					return;
				}
				// A shot into a static mesh marks only the snow over a road or path the
				// blanket buries, where it struck the buried mesh's top.
				if (a_event.onMesh) {
					const float buried = Shelter::BuriedAt(a_event.position.x, a_event.position.y);
					if (!SnowMeshCover::HitsBuriedTop(above, buried) ||
						Surfaces::GroundAt(a_event.position, landZ).type != Surfaces::Type::kSnow) {
						return;
					}
				}
				reach = Settings::projectileImpactRadius;
				depth = Settings::projectileImpactDepth;
				pattern.strokes[pattern.count++] = { 0.0f, 0.0f, 0.0f, 0.0f, reach, 1.0f };
			}

			auto* cell = player->GetParentCell();

			const size_t before = a_out.size();
			for (size_t i = 0; i < pattern.count && a_out.size() < Clipmap::kMaxStamps; ++i) {
				const auto&  stroke = pattern.strokes[i];
				RE::NiPoint3 point{ a_event.position.x + stroke.x, a_event.position.y + stroke.y, landZ };

				float groundZ{};
				if (!tes->GetLandHeight(point, groundZ)) {
					continue;
				}
				point.z = groundZ + 3.0f;

				const auto  ground = Surfaces::GroundAt(point, groundZ);
				const auto& response = ground.response;

				if (a_event.kind == Kind::kExplosion) {
					// Under the snow blanket a buried road's top is the ground the blast must
					// reach; the land under the road would read as blocked by it.
					if (ground.type == Surfaces::Type::kSnow) {
						point.z += Shelter::BuriedAt(point.x, point.y);
					}
					auto origin = a_event.position;
					origin.z += 8.0f;
					if (!ClearPath(cell, origin, point)) {
						continue;
					}
				}

				if (ground.type == Surfaces::Type::kUnknown || ground.type == Surfaces::Type::kStone ||
					response.depthScale <= 0.0f) {
					continue;
				}

				Clipmap::Stamp stamp{};
				stamp.snow = ground.type == Surfaces::Type::kSnow;
				const float span = stamp.snow && Settings::snowStampRimSpan >= 0.0f ? Settings::snowStampRimSpan :
				                                                                     Settings::stampRimSpan;
				const float lean = stamp.snow && Settings::snowStampRimLean >= 0.0f ? Settings::snowStampRimLean :
				                                                                     Settings::stampRimLean;
				stamp.x = point.x;
				stamp.y = point.y;
				stamp.radius = ImpactPatterns::StampRadius(stroke.radius, response.radiusScale, span, lean);
				if (stamp.radius <= 0.5f) {
					continue;
				}
				stamp.motionX = stroke.motionX;
				stamp.motionY = stroke.motionY;
				stamp.depth = std::clamp(depth * stroke.strength * response.depthScale, 0.0f, 64.0f);
				stamp.rim = stamp.depth * Settings::impactRimScale * std::clamp(response.rimScale, 0.0f, 2.0f);

				// A blast in snow throws the blanket off down to the ground.
				if (stamp.snow && a_event.kind == Kind::kExplosion) {
					stamp.depth = std::max(stamp.depth,
						Weather::SnowDepth() * Settings::impactSnowMelt * stroke.strength);
				}
				stamp.depth = std::clamp(stamp.depth, 0.0f, 64.0f);
				stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.5f);
				stamp.decay = Clipmap::RecoveryRate(Settings::stampDecayPerSecond,
					response.decayScale * Weather::DecayScale());
				a_out.push_back(stamp);
			}

			if (Settings::logImpacts && g_logged < 32 && a_out.size() > before) {
				++g_logged;
				logger::info("Impact {} source={:08X} reach={:.1f} stamps={}",
					a_event.kind == Kind::kExplosion ? "explosion" : "shot", a_event.source, reach,
					a_out.size() - before);
			}
		}

		template <class T>
		bool HookVfunc(const REL::VariantID& a_vtable, std::size_t a_slot, const char* a_what)
		{
			const auto vtable = Offsets::Address(a_vtable);
			if (!vtable) {
				logger::info("{}: no verified address on this game version, not hooked", a_what);
				return false;
			}
			REL::Relocation<std::uintptr_t> table{ vtable };
			T::func = table.write_vfunc(a_slot, T::thunk);
			return true;
		}
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_head = g_count = 0;
		g_logged = 0;
		g_callbacksLogged = 0;
		g_samplesLogged = 0;
		g_explosions.store(Settings::enableExplosionImpacts, std::memory_order_relaxed);
		g_shots.store(Settings::enableProjectileImpacts, std::memory_order_relaxed);
	}

	void Append(const RE::NiPoint3& a_anchor, std::vector<Clipmap::Stamp>& a_out)
	{
		// An explosion's pattern can take a full pattern of stamps; an event is only taken
		// off the queue with room for all of it, so one that does not fit waits for the
		// next frame instead of being lost. Impacts are appended first and keep their
		// stamps within the field's own budget, which the gatherer never trims.
		constexpr size_t kMaxEventsPerFrame = 8;
		constexpr size_t kPatternStamps = std::tuple_size_v<decltype(ImpactPatterns::Pattern::strokes)>;

		for (size_t taken = 0; taken < kMaxEventsPerFrame && a_out.size() + kPatternStamps <= Clipmap::kMaxStamps;
			 ++taken) {
			Event event{};
			bool  found = false;
			{
				const std::scoped_lock lock(g_lock);
				while (g_count) {
					event = g_queue[g_head];
					g_head = (g_head + 1) % g_queue.size();
					--g_count;
					if (Clock::now() - event.time < std::chrono::seconds(1)) {
						found = true;
						break;
					}
				}
			}
			if (!found) {
				return;
			}

			const auto start = Profiler::Ticks();
			Build(event, a_anchor, a_out);
			Profiler::AddCpuTicks(Profiler::CpuScope::kImpacts, Profiler::Ticks() - start);
		}
	}

	void Install()
	{
		Reset();

		const bool explosions = HookVfunc<ExplosionInitialize>(Offsets::ExplosionVtbl,
			Offsets::ExplosionInitializeSlot(), "Explosion::Initialize");
		const bool missiles = HookVfunc<AddImpact<0>>(Offsets::MissileProjectileVtbl,
			Offsets::ProjectileAddImpactSlot(), "MissileProjectile::AddImpact");
		const bool beams = HookVfunc<AddImpact<1>>(Offsets::BeamProjectileVtbl,
			Offsets::ProjectileAddImpactSlot(), "BeamProjectile::AddImpact");
		const bool arrows = HookVfunc<AddImpact<2>>(Offsets::ArrowProjectileVtbl,
			Offsets::ProjectileAddImpactSlot(), "ArrowProjectile::AddImpact");

		logger::info("Impacts: explosions {}, bullets {}, lasers {}, arrows {}",
			explosions ? "hooked" : "off", missiles ? "hooked" : "off", beams ? "hooked" : "off",
			arrows ? "hooked" : "off");
	}
}
