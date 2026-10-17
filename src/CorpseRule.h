// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The rules for marks under corpses, free of the engine so the offline checks can run them.
// A body that has stopped moving clears a hollow of its own shape in the snow (it lies on the
// collision ground, under the blanket's top) and presses a shallow print into other soft
// ground. While snow falls the hollow fills back in, and a low mound then grows over the body.
namespace CorpseRule
{
	// ActorState::lifeState: four bits at bit 17 of the dword at Actor+0x130 (ActorState at
	// +0x128, its bit-field word at +0x8) on OG, NG and 1.11.240; VR differs (FO4_PORT_NOTES).
	inline constexpr std::ptrdiff_t kLifeStateOffset = 0x130;
	inline constexpr uint32_t       kLifeStateShift = 17;

	enum LifeState : uint32_t
	{
		kAlive = 0,
		kDying = 1,
		kDead = 2,
		kUnconscious = 3,
		kReanimate = 4,
		kRecycle = 5,
		kRestrained = 6,
		kEssentialDown = 7,
		kBleedout = 8,
	};

	[[nodiscard]] inline uint32_t LifeStateOf(uint32_t a_word)
	{
		return (a_word >> kLifeStateShift) & 0xFu;
	}

	// ActorState::meleeAttackState, three bits at bit 25 of the same word: not 0 while a melee
	// attack runs (drawing back, swinging, hitting, following through).
	inline constexpr uint32_t kMeleeAttackShift = 25;

	[[nodiscard]] inline uint32_t MeleeAttackOf(uint32_t a_word)
	{
		return (a_word >> kMeleeAttackShift) & 0x7u;
	}

	// Lying dead, as Actor::IsDead(true) answers (OG 0x140D89D40): dying, dead or recycled.
	// An essential actor knocked down gets up again and is not a corpse.
	[[nodiscard]] inline bool IsCorpseState(uint32_t a_state)
	{
		return a_state == kDying || a_state == kDead || a_state == kRecycle;
	}

	// ── settling ────────────────────────────────────────────────────────────────

	// A body counts as at rest once its bound has moved less than kStillSpeed (units per
	// second, plus kStillJitter a frame for a ragdoll's twitching) for kSettleSeconds; a move
	// of more than kMoveTolerance from where it settled (dragged, blown away) unsettles it.
	inline constexpr float kStillSpeed = 6.0f;
	inline constexpr float kStillJitter = 0.25f;
	inline constexpr float kSettleSeconds = 1.0f;
	inline constexpr float kMoveTolerance = 8.0f;

	struct Settle
	{
		float stillSeconds{ 0.0f };
		bool  settled{ false };
	};

	// One frame: a_moved is how far the body's bound moved since the last frame, a_drift how
	// far it lies from where it settled (0 while unsettled). A paused frame changes nothing.
	inline bool UpdateSettle(Settle& a_settle, float a_moved, float a_drift, float a_deltaSeconds)
	{
		if (!(a_deltaSeconds > 0.0f)) {
			return a_settle.settled;
		}
		if (a_settle.settled && a_drift > kMoveTolerance) {
			a_settle = {};
		}
		if (a_moved <= kStillSpeed * a_deltaSeconds + kStillJitter) {
			a_settle.stillSeconds = std::min(a_settle.stillSeconds + a_deltaSeconds, kSettleSeconds * 2.0f);
		} else if (!a_settle.settled) {
			a_settle.stillSeconds = 0.0f;
		}
		if (a_settle.stillSeconds >= kSettleSeconds) {
			a_settle.settled = true;
		}
		return a_settle.settled;
	}

	// ── burial ──────────────────────────────────────────────────────────────────

	// The share of the burial spent filling the hollow back in; the rest grows the mound.
	inline constexpr float kRefillShare = 0.5f;

	// Burial 0 to 1, grown by the snowfall (0 to 1) over a_seconds of full snowfall. Without
	// snowfall it stays where it is: the hollow keeps its depth.
	[[nodiscard]] inline float AdvanceBurial(float a_burial, float a_snowfall, float a_deltaSeconds, float a_seconds)
	{
		const float fall = std::clamp(a_snowfall, 0.0f, 1.0f);
		if (!(a_deltaSeconds > 0.0f) || fall <= 0.0f) {
			return std::clamp(a_burial, 0.0f, 1.0f);
		}
		return std::clamp(a_burial + fall * a_deltaSeconds / std::max(a_seconds, 1.0f), 0.0f, 1.0f);
	}

	// How much of the hollow is still open (1 fresh, 0 filled) and how much of the mound has
	// grown (0 none, 1 full). The mound only grows once the hollow is full.
	[[nodiscard]] inline float HollowShare(float a_burial)
	{
		return std::clamp(1.0f - std::clamp(a_burial, 0.0f, 1.0f) / kRefillShare, 0.0f, 1.0f);
	}

	[[nodiscard]] inline float MoundShare(float a_burial)
	{
		return std::clamp((std::clamp(a_burial, 0.0f, 1.0f) - kRefillShare) / (1.0f - kRefillShare), 0.0f, 1.0f);
	}

	// The height the field is drawn to under a body part (world units against the blanket's
	// top): down by the clearing while the hollow is open, up by the mound once it is full.
	[[nodiscard]] inline float FloorFor(float a_burial, float a_clearing, float a_mound)
	{
		return -std::max(a_clearing, 0.0f) * HollowShare(a_burial) + std::max(a_mound, 0.0f) * MoundShare(a_burial);
	}

	// How fast the field is drawn to that height once the burial has begun, as the fraction of
	// the gap closed per second (the field's melt; a fresh hollow is pressed at once instead).
	// A corpse is stamped only every few frames and the field applies the melt only on those,
	// so the strength is raised to close in one frame what a_elapsed seconds would have closed,
	// as far as the field's per-second encoding goes (0.9999: at 60 Hz a frame closes at most a
	// seventh of the gap, plenty for a floor that moves over minutes).
	inline constexpr float kDrawPerSecond = 0.95f;

	[[nodiscard]] inline float CompensatedStrength(float a_perSecond, float a_elapsed, float a_frameSeconds)
	{
		const float rate = std::clamp(a_perSecond, 0.0f, 0.9999f);
		if (!(a_frameSeconds > 0.0f) || !(a_elapsed > 0.0f)) {
			return rate;
		}
		const float remains = std::pow(1.0f - rate, std::max(a_elapsed, a_frameSeconds) / a_frameSeconds);
		return std::clamp(1.0f - remains, 0.0f, 0.9999f);
	}

	// ── body parts ──────────────────────────────────────────────────────────────

	struct Point
	{
		float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
	};

	// A body part as seen from above: a capsule from a to b.
	struct Capsule
	{
		Point a{};
		Point b{};
		float radius{ 0.0f };
	};

	// A bone's world position by its node name.
	struct Bone
	{
		std::string_view name;
		Point            at{};
	};

	// A limb as a chain of joints, each by its node name in the skeletons that have it (|
	// separated: a person's, a feral ghoul's, a deathclaw's, a mirelurk's, a molerat's, a
	// dog's), with the capsule ending at that joint: its radius as a share of its length (so
	// a child and a super mutant scale by themselves), its floor and ceiling, and how far it
	// runs on past the joint (the head beyond its bone), as a share of its length. A joint the
	// skeleton lacks is passed over and its neighbours joined (a ghoul's upper arm to its hand).
	struct Joint
	{
		std::string_view names;
		float            share;
		float            minRadius;
		float            maxRadius;
		float            reach;
	};

	inline constexpr size_t kJointsPerLimb = 4;

	struct Limb
	{
		std::array<Joint, kJointsPerLimb> joints;
		size_t                            count;
	};

	inline constexpr std::array<Limb, 5> kLimbs{ {
		{ { { { "Pelvis|Dogmeat_Pelvis", 0, 0, 0, 0 },
			  { "SPINE2|Spine02|Dogmeat_Spine2", 0.55f, 5.0f, 18.0f, 0.0f },
			  { "Neck|Neck01|Dogmeat_Neck1", 0.5f, 5.0f, 18.0f, 0.0f },
			  { "HEAD|Dogmeat_Head", 0.75f, 3.5f, 10.0f, 0.8f } } },
			4 },
		{ { { { "LArm_UpperArm|LUPPERARM|LArmUpperArm|LArmUpper|Front_LLeg1|Dogmeat_Front_LLeg1", 0, 0, 0, 0 },
			  { "LArm_ForeArm1|LArmFore|Front_LLeg2|Dogmeat_Front_LLeg2", 0.3f, 2.5f, 8.0f, 0.0f },
			  { "LArm_Hand|LHAND|LArmHand|LArmPalm|Front_LLegAnkle|Dogmeat_Front_LLegAnkle", 0.27f, 2.0f, 7.0f, 0.0f } } },
			3 },
		{ { { { "RArm_UpperArm|RUPPERARM|RArmUpperArm|RArmUpper|Front_RLeg1|Dogmeat_Front_RLeg1", 0, 0, 0, 0 },
			  { "RArm_ForeArm1|RArmFore|Front_RLeg2|Dogmeat_Front_RLeg2", 0.3f, 2.5f, 8.0f, 0.0f },
			  { "RArm_Hand|RHAND|RArmHand|RArmPalm|Front_RLegAnkle|Dogmeat_Front_RLegAnkle", 0.27f, 2.0f, 7.0f, 0.0f } } },
			3 },
		{ { { { "LLeg_Thigh|LTHIGH|Back_LLeg1|LBackLegThigh", 0, 0, 0, 0 },
			  { "LLeg_Calf|LCALF|Back_LLeg2|LBackLegCalf", 0.28f, 3.0f, 10.0f, 0.0f },
			  { "LLeg_Foot|LFOOT|Back_LLegAnkle|LBackLegfoot", 0.2f, 2.5f, 8.0f, 0.0f } } },
			3 },
		{ { { { "RLeg_Thigh|RTHIGH|Back_RLeg1|RBackLegThigh", 0, 0, 0, 0 },
			  { "RLeg_Calf|RCALF|Back_RLeg2|RBackLegCalf", 0.28f, 3.0f, 10.0f, 0.0f },
			  { "RLeg_Foot|RFOOT|Back_RLegAnkle|RBackLegFoot", 0.2f, 2.5f, 8.0f, 0.0f } } },
			3 },
	} };

	// At most one capsule per joint after a limb's first.
	inline constexpr size_t kMaxCapsules = 11;

	// Fewer capsules than this and the skeleton is not one the chains know: the bound is used.
	inline constexpr size_t kMinNamedCapsules = 3;

	[[nodiscard]] inline bool SameName(std::string_view a_lhs, std::string_view a_rhs)
	{
		return a_lhs.size() == a_rhs.size() &&
		       std::equal(a_lhs.begin(), a_lhs.end(), a_rhs.begin(), [](char a_l, char a_r) {
				   return std::tolower(static_cast<unsigned char>(a_l)) == std::tolower(static_cast<unsigned char>(a_r));
			   });
	}

	// Whether a node name is one of a joint's names.
	[[nodiscard]] inline bool NameIn(std::string_view a_name, std::string_view a_names)
	{
		while (!a_names.empty()) {
			const auto bar = a_names.find('|');
			if (SameName(a_name, a_names.substr(0, bar))) {
				return true;
			}
			a_names = bar == std::string_view::npos ? std::string_view{} : a_names.substr(bar + 1);
		}
		return false;
	}

	// Whether any limb has a joint of this name (to collect only the bones that matter).
	[[nodiscard]] inline bool IsJointName(std::string_view a_name)
	{
		for (const auto& limb : kLimbs) {
			for (size_t i = 0; i < limb.count; ++i) {
				if (NameIn(a_name, limb.joints[i].names)) {
					return true;
				}
			}
		}
		return false;
	}

	[[nodiscard]] inline const Bone* FindJoint(std::span<const Bone> a_bones, const Joint& a_joint)
	{
		for (const auto& bone : a_bones) {
			if (NameIn(bone.name, a_joint.names)) {
				return &bone;
			}
		}
		return nullptr;
	}

	// The capsules a skeleton's named bones give, limb by limb. Segments shorter than a unit
	// are skipped. A creature whose bones the chains do not know gives few or none and falls
	// back to its bound (kMinNamedCapsules).
	inline size_t CapsulesFromBones(std::span<const Bone> a_bones, std::array<Capsule, kMaxCapsules>& a_out)
	{
		size_t count = 0;
		for (const auto& limb : kLimbs) {
			const Bone* previous = nullptr;
			for (size_t i = 0; i < limb.count && count < kMaxCapsules; ++i) {
				const auto& joint = limb.joints[i];
				const auto* bone = FindJoint(a_bones, joint);
				if (!bone) {
					continue;
				}
				if (previous) {
					const Point a = previous->at;
					Point       b = bone->at;
					const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
					const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (length >= 1.0f) {
						b = { b.x + dx * joint.reach, b.y + dy * joint.reach, b.z + dz * joint.reach };
						const float radius = std::clamp(length * joint.share, joint.minRadius, joint.maxRadius);
						a_out[count++] = { a, b, radius };
					}
				}
				previous = bone;
			}
		}
		return count;
	}
	// A body without the named bones: one disc in the middle of its bound, as wide as about
	// half of it.
	[[nodiscard]] inline Capsule CapsuleFromBound(const Point& a_centre, float a_radius)
	{
		const float radius = std::clamp(a_radius * 0.45f, 6.0f, 60.0f);
		return { a_centre, a_centre, radius };
	}

	// How deep the snow is cleared under a capsule: from the blanket's top down to the part's
	// underside (its bones lie a radius above what it rests on), a little more, never past
	// the blanket and the bite below it. A part lying above the snow clears nothing.
	inline constexpr float kClearingMargin = 2.0f;

	[[nodiscard]] inline float ClearingDepth(const Capsule& a_capsule, float a_landZ, float a_lift, float a_bite)
	{
		if (!(a_lift > 0.0f)) {
			return 0.0f;
		}
		const float underside = std::min(a_capsule.a.z, a_capsule.b.z) - a_capsule.radius;
		const float depth = a_landZ + a_lift - underside + kClearingMargin;
		return std::clamp(depth, 0.0f, a_lift + std::max(a_bite, 0.0f));
	}

	// ── budget ──────────────────────────────────────────────────────────────────

	// A corpse's marks hold between stampings, so each is stamped again only every
	// kRestampSeconds, and only while the frame has room left under the field's budget after
	// the feet, impacts and objects: corpses never push those out. Each corpse goes whole.
	inline constexpr float kRestampSeconds = 0.1f;

	struct Pick
	{
		std::vector<size_t> chosen;
		size_t              cursor{ 0 };
	};

	// Picks this frame's corpses round robin from a_cursor: a corpse is taken when it is due
	// and all its capsules fit in the room left; one that does not fit ends the frame, so a
	// big body is not passed over forever by smaller ones.
	inline Pick PickCorpses(std::span<const size_t> a_capsules, std::span<const uint8_t> a_due, size_t a_room, size_t a_cursor)
	{
		Pick         pick{};
		const size_t count = a_capsules.size();
		pick.cursor = count ? a_cursor % count : 0;
		size_t room = a_room;
		for (size_t visited = 0; visited < count; ++visited) {
			const size_t index = (pick.cursor + visited) % count;
			if (!a_due[index] || a_capsules[index] == 0) {
				continue;
			}
			if (a_capsules[index] > room) {
				pick.cursor = index;
				return pick;
			}
			pick.chosen.push_back(index);
			room -= a_capsules[index];
		}
		pick.cursor = count ? (pick.cursor + 1) % count : 0;
		return pick;
	}
}
