// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Clipmap.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SurfaceProfiles.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <tuple>

namespace Settings
{
	namespace
	{
		constexpr auto kPath = "Data/F4SE/Plugins/NMN_DeformableTerrain.ini";

		// What the player chose in the Mod Configuration Menu: MCM writes it here (its
		// defaults stay in Data/MCM/Config/NMN_DeformableTerrain/settings.ini, never read
		// here), read after the INI so a choice in the menu wins over the INI's line.
		constexpr auto kMcmPath = "Data/MCM/Settings/NMN_DeformableTerrain.ini";

		// Live reload. The INI and the profile folders are watched from a thread of their
		// own: under a virtual file system (MO2) each scan costs milliseconds the frame
		// must not pay. The frame thread only reads the flag and re-parses on a change.
		std::atomic<bool>  g_changed{ false };
		std::atomic<bool>  g_watching{ false };
		std::atomic<bool>  g_watchEnabled{ true };
		std::atomic<float> g_watchInterval{ 0.5f };

		std::filesystem::file_time_type WriteTime(const char* a_path)
		{
			std::error_code ec;
			const auto      when = std::filesystem::last_write_time(a_path, ec);
			return ec ? std::filesystem::file_time_type{} : when;
		}

		// Compare the revisions independently. Taking their maximum lets a newer profile
		// mask edits/deletion of the INI (including preserved/future timestamps).
		auto WriteTimeOrZero()
		{
			return std::tuple{ WriteTime(kPath), WriteTime(kMcmPath), Surfaces::ProfilesRevision() };
		}

		// A save in progress can leave a file empty for a moment.
		bool BeingWritten(const char* a_path)
		{
			std::error_code ec;
			const auto      size = std::filesystem::file_size(a_path, ec);
			return !ec && size == 0;
		}

		void Watch()
		{
			decltype(WriteTimeOrZero()) last{};
			try {
				last = WriteTimeOrZero();
			} catch (...) {
			}

			for (;;) {
				const float seconds = std::clamp(g_watchInterval.load(std::memory_order_relaxed), 0.05f, 10.0f);
				std::this_thread::sleep_for(std::chrono::duration<float>(seconds));
				if (!g_watchEnabled.load(std::memory_order_relaxed)) {
					continue;
				}

				try {
					const auto when = WriteTimeOrZero();
					if (when == last) {
						continue;
					}

					// Look again on the next pass rather than read half a file.
					if (BeingWritten(kPath) || BeingWritten(kMcmPath)) {
						continue;
					}

					last = when;
					g_changed.store(true, std::memory_order_release);
				} catch (...) {
					// Nothing here may take the game down; the next pass tries again.
				}
			}
		}

		void StartWatching()
		{
			bool expected = false;
			if (g_watching.compare_exchange_strong(expected, true)) {
				std::thread(Watch).detach();
			}
		}

		std::string Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return std::string(a_text.substr(first, last - first + 1));
		}

		float AsFloat(const std::string& a_key, const std::string& a_value, float a_fallback)
		{
			float parsed = 0.0f;
			if (!ParseFloat(a_value, parsed)) {
				logger::warn("{}: '{}' is not a number, using {}", a_key, a_value, a_fallback);
				return a_fallback;
			}
			return parsed;
		}

		float Clamped(const std::string& a_key, float a_value, float a_low, float a_high)
		{
			if (std::isnan(a_value)) {
				logger::warn("{}: not a number, using {}", a_key, a_low);
				return a_low;
			}
			const float out = std::clamp(a_value, a_low, a_high);
			if (out != a_value) {
				logger::warn("{}: {} is outside {} to {}, using {}", a_key, a_value, a_low, a_high, out);
			}
			return out;
		}

		std::string AsLower(std::string a_value)
		{
			std::transform(a_value.begin(), a_value.end(), a_value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_value;
		}

		bool AsBool(const std::string& a_value)
		{
			const std::string lowered = AsLower(a_value);
			return lowered == "1" || lowered == "true" || lowered == "yes";
		}

		std::string AsKeywords(const std::string& a_value, const std::string& a_fallback)
		{
			std::string out;
			out.reserve(a_value.size());
			for (const char c : AsLower(a_value)) {
				if (c != ' ' && c != '\t') {
					out += c;
				}
			}
			return out.empty() ? a_fallback : out;
		}

		void ParseResponse(const std::string& a_key, const std::string& a_value, Surfaces::Response& a_out)
		{
			float* fields[] = { &a_out.depthScale, &a_out.radiusScale, &a_out.shoulder,
				&a_out.decayScale, &a_out.rimScale, &a_out.print, &a_out.clearanceScale };
			static_assert(sizeof(fields) / sizeof(fields[0]) == Surfaces::kResponseFields);
			const auto&  lower = Surfaces::kResponseLow;
			const auto&  upper = Surfaces::kResponseHigh;
			const auto&  names = Surfaces::kResponseFieldNames;
			const size_t count = std::size(fields);

			size_t index = 0;
			size_t start = 0;
			while (index < count && start <= a_value.size()) {
				const auto comma = a_value.find(',', start);
				const auto end = comma == std::string::npos ? a_value.size() : comma;

				const auto piece = Trim(std::string_view(a_value).substr(start, end - start));
				if (!piece.empty()) {
					*fields[index] = Clamped(std::string(a_key) + " " + names[index],
						AsFloat(a_key, piece, *fields[index]), lower[index], upper[index]);
				}

				++index;
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
		}

		Surfaces::Response& Response(Surfaces::Type a_type)
		{
			return surfaceResponse[static_cast<size_t>(a_type)];
		}

		bool ApplyTessellationSetting(const std::string& key, const std::string& value)
		{
			if (key == "EnableTessellation") {
				enableTessellation = AsBool(value);
			} else if (key == "TessellationWinding") {
				tessellationWinding = AsLower(value) == "ccw" ? "ccw" : "cw";
			} else if (key == "LogGeneratedShaders") {
				logGeneratedShaders = AsBool(value);
			} else if (key == "TessellationMaxFactor") {
				tessellationMaxFactor = Clamped(key, AsFloat(key, value, tessellationMaxFactor), 1.0f, 64.0f);
			} else if (key == "TessellationTargetSpacing") {
				tessellationTargetSpacing = Clamped(key, AsFloat(key, value, tessellationTargetSpacing), 0.01f, 4096.0f);
			} else if (key == "TessellationRaiseSpacing") {
				tessellationRaiseSpacing = Clamped(key, AsFloat(key, value, tessellationRaiseSpacing), 0.0f, 4096.0f);
			} else if (key == "TessellationBlanketSpacing") {
				tessellationBlanketSpacing = Clamped(key, AsFloat(key, value, tessellationBlanketSpacing), 0.0f, 4096.0f);
			} else if (key == "EnableTessellationBounds") {
				enableTessellationBounds = AsBool(value);
			} else if (key == "TessellationBoundDepth") {
				tessellationBoundDepth = Clamped(key, AsFloat(key, value, tessellationBoundDepth), 0.0f, 256.0f);
			} else if (key == "RecomputeNormals") {
				recomputeNormals = AsBool(value);
			} else if (key == "NormalGradientEpsilon") {
				normalGradientEpsilon = Clamped(key, AsFloat(key, value, normalGradientEpsilon), 0.001f, 64.0f);
			} else if (key == "CullDistantDraws") {
				cullDistantDraws = AsBool(value);
			} else if (key == "EnablePatchCulling") {
				enablePatchCulling = AsBool(value);
			} else if (key == "CullMargin") {
				cullMargin = Clamped(key, AsFloat(key, value, cullMargin), 0.0f, 65536.0f);
			} else if (key == "RouteOnlyPlayerCamera") {
				routeOnlyPlayerCamera = AsBool(value);
			} else if (key == "AsyncShaderCompilation") {
				asyncShaderCompilation = AsBool(value);
			} else if (key == "DebugWorldZOffset") {
				debugWorldZOffset = Clamped(key, AsFloat(key, value, debugWorldZOffset), -4096.0f, 4096.0f);
			} else if (key == "DebugWaveAmplitude") {
				debugWaveAmplitude = Clamped(key, AsFloat(key, value, debugWaveAmplitude), -1024.0f, 1024.0f);
			} else if (key == "DebugWaveLength") {
				debugWaveLength = Clamped(key, AsFloat(key, value, debugWaveLength), 1.0f, 65536.0f);
			} else if (key == "DebugFieldColour" || key == "DebugFieldColor") {
				debugFieldColour = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(debugFieldColour)), 0.0f, 3.0f));
			} else if (key == "DebugBlendLayer") {
				debugBlendLayer = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(debugBlendLayer)), -1.0f, 3.0f));
			} else {
				return false;
			}
			return true;
		}

		bool ApplyMarkSetting(const std::string& key, const std::string& value)
		{
			if (key == "UseClipmap") {
				useClipmap = AsBool(value);
			} else if (key == "ClipmapLevels") {
				clipmapLevels = static_cast<uint32_t>(Clamped(key, AsFloat(key, value, static_cast<float>(clipmapLevels)), 1.0f, 2.0f));
			} else if (key == "ClipmapSeedSmoothing") {
				clipmapSeedSmoothing = Clamped(key, AsFloat(key, value, clipmapSeedSmoothing), 0.0f, 16.0f);
			} else if (key == "StampRadiusScale") {
				stampRadiusScale = Clamped(key, AsFloat(key, value, stampRadiusScale), 0.05f, 16.0f);
			} else if (key == "FootprintMetres") {
				footprintMetres = Clamped(key, AsFloat(key, value, footprintMetres), 1.0f, 200.0f);
			} else if (key == "StampFootReach") {
				stampFootReach = Clamped(key, AsFloat(key, value, stampFootReach), 0.0f, 4096.0f);
			} else if (key == "StampGroundClearance") {
				stampGroundClearance = Clamped(key, AsFloat(key, value, stampGroundClearance), 0.0f, 1024.0f);
			} else if (key == "StampDepth") {
				stampDepth = Clamped(key, AsFloat(key, value, stampDepth), 0.0f, 256.0f);
			} else if (key == "StampDecayPerSecond") {
				stampDecayPerSecond = Clamped(key, AsFloat(key, value, stampDecayPerSecond), 0.0f, 1.0f);
			} else if (key == "StampFootNodes") {
				stampFootNodes = AsKeywords(value, stampFootNodes);
			} else if (key == "StampFootRadius") {
				stampFootRadius = Clamped(key, AsFloat(key, value, stampFootRadius), 0.5f, 256.0f);
			} else if (key == "StampFootSizeReference") {
				stampFootSizeReference = Clamped(key, AsFloat(key, value, stampFootSizeReference), 1.0f, 4096.0f);
			} else if (key == "StampFootSizeMin") {
				stampFootSizeMin = Clamped(key, AsFloat(key, value, stampFootSizeMin), 0.05f, 16.0f);
			} else if (key == "StampFootSizeMax") {
				stampFootSizeMax = Clamped(key, AsFloat(key, value, stampFootSizeMax), 0.05f, 16.0f);
			} else if (key == "StampFootContactHeight") {
				stampFootContactHeight = Clamped(key, AsFloat(key, value, stampFootContactHeight), 0.0f, 512.0f);
			} else if (key == "StampFootPlantFraction") {
				stampFootPlantFraction = Clamped(key, AsFloat(key, value, stampFootPlantFraction), 0.0f, 8.0f);
			} else if (key == "StampFootPlantSpeed") {
				stampFootPlantSpeed = Clamped(key, AsFloat(key, value, stampFootPlantSpeed), 0.0f, 4096.0f);
			} else if (key == "SnowTracks") {
				snowTracks = AsBool(value);
			} else if (key == "StampSyntheticStepLength") {
				stampSyntheticStepLength = Clamped(key, AsFloat(key, value, stampSyntheticStepLength), 4.0f, 256.0f);
			} else if (key == "StampSyntheticStepPerSpeed") {
				stampSyntheticStepPerSpeed = Clamped(key, AsFloat(key, value, stampSyntheticStepPerSpeed), 0.0f, 2.0f);
			} else if (key == "StampSyntheticStepWidth") {
				stampSyntheticStepWidth = Clamped(key, AsFloat(key, value, stampSyntheticStepWidth), 0.0f, 64.0f);
			} else if (key == "StampSyntheticRevealDistance") {
				stampSyntheticRevealDistance = Clamped(key, AsFloat(key, value, stampSyntheticRevealDistance), 0.0f, 4096.0f);
			} else if (key == "StampSyntheticRevealSeconds") {
				stampSyntheticRevealSeconds = Clamped(key, AsFloat(key, value, stampSyntheticRevealSeconds), 0.0f, 600.0f);
			} else if (key == "PowerArmorFootScale") {
				powerArmorFootScale = Clamped(key, AsFloat(key, value, powerArmorFootScale), 0.1f, 8.0f);
			} else if (key == "PowerArmorDepthScale") {
				powerArmorDepthScale = Clamped(key, AsFloat(key, value, powerArmorDepthScale), 0.0f, 8.0f);
			} else if (key == "EnableStampShapes") {
				enableStampShapes = AsBool(value);
			} else if (key == "StampFootShape") {
				stampFootShape = AsBool(value);
			} else if (key == "StampFootLength") {
				stampFootLength = Clamped(key, AsFloat(key, value, stampFootLength), 0.1f, 16.0f);
			} else if (key == "StampFootAspect") {
				stampFootAspect = Clamped(key, AsFloat(key, value, stampFootAspect), 0.1f, 8.0f);
			} else if (key == "StampFootSeparation") {
				stampFootSeparation = Clamped(key, AsFloat(key, value, stampFootSeparation), 0.0f, 512.0f);
			} else if (key == "StampShapeKeyword" || key == "StampShapeKeywords") {
				stampShapeKeyword = Trim(value);
			} else if (key == "StampRimHeight") {
				stampRimHeight = Clamped(key, AsFloat(key, value, stampRimHeight), 0.0f, 16.0f);
			} else if (key == "StampRimIndependence") {
				stampRimIndependence = Clamped(key, AsFloat(key, value, stampRimIndependence), 0.0f, 1.0f);
			} else if (key == "StampRimSpan") {
				stampRimSpan = Clamped(key, AsFloat(key, value, stampRimSpan), 0.0f, 16.0f);
			} else if (key == "StampSlopeLimit") {
				stampSlopeLimit = Clamped(key, AsFloat(key, value, stampSlopeLimit), 0.0f, 89.0f);
			} else if (key == "StampReposeRate") {
				stampReposeRate = Clamped(key, AsFloat(key, value, stampReposeRate), 0.0f, 1.0f);
			} else if (key == "StampRimNoise") {
				stampRimNoise = Clamped(key, AsFloat(key, value, stampRimNoise), 0.0f, 8.0f);
			} else if (key == "StampRimLean") {
				stampRimLean = Clamped(key, AsFloat(key, value, stampRimLean), 0.0f, 1.0f);
			} else if (key == "StampChurn") {
				stampChurn = Clamped(key, AsFloat(key, value, stampChurn), 0.0f, 8.0f);
			} else if (key == "SnowStampRimSpan") {
				snowStampRimSpan = Clamped(key, AsFloat(key, value, snowStampRimSpan), -1.0f, 16.0f);
			} else if (key == "SnowStampReposeRate") {
				snowStampReposeRate = Clamped(key, AsFloat(key, value, snowStampReposeRate), -1.0f, 1.0f);
			} else if (key == "SnowStampRimNoise") {
				snowStampRimNoise = Clamped(key, AsFloat(key, value, snowStampRimNoise), -1.0f, 8.0f);
			} else if (key == "SnowStampRimLean") {
				snowStampRimLean = Clamped(key, AsFloat(key, value, snowStampRimLean), -1.0f, 1.0f);
			} else if (key == "SnowStampChurn") {
				snowStampChurn = Clamped(key, AsFloat(key, value, snowStampChurn), -1.0f, 8.0f);
			} else if (key == "LogStampShape") {
				logStampShape = AsBool(value);
			} else if (key == "LogStampSurfaces") {
				logStampSurfaces = AsBool(value);
			} else if (key == "LogStampFeet") {
				logStampFeet = AsBool(value);
			} else if (key == "TraceFeet") {
				traceFeet = AsBool(value);
			} else if (key == "EnableObjectStamps") {
				enableObjectStamps = AsBool(value);
			} else if (key == "ObjectStampRadiusScale") {
				objectStampRadiusScale = Clamped(key, AsFloat(key, value, objectStampRadiusScale), 0.0f, 16.0f);
			} else if (key == "ObjectStampDepthScale") {
				objectStampDepthScale = Clamped(key, AsFloat(key, value, objectStampDepthScale), 0.0f, 16.0f);
			} else if (key == "ObjectFullSizeRadius") {
				objectFullSizeRadius = Clamped(key, AsFloat(key, value, objectFullSizeRadius), 1.0f, 4096.0f);
			} else if (key == "ObjectContactTolerance") {
				objectContactTolerance = Clamped(key, AsFloat(key, value, objectContactTolerance), 0.0f, 1024.0f);
			} else if (key == "ObjectSinkLimit") {
				objectSinkLimit = Clamped(key, AsFloat(key, value, objectSinkLimit), 0.0f, 4096.0f);
			} else if (key == "ObjectStampMaxDepth") {
				objectStampMaxDepth = Clamped(key, AsFloat(key, value, objectStampMaxDepth), 0.0f, 64.0f);
			} else if (key == "ObjectRimMargin") {
				objectRimMargin = Clamped(key, AsFloat(key, value, objectRimMargin), 0.0f, 64.0f);
			} else if (key == "ObjectStampInterval") {
				objectStampInterval = Clamped(key, AsFloat(key, value, objectStampInterval), 0.0f, 60.0f);
			} else if (key == "LogObjectStamps") {
				logObjectStamps = AsBool(value);
			} else {
				return false;
			}
			return true;
		}

		bool ApplyWorldSetting(const std::string& key, const std::string& value)
		{
			if (key == "EnableHeatSources") {
				enableHeatSources = AsBool(value);
			} else if (key == "HeatKeywords") {
				heatKeywords = AsKeywords(value, heatKeywords);
			} else if (key == "HeatExcludeKeywords") {
				heatExcludeKeywords = AsKeywords(value, "");
			} else if (key == "HeatRadius") {
				heatRadius = Clamped(key, AsFloat(key, value, heatRadius), 0.0f, 8192.0f);
			} else if (key == "HeatRadiusScale") {
				heatRadiusScale = Clamped(key, AsFloat(key, value, heatRadiusScale), 0.0f, 64.0f);
			} else if (key == "HeatMaxRadius") {
				heatMaxRadius = Clamped(key, AsFloat(key, value, heatMaxRadius), 0.0f, 8192.0f);
			} else if (key == "HeatMeltPerSecond") {
				heatMeltPerSecond = Clamped(key, AsFloat(key, value, heatMeltPerSecond), 0.0f, 1.0f);
			} else if (key == "HeatShoulder") {
				heatShoulder = Clamped(key, AsFloat(key, value, heatShoulder), 0.0f, 0.95f);
			} else if (key == "HeatMeltsSnow") {
				heatMeltsSnow = AsBool(value);
			} else if (key == "MeltShowsGround") {
				meltShowsGround = AsBool(value);
			} else if (key == "HeatMeltDepthScale") {
				heatMeltDepthScale = Clamped(key, AsFloat(key, value, heatMeltDepthScale), 0.0f, 4.0f);
			} else if (key == "LogHeatSources") {
				logHeatSources = AsBool(value);
			} else if (key == "EnableScorch") {
				enableScorch = AsBool(value);
			} else if (key == "ScorchFadeSeconds") {
				scorchFadeSeconds = Clamped(key, AsFloat(key, value, scorchFadeSeconds), 1.0f, 86400.0f);
			} else if (key == "ScorchDarkness") {
				scorchDarkness = Clamped(key, AsFloat(key, value, scorchDarkness), 0.0f, 1.0f);
			} else if (key == "EnableThrusterBurn") {
				enableThrusterBurn = AsBool(value);
			} else if (key == "ThrusterRaces") {
				thrusterRaces = Trim(value);
			} else if (key == "ThrusterRadius") {
				thrusterRadius = Clamped(key, AsFloat(key, value, thrusterRadius), 1.0f, 256.0f);
			} else if (key == "ThrusterReach") {
				thrusterReach = Clamped(key, AsFloat(key, value, thrusterReach), 0.0f, 1024.0f);
			} else if (key == "ThrusterScorch") {
				thrusterScorch = Clamped(key, AsFloat(key, value, thrusterScorch), 0.0f, 1.0f);
			} else if (key == "ThrusterNoScorch") {
				thrusterNoScorch = AsKeywords(value, "");
			} else if (key == "ThrusterMeltsSnow") {
				thrusterMeltsSnow = AsBool(value);
			} else if (key == "LogThrusters") {
				logThrusters = AsBool(value);
			} else if (key == "EnableExplosionImpacts") {
				enableExplosionImpacts = AsBool(value);
			} else if (key == "ExplosionImpactRadiusScale") {
				explosionImpactRadiusScale = Clamped(key, AsFloat(key, value, explosionImpactRadiusScale), 0.0f, 16.0f);
			} else if (key == "ExplosionImpactMaxRadius") {
				explosionImpactMaxRadius = Clamped(key, AsFloat(key, value, explosionImpactMaxRadius), 0.0f, 4096.0f);
			} else if (key == "ExplosionImpactDepth") {
				explosionImpactDepth = Clamped(key, AsFloat(key, value, explosionImpactDepth), 0.0f, 256.0f);
			} else if (key == "EnableProjectileImpacts") {
				enableProjectileImpacts = AsBool(value);
			} else if (key == "ProjectileImpactRadius") {
				projectileImpactRadius = Clamped(key, AsFloat(key, value, projectileImpactRadius), 0.0f, 256.0f);
			} else if (key == "ProjectileImpactDepth") {
				projectileImpactDepth = Clamped(key, AsFloat(key, value, projectileImpactDepth), 0.0f, 64.0f);
			} else if (key == "ImpactRimScale") {
				impactRimScale = Clamped(key, AsFloat(key, value, impactRimScale), 0.0f, 8.0f);
			} else if (key == "ImpactSnowMelt") {
				impactSnowMelt = Clamped(key, AsFloat(key, value, impactSnowMelt), 0.0f, 4.0f);
			} else if (key == "LogImpacts") {
				logImpacts = AsBool(value);
			} else if (key == "EnableBloodDecals") {
				enableBloodDecals = AsBool(value);
			} else if (key == "BloodDecalDepthMargin") {
				bloodDecalDepthMargin = Clamped(key, AsFloat(key, value, bloodDecalDepthMargin), 0.0f, 128.0f);
			} else if (key == "BloodDecalGroundTolerance") {
				bloodDecalGroundTolerance = Clamped(key, AsFloat(key, value, bloodDecalGroundTolerance), 0.0f, 256.0f);
			} else if (key == "BloodDecalSnowOpacity") {
				bloodDecalSnowOpacity = Clamped(key, AsFloat(key, value, bloodDecalSnowOpacity), 0.0f, 1.0f);
			} else if (key == "MaxBloodDecalsPerFrame") {
				maxBloodDecalsPerFrame = static_cast<int>(Clamped(key,
					AsFloat(key, value, static_cast<float>(maxBloodDecalsPerFrame)), 0.0f, 4096.0f));
			} else if (key == "LogBloodDecals") {
				logBloodDecals = AsBool(value);
			} else if (key == "CorpseMarks") {
				corpseMarks = AsBool(value);
			} else if (key == "CorpseBurial") {
				corpseBurial = AsBool(value);
			} else if (key == "CorpseBurialSeconds") {
				corpseBurialSeconds = Clamped(key, AsFloat(key, value, corpseBurialSeconds), 10.0f, 7200.0f);
			} else if (key == "CorpseMoundHeight") {
				corpseMoundHeight = Clamped(key, AsFloat(key, value, corpseMoundHeight), 0.0f, 32.0f);
			} else if (key == "MaxCorpseMarks") {
				maxCorpseMarks = static_cast<int>(Clamped(key,
					AsFloat(key, value, static_cast<float>(maxCorpseMarks)), 0.0f, 32.0f));
			} else if (key == "LogCorpses") {
				logCorpses = AsBool(value);
			} else if (key == "EnableWeather") {
				enableWeather = AsBool(value);
			} else if (key == "WeatherSoftenHours") {
				weatherSoftenHours = Clamped(key, AsFloat(key, value, weatherSoftenHours), 0.05f, 720.0f);
			} else if (key == "WeatherFirmHours") {
				weatherFirmHours = Clamped(key, AsFloat(key, value, weatherFirmHours), 0.05f, 720.0f);
			} else if (key == "WeatherSnowSoften") {
				weatherSnowSoften = Clamped(key, AsFloat(key, value, weatherSnowSoften), 0.0f, 16.0f);
			} else if (key == "WeatherRainSoften") {
				weatherRainSoften = Clamped(key, AsFloat(key, value, weatherRainSoften), 0.0f, 16.0f);
			} else if (key == "WeatherCloudyFirmScale") {
				weatherCloudyFirmScale = Clamped(key, AsFloat(key, value, weatherCloudyFirmScale), 0.0f, 1.0f);
			} else if (key == "WeatherSoftDepthScale") {
				weatherSoftDepthScale = Clamped(key, AsFloat(key, value, weatherSoftDepthScale), 0.0f, 16.0f);
			} else if (key == "WeatherFirmDepthScale") {
				weatherFirmDepthScale = Clamped(key, AsFloat(key, value, weatherFirmDepthScale), 0.0f, 16.0f);
			} else if (key == "WeatherSoftDecayScale") {
				weatherSoftDecayScale = Clamped(key, AsFloat(key, value, weatherSoftDecayScale), Surfaces::kDecayScaleLow,
					Surfaces::kDecayScaleHigh);
			} else if (key == "WeatherFirmDecayScale") {
				weatherFirmDecayScale = Clamped(key, AsFloat(key, value, weatherFirmDecayScale), Surfaces::kDecayScaleLow,
					Surfaces::kDecayScaleHigh);
			} else if (key == "WeatherFillPerSecond") {
				weatherFillPerSecond = Clamped(key, AsFloat(key, value, weatherFillPerSecond), 0.0f, 0.99f);
			} else if (key == "LogWeather") {
				logWeather = AsBool(value);
			} else {
				return false;
			}
			return true;
		}

		bool ApplySnowSetting(const std::string& key, const std::string& value)
		{
			if (key == "EnableSnowRaise") {
				enableSnowRaise = AsBool(value);
			} else if (key == "SnowRaiseHeight") {
				snowRaiseHeight = Clamped(key, AsFloat(key, value, snowRaiseHeight), 0.0f, 512.0f);
			} else if (key == "SnowRaiseWeather") {
				snowRaiseWeather = AsBool(value);
			} else if (key == "SnowRaiseDistance") {
				snowRaiseDistance = Clamped(key, AsFloat(key, value, snowRaiseDistance), 0.0f,
					SnowCoverage::kWorldSize * 0.5f - SnowCoverage::kTexelSize * 4.0f);
			} else if (key == "SnowRaiseFadeBand") {
				snowRaiseFadeBand = Clamped(key, AsFloat(key, value, snowRaiseFadeBand), 1.0f, 8192.0f);
			} else if (key == "SnowCoverageBudget") {
				snowCoverageBudget = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(snowCoverageBudget)), 1.0f, 65536.0f));
			} else if (key == "SnowTrenchDepth") {
				snowTrenchDepth = Clamped(key, AsFloat(key, value, snowTrenchDepth), 0.0f, 4.0f);
			} else if (key == "WinterHardGroundDepth") {
				winterHardGroundDepth = Clamped(key, AsFloat(key, value, winterHardGroundDepth), 0.0f, 1.0f);
			} else if (key == "SnowTrenchCoverage") {
				snowTrenchCoverage = Clamped(key, AsFloat(key, value, snowTrenchCoverage), 0.5f, 0.95f);
			} else if (key == "SnowGroundFloor") {
				snowGroundFloor = AsBool(value);
			} else if (key == "SnowGroundBite") {
				snowGroundBite = Clamped(key, AsFloat(key, value, snowGroundBite), 0.0f, 512.0f);
			} else if (key == "SnowSeamTexels") {
				snowSeamTexels = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(snowSeamTexels)), 0.0f,
					static_cast<float>(SnowCoverage::kTexels / 4)));
			} else if (key == "SnowSkipUnderwater") {
				snowSkipUnderwater = AsBool(value);
			} else if (key == "LogSnowCoverage") {
				logSnowCoverage = AsBool(value);
			} else if (key == "EnableShelter") {
				enableShelter = AsBool(value);
			} else if (key == "ShelterBudget") {
				shelterBudget = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(shelterBudget)), 1.0f, 4096.0f));
			} else if (key == "ShelterRefresh") {
				shelterRefresh = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(shelterRefresh)), 0.0f, 2048.0f));

			} else if (key == "ShelterTimeBudget") {
				shelterTimeBudget = Clamped(key, AsFloat(key, value, shelterTimeBudget), 0.05f, 16.0f);
			} else if (key == "ShelterClearance") {
				shelterClearance = Clamped(key, AsFloat(key, value, shelterClearance), 1.0f, 2048.0f);
			} else if (key == "ShelterHeight") {
				shelterHeight = Clamped(key, AsFloat(key, value, shelterHeight), 16.0f, 32768.0f);
			} else if (key == "ShelterMeshCap") {
				shelterMeshCap = AsBool(value);
			} else if (key == "ShelterFloorTolerance") {
				shelterFloorTolerance = Clamped(key, AsFloat(key, value, shelterFloorTolerance), 1.0f, 512.0f);
			} else if (key == "SnowCoverLowMeshes") {
				snowCoverLowMeshes = AsBool(value);
			} else if (key == "SnowCoverMeshHeight") {
				snowCoverMeshHeight = Clamped(key, AsFloat(key, value, snowCoverMeshHeight), 0.0f, 256.0f);
			} else if (key == "SnowOnObjects") {
				snowOnObjects = AsBool(value);
			} else if (key == "SnowOnObjectsStrength") {
				snowOnObjectsStrength = Clamped(key, AsFloat(key, value, snowOnObjectsStrength), 0.0f, 1.0f);
			} else if (key == "SnowOnObjectsMinUp") {
				snowOnObjectsMinUp = Clamped(key, AsFloat(key, value, snowOnObjectsMinUp), 0.0f, 0.99f);
			} else if (key == "SnowOnObjectsDistance") {
				snowOnObjectsDistance = Clamped(key, AsFloat(key, value, snowOnObjectsDistance), 256.0f, 2000.0f);
			} else if (key == "SnowOnObjectsDebug") {
				snowOnObjectsDebug = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(snowOnObjectsDebug)), 0.0f, 1.0f));
			} else if (key == "SnowOnObjectsPass") {
				snowOnObjectsPass = AsBool(value);
			} else if (key == "IndoorPrints") {
				indoorPrints = AsBool(value);
			} else if (key == "IndoorPrintSteps") {
				indoorPrintSteps = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(indoorPrintSteps)), 1.0f, 200.0f));
			} else if (key == "IndoorPrintFadeSeconds") {
				indoorPrintFadeSeconds = Clamped(key, AsFloat(key, value, indoorPrintFadeSeconds), 10.0f, 7200.0f);
			} else if (key == "IndoorPrintOpacity") {
				indoorPrintOpacity = Clamped(key, AsFloat(key, value, indoorPrintOpacity), 0.0f, 1.0f);
			} else if (key == "MaxIndoorPrints") {
				maxIndoorPrints = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(maxIndoorPrints)), 16.0f, 4096.0f));
			} else if (key == "IndoorPrintDistance") {
				indoorPrintDistance = Clamped(key, AsFloat(key, value, indoorPrintDistance), 256.0f, 8192.0f);
			} else if (key == "ShelterSeamTexels") {
				shelterSeamTexels = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(shelterSeamTexels)), 0.0f,
					static_cast<float>(Shelter::kTexels / 4)));
			} else if (key == "SnowSparkle") {
				snowSparkle = AsBool(value);
			} else if (key == "SnowSparkleRate") {
				snowSparkleRate = Clamped(key, AsFloat(key, value, snowSparkleRate), 0.0f, 20000.0f);
			} else if (key == "SnowSparkleFullSpeed") {
				snowSparkleFullSpeed = Clamped(key, AsFloat(key, value, snowSparkleFullSpeed), 1.0f, 8000.0f);
			} else if (key == "SnowSparkleMinSpeed") {
				snowSparkleMinSpeed = Clamped(key, AsFloat(key, value, snowSparkleMinSpeed), 0.0f, 8000.0f);
			} else if (key == "SnowSparkleSize") {
				snowSparkleSize = Clamped(key, AsFloat(key, value, snowSparkleSize), 0.1f, 256.0f);
			} else if (key == "SnowSparkleLife") {
				snowSparkleLife = Clamped(key, AsFloat(key, value, snowSparkleLife), 0.05f, 60.0f);
			} else if (key == "SnowSparkleThrow") {
				snowSparkleThrow = Clamped(key, AsFloat(key, value, snowSparkleThrow), 0.0f, 8000.0f);
			} else if (key == "SnowSparkleRise") {
				snowSparkleRise = Clamped(key, AsFloat(key, value, snowSparkleRise), 0.0f, 8000.0f);
			} else if (key == "SnowSparkleInherit") {
				snowSparkleInherit = Clamped(key, AsFloat(key, value, snowSparkleInherit), 0.0f, 4.0f);
			} else if (key == "SnowSparkleGravity") {
				snowSparkleGravity = Clamped(key, AsFloat(key, value, snowSparkleGravity), 0.0f, 16000.0f);
			} else if (key == "SnowSparkleDrag") {
				snowSparkleDrag = Clamped(key, AsFloat(key, value, snowSparkleDrag), 0.0f, 1000.0f);
			} else if (key == "SnowSparkleSwirl") {
				snowSparkleSwirl = Clamped(key, AsFloat(key, value, snowSparkleSwirl), 0.0f, 4000.0f);
			} else if (key == "SnowSparkleBrightness") {
				snowSparkleBrightness = Clamped(key, AsFloat(key, value, snowSparkleBrightness), 0.0f, 16.0f);
			} else if (key == "SnowSparkleOpacity") {
				snowSparkleOpacity = Clamped(key, AsFloat(key, value, snowSparkleOpacity), 0.0f, 1.0f);
			} else if (key == "SnowSparkleShape") {
				snowSparkleShape = Clamped(key, AsFloat(key, value, snowSparkleShape), 0.0f, 1.0f);
			} else {
				return false;
			}
			return true;
		}

		bool ApplySurfaceSetting(const std::string& key, const std::string& value)
		{
			if (key == "EnableSurfaceMaterial") {
				enableSurfaceMaterial = AsBool(value);
			} else if (key == "EnableSurfaceClassification") {
				enableSurfaceClassification = AsBool(value);
			} else if (key == "LogSurfaceClassification") {
				logSurfaceClassification = AsBool(value);
			} else if (key == "BlendFullDepth") {
				blendFullDepth = Clamped(key, AsFloat(key, value, blendFullDepth), 0.01f, 1024.0f);
			} else if (key == "BlendStrength") {
				blendStrength = Clamped(key, AsFloat(key, value, blendStrength), 0.0f, 1.0f);
			} else if (key == "SnowRevealStrength") {
				snowRevealStrength = Clamped(key, AsFloat(key, value, snowRevealStrength), 0.0f, 1.0f);
			} else if (key == "SurfaceSnow") {
				surfaceSnow = AsKeywords(value, surfaceSnow);
			} else if (key == "SurfaceGrass") {
				surfaceGrass = AsKeywords(value, surfaceGrass);
			} else if (key == "SurfaceDirt") {
				surfaceDirt = AsKeywords(value, surfaceDirt);
			} else if (key == "SurfaceMud") {
				surfaceMud = AsKeywords(value, surfaceMud);
			} else if (key == "WinterMode") {
				winterMode = static_cast<int>(Clamped(key, AsFloat(key, value, static_cast<float>(winterMode)), -1.0f, 2.0f));
			} else if (key == "SurfaceSand") {
				surfaceSand = AsKeywords(value, surfaceSand);
			} else if (key == "SurfaceAsh") {
				surfaceAsh = AsKeywords(value, surfaceAsh);
			} else if (key == "SurfaceGravel") {
				surfaceGravel = AsKeywords(value, surfaceGravel);
			} else if (key == "SurfaceStone") {
				surfaceStone = AsKeywords(value, surfaceStone);
			} else if (key == "ResponseUnknown") {
				ParseResponse(key, value, Response(Surfaces::Type::kUnknown));
			} else if (key == "ResponseSnow") {
				ParseResponse(key, value, Response(Surfaces::Type::kSnow));
			} else if (key == "ResponseGrass") {
				ParseResponse(key, value, Response(Surfaces::Type::kGrass));
			} else if (key == "ResponseDirt") {
				ParseResponse(key, value, Response(Surfaces::Type::kDirt));
			} else if (key == "ResponseMud") {
				ParseResponse(key, value, Response(Surfaces::Type::kMud));
			} else if (key == "ResponseSand") {
				ParseResponse(key, value, Response(Surfaces::Type::kSand));
			} else if (key == "ResponseAsh") {
				ParseResponse(key, value, Response(Surfaces::Type::kAsh));
			} else if (key == "ResponseGravel") {
				ParseResponse(key, value, Response(Surfaces::Type::kGravel));
			} else if (key == "ResponseStone") {
				ParseResponse(key, value, Response(Surfaces::Type::kStone));
			} else {
				return false;
			}
			return true;
		}

		bool ApplyDiagnosticSetting(const std::string& key, const std::string& value)
		{
			if (key == "EnableLogging") {
				enableLogging = AsBool(value);
			} else if (key == "LogDraws") {
				logDraws = AsBool(value);
			} else if (key == "EnableProfiler") {
				enableProfiler = AsBool(value);
			} else if (key == "ProfileInterval") {
				profileInterval = Clamped(key, AsFloat(key, value, profileInterval), 0.5f, 300.0f);
			} else if (key == "ProfileDrawScopes") {
				profileDrawScopes = AsBool(value);
			} else if (key == "EnableLiveReload") {
				enableLiveReload = AsBool(value);
			} else if (key == "LiveReloadInterval") {
				liveReloadInterval = Clamped(key, AsFloat(key, value, liveReloadInterval), 0.05f, 10.0f);
			} else {
				return false;
			}
			return true;
		}
	}

	void ApplyLogLevel()
	{
		// Every diagnostic switch writes info lines, so turning any of them on is enough to
		// see its output; EnableLogging turns on the rest of the info log as well.
		const bool chatty = enableLogging || enableProfiler || logGeneratedShaders || logDraws ||
		                    logStampShape || logStampSurfaces || logStampFeet || logObjectStamps ||
		                    logHeatSources || logThrusters || logImpacts || logBloodDecals || logCorpses || logWeather || logSnowCoverage ||
		                    logSurfaceClassification;
#ifndef NMN_OFFLINE
		if (auto logger = spdlog::default_logger(); logger) {
			logger->set_level(chatty ? spdlog::level::info : spdlog::level::warn);
		}
#else
		(void)chatty;  // the offline logger always writes to stderr
#endif
	}

	bool ParseFloat(std::string_view a_text, float& a_out)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return false;
		}
		a_text.remove_prefix(first);
		if (a_text.front() == '+') {
			a_text.remove_prefix(1);
			if (a_text.empty() || a_text.front() == '+' || a_text.front() == '-') {
				return false;
			}
		}

		float      parsed = 0.0f;
		const auto end = a_text.data() + a_text.size();
		const auto [stop, error] = std::from_chars(a_text.data(), end, parsed);
		if (error != std::errc{} || stop == a_text.data()) {
			return false;
		}
		for (auto rest = stop; rest != end; ++rest) {
			if (*rest != ' ' && *rest != '\t' && *rest != '\r' && *rest != '\n') {
				return false;
			}
		}
		if (!std::isfinite(parsed)) {
			return false;
		}

		a_out = parsed;
		return true;
	}

	namespace
	{
		// MCM's keys carry its type prefix (bSnowTracks, iWinterMode, fStampDepth); the
		// setting's own name follows it.
		std::string McmKey(std::string_view a_key)
		{
			if (a_key.size() > 1 && (a_key[0] == 'b' || a_key[0] == 'i' || a_key[0] == 'f' || a_key[0] == 's') &&
				std::isupper(static_cast<unsigned char>(a_key[1]))) {
				a_key.remove_prefix(1);
			}
			return std::string(a_key);
		}

		// Applies every line it knows; returns how many it applied.
		uint32_t ReadFile(std::ifstream& a_file, const char* a_path, bool a_mcm)
		{
			uint32_t    applied = 0;
			std::string line;
			while (std::getline(a_file, line)) {
				const auto trimmed = Trim(line);
				if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[') {
					continue;
				}

				const auto eq = trimmed.find('=');
				if (eq == std::string::npos) {
					continue;
				}

				const auto raw = Trim(std::string_view(trimmed).substr(0, eq));
				const auto key = a_mcm ? McmKey(raw) : raw;

				// The menu's old Winter ground switch: the ground now tells by its looks, and a
				// choice saved by an older version must not pin it for good.
				if (a_mcm && key == "WinterMode") {
					continue;
				}

				auto       body = std::string_view(trimmed).substr(eq + 1);
				const auto comment = body.find_first_of(";#");
				if (comment != std::string_view::npos) {
					body = body.substr(0, comment);
				}
				const auto value = Trim(body);

				if (!ApplyTessellationSetting(key, value) &&
					!ApplyMarkSetting(key, value) &&
					!ApplyWorldSetting(key, value) &&
					!ApplySnowSetting(key, value) &&
					!ApplySurfaceSetting(key, value) &&
					!ApplyDiagnosticSetting(key, value)) {
					logger::warn("{}: '{}' is not a setting this build knows", a_path, raw);
				} else {
					++applied;
				}
			}
			return applied;
		}

		void LogSummary()
		{
			logger::info(
				"Settings: EnableTessellation={} Winding={} MaxFactor={} TargetSpacing={} Clipmap={} "
				"StampDepth={} Decay={} FootNodes={} FootRadius={} Cull={} CullMargin={} Normals={} "
				"SurfaceMaterial={} Classify={} BlendStrength={}",
				enableTessellation, tessellationWinding, tessellationMaxFactor, tessellationTargetSpacing,
				useClipmap, stampDepth, stampDecayPerSecond, stampFootNodes, stampFootRadius,
				cullDistantDraws, cullMargin, recomputeNormals, enableSurfaceMaterial,
				enableSurfaceClassification, blendStrength);

			logger::info("Marks: rim height={}*depth span={}*radius | shapes={} oriented={} keywords={} "
						 "length={} aspect={} | reach={} clearance={} | repose={} deg",
				stampRimHeight, stampRimSpan, enableStampShapes, stampFootShape, stampShapeKeyword,
				stampFootLength, stampFootAspect, stampFootReach, stampGroundClearance, stampSlopeLimit);

			logger::info("Impacts: explosions={} (x{} radius, max {}, depth {}) | gunfire={} (radius {}, depth {})",
				enableExplosionImpacts, explosionImpactRadiusScale, explosionImpactMaxRadius,
				explosionImpactDepth, enableProjectileImpacts, projectileImpactRadius, projectileImpactDepth);

			logger::info("Snow raise: enabled={} height={} weather={} distance={} fade={} coverage budget={}/frame",
				enableSnowRaise, snowRaiseHeight, snowRaiseWeather, snowRaiseDistance, snowRaiseFadeBand,
				snowCoverageBudget);

			if (enableSnowRaise && snowRaiseFadeBand > snowRaiseDistance) {
				logger::warn("SnowRaiseFadeBand ({}) is wider than SnowRaiseDistance ({}), so the raise "
							 "never reaches full height anywhere.",
					snowRaiseFadeBand, snowRaiseDistance);
			}

			if (recomputeNormals) {
				const float cell = Clipmap::kCellSize;
				if (normalGradientEpsilon > cell * 2.0f || normalGradientEpsilon < cell * 0.5f) {
					logger::warn("NormalGradientEpsilon is {} but the field's cells are {} world units; "
								 "{} is the value that matches.",
						normalGradientEpsilon, cell, cell);
				}
			}
		}
	}

	void Load()
	{
		// Every load starts from the built-in defaults, so a line deleted from the file (or
		// the whole file) falls back to its default on a live reload too. The SnowStamp*
		// keys keep their own defaults here; -1 in the file means "use the ordinary value".
		values = Values{};

		try {
			std::ifstream file(kPath);
			const bool    haveIni = static_cast<bool>(file);
			if (haveIni) {
				ReadFile(file, kPath, false);
			}
			uint32_t fromMenu = 0;
			if (std::ifstream menu(kMcmPath); menu) {
				fromMenu = ReadFile(menu, kMcmPath, true);
			}
			ApplyLogLevel();
			if (!haveIni) {
				logger::warn("No {} - using the built-in defaults", kPath);
			}
			if (fromMenu > 0) {
				logger::info("Settings: {} choice(s) from the MCM menu ({}) override the INI", fromMenu, kMcmPath);
			}
			if (haveIni || fromMenu > 0) {
				LogSummary();
			}
		} catch (const std::exception& e) {
			ApplyLogLevel();
			logger::error("Settings: reading {} stopped part-way ({}); the lines before that "
						  "apply, the rest keep their defaults",
				kPath, e.what());
		}

		g_watchEnabled.store(enableLiveReload, std::memory_order_relaxed);
		g_watchInterval.store(liveReloadInterval, std::memory_order_relaxed);
		if (enableLiveReload) {
			StartWatching();
		}
	}

	bool PollForChanges(float)
	{
		if (!enableLiveReload || !g_changed.exchange(false, std::memory_order_acq_rel)) {
			return false;
		}

		logger::info("Settings changed on disk - reloading");
		Load();
		return true;
	}
}
