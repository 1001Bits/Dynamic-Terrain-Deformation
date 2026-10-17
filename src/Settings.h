// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include "SurfaceTypes.h"

namespace Settings
{
	// Every setting with its built-in default. Load() starts from a fresh Values, so a
	// line deleted from the INI falls back to the value written here; the names below are
	// references into the live copy, so the rest of the plugin reads Settings::name.
	struct Values
	{
		// ── tessellation ────────────────────────────────────────────────────────

		bool enableTessellation{ true };

		std::string tessellationWinding{ "cw" };

		bool logGeneratedShaders{ false };

		float tessellationMaxFactor{ 64.0f };

		float tessellationTargetSpacing{ 2.0f };

		float tessellationRaiseSpacing{ 64.0f };

		float tessellationBlanketSpacing{ 8.0f };

		bool enableTessellationBounds{ true };

		float tessellationBoundDepth{ 0.25f };

		bool recomputeNormals{ true };

		float normalGradientEpsilon{ 0.75f };

		bool cullDistantDraws{ true };

		// Drop tessellated patches that cannot reach the screen before they are subdivided.
		bool enablePatchCulling{ true };

		float cullMargin{ 256.0f };

		bool routeOnlyPlayerCamera{ true };

		bool asyncShaderCompilation{ true };

		float debugWorldZOffset{ 0.0f };

		float debugWaveAmplitude{ 0.0f };

		float debugWaveLength{ 256.0f };

		int debugFieldColour{ 0 };

		int debugBlendLayer{ -1 };

		// ── the field ───────────────────────────────────────────────────────────

		bool useClipmap{ true };

		uint32_t clipmapLevels{ 2 };

		float clipmapSeedSmoothing{ 1.0f };

		// ── marks ───────────────────────────────────────────────────────────────

		float stampRadiusScale{ 1.0f };

		float stampFootReach{ 48.0f };

		float stampGroundClearance{ 0.1f };

		float stampDepth{ 6.0f };

		float stampDecayPerSecond{ 0.99f };

		// How far from the player, in metres, marks are made and shown: other actors' footprints
		// and the ground's fade (at most the field's reach, about 53 m at two levels).
		float       footprintMetres{ 50.0f };
		// Fallout 4 has no per-foot collision bodies to read a size from, so a foot is a
		// skeleton node whose name contains one of these (lowercase, comma separated), pressing
		// a disc this wide, scaled by the actor's size relative to a person.
		// A body with none of them stands on its ankles, wheels or leg ends.
		std::string stampFootNodes{ "foot,toe,paw,hoof" };
		float       stampFootRadius{ 6.0f };
		float       stampFootSizeReference{ 64.0f };
		float       stampFootSizeMin{ 0.5f };
		float       stampFootSizeMax{ 4.0f };
		// A foot node presses the ground while it is at most this far above it (scaled like
		// the radius): a planted ankle sits about 9 units up, a swinging one 15 and more.
		float       stampFootContactHeight{ 12.0f };
		// Only a planted foot presses: one moving slower than this share of the actor's own
		// speed, or slower than StampFootPlantSpeed (units per second) when the actor stands
		// still. A swinging foot passes low too, and pressing it drew a line rather than a
		// row of prints. 0 presses every low foot, smeared along the actor's motion.
		float       stampFootPlantFraction{ 0.5f };
		float       stampFootPlantSpeed{ 30.0f };
		// Snow: false leaves separate footprints there as on any ground; true lets legs
		// wading through the raised snow plough continuous tracks (every low foot presses,
		// no planted-foot test; synthetic steps plough two tracks).
		bool        snowTracks{ false };
		// Legs that are not animated (the player's own body in first person) carry their feet
		// along rigidly, so they never plant: such an actor gets synthetic steps instead, one
		// print every StampSyntheticStepLength + StampSyntheticStepPerSpeed x speed units,
		// alternately StampSyntheticStepWidth to the left and right of its path. A third-person
		// walk on OG landed its feet 0.2-0.25 s apart at about 240 units/s, 5-9 units aside.
		float       stampSyntheticStepLength{ 25.0f };
		float       stampSyntheticStepPerSpeed{ 0.15f };
		float       stampSyntheticStepWidth{ 6.5f };
		// The player's synthetic steps (first person: no feet to see) wait until their ground
		// is out of view before they press, so they are not seen forming under the camera; or
		// until the player is StampSyntheticRevealDistance units away (a stride and a half, so
		// walking backwards shows them a step behind), or StampSyntheticRevealSeconds of game
		// time have passed (0 = no limit of that kind).
		float       stampSyntheticRevealDistance{ 120.0f };
		float       stampSyntheticRevealSeconds{ 30.0f };
		// An actor in power armour presses wider and deeper prints.
		float       powerArmorFootScale{ 1.3f };
		float       powerArmorDepthScale{ 1.6f };

		bool  enableStampShapes{ false };
		bool  stampFootShape{ true };
		float stampFootLength{ 1.3f };
		float stampFootAspect{ 0.50f };
		float stampFootSeparation{ 0.0f };

		// Races with any of these keywords wear footwear (oriented prints); comma separated.
		std::string stampShapeKeyword{ "ActorTypeHuman,ActorTypeGhoul,ActorTypeSuperMutant,ActorTypeSynth" };

		float stampRimHeight{ 0.2f };
		float stampRimIndependence{ 1.0f };
		float stampRimSpan{ 1.0f };
		float stampSlopeLimit{ 50.0f };
		float stampReposeRate{ 0.01f };
		float stampRimNoise{ 0.0f };
		float stampRimLean{ 1.0f };
		float stampChurn{ 0.45f };

		float snowStampRimSpan{ 0.7f };
		float snowStampReposeRate{ 0.10f };
		float snowStampRimNoise{ 0.8f };
		float snowStampRimLean{ 1.0f };
		float snowStampChurn{ 0.80f };

		bool logStampShape{ false };
		bool logStampSurfaces{ false };
		bool logStampFeet{ false };
		// Every foot of every moving actor, every frame, as CSV beside the log (bounded).
		bool traceFeet{ false };

		// ── objects ─────────────────────────────────────────────────────────────

		bool  enableObjectStamps{ true };
		float objectStampRadiusScale{ 1.0f };
		float objectStampDepthScale{ 1.0f };
		float objectFullSizeRadius{ 1.0f };
		float objectContactTolerance{ 48.0f };
		float objectSinkLimit{ 40.0f };
		// A resting object lies on the undeformed collision ground: its dent stays shallow
		// (world units) so it does not hover over it, and the soil it pushes aside rises
		// only this far outside its footprint, never over the object.
		float objectStampMaxDepth{ 1.5f };
		float objectRimMargin{ 3.0f };
		float objectStampInterval{ 0.20f };
		bool  logObjectStamps{ false };

		// ── heat ────────────────────────────────────────────────────────────────

		bool        enableHeatSources{ true };
		// Matched against the model file name (lowercase, comma separated). The vanilla fire
		// effects are fxfire*, fire0*fxsp and mpsfire*, burning barrels metalbarrel01fire, camp fires
		// woodfire*, candles waxcandle*, road flares flare01on.
		std::string heatKeywords{ "fxfire,fire01fx,fire02small,fire03small,mpsfire,barrel01fire,woodfire,waxcandle,flare01on,burningdebris,trashburnpile,brazier" };
		// A model that also holds one of these is not hot: unlit variants and look-alikes.
		std::string heatExcludeKeywords{ "off,doused,marker,extinguisher,escape,firescape,burnt,grating,clothesline,puddle" };
		float       heatRadius{ 2.5f };
		float       heatRadiusScale{ 2.5f };
		float       heatMaxRadius{ 256.0f };
		float       heatMeltPerSecond{ 0.85f };
		// The melt is full out to this share of the radius, then fades over a short edge.
		float       heatShoulder{ 0.70f };
		bool        heatMeltsSnow{ true };
		// How much of the snow depth a fire melts: all of it, down to the ground.
		float       heatMeltDepthScale{ 1.00f };
		// Where heat melts the snow down to the ground, the ground shows: a winter mod's white
		// repaint is tinted to that ground's own colour (wet grass, soil, gravel).
		bool        meltShowsGround{ true };
		bool        logHeatSources{ false };

		// ── scorch and jet thrusters ────────────────────────────────────────────

		// Burns darken the ground through the terrain's vertex colour, near the player
		// (the finest field level). A full burn fades out over ScorchFadeSeconds; at full
		// strength it keeps 1 - ScorchDarkness of the ground's colour.
		bool  enableScorch{ true };
		float scorchFadeSeconds{ 600.0f };
		float scorchDarkness{ 0.85f };

		// Robots that fly on a jet (Mr. Handy, Mr. Gutsy and Miss Nanny are all HandyRace,
		// Fallout4.esm 000359F4) melt the snow under them (ThrusterMeltsSnow) and, with
		// EnableThrusterBurn, burn a line along their path, while they hover within
		// ThrusterReach of the land. Races are hex form ids, comma separated.
		bool        enableThrusterBurn{ false };
		std::string thrusterRaces{ "000359F4" };
		float       thrusterRadius{ 14.0f };
		float       thrusterReach{ 96.0f };
		float       thrusterScorch{ 1.0f };
		// Ground the jet does not blacken (surface names, comma separated): wet mud and sand
		// only dry under it. Snow is melted wherever it lies regardless.
		std::string thrusterNoScorch{ "sand,mud" };
		bool        thrusterMeltsSnow{ true };
		bool        logThrusters{ false };

		// ── explosions and gunfire ──────────────────────────────────────────────

		bool  enableExplosionImpacts{ true };
		float explosionImpactRadiusScale{ 0.5f };
		float explosionImpactMaxRadius{ 256.0f };
		float explosionImpactDepth{ 18.0f };

		bool  enableProjectileImpacts{ true };
		float projectileImpactRadius{ 5.0f };
		float projectileImpactDepth{ 2.5f };

		float impactRimScale{ 0.1f };
		float impactSnowMelt{ 0.3f };
		bool  logImpacts{ false };

		// ── blood and other ground decals ───────────────────────────────────────

		// Fallout 4 lays blood, scorch marks and bullet holes on the ground as deferred
		// decals: boxes projected onto whatever surface lies inside them. A static decal
		// lying on the landscape has its box stretched each frame from BloodDecalDepthMargin
		// below the ground up to the top of the snow blanket (plus the margin), so it lands
		// on the deformed surface - a crater, a trench, the raised snow - instead of floating
		// over a dent or vanishing inside the snow. Its centre must lie within
		// BloodDecalGroundTolerance of the land height to count as lying on it; at most
		// MaxBloodDecalsPerFrame decals are fitted a frame (flat screen only).
		bool  enableBloodDecals{ true };
		float bloodDecalDepthMargin{ 20.0f };
		float bloodDecalGroundTolerance{ 24.0f };
		// How strongly a decal lying on snow shows (its fade is scaled by this): blood
		// soaks into snow, so the snow shows through it.
		float bloodDecalSnowOpacity{ 0.75f };
		int   maxBloodDecalsPerFrame{ 256 };
		bool  logBloodDecals{ false };

		// Corpses (Corpses): a body that has come to rest clears a hollow of its own shape in
		// the snow (it lies on the ground, under the blanket's top) and presses a shallow print
		// into other soft ground. While snow falls (a snow weather, or rain under WinterMode)
		// the hollow fills back in over the first half of CorpseBurialSeconds and a mound
		// CorpseMoundHeight units high grows over the body in the second. The MaxCorpseMarks
		// nearest bodies are marked, never taking the field's room from feet, impacts or objects.
		bool  corpseMarks{ true };
		bool  corpseBurial{ true };
		float corpseBurialSeconds{ 600.0f };
		float corpseMoundHeight{ 6.0f };
		int   maxCorpseMarks{ 12 };
		bool  logCorpses{ false };

		// ── weather ─────────────────────────────────────────────────────────────

		bool  enableWeather{ true };
		float weatherSoftenHours{ 1.0f };
		float weatherFirmHours{ 1.0f };
		float weatherSnowSoften{ 1.0f };
		float weatherRainSoften{ 8.0f };
		float weatherCloudyFirmScale{ 0.25f };
		float weatherSoftDepthScale{ 1.5f };
		float weatherFirmDepthScale{ 1.0f };
		// Lifetime multipliers like the Response decay column: soft ground recovers sooner,
		// firm ground keeps its marks longer.
		float weatherSoftDecayScale{ 0.5f };
		float weatherFirmDecayScale{ 1.0f };
		float weatherFillPerSecond{ 0.0038f };
		bool  logWeather{ false };

		// ── snow blanket ────────────────────────────────────────────────────────

		bool  enableSnowRaise{ true };
		float snowRaiseHeight{ 35.0f };
		bool  snowRaiseWeather{ false };
		float snowRaiseDistance{ 7936.0f };
		float snowRaiseFadeBand{ 4000.0f };
		int   snowCoverageBudget{ 2048 };
		float snowTrenchDepth{ 0.5f };
		// Under WinterMode, stone and gravel turned to snow hold a thin layer only: marks
		// there go this share as deep as on other snow.
		float winterHardGroundDepth{ 0.3f };
		float snowTrenchCoverage{ 0.95f };
		bool  snowGroundFloor{ true };
		float snowGroundBite{ 0.0f };
		int   snowSeamTexels{ 1 };
		bool  snowSkipUnderwater{ true };
		bool  logSnowCoverage{ false };

		bool  enableShelter{ true };
		int   shelterBudget{ 48 };
		int   shelterRefresh{ 4 };
		float shelterTimeBudget{ 0.5f };
		float shelterClearance{ 48.0f };
		float shelterHeight{ 1024.0f };
		int   shelterSeamTexels{ 1 };
		bool  shelterMeshCap{ true };
		float shelterFloorTolerance{ 1.0f };
		// A static mesh whose top lies no higher than SnowCoverMeshHeight above the land (a
		// road, a path, a slab, low rubble) is buried by the blanket instead of keeping it
		// below its top; never more than SnowRaiseHeight - 2. Higher ones still keep it below.
		bool  snowCoverLowMeshes{ true };
		float snowCoverMeshHeight{ 31.0f };

		// Snow on the tops of static objects that stand above the blanket (rock tops, roofs,
		// car roofs, porches) where the ground around them is snow: a G-buffer pass after the
		// game's own whitens their upward faces. Flat screens only. Strength 0-1 is how white;
		// faces steeper than MinUp (the up component of their normal) stay bare; Distance is
		// how far around the player the tops are probed (units). Off by default: about 0.3 ms a
		// frame in snow (0.22 ms of GPU for the pass, 0.06 ms of CPU for the probes).
		bool  snowOnObjects{ false };
		float snowOnObjectsStrength{ 0.85f };
		float snowOnObjectsMinUp{ 0.5f };
		float snowOnObjectsDistance{ 1500.0f };
		// 1 colours what the pass decided instead of whitening (for diagnosis): green snow, red over
		// the snow but on no top, yellow no top in reach, blue at or under the snow surface.
		int   snowOnObjectsDebug{ 0 };
		// For timing only: false keeps the probes but skips the whitening pass.
		bool  snowOnObjectsPass{ true };

		// Shoe prints on floors (IndoorPrints): boots pick up mud, snow (wet) or dust on the
		// landscape and leave a fading trail on floors that are not landscape (interiors, porches,
		// roads the snow leaves bare). Steps: prints until fully loaded boots are clean. FadeSeconds:
		// a dusty print's life (wet ones dry in 0.4 of it, mud lasts 1.5 times). Opacity 0-1 at
		// full strength. MaxIndoorPrints are kept (the oldest overwritten), drawn within Distance
		// (units) of the player. Flat screens only.
		bool  indoorPrints{ true };
		int   indoorPrintSteps{ 14 };
		float indoorPrintFadeSeconds{ 600.0f };
		float indoorPrintOpacity{ 0.8f };
		int   maxIndoorPrints{ 512 };
		float indoorPrintDistance{ 2048.0f };

		bool  snowSparkle{ true };
		float snowSparkleRate{ 1000.0f };
		float snowSparkleFullSpeed{ 350.0f };
		float snowSparkleMinSpeed{ 40.0f };
		float snowSparkleSize{ 1.6f };
		float snowSparkleLife{ 1.2f };
		float snowSparkleThrow{ 500.0f };
		float snowSparkleRise{ 80.0f };
		float snowSparkleInherit{ 1.0f };
		float snowSparkleGravity{ 300.0f };
		float snowSparkleDrag{ 4.2f };
		float snowSparkleSwirl{ 100.0f };
		float snowSparkleBrightness{ 0.75f };
		float snowSparkleOpacity{ 0.8f };
		float snowSparkleShape{ 1.0f };

		// ── surfaces ────────────────────────────────────────────────────────────

		bool enableSurfaceMaterial{ true };
		bool enableSurfaceClassification{ true };

		// Fallout 4 ships no snow landscape; winter mods either add snow land textures (found
		// by name or material) or repaint the vanilla ones in place, keeping their names.
		// -1 = each ground texture that looks like snow is snow (WinterLook measures them),
		// 0 = by name and material only, 1 = all ground but stone is snow, 2 = all ground is
		// snow.
		int winterMode{ -1 };
		bool logSurfaceClassification{ false };

		float blendFullDepth{ 4.0f };
		float blendStrength{ 0.45f };
		float snowRevealStrength{ 0.4f };

		// Texture-name keywords, used when a land texture's material type does not decide
		// (a mod's own material, or a texture the landscape shader cannot trace to an LTEX).
		std::string surfaceSnow{ "snow,ice,frost,frozen,winter" };
		std::string surfaceGrass{ "grass,lawn,scrub,weeds,moss,shrub,meadow" };
		std::string surfaceDirt{ "dirt,path,forestfloor,leaves,needles,roots,blastedforest,foothills,hilldirt,valleydirt,soil,commonwealthdefault" };
		std::string surfaceMud{ "mud,muck,silt,marsh,swamp,bog,nest,corrosion" };
		std::string surfaceSand{ "sand,oceanfloor,beach,dune" };
		std::string surfaceAsh{ "^ash,_ash,seaash,ashes,volcanic,burnt,scorch,cinder" };
		std::string surfaceGravel{ "gravel,rubble,debris,riverbedrocks,rockyshore,oyster,trash,pebble" };
		std::string surfaceStone{ "asphalt,concrete,cobble,rock,cliff,marble,brick,pavement,road,tile,metal,quarry" };

		// depth, radius, shoulder, decay, rim, print, clearance. Decay multiplies how long a
		// mark lasts relative to StampDecayPerSecond (2 = twice as long; see
		// Clipmap::RecoveryRate).
		Surfaces::Response surfaceResponse[static_cast<size_t>(Surfaces::Type::kCount)]{
			{ 0.60f, 1.00f, 0.35f, 1.00f, 0.60f, 1.00f, 1.0f },   // unknown
			{ 1.20f, 1.00f, 0.40f, 4.00f, 1.25f, 1.00f, 4.0f },   // snow
			{ 0.45f, 1.00f, 0.25f, 0.33f, 0.30f, 0.00f, 1.0f },   // grass
			{ 0.75f, 1.00f, 0.45f, 2.00f, 0.45f, 1.00f, 1.0f },   // dirt
			{ 1.10f, 1.05f, 0.35f, 3.00f, 0.70f, 1.00f, 1.0f },   // mud
			{ 0.90f, 1.15f, 0.15f, 0.10f, 1.10f, 1.00f, 1.0f },   // sand
			{ 1.00f, 1.10f, 0.20f, 1.00f, 1.00f, 1.00f, 1.0f },   // ash
			{ 0.35f, 1.00f, 0.20f, 0.25f, 0.50f, 0.00f, 1.0f },   // gravel
			{ 0.00f, 1.00f, 0.30f, 1.00f, 0.00f, 0.00f, 1.0f },   // stone
		};

		// ── diagnostics ─────────────────────────────────────────────────────────

		bool  enableLogging{ false };
		bool  logDraws{ false };
		bool  enableProfiler{ false };
		float profileInterval{ 5.0f };
		bool  profileDrawScopes{ false };
		bool  enableLiveReload{ true };
		float liveReloadInterval{ 0.5f };
	};

	inline Values values{};

	inline auto& enableTessellation = values.enableTessellation;
	inline auto& tessellationWinding = values.tessellationWinding;
	inline auto& logGeneratedShaders = values.logGeneratedShaders;
	inline auto& tessellationMaxFactor = values.tessellationMaxFactor;
	inline auto& tessellationTargetSpacing = values.tessellationTargetSpacing;
	inline auto& tessellationRaiseSpacing = values.tessellationRaiseSpacing;
	inline auto& tessellationBlanketSpacing = values.tessellationBlanketSpacing;
	inline auto& enableTessellationBounds = values.enableTessellationBounds;
	inline auto& tessellationBoundDepth = values.tessellationBoundDepth;
	inline auto& recomputeNormals = values.recomputeNormals;
	inline auto& normalGradientEpsilon = values.normalGradientEpsilon;
	inline auto& cullDistantDraws = values.cullDistantDraws;
	inline auto& enablePatchCulling = values.enablePatchCulling;
	inline auto& cullMargin = values.cullMargin;
	inline auto& routeOnlyPlayerCamera = values.routeOnlyPlayerCamera;
	inline auto& asyncShaderCompilation = values.asyncShaderCompilation;
	inline auto& debugWorldZOffset = values.debugWorldZOffset;
	inline auto& debugWaveAmplitude = values.debugWaveAmplitude;
	inline auto& debugWaveLength = values.debugWaveLength;
	inline auto& debugFieldColour = values.debugFieldColour;
	inline auto& debugBlendLayer = values.debugBlendLayer;
	inline auto& useClipmap = values.useClipmap;
	inline auto& clipmapLevels = values.clipmapLevels;
	inline auto& clipmapSeedSmoothing = values.clipmapSeedSmoothing;
	inline auto& stampRadiusScale = values.stampRadiusScale;
	inline auto& stampFootReach = values.stampFootReach;
	inline auto& stampGroundClearance = values.stampGroundClearance;
	inline auto& stampDepth = values.stampDepth;
	inline auto& stampDecayPerSecond = values.stampDecayPerSecond;
	inline auto& footprintMetres = values.footprintMetres;
	inline auto& stampFootNodes = values.stampFootNodes;
	inline auto& stampFootRadius = values.stampFootRadius;
	inline auto& stampFootSizeReference = values.stampFootSizeReference;
	inline auto& stampFootSizeMin = values.stampFootSizeMin;
	inline auto& stampFootSizeMax = values.stampFootSizeMax;
	inline auto& stampFootContactHeight = values.stampFootContactHeight;
	inline auto& stampFootPlantFraction = values.stampFootPlantFraction;
	inline auto& stampFootPlantSpeed = values.stampFootPlantSpeed;
	inline auto& snowTracks = values.snowTracks;
	inline auto& stampSyntheticStepLength = values.stampSyntheticStepLength;
	inline auto& stampSyntheticStepPerSpeed = values.stampSyntheticStepPerSpeed;
	inline auto& stampSyntheticStepWidth = values.stampSyntheticStepWidth;
	inline auto& stampSyntheticRevealDistance = values.stampSyntheticRevealDistance;
	inline auto& stampSyntheticRevealSeconds = values.stampSyntheticRevealSeconds;
	inline auto& powerArmorFootScale = values.powerArmorFootScale;
	inline auto& powerArmorDepthScale = values.powerArmorDepthScale;
	inline auto& enableStampShapes = values.enableStampShapes;
	inline auto& stampFootShape = values.stampFootShape;
	inline auto& stampFootLength = values.stampFootLength;
	inline auto& stampFootAspect = values.stampFootAspect;
	inline auto& stampFootSeparation = values.stampFootSeparation;
	inline auto& stampShapeKeyword = values.stampShapeKeyword;
	inline auto& stampRimHeight = values.stampRimHeight;
	inline auto& stampRimIndependence = values.stampRimIndependence;
	inline auto& stampRimSpan = values.stampRimSpan;
	inline auto& stampSlopeLimit = values.stampSlopeLimit;
	inline auto& stampReposeRate = values.stampReposeRate;
	inline auto& stampRimNoise = values.stampRimNoise;
	inline auto& stampRimLean = values.stampRimLean;
	inline auto& stampChurn = values.stampChurn;
	inline auto& snowStampRimSpan = values.snowStampRimSpan;
	inline auto& snowStampReposeRate = values.snowStampReposeRate;
	inline auto& snowStampRimNoise = values.snowStampRimNoise;
	inline auto& snowStampRimLean = values.snowStampRimLean;
	inline auto& snowStampChurn = values.snowStampChurn;
	inline auto& logStampShape = values.logStampShape;
	inline auto& logStampSurfaces = values.logStampSurfaces;
	inline auto& logStampFeet = values.logStampFeet;
	inline auto& traceFeet = values.traceFeet;
	inline auto& enableObjectStamps = values.enableObjectStamps;
	inline auto& objectStampRadiusScale = values.objectStampRadiusScale;
	inline auto& objectStampDepthScale = values.objectStampDepthScale;
	inline auto& objectFullSizeRadius = values.objectFullSizeRadius;
	inline auto& objectContactTolerance = values.objectContactTolerance;
	inline auto& objectSinkLimit = values.objectSinkLimit;
	inline auto& objectStampMaxDepth = values.objectStampMaxDepth;
	inline auto& objectRimMargin = values.objectRimMargin;
	inline auto& objectStampInterval = values.objectStampInterval;
	inline auto& logObjectStamps = values.logObjectStamps;
	inline auto& enableHeatSources = values.enableHeatSources;
	inline auto& heatKeywords = values.heatKeywords;
	inline auto& heatExcludeKeywords = values.heatExcludeKeywords;
	inline auto& heatRadius = values.heatRadius;
	inline auto& heatRadiusScale = values.heatRadiusScale;
	inline auto& heatMaxRadius = values.heatMaxRadius;
	inline auto& heatMeltPerSecond = values.heatMeltPerSecond;
	inline auto& heatShoulder = values.heatShoulder;
	inline auto& heatMeltsSnow = values.heatMeltsSnow;
	inline auto& heatMeltDepthScale = values.heatMeltDepthScale;
	inline auto& meltShowsGround = values.meltShowsGround;
	inline auto& logHeatSources = values.logHeatSources;
	inline auto& enableScorch = values.enableScorch;
	inline auto& scorchFadeSeconds = values.scorchFadeSeconds;
	inline auto& scorchDarkness = values.scorchDarkness;
	inline auto& enableThrusterBurn = values.enableThrusterBurn;
	inline auto& thrusterRaces = values.thrusterRaces;
	inline auto& thrusterNoScorch = values.thrusterNoScorch;
	inline auto& thrusterRadius = values.thrusterRadius;
	inline auto& thrusterReach = values.thrusterReach;
	inline auto& thrusterScorch = values.thrusterScorch;
	inline auto& thrusterMeltsSnow = values.thrusterMeltsSnow;
	inline auto& logThrusters = values.logThrusters;
	inline auto& enableExplosionImpacts = values.enableExplosionImpacts;
	inline auto& explosionImpactRadiusScale = values.explosionImpactRadiusScale;
	inline auto& explosionImpactMaxRadius = values.explosionImpactMaxRadius;
	inline auto& explosionImpactDepth = values.explosionImpactDepth;
	inline auto& enableProjectileImpacts = values.enableProjectileImpacts;
	inline auto& projectileImpactRadius = values.projectileImpactRadius;
	inline auto& projectileImpactDepth = values.projectileImpactDepth;
	inline auto& impactRimScale = values.impactRimScale;
	inline auto& impactSnowMelt = values.impactSnowMelt;
	inline auto& logImpacts = values.logImpacts;
	inline auto& enableBloodDecals = values.enableBloodDecals;
	inline auto& bloodDecalDepthMargin = values.bloodDecalDepthMargin;
	inline auto& bloodDecalGroundTolerance = values.bloodDecalGroundTolerance;
	inline auto& bloodDecalSnowOpacity = values.bloodDecalSnowOpacity;
	inline auto& maxBloodDecalsPerFrame = values.maxBloodDecalsPerFrame;
	inline auto& logBloodDecals = values.logBloodDecals;
	inline auto& corpseMarks = values.corpseMarks;
	inline auto& corpseBurial = values.corpseBurial;
	inline auto& corpseBurialSeconds = values.corpseBurialSeconds;
	inline auto& corpseMoundHeight = values.corpseMoundHeight;
	inline auto& maxCorpseMarks = values.maxCorpseMarks;
	inline auto& logCorpses = values.logCorpses;
	inline auto& enableWeather = values.enableWeather;
	inline auto& weatherSoftenHours = values.weatherSoftenHours;
	inline auto& weatherFirmHours = values.weatherFirmHours;
	inline auto& weatherSnowSoften = values.weatherSnowSoften;
	inline auto& weatherRainSoften = values.weatherRainSoften;
	inline auto& weatherCloudyFirmScale = values.weatherCloudyFirmScale;
	inline auto& weatherSoftDepthScale = values.weatherSoftDepthScale;
	inline auto& weatherFirmDepthScale = values.weatherFirmDepthScale;
	inline auto& weatherSoftDecayScale = values.weatherSoftDecayScale;
	inline auto& weatherFirmDecayScale = values.weatherFirmDecayScale;
	inline auto& weatherFillPerSecond = values.weatherFillPerSecond;
	inline auto& logWeather = values.logWeather;
	inline auto& enableSnowRaise = values.enableSnowRaise;
	inline auto& snowRaiseHeight = values.snowRaiseHeight;
	inline auto& snowRaiseWeather = values.snowRaiseWeather;
	inline auto& snowRaiseDistance = values.snowRaiseDistance;
	inline auto& snowRaiseFadeBand = values.snowRaiseFadeBand;
	inline auto& snowCoverageBudget = values.snowCoverageBudget;
	inline auto& snowTrenchDepth = values.snowTrenchDepth;
	inline auto& winterHardGroundDepth = values.winterHardGroundDepth;
	inline auto& snowTrenchCoverage = values.snowTrenchCoverage;
	inline auto& snowGroundFloor = values.snowGroundFloor;
	inline auto& snowGroundBite = values.snowGroundBite;
	inline auto& snowSeamTexels = values.snowSeamTexels;
	inline auto& snowSkipUnderwater = values.snowSkipUnderwater;
	inline auto& logSnowCoverage = values.logSnowCoverage;
	inline auto& enableShelter = values.enableShelter;
	inline auto& shelterBudget = values.shelterBudget;
	inline auto& shelterRefresh = values.shelterRefresh;
	inline auto& shelterTimeBudget = values.shelterTimeBudget;
	inline auto& shelterClearance = values.shelterClearance;
	inline auto& shelterHeight = values.shelterHeight;
	inline auto& shelterSeamTexels = values.shelterSeamTexels;
	inline auto& shelterMeshCap = values.shelterMeshCap;
	inline auto& shelterFloorTolerance = values.shelterFloorTolerance;
	inline auto& snowCoverLowMeshes = values.snowCoverLowMeshes;
	inline auto& snowCoverMeshHeight = values.snowCoverMeshHeight;
	inline auto& snowOnObjects = values.snowOnObjects;
	inline auto& snowOnObjectsStrength = values.snowOnObjectsStrength;
	inline auto& snowOnObjectsMinUp = values.snowOnObjectsMinUp;
	inline auto& snowOnObjectsDistance = values.snowOnObjectsDistance;
	inline auto& snowOnObjectsDebug = values.snowOnObjectsDebug;
	inline auto& snowOnObjectsPass = values.snowOnObjectsPass;
	inline auto& indoorPrints = values.indoorPrints;
	inline auto& indoorPrintSteps = values.indoorPrintSteps;
	inline auto& indoorPrintFadeSeconds = values.indoorPrintFadeSeconds;
	inline auto& indoorPrintOpacity = values.indoorPrintOpacity;
	inline auto& maxIndoorPrints = values.maxIndoorPrints;
	inline auto& indoorPrintDistance = values.indoorPrintDistance;
	inline auto& snowSparkle = values.snowSparkle;
	inline auto& snowSparkleRate = values.snowSparkleRate;
	inline auto& snowSparkleFullSpeed = values.snowSparkleFullSpeed;
	inline auto& snowSparkleMinSpeed = values.snowSparkleMinSpeed;
	inline auto& snowSparkleSize = values.snowSparkleSize;
	inline auto& snowSparkleLife = values.snowSparkleLife;
	inline auto& snowSparkleThrow = values.snowSparkleThrow;
	inline auto& snowSparkleRise = values.snowSparkleRise;
	inline auto& snowSparkleInherit = values.snowSparkleInherit;
	inline auto& snowSparkleGravity = values.snowSparkleGravity;
	inline auto& snowSparkleDrag = values.snowSparkleDrag;
	inline auto& snowSparkleSwirl = values.snowSparkleSwirl;
	inline auto& snowSparkleBrightness = values.snowSparkleBrightness;
	inline auto& snowSparkleOpacity = values.snowSparkleOpacity;
	inline auto& snowSparkleShape = values.snowSparkleShape;
	inline auto& enableSurfaceMaterial = values.enableSurfaceMaterial;
	inline auto& enableSurfaceClassification = values.enableSurfaceClassification;
	inline auto& winterMode = values.winterMode;
	inline auto& logSurfaceClassification = values.logSurfaceClassification;
	inline auto& blendFullDepth = values.blendFullDepth;
	inline auto& blendStrength = values.blendStrength;
	inline auto& snowRevealStrength = values.snowRevealStrength;
	inline auto& surfaceSnow = values.surfaceSnow;
	inline auto& surfaceGrass = values.surfaceGrass;
	inline auto& surfaceDirt = values.surfaceDirt;
	inline auto& surfaceMud = values.surfaceMud;
	inline auto& surfaceSand = values.surfaceSand;
	inline auto& surfaceAsh = values.surfaceAsh;
	inline auto& surfaceGravel = values.surfaceGravel;
	inline auto& surfaceStone = values.surfaceStone;
	inline auto& surfaceResponse = values.surfaceResponse;
	inline auto& enableLogging = values.enableLogging;
	inline auto& logDraws = values.logDraws;
	inline auto& enableProfiler = values.enableProfiler;
	inline auto& profileInterval = values.profileInterval;
	inline auto& profileDrawScopes = values.profileDrawScopes;
	inline auto& enableLiveReload = values.enableLiveReload;
	inline auto& liveReloadInterval = values.liveReloadInterval;

	void Load();

	// Parses a whole number the way the INI files are read: locale independent, the text
	// must be one finite number (surrounding blanks allowed, a leading '+' too).
	bool ParseFloat(std::string_view a_text, float& a_out);

	bool PollForChanges(float a_deltaSeconds);

	void ApplyLogLevel();
}
