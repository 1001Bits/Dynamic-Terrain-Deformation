// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace ShelterTransition
{
	inline float Alpha(float dt)
	{
		if (!std::isfinite(dt) || dt <= 0.0f) { return 0.0f; }
		return -std::expm1(-std::min(dt, 0.1f) / 0.35f);
	}

	inline bool Advance(float& value, uint8_t target, float alpha)
	{
		value += (static_cast<float>(target) - value) * alpha;
		if (std::abs(value - target) < 0.01f) {
			value = target;
			return false;
		}
		return true;
	}
}

// The shelter and snow-coverage maps are square byte maps over world cells, addressed
// toroidally (texel = cell & mask on each axis), and box-smoothed after texels change. A
// frame usually changes a handful of texels or one strip, so only the cells a change can
// reach are refiltered.
namespace MapFilter
{
	// Inclusive range of world cells; empty when x1 < x0.
	struct CellRect
	{
		int32_t x0{ 0 };
		int32_t y0{ 0 };
		int32_t x1{ -1 };
		int32_t y1{ -1 };

		[[nodiscard]] bool Empty() const { return x1 < x0 || y1 < y0; }

		[[nodiscard]] int64_t Area() const
		{
			return Empty() ? 0 : static_cast<int64_t>(x1 - x0 + 1) * static_cast<int64_t>(y1 - y0 + 1);
		}

		void Add(int32_t a_x, int32_t a_y) { Add(CellRect{ a_x, a_y, a_x, a_y }); }

		void Add(const CellRect& a_other)
		{
			if (a_other.Empty()) {
				return;
			}
			if (Empty()) {
				*this = a_other;
				return;
			}
			x0 = std::min(x0, a_other.x0);
			y0 = std::min(y0, a_other.y0);
			x1 = std::max(x1, a_other.x1);
			y1 = std::max(y1, a_other.y1);
		}

		[[nodiscard]] CellRect Grown(int32_t a_by) const
		{
			return Empty() ? *this : CellRect{ x0 - a_by, y0 - a_by, x1 + a_by, y1 + a_by };
		}

		[[nodiscard]] CellRect Clipped(const CellRect& a_to) const
		{
			const CellRect out{ std::max(x0, a_to.x0), std::max(y0, a_to.y0),
				std::min(x1, a_to.x1), std::min(y1, a_to.y1) };
			return out.Empty() ? CellRect{} : out;
		}
	};

	// Records a region in a short list: grows the last entry when the new region touches
	// it, and merges everything into one rectangle once the list is full.
	inline void AddRect(std::vector<CellRect>& a_list, const CellRect& a_rect, size_t a_maxRects = 32)
	{
		if (a_rect.Empty()) {
			return;
		}
		if (!a_list.empty()) {
			auto& last = a_list.back();
			if (!last.Grown(1).Clipped(a_rect).Empty()) {
				last.Add(a_rect);
				return;
			}
		}
		if (a_list.size() >= a_maxRects) {
			CellRect all = a_rect;
			for (const auto& rect : a_list) {
				all.Add(rect);
			}
			a_list.assign(1, all);
			return;
		}
		a_list.push_back(a_rect);
	}

	// Box-filters a_src into a_dst (radius a_radius, wrapping at the map's edges) over the
	// cells of a_rect, limited to one period of the map. Filtering the whole period gives
	// exactly the full-map filter. a_scratch is reused between calls.
	inline void BoxRect(const std::vector<uint8_t>& a_src, std::vector<uint8_t>& a_dst,
		uint32_t a_texels, int a_radius, CellRect a_rect, std::vector<uint32_t>& a_scratch)
	{
		if (a_rect.Empty() || a_texels == 0 || a_src.size() < static_cast<size_t>(a_texels) * a_texels ||
			a_dst.size() < a_src.size()) {
			return;
		}

		const int32_t  n = static_cast<int32_t>(a_texels);
		const uint32_t mask = a_texels - 1;
		a_rect.x1 = std::min(a_rect.x1, a_rect.x0 + n - 1);
		a_rect.y1 = std::min(a_rect.y1, a_rect.y0 + n - 1);

		const auto at = [&](int32_t a_x, int32_t a_y) {
			return (static_cast<size_t>(static_cast<uint32_t>(a_y) & mask) * a_texels) +
			       (static_cast<uint32_t>(a_x) & mask);
		};

		const int r = std::clamp(a_radius, 0, n / 4);
		if (r <= 0) {
			for (int32_t y = a_rect.y0; y <= a_rect.y1; ++y) {
				for (int32_t x = a_rect.x0; x <= a_rect.x1; ++x) {
					a_dst[at(x, y)] = a_src[at(x, y)];
				}
			}
			return;
		}

		const int32_t w = a_rect.x1 - a_rect.x0 + 1;
		const int32_t h = a_rect.y1 - a_rect.y0 + 1;
		const int32_t rows = h + 2 * r;
		a_scratch.resize(static_cast<size_t>(w) * static_cast<size_t>(rows));

		// Horizontal sums of every row the vertical pass needs.
		for (int32_t j = 0; j < rows; ++j) {
			const int32_t y = a_rect.y0 - r + j;
			uint32_t*     out = &a_scratch[static_cast<size_t>(j) * w];

			uint32_t sum = 0;
			for (int k = -r; k <= r; ++k) {
				sum += a_src[at(a_rect.x0 + k, y)];
			}
			for (int32_t i = 0; i < w; ++i) {
				out[i] = sum;
				const int32_t x = a_rect.x0 + i;
				sum -= a_src[at(x - r, y)];
				sum += a_src[at(x + r + 1, y)];
			}
		}

		const uint32_t width = static_cast<uint32_t>(2 * r + 1);
		const uint32_t area = width * width;
		for (int32_t i = 0; i < w; ++i) {
			uint32_t sum = 0;
			for (int32_t j = 0; j < 2 * r + 1; ++j) {
				sum += a_scratch[static_cast<size_t>(j) * w + i];
			}
			for (int32_t j = 0; j < h; ++j) {
				a_dst[at(a_rect.x0 + i, a_rect.y0 + j)] = static_cast<uint8_t>(sum / area);
				sum -= a_scratch[static_cast<size_t>(j) * w + i];
				if (j + 2 * r + 1 < rows) {
					sum += a_scratch[static_cast<size_t>(j + 2 * r + 1) * w + i];
				}
			}
		}
	}

	// The lowest value within a_radius of each cell of a_rect (wrapping like BoxRect), so a
	// box filter after it spreads a low cap outwards instead of averaging it away.
	inline void MinRect(const std::vector<uint8_t>& a_src, std::vector<uint8_t>& a_dst,
		uint32_t a_texels, int a_radius, CellRect a_rect)
	{
		if (a_rect.Empty() || a_texels == 0 || a_src.size() < static_cast<size_t>(a_texels) * a_texels ||
			a_dst.size() < a_src.size()) {
			return;
		}

		const int32_t  n = static_cast<int32_t>(a_texels);
		const uint32_t mask = a_texels - 1;
		a_rect.x1 = std::min(a_rect.x1, a_rect.x0 + n - 1);
		a_rect.y1 = std::min(a_rect.y1, a_rect.y0 + n - 1);

		const auto at = [&](int32_t a_x, int32_t a_y) {
			return (static_cast<size_t>(static_cast<uint32_t>(a_y) & mask) * a_texels) +
			       (static_cast<uint32_t>(a_x) & mask);
		};

		const int r = std::clamp(a_radius, 0, n / 4);
		for (int32_t y = a_rect.y0; y <= a_rect.y1; ++y) {
			for (int32_t x = a_rect.x0; x <= a_rect.x1; ++x) {
				uint8_t low = a_src[at(x, y)];
				for (int dy = -r; dy <= r; ++dy) {
					for (int dx = -r; dx <= r; ++dx) {
						low = std::min(low, a_src[at(x + dx, y + dy)]);
					}
				}
				a_dst[at(x, y)] = low;
			}
		}
	}

	// Offsets (dx, dy) of an n x n window around its centre cell, nearest first, so a scan
	// that starts over after a reset or a jump classifies the ground under the player first.
	// The window spans [-n/2, n/2 - 1] on each axis, as the maps' base = centre - n/2.
	inline std::vector<int16_t> NearestFirst(uint32_t a_texels)
	{
		const int32_t        half = static_cast<int32_t>(a_texels / 2);
		std::vector<int16_t> pairs;
		pairs.reserve(static_cast<size_t>(a_texels) * a_texels * 2);

		std::vector<std::pair<int32_t, int32_t>> order;
		order.reserve(static_cast<size_t>(a_texels) * a_texels);
		for (int32_t dy = -half; dy < half; ++dy) {
			for (int32_t dx = -half; dx < half; ++dx) {
				order.emplace_back(dx, dy);
			}
		}
		std::stable_sort(order.begin(), order.end(), [](const auto& a_lhs, const auto& a_rhs) {
			return a_lhs.first * a_lhs.first + a_lhs.second * a_lhs.second <
			       a_rhs.first * a_rhs.first + a_rhs.second * a_rhs.second;
		});
		for (const auto& [dx, dy] : order) {
			pairs.push_back(static_cast<int16_t>(dx));
			pairs.push_back(static_cast<int16_t>(dy));
		}
		return pairs;
	}
}
