// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Offsets.h"

#include "Clipmap.h"

#include "ClipmapUpdateCS.h"
#include "Corpses.h"
#include "FrameCamera.h"
#include "GameLayouts.h"
#include "Globals.h"
#include "Impacts.h"
#include "FootGeometry.h"
#include "FootprintProfiles.h"
#include "HiddenMarks.h"
#include "IndoorPrints.h"
#include "ObjectStamps.h"
#include "Physics.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShaderState.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SnowSparkle.h"
#include "SnowSurface.h"
#include "StampShapes.h"
#include "SurfaceProfiles.h"
#include "Tessellation.h"
#include "SurfaceTypes.h"
#include "Weather.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <string>
#include <vector>

namespace Clipmap
{
	namespace
	{

		struct WindowCB
		{
			float centreAndFade[4]{};

			float raise[4]{};

			float window1[4]{};
		};
		static_assert(sizeof(WindowCB) % 16 == 0);

		ID3D11Texture2D*           g_texture[kMaxLevels]{};
		ID3D11ShaderResourceView*  g_srv[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_uav[kMaxLevels]{};

		// Per-texel recovery rate and snow flag (ClipmapUpdateCS.h, EncodeMeta).
		ID3D11Texture2D*           g_metaTexture[kMaxLevels]{};
		ID3D11UnorderedAccessView* g_metaUAV[kMaxLevels]{};
		ID3D11ShaderResourceView*  g_metaSRV[kMaxLevels]{};

		// Each level's activity is written by one update and read by the next (to skip
		// flat tiles) and by the hull stage, so it alternates between two textures;
		// g_activityCurrent is the one the last update wrote.
		ID3D11Texture2D*           g_activity[kMaxLevels][2]{};
		ID3D11UnorderedAccessView* g_activityUAV[kMaxLevels][2]{};
		ID3D11ShaderResourceView*  g_activitySRV[kMaxLevels][2]{};
		uint32_t                   g_activityCurrent[kMaxLevels]{};

		ID3D11SamplerState*  g_sampler{ nullptr };
		ID3D11ComputeShader* g_updateCS{ nullptr };
		ID3D11ComputeShader* g_selectCS{ nullptr };
		SparseWork          g_work;
		ID3D11Buffer*        g_paramsCB{ nullptr };
		ID3D11Buffer*        g_windowCB{ nullptr };
		ID3D11Buffer*        g_halfCB[2]{};
		bool                 g_failed{ false };
		bool                 g_levelFailed[kMaxLevels]{};
		uint32_t             g_updates{ 0 };

		int32_t g_prevWindowX[kMaxLevels]{};
		int32_t g_prevWindowY[kMaxLevels]{};
		bool    g_prevWindowValid[kMaxLevels]{};

		float g_windowCentreX{ 0.0f };
		float g_windowCentreY{ 0.0f };
		float g_windowHalfExtent{ 0.0f };
		bool  g_windowValid{ false };
		float g_displacementBound{ 0.0f };  // monotonic until every field level is cleared

		struct ComputeStageGuard
		{
			static constexpr UINT kCBs = 2;
			static constexpr UINT kSRVs = 7;
			static constexpr UINT kUAVs = 4;

			explicit ComputeStageGuard(ID3D11DeviceContext* a_context) :
				_context(a_context)
			{
				_shader.Capture(_context);
				_context->CSGetConstantBuffers(0, kCBs, _cb);
				_context->CSGetUnorderedAccessViews(0, kUAVs, _uav);
				_context->CSGetShaderResources(0, kSRVs, _srv);
				_context->CSGetSamplers(0, 1, &_sampler);
			}

			~ComputeStageGuard()
			{
				_shader.Restore(_context);
				_context->CSSetConstantBuffers(0, kCBs, _cb);
				_context->CSSetUnorderedAccessViews(0, kUAVs, _uav, nullptr);
				_context->CSSetShaderResources(0, kSRVs, _srv);
				_context->CSSetSamplers(0, 1, &_sampler);

				for (auto*& cb : _cb) {
					if (cb) {
						cb->Release();
					}
				}
				for (auto*& srv : _srv) {
					if (srv) {
						srv->Release();
					}
				}
				if (_sampler) {
					_sampler->Release();
				}
				for (auto*& uav : _uav) {
					if (uav) {
						uav->Release();
					}
				}
			}

			ComputeStageGuard(const ComputeStageGuard&) = delete;
			ComputeStageGuard& operator=(const ComputeStageGuard&) = delete;

		private:
			ID3D11DeviceContext*       _context;

			ID3D11ShaderResourceView*  _srv[kSRVs]{};
			ID3D11SamplerState*        _sampler{ nullptr };
			ShaderState::Compute       _shader;
			ID3D11Buffer*              _cb[kCBs]{};
			ID3D11UnorderedAccessView* _uav[kUAVs]{};
		};

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		void ReleaseLevel(uint32_t a_level)
		{
			g_prevWindowValid[a_level] = false;
			g_activityCurrent[a_level] = 0;
			for (uint32_t i = 0; i < 2; ++i) {
				Drop(g_activitySRV[a_level][i]);
				Drop(g_activityUAV[a_level][i]);
				Drop(g_activity[a_level][i]);
			}
			Drop(g_metaSRV[a_level]);
			Drop(g_metaUAV[a_level]);
			Drop(g_metaTexture[a_level]);
			Drop(g_uav[a_level]);
			Drop(g_srv[a_level]);
			Drop(g_texture[a_level]);
		}

		bool LevelReady(uint32_t a_level)
		{
			return a_level < kMaxLevels && g_srv[a_level] && g_uav[a_level] &&
			       g_metaUAV[a_level] && g_metaSRV[a_level] && g_activityUAV[a_level][0] &&
			       g_activityUAV[a_level][1] && g_activitySRV[a_level][0] &&
			       g_activitySRV[a_level][1];
		}

		// Creates one level whole or not at all: a partial level is released again, so a
		// failure leaves nothing behind (and is not retried every frame).
		bool CreateLevel(ID3D11Device* a_device, uint32_t a_level)
		{
			if (a_level >= kMaxLevels || g_levelFailed[a_level]) {
				return false;
			}
			if (LevelReady(a_level)) {
				return true;
			}
			ReleaseLevel(a_level);

			const auto fail = [&](const char* a_what) {
				logger::error("Clipmap: {} failed for level {}", a_what, a_level);
				ReleaseLevel(a_level);
				g_levelFailed[a_level] = true;
				return false;
			};

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTexels;
			desc.Height = kTexels;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			// Zero bits are 0.0f and metadata 0 alike, so one buffer seeds both.
			const std::vector<uint32_t> zeros(static_cast<size_t>(kTexels) * kTexels, 0u);
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = zeros.data();
			initial.SysMemPitch = kTexels * sizeof(uint32_t);

			if (FAILED(a_device->CreateTexture2D(&desc, &initial, &g_texture[a_level]))) {
				return fail("CreateTexture2D");
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_texture[a_level], nullptr, &g_srv[a_level]))) {
				return fail("CreateShaderResourceView");
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_texture[a_level], nullptr, &g_uav[a_level]))) {
				return fail("CreateUnorderedAccessView");
			}

			// R32_UINT is the one integer format every Direct3D 11 device can load from a
			// UAV, which the update does for its metadata.
			D3D11_TEXTURE2D_DESC metaDesc = desc;
			metaDesc.Format = DXGI_FORMAT_R32_UINT;

			if (FAILED(a_device->CreateTexture2D(&metaDesc, &initial, &g_metaTexture[a_level]))) {
				return fail("CreateTexture2D (metadata)");
			}
			if (FAILED(a_device->CreateUnorderedAccessView(
					g_metaTexture[a_level], nullptr, &g_metaUAV[a_level]))) {
				return fail("CreateUnorderedAccessView (metadata)");
			}
			if (FAILED(a_device->CreateShaderResourceView(
					g_metaTexture[a_level], nullptr, &g_metaSRV[a_level]))) {
				return fail("CreateShaderResourceView (metadata)");
			}

			D3D11_TEXTURE2D_DESC activityDesc{};
			activityDesc.Width = kActivityTexels;
			activityDesc.Height = kActivityTexels;
			activityDesc.MipLevels = 1;
			activityDesc.ArraySize = 1;
			activityDesc.Format = DXGI_FORMAT_R32_UINT;
			activityDesc.SampleDesc.Count = 1;
			activityDesc.Usage = D3D11_USAGE_DEFAULT;
			activityDesc.BindFlags =
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			D3D11_SUBRESOURCE_DATA activityInitial{};
			activityInitial.pSysMem = zeros.data();
			activityInitial.SysMemPitch = kActivityTexels * sizeof(uint32_t);

			for (uint32_t i = 0; i < 2; ++i) {
				if (FAILED(a_device->CreateTexture2D(
						&activityDesc, &activityInitial, &g_activity[a_level][i]))) {
					return fail("CreateTexture2D (activity)");
				}
				if (FAILED(a_device->CreateUnorderedAccessView(
						g_activity[a_level][i], nullptr, &g_activityUAV[a_level][i]))) {
					return fail("CreateUnorderedAccessView (activity)");
				}
				if (FAILED(a_device->CreateShaderResourceView(
						g_activity[a_level][i], nullptr, &g_activitySRV[a_level][i]))) {
					return fail("CreateShaderResourceView (activity)");
				}
			}

			logger::info("Clipmap level {}: {}x{} texels over {:.0f} world units "
						 "({:.2f} per cell, +/- {:.1f} m)",
				a_level, kTexels, kTexels, WorldSizeFor(a_level), CellSizeFor(a_level),
				WorldSizeFor(a_level) * 0.5f / 70.0f);
			return true;
		}

		bool CompileUpdateShader(ID3D11Device* a_device, const char* a_entry,
			ID3D11ComputeShader** a_shader)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;

			const std::string source = UpdateShaderSource();

			const std::string maxStamps = std::to_string(kMaxStamps);
			const D3D_SHADER_MACRO defines[] = {
				{ "MAX_STAMPS", maxStamps.c_str() },
				{ "SPARSE_DISPATCH", "1" },
				{ nullptr, nullptr }
			};

			const HRESULT hr = ::D3DCompile(
				source.c_str(), source.size(), "ClipmapUpdateCS", defines, nullptr,
				a_entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

			if (FAILED(hr)) {
				logger::error("Clipmap {} CS failed to compile: {}", a_entry,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				if (errors) {
					errors->Release();
				}
				return false;
			}
			if (errors) {
				errors->Release();
			}

			const HRESULT createHr = a_device->CreateComputeShader(
				code->GetBufferPointer(), code->GetBufferSize(), nullptr, a_shader);
			code->Release();

			if (FAILED(createHr)) {
				logger::error("Clipmap: CreateComputeShader failed");
				return false;
			}
			return true;
		}

		using ActorPtr = RE::NiPointer<RE::Actor>;

		// Whether the middle-high list sits where CommonLibF4 has it: the lists keep a table of
		// their four arrays (allProcesss), and one of its pointers must be that list's own
		// address. Only compared, never followed, so a runtime that moved it reads the high list
		// alone.
		bool MiddleHighListed(const RE::ProcessLists& a_lists)
		{
			const bool found = std::ranges::any_of(a_lists.allProcesss,
				[&](const RE::BSTArray<RE::ActorHandle>* a_list) { return a_list == &a_lists.middleHighActorHandles; });
			static std::atomic<int> logged{ -1 };
			if (logged.exchange(found ? 1 : 0) != (found ? 1 : 0)) {
				logger::info("Actors: {}", found ? "the high and middle-high process lists press the ground" :
				                                   "the middle-high process list is not where expected - high process only");
			}
			return found;
		}

		// The actors the game keeps up to date: high process, and middle high (settlers and
		// passers-by a little further off, still loaded and walking, who left no trail when only
		// the high list was read).
		// TraceFeet: the process list each collected actor came from, and the loaded actors of
		// the lists not read (low, middle low), so the actor trace can say who was passed over.
		std::unordered_map<RE::TESFormID, const char*> g_actorList;
		std::vector<std::pair<ActorPtr, const char*>>  g_unreadActors;

		void CollectActors(std::vector<ActorPtr>& a_out)
		{
			g_actorList.clear();
			g_unreadActors.clear();
			if (auto* lists = Offsets::ReadGlobalPointer<RE::ProcessLists>(Offsets::ProcessListsSingleton)) {
				const auto add = [&a_out](RE::BSTArray<RE::ActorHandle>& a_handles, const char* a_list) {
					for (auto& handle : a_handles) {
						auto actor = handle.get();
						if (actor && globals::Loaded3D(actor.get())) {
							if (Settings::traceFeet) {
								g_actorList[actor->GetFormID()] = a_list;
							}
							a_out.push_back(std::move(actor));
						}
					}
				};
				add(lists->highActorHandles, "high");
				const bool middleHigh = MiddleHighListed(*lists);
				if (middleHigh) {
					add(lists->middleHighActorHandles, "middle high");
				}
				if (Settings::traceFeet && middleHigh) {
					const auto unread = [](RE::BSTArray<RE::ActorHandle>& a_handles, const char* a_list) {
						for (auto& handle : a_handles) {
							auto actor = handle.get();
							if (actor && globals::Loaded3D(actor.get())) {
								g_unreadActors.emplace_back(std::move(actor), a_list);
							}
						}
					};
					unread(lists->lowActorHandles, "low");
					unread(lists->middleLowActorHandles, "middle low");
				}
			}

			if (auto* player = globals::game::player; player && globals::Loaded3D(player)) {
				a_out.emplace_back(player);
			}
		}

		// Fallout 4 gives feet no collision bodies of their own, so an actor's feet are nodes found
		// by name (FootGeometry::ContactWalk): StampFootNodes words, else ankles, wheels or the ends
		// of its legs. At most this many per actor.
		constexpr size_t kMaxFeetPerActor = 8;

		// The StampFootNodes words, lowered once per change of the setting.
		struct FootWords
		{
			std::string              source;
			std::vector<std::string> words;
			bool                     parsed{ false };

			bool Refresh(const std::string& a_setting)
			{
				if (parsed && source == a_setting) {
					return false;
				}
				parsed = true;
				source = a_setting;
				words.clear();
				for (size_t start = 0; start <= a_setting.size();) {
					const auto comma = a_setting.find(',', start);
					const auto end = comma == std::string::npos ? a_setting.size() : comma;
					std::string word = a_setting.substr(start, end - start);
					std::erase_if(word, [](unsigned char c) { return std::isspace(c) != 0; });
					std::transform(word.begin(), word.end(), word.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (!word.empty()) {
						words.push_back(std::move(word));
					}
					if (comma == std::string::npos) {
						break;
					}
					start = comma + 1;
				}
				return true;
			}
		};

		FootWords g_footWords;
		std::unordered_map<uint32_t, FootprintProfiles::Shape> g_raceFootprints;
		std::string g_footprintKeywords;

		// Fallout4.esm's BrahminRace (the master always loads first): no keyword marks a brahmin.
		constexpr uint32_t kBrahminRace = 0x0002047E;

		FootprintProfiles::Profile FootprintFor(RE::Actor* a_actor)
		{
			if (g_footprintKeywords != Settings::stampShapeKeyword) {
				g_footprintKeywords = Settings::stampShapeKeyword;
				g_raceFootprints.clear();
			}

			const auto race = globals::RaceFormID(a_actor);
			if (const auto found = g_raceFootprints.find(race); race != 0 && found != g_raceFootprints.end()) {
				return FootprintProfiles::For(found->second);
			}

			// The mirelurk queen has a keyword of her own, and a feral ghoul walks on a person's
			// bare feet, not paws.
			const auto shape = FootprintProfiles::Select(
				globals::RaceHasAnyKeyword(a_actor, "ActorTypeDeathclaw"),
				globals::RaceHasAnyKeyword(a_actor, "ActorTypeMirelurk,ActorTypeMirelurkQueen"),
				race == kBrahminRace || globals::RaceHasAnyKeyword(a_actor, "ActorTypeRadStag"),
				globals::RaceHasAnyKeyword(a_actor, "ActorTypeYaoGuai"),
				globals::RaceHasAnyKeyword(a_actor, "ActorTypeDog"),
				globals::RaceHasAnyKeyword(a_actor, "ActorTypeRobot,ActorTypeLibertyPrime"),
				globals::RaceHasAnyKeyword(a_actor, "ActorTypeSuperMutantBehemoth"),
				globals::RaceHasAnyKeyword(a_actor, Settings::stampShapeKeyword) ||
					globals::RaceHasAnyKeyword(a_actor, "ActorTypeFeralGhoul"));
			if (race != 0) {
				g_raceFootprints.insert_or_assign(race, shape);
			}
			return FootprintProfiles::For(shape);
		}

		// An actor's ground contacts (FootGeometry::ContactWalk) over the engine's nodes.
		FootGeometry::Contacts<RE::NiAVObject> FindContacts(RE::NiAVObject* a_root)
		{
			if (!a_root || !a_root->IsNode()) {
				return {};
			}
			return FootGeometry::FindContacts(a_root, std::span<const std::string>(g_footWords.words), kMaxFeetPerActor,
				[](RE::NiAVObject* a_node) { return std::string_view{ a_node->name.c_str() }; },
				[](RE::NiAVObject* a_node, auto&& a_visit) {
					auto* node = a_node->IsNode();
					if (!node) {
						return;
					}
					for (auto& child : GameLayouts::Children(node)) {
						if (auto* below = child.get(); below && below->IsNode()) {
							a_visit(below);
						}
					}
				});
		}

		// What lies straight under an actor, from two short rays down (ProbeUnderfoot): the
		// landscape's own collision and the static meshes on it, as heights (NaN: no hit).
		struct Underfoot
		{
			bool  probed{ false };
			float x{ 0.0f }, y{ 0.0f };
			float terrainZ{ std::numeric_limits<float>::quiet_NaN() };
			float meshZ{ std::numeric_limits<float>::quiet_NaN() };

			// A mesh (a road, a rock, a foundation) above the landscape, or no landscape.
			[[nodiscard]] bool OnMesh() const
			{
				return !std::isnan(meshZ) && (std::isnan(terrainZ) || meshZ > terrainZ + 1.0f);
			}
			[[nodiscard]] bool OnLandscape() const { return !std::isnan(terrainZ) && !OnMesh(); }
		};

		// An actor's foot nodes, found by a walk of its whole skeleton and then reused: the
		// walk runs again when the actor's 3D changes, a foot leaves the scene graph (an
		// armour or power armour change), or every kFootRewalkFrames frames.
		struct FootCache
		{
			const RE::NiAVObject*                      root{ nullptr };
			uint64_t                                   walked{ 0 };
			uint64_t                                   seen{ 0 };
			std::vector<RE::NiPointer<RE::NiAVObject>> feet;
			// Each foot's toe bone (or null), for the way it points.
			std::vector<RE::NiPointer<RE::NiAVObject>> toes;
			// What the feet are (FootGeometry::Contact), and whether the body has thruster legs.
			FootGeometry::Contact contact{ FootGeometry::Contact::kNone };
			bool                  thruster{ false };
			// Each foot's position and planted state on frame `tracked`, to tell a planted
			// foot from a swinging one.
			std::vector<RE::NiPoint3> last;
			std::vector<uint8_t>      planted;
			uint64_t                  tracked{ 0 };
			// How far the actor has moved since any of its feet was last planted: feet that
			// never plant fall back to swept trails.
			float unplanted{ 0.0f };
			// Whether the real feet still press (FootGeometry::UpdateFeetMode): feet that
			// stopped (the player's body in first person) get synthetic steps. `rigid` is the
			// mode in use, `rigidLeft` the frame it last ended on.
			FootGeometry::FeetMode mode{};
			bool                   rigid{ false };
			uint64_t               rigidLeft{ 0 };
			// Mode switches not logged one by one (past the first lines), reported together.
			uint32_t                              switches{ 0 };
			std::chrono::steady_clock::time_point switchesSince{};
			// Synthetic steps for such legs: travel since the last one, the side of the next,
			// frames stood still since stepping, and the last print, pressed once more on the
			// next frame.
			float    stepTravel{ 0.0f };
			float    nextSide{ 1.0f };
			uint32_t stillFrames{ 0 };
			bool     stepping{ false };
			Stamp    held{};
			bool     holding{ false };
			// A trench the player ploughs out of sight is gathered into stretches (where the
			// stretch began, how long it is so far) before it waits to be pressed.
			float trenchFromX{ 0.0f };
			float trenchFromY{ 0.0f };
			float trenchTravel{ 0.0f };
			bool  trenchOpen{ false };
			// What lies under the actor (ProbeUnderfoot), kept for a few frames.
			Underfoot under{};
			uint64_t  underFrame{ 0 };
			// Floor prints (IndoorPrints): each foot planted on a floor last frame, and whether
			// this plant already left its print; synthetic steps on a floor keep a stride of
			// their own.
			std::vector<uint8_t> floorPlanted;
			std::vector<uint8_t> floorPrinted;
			// Your own feet under FRIK (PlayerFeetVisible): whether each is swinging, and the print
			// it left where it last came down, pressed once more on the next update.
			std::vector<uint8_t> swinging;
			std::vector<Stamp>   landed;
			std::vector<uint8_t> landedHeld;
			// Where each foot last pressed and on which frame: a planted foot that has not moved
			// presses again only now and then (kRepressFrames), so a crowd standing about does not
			// take the frame's stamps from people walking further off.
			std::vector<RE::NiPoint3> pressedAt;
			std::vector<uint64_t>     pressedFrame;
			float                floorStepTravel{ 0.0f };
			float                floorNextSide{ 1.0f };
			uint32_t             floorStill{ 0 };
			bool                 floorStepping{ false };
		};

		constexpr uint64_t kFootRewalkFrames = 30;

		std::unordered_map<RE::TESFormID, FootCache> g_footCache;

		struct Foot
		{
			RE::NiPoint3     position;
			std::string_view name;
			// How far the foot moved since the previous frame, when that frame saw it.
			float moveX{ 0.0f };
			float moveY{ 0.0f };
			bool  tracked{ false };
			// Whether it was planted on the previous frame as well.
			bool  plantedBefore{ false };
			size_t slot{ 0 };
			// Its toe bone's position, when it has one, for the way the foot points.
			RE::NiPoint3 toe;
			bool         hasToe{ false };
			// The side its name says (FootGeometry::SideFromName): -1 left, +1 right, 0 unknown.
			int nameSide{ 0 };
		};

		std::atomic<uint32_t> g_surfacesLogged{ 0 };

		std::atomic<uint32_t> g_sizesLogged{ 0 };
		constexpr uint32_t    kMaxSizesLogged = 4;

		void LogStampSize(const Stamp& a_stamp)
		{
			if (!Settings::logStampShape || a_stamp.kind != Stamp::Kind::kPrint) {
				return;
			}
			if (g_sizesLogged.load() >= kMaxSizesLogged ||
				g_sizesLogged.fetch_add(1) >= kMaxSizesLogged) {
				return;
			}

			const float length = a_stamp.radius * 2.0f;
			const float width = a_stamp.halfWidth * 2.0f;

			const float vertexSpacing = std::max(Settings::tessellationTargetSpacing, 0.01f);

			logger::info("Print size: {:.1f} x {:.1f} world units | {:.0f} x {:.0f} field "
						 "texels at {:.2f}/cell | about {:.1f} x {:.1f} GENERATED VERTICES "
						 "at spacing {:.1f}",
				length, width, length / kCellSize, width / kCellSize, kCellSize,
				length / vertexSpacing, width / vertexSpacing,
				Settings::tessellationTargetSpacing);

			if (length / vertexSpacing < 8.0f) {
				logger::warn("  that is too few vertices to carry a silhouette - the mark "
							 "will read as a smooth blob whatever the mask contains. The "
							 "field holds {:.0f}x more detail than the geometry samples. "
							 "Lower TessellationTargetSpacing (and raise "
							 "TessellationMaxFactor with it, or the cap just clamps it "
							 "back).",
					vertexSpacing / kCellSize);
			}
		}

		// A few lines while the actor stands, and more once it moves, where the planted test
		// matters.
		std::atomic<uint32_t> g_feetLogged{ 0 };
		std::atomic<uint32_t> g_stillFeetLogged{ 0 };
		constexpr uint32_t    kMaxFeetLogged = 60;
		constexpr uint32_t    kMaxStillFeetLogged = 6;

		// Actors switching between animated legs and synthetic steps, and the steps made.
		std::atomic<uint32_t> g_rigidLogged{ 0 };
		std::atomic<uint32_t> g_stepsLogged{ 0 };
		constexpr uint32_t    kMaxRigidLogged = 8;
		constexpr uint32_t    kMaxStepsLogged = 24;

		bool TakeLogLine(std::atomic<uint32_t>& a_counter, uint32_t a_limit)
		{
			return Settings::logStampFeet && a_counter.load() < a_limit && a_counter.fetch_add(1) < a_limit;
		}

		struct FootReport
		{
			std::string_view node;
			float            side{ 0.0f };
			float            radius{ 0.0f };
			float            above{ 0.0f };
			float            contact{ 0.0f };
			bool             isFoot{ false };
			float            speed{ -1.0f };
			float            plantLimit{ -1.0f };
			bool             presses{ false };
		};

		void LogFootShape(const FootReport& a_report, bool a_moving)
		{
			if (!Settings::logStampFeet) {
				return;
			}
			auto&          counter = a_moving ? g_feetLogged : g_stillFeetLogged;
			const uint32_t limit = a_moving ? kMaxFeetLogged : kMaxStillFeetLogged;
			if (counter.load() >= limit || counter.fetch_add(1) >= limit) {
				return;
			}

			const std::string plant =
				a_report.plantLimit < 0.0f ? std::string("plant test off") :
				a_report.speed < 0.0f      ? std::string("speed unknown") :
				                             std::format("foot {:6.1f}/s vs planted below {:6.1f}/s",
												 a_report.speed, a_report.plantLimit);
			logger::info("Foot test: node '{}' side {:+7.2f} vs separation {:.2f} -> {:<6} | "
						 "radius {:5.2f} | {:+7.2f} above land, presses below {:.2f} | {} -> {}",
				a_report.node, a_report.side, Settings::stampFootSeparation,
				a_report.isFoot ? "PRINT" : "circle", a_report.radius, a_report.above,
				a_report.contact, plant, a_report.presses ? "PRESS" : "no");
		}

		void LogStampSurface(const Surfaces::Ground& a_ground, const RE::NiPoint3& a_position)
		{
			if (!Settings::logStampSurfaces) {
				return;
			}

			const auto index = a_ground.profile >= 0 ?
								   static_cast<uint32_t>(a_ground.profile) +
									   static_cast<uint32_t>(Surfaces::Type::kCount) :
								   static_cast<uint32_t>(a_ground.type);
			const uint32_t bit = 1u << (index < 32u ? index : static_cast<uint32_t>(a_ground.type));
			if (g_surfacesLogged.fetch_or(bit) & bit) {
				return;
			}

			const auto* profile = Surfaces::ProfileName(a_ground.profile);
			const auto& response = a_ground.response;
			logger::info(
				"Stamp surface at ({:.0f}, {:.0f}): {:<8} depth x{:.2f} radius x{:.2f} "
				"shoulder {:.2f} decay x{:.2f}{}",
				a_position.x, a_position.y, Surfaces::Name(a_ground.type),
				response.depthScale, response.radiusScale, response.shoulder,
				response.decayScale,
				profile ? std::format("  <- profile [{}]", profile) :
						  std::string{ "  (no profile - keyword list or material id)" });
		}

		struct TrackedActor
		{
			float    x{ 0.0f }, y{ 0.0f };
			uint64_t frame{ 0 };
		};

		std::unordered_map<RE::TESFormID, TrackedActor> g_actorMotion;
		uint64_t                                     g_actorFrame{ 0 };

		constexpr float kMaxActorMotion = 48.0f;

		void TrackActorMotion(RE::TESFormID a_form, const RE::NiPoint3& a_position,
			float& a_motionX, float& a_motionY)
		{
			a_motionX = 0.0f;
			a_motionY = 0.0f;

			const auto previous = g_actorMotion.find(a_form);
			if (previous != g_actorMotion.end()) {
				const float dx = a_position.x - previous->second.x;
				const float dy = a_position.y - previous->second.y;
				if (dx * dx + dy * dy <= kMaxActorMotion * kMaxActorMotion) {
					a_motionX = dx;
					a_motionY = dy;
				}
			}

			g_actorMotion[a_form] = { a_position.x, a_position.y, g_actorFrame };
		}

		// TraceFeet: every foot of every moving actor, every frame, as CSV beside the log, for
		// studying footfalls offline. Bounded; the file starts afresh each game session.
		struct FootTraceRow
		{
			const RE::Actor*   actor;
			const Foot&        foot;
			const RE::NiPoint3& body;
			float              landZ;
			float              buried;
			float              footStep;
			float              plantStep;
			float              bodyStep;
			float              contact;
			bool               low;
			bool               planted;
			bool               presses;
			bool               rigid;
			Surfaces::Type     surface;
		};

		void TraceFoot(const FootTraceRow& a_row, float a_deltaSeconds)
		{
			static std::ofstream                          file;
			static bool                                   failed = false;
			static uint64_t                               rows = 0;
			static std::chrono::steady_clock::time_point start{};
			constexpr uint64_t                            kMaxRows = 500000;
			if (!Settings::traceFeet || failed || rows >= kMaxRows) {
				return;
			}
			if (!file.is_open()) {
				auto dir = F4SE::log::log_directory();
				if (dir && dir->generic_string().find("Fallout4") == std::string::npos) {
					*dir = dir->parent_path() / (REL::Module::IsVR() ? "Fallout4VR" : "Fallout4") / "F4SE";
				}
				if (dir) {
					file.open(*dir / "NMN_DeformableTerrain_feet.csv", std::ios::out | std::ios::trunc);
				}
				if (!file.is_open()) {
					failed = true;
					logger::warn("TraceFeet: could not open the trace file beside the log");
					return;
				}
				start = std::chrono::steady_clock::now();
				file << "seconds,frame,dt,actor,race,player,slot,node,footX,footY,footZ,landZ,buried,above,"
				        "footStep,plantStep,bodyStep,contact,low,planted,plantedBefore,presses,synthetic,"
				        "surface,bodyX,bodyY,bodyZ\n";
				logger::info("TraceFeet: tracing every moving actor's feet (at most {} rows)", kMaxRows);
			}
			const auto& foot = a_row.foot;
			const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
			file << std::format("{:.4f},{},{:.4f},{:08X},{:08X},{},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},"
			                    "{:.3f},{:.3f},{:.3f},{:.2f},{},{},{},{},{},{},{:.2f},{:.2f},{:.2f}\n",
				seconds, g_actorFrame, a_deltaSeconds, a_row.actor->GetFormID(), globals::RaceFormID(a_row.actor),
				a_row.actor == globals::game::player ? 1 : 0, foot.slot, foot.name, foot.position.x, foot.position.y,
				foot.position.z, a_row.landZ, a_row.buried, foot.position.z - a_row.landZ - a_row.buried, a_row.footStep,
				a_row.plantStep, a_row.bodyStep, a_row.contact, a_row.low ? 1 : 0, a_row.planted ? 1 : 0,
				foot.plantedBefore ? 1 : 0, a_row.presses ? 1 : 0, a_row.rigid ? 1 : 0, Surfaces::Name(a_row.surface),
				a_row.body.x, a_row.body.y, a_row.body.z);
			if (++rows % 4096 == 0 || rows >= kMaxRows) {
				file.flush();
			}
			if (rows >= kMaxRows) {
				logger::info("TraceFeet: {} rows written, the trace is full", rows);
			}
		}

		// TraceFeet's actor file: every actor the gather sees, every frame, and what became of it
		// (its feet checked, or which test passed it over), beside the log. Bounded.
		void TraceActor(const RE::Actor* a_actor, const RE::NiPoint3& a_eye, std::string_view a_what,
			std::string_view a_detail = {}, const char* a_list = nullptr)
		{
			static std::ofstream                          file;
			static bool                                   failed = false;
			static uint64_t                               rows = 0;
			static std::chrono::steady_clock::time_point start{};
			constexpr uint64_t                            kMaxRows = 300000;
			if (!Settings::traceFeet || failed || rows >= kMaxRows || !a_actor) {
				return;
			}
			if (!file.is_open()) {
				auto dir = F4SE::log::log_directory();
				if (dir && dir->generic_string().find("Fallout4") == std::string::npos) {
					*dir = dir->parent_path() / (REL::Module::IsVR() ? "Fallout4VR" : "Fallout4") / "F4SE";
				}
				if (dir) {
					file.open(*dir / "NMN_DeformableTerrain_actors.csv", std::ios::out | std::ios::trunc);
				}
				if (!file.is_open()) {
					failed = true;
					return;
				}
				start = std::chrono::steady_clock::now();
				file << "seconds,frame,actor,race,list,x,y,z,distance,what,detail\n";
			}
			if (!a_list) {
				const auto found = g_actorList.find(a_actor->GetFormID());
				a_list = found != g_actorList.end() ? found->second : (a_actor == globals::game::player ? "player" : "?");
			}
			const auto  at = a_actor->GetPosition();
			const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
			file << std::format("{:.4f},{},{:08X},{:08X},{},{:.1f},{:.1f},{:.1f},{:.1f},{},{}\n", seconds, g_actorFrame,
				a_actor->GetFormID(), globals::RaceFormID(a_actor), a_list, at.x, at.y, at.z, std::sqrt(a_eye.GetSquaredDistance(at)),
				a_what, a_detail);
			if (++rows % 4096 == 0 || rows >= kMaxRows) {
				file.flush();
			}
		}

		std::atomic<uint32_t> g_contactsLogged{ 0 };
		constexpr uint32_t    kMaxContactsLogged = 24;

		// An actor's ground contacts, cached: the body is walked again when its 3D changes, a foot
		// leaves the scene graph (an armour or power armour change), or every kFootRewalkFrames.
		FootCache& WalkFeet(RE::TESFormID a_form, RE::NiAVObject* a_root)
		{
			auto& cache = g_footCache[a_form];
			cache.seen = g_actorFrame;

			const bool detached = std::ranges::any_of(cache.feet,
				[](const RE::NiPointer<RE::NiAVObject>& a_foot) { return !a_foot->parent; });
			if (cache.root != a_root || detached || cache.walked + kFootRewalkFrames <= g_actorFrame) {
				auto previous = std::move(cache.feet);
				cache.feet.clear();
				cache.toes.clear();
				const auto found = FindContacts(a_root);
				for (size_t i = 0; i < found.nodes.size(); ++i) {
					cache.feet.emplace_back(found.nodes[i]);
					cache.toes.emplace_back(found.tips[i]);
				}
				const bool changed = previous != cache.feet || cache.thruster != found.thruster;
				cache.contact = found.kind;
				cache.thruster = found.thruster;
				cache.root = a_root;
				cache.walked = g_actorFrame;
				if (previous != cache.feet) {
					cache.last.clear();
					cache.planted.clear();
					cache.floorPlanted.clear();
					cache.floorPrinted.clear();
					cache.swinging.clear();
					cache.landed.clear();
					cache.landedHeld.clear();
					cache.pressedAt.clear();
					cache.pressedFrame.clear();
				}
				if (changed && TakeLogLine(g_contactsLogged, kMaxContactsLogged)) {
					std::string names;
					for (const auto& node : cache.feet) {
						names += names.empty() ? "" : ", ";
						names += node->name.c_str();
					}
					logger::info("Feet: {:08X} touches the ground with {} {}{}{}", a_form, cache.feet.size(),
						FootGeometry::ContactName(cache.contact), names.empty() ? "" : std::format(" ({})", names),
						cache.thruster ? " - thruster legs" : "");
				}
			}
			return cache;
		}

		// This frame's feet of an actor, from its cached contacts. An attached armour or
		// power-armour frame can carry its own copy of a foot bone at the same place; one mark is
		// enough.
		void FeetOf(RE::TESFormID a_form, RE::NiAVObject* a_root, std::vector<Foot>& a_out)
		{
			a_out.clear();
			auto& cache = WalkFeet(a_form, a_root);

			// Positions from the previous frame only: an actor skipped for a while (out of
			// range, or nothing to press on its ground) starts again unknown.
			const bool tracked = cache.tracked + 1 == g_actorFrame &&
			                     cache.last.size() == cache.feet.size();
			cache.last.resize(cache.feet.size());
			cache.planted.resize(cache.feet.size(), 0);
			cache.floorPlanted.resize(cache.feet.size(), 0);
			cache.floorPrinted.resize(cache.feet.size(), 0);
			cache.swinging.resize(cache.feet.size(), 0);
			cache.landed.resize(cache.feet.size());
			cache.landedHeld.resize(cache.feet.size(), 0);
			cache.pressedAt.resize(cache.feet.size());
			cache.pressedFrame.resize(cache.feet.size(), 0);
			cache.tracked = g_actorFrame;

			for (size_t i = 0; i < cache.feet.size(); ++i) {
				const auto& at = cache.feet[i]->world.translate;
				Foot foot{ at, cache.feet[i]->name.c_str() };
				foot.slot = i;
				foot.nameSide = FootGeometry::SideFromName(foot.name);
				if (i < cache.toes.size() && cache.toes[i] && cache.toes[i]->parent) {
					foot.toe = cache.toes[i]->world.translate;
					foot.hasToe = true;
					// An ankle stands over its paw, which comes down where the digits do.
					if (cache.contact == FootGeometry::Contact::kAnkles) {
						foot.position.z = std::min(foot.position.z, foot.toe.z);
					}
				}
				if (tracked) {
					foot.tracked = true;
					foot.moveX = at.x - cache.last[i].x;
					foot.moveY = at.y - cache.last[i].y;
					foot.plantedBefore = cache.planted[i] != 0;
				}
				cache.last[i] = at;
				cache.planted[i] = 0;

				const bool seen = std::ranges::any_of(a_out, [&](const Foot& a_other) {
					return a_other.position.GetSquaredDistance(at) < 1.0f;
				});
				if (!seen) {
					a_out.push_back(foot);
				}
			}
		}

		// Called for a foot found planted this frame (see AppendActorStamps).
		void NotePlanted(RE::TESFormID a_form, size_t a_slot)
		{
			auto found = g_footCache.find(a_form);
			if (found != g_footCache.end() && a_slot < found->second.planted.size()) {
				found->second.planted[a_slot] = 1;
				found->second.unplanted = 0.0f;
			}
		}

		// An actor whose feet have not planted over this much of its own travel has a
		// skeleton that does not animate its legs: its feet press swept trails as before.
		// Synthetic steps take over once no real foot pressed for FootGeometry::kRealFeetTravel,
		// well before this.
		constexpr float kUnplantedFallback = 384.0f;

		// Frames an actor stands still after synthetic steps before its trailing foot comes
		// down beside the other.
		constexpr uint32_t kStanceFrames = 6;

		// Synthetic steps that come back within this many frames of ending carry on with
		// their stride and side; after a longer break a new walk starts.
		constexpr uint64_t kResumeFrames = 90;

		// Mode switches past the first logged lines are summed and reported this often.
		constexpr auto kSwitchSummary = std::chrono::seconds(10);

		// The rays down under an actor start this far above its feet and end this far below
		// the lower of its feet and the land; their answer is kept this many frames.
		constexpr float    kProbeUp = 24.0f;
		constexpr float    kProbeDown = 48.0f;
		constexpr uint64_t kProbeFrames = 8;

		// What lies under an actor, by two rays straight down in its cell's physics world:
		// the landscape's collision layer alone, then the static meshes alone. Impacts casts
		// its rays from the same stamp gathering, so this runs where they already run. Tells
		// a step skipped on a road, a rock or a foundation (as it should be) from one
		// skipped because the land height read wrong.
		Underfoot ProbeUnderfoot(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_landZ)
		{
			Underfoot out{};
			auto*     cell = a_actor ? a_actor->GetParentCell() : nullptr;
			if (!cell || !Physics::Available()) {
				return out;
			}

			const RE::NiPoint3 from{ a_position.x, a_position.y, a_position.z + kProbeUp };
			const RE::NiPoint3 to{ a_position.x, a_position.y, std::min(a_position.z, a_landZ) - kProbeDown };
			const float        span = to.z - from.z;

			const float terrain = Physics::CastRay(cell, from, to, Physics::kLayerGround);
			const float mesh = Physics::CastRay(cell, from, to, Physics::kLayerStatic | Physics::kLayerAnimStatic);
			out.probed = true;
			out.x = a_position.x;
			out.y = a_position.y;
			if (terrain >= 0.0f) {
				out.terrainZ = from.z + terrain * span;
			}
			if (mesh >= 0.0f) {
				out.meshZ = from.z + mesh * span;
			}
			return out;
		}

		// The actor's cached probe, cast again once it is kProbeFrames old or the actor has
		// moved kProbeMove from where it was cast.
		constexpr float kProbeMove = 16.0f;

		const Underfoot& UnderfootOf(FootCache& a_track, RE::Actor* a_actor, const RE::NiPoint3& a_position,
			float a_landZ)
		{
			const auto& under = a_track.under;
			const float movedSq = (a_position.x - under.x) * (a_position.x - under.x) +
			                      (a_position.y - under.y) * (a_position.y - under.y);
			if (!under.probed || a_track.underFrame + kProbeFrames <= g_actorFrame ||
				movedSq > kProbeMove * kProbeMove) {
				a_track.under = ProbeUnderfoot(a_actor, a_position, a_landZ);
				a_track.underFrame = g_actorFrame;
			}
			return a_track.under;
		}

		// "landscape -2.1 (land height +0.3)": where the first surface under the actor lies
		// against its feet, and for the landscape how far GetLandHeight is from its collision.
		std::string DescribeUnderfoot(const Underfoot& a_under, float a_feetZ, float a_landZ)
		{
			if (!a_under.probed) {
				return "not probed";
			}
			if (a_under.OnLandscape()) {
				return std::format("landscape {:+.1f} (land height {:+.1f})", a_under.terrainZ - a_feetZ,
					a_landZ - a_under.terrainZ);
			}
			if (a_under.OnMesh()) {
				return std::isnan(a_under.terrainZ) ?
				           std::format("mesh {:+.1f}, no landscape", a_under.meshZ - a_feetZ) :
				           std::format("mesh {:+.1f} over landscape {:+.1f}", a_under.meshZ - a_feetZ,
							   a_under.terrainZ - a_feetZ);
			}
			return std::format("nothing within {:.0f}", kProbeUp + kProbeDown);
		}

		// The races in Settings::thrusterRaces, parsed again when the list changes.
		struct ThrusterRaces
		{
			std::string           source;
			std::vector<uint32_t> ids;
			bool                  parsed{ false };

			bool Contains(uint32_t a_race)
			{
				if (!parsed || source != Settings::thrusterRaces) {
					source = Settings::thrusterRaces;
					ids.clear();
					std::string_view rest = source;
					while (!rest.empty()) {
						const auto comma = rest.find(',');
						auto piece = rest.substr(0, comma);
						rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
						while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) {
							piece.remove_prefix(1);
						}
						while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) {
							piece.remove_suffix(1);
						}
						if (piece.starts_with("0x") || piece.starts_with("0X")) {
							piece.remove_prefix(2);
						}
						uint32_t id = 0;
						const auto [end, error] = std::from_chars(piece.data(), piece.data() + piece.size(), id, 16);
						if (!piece.empty() && error == std::errc{} && end == piece.data() + piece.size()) {
							ids.push_back(id);
						} else if (!piece.empty()) {
							logger::warn("ThrusterRaces: '{}' is not a hex form id", piece);
						}
					}
					parsed = true;
				}
				return a_race != 0 && std::ranges::find(ids, a_race) != ids.end();
			}
		};
		ThrusterRaces g_thrusterRaces;

		// Whether a surface's name is in a comma-separated list of names (lower case, no
		// spaces, as Settings stores keyword lists).
		bool SurfaceListed(std::string_view a_list, Surfaces::Type a_type)
		{
			const std::string_view name = Surfaces::Name(a_type);
			while (!a_list.empty()) {
				const auto comma = a_list.find(',');
				if (a_list.substr(0, comma) == name) {
					return true;
				}
				a_list = comma == std::string_view::npos ? std::string_view{} : a_list.substr(comma + 1);
			}
			return false;
		}

		std::atomic<uint32_t> g_thrustersLogged{ 0 };
		constexpr uint32_t    kMaxThrustersLogged = 12;

		// A jet-borne robot (Codsworth and the other Mr. Handy types): no feet, but a jet
		// that blackens the ground in a line along its path and melts the snow under it,
		// while it hovers within reach of the land (not over a floor or a roof).
		void AppendThrusterStamp(RE::Actor* a_actor, const RE::NiPoint3& a_position,
			std::vector<Stamp>& a_out)
		{
			float motionX = 0.0f;
			float motionY = 0.0f;
			TrackActorMotion(a_actor->GetFormID(), a_position, motionX, motionY);

			auto* const tes = globals::TES();
			float       landZ = a_position.z;
			if (!tes || !tes->GetLandHeight(a_position, landZ)) {
				return;
			}
			const float above = a_position.z - landZ;
			const bool  reaches = above <= Settings::thrusterReach && above >= -Settings::thrusterReach;

			const auto ground = Surfaces::GroundAt(a_position);
			const bool snow = ground.type == Surfaces::Type::kSnow;
			const bool melts = snow && Settings::thrusterMeltsSnow;
			const bool spared = SurfaceListed(Settings::thrusterNoScorch, ground.type);
			const float scorch =
				Settings::enableThrusterBurn && Settings::enableScorch && !spared ? Settings::thrusterScorch : 0.0f;

			if (Settings::logThrusters &&
				(g_thrustersLogged.load() < kMaxThrustersLogged &&
					g_thrustersLogged.fetch_add(1) < kMaxThrustersLogged)) {
				logger::info("Thruster: {:08X} at ({:.0f}, {:.0f}) {:+.1f} above land (reach {:.0f}) "
							 "on {} | moved {:.1f} | {}",
					a_actor->GetFormID(), a_position.x, a_position.y, above, Settings::thrusterReach,
					Surfaces::Name(ground.type), std::sqrt(motionX * motionX + motionY * motionY),
					!reaches                ? "too high, no burn" :
					melts && scorch > 0.0f ? "burns and melts" :
					melts                  ? "melts" :
					scorch > 0.0f          ? "burns" :
					                         "nothing (no scorch on this ground)");
			}

			if (!reaches || (scorch <= 0.0f && !melts)) {
				return;
			}

			Stamp stamp{};
			stamp.x = a_position.x;
			stamp.y = a_position.y;
			stamp.radius = Settings::thrusterRadius;
			stamp.shoulder = 0.35f;
			stamp.snow = snow;
			stamp.motionX = motionX;
			stamp.motionY = motionY;
			stamp.scorch = scorch;

			if (melts) {
				// All of the snow at once where the jet is (a melt rate of 1 per second
				// leaves nothing after any time step), down to the ground.
				stamp.kind = Stamp::Kind::kMelt;
				stamp.heat = true;
				stamp.depth = 1.0f;
				stamp.rim = -Weather::SnowDepth();
				stamp.decay = RecoveryRate(Settings::stampDecayPerSecond,
					ground.response.decayScale * Weather::DecayScale());
			} else {
				// Only the colour: a press of no depth leaves the height as it is.
				stamp.kind = Stamp::Kind::kPress;
				stamp.depth = 0.0f;
				stamp.rim = 0.0f;
				stamp.decay = 1.0f;
			}

			a_out.push_back(stamp);
		}

		// The player's synthetic marks waiting out of sight (HiddenMarks): a print, or a
		// stretch of trench, with the ground it covers and what its snow is kicked with when it
		// is finally pressed.
		struct HiddenMark
		{
			Stamp          stamp{};
			float          from[3]{};
			float          to[3]{};
			float          age{ 0.0f };
			Surfaces::Type surface{ Surfaces::Type::kUnknown };
			float          forwardX{ 0.0f };
			float          forwardY{ 1.0f };
			float          velocityX{ 0.0f };
			float          velocityY{ 0.0f };
			// Let out: pressed on that frame and once more on the next (as a synthetic print
			// always is), then dropped.
			bool released{ false };
		};

		HiddenMarks::Queue<HiddenMark> g_hidden;
		// Marks pushed out of a full queue, pressed on the next gather.
		std::vector<HiddenMark> g_hiddenOverflow;

		// Why marks were let out since the last summary line (LogStampFeet).
		struct HiddenCounts
		{
			uint32_t queued{ 0 };
			uint32_t outOfView{ 0 };
			uint32_t distance{ 0 };
			uint32_t age{ 0 };
			uint32_t overflow{ 0 };
		};
		HiddenCounts                          g_hiddenCounts;
		std::chrono::steady_clock::time_point g_hiddenSince{};
		std::atomic<uint32_t>                 g_hiddenLogged{ 0 };
		constexpr uint32_t                    kMaxHiddenLogged = 12;

		// A stretch of hidden trench is this long before it waits as one mark; a stretch that
		// would span more than kHiddenTrenchJump (a teleport, a mode switch) is dropped.
		constexpr float kHiddenTrenchStretch = 8.0f;
		constexpr float kHiddenTrenchJump = 64.0f;

		// Added to a mark's radius in the view test: the eye moves a little in the frames the
		// camera takes to come back from the GPU.
		constexpr float kHiddenViewMargin = 16.0f;

		// The camera further than this from the player is not the player's (a map, a menu
		// render): nothing is hidden from it.
		constexpr float kHiddenCameraReach = 512.0f;

		// The view is taken this much wider. Flat: the camera comes back from the GPU a few
		// frames late, and the player may have turned since. VR: the camera block holds one eye;
		// both eyes and the headset's own turn between frames see a little wider.
		constexpr float kHiddenFlatWiden = 1.3f;
		constexpr float kHiddenVRWiden = 1.25f;

		// Why the view a mark hides from could not be had (the marks then press at once).
		std::atomic<uint32_t> g_hiddenViewLogged{ 0 };
		constexpr uint32_t    kMaxHiddenViewLogged = 6;

		// A full-body first-person mod (Full Body First Person, True First Person; FRIK in VR)
		// shows and animates the body in first person: its feet then press as in third person.
		// Looked up once, in the world, when every F4SE plugin has loaded.
		bool BodyAnimatedInFirstPerson()
		{
			static const bool animated = [] {
				const bool found = GetModuleHandleW(L"FullBodyFirstPerson.dll") != nullptr ||
				                   GetModuleHandleW(L"TrueFirstPerson.dll") != nullptr ||
				                   GetModuleHandleW(L"FRIK.dll") != nullptr;
				logger::info("First person: {}", found ?
				                                     "a full-body first-person mod is loaded - its animated feet press" :
				                                     "no full-body first-person mod - the player's steps are made");
				return found;
			}();
			return animated;
		}

		// The player in first person (flat): the world camera sits in the head. The game does
		// not animate the third-person body then, so its feet say nothing; the player's steps
		// are made for them, as in third person, unless a full-body first-person mod animates
		// the body. VR is always seen from the head and shows no body without FRIK, yet still
		// walks the hidden body's legs: their prints formed in front of the player looking down.
		// The camera read back is a few frames old: it is held against where the player stood
		// when it drew, and the verdict is held over a step or a head bob.
		struct FirstPersonTrack
		{
			std::array<RE::NiPoint3, 8>      positions{};
			std::array<uint64_t, 8>          frames{};
			FootGeometry::FirstPersonState   state{};
			uint64_t                         decidedFrame{ ~0ull };
		};
		FirstPersonTrack g_firstPerson;

		bool PlayerFirstPerson(RE::Actor* a_actor, const RE::NiPoint3& a_position)
		{
			if (a_actor != globals::game::player || BodyAnimatedInFirstPerson()) {
				return false;
			}
			if (REL::Module::IsVR()) {
				return true;
			}
			auto&          track = g_firstPerson;
			const uint64_t frame = globals::FrameIndex();
			if (track.decidedFrame == frame) {
				return track.state.on;
			}
			track.decidedFrame = frame;
			track.positions[frame % track.positions.size()] = a_position;
			track.frames[frame % track.frames.size()] = frame;

			FrameCamera::Camera camera{};
			uint64_t            drew = 0;
			if (!FrameCamera::Latest(camera, &drew)) {
				return track.state.on;
			}
			RE::NiPoint3 then = a_position;
			if (track.frames[drew % track.frames.size()] == drew) {
				then = track.positions[drew % track.positions.size()];
			}
			return FootGeometry::UpdateFirstPerson(track.state,
				FootGeometry::CameraInHead(camera.eye[0], camera.eye[1], camera.eye[2], then.x, then.y, then.z));
		}

		// FRIK shows your own body in VR: its feet are where your prints belong, each pressed as
		// it comes down. A made-up step would land where no visible foot is, so none is made.
		// A foot of FRIK's swings faster than this (units a second): its swing runs at 300 to 600,
		// its planted foot drifts at up to 60 as FRIK settles it.
		constexpr float kVisibleSwingSpeed = 120.0f;

		// A heavy creature's melee lunge ploughs a track faster than this (units a second; a
		// deathclaw charges at 300 to 500), kLungeWidth foot radii wide.
		constexpr float kLungeSpeed = 150.0f;
		constexpr float kLungeWidth = 1.5f;

		// A planted foot that stays within kRepressReach of where it pressed presses again only
		// this many frames later, once it has pressed on the frame it came down and the next.
		constexpr uint64_t kRepressFrames = 30;
		constexpr float    kRepressReach = 1.5f;

		bool PlayerFeetVisible(const RE::Actor* a_actor)
		{
			return a_actor == globals::game::player && REL::Module::IsVR() && BodyAnimatedInFirstPerson();
		}

		// The camera this frame's marks are hidden from, while it is the player's. Flat: the
		// world camera the G-buffer was drawn with (FrameCamera; the engine's camera block is
		// not that camera there). VR: the engine's camera block for the view's axes and
		// projection, and the headset node for the eye.
		HiddenMarks::View HiddenView(const RE::NiPoint3& a_player)
		{
			HiddenMarks::View view{};
			const auto        none = [&view](const char* a_why) {
				if (TakeLogLine(g_hiddenViewLogged, kMaxHiddenViewLogged)) {
					logger::info("Hidden steps: no view to hide them from ({}) - they press at once", a_why);
				}
				return view;
			};
			alignas(16) float p[16];
			if (!REL::Module::IsVR()) {
				FrameCamera::Camera camera{};
				if (!FrameCamera::Latest(camera)) {
					return none("no camera read back yet");
				}
				std::copy_n(camera.eye, 3, view.eye);
				std::copy_n(camera.forward, 3, view.forward);
				std::copy_n(camera.right, 3, view.right);
				std::copy_n(camera.up, 3, view.up);
				std::copy_n(camera.proj, 16, p);
			} else {
				const auto* camera = globals::Camera();
				if (!camera) {
					return none("no camera block");
				}
				const auto& data = camera->camViewData;
				const auto  axis = [](const __m128& a_from, float (&a_to)[3]) {
					alignas(16) float v[4];
					_mm_store_ps(v, a_from);
					a_to[0] = v[0];
					a_to[1] = v[1];
					a_to[2] = v[2];
					const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					return length > 0.9f && length < 1.1f;
				};
				if (!axis(data.viewDir, view.forward) || !axis(data.viewRight, view.right) ||
					!axis(data.viewUp, view.up)) {
					return none("the camera block's view axes are not unit vectors");
				}
				// The block holds both eyes' view data on VR, so the flat layout's position
				// adjust after the first eye is the second eye's viewport there.
				const auto* headset = GameLayouts::HeadsetNode(globals::game::player);
				if (!headset) {
					return none("no headset node");
				}
				view.eye[0] = headset->world.translate.x;
				view.eye[1] = headset->world.translate.y;
				view.eye[2] = headset->world.translate.z;
				for (int row = 0; row < 4; ++row) {
					_mm_store_ps(p + row * 4, data.projMat[row]);
				}
			}

			const float dx = view.eye[0] - a_player.x;
			const float dy = view.eye[1] - a_player.y;
			const float dz = view.eye[2] - a_player.z;
			if (!std::isfinite(dx + dy + dz) || dx * dx + dy * dy + dz * dz > kHiddenCameraReach * kHiddenCameraReach) {
				return none("the camera is not at the player");
			}

			// The projection's diagonal scales x and y at unit depth whichever way the matrix
			// is stored; an off-centre one (a VR eye) shifts the view by its third-row or
			// third-column term, taken from both places.
			if (!(p[0] > 0.01f) || !(p[5] > 0.01f) || !std::isfinite(p[0] + p[5])) {
				return none("no usable projection");
			}
			const float shiftX = std::max(std::abs(p[2]), std::abs(p[8]));
			const float shiftY = std::max(std::abs(p[6]), std::abs(p[9]));
			const float widen = REL::Module::IsVR() ? kHiddenVRWiden : kHiddenFlatWiden;
			view.halfX = (1.0f + std::min(shiftX, 1.0f)) / p[0] * widen;
			view.halfY = (1.0f + std::min(shiftY, 1.0f)) / p[5] * widen;
			view.valid = true;
			return view;
		}

		void QueueHidden(HiddenMark a_mark)
		{
			++g_hiddenCounts.queued;
			if (auto evicted = g_hidden.Push(std::move(a_mark)); evicted && !evicted->released) {
				++g_hiddenCounts.overflow;
				g_hiddenOverflow.push_back(std::move(*evicted));
			}
		}

		void LogHiddenSummary()
		{
			auto& counts = g_hiddenCounts;
			if (!Settings::logStampFeet || counts.queued + counts.outOfView + counts.distance + counts.age +
			                                       counts.overflow ==
			                                   0) {
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (g_hiddenSince == std::chrono::steady_clock::time_point{}) {
				g_hiddenSince = now;
				return;
			}
			if (now - g_hiddenSince < kSwitchSummary) {
				return;
			}
			if (TakeLogLine(g_hiddenLogged, kMaxHiddenLogged)) {
				logger::info("Hidden steps: {} queued in the last {:.0f} s, pressed {} out of view, {} at {:.0f} "
							 "units, {} after {:.0f} s, {} from a full queue | {} waiting",
					counts.queued, std::chrono::duration<float>(now - g_hiddenSince).count(), counts.outOfView,
					counts.distance, Settings::stampSyntheticRevealDistance, counts.age,
					Settings::stampSyntheticRevealSeconds, counts.overflow, g_hidden.items.size());
			}
			counts = {};
			g_hiddenSince = now;
		}

		// Presses the hidden marks whose ground has left the view, that the player has left
		// far enough behind or that have waited long enough (all of them once hiding is
		// switched off), each with the snow it kicks.
		void AppendHiddenMarks(float a_deltaSeconds, const RE::NiPoint3& a_player, std::vector<Stamp>& a_out)
		{
			const auto press = [&](const HiddenMark& a_mark, bool a_kick) {
				// These are one-shot marks. Consume one only when it fits the GPU's actual
				// budget, before repeatable feet and objects are gathered and distance-cut.
				if (a_out.size() >= kMaxStamps) {
					return false;
				}
				a_out.push_back(a_mark.stamp);
				if (a_kick && SnowSparkle::Enabled()) {
					SnowSparkle::NoteContact(a_mark.surface, { a_mark.to[0], a_mark.to[1], a_mark.to[2] },
						a_mark.forwardX, a_mark.forwardY, a_mark.velocityX, a_mark.velocityY, 18.0f);
				}
				return true;
			};

			size_t consumed = 0;
			for (const auto& mark : g_hiddenOverflow) {
				if (!press(mark, true)) {
					break;
				}
				++consumed;
			}
			g_hiddenOverflow.erase(g_hiddenOverflow.begin(),
				g_hiddenOverflow.begin() + static_cast<std::ptrdiff_t>(consumed));

			if (!g_hidden.items.empty()) {
				const auto view = HiddenView(a_player);
				for (auto it = g_hidden.items.begin(); it != g_hidden.items.end();) {
					auto& mark = *it;
					if (mark.released) {
						it = press(mark, false) ? g_hidden.items.erase(it) : std::next(it);
						continue;
					}
					mark.age += std::max(a_deltaSeconds, 0.0f);
					const float dx = mark.to[0] - a_player.x;
					const float dy = mark.to[1] - a_player.y;
					const auto  release = HiddenMarks::Decide(view, mark.from, mark.to,
						mark.stamp.radius + kHiddenViewMargin, std::sqrt(dx * dx + dy * dy),
						Settings::stampSyntheticRevealDistance, mark.age, Settings::stampSyntheticRevealSeconds);
					if (release != HiddenMarks::Release::kKeep && !press(mark, true)) {
						++it;
						continue;
					}
					switch (release) {
					case HiddenMarks::Release::kKeep:
						++it;
						continue;
					case HiddenMarks::Release::kOutOfView:
						++g_hiddenCounts.outOfView;
						break;
					case HiddenMarks::Release::kDistance:
						++g_hiddenCounts.distance;
						break;
					case HiddenMarks::Release::kAge:
						++g_hiddenCounts.age;
						break;
					}
					mark.released = true;
					++it;
				}
			}
			LogHiddenSummary();
		}

		// What AppendActorStamps worked out for an actor, for its synthetic steps.
		struct StepContext
		{
			RE::Actor*                actor;
			const RE::NiPoint3&       position;
			const RE::NiPoint3&       forward;
			const RE::NiPoint3&       right;
			Surfaces::Type            surface;
			const Surfaces::Response& response;
			FootCache&                track;
			float                     deltaSeconds;
			float                     bodyStep;
			float                     motionX;
			float                     motionY;
			bool                      moving;
			// False where legs plough trenches instead of leaving prints (snow).
			bool                      steps;
			bool                      printed;
			bool                      oriented;
			// A creature's own print (claw, paw, hoof), and every print's shape and proportions.
			bool                       creatureShape;
			FootprintProfiles::Profile footprint;
			float                     sizeScale;
			float                     footScale;
			float                     footRadius;
			float                     contact;
			float                     reach;
			float                     ordinary;
			float                     decay;
			// The ground's share of an ordinary snow mark (thin snow over stone and gravel),
			// times heavy.
			float                     snowSink;
			// FootGeometry::Heavy: a big creature's rims and kicked-up snow.
			float                     heavy;
		};

		// Steps for legs that are not animated: a print every stride, alternately left and
		// right of the actor's path, as planted feet leave them; on snow two trenches, as
		// legs wading through it plough them. The frozen feet themselves press nothing.
		void AppendSyntheticSteps(const StepContext& a_ctx, std::vector<Stamp>& a_out)
		{
			auto& track = a_ctx.track;
			if (track.holding && a_out.size() < kGatherCap) {
				a_out.push_back(track.held);
				track.holding = false;
			}

			auto* const tes = globals::TES();
			float       landZ = a_ctx.position.z;
			if (!tes || !tes->GetLandHeight(a_ctx.position, landZ)) {
				return;
			}
			// On a road or path buried under the snow blanket the actor stands on the mesh,
			// a few units over the land, and wades through the snow over it all the same.
			const float buried = a_ctx.surface == Surfaces::Type::kSnow ?
			                         Shelter::BuriedAt(a_ctx.position.x, a_ctx.position.y) :
			                         0.0f;
			const float above = a_ctx.position.z - landZ;
			const bool  grounded = above - buried <= a_ctx.contact && above >= -a_ctx.reach;
			const float width = Settings::stampSyntheticStepWidth * a_ctx.sizeScale * a_ctx.footScale;
			const float speed = a_ctx.deltaSeconds > 0.0f ? a_ctx.bodyStep / a_ctx.deltaSeconds : 0.0f;

			// Too far above the land height, yet standing on the landscape's own collision:
			// the land height read wrong there, and the step is pressed all the same. On a
			// mesh (a road, a rock, a foundation) or in the air it is not.
			const auto onLandscapeAnyway = [&]() {
				const auto& under = UnderfootOf(track, a_ctx.actor, a_ctx.position, landZ);
				const float onIt = a_ctx.position.z - under.terrainZ;
				return under.OnLandscape() && onIt <= a_ctx.contact && onIt >= -a_ctx.reach;
			};

			const auto stampAt = [&](float a_side) {
				Stamp stamp{};
				stamp.snow = a_ctx.surface == Surfaces::Type::kSnow;
				stamp.x = a_ctx.position.x + a_ctx.right.x * a_side;
				stamp.y = a_ctx.position.y + a_ctx.right.y * a_side;
				stamp.radius = a_ctx.footRadius * Settings::stampRadiusScale * a_ctx.response.radiusScale;
				stamp.depth = Surfaces::MarkDepth(a_ctx.surface, a_ctx.ordinary, a_ctx.snowSink, stamp.x, stamp.y);
				stamp.shoulder = std::clamp(a_ctx.response.shoulder, 0.0f, 0.95f);
				stamp.decay = a_ctx.decay;
				stamp.rim = Surfaces::RimHeight(a_ctx.ordinary, a_ctx.response.rimScale) * a_ctx.heavy;
				return stamp;
			};

			// The player's own steps are never seen forming: in first person there are no feet
			// to see, so each waits out of sight (AppendHiddenMarks) before it presses.
			const bool hide = a_ctx.actor == globals::game::player;
			const auto hidden = [&](const Stamp& a_stamp, float a_fromX, float a_fromY) {
				HiddenMark mark{};
				mark.stamp = a_stamp;
				mark.from[0] = a_fromX;
				mark.from[1] = a_fromY;
				mark.from[2] = landZ;
				mark.to[0] = a_stamp.x;
				mark.to[1] = a_stamp.y;
				mark.to[2] = landZ;
				mark.surface = a_ctx.surface;
				mark.forwardX = a_ctx.forward.x;
				mark.forwardY = a_ctx.forward.y;
				if (a_ctx.deltaSeconds > 0.0f) {
					mark.velocityX = a_ctx.motionX / a_ctx.deltaSeconds;
					mark.velocityY = a_ctx.motionY / a_ctx.deltaSeconds;
				}
				QueueHidden(std::move(mark));
			};

			bool         pressed = false;
			bool         kick = false;
			RE::NiPoint3 kickAt = a_ctx.position;  // a print kicks its snow where it lands
			if (!a_ctx.steps) {
				// Snow: both legs push through it the whole way.
				const bool ploughing = a_ctx.moving && (grounded || onLandscapeAnyway());
				if (hide) {
					// Out of sight it is gathered into stretches, each swept over the same
					// ground the frame-by-frame trench would have pressed, and waits as one.
					if (ploughing) {
						if (!track.trenchOpen) {
							track.trenchOpen = true;
							track.trenchFromX = a_ctx.position.x - a_ctx.motionX;
							track.trenchFromY = a_ctx.position.y - a_ctx.motionY;
							track.trenchTravel = 0.0f;
						}
						track.trenchTravel += a_ctx.bodyStep;
					}
					const bool ends = track.trenchOpen && (ploughing ? track.trenchTravel >= kHiddenTrenchStretch :
					                                                   track.trenchTravel > 0.0f);
					if (ends) {
						const float moveX = a_ctx.position.x - track.trenchFromX;
						const float moveY = a_ctx.position.y - track.trenchFromY;
						if (moveX * moveX + moveY * moveY <= kHiddenTrenchJump * kHiddenTrenchJump) {
							for (const float side : { -width, width }) {
								Stamp stamp = stampAt(side);
								stamp.motionX = moveX;
								stamp.motionY = moveY;
								hidden(stamp, stamp.x - moveX, stamp.y - moveY);
							}
							pressed = true;
						}
						track.trenchFromX = a_ctx.position.x;
						track.trenchFromY = a_ctx.position.y;
						track.trenchTravel = 0.0f;
					}
					if (!ploughing) {
						track.trenchOpen = false;
					}
				} else if (ploughing) {
					for (const float side : { -width, width }) {
						if (a_out.size() >= kGatherCap) {
							break;
						}
						Stamp stamp = stampAt(side);
						stamp.motionX = a_ctx.motionX;
						stamp.motionY = a_ctx.motionY;
						a_out.push_back(stamp);
						pressed = true;
						kick = true;
					}
				}
			} else {
				const float base = Settings::stampSyntheticStepLength;
				const float stride = std::clamp(base + Settings::stampSyntheticStepPerSpeed * speed, base, base * 4.0f) *
				                     a_ctx.sizeScale;

				float side = 0.0f;
				if (a_ctx.moving) {
					track.stillFrames = 0;
					if (!track.stepping) {
						track.stepping = true;
						track.stepTravel = stride * 0.5f;
					}
					track.stepTravel += a_ctx.bodyStep;
					if (track.stepTravel >= stride) {
						track.stepTravel = std::fmod(track.stepTravel, stride);
						side = track.nextSide;
					}
				} else if (track.stepping && ++track.stillFrames >= kStanceFrames) {
					// Stopped: the trailing foot comes down beside the leading one.
					track.stepping = false;
					side = track.nextSide;
				}

				if (side != 0.0f) {
					track.nextSide = -side;
					const bool heightOff = !grounded && onLandscapeAnyway();
					if ((grounded || heightOff) && a_out.size() < kGatherCap) {
						Stamp stamp = stampAt(side * width);
						if (a_ctx.printed || a_ctx.oriented) {
							if (a_ctx.printed || a_ctx.creatureShape) {
								stamp.kind = Stamp::Kind::kPrint;
								stamp.mirror = static_cast<float>(a_ctx.footprint.shape) * (side < 0.0f ? -1.0f : 1.0f);
							}
							stamp.forwardX = a_ctx.forward.x;
							stamp.forwardY = a_ctx.forward.y;
							stamp.radius *= Settings::stampFootLength * a_ctx.footprint.lengthScale;
							stamp.halfWidth = stamp.radius * Settings::stampFootAspect * a_ctx.footprint.widthScale;
							stamp.x += a_ctx.forward.x * a_ctx.footRadius * 0.5f;
							stamp.y += a_ctx.forward.y * a_ctx.footRadius * 0.5f;
							stamp.depth = Surfaces::MarkDepth(a_ctx.surface, a_ctx.ordinary, a_ctx.snowSink, stamp.x, stamp.y);
						}
						LogStampSize(stamp);
						if (hide) {
							hidden(stamp, stamp.x, stamp.y);
						} else {
							a_out.push_back(stamp);
							track.held = stamp;
							track.holding = true;
							kick = true;
							kickAt = { stamp.x, stamp.y, a_ctx.position.z };
						}
						pressed = true;
					}
					if (TakeLogLine(g_stepsLogged, kMaxStepsLogged)) {
						const auto& under = UnderfootOf(track, a_ctx.actor, a_ctx.position, landZ);
						logger::info("Synthetic step: {:08X} {} | at ({:.0f}, {:.0f}) | speed {:6.1f}/s, stride "
									 "{:5.1f} | {:+7.2f} above land, presses below {:.2f} | under: {} -> {}",
							a_ctx.actor->GetFormID(), side < 0.0f ? "left " : "right", a_ctx.position.x,
							a_ctx.position.y, speed, stride, above, a_ctx.contact,
							DescribeUnderfoot(under, a_ctx.position.z, landZ),
							!pressed ? "no" :
							heightOff ? (hide ? "PRESS once out of view (land height read wrong here)" :
							                    "PRESS (land height read wrong here)") :
							hide      ? "PRESS once out of view" :
							            "PRESS");
					}
				}
			}

			if (kick && SnowSparkle::Enabled() && a_ctx.deltaSeconds > 0.0f) {
				SnowSparkle::NoteContact(a_ctx.surface, kickAt, a_ctx.forward.x, a_ctx.forward.y,
					a_ctx.motionX / a_ctx.deltaSeconds, a_ctx.motionY / a_ctx.deltaSeconds, 18.0f * a_ctx.heavy,
					a_ctx.heavy * a_ctx.heavy);
			}
		}

		// ── floor prints (IndoorPrints) ──────────────────────────────────────────────────
		// What an actor's feet do on floors that are not landscape (every interior, a porch or
		// a road the snow does not bury): a foot planted on two frames running leaves one print
		// per plant, coloured onto the floor by IndoorPrints with whatever the boots carry.
		struct FloorContext
		{
			RE::Actor*          actor;
			const RE::NiPoint3& position;
			const RE::NiPoint3& forward;
			const RE::NiPoint3& right;
			FootCache&          track;
			float               sizeScale;
			float               footRadius;
			float               contact;
			float               reach;
			float               bodyStep;
			float               deltaSeconds;
			bool                moving;
		};

		// One foot; true while it is planted on the ground under the actor (a floor, or the
		// land when a_onLand), the evidence that the real feet work there.
		bool FloorFoot(const FloorContext& a_ctx, const Foot& a_foot, float a_footStep, bool a_onLand, float a_centreX,
			float a_centreY)
		{
			auto& track = a_ctx.track;
			if (a_foot.slot >= track.floorPlanted.size() || a_foot.slot >= track.floorPrinted.size()) {
				return false;
			}
			// The floor's own plant test: the snow's trench rule (no plant test) does not apply.
			const float plantStep = Settings::stampFootPlantFraction * a_ctx.bodyStep +
			                        Settings::stampFootPlantSpeed * a_ctx.deltaSeconds;
			const float overFloor = a_foot.position.z - a_ctx.position.z;
			const bool  planted = a_foot.tracked && a_footStep <= plantStep && overFloor <= a_ctx.contact &&
			                     overFloor >= -a_ctx.reach;
			if (a_onLand) {
				track.floorPlanted[a_foot.slot] = 0;
				track.floorPrinted[a_foot.slot] = 0;
				return planted;
			}
			const bool before = track.floorPlanted[a_foot.slot] != 0;
			track.floorPlanted[a_foot.slot] = planted ? 1 : 0;
			if (!planted) {
				track.floorPrinted[a_foot.slot] = 0;
				return false;
			}
			// Two frames running (a throttled animation can hold a swinging foot still for one),
			// and one print per plant.
			if (!before || track.floorPrinted[a_foot.slot]) {
				return true;
			}
			track.floorPrinted[a_foot.slot] = 1;

			float pointX = a_ctx.forward.x;
			float pointY = a_ctx.forward.y;
			if (a_foot.hasToe) {
				FootGeometry::ToeHeading(a_foot.position.x, a_foot.position.y, a_foot.toe.x, a_foot.toe.y, pointX, pointY);
			}
			const float side = FootGeometry::Lateral(a_foot.position.x, a_foot.position.y, a_centreX, a_centreY,
				a_ctx.right.x, a_ctx.right.y);

			IndoorPrints::FloorStep step{};
			// The foot bone is the ankle; the middle of the sole lies ahead of it, on the floor.
			step.x = a_foot.position.x + pointX * a_ctx.footRadius * 0.5f;
			step.y = a_foot.position.y + pointY * a_ctx.footRadius * 0.5f;
			step.z = a_ctx.position.z;
			step.forwardX = pointX;
			step.forwardY = pointY;
			step.mirror = FootGeometry::MirrorFor(a_foot.nameSide, side);
			step.sizeScale = a_ctx.sizeScale;
			IndoorPrints::NoteFloorStep(a_ctx.actor->GetFormID(), step);
			return true;
		}

		// Synthetic steps on a floor, for legs the game does not animate (the player in first
		// person without a body mod): a print every stride, alternately left and right of the
		// path, as AppendSyntheticSteps lays them on the landscape. The player's own are not
		// seen forming.
		void SyntheticFloorSteps(const FloorContext& a_ctx)
		{
			auto&       track = a_ctx.track;
			const float speed = a_ctx.deltaSeconds > 0.0f ? a_ctx.bodyStep / a_ctx.deltaSeconds : 0.0f;
			const float base = Settings::stampSyntheticStepLength;
			const float stride =
				std::clamp(base + Settings::stampSyntheticStepPerSpeed * speed, base, base * 4.0f) * a_ctx.sizeScale;

			float side = 0.0f;
			if (a_ctx.moving) {
				track.floorStill = 0;
				if (!track.floorStepping) {
					track.floorStepping = true;
					track.floorStepTravel = stride * 0.5f;
				}
				track.floorStepTravel += a_ctx.bodyStep;
				if (track.floorStepTravel >= stride) {
					track.floorStepTravel = std::fmod(track.floorStepTravel, stride);
					side = track.floorNextSide;
				}
			} else if (track.floorStepping && ++track.floorStill >= kStanceFrames) {
				// Stopped: the trailing foot comes down beside the leading one.
				track.floorStepping = false;
				side = track.floorNextSide;
			}
			if (side == 0.0f) {
				return;
			}
			track.floorNextSide = -side;

			const float          width = Settings::stampSyntheticStepWidth * a_ctx.sizeScale;
			IndoorPrints::FloorStep step{};
			step.x = a_ctx.position.x + a_ctx.right.x * side * width + a_ctx.forward.x * a_ctx.footRadius * 0.5f;
			step.y = a_ctx.position.y + a_ctx.right.y * side * width + a_ctx.forward.y * a_ctx.footRadius * 0.5f;
			step.z = a_ctx.position.z;
			step.forwardX = a_ctx.forward.x;
			step.forwardY = a_ctx.forward.y;
			step.mirror = side < 0.0f ? -1.0f : 1.0f;
			step.sizeScale = a_ctx.sizeScale;
			step.hidden = a_ctx.actor == globals::game::player;
			IndoorPrints::NoteFloorStep(a_ctx.actor->GetFormID(), step);
		}

		// Feet for floor prints alone: every interior (no landscape at all), and outside on
		// ground the field does not mark (stone, a road's land), where AppendActorStamps does
		// not read the feet. On the landscape the boots are loaded or wiped by its ground.
		void ObserveFloors(RE::Actor* a_actor, float a_deltaSeconds, bool a_exterior)
		{
			auto* root = globals::Loaded3D(a_actor);
			if (!root) {
				return;
			}
			const RE::NiPoint3 position = a_actor->GetPosition();

			const float        heading = a_actor->data.angle.z;
			const RE::NiPoint3 forward{ std::sin(heading), std::cos(heading), 0.0f };
			const RE::NiPoint3 right{ std::cos(heading), -std::sin(heading), 0.0f };

			float motionX = 0.0f;
			float motionY = 0.0f;
			// From the 3D root, which moves with the feet every update: in VR an actor's position
			// arrives every other update (half read still, the next twice as far), which made a
			// planted foot read as sliding and a frozen pose carried along read as planted.
			TrackActorMotion(a_actor->GetFormID(), root->world.translate, motionX, motionY);
			const float bodyStep = std::sqrt(motionX * motionX + motionY * motionY);

			const float sizeScale = std::clamp(
				root->worldBound.fRadius / std::max(Settings::stampFootSizeReference, 1.0f),
				Settings::stampFootSizeMin, std::max(Settings::stampFootSizeMin, Settings::stampFootSizeMax));
			const float footScale = RE::PowerArmor::ActorInPowerArmor(*a_actor) ? Settings::powerArmorFootScale : 1.0f;

			static std::vector<Foot> feet;
			FeetOf(a_actor->GetFormID(), root, feet);
			auto& track = g_footCache[a_actor->GetFormID()];
			// Wheels leave no shoe prints on a floor.
			if (feet.empty() || track.contact == FootGeometry::Contact::kWheels) {
				return;
			}

			// Standing on the landscape (outside only): the boots take its ground.
			bool           onLand = false;
			Surfaces::Type surface = Surfaces::Type::kUnknown;
			const float    contact = Settings::stampFootContactHeight * sizeScale;
			const float    reach = Settings::stampFootReach;
			if (a_exterior) {
				auto* const tes = globals::TES();
				float       landZ = position.z;
				if (tes && tes->GetLandHeight(position, landZ)) {
					surface = Surfaces::GroundAt(position).type;
					const float buried =
						surface == Surfaces::Type::kSnow ? Shelter::BuriedAt(position.x, position.y) : 0.0f;
					const float above = position.z - landZ - buried;
					onLand = above <= contact && above >= -reach;
				}
			}
			if (onLand) {
				IndoorPrints::NoteGroundWalk(a_actor->GetFormID(), surface, bodyStep);
			}

			float centreX = root->world.translate.x;
			float centreY = root->world.translate.y;
			if (feet.size() >= 2) {
				centreX = 0.0f;
				centreY = 0.0f;
				for (const auto& foot : feet) {
					centreX += foot.position.x;
					centreY += foot.position.y;
				}
				centreX /= static_cast<float>(feet.size());
				centreY /= static_cast<float>(feet.size());
			}

			const FloorContext ctx{ a_actor, position, forward, right, track, sizeScale,
				Settings::stampFootRadius * sizeScale * footScale, contact, reach, bodyStep, a_deltaSeconds,
				bodyStep > 0.01f };
			// In first person the body's feet are frozen: they print nothing of their own.
			const bool firstPerson = PlayerFirstPerson(a_actor, position);
			bool       planted = false;
			if (!firstPerson) {
				for (const auto& foot : feet) {
					const float footStep = std::sqrt(foot.moveX * foot.moveX + foot.moveY * foot.moveY);
					planted = FloorFoot(ctx, foot, footStep, onLand, centreX, centreY) || planted;
				}
			}

			// Real feet while one of them comes down planted; legs that never do (first person)
			// get synthetic steps on the floor.
			const bool synthetic =
				(FootGeometry::UpdateFeetMode(track.mode, planted, bodyStep / std::max(sizeScale, 1.0f)) &&
					!PlayerFeetVisible(a_actor)) ||
				firstPerson;
			track.rigid = synthetic;
			if (synthetic && !onLand) {
				SyntheticFloorSteps(ctx);
			}
		}

		void AppendActorStamps(RE::Actor* a_actor, const RE::NiPoint3& a_eye,
			float a_maxDistanceSq, float a_deltaSeconds, std::vector<Stamp>& a_out)
		{
			if (!a_actor) {
				return;
			}
			if (a_out.size() >= kGatherCap) {
				TraceActor(a_actor, a_eye, "skipped: the frame's stamps are full");
				return;
			}

			auto* root = globals::Loaded3D(a_actor);
			if (!root) {
				TraceActor(a_actor, a_eye, "skipped: no 3D");
				return;
			}

			const RE::NiPoint3 position = a_actor->GetPosition();
			if (a_actor != globals::game::player && a_eye.GetSquaredDistance(position) > a_maxDistanceSq) {
				TraceActor(a_actor, a_eye, "skipped: too far");
				return;
			}

			// A robot on a jet: nothing touches the ground, and it has thruster legs (any robot can be
			// fitted with them) or is of a ThrusterRaces race (Codsworth's).
			const auto& body = WalkFeet(a_actor->GetFormID(), root);
			if ((Settings::enableThrusterBurn || Settings::thrusterMeltsSnow) && body.feet.empty() &&
				(body.thruster || g_thrusterRaces.Contains(globals::RaceFormID(a_actor)))) {
				AppendThrusterStamp(a_actor, position, a_out);
				TraceActor(a_actor, a_eye, "thruster");
				return;
			}

			const auto  ground = Surfaces::GroundAt(position);
			const auto  surface = ground.type;
			const auto& response = ground.response;

			LogStampSurface(ground, position);

			if (response.depthScale <= 0.0f &&
				Surfaces::RimHeight(0.0f, response.rimScale) <= 0.0f) {
				// Ground that takes no mark (stone, a road's land): the feet still matter for the
				// boots and for floor prints on what stands on it.
				if (IndoorPrints::Wanted()) {
					ObserveFloors(a_actor, a_deltaSeconds, true);
				}
				TraceActor(a_actor, a_eye, "skipped: its ground takes no mark", Surfaces::Name(surface));
				return;
			}

			const float feetZ = position.z;
			const float reach = Settings::stampFootReach;

			const float        heading = a_actor->data.angle.z;
			const float        headingSin = std::sin(heading);
			const float        headingCos = std::cos(heading);
			const RE::NiPoint3 forward{ headingSin, headingCos, 0.0f };
			const RE::NiPoint3 right{ headingCos, -headingSin, 0.0f };

			float motionX = 0.0f;
			float motionY = 0.0f;
			// From the 3D root, which moves with the feet every update: in VR an actor's position
			// arrives every other update (half read still, the next twice as far), which made a
			// planted foot read as sliding and a frozen pose carried along read as planted.
			TrackActorMotion(a_actor->GetFormID(), root->world.translate, motionX, motionY);

			const auto footprint = FootprintFor(a_actor);
			const bool booted = footprint.shape == FootprintProfiles::Shape::kBoot;
			// Ground that keeps no boot's sole (grass, gravel) keeps no claw or hoof either.
			const bool creatureShape = !booted && Settings::stampFootShape && response.print >= 0.5f;

			const bool printed =
				Settings::enableStampShapes && StampShapes::Ready() &&
				response.print >= 0.5f && booted;

			const bool oriented = Settings::stampFootShape && (booted || creatureShape);

			// A foot is scaled by the actor's size against a person's: a radroach's feet
			// press small dents and a deathclaw's broad ones.
			const float sizeScale = std::clamp(
				root->worldBound.fRadius / std::max(Settings::stampFootSizeReference, 1.0f),
				Settings::stampFootSizeMin,
				std::max(Settings::stampFootSizeMin, Settings::stampFootSizeMax));

			const bool  powerArmor = RE::PowerArmor::ActorInPowerArmor(*a_actor);
			const float footScale = powerArmor ? Settings::powerArmorFootScale : 1.0f;
			const float depthScale = powerArmor ? Settings::powerArmorDepthScale : 1.0f;

			// In first person the body's feet are frozen (the game animates them only now and
			// then): neither they nor what they seem to plant say anything; the steps are made,
			// with or without feet to find.
			const bool firstPerson = PlayerFirstPerson(a_actor, position);

			static std::vector<Foot> feet;
			FeetOf(a_actor->GetFormID(), root, feet);
			if (feet.empty() && !firstPerson) {
				TraceActor(a_actor, a_eye, "skipped: no feet");
				return;
			}
			// Wheels and treads (a sentry bot, a robobrain) roll: a wheel on the ground presses all
			// along its way, a groove, and never plants or steps.
			const bool rolling = body.contact == FootGeometry::Contact::kWheels;
			const bool visibleFeet = PlayerFeetVisible(a_actor);

			auto* const tes = globals::TES();

			const float clearance =
				Settings::stampGroundClearance * std::max(response.clearanceScale, 0.0f);
			const float contact = Settings::stampFootContactHeight * sizeScale + clearance;
			const float footContact = FootGeometry::FootContact(contact, Settings::stampFootContactHeight + clearance,
				visibleFeet);

			const float footRadius = Settings::stampFootRadius * sizeScale * footScale;

			// A creature bigger than a person presses deeper, into snow too, with a higher rim
			// and more kicked-up snow (FootGeometry::Heavy: a deathclaw about 1.6 times).
			const float heavy = FootGeometry::Heavy(sizeScale);
			// A big creature's planted foot node rides higher and varies more with its pose (a
			// deathclaw's 50 to 88 units over its base, lunging and rearing): its contact grows
			// with heavy squared. The planted-foot test still keeps swinging feet out.
			const float bodyContact = footContact * heavy * heavy;
			const float ordinary = Settings::stampDepth * response.depthScale *
			                       Weather::DepthScale() * depthScale * footprint.depthScale * heavy;
			const float decay = RecoveryRate(Settings::stampDecayPerSecond,
				response.decayScale * Weather::DecayScale());

			const bool onSnow = surface == Surfaces::Type::kSnow;

			// Whether an NPC stands on the ground (FootGeometry::OnGround), for a planted foot whose
			// own land query says otherwise (paths raised over the land the snow map misses).
			const RE::NiPoint3& base = root->world.translate;
			bool               onGround = false;
			if (!visibleFeet && tes) {
				float baseLand = base.z;
				if (tes->GetLandHeight(base, baseLand)) {
					const float buriedBase = onSnow ? Shelter::BuriedAt(base.x, base.y) : 0.0f;
					const float snowTop = onSnow ? SnowSurface::LiftAt(base.x, base.y) : 0.0f;
					onGround = FootGeometry::OnGround(base.z - baseLand, buriedBase, snowTop, contact, reach);
				}
			}

			// A planted foot stays put while the body moves over it; a swinging one moves
			// faster than the body. Pressing only planted feet leaves a row of prints.
			const float plantFraction = onSnow && Settings::snowTracks ? 0.0f : Settings::stampFootPlantFraction;
			const float bodyStep = std::sqrt(motionX * motionX + motionY * motionY);
			const float plantStep =
				plantFraction * bodyStep + Settings::stampFootPlantSpeed * a_deltaSeconds;
			const bool  moving = bodyStep > 0.01f;

			auto& track = g_footCache[a_actor->GetFormID()];
			if (std::ranges::any_of(feet, [](const Foot& a_foot) { return a_foot.tracked; })) {
				track.unplanted = std::min(track.unplanted + bodyStep, kUnplantedFallback * 2.0f);
			}
			const bool plantTest =
				!rolling && plantFraction > 0.0f && (visibleFeet || track.unplanted < kUnplantedFallback);

			// Across the body from the feet's own middle, not the actor's position: a body
			// another mod moved off it (True First Person, Full Body First Person) still has a
			// left and a right. One foot alone is measured from the 3D root.
			float centreX = root->world.translate.x;
			float centreY = root->world.translate.y;
			if (feet.size() >= 2) {
				centreX = 0.0f;
				centreY = 0.0f;
				for (const auto& foot : feet) {
					centreX += foot.position.x;
					centreY += foot.position.y;
				}
				centreX /= static_cast<float>(feet.size());
				centreY /= static_cast<float>(feet.size());
			}

			// Floor prints read the same feet: a foot planted on what stands over the land (a
			// porch, a road the snow does not bury) prints there with what the boots carry.
			const bool         floors = IndoorPrints::Wanted();
			const FloorContext floorCtx{ a_actor, position, forward, right, track, sizeScale, footRadius,
				Settings::stampFootContactHeight * sizeScale, reach, bodyStep, a_deltaSeconds, moving };
			bool floorPlanted = false;
			// The actor on the landscape itself loads its boots with the ground.
			bool actorOnLand = false;
			if (floors && tes) {
				float landZ = feetZ;
				if (tes->GetLandHeight(position, landZ)) {
					const float buried = onSnow ? Shelter::BuriedAt(position.x, position.y) : 0.0f;
					const float above = position.z - landZ - buried;
					actorOnLand = above <= contact && above >= -reach;
				}
				if (actorOnLand) {
					IndoorPrints::NoteGroundWalk(a_actor->GetFormID(), surface, bodyStep);
				}
			}

			// Every foot is tested every frame and its stamp kept aside: whether a real foot
			// presses decides between the real feet and synthetic steps, below.
			static std::vector<Stamp> real;
			real.clear();
			bool         plantedFoot = false;
			RE::NiPoint3 kickAt{};
			for (const auto& foot : feet) {
				const float footStep =
					std::sqrt(foot.moveX * foot.moveX + foot.moveY * foot.moveY);
				const bool planted = foot.tracked && footStep <= plantStep;

				// FRIK: one print where each foot comes down, the first slow update after a swing,
				// pressed on that update and the next. A foot that stays down presses nothing more.
				bool comesDown = false;
				if (visibleFeet && plantTest && foot.slot < track.swinging.size()) {
					if (track.landedHeld[foot.slot]) {
						track.landedHeld[foot.slot] = 0;
						real.push_back(track.landed[foot.slot]);
					}
					const float speed = foot.tracked && a_deltaSeconds > 0.0f ? footStep / a_deltaSeconds : 0.0f;
					comesDown = FootGeometry::Landed(track.swinging[foot.slot], speed, kVisibleSwingSpeed);
				}
				if (planted) {
					NotePlanted(a_actor->GetFormID(), foot.slot);
				}

				// Off the landscape (a floor, a rock, a roof) the land lies far below the
				// foot and nothing is pressed.
				float      landZ = feetZ;
				const bool haveLand = tes && tes->GetLandHeight(foot.position, landZ);
				const float above = foot.position.z - landZ;
				// A foot on a road or path the snow blanket buries stands that much over the
				// land and still wades through the snow over it.
				const float buried = onSnow ? Shelter::BuriedAt(foot.position.x, foot.position.y) : 0.0f;
				if (floors && !firstPerson && !rolling) {
					const bool footOnLand = haveLand && above - buried <= footContact && above >= -reach;
					floorPlanted = FloorFoot(floorCtx, foot, footStep, footOnLand, centreX, centreY) || floorPlanted;
				}

				// A big creature's foot node stands higher over its base (a deathclaw stands on
				// its toes, the node is its ankle, 40 to 60 units up): the reach grows with its size.
				if (foot.position.z > feetZ + reach * std::max(sizeScale, 1.0f) || !haveLand) {
					continue;
				}

				Stamp stamp{};
				stamp.snow = surface == Surfaces::Type::kSnow;
				stamp.x = foot.position.x;
				stamp.y = foot.position.y;
				stamp.radius = footRadius * Settings::stampRadiusScale * response.radiusScale;

				const float side = FootGeometry::Lateral(foot.position.x, foot.position.y, centreX, centreY,
					right.x, right.y);
				const bool  isFoot = (printed || oriented) && !rolling &&
				                    std::abs(side) >= Settings::stampFootSeparation;

				// From the first update a foot is down: at VR's frame rates a running foot is down
				// for one or two updates. A foot an animation left un-updated for one frame mid-swing
				// reads as still too, but rarely (7 of 1762 swing updates on a recorded walk).
				// On snow a foot standing inside the blanket wades through it, whatever it stands on
				// underneath: a path or yard the snow covers (pre-war Sanctuary walks 28 units over its
				// land, under a 35-unit blanket) that the mesh map does not always find.
				// Over its own land: within the contact height (a road the snow buries counts), or on
				// snow inside the blanket, whatever it stands on underneath. An NPC's planted foot also
				// counts while the actor stands on the ground and the foot lies low over its base.
				const bool  overLand = above >= -reach &&
				                      (above - buried <= bodyContact ||
				                          (onSnow &&
				                              above <= SnowSurface::LiftAt(foot.position.x, foot.position.y) + 0.5f * contact));
				const bool  low = overLand || (onGround && foot.position.z - base.z <= bodyContact);
				const bool  presses = visibleFeet && plantTest ? comesDown && low : low && (!plantTest || planted);
				// Animated legs plant a foot as the body moves on; frozen ones (your own body in
				// first person) carry it along, so where every low foot presses (snow tracks)
				// that alone says nothing about them.
				// A wheel rolling on the ground is as good as a planted foot.
				plantedFoot = plantedFoot || (low && moving && (rolling || planted));

				LogFootShape({ foot.name, side, stamp.radius, above, footContact, isFoot,
								 foot.tracked && a_deltaSeconds > 0.0f ? footStep / a_deltaSeconds : -1.0f,
								 plantTest ? (a_deltaSeconds > 0.0f ? plantStep / a_deltaSeconds : 0.0f) : -1.0f,
								 presses },
					moving);
				if (moving || footStep > 0.01f || presses) {
					TraceFoot({ a_actor, foot, root->world.translate, landZ, buried, footStep, plantStep, bodyStep, footContact, low, planted,
								  presses, track.rigid, surface },
						a_deltaSeconds);
				}

				if (!presses) {
					continue;
				}

				// A planted foot that has not moved since it pressed (and pressed once more) waits.
				if (plantTest && foot.slot < track.pressedAt.size()) {
					const auto& last = track.pressedAt[foot.slot];
					const auto  since = g_actorFrame - track.pressedFrame[foot.slot];
					const float dx = foot.position.x - last.x;
					const float dy = foot.position.y - last.y;
					const bool  still = track.pressedFrame[foot.slot] != 0 && dx * dx + dy * dy <= kRepressReach * kRepressReach;
					if (still && since > 1 && since < kRepressFrames) {
						continue;
					}
					if (!still || since >= kRepressFrames) {
						track.pressedAt[foot.slot] = foot.position;
						track.pressedFrame[foot.slot] = g_actorFrame;
					}
				}

				if (isFoot) {
					if (printed || creatureShape) {
						stamp.kind = Stamp::Kind::kPrint;
						stamp.mirror = static_cast<float>(footprint.shape) *
						               FootGeometry::MirrorFor(foot.nameSide, side);
					}

					// The way the foot points (ankle to toe), not where the body faces: True
					// First Person turns the hips toward the walk, and a strafing or
					// backpedalling foot points its own way.
					float pointX = forward.x;
					float pointY = forward.y;
					if (foot.hasToe) {
						FootGeometry::ToeHeading(foot.position.x, foot.position.y, foot.toe.x, foot.toe.y,
							pointX, pointY);
					}
					stamp.forwardX = pointX;
					stamp.forwardY = pointY;

					stamp.radius *= Settings::stampFootLength * footprint.lengthScale;
					stamp.halfWidth = stamp.radius * Settings::stampFootAspect * footprint.widthScale;

					// The foot bone is the ankle; the middle of the sole lies ahead of it.
					stamp.x += pointX * footRadius * 0.5f;
					stamp.y += pointY * footRadius * 0.5f;
				}

				stamp.depth = Surfaces::MarkDepth(surface, ordinary, ground.snowSink * heavy, stamp.x, stamp.y);
				stamp.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);
				stamp.decay = decay;
				stamp.rim = Surfaces::RimHeight(ordinary, response.rimScale) * heavy;

				// A planted foot is swept only by its own slip; a trench follows the body.
				if (stamp.kind != Stamp::Kind::kPrint) {
					stamp.motionX = plantTest ? foot.moveX : motionX;
					stamp.motionY = plantTest ? foot.moveY : motionY;
				}

				real.push_back(stamp);
				if (comesDown) {
					track.landed[foot.slot] = stamp;
					track.landedHeld[foot.slot] = 1;
				}
				kickAt.x += foot.position.x;
				kickAt.y += foot.position.y;
				kickAt.z += foot.position.z;
			}

			// Real feet while one of them planted within the last stretch of the actor's
			// travel; feet that stopped planting (the player's body in first person, a pose the
			// game only now and then animates) give way to synthetic steps until one plants.
			// A foot planted on a floor (a porch) is evidence too: the real feet work there.
			// A deathclaw's stride is three of a person's: its travel counts against its size.
			const bool synthetic = FootGeometry::UpdateFeetMode(track.mode, plantedFoot || floorPlanted,
				bodyStep / std::max(sizeScale, 1.0f));
			const bool rigid = (synthetic && !rolling && !visibleFeet) || firstPerson;
			if (rigid != track.rigid) {
				track.rigid = rigid;
				// A hidden trench stretch never spans a switch: its start belongs to the walk
				// before it.
				track.trenchOpen = false;
				if (rigid) {
					// Back within moments: the same walk carries on, stride and side kept.
					if (track.rigidLeft + kResumeFrames < g_actorFrame) {
						track.stepping = false;
					}
				} else {
					track.rigidLeft = g_actorFrame;
					track.holding = false;
				}
				if (TakeLogLine(g_rigidLogged, kMaxRigidLogged)) {
					logger::info("Feet: {:08X} {}", a_actor->GetFormID(),
						rigid ? "real feet stopped planting (first person?) - synthetic steps" :
						        "a real foot planted - real feet");
				} else if (Settings::logStampFeet) {
					if (track.switches++ == 0) {
						track.switchesSince = std::chrono::steady_clock::now();
					}
				}
			}
			if (track.switches > 0 && Settings::logStampFeet) {
				const auto now = std::chrono::steady_clock::now();
				if (now - track.switchesSince >= kSwitchSummary) {
					logger::info("Feet: {:08X} switched {} more time(s) between real feet and synthetic "
								 "steps in the last {:.0f} s, now {}",
						a_actor->GetFormID(), track.switches,
						std::chrono::duration<float>(now - track.switchesSince).count(),
						rigid ? "synthetic steps" : "real feet");
					track.switches = 0;
				}
			}

			// A creature bigger than a person lunging in a melee attack (a deathclaw's charge and
			// swipe) ploughs a track along its body's path, as wide as its stance, while it stands
			// on the ground and moves faster than kLungeSpeed.
			if (heavy > 1.0f && onGround && a_deltaSeconds > 0.0f && bodyStep / a_deltaSeconds > kLungeSpeed &&
				a_out.size() < kGatherCap && Corpses::InMeleeAttack(a_actor)) {
				Stamp lunge{};
				lunge.snow = onSnow;
				lunge.x = base.x;
				lunge.y = base.y;
				lunge.radius = footRadius * kLungeWidth * Settings::stampRadiusScale * response.radiusScale;
				lunge.depth = Surfaces::MarkDepth(surface, ordinary, ground.snowSink * heavy, base.x, base.y);
				lunge.shoulder = std::clamp(response.shoulder, 0.0f, 0.95f);
				lunge.decay = decay;
				lunge.rim = Surfaces::RimHeight(ordinary, response.rimScale) * heavy;
				lunge.motionX = motionX;
				lunge.motionY = motionY;
				a_out.push_back(lunge);
			}

			if (rigid) {
				// Off the landscape (a porch, a road the snow leaves bare) the made-up steps
				// print on the floor instead.
				if (floors && !actorOnLand) {
					SyntheticFloorSteps(floorCtx);
				}
				// Feet that do not press never plant either, so the swept-trail fallback must
				// not count their travel: steps or trenches follow the ground alone.
				track.unplanted = 0.0f;
				TraceActor(a_actor, a_eye, "synthetic steps", Surfaces::Name(surface));
				AppendSyntheticSteps({ a_actor, position, forward, right, surface, response, track, a_deltaSeconds,
										 bodyStep, motionX, motionY, moving, plantFraction > 0.0f, printed, oriented, creatureShape, footprint,
										 sizeScale, footScale, footRadius, contact, reach, ordinary, decay, ground.snowSink * heavy, heavy },
					a_out);
				return;
			}
			track.holding = false;
			// First person with the made-up steps switched off: the frozen feet press nothing.
			if (firstPerson) {
				return;
			}

			for (const auto& stamp : real) {
				if (a_out.size() >= kGatherCap) {
					break;
				}
				LogStampSize(stamp);
				a_out.push_back(stamp);
			}
			if (Settings::traceFeet) {
				TraceActor(a_actor, a_eye, "feet checked",
					std::format("{} {} pressed", Surfaces::Name(surface), real.size()));
			}

			// Snow is only kicked by a foot that touched the ground this frame, where it
			// touched (a body another mod moved off the actor's position kicks from its own
			// feet); the velocity comes from the tracked motion rather than the character
			// controller, which leaves a dead or still actor throwing nothing.
			if (!real.empty() && SnowSparkle::Enabled() && a_deltaSeconds > 0.0f) {
				const float count = static_cast<float>(real.size());
				SnowSparkle::NoteContact(surface, { kickAt.x / count, kickAt.y / count, kickAt.z / count },
					forward.x, forward.y, motionX / a_deltaSeconds, motionY / a_deltaSeconds, 18.0f * heavy,
					heavy * heavy);
			}
		}

		// How far from the player marks are made and shown: FootprintMetres (the menu's
		// footprint distance; 70 units make a metre), at most to 97% of the outer level's half
		// width, where its marks end.
		float FootprintReach(uint32_t a_outerLevel)
		{
			const float edge = static_cast<float>(kTexels / 2 - 2) * CellSizeFor(a_outerLevel) * 0.97f;
			return std::clamp(Settings::footprintMetres * 70.0f, 0.0f, edge);
		}

		// Stamps for this frame. Impacts and released hidden marks come first and are kept
		// (each is a one-off
		// event); feet and objects are pressed again next frame, so when the frame wants more
		// than the budget the furthest of them are cut.
		void GatherStamps(float a_deltaSeconds, std::vector<Stamp>& a_stamps)
		{
			++g_actorFrame;

			for (auto it = g_actorMotion.begin(); it != g_actorMotion.end();) {
				it = (it->second.frame + 120 < g_actorFrame) ? g_actorMotion.erase(it) :
				                                               std::next(it);
			}
			for (auto it = g_footCache.begin(); it != g_footCache.end();) {
				it = (it->second.seen + 120 < g_actorFrame) ? g_footCache.erase(it) :
				                                             std::next(it);
			}
			if (g_footWords.Refresh(Settings::stampFootNodes)) {
				g_footCache.clear();
			}

			a_stamps.clear();

			RE::NiPoint3 anchor{};
			if (auto* player = globals::game::player) {
				anchor = player->GetPosition();
			}

			Impacts::Append(anchor, a_stamps);
			AppendHiddenMarks(a_deltaSeconds, anchor, a_stamps);
			const size_t oneShot = std::min<size_t>(a_stamps.size(), kMaxStamps);

			static std::vector<ActorPtr> actors;
			actors.clear();
			CollectActors(actors);

			// Actors press as far as the ground shows marks (FootprintReach), into the outer
			// level past the fine level's 11 m. The fine level's reach alone (8 m) left people
			// running by across a street without a print.
			const float maxDistance = FootprintReach(LevelCount() - 1);
			const float maxDistanceSq = maxDistance * maxDistance;

			std::sort(actors.begin(), actors.end(),
				[&anchor](const ActorPtr& a_lhs, const ActorPtr& a_rhs) {
					return anchor.GetSquaredDistance(a_lhs->GetPosition()) <
					       anchor.GetSquaredDistance(a_rhs->GetPosition());
				});

			// A body lying dead presses nothing with its feet; Corpses marks it whole, after
			// everything else and only in the room that is left.
			static std::vector<RE::Actor*> dead;
			dead.clear();
			const bool corpses = Settings::corpseMarks && Corpses::Supported();
			for (const auto& actor : actors) {
				if (Corpses::IsDead(actor.get())) {
					if (corpses) {
						dead.push_back(actor.get());
					}
					TraceActor(actor.get(), anchor, "skipped: dead");
					continue;
				}
				AppendActorStamps(actor.get(), anchor, maxDistanceSq, a_deltaSeconds, a_stamps);
			}
			for (const auto& [unread, list] : g_unreadActors) {
				TraceActor(unread.get(), anchor, "not read: its process list", {}, list);
			}
			g_unreadActors.clear();

			ObjectStamps::Append(a_deltaSeconds, anchor, a_stamps);

			// The actors are held until here: dead points into them.
			Corpses::Append(a_deltaSeconds, anchor, dead, a_stamps);
			dead.clear();
			actors.clear();

			if (a_stamps.size() > kMaxStamps) {
				const auto distance = [&anchor](const Stamp& a_stamp) {
					const float dx = a_stamp.x - anchor.x;
					const float dy = a_stamp.y - anchor.y;
					return dx * dx + dy * dy;
				};
				const auto keep = a_stamps.begin() + static_cast<std::ptrdiff_t>(kMaxStamps);
				std::nth_element(a_stamps.begin() + static_cast<std::ptrdiff_t>(oneShot), keep,
					a_stamps.end(), [&](const Stamp& a_lhs, const Stamp& a_rhs) {
						return distance(a_lhs) < distance(a_rhs);
					});
			}
		}
	}

	void UpdateFloorFeet(float a_deltaSeconds)
	{
		if (!IndoorPrints::Wanted()) {
			return;
		}
		++g_actorFrame;
		for (auto it = g_actorMotion.begin(); it != g_actorMotion.end();) {
			it = (it->second.frame + 120 < g_actorFrame) ? g_actorMotion.erase(it) : std::next(it);
		}
		for (auto it = g_footCache.begin(); it != g_footCache.end();) {
			it = (it->second.seen + 120 < g_actorFrame) ? g_footCache.erase(it) : std::next(it);
		}
		if (g_footWords.Refresh(Settings::stampFootNodes)) {
			g_footCache.clear();
		}

		auto* player = globals::game::player;
		if (!player) {
			return;
		}
		const RE::NiPoint3 anchor = player->GetPosition();
		const float        reachSq = Settings::indoorPrintDistance * Settings::indoorPrintDistance;

		static std::vector<ActorPtr> actors;
		actors.clear();
		CollectActors(actors);
		for (const auto& actor : actors) {
			// A body lying dead walks nowhere: its still feet would read as planted.
			if (anchor.GetSquaredDistance(actor->GetPosition()) <= reachSq && !Corpses::IsDead(actor.get())) {
				ObserveFloors(actor.get(), a_deltaSeconds, false);
			}
		}
		actors.clear();
	}

	HiddenMarks::View PlayerView(const RE::NiPoint3& a_player)
	{
		return HiddenView(a_player);
	}

	void ResetWorld()
	{
		g_actorMotion.clear();
		g_footCache.clear();
		g_raceFootprints.clear();
		g_hidden.items.clear();
		g_hiddenOverflow.clear();
		g_windowValid = false;

		auto* context = globals::d3d::context;
		const float zeroFloat[4]{};
		const UINT  zeroUint[4]{};
		for (uint32_t level = 0; level < kMaxLevels; ++level) {
			g_prevWindowValid[level] = false;
			if (!context || !LevelReady(level)) {
				continue;
			}
			context->ClearUnorderedAccessViewFloat(g_uav[level], zeroFloat);
			context->ClearUnorderedAccessViewUint(g_metaUAV[level], zeroUint);
			context->ClearUnorderedAccessViewUint(g_activityUAV[level][0], zeroUint);
			context->ClearUnorderedAccessViewUint(g_activityUAV[level][1], zeroUint);
		}
		if (context) {
			g_displacementBound = 0.0f;
		}
		logger::info("Clipmap: field cleared for a new world");
	}

	void ResetDiagnostics()
	{
		g_sizesLogged.store(0);
		g_surfacesLogged.store(0);
		g_feetLogged.store(0);
		g_stillFeetLogged.store(0);
		g_rigidLogged.store(0);
		g_stepsLogged.store(0);
		g_hiddenLogged.store(0);
	}

	bool Ready()
	{
		return LevelReady(0) && g_sampler && g_updateCS && g_selectCS && g_work.args && g_paramsCB && g_windowCB &&
		       g_halfCB[0] && g_halfCB[1];
	}

	uint32_t ActiveLevels()
	{
		uint32_t levels = 0;
		while (levels < LevelCount() && LevelReady(levels)) {
			++levels;
		}
		return levels;
	}

	bool Initialize()
	{
		if (Ready()) {
			return true;
		}
		if (g_failed || !globals::Ready()) {
			return false;
		}

		auto* device = globals::d3d::device;

		const auto fail = [&](const char* a_what) {
			logger::error("Clipmap: {}", a_what);
			Release();
			g_failed = true;
			return false;
		};

		if (!CreateLevel(device, 0)) {
			return fail("level allocation failed");
		}
		for (uint32_t level = 1; level < LevelCount(); ++level) {
			if (!CreateLevel(device, level)) {
				logger::error("Clipmap: running with {} level(s)", level);
				break;
			}
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return fail("CreateSamplerState failed");
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(UpdateParams);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_paramsCB))) {
			return fail("CreateBuffer failed");
		}

		cbDesc.ByteWidth = sizeof(WindowCB);
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_windowCB))) {
			return fail("CreateBuffer (window) failed");
		}

		if (!CreateHalfBuffers(device, g_halfCB)) {
			return fail("CreateBuffer (checkerboard half) failed");
		}

		if (!g_work.Create(device, kTexels)) {
			return fail("sparse work allocation failed");
		}
		if (!CompileUpdateShader(device, "main", &g_updateCS) ||
			!CompileUpdateShader(device, "selectPairs", &g_selectCS)) {
			return fail("update shader unavailable");
		}

		logger::info("Clipmap ready: {} level(s), marks survive to {:.0f} world units "
					 "({:.1f} m) from the player",
			LevelCount(), WorldSizeFor(LevelCount() - 1) * 0.5f,
			WorldSizeFor(LevelCount() - 1) * 0.5f / 70.0f);
		return true;
	}

	void Release()
	{
		g_displacementBound = 0.0f;
		Drop(g_windowCB);
		Drop(g_paramsCB);
		Drop(g_halfCB[0]);
		Drop(g_halfCB[1]);
		Drop(g_updateCS);
		Drop(g_selectCS);
		g_work.Release();
		Drop(g_sampler);

		for (uint32_t level = 0; level < kMaxLevels; ++level) {
			ReleaseLevel(level);
		}
	}

	void Update(float a_deltaSeconds)
	{
		if (!Ready() || !globals::game::player) {
			return;
		}

		const auto position = globals::game::player->GetPosition();

		// The levels in use: level 0 always, then each coarser one that exists. The hull and
		// domain stages are generated for this count, so a change rebuilds them.
		uint32_t levels = 1;
		while (levels < LevelCount() && CreateLevel(globals::d3d::device, levels)) {
			++levels;
		}
		static uint32_t generatedFor = 0;
		if (levels != generatedFor) {
			if (generatedFor != 0) {
				logger::info("Clipmap: now {} level(s) - regenerating the terrain shaders", levels);
				Tessellation::Reset();
			}
			generatedFor = levels;
		}

		UpdateParams params{};

		params.window[3] = static_cast<float>(kTexels);

		const float dt = std::clamp(a_deltaSeconds, 0.0f, 0.25f);
		params.control[0] = dt;

		static std::vector<Stamp> stamps;
		const int64_t             gatherStart = Profiler::Ticks();
		GatherStamps(dt, stamps);
		Profiler::AddCpuTicks(Profiler::CpuScope::kGatherStamps, Profiler::Ticks() - gatherStart);
		const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(stamps.size()), kMaxStamps);
		params.control[1] = static_cast<float>(count);

		static size_t reportedDropped = 0;
		const size_t  dropped = stamps.size() - count;
		if (dropped != reportedDropped) {
			reportedDropped = dropped;
			if (dropped > 0) {
				logger::warn("Stamps: {} pressed, {} DROPPED - the frame wanted more than "
				             "the budget of {}, and the furthest feet and objects were cut",
					count, dropped, kMaxStamps);
			} else if (Settings::logObjectStamps) {
				logger::info("Stamps: {} pressed", count);
			}
		}

		params.control[2] = static_cast<float>(kTexels / 2 - 2);

		params.control[3] = std::max(Settings::stampRimSpan, 0.0f);

		params.weather[0] = Weather::FillPerSecond();

		params.weather[1] = Settings::stampSlopeLimit > 0.0f ?
								std::tan(std::clamp(Settings::stampSlopeLimit, 1.0f, 89.0f) *
									0.017453292f) :
								0.0f;
		params.weather[3] = std::clamp(Settings::stampRimNoise, 0.0f, 8.0f);

		// The scorch: a full burn fades over ScorchFadeSeconds; the seed changes the fade's
		// dither every update; a scorched tile reports a full mark's activity.
		static uint32_t scorchSeed = 0;
		scorchSeed = (scorchSeed + 1) & 0xFFFFu;
		params.weather[2] = 1.0f / std::max(Settings::scorchFadeSeconds, 1.0f);
		const float scorchActivity = std::max(Settings::tessellationBoundDepth, 0.01f);
		params.rimShape[0] = std::clamp(Settings::stampRimLean, 0.0f, 1.0f);
		params.rimShape[1] = std::clamp(Settings::stampChurn, 0.0f, 8.0f);
		const auto snowValue = [](float overrideValue, float globalValue) {
			return overrideValue < 0.0f ? globalValue : overrideValue;
		};
		const float reposeRate = std::clamp(Settings::stampReposeRate, 0.0f, 1.0f);
		const float snowReposeRate = std::clamp(
			snowValue(Settings::snowStampReposeRate, reposeRate), 0.0f, 1.0f);
		params.snowRim[0] = snowValue(Settings::snowStampRimSpan, params.control[3]);
		params.snowRim[1] = snowValue(Settings::snowStampRimNoise, params.weather[3]);
		params.snowRim[2] = snowValue(Settings::snowStampRimLean, params.rimShape[0]);
		params.snowRim[3] = snowValue(Settings::snowStampChurn, params.rimShape[1]);

		for (uint32_t i = 0; i < count; ++i) {
			params.stamps[i][0] = stamps[i].x;
			params.stamps[i][1] = stamps[i].y;
			params.stamps[i][2] = stamps[i].radius;
			params.stamps[i][3] = stamps[i].depth;

			params.stampParams[i][0] = stamps[i].shoulder;
			params.stampParams[i][1] = stamps[i].decay;

			params.stampParams[i][2] = static_cast<float>(stamps[i].kind);
			params.stampParams[i][3] = stamps[i].rim;

			params.stampShape[i][0] = stamps[i].forwardX;
			params.stampShape[i][1] = stamps[i].forwardY;
			params.stampShape[i][2] = stamps[i].halfWidth;
			// A print's mirror, or -(1 + margin) for an object's press (see ObjectRimMargin
			// in the update shader); 1 for any other press, which ignores it.
			params.stampShape[i][3] = stamps[i].kind == Stamp::Kind::kPress && stamps[i].rimMargin >= 0.0f ?
			                              -(1.0f + stamps[i].rimMargin) :
			                              stamps[i].mirror;

			params.stampMotion[i][0] = stamps[i].motionX;
			params.stampMotion[i][1] = stamps[i].motionY;
			params.stampMotion[i][2] = (stamps[i].snow ? 1.0f : 0.0f) + (stamps[i].heat ? 2.0f : 0.0f);
			params.stampMotion[i][3] = std::clamp(stamps[i].scorch, 0.0f, 1.0f);
		}

		auto* context = globals::d3d::context;

		{

			const auto fadeFor = [&](uint32_t a_level, float a_out[4]) {
				const float cell = CellSizeFor(a_level);
				const float validHalf = params.control[2] * cell;

				a_out[0] = std::floor(position.x / cell) * cell;
				a_out[1] = std::floor(position.y / cell) * cell;
				a_out[2] = validHalf * 0.80f;
				a_out[3] = validHalf * 0.97f;
				// The outer level shows marks out to the footprint distance, fading them over 5%
				// of its half width before it.
				if (a_level + 1 == levels) {
					const float reach = std::max(FootprintReach(a_level), cell);
					a_out[3] = reach;
					a_out[2] = std::max(reach - validHalf * 0.05f, 0.0f);
				}
			};

			WindowCB window{};
			fadeFor(0, window.centreAndFade);
			if (levels > 1) {
				fadeFor(1, window.window1);
			}

			window.raise[0] = Weather::RaiseScale();
			g_displacementBound = std::max(g_displacementBound, SubmittedDisplacementBound(params));
			window.raise[1] = g_displacementBound;

			{
				const bool floored = Settings::snowGroundFloor && Settings::enableSnowRaise &&
					Settings::useClipmap && Settings::snowRaiseHeight > 0.0f &&
					SnowCoverage::Ready();

				params.raise[0] = floored ? Settings::snowRaiseHeight : 0.0f;
				params.raise[1] = window.raise[0];
				params.raise[2] = std::max(Settings::snowGroundBite, 0.0f);

				params.raise[3] =
					(floored && Settings::shelterMeshCap && Shelter::View()) ? 1.0f : 0.0f;

				params.raiseWindow[0] = window.centreAndFade[0];
				params.raiseWindow[1] = window.centreAndFade[1];
				params.raiseWindow[3] = Settings::snowRaiseDistance;
				params.raiseWindow[2] = std::max(
					Settings::snowRaiseDistance - Settings::snowRaiseFadeBand, 0.0f);
			}

			D3D11_MAPPED_SUBRESOURCE windowMap{};
			if (FAILED(context->Map(g_windowCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &windowMap))) {
				// Keep the old field if its matching window/bound cannot be published.
				return;
			}
			std::memcpy(windowMap.pData, &window, sizeof(window));
			context->Unmap(g_windowCB, 0);
			g_windowCentreX = window.centreAndFade[0];
			g_windowCentreY = window.centreAndFade[1];
			g_windowHalfExtent = (levels > 1) ? window.window1[3] : window.centreAndFade[3];
			g_windowValid = true;
		}

		{
			const ComputeStageGuard guard(context);

			ID3D11ShaderResourceView* shape = StampShapes::View();
			context->CSSetShaderResources(0, 1, &shape);

			ID3D11ShaderResourceView* floorMaps[2] = {
				params.raise[0] > 0.0f ? SnowCoverage::View() : nullptr,
				params.raise[3] > 0.0f ? Shelter::View() : nullptr
			};
			if (!floorMaps[0]) {
				params.raise[0] = 0.0f;
			}
			if (!floorMaps[1]) {
				params.raise[3] = 0.0f;
			}
			context->CSSetShaderResources(3, 2, floorMaps);

			context->CSSetSamplers(0, 1, &g_sampler);
			context->CSSetShader(g_updateCS, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &g_paramsCB);

			Profiler::GpuBegin(Profiler::Scope::kClipmap);

			const uint32_t firstHalf = g_updates++ & 1u;

			for (uint32_t i = 0; i < levels; ++i) {
				const uint32_t level = levels - 1 - i;

				const float cell = CellSizeFor(level);

				params.window[0] = std::floor(position.x / cell);
				params.window[1] = std::floor(position.y / cell);
				params.window[2] = cell;

				const bool hasCoarser = (level + 1) < levels;
				params.coarse[0] = hasCoarser ? 1.0f : 0.0f;
				params.coarse[1] = WorldSizeFor(level + 1);
				params.coarse[2] = std::max(Settings::clipmapSeedSmoothing, 0.0f);

				const int32_t nowX = static_cast<int32_t>(params.window[0]);
				const int32_t nowY = static_cast<int32_t>(params.window[1]);

				int32_t moved = static_cast<int32_t>(kTexels);
				if (g_prevWindowValid[level]) {
					moved = std::max(std::abs(nowX - g_prevWindowX[level]),
						std::abs(nowY - g_prevWindowY[level]));
				}

				// Paused, nothing pressed and the window still: the update would change
				// nothing (decay, melt, fill and settling all scale with the frame time).
				if (dt <= 0.0f && count == 0 && moved == 0) {
					continue;
				}

				params.coarse[3] = std::clamp(params.control[2] - static_cast<float>(moved) - 2.0f,
					0.0f, params.control[2]);

				params.repose[0] = ReposeAlpha(reposeRate, dt, level);
				params.repose[1] = ReposeAlpha(snowReposeRate, dt, level);
				params.repose[2] = static_cast<float>(scorchSeed * 7919u % 65536u);
				params.repose[3] = scorchActivity;

				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(g_paramsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					break;
				}
				std::memcpy(mapped.pData, &params, sizeof(params));
				context->Unmap(g_paramsCB, 0);

				const uint32_t write = g_activityCurrent[level] ^ 1u;

				LevelViews views{};
				views.field = g_uav[level];
				views.meta = g_metaUAV[level];
				views.activity = g_activityUAV[level][write];
				views.prevActivity = g_activitySRV[level][g_activityCurrent[level]];
				views.coarseField = hasCoarser ? g_srv[level + 1] : nullptr;
				views.coarseMeta = hasCoarser ? g_metaSRV[level + 1] : nullptr;

				DispatchLevel(context, views, g_halfCB, firstHalf, kTexels, &g_work, g_selectCS, g_updateCS);

				g_activityCurrent[level] = write;
				g_prevWindowX[level] = nowX;
				g_prevWindowY[level] = nowY;
				g_prevWindowValid[level] = true;
			}

			ID3D11ShaderResourceView* nullFloor[2] = { nullptr, nullptr };
			context->CSSetShaderResources(3, 2, nullFloor);

			Profiler::GpuEnd();
		}
	}

	bool GetWindow(float& a_centreX, float& a_centreY, float& a_halfExtent)
	{
		if (!g_windowValid) {
			return false;
		}
		a_centreX = g_windowCentreX;
		a_centreY = g_windowCentreY;
		a_halfExtent = g_windowHalfExtent;
		return true;
	}

	ID3D11SamplerState* Sampler()
	{
		return g_sampler;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!Ready()) {
			return;
		}
		a_context->DSSetShaderResources(0, 1, &g_srv[0]);
		if (Settings::enableScorch || Settings::meltShowsGround) {
			a_context->DSSetShaderResources(kScorchSlot, 1, &g_metaSRV[0]);
		}
		a_context->DSSetSamplers(0, 1, &g_sampler);
		a_context->DSSetConstantBuffers(kParamsSlot, 1, &g_windowCB);
		a_context->HSSetConstantBuffers(kParamsSlot, 1, &g_windowCB);
		a_context->HSSetSamplers(0, 1, &g_sampler);

		a_context->HSSetShaderResources(kActivitySlot, 1, &g_activitySRV[0][g_activityCurrent[0]]);

		if (LevelReady(1)) {
			a_context->DSSetShaderResources(kLevel1Slot, 1, &g_srv[1]);
			a_context->HSSetShaderResources(kActivity1Slot, 1, &g_activitySRV[1][g_activityCurrent[1]]);
		}
	}
}
