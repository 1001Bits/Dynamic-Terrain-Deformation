// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Corpses.h"

#include "CorpseRule.h"
#include "FootGeometry.h"
#include "GameLayouts.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "SnowSurface.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace Corpses
{
	namespace
	{
		std::mutex g_lock;

		// In the snow a part's hollow reaches its full depth over the part itself and slopes
		// out to this many of its radii (the edge of the snow it fell through), close around
		// the body; a print in other ground is a little wider than the part.
		constexpr float kSnowSpread = 1.3f;
		constexpr float kSnowShoulder = 1.0f / kSnowSpread;
		constexpr float kPrintSpread = 1.2f;
		constexpr float kPrintShoulder = 0.6f;

		// A print in ground other than snow: this share of a footprint's depth, at most
		// kMaxBodyPrint units, and only under a body lying within kRestingHeight of the land
		// (not on a table, a porch or a car).
		constexpr float kBodyPrintShare = 0.5f;
		constexpr float kMaxBodyPrint = 4.0f;
		constexpr float kRestingHeight = 16.0f;

		// The longest gap between two stampings the strength makes up for.
		constexpr float kMaxElapsed = 2.0f;

		// Bodies not seen for this many frames are forgotten.
		constexpr uint64_t kForgetFrames = 3600;

		struct Tracked
		{
			CorpseRule::Settle settle{};
			RE::NiPoint3       last{};
			bool               haveLast{ false };
			RE::NiPoint3       settledAt{};

			std::array<CorpseRule::Capsule, CorpseRule::kMaxCapsules> capsules{};
			std::array<float, CorpseRule::kMaxCapsules>                landZ{};
			size_t                                                     count{ 0 };
			bool                                                       built{ false };
			bool                                                       named{ false };

			bool  onSnow{ false };
			float decay{ 0.0f };
			float printDepth{ 0.0f };
			float printRim{ 0.0f };
			float printShoulder{ 0.3f };

			float    burial{ 0.0f };
			int      loggedQuarter{ 0 };
			float    lastStamp{ -1.0e9f };
			uint64_t seen{ 0 };
		};

		std::unordered_map<RE::TESFormID, Tracked> g_tracked;
		float                                      g_clock{ 0.0f };
		uint64_t                                   g_frame{ 0 };
		size_t                                     g_cursor{ 0 };

		std::atomic<uint32_t> g_logged{ 0 };
		constexpr uint32_t    kMaxLogged = 24;

		// Whether one more of the capped corpse lines may be written.
		bool Logging()
		{
			return Settings::logCorpses && g_logged.load() < kMaxLogged && g_logged.fetch_add(1) < kMaxLogged;
		}

		CorpseRule::Point ToPoint(const RE::NiPoint3& a_point)
		{
			return { a_point.x, a_point.y, a_point.z };
		}

		// The bones the limb chains name, first of each name down the skeleton. Another
		// mod's copy of the skeleton (True First Person's shadow twin) is not the body's.
		void CollectJoints(RE::NiAVObject* a_object, std::vector<CorpseRule::Bone>& a_out, uint32_t a_depth = 0)
		{
			if (!a_object || a_depth > 96 || a_out.size() >= 48) {
				return;
			}
			auto* node = a_object->IsNode();
			if (!node) {
				return;
			}
			const std::string_view name = a_object->name.c_str();
			if (FootGeometry::IsCloneNode(name)) {
				return;
			}
			if (CorpseRule::IsJointName(name)) {
				a_out.push_back({ name, ToPoint(a_object->world.translate) });
			}
			for (auto& child : GameLayouts::Children(node)) {
				if (child) {
					CollectJoints(child.get(), a_out, a_depth + 1);
				}
			}
		}

		// The body's parts where it lies, and what the ground under it does with them. a_resting:
		// where it came to rest (logged); otherwise where it is this frame, still falling.
		void Build(RE::Actor* a_actor, RE::NiAVObject* a_root, Tracked& a_tracked, bool a_resting = true)
		{
			a_tracked.built = true;
			a_tracked.count = 0;
			a_tracked.burial = 0.0f;
			a_tracked.loggedQuarter = 0;
			a_tracked.lastStamp = -1.0e9f;

			static std::vector<CorpseRule::Bone> bones;
			bones.clear();
			CollectJoints(a_root, bones);

			auto&  capsules = a_tracked.capsules;
			size_t count = CorpseRule::CapsulesFromBones(bones, capsules);
			a_tracked.named = count >= CorpseRule::kMinNamedCapsules;
			if (!a_tracked.named) {
				const auto& bound = a_root->worldBound;
				if (!(bound.fRadius > 1.0f)) {
					return;
				}
				capsules[0] = CorpseRule::CapsuleFromBound(ToPoint(bound.center), bound.fRadius);
				count = 1;
			}
			bones.clear();

			auto* tes = globals::TES();
			if (!tes) {
				return;
			}
			const auto&        first = capsules[0];
			const RE::NiPoint3 middle{ 0.5f * (first.a.x + first.b.x), 0.5f * (first.a.y + first.b.y),
				0.5f * (first.a.z + first.b.z) };
			float landZ = middle.z;
			if (!tes->GetLandHeight(middle, landZ)) {
				return;
			}

			const auto  ground = Surfaces::GroundAt(middle, landZ);
			const auto& response = ground.response;
			a_tracked.onSnow = ground.type == Surfaces::Type::kSnow && SnowSurface::LiftAt(middle.x, middle.y) > 1.0f;
			a_tracked.decay = Clipmap::RecoveryRate(Settings::stampDecayPerSecond,
				response.decayScale * Weather::DecayScale());

			float lowest = std::numeric_limits<float>::max();
			for (size_t i = 0; i < count; ++i) {
				const auto&        capsule = capsules[i];
				const RE::NiPoint3 at{ 0.5f * (capsule.a.x + capsule.b.x), 0.5f * (capsule.a.y + capsule.b.y),
					0.5f * (capsule.a.z + capsule.b.z) };
				float z = landZ;
				a_tracked.landZ[i] = tes->GetLandHeight(at, z) ? z : landZ;
				lowest = std::min(lowest,
					std::min(capsule.a.z, capsule.b.z) - capsule.radius - a_tracked.landZ[i]);
			}

			if (a_tracked.onSnow) {
				// A body lying above the snow (on a porch) has nothing to clear.
				bool clears = false;
				for (size_t i = 0; i < count && !clears; ++i) {
					const auto& capsule = capsules[i];
					const float lift = SnowSurface::LiftAt(0.5f * (capsule.a.x + capsule.b.x),
						0.5f * (capsule.a.y + capsule.b.y));
					clears = CorpseRule::ClearingDepth(capsule, a_tracked.landZ[i], lift, Settings::snowGroundBite) > 1.0f;
				}
				if (!clears) {
					if (a_resting && Logging()) {
						logger::info("Corpse {:08X} at rest at ({:.0f}, {:.0f}) above the snow: no hollow", a_actor->GetFormID(),
							middle.x, middle.y);
					}
					return;
				}
			} else {
				if (response.depthScale <= 0.0f || lowest > kRestingHeight) {
					if (a_resting && Logging()) {
						logger::info("Corpse {:08X} at rest at ({:.0f}, {:.0f}) on {}{}: no print", a_actor->GetFormID(), middle.x,
							middle.y, Surfaces::Name(ground.type), lowest > kRestingHeight ? " above the land" : "");
					}
					return;
				}
				a_tracked.printDepth = std::min(Settings::stampDepth * response.depthScale * Weather::DepthScale() *
				                                    kBodyPrintShare,
					kMaxBodyPrint);
				a_tracked.printRim = Surfaces::RimHeight(a_tracked.printDepth, response.rimScale) * 0.5f;
				a_tracked.printShoulder = std::clamp(response.shoulder, kPrintShoulder, 0.95f);
			}

			a_tracked.count = count;
			if (a_resting && Logging()) {
				logger::info("Corpse {:08X} at rest at ({:.0f}, {:.0f}) on {}: {} body part(s) ({})", a_actor->GetFormID(), middle.x,
					middle.y, a_tracked.onSnow ? "snow - a hollow down to it" : Surfaces::Name(ground.type), count,
					a_tracked.named ? "from its bones" : "its bound, bones not known");
			}
		}

		// The body's stamps. In the snow a fresh hollow is pressed at once; once snow has
		// begun to fall on it the field is drawn to the hollow's floor as it rises, then to the
		// mound (the melt kind, which moves a texel either way). Elsewhere a shallow press.
		void Emit(const Tracked& a_tracked, float a_strength, std::vector<Clipmap::Stamp>& a_out)
		{
			for (size_t i = 0; i < a_tracked.count; ++i) {
				const auto&    capsule = a_tracked.capsules[i];
				Clipmap::Stamp stamp{};
				stamp.x = capsule.b.x;
				stamp.y = capsule.b.y;
				stamp.motionX = capsule.b.x - capsule.a.x;
				stamp.motionY = capsule.b.y - capsule.a.y;
				stamp.decay = a_tracked.decay;

				if (a_tracked.onSnow) {
					const float lift = SnowSurface::LiftAt(0.5f * (capsule.a.x + capsule.b.x),
						0.5f * (capsule.a.y + capsule.b.y));
					const float clearing =
						CorpseRule::ClearingDepth(capsule, a_tracked.landZ[i], lift, Settings::snowGroundBite);
					stamp.radius = capsule.radius * kSnowSpread;
					stamp.shoulder = kSnowShoulder;
					stamp.snow = true;
					if (a_tracked.burial <= 0.0f) {
						stamp.kind = Clipmap::Stamp::Kind::kPress;
						stamp.depth = clearing;
						stamp.rimMargin = -1.0f;
					} else {
						stamp.kind = Clipmap::Stamp::Kind::kMelt;
						stamp.depth = a_strength;
						stamp.rim = CorpseRule::FloorFor(a_tracked.burial, clearing, Settings::corpseMoundHeight);
					}
				} else {
					stamp.kind = Clipmap::Stamp::Kind::kPress;
					stamp.radius = capsule.radius * kPrintSpread;
					stamp.shoulder = a_tracked.printShoulder;
					stamp.depth = a_tracked.printDepth;
					stamp.rim = a_tracked.printRim;
					stamp.rimMargin = -1.0f;
				}
				a_out.push_back(stamp);
			}
		}
	}

	bool Supported()
	{
		// IsDead reads the life state at Actor+0x130 on OG, NG and 1.11.240. VR's code never
		// masks or shifts those bits that way, so its layout is not known: off there.
		return !REL::Module::IsVR();
	}

	bool IsDead(const RE::Actor* a_actor)
	{
		if (!a_actor || !Supported()) {
			return false;
		}
		std::uint32_t word = 0;
		std::memcpy(&word, reinterpret_cast<const std::byte*>(a_actor) + CorpseRule::kLifeStateOffset, sizeof(word));
		return CorpseRule::IsCorpseState(CorpseRule::LifeStateOf(word));
	}

	bool InMeleeAttack(const RE::Actor* a_actor)
	{
		if (!a_actor || !Supported()) {
			return false;
		}
		std::uint32_t word = 0;
		std::memcpy(&word, reinterpret_cast<const std::byte*>(a_actor) + CorpseRule::kLifeStateOffset, sizeof(word));
		return CorpseRule::MeleeAttackOf(word) != 0;
	}

	size_t BodyCapsules(RE::Actor* a_actor, std::array<CorpseRule::Capsule, CorpseRule::kMaxCapsules>& a_out)
	{
		auto* root = a_actor ? globals::Loaded3D(a_actor) : nullptr;
		if (!root) {
			return 0;
		}
		static std::vector<CorpseRule::Bone> bones;
		bones.clear();
		CollectJoints(root, bones);
		size_t count = CorpseRule::CapsulesFromBones(bones, a_out);
		bones.clear();
		if (count < CorpseRule::kMinNamedCapsules) {
			const auto& bound = root->worldBound;
			if (!(bound.fRadius > 1.0f)) {
				return 0;
			}
			a_out[0] = CorpseRule::CapsuleFromBound(ToPoint(bound.center), bound.fRadius);
			count = 1;
		}
		return count;
	}

	void Append(float a_deltaSeconds, const RE::NiPoint3& a_anchor, std::span<RE::Actor* const> a_dead,
		std::vector<Clipmap::Stamp>& a_out)
	{
		if (!Supported()) {
			static bool told = false;
			if (Settings::corpseMarks && !std::exchange(told, true)) {
				logger::info("Corpses: off on VR (the actor's life state is not verified there)");
			}
			return;
		}
		if (!Settings::corpseMarks || Settings::maxCorpseMarks <= 0) {
			return;
		}

		const std::scoped_lock lock(g_lock);
		++g_frame;
		const float dt = std::max(a_deltaSeconds, 0.0f);
		g_clock += dt;

		const float snowfall = Settings::corpseBurial ? Weather::Snowfall() : 0.0f;
		const float reachSq = Clipmap::kWorldSize * 0.375f * Clipmap::kWorldSize * 0.375f;

		static std::vector<Tracked*> live;
		live.clear();
		for (auto* actor : a_dead) {
			if (live.size() >= static_cast<size_t>(Settings::maxCorpseMarks)) {
				break;
			}
			auto* root = actor ? globals::Loaded3D(actor) : nullptr;
			if (!root || a_anchor.GetSquaredDistance(actor->GetPosition()) > reachSq) {
				continue;
			}

			auto& tracked = g_tracked[actor->GetFormID()];
			tracked.seen = g_frame;

			const RE::NiPoint3 centre = root->worldBound.center;
			const float        moved = tracked.haveLast ? std::sqrt(centre.GetSquaredDistance(tracked.last)) :
			                                              std::numeric_limits<float>::max();
			const float        drift = tracked.settle.settled ? std::sqrt(centre.GetSquaredDistance(tracked.settledAt)) : 0.0f;
			const bool         wasSettled = tracked.settle.settled;
			CorpseRule::UpdateSettle(tracked.settle, moved, drift, dt);
			tracked.last = centre;
			tracked.haveLast = true;

			if (wasSettled && !tracked.settle.settled) {
				tracked.built = false;
				tracked.count = 0;
				if (Logging()) {
					logger::info("Corpse {:08X} moved {:.0f} units: marked again where it comes to rest", actor->GetFormID(), drift);
				}
			}
			if (!tracked.settle.settled) {
				// Still falling or sliding: the parts touching the ground press it at once,
				// where they are this frame; the full hollow (and any burial) follows once
				// the body lies still.
				static Tracked                     falling;
				static std::vector<Clipmap::Stamp> pressed;
				falling = {};
				Build(actor, root, falling, false);
				pressed.clear();
				Emit(falling, 0.0f, pressed);
				for (const auto& stamp : pressed) {
					if (a_out.size() >= Clipmap::kMaxStamps) {
						break;
					}
					if (stamp.depth > 0.5f) {
						a_out.push_back(stamp);
					}
				}
				continue;
			}
			if (!tracked.built) {
				tracked.settledAt = centre;
				Build(actor, root, tracked);
			}
			if (tracked.count == 0) {
				continue;
			}

			// The first submitted mark must clear the hollow even when it is already
			// snowing. Waiting for stamp-budget room must not skip straight to burial.
			if (tracked.onSnow && Settings::corpseBurial && tracked.lastStamp >= 0.0f) {
				tracked.burial = CorpseRule::AdvanceBurial(tracked.burial, snowfall, dt, Settings::corpseBurialSeconds);
				const int quarter = static_cast<int>(tracked.burial * 4.0f + 1e-4f);
				if (quarter > tracked.loggedQuarter) {
					tracked.loggedQuarter = quarter;
					if (Logging()) {
						logger::info("Corpse {:08X} {}% buried ({}; snowfall {:.2f})", actor->GetFormID(), quarter * 25,
							quarter <= 2 ? "the hollow filling in" : "the mound growing", snowfall);
					}
				}
			}
			live.push_back(&tracked);
		}

		for (auto it = g_tracked.begin(); it != g_tracked.end();) {
			it = it->second.seen + kForgetFrames < g_frame ? g_tracked.erase(it) : std::next(it);
		}
		if (live.empty()) {
			return;
		}

		// Corpses only take what the feet, impacts and objects left of the field's budget, so
		// the gatherer never has to cut one of those for a body.
		const size_t room = a_out.size() < Clipmap::kMaxStamps ? Clipmap::kMaxStamps - a_out.size() : 0;
		if (room == 0) {
			return;
		}

		static std::vector<size_t>  capsules;
		static std::vector<uint8_t> due;
		capsules.clear();
		due.clear();
		for (const auto* tracked : live) {
			capsules.push_back(tracked->count);
			due.push_back(g_clock - tracked->lastStamp >= CorpseRule::kRestampSeconds ? 1 : 0);
		}
		const auto pick = CorpseRule::PickCorpses(capsules, due, room, g_cursor);
		g_cursor = pick.cursor;

		for (const auto index : pick.chosen) {
			auto&       tracked = *live[index];
			const float elapsed = std::min(g_clock - tracked.lastStamp, kMaxElapsed);
			const float strength = CorpseRule::CompensatedStrength(CorpseRule::kDrawPerSecond, elapsed, dt);
			Emit(tracked, strength, a_out);
			tracked.lastStamp = g_clock;
			Profiler::Tally(Profiler::Count::kCorpsesMarked);
		}
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_tracked.clear();
		g_cursor = 0;
		g_clock = 0.0f;
	}

	void ResetDiagnostics()
	{
		g_logged.store(0);
	}
}
