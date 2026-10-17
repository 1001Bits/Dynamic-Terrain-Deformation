// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include "Clipmap.h"
#include "CorpseRule.h"

#include <array>
#include <span>
#include <vector>

namespace RE
{
	class Actor;
}

// Marks under bodies (CorpseRule has the rules): a corpse at rest lies in a hollow of its own
// shape in the snow, or on a shallow print in other soft ground, and snowfall buries it.
namespace Corpses
{
	// Whether the life state can be read on this runtime (verified per runtime; see
	// FO4_PORT_NOTES). Without it no actor counts as dead and nothing here runs.
	bool Supported();

	// Whether an actor lies dead (dying, dead or recycled, as Actor::IsDead(true) answers).
	// Its feet press nothing then: the body is marked here instead.
	bool IsDead(const RE::Actor* a_actor);

	// Whether an actor's melee attack runs (the attack state beside the life state; the same
	// runtimes as Supported).
	bool InMeleeAttack(const RE::Actor* a_actor);

	// The body parts of an actor (living or dead) where its skeleton is this frame, as
	// capsules (head, torso, limbs; its bound when the bones are not known). Returns how many.
	size_t BodyCapsules(RE::Actor* a_actor, std::array<CorpseRule::Capsule, CorpseRule::kMaxCapsules>& a_out);

	// Adds the marks of the nearest bodies in a_dead (nearest first) while the frame has room
	// left under the field's budget after the feet, impacts and objects already in a_out.
	void Append(float a_deltaSeconds, const RE::NiPoint3& a_anchor, std::span<RE::Actor* const> a_dead,
		std::vector<Clipmap::Stamp>& a_out);

	// Forgets every tracked body (a save load, a new worldspace).
	void Reset();

	// Lets the capped log lines print again (a settings reload).
	void ResetDiagnostics();
}
