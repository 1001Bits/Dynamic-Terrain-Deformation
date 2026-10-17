// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

// The player's synthetic steps in first person are not seen forming. A first-person view has
// no feet, so a print or a trench appearing under the camera while the player looks down
// gives the trick away (seen in game on snow and sand). Such a mark waits until its ground
// has left the view, the player has walked far enough on, or enough time has passed - then it
// is pressed exactly as it would have been. Apart from the engine types so the offline tests
// can check it.
namespace HiddenMarks
{
	// The camera a mark is hidden from. The extents are the view's half width and height at
	// one unit of depth; an off-centre (VR) projection widens them to cover both eyes.
	struct View
	{
		float eye[3]{};
		float forward[3]{ 0.0f, 1.0f, 0.0f };
		float right[3]{ 1.0f, 0.0f, 0.0f };
		float up[3]{ 0.0f, 0.0f, 1.0f };
		float halfX{ 1.0f };
		float halfY{ 1.0f };
		bool  valid{ false };
	};

	// Whether a swept sphere can be on screen. Reject only when both endpoints lie outside
	// the same expanded frustum plane: testing the two spheres separately misses a trench
	// crossing the screen with both ends outside it. Without a camera
	// nothing can be hidden from it, so nothing counts as in view (the marks press at once,
	// as they would without this).
	inline bool SegmentInView(const View& a_view, const float (&a_from)[3], const float (&a_to)[3], float a_radius)
	{
		if (!a_view.valid) {
			return false;
		}
		const auto along = [&](const float (&a_point)[3], const float (&a_axis)[3]) {
			return (a_point[0] - a_view.eye[0]) * a_axis[0] +
			       (a_point[1] - a_view.eye[1]) * a_axis[1] +
			       (a_point[2] - a_view.eye[2]) * a_axis[2];
		};
		const float radius = std::max(a_radius, 0.0f);
		const float depth0 = along(a_from, a_view.forward);
		const float depth1 = along(a_to, a_view.forward);
		if (depth0 < -radius && depth1 < -radius) {
			return false;
		}
		const auto outside = [&](const float (&a_axis)[3], float a_half) {
			const float p0 = along(a_from, a_axis), p1 = along(a_to, a_axis);
			const float margin = radius * std::sqrt(1.0f + a_half * a_half);
			return (p0 - depth0 * a_half > margin && p1 - depth1 * a_half > margin) ||
			       (-p0 - depth0 * a_half > margin && -p1 - depth1 * a_half > margin);
		};
		return !outside(a_view.right, a_view.halfX) && !outside(a_view.up, a_view.halfY);
	}

	inline bool InView(const View& a_view, const float (&a_point)[3], float a_radius)
	{
		return SegmentInView(a_view, a_point, a_point, a_radius);
	}

	enum class Release
	{
		kKeep,
		kOutOfView,
		kDistance,
		kAge,
	};

	// A mark covering the ground from a_from to a_to (the same point for a print): kept while
	// any part can be seen, released once the whole segment is out of view, once the player is
	// a_maxDistance away (0 = never for distance), or once it has waited a_maxAge seconds of
	// game time (0 = never for age), so nothing waits for ever.
	inline Release Decide(const View& a_view, const float (&a_from)[3], const float (&a_to)[3], float a_radius,
		float a_distance, float a_maxDistance, float a_age, float a_maxAge)
	{
		if (a_maxDistance > 0.0f && a_distance >= a_maxDistance) {
			return Release::kDistance;
		}
		if (a_maxAge > 0.0f && a_age >= a_maxAge) {
			return Release::kAge;
		}
		if (!SegmentInView(a_view, a_from, a_to, a_radius)) {
			return Release::kOutOfView;
		}
		return Release::kKeep;
	}

	// The marks waiting, oldest first, at most kCapacity of them: one past that pushes the
	// oldest out, to be pressed at once rather than lost.
	inline constexpr std::size_t kCapacity = 256;

	template <class T>
	struct Queue
	{
		std::deque<T> items;

		[[nodiscard]] std::optional<T> Push(T a_item)
		{
			std::optional<T> evicted;
			if (items.size() >= kCapacity) {
				evicted = std::move(items.front());
				items.pop_front();
			}
			items.push_back(std::move(a_item));
			return evicted;
		}
	};
}
