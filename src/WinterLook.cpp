// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "WinterLook.h"

#include "Settings.h"
#include "SurfaceTypes.h"

#include <unordered_map>

namespace WinterLook
{
	namespace
	{
		enum class State : uint8_t
		{
			kWaiting,   // known, not yet shown by a draw since
			kClaimed,   // a draw's texture was taken; the measurement is on its way
			kMeasured,
			kFailed,    // could not be measured: not snow
		};

		struct Entry
		{
			std::string path;
			State       state{ State::kWaiting };
			bool        snow{ false };
			uint8_t     attempts{ 0 };
			uint64_t    seen{ 0 };  // the frame it was last drawn, plus one; 0 never
		};

		std::mutex                          g_lock;
		std::unordered_map<uint64_t, Entry> g_entries;
		std::atomic<uint32_t>               g_waiting{ 0 };
		std::atomic<bool>                   g_mostlySnowy{ false };

		// A texture whose layer gave no texture to measure is tried this many times.
		constexpr uint8_t kMaxAttempts = 3;

		// MostlySnowy weighs the textures drawn in about the last ten seconds, once at least
		// this many of them are measured; nothing drawn (an interior) keeps what it was. It
		// turns on above the first share and off below the second, so a mixed area cannot
		// make it flicker.
		constexpr uint64_t kRecentFrames = 600;
		constexpr uint32_t kMinMeasured = 2;
		constexpr float    kSnowyOn = 0.5f;
		constexpr float    kSnowyOff = 0.35f;

		std::atomic<uint32_t> g_logged{ 0 };
		constexpr uint32_t    kMaxLogged = 256;

		uint64_t Hash(std::string_view a_text)
		{
			uint64_t hash = 14695981039346656037ull;
			for (const char c : a_text) {
				hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
			}
			return hash == 0 ? 1 : hash;  // 0 stands for no texture
		}
	}

	bool IsSnow(const Sample& a_sample)
	{
		return a_sample.white >= kSnowShare;
	}

	bool Enabled()
	{
		return Settings::winterMode < 0;
	}

	uint64_t KeyOf(std::string_view a_texturePath)
	{
		const auto key = Surfaces::TextureKey(a_texturePath);
		return key.empty() ? 0 : Hash(key);
	}

	uint64_t Register(std::string_view a_texturePath)
	{
		const auto key = Surfaces::TextureKey(a_texturePath);
		if (key.empty()) {
			return 0;
		}
		const uint64_t hash = Hash(key);

		const std::scoped_lock lock(g_lock);
		const auto [it, added] = g_entries.try_emplace(hash);
		if (added) {
			it->second.path = std::string(a_texturePath);
			g_waiting.fetch_add(1);
		}
		return hash;
	}

	bool LooksSnowy(std::string_view a_texturePath)
	{
		const uint64_t key = KeyOf(a_texturePath);
		if (!key) {
			return false;
		}
		const std::scoped_lock lock(g_lock);
		const auto it = g_entries.find(key);
		return it != g_entries.end() && it->second.snow;
	}

	bool MostlySnowy()
	{
		return g_mostlySnowy.load(std::memory_order_relaxed);
	}

	bool Waiting()
	{
		return g_waiting.load(std::memory_order_relaxed) > 0;
	}

	uint32_t Seen(const uint64_t (&a_keys)[4], uint64_t a_frame)
	{
		uint32_t claimed = 0;

		const std::scoped_lock lock(g_lock);
		for (uint32_t i = 0; i < 4; ++i) {
			if (!a_keys[i]) {
				continue;
			}
			const auto it = g_entries.find(a_keys[i]);
			if (it == g_entries.end()) {
				continue;
			}
			auto& entry = it->second;
			entry.seen = a_frame + 1;
			if (entry.state == State::kWaiting) {
				entry.state = State::kClaimed;
				g_waiting.fetch_sub(1);
				claimed |= 1u << i;
			}
		}
		return claimed;
	}

	bool Report(uint64_t a_key, const Sample& a_sample)
	{
		const bool  snow = IsSnow(a_sample);
		bool        changed = false;
		std::string path;
		{
			const std::scoped_lock lock(g_lock);
			const auto it = g_entries.find(a_key);
			if (it == g_entries.end()) {
				return false;
			}
			auto& entry = it->second;
			if (entry.state == State::kWaiting) {
				g_waiting.fetch_sub(1);
			}
			entry.state = State::kMeasured;
			changed = entry.snow != snow;
			entry.snow = snow;
			path = entry.path;
		}

		if (g_logged.fetch_add(1) < kMaxLogged) {
			logger::info("Winter look: {} | mean colour ({:.2f}, {:.2f}, {:.2f}), {:.0f}% white -> {}", path,
				a_sample.r, a_sample.g, a_sample.b, a_sample.white * 100.0f, snow ? "snow" : "not snow");
		}
		return changed;
	}

	void Abandon(uint64_t a_key)
	{
		std::string path;
		{
			const std::scoped_lock lock(g_lock);
			const auto it = g_entries.find(a_key);
			if (it == g_entries.end()) {
				return;
			}
			auto& entry = it->second;
			if (entry.state == State::kWaiting) {
				g_waiting.fetch_sub(1);
			}
			if (++entry.attempts < kMaxAttempts) {
				entry.state = State::kWaiting;
				g_waiting.fetch_add(1);
				return;
			}
			entry.state = State::kFailed;
			path = entry.path;
		}
		if (g_logged.fetch_add(1) < kMaxLogged) {
			logger::info("Winter look: {} | no texture to measure in {} draws - not snow", path,
				static_cast<int>(kMaxAttempts));
		}
	}

	bool Reevaluate(uint64_t a_frame)
	{
		uint32_t measured = 0;
		uint32_t snowy = 0;
		{
			const std::scoped_lock lock(g_lock);
			for (const auto& [key, entry] : g_entries) {
				if (entry.state != State::kMeasured || entry.seen == 0 || entry.seen + kRecentFrames <= a_frame) {
					continue;
				}
				++measured;
				snowy += entry.snow ? 1 : 0;
			}
		}
		if (measured < kMinMeasured) {
			return false;
		}

		const float share = static_cast<float>(snowy) / static_cast<float>(measured);
		const bool  was = g_mostlySnowy.load(std::memory_order_relaxed);
		const bool  now = was ? share >= kSnowyOff : share >= kSnowyOn;
		if (now == was) {
			return false;
		}
		g_mostlySnowy.store(now, std::memory_order_relaxed);
		logger::info("Winter look: {} of {} ground textures drawn lately look like snow - {}", snowy, measured,
			now ? "a winter world: the default ground is snow and rain falls as snow" : "not a winter world");
		return true;
	}

	void Forget()
	{
		const std::scoped_lock lock(g_lock);
		g_entries.clear();
		g_waiting.store(0);
		g_mostlySnowy.store(false);
		g_logged.store(0);
	}
}
