// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "HeatSources.h"
#include "ImpactPatterns.h"

#include "GameLayouts.h"
#include "Globals.h"
#include "Settings.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HeatSources
{
	namespace
	{
		std::mutex                                 g_lock;
		std::unordered_set<RE::TESFormID>          g_reported;
		std::unordered_map<RE::TESFormID, bool>    g_hotBase;
		constexpr size_t                           kMaxCachedBases = 8192;

		// Fires stamped lately, for the snow-on-objects pass (no snow lies on a fire).
		struct Seen
		{
			Hot      hot;
			uint64_t frame{ 0 };
		};
		std::unordered_map<RE::TESFormID, Seen> g_seen;
		constexpr uint64_t                      kSeenFrames = 600;
		constexpr size_t                        kMaxSeen = 256;

		std::string Lowered(std::string_view a_text)
		{
			std::string out(a_text);
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// Fallout 4 has no fire keyword on its forms, and a light form is as often a bulb
		// as a flame, so a heat source is told by its model: fire effects, burning
		// barrels, wood fires and candles. The exclusions strip the unlit variants and the
		// many meshes that only carry the word (fire escapes, extinguishers, burnt trees).
		bool ModelIsHot(const RE::TESBoundObject* a_base)
		{
			switch (a_base->GetFormType()) {
			case RE::ENUM_FORM_ID::kEXPL:  // explosions press their own craters
			case RE::ENUM_FORM_ID::kNPC_:
			case RE::ENUM_FORM_ID::kWEAP:
			case RE::ENUM_FORM_ID::kAMMO:
				return false;
			default:
				break;
			}

			const auto* model = a_base->As<RE::TESModel>();
			if (!model) {
				return false;
			}

			const char* path = model->GetModel();
			if (!path || !*path) {
				return false;
			}

			const std::string lowered = Lowered(path);
			const auto        slash = lowered.find_last_of("\\/");
			const std::string name = slash == std::string::npos ? lowered : lowered.substr(slash + 1);

			return Surfaces::MatchesKeywords(name, Settings::heatKeywords) &&
			       !Surfaces::MatchesKeywords(name, Settings::heatExcludeKeywords);
		}

		bool IsHot(RE::TESObjectREFR* a_ref)
		{
			const auto* base = a_ref->GetObjectReference();
			if (!base) {
				return false;
			}

			const std::scoped_lock lock(g_lock);
			const auto             cached = g_hotBase.find(base->GetFormID());
			if (cached != g_hotBase.end()) {
				return cached->second;
			}

			const bool hot = ModelIsHot(base);
			if (g_hotBase.size() >= kMaxCachedBases) {
				g_hotBase.clear();
			}
			g_hotBase.emplace(base->GetFormID(), hot);
			return hot;
		}

		void MergeSphere(RE::NiPoint3& a_centre, float& a_radius, const RE::NiPoint3& a_otherCentre,
			float a_otherRadius)
		{
			if (a_radius <= 0.0f) {
				a_centre = a_otherCentre;
				a_radius = a_otherRadius;
				return;
			}

			const float distance = std::sqrt(a_centre.GetSquaredDistance(a_otherCentre));
			if (distance + a_otherRadius <= a_radius) {
				return;
			}
			if (distance + a_radius <= a_otherRadius || distance <= 1e-4f) {
				a_centre = a_otherCentre;
				a_radius = std::max(a_radius, a_otherRadius);
				return;
			}

			const float merged = 0.5f * (distance + a_radius + a_otherRadius);
			const float shift = (merged - a_radius) / distance;
			a_centre.x += (a_otherCentre.x - a_centre.x) * shift;
			a_centre.y += (a_otherCentre.y - a_centre.y) * shift;
			a_centre.z += (a_otherCentre.z - a_centre.z) * shift;
			a_radius = merged;
		}

		// The bound of the fire's solid meshes (logs, barrel, candle). The scene root's
		// bound also holds the flame, smoke and spark particles, which swell and shrink
		// every frame and made the melt circle breathe with them.
		void SolidBound(RE::NiAVObject* a_object, RE::NiPoint3& a_centre, float& a_radius,
			uint32_t a_depth = 0)
		{
			if (!a_object || a_depth > 32 || a_object->GetAppCulled()) {
				return;
			}

			if (auto* node = a_object->IsNode()) {
				for (auto& child : GameLayouts::Children(node)) {
					if (child) {
						SolidBound(child.get(), a_centre, a_radius, a_depth + 1);
					}
				}
				return;
			}

			if (a_object->IsTriShape() && a_object->worldBound.fRadius > 0.1f) {
				MergeSphere(a_centre, a_radius, a_object->worldBound.center, a_object->worldBound.fRadius);
			}
		}

		float ReachFor(float a_boundRadius)
		{
			return ImpactPatterns::BoundedRadius(
				std::max(Settings::heatRadius, a_boundRadius * Settings::heatRadiusScale),
				1.0f, Settings::heatMaxRadius);
		}

		float MeltFloor(Surfaces::Type a_surface)
		{
			if (!Settings::heatMeltsSnow || a_surface != Surfaces::Type::kSnow) {
				return 0.0f;
			}

			return -Weather::SnowDepth() *
			       std::clamp(Settings::heatMeltDepthScale, 0.0f, 4.0f);
		}

		void Fill(Clipmap::Stamp& a_out, const RE::NiPoint3& a_position, float a_reach)
		{
			a_out = {};
			a_out.kind = Clipmap::Stamp::Kind::kMelt;
			a_out.heat = true;
			a_out.x = a_position.x;
			a_out.y = a_position.y;
			a_out.radius = a_reach;

			a_out.depth = std::clamp(Settings::heatMeltPerSecond, 0.0f, 1.0f);

			a_out.shoulder = std::clamp(Settings::heatShoulder, 0.0f, 0.95f);

			const auto ground = Surfaces::GroundAt(a_position);
			const auto surface = ground.type;

			a_out.rim = MeltFloor(surface);

			a_out.decay = 1.0f;
			if (a_out.rim < 0.0f) {
				const auto& response = ground.response;
				a_out.decay = Clipmap::RecoveryRate(Settings::stampDecayPerSecond,
					response.decayScale * Weather::DecayScale());
			}
		}

		void Log(RE::TESObjectREFR* a_ref, const Clipmap::Stamp& a_stamp)
		{
			if (!Settings::logHeatSources) {
				return;
			}

			const auto* base = a_ref->GetObjectReference();
			const auto* model = base ? base->As<RE::TESModel>() : nullptr;

			const std::scoped_lock lock(g_lock);
			if (g_reported.size() >= 24 || !g_reported.insert(a_ref->GetFormID()).second) {
				return;
			}

			logger::info(
				"Heat source: {:08X} [{}] at ({:.0f}, {:.0f}) reach={:.0f} melt={:.2f} floor={:.0f}",
				a_ref->GetFormID(), model && model->GetModel() ? model->GetModel() : "?",
				a_stamp.x, a_stamp.y, a_stamp.radius, a_stamp.depth, a_stamp.rim);
		}
	}

	bool IsHeatSource(RE::TESObjectREFR* a_ref)
	{
		return Settings::enableHeatSources && a_ref && globals::Loaded3D(a_ref) && IsHot(a_ref);
	}

	bool StampFor(RE::TESObjectREFR* a_ref, Clipmap::Stamp& a_out)
	{
		if (!Settings::enableHeatSources || !a_ref || !IsHot(a_ref)) {
			return false;
		}

		auto* root = globals::Loaded3D(a_ref);
		if (!root) {
			return false;
		}

		RE::NiPoint3 solidCentre{};
		float        solidRadius = 0.0f;
		SolidBound(root, solidCentre, solidRadius);
		if (!(solidRadius > 0.0f)) {
			solidRadius = root->worldBound.fRadius;
		}

		const RE::NiPoint3 position = a_ref->GetPosition();
		const float        reach = ReachFor(solidRadius);
		if (!(reach > 0.0f)) {
			return false;
		}
		Fill(a_out, position, reach);

		{
			const std::scoped_lock lock(g_lock);
			if (g_seen.size() >= kMaxSeen) {
				g_seen.clear();
			}
			g_seen[a_ref->GetFormID()] = { { solidCentre.x, solidCentre.y, solidCentre.z, solidRadius },
				globals::FrameIndex() };
		}

		Log(a_ref, a_out);
		return true;
	}

	size_t Nearby(float a_fromX, float a_fromY, float a_reach, std::span<Hot> a_out)
	{
		const std::scoped_lock lock(g_lock);
		const uint64_t         frame = globals::FrameIndex();
		static std::vector<std::pair<float, Hot>> found;
		found.clear();
		for (auto it = g_seen.begin(); it != g_seen.end();) {
			if (frame > it->second.frame + kSeenFrames) {
				it = g_seen.erase(it);
				continue;
			}
			const auto& hot = it->second.hot;
			const float dx = hot.x - a_fromX;
			const float dy = hot.y - a_fromY;
			const float reach = a_reach + hot.radius;
			if (dx * dx + dy * dy <= reach * reach) {
				found.emplace_back(dx * dx + dy * dy, hot);
			}
			++it;
		}
		std::sort(found.begin(), found.end(), [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.first < a_rhs.first; });
		const size_t count = std::min(found.size(), a_out.size());
		for (size_t i = 0; i < count; ++i) {
			a_out[i] = found[i].second;
		}
		return count;
	}

	void Reset()
	{
		const std::scoped_lock lock(g_lock);
		g_reported.clear();
		g_hotBase.clear();
		g_seen.clear();
	}
}
