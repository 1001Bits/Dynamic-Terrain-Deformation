// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "GameLayouts.h"

#include "Offsets.h"
#include "Settings.h"
#include "Weather.h"
#include "WinterLook.h"

#include <algorithm>
#include <cmath>

namespace Weather
{
	namespace
	{
		float g_level{ 0.0f };

		float g_previousHours{ -1.0f };

		float         g_precipitation{ 0.0f };
		float         g_drying{ 0.0f };
		std::uint32_t g_weatherFormID{ 0 };

		const char* g_weatherKind{ "none" };

		char g_stateLine[192]{ "weather: not yet sampled" };

		float         g_loggedLevel{ 9.0f };
		std::uint32_t g_loggedWeather{ 0xFFFFFFFF };
		const char*   g_loggedKind{ nullptr };

		constexpr float kMaxIntegratedHours = 24.0f;

		// TESWeather's DATA block, byte 11: the weather classification.
		enum Classification : std::uint8_t
		{
			kPleasant = 0x01,
			kCloudy = 0x02,
			kRainy = 0x04,
			kSnow = 0x08,
		};

		std::uint8_t ClassOf(const RE::TESWeather* a_weather)
		{
			return a_weather ? static_cast<std::uint8_t>(a_weather->weatherData[11]) : 0;
		}

		const char* KindOf(const RE::TESWeather* a_weather)
		{
			if (!a_weather) {
				return "none";
			}
			const auto flags = ClassOf(a_weather);
			if (flags & kSnow) {
				return "snow";
			}
			if (flags & kRainy) {
				return "rain";
			}
			if (flags & kPleasant) {
				return "clear";
			}
			return "cloudy";
		}

		void Classify(const RE::TESWeather* a_weather, float& a_wet, float& a_dry)
		{
			a_wet = 0.0f;
			a_dry = 0.0f;
			if (!a_weather) {
				return;
			}

			const auto flags = ClassOf(a_weather);
			if (flags & kSnow) {
				a_wet = std::max(Settings::weatherSnowSoften, 0.0f);
			} else if (flags & kRainy) {
				a_wet = std::max(Settings::weatherRainSoften, 0.0f);
			} else if (flags & kPleasant) {
				a_dry = 1.0f;
			} else {
				a_dry = std::clamp(Settings::weatherCloudyFirmScale, 0.0f, 1.0f);
			}
		}

		RE::Sky* GetSky()
		{
			using func_t = RE::Sky* (*)();
			static const auto address = Offsets::Address(Offsets::SkyGetSingleton);
			return address ? reinterpret_cast<func_t>(address)() : nullptr;
		}
	}

	RE::Sky* Sky()
	{
		return GetSky();
	}

	float Level()
	{
		return Settings::enableWeather ? g_level : 0.0f;
	}

	float Precipitation()
	{
		return Settings::enableWeather ? g_precipitation : 0.0f;
	}

	namespace
	{
		float Deflect(float a_soft, float a_firm)
		{
			const float level = Level();
			return level >= 0.0f ? std::lerp(1.0f, a_soft, level) : std::lerp(1.0f, a_firm, -level);
		}
	}

	float DepthScale()
	{
		return Deflect(std::max(Settings::weatherSoftDepthScale, 0.0f),
			std::max(Settings::weatherFirmDepthScale, 0.0f));
	}

	float DecayScale()
	{
		return Deflect(std::max(Settings::weatherSoftDecayScale, 0.0f),
			std::max(Settings::weatherFirmDecayScale, 0.0f));
	}

	float RaiseScale()
	{
		return Settings::snowRaiseWeather ? std::clamp(0.5f + 0.5f * Level(), 0.0f, 1.0f) : 1.0f;
	}

	float SnowDepth()
	{
		if (!Settings::enableSnowRaise || !Settings::useClipmap) {
			return 0.0f;
		}
		return std::max(Settings::snowRaiseHeight, 0.0f) * RaiseScale();
	}

	float FillPerSecond()
	{
		if (!Settings::enableWeather) {
			return 0.0f;
		}
		// As fast as the weather softens the ground: snowfall (WeatherSnowSoften 1) half fills a
		// mark in about 3 minutes, rain (WeatherRainSoften 8) eight times as fast.
		return std::clamp(g_precipitation * Settings::weatherFillPerSecond, 0.0f, 0.99f);
	}

	namespace
	{
		void ComposeStateLine()
		{
			std::snprintf(g_stateLine, sizeof(g_stateLine),
				"Weather %s %08X | precip %.2f dry %.2f | level %+.2f | depth x%.2f decay x%.2f | fill %.3f/s",
				g_weatherKind, g_weatherFormID, g_precipitation, g_drying, Level(), DepthScale(),
				DecayScale(), FillPerSecond());
		}

		void MaybeLog()
		{
			if (!Settings::logWeather) {
				return;
			}

			const float stepped = std::round(Level() * 20.0f) / 20.0f;
			if (stepped == g_loggedLevel && g_weatherFormID == g_loggedWeather && g_weatherKind == g_loggedKind) {
				return;
			}
			g_loggedLevel = stepped;
			g_loggedWeather = g_weatherFormID;
			g_loggedKind = g_weatherKind;

			ComposeStateLine();
			logger::info("{}", g_stateLine);
		}
	}

	void Reset()
	{
		g_level = 0.0f;
		g_previousHours = -1.0f;
		g_precipitation = 0.0f;
		g_drying = 0.0f;
		g_weatherFormID = 0;
		g_weatherKind = "none";
		g_loggedLevel = 9.0f;
		g_loggedWeather = 0xFFFFFFFF;
		g_loggedKind = nullptr;
		std::snprintf(g_stateLine, sizeof(g_stateLine), "Weather: reset");
	}

	void Update()
	{
		if (!Settings::enableWeather) {
			g_precipitation = 0.0f;
			g_drying = 0.0f;
			g_previousHours = -1.0f;
			return;
		}

		const auto* calendar = Offsets::ReadGlobalPointer<RE::Calendar>(Offsets::CalendarSingleton);
		const auto* sky = GetSky();
		if (!calendar || !sky || GameLayouts::SkyMode(sky) != static_cast<std::uint32_t>(RE::Sky::Mode::kFull)) {
			g_precipitation = 0.0f;
			g_drying = 0.0f;
			// Indoors (no full sky) the outdoor weather is not seen: the clock starts over
			// on the way out instead of integrating the hours spent inside.
			g_previousHours = -1.0f;
			return;
		}

		const float hours = calendar->GetHoursPassed();

		float currentWet = 0.0f;
		float currentDry = 0.0f;
		Classify(sky->currentWeather, currentWet, currentDry);

		float lastWet = currentWet;
		float lastDry = currentDry;
		if (sky->lastWeather) {
			Classify(sky->lastWeather, lastWet, lastDry);
		}

		const float pct = std::clamp(GameLayouts::SkyCurrentWeatherPct(sky), 0.0f, 1.0f);
		g_precipitation = std::lerp(lastWet, currentWet, pct);
		g_drying = std::lerp(lastDry, currentDry, pct);
		g_weatherFormID = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
		g_weatherKind = KindOf(sky->currentWeather);

		if (g_previousHours < 0.0f) {
			g_previousHours = hours;
			MaybeLog();
			return;
		}

		float deltaHours = hours - g_previousHours;
		g_previousHours = hours;
		if (deltaHours <= 0.0f) {
			MaybeLog();
			return;
		}
		deltaHours = std::min(deltaHours, kMaxIntegratedHours);

		const float rate = g_precipitation / std::max(Settings::weatherSoftenHours, 0.05f) -
		                   g_drying / std::max(Settings::weatherFirmHours, 0.05f);

		g_level = std::clamp(g_level + rate * deltaHours, -1.0f, 1.0f);

		MaybeLog();
	}

	const char* StateLine()
	{
		ComposeStateLine();
		return g_stateLine;
	}

	float Snowfall()
	{
		const auto* sky = GetSky();
		if (!sky || GameLayouts::SkyMode(sky) != static_cast<std::uint32_t>(RE::Sky::Mode::kFull)) {
			return 0.0f;
		}

		// A winter mod that whitens the ground in place keeps the game's rain weathers: under
		// WinterMode their rain is taken for snow (-1: where most of the ground looks like
		// snow).
		const bool winter = Settings::winterMode < 0 ? WinterLook::MostlySnowy() : Settings::winterMode > 0;
		const auto fallOf = [winter](const RE::TESWeather* a_weather) {
			const auto flags = ClassOf(a_weather);
			if (flags & kSnow) {
				return 1.0f;
			}
			return winter && (flags & kRainy) ? 1.0f : 0.0f;
		};
		const float current = fallOf(sky->currentWeather);
		const float last = sky->lastWeather ? fallOf(sky->lastWeather) : current;
		return std::lerp(last, current, std::clamp(GameLayouts::SkyCurrentWeatherPct(sky), 0.0f, 1.0f));
	}
}
