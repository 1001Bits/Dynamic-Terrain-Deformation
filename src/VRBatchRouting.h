// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.
#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace VRBatchRouting
{
	// The native submitter uses the first packet in each group to choose between its
	// packed draw and RenderPassImmediately. The caller must join packet preparation
	// and keep the arrays alive/exclusive until this scope has restored their pointers.
	class ScopedFallback
	{
	public:
		ScopedFallback() = default;
		ScopedFallback(const ScopedFallback&) = delete;
		ScopedFallback& operator=(const ScopedFallback&) = delete;
		~ScopedFallback()
		{
			for (const auto& [slot, value] : _saved) *slot = value;
		}

		template <class Predicate>
		bool Select(std::span<const std::uint32_t> a_ends, std::span<void*> a_packets,
			Predicate&& a_needsFallback)
		{
			// Validate the whole index list before changing anything or reading a pass.
			if (!_saved.empty()) return false;
			std::uint32_t first = 0;
			for (const auto end : a_ends) {
				if (end <= first || end > a_packets.size()) return false;
				first = end;
			}
			first = 0;
			for (const auto end : a_ends) {
				if (a_packets[first]) {
					for (auto i = first; i < end; ++i) {
						if (!a_needsFallback(i)) continue;
						// Save before mutation: allocation failure leaves this pointer intact.
						_saved.emplace_back(&a_packets[first], a_packets[first]);
						a_packets[first] = nullptr;
						break;
					}
				}
				first = end;
			}
			return true;
		}

		std::size_t Count() const { return _saved.size(); }

	private:
		std::vector<std::pair<void**, void*>> _saved;
	};
}
