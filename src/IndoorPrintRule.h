// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Shoe prints on floors: boots pick up mud, snow or dust walking on the landscape and leave
// a fading trail of prints on the floors after it (a house, a vault, a porch). Interiors have
// no landscape to dent, so a print is only coloured onto the floor. These are the rules,
// apart from the engine types so the offline tests can check them; IndoorPrints.cpp applies
// them and draws the prints.
namespace IndoorPrintRule
{
	// What a pair of boots carries.
	enum class Dirt : std::uint8_t
	{
		kNone,
		kMud,
		kWet,  // snow and water: a dark, damp print that dries
		kDust,
	};

	struct Boots
	{
		Dirt  dirt{ Dirt::kNone };
		float level{ 0.0f };  // 0 clean to 1 fully loaded
	};

	// Walking on clean ground (stone, a road's land, the floor of a building outside) wipes
	// the boots over this much travel.
	inline constexpr float kCleanTravel = 640.0f;

	// One frame of walking on the landscape: a dirtier ground than the boots carry loads them
	// to its strength and its kind of dirt; anything cleaner slowly wipes them.
	inline void Walk(Boots& a_boots, Dirt a_dirt, float a_strength, float a_travel)
	{
		const float strength = std::clamp(a_strength, 0.0f, 1.0f);
		if (a_dirt != Dirt::kNone && strength > 0.0f && strength >= a_boots.level) {
			a_boots.dirt = a_dirt;
			a_boots.level = strength;
			return;
		}
		if (a_travel > 0.0f && a_boots.level > 0.0f) {
			a_boots.level = std::max(0.0f, a_boots.level - a_travel / kCleanTravel);
			if (a_boots.level <= 0.0f) {
				a_boots.dirt = Dirt::kNone;
			}
		}
	}

	// One step on a floor: how strong a print it leaves (0 = none), and the boots lose one
	// step's worth, clean after a_steps prints from fully loaded.
	inline float TakeStep(Boots& a_boots, int a_steps)
	{
		if (a_boots.dirt == Dirt::kNone || a_boots.level <= 0.0f) {
			return 0.0f;
		}
		const float print = a_boots.level;
		a_boots.level = std::max(0.0f, a_boots.level - 1.0f / static_cast<float>(std::max(a_steps, 1)));
		// Rounding leaves a sliver after the last step; that is clean too.
		if (a_boots.level <= 1e-4f) {
			a_boots.level = 0.0f;
			a_boots.dirt = Dirt::kNone;
		}
		return print;
	}

	// How long each kind lasts against the base fade: a damp print dries soonest, mud
	// stays longest.
	inline float FadeScale(Dirt a_dirt)
	{
		switch (a_dirt) {
		case Dirt::kWet:
			return 0.4f;
		case Dirt::kMud:
			return 1.5f;
		case Dirt::kDust:
			return 1.0f;
		default:
			return 0.0f;
		}
	}

	// A print's strength left after a_age seconds: full for the first half of its life, then
	// easing to nothing.
	inline float Fade(float a_age, float a_fadeSeconds, Dirt a_dirt)
	{
		const float life = std::max(a_fadeSeconds * FadeScale(a_dirt), 0.0f);
		if (!(life > 0.0f) || a_age >= life) {
			return 0.0f;
		}
		const float t = std::clamp((a_age - life * 0.5f) / (life * 0.5f), 0.0f, 1.0f);
		return 1.0f - t * t * (3.0f - 2.0f * t);
	}

	// The colour a print lays over the floor (display sRGB) and how much of it at full
	// strength: mud is dark and strong, snow melts into a pale grey slush that shows on dark
	// and light floors alike, dust is a light grey-brown.
	struct Tint
	{
		float r, g, b, alpha;
	};

	inline Tint TintFor(Dirt a_dirt)
	{
		switch (a_dirt) {
		case Dirt::kMud:
			return { 0.20f, 0.14f, 0.09f, 0.90f };
		case Dirt::kWet:
			return { 0.62f, 0.64f, 0.68f, 0.75f };
		case Dirt::kDust:
			return { 0.58f, 0.53f, 0.45f, 0.55f };
		default:
			return { 0.0f, 0.0f, 0.0f, 0.0f };
		}
	}

	// Outside in a snowy place a floor (steps, a plaza the winter mod paints white) is snow
	// too: a print there is pressed snow, a cool blue-grey shadow, not slush.
	inline constexpr Tint kPackedSnow{ 0.46f, 0.52f, 0.62f, 0.70f };

	// A print's box: a_halfLength along the foot, a_halfWidth across, kHalfHeight up and down
	// from the sole.
	inline constexpr float kHalfHeight = 4.0f;
	inline constexpr float kHalfLength = 13.0f;
	inline constexpr float kHalfWidth = 5.0f;

	struct Box
	{
		float x, y, z;
		float forwardX, forwardY;
		float halfLength, halfWidth;
		float mirror;
	};

	// A point's place in a print, -1 to 1 on each axis when inside: across (mirrored for a
	// left foot, as the terrain prints are), along and up. False outside the box.
	inline bool Local(const Box& a_box, float a_x, float a_y, float a_z, float& a_across, float& a_along, float& a_up)
	{
		const float dx = a_x - a_box.x;
		const float dy = a_y - a_box.y;
		// The terrain prints' frame (ClipmapUpdateCS PrintProfile): right = (-fy, fx).
		a_across = (dx * -a_box.forwardY + dy * a_box.forwardX) / std::max(a_box.halfWidth, 1e-3f) * a_box.mirror;
		a_along = (dx * a_box.forwardX + dy * a_box.forwardY) / std::max(a_box.halfLength, 1e-3f);
		a_up = (a_z - a_box.z) / kHalfHeight;
		return std::abs(a_across) <= 1.0f && std::abs(a_along) <= 1.0f && std::abs(a_up) <= 1.0f;
	}

	// The prints kept, oldest overwritten first once a_capacity are held.
	template <class T>
	struct Ring
	{
		std::vector<T> items;
		std::size_t    next{ 0 };
		std::size_t    limit{ 0 };

		void Push(const T& a_item, std::size_t a_capacity)
		{
			const std::size_t capacity = std::max<std::size_t>(a_capacity, 1);
			if (capacity != limit) {
				// Restore oldest-to-newest order before resizing. A wrapped ring that
				// grows must not append after a physical slot containing an older print.
				if (limit != 0 && items.size() == limit && !items.empty()) {
					std::rotate(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(next), items.end());
				}
				if (items.size() > capacity) {
					items.erase(items.begin(), items.end() - static_cast<std::ptrdiff_t>(capacity));
				}
				limit = capacity;
				next = items.size() % capacity;
			}
			if (items.size() < capacity) {
				items.push_back(a_item);
				next = items.size() % capacity;
				return;
			}
			items[next] = a_item;
			next = (next + 1) % capacity;
		}

		void Clear()
		{
			items.clear();
			next = 0;
			limit = 0;
		}
	};
}
