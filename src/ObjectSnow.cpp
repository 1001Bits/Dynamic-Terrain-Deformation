// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "ObjectSnow.h"

#include "FrameCamera.h"
#include "Corpses.h"
#include "Globals.h"
#include "HeatSources.h"
#include "ObjectSnowRule.h"
#include "ObjectSnowShader.h"
#include "Offsets.h"
#include "Physics.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShaderState.h"
#include "SnowCoverage.h"
#include "Weather.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace ObjectSnow
{
	namespace
	{
		constexpr uint32_t kTotal = ObjectSnowRule::kTexels * ObjectSnowRule::kTexels;

		// Rays a batch may cast, and its CPU time, on the game thread.
		constexpr uint32_t kRaysPerBatch = 384;
		constexpr float    kBatchMilliseconds = 0.75f;

		// Once every square in reach is read, a slow re-read for statics that change (a door,
		// a workshop build): this many rays a batch, about 0.03 ms, so a whole reach of some
		// 27,000 squares is read again in about a minute at 60 frames a second.
		constexpr uint32_t kRefreshRaysPerBatch = 8;

		// A top this far over the snow surface counts as roof-high in the log line.
		constexpr float kRoofHigh = 100.0f;

		// The ray starts this far over the land, so a roof or a rock top below that is found.
		constexpr float kRayAbove = 2048.0f;
		constexpr float kRayBelow = 2.0f;

		// Pixels nearer the eye than this are the first-person weapon and hands, drawn with
		// their own projection; they are left alone.
		constexpr float kNearestPixel = 72.0f;

		// The ground around a top must be at least this much snow.
		constexpr float kSnowGround = 0.5f;

		// A refresh of the whole grid on the GPU at least this often, so a change of the snow
		// ground (the coverage sweep after a load, a winter switch) reaches the tops in time.
		constexpr uint32_t kUploadEveryFrames = 30;

		// ── the grid (game thread, under g_lock) ────────────────────────────────────
		std::mutex           g_lock;
		std::atomic<bool>    g_probeQueued{ false };
		std::atomic<uint64_t> g_probeEpoch{ 0 };
		std::vector<float>   g_top;      // world z of the topmost static, kNoTop where none
		std::vector<float>   g_land;     // world z of the land under the square, kNoTop where none
		std::vector<int32_t> g_tagX;     // which world cell each wrapped texel holds
		std::vector<int32_t> g_tagY;
		std::vector<int16_t> g_order;    // cell offsets, nearest first
		int32_t              g_centreX{ INT32_MIN };
		int32_t              g_centreY{ INT32_MIN };
		uint32_t             g_scan{ 0 };
		uint32_t             g_refresh{ 0 };
		bool                 g_changed{ true };
		uint32_t             g_batchRays{ 0 };
		int64_t              g_batchTicks{ 0 };

		// ── the GPU side (render thread) ────────────────────────────────────────────
		ID3D11VertexShader*       g_vs{ nullptr };
		ID3D11PixelShader*        g_ps{ nullptr };
		ID3D11Buffer*             g_params{ nullptr };
		ID3D11Texture2D*          g_topsTexture{ nullptr };
		ID3D11ShaderResourceView* g_topsView{ nullptr };
		ID3D11BlendState*         g_blend{ nullptr };
		ID3D11DepthStencilState*  g_noDepth{ nullptr };
		ID3D11RasterizerState*    g_raster{ nullptr };
		bool                      g_initialized{ false };
		bool                      g_failed{ false };

		std::vector<float> g_upload;
		uint32_t           g_framesSinceUpload{ kUploadEveryFrames };
		float              g_uploadCentreX{ 0.0f };
		float              g_uploadCentreY{ 0.0f };
		uint32_t           g_uploadedTops{ 0 };

		// What the G-buffer pass bound this frame.
		ID3D11RenderTargetView*   g_albedo{ nullptr };
		ID3D11Texture2D*          g_depthTexture{ nullptr };
		ID3D11ShaderResourceView* g_depthView{ nullptr };
		D3D11_VIEWPORT            g_viewport{};
		std::atomic<uint64_t>     g_capturedFrame{ ~0ull };
		std::mutex                g_captureLock;
		bool                      g_albedoSRGB{ false };
		bool                      g_rejectedReported{ false };
		bool                      g_readyReported{ false };

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		struct Params
		{
			float viewport[4];
			float grid[4];
			float reach[4];
			float look[4];
			float snow[4];  // snow depth over the land, debug colours, surface band, actor capsules
			float capsules[ObjectSnowRule::kMaxActorCapsules][8];  // end a, radius; end b, -
		};
		static_assert(sizeof(Params) % 16 == 0);
		static_assert(ObjectSnowRule::kMaxActorCapsules == 64 && ObjectSnowRule::kActorTopAbove == 2.0f,
			"ObjectSnowShader.h declares Capsules[128] (two rows each) and kActorTopAbove 2");

		// The body parts of the actors nearest the player within a_reach (living, dead, the
		// player), for the pass to leave them their own colour. Returns how many capsules.
		uint32_t GatherActors(float (&a_out)[ObjectSnowRule::kMaxActorCapsules][8], const RE::NiPoint3& a_from,
			float a_reach)
		{
			struct Near
			{
				float      distanceSq;
				RE::Actor* actor;
			};
			static std::vector<Near> nearby;
			nearby.clear();
			const auto consider = [&](RE::Actor* a_actor) {
				const auto* root = a_actor ? globals::Loaded3D(a_actor) : nullptr;
				if (!root) {
					return;
				}
				const auto& bound = root->worldBound;
				const float dx = bound.center.x - a_from.x;
				const float dy = bound.center.y - a_from.y;
				const float reach = a_reach + bound.fRadius;
				const float distanceSq = dx * dx + dy * dy;
				if (std::isfinite(distanceSq) && distanceSq <= reach * reach) {
					nearby.push_back({ distanceSq, a_actor });
				}
			};
			if (auto* lists = Offsets::ReadGlobalPointer<RE::ProcessLists>(Offsets::ProcessListsSingleton)) {
				for (auto& handle : lists->highActorHandles) {
					const auto actor = handle.get();
					consider(actor.get());
				}
			}
			consider(globals::game::player);
			std::sort(nearby.begin(), nearby.end(),
				[](const Near& a_lhs, const Near& a_rhs) { return a_lhs.distanceSq < a_rhs.distanceSq; });

			uint32_t count = 0;

			// Fires first: their whole bound stays bare.
			std::array<HeatSources::Hot, ObjectSnowRule::kMaxFires> fires{};
			const size_t heat = HeatSources::Nearby(a_from.x, a_from.y, a_reach, fires);
			for (size_t i = 0; i < heat && count < ObjectSnowRule::kMaxActorCapsules; ++i) {
				const auto& fire = fires[i];
				auto&       out = a_out[count++];
				out[0] = out[4] = fire.x;
				out[1] = out[5] = fire.y;
				out[2] = out[6] = fire.z;
				out[3] = fire.radius + ObjectSnowRule::kFireMargin;
				out[7] = 1.0f;
			}

			std::array<CorpseRule::Capsule, CorpseRule::kMaxCapsules> parts{};
			for (const auto& entry : nearby) {
				const size_t made = Corpses::BodyCapsules(entry.actor, parts);
				if (count + made > ObjectSnowRule::kMaxActorCapsules) {
					break;
				}
				for (size_t i = 0; i < made; ++i) {
					const auto& part = parts[i];
					auto&       out = a_out[count++];
					out[0] = part.a.x;
					out[1] = part.a.y;
					out[2] = part.a.z;
					out[3] = part.radius + ObjectSnowRule::kActorMargin;
					out[4] = part.b.x;
					out[5] = part.b.y;
					out[6] = part.b.z;
					out[7] = 0.0f;
				}
			}
			return count;
		}

		void BuildOrder()
		{
			// Every cell within the furthest distance, nearest first.
			const auto radius = static_cast<int32_t>(std::ceil(ObjectSnowRule::kMaxDistance / ObjectSnowRule::kTexelSize));
			struct Offset
			{
				int16_t x, y;
				int32_t d2;
			};
			std::vector<Offset> offsets;
			offsets.reserve(static_cast<size_t>((2 * radius + 1) * (2 * radius + 1)));
			for (int32_t y = -radius; y <= radius; ++y) {
				for (int32_t x = -radius; x <= radius; ++x) {
					const int32_t d2 = x * x + y * y;
					if (d2 <= radius * radius) {
						offsets.push_back({ static_cast<int16_t>(x), static_cast<int16_t>(y), d2 });
					}
				}
			}
			std::stable_sort(offsets.begin(), offsets.end(),
				[](const Offset& a_lhs, const Offset& a_rhs) { return a_lhs.d2 < a_rhs.d2; });
			g_order.clear();
			g_order.reserve(offsets.size() * 2);
			for (const auto& offset : offsets) {
				g_order.push_back(offset.x);
				g_order.push_back(offset.y);
			}
		}

		void ClearGrid()
		{
			g_top.assign(kTotal, ObjectSnowRule::kNoTop);
			g_land.assign(kTotal, ObjectSnowRule::kNoTop);
			g_tagX.assign(kTotal, INT32_MIN);
			g_tagY.assign(kTotal, INT32_MIN);
			g_scan = 0;
			g_refresh = 0;
			g_changed = true;
		}

		// How many order entries lie within the set distance.
		uint32_t OrderCount(float a_distance)
		{
			const float distance = std::clamp(a_distance, ObjectSnowRule::kTexelSize,
				ObjectSnowRule::kMaxDistance);
			const float cells = distance / ObjectSnowRule::kTexelSize;
			const auto  limit = static_cast<int32_t>(cells * cells);
			const auto  total = static_cast<uint32_t>(g_order.size() / 2);
			// The offsets are sorted by squared distance: a binary search finds the end.
			uint32_t lo = 0;
			uint32_t hi = total;
			while (lo < hi) {
				const uint32_t mid = (lo + hi) / 2;
				const int32_t  x = g_order[2 * static_cast<size_t>(mid)];
				const int32_t  y = g_order[2 * static_cast<size_t>(mid) + 1];
				if (x * x + y * y <= limit) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			return lo;
		}

		bool Wanted()
		{
			return Settings::snowOnObjects && Settings::enableSnowRaise && !REL::Module::IsVR();
		}

		// One frame's probes on the game thread (the shelter's reasons: a pick from the frame
		// hook waits for the Havok step; from the task it takes microseconds).
		void ProbeBatch(float a_distance, uint64_t a_epoch)
		{
			struct Done
			{
				~Done() { g_probeQueued.store(false, std::memory_order_release); }
			} done;

			if (g_probeEpoch.load(std::memory_order_acquire) != a_epoch) {
				return;
			}
			auto* player = globals::game::player;
			auto* tes = globals::TES();
			auto* cell = globals::PlayerCell();
			if (!player || !tes || !cell || !cell->IsExterior() || !Physics::Available()) {
				return;
			}
			// Update queues work only with snowy ground in reach. Do not read the
			// render thread's coverage grid from this game-thread task.

			const std::scoped_lock lock(g_lock);
			if (g_top.empty() || g_order.empty() || g_probeEpoch.load(std::memory_order_relaxed) != a_epoch) {
				return;
			}

			const int64_t      started = Profiler::Ticks();
			const auto         deadline = std::chrono::steady_clock::now() +
			                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
									  std::chrono::duration<float, std::milli>(kBatchMilliseconds));
			const RE::NiPoint3 position = player->GetPosition();
			const int32_t      centreX = ObjectSnowRule::CellOf(position.x);
			const int32_t      centreY = ObjectSnowRule::CellOf(position.y);
			if (centreX != g_centreX || centreY != g_centreY) {
				// The nearest-first pass restarts around the new centre; texels already read
				// for their cells are skipped at no cost.
				g_centreX = centreX;
				g_centreY = centreY;
				g_scan = 0;
			}

			const uint32_t count = OrderCount(a_distance);
			uint32_t       rays = 0;
			const auto     mayProbe = [&] {
				return rays < kRaysPerBatch && (rays == 0 || std::chrono::steady_clock::now() < deadline);
			};

			const auto probe = [&](int32_t a_cellX, int32_t a_cellY) {
				const uint32_t     index = ObjectSnowRule::Index(a_cellX, a_cellY);
				const RE::NiPoint3 at{ (static_cast<float>(a_cellX) + 0.5f) * ObjectSnowRule::kTexelSize,
					(static_cast<float>(a_cellY) + 0.5f) * ObjectSnowRule::kTexelSize, position.z };

				float top = ObjectSnowRule::kNoTop;
				float land = ObjectSnowRule::kNoTop;
				float landZ = 0.0f;
				if (tes->GetLandHeight(at, landZ)) {
					land = landZ;
					const RE::NiPoint3 from{ at.x, at.y, landZ + kRayAbove };
					const RE::NiPoint3 to{ at.x, at.y, landZ - kRayBelow };
					const float        fraction = Physics::CastRay(cell, from, to,
								   Physics::kLayerStatic | Physics::kLayerAnimStatic);
					if (fraction >= 0.0f) {
						top = from.z + (to.z - from.z) * fraction;
					}
				}
				++rays;

				if (g_top[index] != top || g_land[index] != land || g_tagX[index] != a_cellX ||
					g_tagY[index] != a_cellY) {
					g_changed = true;
				}
				g_top[index] = top;
				g_land[index] = land;
				g_tagX[index] = a_cellX;
				g_tagY[index] = a_cellY;
			};

			// Squares not read yet for their cell, nearest first.
			while (g_scan < count && mayProbe()) {
				const uint32_t k = g_scan++;
				const int32_t  cellX = centreX + g_order[2 * static_cast<size_t>(k)];
				const int32_t  cellY = centreY + g_order[2 * static_cast<size_t>(k) + 1];
				const uint32_t index = ObjectSnowRule::Index(cellX, cellY);
				if (g_tagX[index] == cellX && g_tagY[index] == cellY) {
					continue;
				}
				probe(cellX, cellY);
			}

			// Then a slow re-read of everything in reach (a door opened, a workshop build).
			while (g_scan >= count && count > 0 && rays < kRefreshRaysPerBatch && mayProbe()) {
				const uint32_t k = g_refresh++ % count;
				probe(centreX + g_order[2 * static_cast<size_t>(k)], centreY + g_order[2 * static_cast<size_t>(k) + 1]);
			}

			g_batchRays = rays;
			g_batchTicks = Profiler::Ticks() - started;
		}

		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob*  errors = nullptr;
			const auto hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "NMN snow on objects", nullptr,
				nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				logger::error("Snow on objects: {} compile failed: {}", a_target,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
			}
			Drop(errors);
			return SUCCEEDED(hr);
		}

		bool Initialize()
		{
			if (g_initialized || g_failed) {
				return g_initialized;
			}
			auto* device = globals::d3d::device;
			if (!device) {
				return false;
			}
			g_failed = true;

			const auto started = std::chrono::steady_clock::now();
			ID3DBlob*  vsCode = nullptr;
			ID3DBlob*  psCode = nullptr;
			if (!Compile("vsMain", "vs_5_0", &vsCode) || !Compile("psMain", "ps_5_0", &psCode)) {
				Drop(vsCode);
				Drop(psCode);
				return false;
			}
			const bool shaders =
				SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs)) &&
				SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &g_ps));
			Drop(vsCode);
			Drop(psCode);
			if (!shaders) {
				logger::error("Snow on objects: the pass shaders could not be created - off");
				return false;
			}

			D3D11_BUFFER_DESC params{};
			params.ByteWidth = sizeof(Params);
			params.Usage = D3D11_USAGE_DYNAMIC;
			params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			D3D11_TEXTURE2D_DESC tops{};
			tops.Width = ObjectSnowRule::kTexels;
			tops.Height = ObjectSnowRule::kTexels;
			tops.MipLevels = 1;
			tops.ArraySize = 1;
			// Each texel holds the square's top (x) and the land under it (y).
			tops.Format = DXGI_FORMAT_R32G32_FLOAT;
			tops.SampleDesc.Count = 1;
			tops.Usage = D3D11_USAGE_DEFAULT;
			tops.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			g_upload.assign(static_cast<size_t>(kTotal) * 2, ObjectSnowRule::kNoTop);
			D3D11_SUBRESOURCE_DATA initial{ g_upload.data(), ObjectSnowRule::kTexels * 2 * sizeof(float), 0 };

			// Albedo = albedo * (1 - a) + snow * a, colour only: the alpha the game keeps
			// there is left as it is.
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].RenderTargetWriteMask =
				D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

			D3D11_DEPTH_STENCIL_DESC noDepth{};
			noDepth.DepthEnable = FALSE;
			noDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			noDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;

			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;

			if (FAILED(device->CreateBuffer(&params, nullptr, &g_params)) ||
				FAILED(device->CreateTexture2D(&tops, &initial, &g_topsTexture)) ||
				FAILED(device->CreateShaderResourceView(g_topsTexture, nullptr, &g_topsView)) ||
				FAILED(device->CreateBlendState(&blend, &g_blend)) ||
				FAILED(device->CreateDepthStencilState(&noDepth, &g_noDepth)) ||
				FAILED(device->CreateRasterizerState(&raster, &g_raster))) {
				logger::error("Snow on objects: its resources could not be created - off");
				return false;
			}

			{
				const std::scoped_lock lock(g_lock);
				BuildOrder();
				ClearGrid();
			}

			g_failed = false;
			g_initialized = true;
			logger::info("Snow on objects: ready ({:.1f} ms to compile), tops probed in {} units squares",
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
				ObjectSnowRule::kTexelSize);
			return true;
		}

		void ReleaseCapture()
		{
			Drop(g_albedo);
		}

		// The SRV format that reads a depth texture's depth.
		DXGI_FORMAT DepthReadFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
				return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
				return DXGI_FORMAT_R16_UNORM;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		bool AlbedoFormat(DXGI_FORMAT a_format, bool& a_srgb)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				a_srgb = false;
				return true;
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				a_srgb = true;
				return true;
			default:
				return false;
			}
		}

		void Reject(const char* a_why)
		{
			if (!g_rejectedReported) {
				g_rejectedReported = true;
				logger::warn("Snow on objects: {} - the pass stays off for this target", a_why);
			}
		}

		// The pipeline the pass changes, captured before and put back after.
		struct SavedState
		{
			ID3D11RenderTargetView*   rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   dsv{ nullptr };
			ID3D11BlendState*         blend{ nullptr };
			FLOAT                     blendFactor[4]{};
			UINT                      sampleMask{ 0 };
			ID3D11DepthStencilState*  depth{ nullptr };
			UINT                      stencilRef{ 0 };
			ID3D11RasterizerState*    raster{ nullptr };
			D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			ID3D11InputLayout*        layout{ nullptr };
			D3D11_PRIMITIVE_TOPOLOGY  topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
			ShaderState::Vertex vs;
			ShaderState::Pixel ps;
			ShaderState::Geometry gs;
			ShaderState::Hull hs;
			ShaderState::Domain ds;
			ID3D11ShaderResourceView* psSRV[2]{};
			ID3D11Buffer*             psCB0{ nullptr };
			ID3D11Buffer*             psCB12{ nullptr };

			explicit SavedState(ID3D11DeviceContext* a_context) :
				_context(a_context)
			{
				_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				_context->OMGetBlendState(&blend, blendFactor, &sampleMask);
				_context->OMGetDepthStencilState(&depth, &stencilRef);
				_context->RSGetState(&raster);
				_context->RSGetViewports(&viewportCount, viewports);
				_context->IAGetInputLayout(&layout);
				_context->IAGetPrimitiveTopology(&topology);
				vs.Capture(_context);
				ps.Capture(_context);
				gs.Capture(_context);
				hs.Capture(_context);
				ds.Capture(_context);
				_context->PSGetShaderResources(0, 2, psSRV);
				_context->PSGetConstantBuffers(0, 1, &psCB0);
				_context->PSGetConstantBuffers(12, 1, &psCB12);
			}

			~SavedState()
			{
				_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				_context->OMSetBlendState(blend, blendFactor, sampleMask);
				_context->OMSetDepthStencilState(depth, stencilRef);
				_context->RSSetState(raster);
				if (viewportCount > 0) {
					_context->RSSetViewports(viewportCount, viewports);
				}
				_context->IASetInputLayout(layout);
				_context->IASetPrimitiveTopology(topology);
				vs.Restore(_context);
				ps.Restore(_context);
				gs.Restore(_context);
				hs.Restore(_context);
				ds.Restore(_context);
				_context->PSSetShaderResources(0, 2, psSRV);
				_context->PSSetConstantBuffers(0, 1, &psCB0);
				_context->PSSetConstantBuffers(12, 1, &psCB12);

				for (auto*& rtv : rtvs) {
					Drop(rtv);
				}
				Drop(dsv);
				Drop(blend);
				Drop(depth);
				Drop(raster);
				Drop(layout);
				vs.Release();
				ps.Release();
				gs.Release();
				hs.Release();
				ds.Release();
				for (auto*& view : psSRV) {
					Drop(view);
				}
				Drop(psCB0);
				Drop(psCB12);
			}

			SavedState(const SavedState&) = delete;
			SavedState& operator=(const SavedState&) = delete;

		private:
			ID3D11DeviceContext* _context;
		};

		uint32_t g_reportFrames{ 0 };
	}

	void Update()
	{
		if (!Wanted() || !globals::Ready()) {
			return;
		}
		if (!Initialize()) {
			return;
		}

		// Queue the next probe batch for the game thread (one at a time); none without snow.
		if (SnowCoverage::AnySnow() && !g_probeQueued.exchange(true, std::memory_order_acq_rel)) {
			if (const auto* tasks = F4SE::GetTaskInterface()) {
				// Settings reloads belong to the render thread. Give the game task a
				// snapshot and cancel stale queued work when the grid is reset.
				const float distance = Settings::snowOnObjectsDistance;
				const uint64_t epoch = g_probeEpoch.load(std::memory_order_acquire);
				tasks->AddTask([distance, epoch] { ProbeBatch(distance, epoch); });
			} else {
				g_probeQueued.store(false, std::memory_order_release);
			}
		}

		auto* player = globals::game::player;
		if (!player) {
			return;
		}

		// The grid the pass reads: a probed top that stands above the snow where the ground
		// around it is snow, nothing elsewhere (and nothing for a wrapped texel that still holds
		// another cell); the land under every read square, for the snow surface.
		++g_framesSinceUpload;
		bool changed = false;
		uint32_t rays = 0;
		int64_t  ticks = 0;
		{
			const std::scoped_lock lock(g_lock);
			changed = g_changed;
			rays = std::exchange(g_batchRays, 0u);
			ticks = std::exchange(g_batchTicks, 0);
		}
		Profiler::Tally(Profiler::Count::kObjectSnowRays, rays);
		Profiler::AddCpuTicks(Profiler::CpuScope::kObjectSnow, ticks);

		if (!changed && g_framesSinceUpload < kUploadEveryFrames) {
			return;
		}
		if (changed && g_framesSinceUpload < 4) {
			return;
		}

		const int64_t      started = Profiler::Ticks();
		const RE::NiPoint3 position = player->GetPosition();
		const int32_t      centreX = ObjectSnowRule::CellOf(position.x);
		const int32_t      centreY = ObjectSnowRule::CellOf(position.y);
		const int32_t      half = static_cast<int32_t>(ObjectSnowRule::kTexels / 2);
		const float        snowDepth = Weather::SnowDepth();
		uint32_t           tops = 0;
		uint32_t           roofHigh = 0;
		uint32_t           underSnow = 0;
		{
			const std::scoped_lock lock(g_lock);
			if (g_top.empty()) {
				return;
			}
			g_changed = false;
			for (uint32_t index = 0; index < kTotal; ++index) {
				const int32_t cellX = g_tagX[index];
				const int32_t cellY = g_tagY[index];
				float         top = ObjectSnowRule::kNoTop;
				float         land = ObjectSnowRule::kNoTop;
				if (cellX != INT32_MIN && std::abs(cellX - centreX) < half && std::abs(cellY - centreY) < half) {
					land = g_land[index];
					if (g_top[index] > ObjectSnowRule::kNoTop * 0.5f) {
						const float x = (static_cast<float>(cellX) + 0.5f) * ObjectSnowRule::kTexelSize;
						const float y = (static_cast<float>(cellY) + 0.5f) * ObjectSnowRule::kTexelSize;
						if (SnowCoverage::GroundAt(x, y) >= kSnowGround) {
							if (ObjectSnowRule::AboveSnow(g_top[index], land, snowDepth)) {
								top = g_top[index];
								++tops;
								if (top - land >= snowDepth + kRoofHigh) {
									++roofHigh;
								}
							} else {
								++underSnow;
							}
						}
					}
				}
				g_upload[2 * static_cast<size_t>(index)] = top;
				g_upload[2 * static_cast<size_t>(index) + 1] = land;
			}
		}

		globals::d3d::context->UpdateSubresource(g_topsTexture, 0, nullptr, g_upload.data(),
			ObjectSnowRule::kTexels * 2 * sizeof(float), 0);
		g_uploadCentreX = position.x;
		g_uploadCentreY = position.y;
		g_uploadedTops = tops;
		g_framesSinceUpload = 0;
		Profiler::AddCpuTicks(Profiler::CpuScope::kObjectSnow, Profiler::Ticks() - started);

		if (Settings::logSnowCoverage && ++g_reportFrames % 10 == 1) {
			logger::info("Snow on objects: {} snowy tops above the snow ({} roof-high, {:.0f}+ over it), {} under or "
						 "flush with it, in reach of ({:.0f}, {:.0f}), {} rays last batch",
				tops, roofHigh, kRoofHigh, underSnow, position.x, position.y, rays);
		}
	}

	void NoteGBuffer(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !g_initialized || !Wanted()) {
			return;
		}
		const uint64_t frame = globals::FrameIndex();
		if (g_capturedFrame.load(std::memory_order_acquire) == frame) {
			return;
		}
		// Routed landscape draws may arrive from several deferred contexts. Only one
		// may replace the shared capture and release the previous COM references.
		const std::scoped_lock lock(g_captureLock);
		if (g_capturedFrame.load(std::memory_order_relaxed) == frame) {
			return;
		}
		struct PublishCapture
		{
			uint64_t frame;
			~PublishCapture() { g_capturedFrame.store(frame, std::memory_order_release); }
		} publish{ frame };
		ReleaseCapture();

		ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* depth = nullptr;
		a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
		UINT count = 1;
		D3D11_VIEWPORT viewport{};
		a_context->RSGetViewports(&count, &viewport);

		// Slot 0 is the albedo (DrawWorld::MainRenderSetup binds targets 0x1A albedo, 0x1B
		// normal, 0x1D envmap, 0x1E ambient, 0x1F emit, 0x20 motion vectors on OG, NG and
		// 1.11.240 alike; docs/FO4_PORT_NOTES.md).
		ID3D11RenderTargetView* albedo = std::exchange(targets[0], nullptr);
		for (auto*& target : targets) {
			Drop(target);
		}

		const auto reject = [&](const char* a_why) {
			Reject(a_why);
			Drop(albedo);
			Drop(depth);
		};
		if (!albedo || !depth || count == 0 || !(viewport.Width > 0.0f)) {
			reject("no albedo target, depth or viewport is bound in the G-buffer pass");
			return;
		}

		D3D11_RENDER_TARGET_VIEW_DESC albedoDesc{};
		albedo->GetDesc(&albedoDesc);
		bool srgb = false;
		if (albedoDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || !AlbedoFormat(albedoDesc.Format, srgb)) {
			reject("the first G-buffer target is not an 8-bit colour target");
			return;
		}

		ID3D11Resource* depthResource = nullptr;
		depth->GetResource(&depthResource);
		ID3D11Texture2D* depthTexture = nullptr;
		if (depthResource) {
			depthResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTexture));
		}
		Drop(depthResource);
		Drop(depth);
		if (!depthTexture) {
			reject("the depth target is not a 2D texture");
			Drop(albedo);
			return;
		}

		if (depthTexture != g_depthTexture) {
			Drop(g_depthView);
			Drop(g_depthTexture);
			D3D11_TEXTURE2D_DESC desc{};
			depthTexture->GetDesc(&desc);
			const DXGI_FORMAT read = DepthReadFormat(desc.Format);
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = read;
			view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipLevels = 1;
			if (desc.SampleDesc.Count != 1 || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
				read == DXGI_FORMAT_UNKNOWN ||
				FAILED(globals::d3d::device->CreateShaderResourceView(depthTexture, &view, &g_depthView))) {
				Drop(depthTexture);
				Drop(albedo);
				Reject("the depth target cannot be read in a shader");
				return;
			}
			g_depthTexture = depthTexture;
			if (!g_readyReported) {
				g_readyReported = true;
				logger::info("Snow on objects: G-buffer albedo format {} ({}), depth format {}, {:.0f}x{:.0f} viewport",
					static_cast<uint32_t>(albedoDesc.Format), srgb ? "sRGB" : "linear", static_cast<uint32_t>(desc.Format),
					viewport.Width, viewport.Height);
			}
		} else {
			Drop(depthTexture);
		}

		g_albedo = albedo;
		g_albedoSRGB = srgb;
		g_viewport = viewport;
	}

	void Render()
	{
		if (!g_initialized || !Wanted() || !Settings::snowOnObjectsPass ||
			g_capturedFrame.load(std::memory_order_acquire) != globals::FrameIndex() ||
			!g_albedo || !g_depthView ||
			(g_uploadedTops == 0 && Settings::snowOnObjectsDebug <= 0) ||
			!globals::PlayerInExterior() || !SnowCoverage::AnySnow()) {
			return;
		}
		ID3D11Buffer* frame = FrameCamera::Constants();
		auto*         context = globals::d3d::context;
		if (!frame || !context) {
			return;
		}

		Params params{};
		params.viewport[0] = g_viewport.TopLeftX;
		params.viewport[1] = g_viewport.TopLeftY;
		params.viewport[2] = g_viewport.Width;
		params.viewport[3] = g_viewport.Height;
		params.grid[0] = ObjectSnowRule::kTexelSize;
		params.grid[1] = static_cast<float>(ObjectSnowRule::kTexels);
		params.grid[2] = g_uploadCentreX;
		params.grid[3] = g_uploadCentreY;
		const float distance = std::clamp(Settings::snowOnObjectsDistance, ObjectSnowRule::kTexelSize,
			ObjectSnowRule::kMaxDistance);
		params.reach[0] = distance;
		params.reach[1] = std::max(distance * 0.15f, ObjectSnowRule::kTexelSize);
		params.reach[2] = kNearestPixel;
		params.reach[3] = Settings::snowOnObjectsStrength;
		params.look[0] = Settings::snowOnObjectsMinUp;
		params.look[1] = ObjectSnowRule::kFadeUp;
		// Fresh snow as an albedo: about 0.84 in display (sRGB) terms.
		params.look[2] = g_albedoSRGB ? 0.68f : 0.84f;
		params.look[3] = ObjectSnowRule::kTopTolerance;
		params.snow[0] = Weather::SnowDepth();
		params.snow[1] = Settings::snowOnObjectsDebug > 0 ? 1.0f : 0.0f;
		params.snow[2] = ObjectSnowRule::kSurfaceBand;
		if (auto* player = globals::game::player) {
			params.snow[3] = static_cast<float>(GatherActors(params.capsules, player->GetPosition(), distance));
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, &params, sizeof(params));
		context->Unmap(g_params, 0);

		const SavedState saved(context);

		Profiler::GpuBegin(Profiler::Scope::kObjectSnow);

		// The depth is read, so it cannot stay bound for writing.
		context->OMSetRenderTargets(1, &g_albedo, nullptr);
		const FLOAT factor[4]{};
		context->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
		context->OMSetDepthStencilState(g_noDepth, 0);
		context->RSSetState(g_raster);
		context->RSSetViewports(1, &g_viewport);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_vs, nullptr, 0);
		context->PSSetShader(g_ps, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		ID3D11ShaderResourceView* views[2]{ g_depthView, g_topsView };
		context->PSSetShaderResources(0, 2, views);
		context->PSSetConstantBuffers(0, 1, &g_params);
		context->PSSetConstantBuffers(12, 1, &frame);

		context->Draw(3, 0);

		// Nothing of ours stays bound for the passes that follow.
		ID3D11ShaderResourceView* none[2]{};
		context->PSSetShaderResources(0, 2, none);

		Profiler::GpuEnd();
		Profiler::Tally(Profiler::Count::kObjectSnowPasses);
	}

	bool SnowyTopAt(float a_x, float a_y, float a_z)
	{
		if (!Wanted() || !globals::PlayerInExterior() || SnowCoverage::GroundAt(a_x, a_y) < kSnowGround) {
			return false;
		}
		const float            snowDepth = Weather::SnowDepth();
		const int32_t          cellX = ObjectSnowRule::CellOf(a_x);
		const int32_t          cellY = ObjectSnowRule::CellOf(a_y);
		const std::scoped_lock lock(g_lock);
		if (g_top.empty()) {
			return false;
		}
		// The square under the point and the eight around it: a stair's tread spans several,
		// and a foot stands near a square's edge as often as not.
		for (int32_t dy = -1; dy <= 1; ++dy) {
			for (int32_t dx = -1; dx <= 1; ++dx) {
				const uint32_t index = ObjectSnowRule::Index(cellX + dx, cellY + dy);
				if (g_tagX[index] == cellX + dx && g_tagY[index] == cellY + dy &&
					ObjectSnowRule::AboveSnow(g_top[index], g_land[index], snowDepth) &&
					std::fabs(a_z - g_top[index]) <= ObjectSnowRule::kTopTolerance) {
					return true;
				}
			}
		}
		return false;
	}

	void Reset()
	{
		g_probeEpoch.fetch_add(1, std::memory_order_acq_rel);
		g_rejectedReported = false;
		g_reportFrames = 0;
		g_framesSinceUpload = kUploadEveryFrames;
		if (!g_initialized) {
			return;
		}
		const std::scoped_lock lock(g_lock);
		ClearGrid();
		g_centreX = INT32_MIN;
		g_centreY = INT32_MIN;
	}

	void Shutdown()
	{
		ReleaseCapture();
		Drop(g_depthView);
		Drop(g_depthTexture);
		Drop(g_vs);
		Drop(g_ps);
		Drop(g_params);
		Drop(g_topsView);
		Drop(g_topsTexture);
		Drop(g_blend);
		Drop(g_noDepth);
		Drop(g_raster);
		g_initialized = false;
		g_failed = false;
	}
}
