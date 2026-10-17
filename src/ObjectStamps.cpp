// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"
#include "Profiler.h"

#include "ObjectStamps.h"

#include "Globals.h"
#include "GameLayouts.h"
#include "HeatSources.h"
#include "Settings.h"
#include "SnowSurface.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ObjectStamps
{
	namespace
	{
		std::mutex g_lock;

		struct Candidate
		{
			RE::ObjectRefHandle handle;

			float distanceSq{ 0.0f };

			// Only compared, never followed: which cell the reference was found in, so a
			// refresh that could not lock that cell keeps its earlier candidates.
			const RE::TESObjectCELL* cell{ nullptr };

			bool heat{ false };
		};

		std::vector<Candidate> g_candidates;
		float                  g_timer{ 0.0f };
		bool                   g_haveSet{ false };
		uint64_t               g_frame{ 0 };

		// Game seconds (paused time excluded), for the stamp cache.
		float g_clock{ 0.0f };

		constexpr size_t kMaxCandidates = 256;

		// References near the player copied out of the cells per refresh, at most.
		constexpr size_t kMaxNearby = 2048;

		constexpr float kMaxMotion = 48.0f;

		// A Fallout 4 exterior cell is 4096 units on a side.
		constexpr float kCellUnits = 4096.0f;

		// A resting object's stamps are rebuilt this often (the weather and the snow they
		// depend on change slowly); a moving one's every frame. Fires barely move at all.
		constexpr float kRestRebuildSeconds = 0.5f;
		constexpr float kHeatRebuildSeconds = 1.0f;

		struct float2
		{
			float x{ 0.0f }, y{ 0.0f };
		};

		struct Tracked
		{
			float    x{ 0.0f }, y{ 0.0f };
			uint64_t frame{ 0 };
		};

		std::unordered_map<RE::TESFormID, Tracked> g_motion;

		constexpr size_t kMaxShapesPerObject = 3;

		// The stamps last built for a candidate, kept while it rests.
		struct Built
		{
			Clipmap::Stamp stamps[kMaxShapesPerObject]{};
			size_t         count{ 0 };
			RE::NiPoint3   at{};
			RE::NiTransform world{};
			float          builtAt{ -1.0e9f };
			uint64_t       seen{ 0 };
		};

		std::unordered_map<uint32_t, Built> g_built;

		// Loose things that come to rest on the ground: dropped weapons, armour, junk,
		// food and chems. Bullets and lasers are pressed by their impacts instead, actors
		// by their feet, and projectiles (thrown grenades, mines, arrows) not at all: they
		// are either about to explode or stuck in something.
		bool LeavesAMark(RE::TESObjectREFR* a_ref)
		{
			if (a_ref->GetFormType() != RE::ENUM_FORM_ID::kREFR) {
				return false;
			}

			const auto* base = a_ref->GetObjectReference();
			if (!base) {
				return false;
			}

			switch (base->GetFormType()) {
			case RE::ENUM_FORM_ID::kWEAP:
			case RE::ENUM_FORM_ID::kARMO:
			case RE::ENUM_FORM_ID::kAMMO:
			case RE::ENUM_FORM_ID::kMISC:
			case RE::ENUM_FORM_ID::kCMPO:
			case RE::ENUM_FORM_ID::kINGR:
			case RE::ENUM_FORM_ID::kALCH:
			case RE::ENUM_FORM_ID::kBOOK:
			case RE::ENUM_FORM_ID::kNOTE:
			case RE::ENUM_FORM_ID::kKEYM:
				return true;
			default:
				return false;
			}
		}

		std::unordered_set<RE::TESFormID> g_reported;

		void LogObject(RE::TESObjectREFR* a_ref, Surfaces::Type a_surface,
			const Clipmap::Stamp& a_stamp, float a_drop, bool a_footprint)
		{
			if (!Settings::logObjectStamps || g_reported.size() >= 24) {
				return;
			}

			const auto* base = a_ref->GetObjectReference();
			if (!base || !g_reported.insert(base->GetFormID()).second) {
				return;
			}

			const std::string shape =
				a_footprint ? std::format("footprint {:.1f} x {:.1f} heading {:.0f} deg", a_stamp.radius * 2.0f,
								  a_stamp.halfWidth * 2.0f,
								  std::atan2(a_stamp.forwardX, a_stamp.forwardY) * 57.29578f) :
							  std::format("disc radius={:.1f} (no usable bounds)", a_stamp.radius);
			logger::info(
				"Object stamp: {:08X} (base {:08X}) on {:<7} {} depth={:.1f} rim={:.2f} from {:.1f} out drop={:.1f}",
				a_ref->GetFormID(), base->GetFormID(), Surfaces::Name(a_surface), shape, a_stamp.depth,
				a_stamp.rim, a_stamp.rimMargin, a_drop);
		}

		// What an object covers seen from above: its base form's bounds (OBND, model units,
		// the box around its model) turned and scaled as its 3D root lies, measured along
		// the box edge that looks longest from above - a rifle along its length.
		struct Footprint
		{
			float x{ 0.0f }, y{ 0.0f };
			float forwardX{ 0.0f }, forwardY{ 1.0f };
			float halfLength{ 0.0f }, halfWidth{ 0.0f };
			float bottom{ 0.0f };
		};

		// Bounds that disagree with the model's own bound sphere by more than this (in
		// either direction) belong to another model (a weapon rebuilt from its mods, say),
		// and the object falls back to the discs of its shapes.
		constexpr float kBoundsAgreement = 2.5f;

		// A footprint is never narrower than this (world units): about two field texels.
		constexpr float kMinFootprintHalfWidth = 0.75f;

		bool FootprintOf(const RE::TESObjectREFR* a_ref, const RE::NiAVObject* a_root, Footprint& a_out)
		{
			// TESBoundObject::boundData sits right after TESForm (+0x20) on every runtime
			// this plugin supports; the TESNPC components RaceFormID reads lie past it.
			const auto* base = a_ref->GetObjectReference();
			if (!base) {
				return false;
			}
			const auto& bounds = base->boundData;
			const float lo[3]{ static_cast<float>(bounds.boundMin.x), static_cast<float>(bounds.boundMin.y),
				static_cast<float>(bounds.boundMin.z) };
			const float hi[3]{ static_cast<float>(bounds.boundMax.x), static_cast<float>(bounds.boundMax.y),
				static_cast<float>(bounds.boundMax.z) };
			if (hi[0] < lo[0] || hi[1] < lo[1] || hi[2] < lo[2]) {
				return false;
			}

			const auto& world = a_root->world;
			const float scale = world.scale > 0.0f ? world.scale : 1.0f;
			const float halfDiagonal = 0.5f * scale *
			                           std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
										   (hi[2] - lo[2]) * (hi[2] - lo[2]));
			const float sphere = a_root->worldBound.fRadius;
			if (!(halfDiagonal >= 1.0f) || !(sphere > 0.1f) || halfDiagonal > sphere * kBoundsAgreement ||
				halfDiagonal * kBoundsAgreement < sphere) {
				return false;
			}

			RE::NiPoint3 corners[8];
			for (int i = 0; i < 8; ++i) {
				const RE::NiPoint3 local{ (i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2] };
				corners[i] = world.rotate * (local * scale) + world.translate;
			}

			// The box edge that looks longest from above sets the footprint's length axis.
			float axisX = 0.0f, axisY = 0.0f, longest = -1.0f;
			for (const int edge : { 1, 2, 4 }) {
				const float dx = corners[edge].x - corners[0].x;
				const float dy = corners[edge].y - corners[0].y;
				if (dx * dx + dy * dy > longest) {
					longest = dx * dx + dy * dy;
					axisX = dx;
					axisY = dy;
				}
			}
			if (!(longest > 1e-6f)) {
				return false;
			}
			const float axisLength = std::sqrt(longest);
			float       forwardX = axisX / axisLength;
			float       forwardY = axisY / axisLength;

			const auto extent = [&](float a_dirX, float a_dirY, float& a_min, float& a_max) {
				a_min = a_max = corners[0].x * a_dirX + corners[0].y * a_dirY;
				for (const auto& corner : corners) {
					const float along = corner.x * a_dirX + corner.y * a_dirY;
					a_min = std::min(a_min, along);
					a_max = std::max(a_max, along);
				}
			};
			float minAlong{}, maxAlong{}, minAcross{}, maxAcross{};
			extent(forwardX, forwardY, minAlong, maxAlong);
			extent(-forwardY, forwardX, minAcross, maxAcross);

			const float centreAlong = 0.5f * (minAlong + maxAlong);
			const float centreAcross = 0.5f * (minAcross + maxAcross);
			a_out.x = forwardX * centreAlong - forwardY * centreAcross;
			a_out.y = forwardY * centreAlong + forwardX * centreAcross;

			float halfLength = 0.5f * (maxAlong - minAlong);
			float halfWidth = 0.5f * (maxAcross - minAcross);
			if (halfWidth > halfLength) {
				std::swap(halfLength, halfWidth);
				const float turned = forwardX;
				forwardX = -forwardY;
				forwardY = turned;
			}
			a_out.forwardX = forwardX;
			a_out.forwardY = forwardY;
			a_out.halfWidth = std::max(halfWidth, kMinFootprintHalfWidth);
			a_out.halfLength = std::max(halfLength, a_out.halfWidth);

			a_out.bottom = corners[0].z;
			for (const auto& corner : corners) {
				a_out.bottom = std::min(a_out.bottom, corner.z);
			}
			return true;
		}

		struct ShapeBound
		{
			RE::NiPoint3 centre;
			float        radius{ 0.0f };
		};

		// Fallout 4's havok bodies do not expose a bounding sphere the way Skyrim's did,
		// so an object is pressed by the bounds of its largest visible shapes: a rifle
		// becomes a row of small discs along its length rather than one disc as wide as
		// the whole gun.
		void CollectShapes(RE::NiAVObject* a_object, std::vector<ShapeBound>& a_out,
			uint32_t a_depth = 0)
		{
			if (!a_object || a_depth > 32 || a_object->GetAppCulled() || a_out.size() >= 64) {
				return;
			}

			if (auto* node = a_object->IsNode()) {
				for (auto& child : GameLayouts::Children(node)) {
					if (child) {
						CollectShapes(child.get(), a_out, a_depth + 1);
					}
				}
				return;
			}

			const auto& bound = a_object->worldBound;
			if (bound.fRadius > 0.1f) {
				a_out.push_back({ bound.center, bound.fRadius });
			}
		}

		size_t GatherShapes(RE::NiAVObject* a_root, ShapeBound (&a_out)[kMaxShapesPerObject])
		{
			// Reused between calls; Append holds g_lock.
			static std::vector<ShapeBound> found;
			found.clear();
			CollectShapes(a_root, found);

			if (found.empty()) {
				const auto& bound = a_root->worldBound;
				if (bound.fRadius <= 0.1f) {
					return 0;
				}
				a_out[0] = ShapeBound{ bound.center, bound.fRadius };
				return 1;
			}

			const size_t take = std::min(found.size(), kMaxShapesPerObject);
			std::partial_sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(take),
				found.end(), [](const ShapeBound& a_lhs, const ShapeBound& a_rhs) {
					return a_lhs.radius > a_rhs.radius;
				});

			for (size_t i = 0; i < take; ++i) {
				a_out[i] = found[i];
			}
			return take;
		}

		size_t StampsFor(RE::TESObjectREFR* a_ref, float a_motionX, float a_motionY,
			Clipmap::Stamp (&a_out)[kMaxShapesPerObject])
		{
			auto* root = globals::Loaded3D(a_ref);
			if (!root) {
				return 0;
			}

			// One stamp over the object's footprint when its bounds can be trusted; else a
			// disc per largest shape, as wide as that shape's bound sphere.
			Footprint    footprint{};
			const bool   haveFootprint = FootprintOf(a_ref, root, footprint);
			ShapeBound   shapes[kMaxShapesPerObject]{};
			const size_t shapeCount = haveFootprint ? 0 : GatherShapes(root, shapes);
			if (!haveFootprint && shapeCount == 0) {
				return 0;
			}

			float groundZ = haveFootprint ? footprint.bottom : shapes[0].centre.z - shapes[0].radius;
			for (size_t i = 1; i < shapeCount; ++i) {
				groundZ = std::min(groundZ, shapes[i].centre.z - shapes[i].radius);
			}

			const RE::NiPoint3 probe = haveFootprint ?
			                               RE::NiPoint3{ footprint.x, footprint.y, footprint.bottom } :
			                               shapes[0].centre;

			auto* tes = globals::TES();
			float landZ = 0.0f;
			if (!tes || !tes->GetLandHeight(probe, landZ)) {
				return 0;
			}

			const float surfaceZ = landZ + SnowSurface::LiftAt(probe.x, probe.y);
			const float drop = groundZ - surfaceZ;
			if (drop > Settings::objectContactTolerance || drop < -Settings::objectSinkLimit) {
				return 0;
			}

			const auto  ground = Surfaces::GroundAt(probe, landZ);
			const auto  surface = ground.type;
			const auto& response = ground.response;

			if (response.depthScale <= 0.0f &&
				Surfaces::RimHeight(0.0f, response.rimScale) <= 0.0f) {
				return 0;
			}

			const float decay = Clipmap::RecoveryRate(Settings::stampDecayPerSecond,
				response.decayScale * Weather::DecayScale());
			const float sizeScale = Settings::objectStampRadiusScale * response.radiusScale;

			// The object lies on the undeformed collision ground: a shallow dent (snow is
			// still cleared off it) and a rim that rises only outside the footprint.
			const auto finish = [&](Clipmap::Stamp& a_stamp, float a_size) {
				a_stamp.snow = surface == Surfaces::Type::kSnow;
				a_stamp.motionX = a_motionX;
				a_stamp.motionY = a_motionY;

				const float bulk = std::clamp(
					a_size / std::max(Settings::objectFullSizeRadius, 1.0f), 0.0f, 1.0f);

				const float ordinary = std::min(Settings::stampDepth * Settings::objectStampDepthScale *
				                                    bulk * response.depthScale * Weather::DepthScale(),
					Settings::objectStampMaxDepth);

				a_stamp.depth = Surfaces::MarkDepth(surface, ordinary, bulk * ground.snowSink, a_stamp.x, a_stamp.y);
				a_stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);
				a_stamp.decay = decay;
				a_stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale);
				a_stamp.rimMargin = Settings::objectRimMargin;
			};

			if (haveFootprint) {
				Clipmap::Stamp stamp{};
				stamp.x = footprint.x;
				stamp.y = footprint.y;
				stamp.forwardX = footprint.forwardX;
				stamp.forwardY = footprint.forwardY;
				stamp.radius = footprint.halfLength * sizeScale;
				stamp.halfWidth = std::min(footprint.halfWidth * sizeScale, stamp.radius);
				finish(stamp, std::sqrt(footprint.halfLength * footprint.halfWidth));

				LogObject(a_ref, surface, stamp, drop, true);
				a_out[0] = stamp;
				return 1;
			}

			for (size_t i = 0; i < shapeCount; ++i) {
				const auto& shape = shapes[i];

				Clipmap::Stamp stamp{};
				stamp.x = shape.centre.x;
				stamp.y = shape.centre.y;
				stamp.radius = shape.radius * sizeScale;
				finish(stamp, shape.radius);

				if (i == 0) {
					LogObject(a_ref, surface, stamp, drop, false);
				}
				a_out[i] = stamp;
			}
			return shapeCount;
		}

		bool Usable(RE::TESObjectREFR* a_ref)
		{
			return a_ref && !a_ref->IsDisabled() && !a_ref->IsDeleted() &&
			       globals::Loaded3D(a_ref);
		}

		// A try-lock on a BSSpinLock, following the engine's own lock: re-entrant on the
		// owning thread, otherwise one compare-exchange of the count from 0 to 1. It is
		// released with CommonLibF4's unlock(). BSSpinLock::try_lock is not used because its
		// address is not verified on every runtime this plugin supports.
		class SpinTryLock
		{
		public:
			explicit SpinTryLock(RE::BSSpinLock& a_lock) :
				_lock(a_lock)
			{
				struct View
				{
					std::uint32_t          owningThread;
					volatile std::uint32_t lockCount;
				};
				static_assert(sizeof(View) == sizeof(RE::BSSpinLock));

				auto&      view = reinterpret_cast<View&>(a_lock);
				const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
				auto*      count = reinterpret_cast<volatile LONG*>(&view.lockCount);
				if (view.owningThread == thread) {
					::InterlockedIncrement(count);
					_owned = true;
				} else if (::InterlockedCompareExchange(count, 1, 0) == 0) {
					view.owningThread = thread;
					_owned = true;
				}
			}

			SpinTryLock(const SpinTryLock&) = delete;
			SpinTryLock& operator=(const SpinTryLock&) = delete;

			~SpinTryLock()
			{
				if (_owned) {
					_lock.unlock();
				}
			}

			[[nodiscard]] bool Owned() const { return _owned; }

		private:
			RE::BSSpinLock& _lock;
			bool            _owned{ false };
		};

		struct Nearby
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			float                            distanceSq{ 0.0f };
			const RE::TESObjectCELL*         cell{ nullptr };
		};

		std::vector<Nearby>                          g_nearby;
		std::unordered_set<const RE::TESObjectCELL*> g_busyCells;

		// Copies the references of a loaded cell that pass the cheap tests (distance, not
		// an actor) under the cell's own lock, and nothing else: classifying them calls
		// into the engine and takes other locks, so it happens after the lock is let go.
		// A cell whose lock is held elsewhere (a cell loading or a door transition) is
		// skipped rather than waited for, and keeps its earlier candidates.
		void CopyNearby(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_centre, float a_radiusSq)
		{
			if (!a_cell || g_nearby.size() >= kMaxNearby) {
				return;
			}

			// Attached cells only, as the engine's own nearby-reference search
			// (TES::ForEachLoadedCellOverlappingAABB) takes them: a cell still loading or
			// attaching has references in flux.
			if (!GameLayouts::CellAttached(a_cell)) {
				return;
			}

			const SpinTryLock lock{ a_cell->spinLock };
			if (!lock.Owned()) {
				g_busyCells.insert(a_cell);
				return;
			}

			for (auto& ref : a_cell->references) {
				if (g_nearby.size() >= kMaxNearby) {
					break;
				}
				if (!ref || ref->GetFormType() == RE::ENUM_FORM_ID::kACHR) {
					continue;
				}
				const float distanceSq = a_centre.GetSquaredDistance(ref->GetPosition());
				if (distanceSq > a_radiusSq) {
					continue;
				}
				g_nearby.push_back({ ref, distanceSq, a_cell });
			}
		}

		void Refresh()
		{
			std::vector<Candidate> previous;
			previous.swap(g_candidates);
			g_nearby.clear();
			g_busyCells.clear();

			auto* player = globals::game::player;
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return;
			}

			const RE::NiPoint3 centre = player->GetPosition();
			const float        radius = Clipmap::kWorldSize * 0.375f;
			const float        radiusSq = radius * radius;

			auto* tes = globals::TES();
			if (cell->IsInterior() || !tes) {
				CopyNearby(cell, centre, radiusSq);
			} else {
				// The marks only live near the player, so only the loaded exterior cells
				// the window overlaps are walked (one to four of them).
				const auto cellOf = [](float a_units) {
					return static_cast<int32_t>(std::floor(a_units / kCellUnits));
				};
				const int32_t x0 = cellOf(centre.x - radius);
				const int32_t x1 = cellOf(centre.x + radius);
				const int32_t y0 = cellOf(centre.y - radius);
				const int32_t y1 = cellOf(centre.y + radius);

				for (int32_t x = x0; x <= x1; ++x) {
					for (int32_t y = y0; y <= y1; ++y) {
						CopyNearby(tes->GetGridCellCell(x, y), centre, radiusSq);
					}
				}
			}

			for (auto& nearby : g_nearby) {
				auto* ref = nearby.ref.get();
				if (!Usable(ref)) {
					continue;
				}

				// A projectile may be a lit flare (heat), never an object mark (LeavesAMark).
				const bool heat = HeatSources::IsHeatSource(ref);
				if (!heat && !(Settings::enableObjectStamps && LeavesAMark(ref))) {
					continue;
				}
				g_candidates.push_back({ ref->GetHandle(), nearby.distanceSq, nearby.cell, heat });
			}
			g_nearby.clear();

			for (const auto& candidate : previous) {
				if (g_busyCells.contains(candidate.cell)) {
					g_candidates.push_back(candidate);
				}
			}

			const size_t keep = std::min(g_candidates.size(), kMaxCandidates);
			std::partial_sort(g_candidates.begin(), g_candidates.begin() + static_cast<std::ptrdiff_t>(keep),
				g_candidates.end(), [](const Candidate& a_lhs, const Candidate& a_rhs) {
					return a_lhs.distanceSq < a_rhs.distanceSq;
				});
			g_candidates.resize(keep);
		}

		float2 TrackMotion(RE::TESFormID a_form, const RE::NiPoint3& a_position)
		{
			float2 motion{ 0.0f, 0.0f };

			const auto previous = g_motion.find(a_form);
			if (previous != g_motion.end()) {
				const float dx = a_position.x - previous->second.x;
				const float dy = a_position.y - previous->second.y;
				if (dx * dx + dy * dy <= kMaxMotion * kMaxMotion) {
					motion = { dx, dy };
				}
			}

			g_motion[a_form] = { a_position.x, a_position.y, g_frame };
			return motion;
		}

		bool Moved(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			return a_from.GetSquaredDistance(a_to) > 0.25f;
		}
	}

	void Append(float a_deltaSeconds, const RE::NiPoint3& a_anchor,
		std::vector<Clipmap::Stamp>& a_out)
	{
		if ((!Settings::enableObjectStamps && !Settings::enableHeatSources) ||
			a_out.size() >= Clipmap::kGatherCap) {
			return;
		}

		const std::scoped_lock lock(g_lock);
		++g_frame;
		g_clock += std::max(a_deltaSeconds, 0.0f);

		g_timer -= a_deltaSeconds;
		if (!g_haveSet || g_timer <= 0.0f) {
			g_timer = std::max(Settings::objectStampInterval, 0.0f);
			g_haveSet = true;
			const int64_t scanStarted = Profiler::Ticks();
			Refresh();
			Profiler::AddCpuTicks(Profiler::CpuScope::kObjectScan, Profiler::Ticks() - scanStarted);
		}

		const size_t room = Clipmap::kGatherCap - a_out.size();

		static std::vector<Clipmap::Stamp> built;
		built.clear();

		for (auto& candidate : g_candidates) {
			if (built.size() >= room) {
				break;
			}

			auto  refPtr = candidate.handle.get();
			auto* ref = refPtr.get();
			if (!Usable(ref)) {
				continue;
			}

			auto& cache = g_built[candidate.handle.native_handle()];
			cache.seen = g_frame;
			const RE::NiPoint3 position = ref->GetPosition();

			if (candidate.heat) {
				if (g_clock - cache.builtAt >= kHeatRebuildSeconds || Moved(cache.at, position)) {
					cache.count = HeatSources::StampFor(ref, cache.stamps[0]) ? 1 : 0;
					cache.at = position;
					cache.builtAt = g_clock;
				}
			} else {
				const auto motion = TrackMotion(ref->GetFormID(), position);
				// Havok can rotate or settle the 3D while the reference's recorded position
				// stays put. The footprint is built from that 3D, so its cache must follow it.
				const auto* root = globals::Loaded3D(ref);
				const bool changed = root && (root->world.rotate != cache.world.rotate ||
					root->world.scale != cache.world.scale || Moved(cache.world.translate, root->world.translate));
				const bool moving = motion.x != 0.0f || motion.y != 0.0f || Moved(cache.at, position) || changed;
				if (moving || g_clock - cache.builtAt >= kRestRebuildSeconds) {
					cache.count = StampsFor(ref, motion.x, motion.y, cache.stamps);
					cache.at = position;
					if (root) {
						cache.world = root->world;
					}
					cache.builtAt = moving ? -1.0e9f : g_clock;
				} else {
					// At rest: the swept part of the last moving frame's stamps is spent.
					for (size_t i = 0; i < cache.count; ++i) {
						cache.stamps[i].motionX = 0.0f;
						cache.stamps[i].motionY = 0.0f;
					}
				}
			}

			const size_t take = std::min(cache.count, room - built.size());
			built.insert(built.end(), cache.stamps, cache.stamps + take);
		}

		std::sort(built.begin(), built.end(),
			[&a_anchor](const Clipmap::Stamp& a_lhs, const Clipmap::Stamp& a_rhs) {
				const float lx = a_lhs.x - a_anchor.x;
				const float ly = a_lhs.y - a_anchor.y;
				const float rx = a_rhs.x - a_anchor.x;
				const float ry = a_rhs.y - a_anchor.y;
				return (lx * lx + ly * ly) < (rx * rx + ry * ry);
			});

		const size_t take = std::min(room, built.size());
		a_out.insert(a_out.end(), built.begin(), built.begin() + static_cast<std::ptrdiff_t>(take));

		for (auto it = g_motion.begin(); it != g_motion.end();) {
			it = (it->second.frame + 120 < g_frame) ? g_motion.erase(it) : std::next(it);
		}
		for (auto it = g_built.begin(); it != g_built.end();) {
			it = (it->second.seen + 120 < g_frame) ? g_built.erase(it) : std::next(it);
		}
	}

	void Reset()
	{
		HeatSources::Reset();

		const std::scoped_lock lock(g_lock);
		g_candidates.clear();
		g_nearby.clear();
		g_busyCells.clear();
		g_motion.clear();
		g_built.clear();
		g_reported.clear();
		g_timer = 0.0f;
		g_haveSet = false;
	}
}
