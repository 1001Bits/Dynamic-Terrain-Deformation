// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Reading feet off a skeleton, apart from the engine types so the offline tests can check it.
// Full-body first-person mods (True First Person, Full Body First Person) show the player's
// own third-person body but move it off the actor's position (about 10 units with True First
// Person, up to 40 with Full Body First Person) and turn its hips toward where it walks, and
// True First Person hangs a copy of the whole skeleton under it. So a foot's side comes from
// its name or the feet's own middle, and a print points the way the foot does.
namespace FootGeometry
{
	// Node-name prefixes of a body copy another mod attaches under an actor's own 3D: True
	// First Person's "TFP_ShadowTwin" (a clone of the skeleton, feet included, the last
	// child of the root). Its feet are not the actor's and are never pressed.
	inline constexpr std::array<std::string_view, 1> kCloneNodePrefixes{ "TFP_" };

	inline bool IsCloneNode(std::string_view a_name)
	{
		return std::ranges::any_of(kCloneNodePrefixes,
			[&](std::string_view a_prefix) { return a_name.starts_with(a_prefix); });
	}

	// Which side a bone is on by its name: -1 left, +1 right, 0 when the name does not say.
	// The name splits into words at separators and case changes ("LLeg_Foot" is L, Leg, Foot;
	// "Bip01 L Foot"; "FrontRLeg_Foot" is Front, R, Leg, Foot; "LeftFoot"), and a word L or
	// Left, R or Right decides. Both, or neither, says nothing.
	inline int SideFromName(std::string_view a_name)
	{
		bool   left = false;
		bool   right = false;
		size_t start = 0;
		const auto word = [&](size_t a_end) {
			if (a_end <= start) {
				return;
			}
			const auto w = a_name.substr(start, a_end - start);
			const auto is = [&](std::string_view a_lowered) {
				return w.size() == a_lowered.size() &&
				       std::equal(w.begin(), w.end(), a_lowered.begin(), [](char a_c, char a_l) {
						   return std::tolower(static_cast<unsigned char>(a_c)) == a_l;
					   });
			};
			left = left || is("l") || is("left");
			right = right || is("r") || is("right");
		};
		for (size_t i = 0; i < a_name.size(); ++i) {
			const auto c = static_cast<unsigned char>(a_name[i]);
			if (c == ' ' || c == '_' || c == '-' || c == '.' || c == ':') {
				word(i);
				start = i + 1;
				continue;
			}
			if (i > start && std::isupper(c)) {
				const auto previous = static_cast<unsigned char>(a_name[i - 1]);
				const bool nextLower = i + 1 < a_name.size() &&
				                       std::islower(static_cast<unsigned char>(a_name[i + 1]));
				if (std::islower(previous) || std::isdigit(previous) || (std::isupper(previous) && nextLower)) {
					word(i);
					start = i;
				}
			}
		}
		word(a_name.size());
		return left == right ? 0 : (left ? -1 : 1);
	}

	// How far a foot stands to the right of a point across the body (negative: left).
	inline float Lateral(float a_x, float a_y, float a_centreX, float a_centreY, float a_rightX, float a_rightY)
	{
		return (a_x - a_centreX) * a_rightX + (a_y - a_centreY) * a_rightY;
	}

	// The side a print is mirrored to: the name's when it says, else the offset's sign.
	inline float MirrorFor(int a_nameSide, float a_lateral)
	{
		if (a_nameSide != 0) {
			return a_nameSide < 0 ? -1.0f : 1.0f;
		}
		return a_lateral < 0.0f ? -1.0f : 1.0f;
	}

	// Whether an actor's real feet press, or synthetic steps stand in for them: real feet
	// while one of them was planted on the ground, as the body moved, within the last
	// kRealFeetTravel of the actor's own travel (a sprint stride is about 90). The player's
	// body in first person keeps a pose the game only now and then animates, so how the legs
	// move says little (in game a leg-motion rule still switched a dozen times in 10 s);
	// whether a foot reaches the ground planted is what a print needs. A frozen foot travels
	// with the body and never plants, even where it presses (snow tracks need no planted
	// foot). A planted foot brings the real feet back at once; standing still changes
	// nothing.
	inline constexpr float kRealFeetTravel = 96.0f;

	// The player is in first person when the camera sits in the head: within this many units
	// of the player across (where the player stood when that camera drew), and between
	// kFirstPersonEyeLow and kFirstPersonEyeHigh above the feet (crouching, standing, in power
	// armour). A third-person camera, even over the shoulder, sits much further back. Once in
	// first person it lasts until the camera has been out of the head kFirstPersonLeaveFrames
	// frames running, so a step or a head bob never flips it.
	inline constexpr float    kFirstPersonReach = 32.0f;
	inline constexpr uint32_t kFirstPersonLeaveFrames = 10;
	inline constexpr float kFirstPersonEyeLow = 30.0f;
	inline constexpr float kFirstPersonEyeHigh = 220.0f;

	struct FirstPersonState
	{
		bool     on{ false };
		uint32_t outFrames{ 0 };
	};

	// One frame's verdict into the held state: in at once, out only after a run of frames.
	inline bool UpdateFirstPerson(FirstPersonState& a_state, bool a_inHead)
	{
		if (a_inHead) {
			a_state.on = true;
			a_state.outFrames = 0;
		} else if (a_state.on && ++a_state.outFrames >= kFirstPersonLeaveFrames) {
			a_state.on = false;
			a_state.outFrames = 0;
		}
		return a_state.on;
	}

	inline bool CameraInHead(float a_eyeX, float a_eyeY, float a_eyeZ, float a_feetX, float a_feetY, float a_feetZ)
	{
		const float dx = a_eyeX - a_feetX;
		const float dy = a_eyeY - a_feetY;
		const float up = a_eyeZ - a_feetZ;
		return std::isfinite(dx + dy + up) && dx * dx + dy * dy <= kFirstPersonReach * kFirstPersonReach &&
		       up >= kFirstPersonEyeLow && up <= kFirstPersonEyeHigh;
	}

	struct FeetMode
	{
		float sincePress{ 0.0f };
		bool  synthetic{ false };
	};

	inline bool UpdateFeetMode(FeetMode& a_mode, bool a_realPressed, float a_bodyStep)
	{
		if (a_realPressed) {
			a_mode.sincePress = 0.0f;
		} else if (a_bodyStep > 0.01f) {
			a_mode.sincePress = std::min(a_mode.sincePress + a_bodyStep, 2.0f * kRealFeetTravel);
		}
		a_mode.synthetic = a_mode.sincePress >= kRealFeetTravel;
		return a_mode.synthetic;
	}

	// A toe this close to its ankle (seen from above) says nothing about the way the foot
	// points: a foot on tiptoe, or a toe bone sitting on the ankle.
	inline constexpr float kMinToeReach = 2.0f;

	// The horizontal way a foot points, ankle to toe; false leaves the caller's heading.
	inline bool ToeHeading(float a_ankleX, float a_ankleY, float a_toeX, float a_toeY,
		float& a_outX, float& a_outY)
	{
		const float dx = a_toeX - a_ankleX;
		const float dy = a_toeY - a_ankleY;
		const float length = std::sqrt(dx * dx + dy * dy);
		if (!(length > kMinToeReach)) {
			return false;
		}
		a_outX = dx / length;
		a_outY = dy / length;
		return true;
	}

	// How high over the ground a foot still counts as down. An actor's own contact height, a
	// person's at the least (a child's ankle stands nearly as high as a grown-up's), and half
	// again for a slope, where the ankle stands over higher ground than the sole. FRIK's feet
	// three times the actor's: their ankles stand 13 to 35 units over the ground, an NPC's 7.
	// Measured on recorded walks in VR: 99% of NPC steps on the land and 281 of 286 FRIK
	// steps come down within these (the rest jumps).
	inline float FootContact(float a_contact, float a_personContact, bool a_visibleFeet)
	{
		return a_visibleFeet ? a_contact * 3.0f : std::max(a_contact, a_personContact) * 1.5f;
	}

	// How much more a creature bigger than a person presses: deeper prints (into snow too),
	// higher rims, more kicked-up snow. 1 up to kHeavyFrom times a person's size (super
	// mutants, ghouls), then the square root of the size over it: a deathclaw (size 3.2)
	// about 1.6, at most kMaxHeavy (a behemoth, a mirelurk queen).
	inline constexpr float kHeavyFrom = 1.25f;
	inline constexpr float kMaxHeavy = 2.0f;

	inline float Heavy(float a_sizeScale)
	{
		return std::clamp(std::sqrt(std::max(a_sizeScale, 0.0f) / kHeavyFrom), 1.0f, kMaxHeavy);
	}

	// Whether an actor stands on the ground: its base (the 3D root) within a contact height of
	// the land, a road or path the snow buries counting (a_buried), or inside the snow blanket
	// over the land (a_snowTop, the blanket's height there, 0 off snow), whatever it stands on
	// underneath. Pre-war Sanctuary walks 28 units over its land, under a 35-unit blanket.
	inline constexpr float kSnowTopMargin = 2.0f;

	inline bool OnGround(float a_baseOverLand, float a_buried, float a_snowTop, float a_contact, float a_reach)
	{
		if (a_baseOverLand < -a_reach) {
			return false;
		}
		return a_baseOverLand - a_buried <= a_contact || (a_snowTop > 0.0f && a_baseOverLand <= a_snowTop + kSnowTopMargin);
	}

	// A visible foot (FRIK's) comes down once per step: on the first slow update after it
	// swung. A foot that only drifts on the ground (FRIK settling it as the head moves) never
	// swings, so it never comes down again.
	inline bool Landed(std::uint8_t& a_swinging, float a_speed, float a_swingSpeed)
	{
		if (a_speed > a_swingSpeed) {
			a_swinging = 1;
			return false;
		}
		const bool landed = a_swinging != 0;
		a_swinging = 0;
		return landed;
	}

	// Whether a node's name holds a lower-case word, in any case.
	inline bool NameHas(std::string_view a_name, std::string_view a_lowered)
	{
		if (a_lowered.empty() || a_lowered.size() > a_name.size()) {
			return false;
		}
		for (size_t at = 0; at + a_lowered.size() <= a_name.size(); ++at) {
			size_t i = 0;
			while (i < a_lowered.size() &&
				   std::tolower(static_cast<unsigned char>(a_name[at + i])) == static_cast<unsigned char>(a_lowered[i])) {
				++i;
			}
			if (i == a_lowered.size()) {
				return true;
			}
		}
		return false;
	}

	// What a body touches the ground with, read off its node names: the first of these it has.
	// Feet: the first node down each branch holding a foot word (LLeg_Foot, a dog's toes, a
	// protectron's LFootBiped); a person's toes hang under the foot and are skipped. Ankles: a
	// yao guai's or a mirelurk king's digits hang under one, with no foot bone. Wheels: a sentry
	// bot's three, a robobrain's tread wheels. Leg ends: the last bone of each leg (a radroach, a
	// fog crawler). A robot's parts bring their own bones: a part's root ("LegsHandyThruster.nif")
	// and connect points ("C-BotLegs") are no limbs, "Legs" names a part and not a bone, and a
	// thruster part means the robot flies on a jet.
	enum class Contact : uint8_t
	{
		kNone,
		kFeet,
		kAnkles,
		kWheels,
		kLegEnds,
	};

	inline const char* ContactName(Contact a_contact)
	{
		switch (a_contact) {
		case Contact::kFeet:
			return "feet";
		case Contact::kAnkles:
			return "ankles";
		case Contact::kWheels:
			return "wheels";
		case Contact::kLegEnds:
			return "leg ends";
		default:
			return "nothing";
		}
	}

	template <class Node>
	struct Contacts
	{
		Contact            kind{ Contact::kNone };
		std::vector<Node*> nodes;
		// Each node's tip, or null: a foot's toe, an ankle's middle digit (the way it points, and
		// where an ankle's paw comes down).
		std::vector<Node*> tips;
		bool               thruster{ false };
	};

	// Walks a body from its 3D root, which is never a limb whatever it is named. a_name(node)
	// gives a node's name; a_children(node, visit) calls visit(child) for each child node.
	template <class Node, class NameFn, class ChildrenFn>
	class ContactWalk
	{
	public:
		ContactWalk(std::span<const std::string> a_footWords, size_t a_max, NameFn a_name, ChildrenFn a_children) :
			_footWords(a_footWords), _max(a_max), _name(std::move(a_name)), _children(std::move(a_children))
		{}

		Contacts<Node> Run(Node* a_root)
		{
			Contacts<Node> out;
			if (!a_root) {
				return out;
			}
			Walk(a_root, 0, false, false);
			out.thruster = _thruster;
			for (size_t kind = 0; kind < _found.size(); ++kind) {
				if (!_found[kind].empty()) {
					out.kind = static_cast<Contact>(kind + 1);
					out.nodes = std::move(_found[kind]);
					out.tips = std::move(_tips[kind]);
					break;
				}
			}
			return out;
		}

	private:
		static constexpr uint32_t kMaxDepth = 96;
		static constexpr uint32_t kMaxTipDepth = 6;

		bool IsFoot(std::string_view a_name) const
		{
			return std::ranges::any_of(_footWords, [&](const std::string& a_word) { return NameHas(a_name, a_word); });
		}

		void Add(size_t a_kind, Node* a_node, Node* a_tip)
		{
			if (_found[a_kind].size() < _max) {
				_found[a_kind].push_back(a_node);
				_tips[a_kind].push_back(a_tip);
			}
		}

		// The first node down a foot's branches whose name holds "toe".
		Node* Toe(Node* a_foot, uint32_t a_depth)
		{
			Node* found = nullptr;
			if (a_depth > kMaxTipDepth) {
				return found;
			}
			_children(a_foot, [&](Node* a_child) {
				if (!found && !IsCloneNode(_name(a_child))) {
					found = NameHas(_name(a_child), "toe") ? a_child : Toe(a_child, a_depth + 1);
				}
			});
			return found;
		}

		// An ankle's middle digit, down to its end: the middle child at each step.
		Node* Digit(Node* a_ankle)
		{
			Node*              at = a_ankle;
			std::vector<Node*> below;
			for (uint32_t depth = 0; depth < kMaxTipDepth; ++depth) {
				below.clear();
				_children(at, [&](Node* a_child) {
					if (!IsCloneNode(_name(a_child))) {
						below.push_back(a_child);
					}
				});
				if (below.empty()) {
					break;
				}
				at = below[below.size() / 2];
			}
			return at == a_ankle ? nullptr : at;
		}

		// Whether a_node, or a node under it, is a leg bone.
		bool Walk(Node* a_node, uint32_t a_depth, bool a_inAnkle, bool a_inWheel)
		{
			const std::string_view name = _name(a_node);
			if (a_depth > kMaxDepth || IsCloneNode(name)) {
				return false;
			}
			if (a_depth > 0 && NameHas(name, "thruster")) {
				_thruster = true;
			}
			const bool limb = a_depth > 0 && !name.starts_with("C-") && !NameHas(name, ".nif");
			const bool leg = limb && NameHas(name, "leg") && !NameHas(name, "legs");
			if (limb && IsFoot(name)) {
				Add(0, a_node, Toe(a_node, 0));
				return leg;
			}
			const bool ankle = limb && !a_inAnkle && NameHas(name, "ankle");
			const bool wheel = limb && !a_inWheel && NameHas(name, "wheel");
			if (ankle) {
				Add(1, a_node, Digit(a_node));
			}
			if (wheel) {
				Add(2, a_node, nullptr);
			}
			bool legBelow = false;
			_children(a_node, [&](Node* a_child) {
				legBelow = Walk(a_child, a_depth + 1, a_inAnkle || ankle, a_inWheel || wheel) || legBelow;
			});
			if (leg && !legBelow) {
				Add(3, a_node, nullptr);
			}
			return leg || legBelow;
		}

		std::span<const std::string>      _footWords;
		size_t                            _max;
		NameFn                            _name;
		ChildrenFn                        _children;
		std::array<std::vector<Node*>, 4> _found{};
		std::array<std::vector<Node*>, 4> _tips{};
		bool                              _thruster{ false };
	};

	template <class Node, class NameFn, class ChildrenFn>
	Contacts<Node> FindContacts(Node* a_root, std::span<const std::string> a_footWords, size_t a_max, NameFn a_name,
		ChildrenFn a_children)
	{
		return ContactWalk<Node, NameFn, ChildrenFn>(a_footWords, a_max, std::move(a_name), std::move(a_children)).Run(a_root);
	}
}
