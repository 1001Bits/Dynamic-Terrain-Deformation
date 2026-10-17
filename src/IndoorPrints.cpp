// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "IndoorPrints.h"

#include "Clipmap.h"
#include "DdsLoader.h"
#include "FrameCamera.h"
#include "Globals.h"
#include "HiddenMarks.h"
#include "IndoorPrintRule.h"
#include "IndoorPrintShader.h"
#include "ObjectSnow.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShaderState.h"
#include "SnowCoverage.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace IndoorPrints
{
	namespace
	{
		using IndoorPrintRule::Dirt;

		// Prints drawn in one pass at most (the nearest first).
		constexpr uint32_t kMaxDrawn = 256;
		constexpr size_t kMaxBoots = 1024;

		// Pixels nearer the eye than this are the first-person weapon and hands.
		constexpr float kNearestPixel = 48.0f;

		// A face takes a print from this much "up" in its normal (a floor, a gentle ramp).
		constexpr float kMinUp = 0.6f;

		// Added to a print's radius in the view tests: the eye moves a little in the frames the
		// camera takes to come back from the GPU.
		constexpr float kViewMargin = 16.0f;

		struct Print
		{
			IndoorPrintRule::Box box{};
			Dirt                 dirt{ Dirt::kNone };
			float                strength{ 0.0f };
			double               born{ 0.0 };
			uint32_t             space{ 0 };
			// The player's own first-person print, not shown until its spot left the view.
			bool  hidden{ false };
			// Made outside in a snowy place: pressed snow rather than slush.
			bool  packedSnow{ false };
			float waited{ 0.0f };
		};

		struct alignas(16) GpuPrint
		{
			float centre[4];
			float axes[4];
			float colour[4];
			float misc[4];
		};
		static_assert(sizeof(GpuPrint) == 64);

		struct alignas(16) FrameParams
		{
			float viewport[4];
			float look[4];
		};
		static_assert(sizeof(FrameParams) % 16 == 0);

		struct BootTrack
		{
			IndoorPrintRule::Boots boots;
			double lastSeen{ 0.0 };
		};

		IndoorPrintRule::Ring<Print>                         g_prints;
		std::unordered_map<RE::TESFormID, BootTrack>          g_boots;
		double                                               g_clock{ 0.0 };
		uint32_t                                             g_space{ 0 };

		// ── the GPU side ─────────────────────────────────────────────────────────────────
		ID3D11VertexShader*       g_vs{ nullptr };
		ID3D11PixelShader*        g_ps{ nullptr };
		ID3D11Buffer*             g_params{ nullptr };
		ID3D11Buffer*             g_instances{ nullptr };
		ID3D11ShaderResourceView* g_instanceView{ nullptr };
		ID3D11ShaderResourceView* g_mask{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		ID3D11BlendState*         g_blend{ nullptr };
		ID3D11DepthStencilState*  g_noDepth{ nullptr };
		ID3D11RasterizerState*    g_raster{ nullptr };
		bool                      g_initialized{ false };
		bool                      g_failed{ false };

		// What the G-buffer pass bound this frame.
		ID3D11RenderTargetView*   g_albedo{ nullptr };
		ID3D11Texture2D*          g_depthTexture{ nullptr };
		ID3D11ShaderResourceView* g_depthView{ nullptr };
		D3D11_VIEWPORT            g_viewport{};
		std::atomic<uint64_t>     g_capturedFrame{ ~0ull };
		std::mutex                g_captureLock;
		bool                      g_albedoSRGB{ false };

		// Log lines (LogStampFeet), each kind capped and printed again after a reload.
		std::atomic<uint32_t> g_bootsLogged{ 0 };
		std::atomic<uint32_t> g_printsLogged{ 0 };
		bool                  g_rejectedReported{ false };
		bool                  g_readyReported{ false };
		constexpr uint32_t    kMaxBootsLogged = 16;
		constexpr uint32_t    kMaxPrintsLogged = 24;

		bool TakeLogLine(std::atomic<uint32_t>& a_counter, uint32_t a_limit)
		{
			return Settings::logStampFeet && a_counter.load() < a_limit && a_counter.fetch_add(1) < a_limit;
		}

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		const char* Name(Dirt a_dirt)
		{
			switch (a_dirt) {
			case Dirt::kMud:
				return "muddy";
			case Dirt::kWet:
				return "wet";
			case Dirt::kDust:
				return "dusty";
			default:
				return "clean";
			}
		}

		// What a kind of ground leaves on the boots, and how much.
		void DirtOf(Surfaces::Type a_surface, Dirt& a_dirt, float& a_strength)
		{
			switch (a_surface) {
			case Surfaces::Type::kMud:
				a_dirt = Dirt::kMud;
				a_strength = 1.0f;
				return;
			case Surfaces::Type::kSnow:
				a_dirt = Dirt::kWet;
				a_strength = 1.0f;
				return;
			case Surfaces::Type::kSand:
			case Surfaces::Type::kAsh:
				a_dirt = Dirt::kDust;
				a_strength = 0.8f;
				return;
			case Surfaces::Type::kDirt:
				a_dirt = Dirt::kDust;
				a_strength = 0.7f;
				return;
			case Surfaces::Type::kGravel:
				a_dirt = Dirt::kDust;
				a_strength = 0.3f;
				return;
			case Surfaces::Type::kGrass:
				a_dirt = Dirt::kDust;
				a_strength = 0.25f;
				return;
			default:
				a_dirt = Dirt::kNone;
				a_strength = 0.0f;
				return;
			}
		}

		// The space prints belong to: an interior cell, or the exterior worldspace. Positions
		// in two interiors overlap, so a print is only ever drawn in its own.
		uint32_t CurrentSpace()
		{
			auto* cell = globals::PlayerCell();
			if (!cell) {
				return 0;
			}
			if (cell->IsExterior()) {
				auto* world = globals::PlayerWorldspace();
				return world ? world->GetFormID() : 0;
			}
			return cell->GetFormID();
		}

		// ── GPU resources ────────────────────────────────────────────────────────────────
		bool Compile(const char* a_entry, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob*  errors = nullptr;
			const auto hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "NMN floor prints", nullptr,
				nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				logger::error("Floor prints: {} compile failed: {}", a_target,
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

			ID3DBlob* vsCode = nullptr;
			ID3DBlob* psCode = nullptr;
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
				logger::error("Floor prints: the pass shaders could not be created - off");
				return false;
			}

			D3D11_BUFFER_DESC params{};
			params.ByteWidth = sizeof(FrameParams);
			params.Usage = D3D11_USAGE_DYNAMIC;
			params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			D3D11_BUFFER_DESC instances{};
			instances.ByteWidth = sizeof(GpuPrint) * kMaxDrawn;
			instances.Usage = D3D11_USAGE_DYNAMIC;
			instances.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			instances.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			instances.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			instances.StructureByteStride = sizeof(GpuPrint);

			D3D11_SHADER_RESOURCE_VIEW_DESC instanceView{};
			instanceView.Format = DXGI_FORMAT_UNKNOWN;
			instanceView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			instanceView.Buffer.FirstElement = 0;
			instanceView.Buffer.NumElements = kMaxDrawn;

			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;

			// Albedo = albedo * (1 - a) + tint * a, colour only: the alpha the game keeps there
			// is left as it is.
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

			// One side of each box only, so every pixel it covers is blended once.
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_FRONT;
			raster.DepthClipEnable = TRUE;

			if (FAILED(device->CreateBuffer(&params, nullptr, &g_params)) ||
				FAILED(device->CreateBuffer(&instances, nullptr, &g_instances)) ||
				FAILED(device->CreateShaderResourceView(g_instances, &instanceView, &g_instanceView)) ||
				FAILED(device->CreateSamplerState(&sampler, &g_sampler)) ||
				FAILED(device->CreateBlendState(&blend, &g_blend)) ||
				FAILED(device->CreateDepthStencilState(&noDepth, &g_noDepth)) ||
				FAILED(device->CreateRasterizerState(&raster, &g_raster))) {
				logger::error("Floor prints: its resources could not be created - off");
				return false;
			}

			// The terrain prints' own sole; without it the pass draws a sole of its own.
			if (FAILED(Dds::LoadTexture(device, L"Data/Textures/NMN_DeformableTerrain/FootprintHeight.dds", &g_mask))) {
				g_mask = nullptr;
				logger::info("Floor prints: no sole mask (FootprintHeight.dds) - a drawn sole is used");
			}

			g_failed = false;
			g_initialized = true;
			logger::info("Floor prints: ready (up to {} drawn a frame)", kMaxDrawn);
			return true;
		}

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
				logger::warn("Floor prints: {} - the pass stays off for this target", a_why);
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
			ID3D11ShaderResourceView* vsSRV[2]{};
			ID3D11ShaderResourceView* psSRV[3]{};
			ID3D11Buffer*             vsCB0{ nullptr };
			ID3D11Buffer*             psCB0{ nullptr };
			ID3D11Buffer*             vsFrame{ nullptr };
			ID3D11Buffer*             psFrame{ nullptr };
			ID3D11SamplerState*       psSampler{ nullptr };

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
				_context->VSGetShaderResources(0, 2, vsSRV);
				_context->PSGetShaderResources(0, 3, psSRV);
				_context->VSGetConstantBuffers(0, 1, &vsCB0);
				_context->PSGetConstantBuffers(0, 1, &psCB0);
				_context->VSGetConstantBuffers(FrameCamera::kSlot, 1, &vsFrame);
				_context->PSGetConstantBuffers(FrameCamera::kSlot, 1, &psFrame);
				_context->PSGetSamplers(0, 1, &psSampler);
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
				_context->VSSetShaderResources(0, 2, vsSRV);
				_context->PSSetShaderResources(0, 3, psSRV);
				_context->VSSetConstantBuffers(0, 1, &vsCB0);
				_context->PSSetConstantBuffers(0, 1, &psCB0);
				_context->VSSetConstantBuffers(FrameCamera::kSlot, 1, &vsFrame);
				_context->PSSetConstantBuffers(FrameCamera::kSlot, 1, &psFrame);
				_context->PSSetSamplers(0, 1, &psSampler);

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
				for (auto*& view : vsSRV) {
					Drop(view);
				}
				for (auto*& view : psSRV) {
					Drop(view);
				}
				Drop(vsCB0);
				Drop(psCB0);
				Drop(vsFrame);
				Drop(psFrame);
				Drop(psSampler);
			}

			SavedState(const SavedState&) = delete;
			SavedState& operator=(const SavedState&) = delete;

		private:
			ID3D11DeviceContext* _context;
		};

		float ToLinear(float a_srgb)
		{
			return a_srgb <= 0.04045f ? a_srgb / 12.92f : std::pow((a_srgb + 0.055f) / 1.055f, 2.4f);
		}
	}

	bool Wanted()
	{
		return Settings::indoorPrints && !REL::Module::IsVR();
	}

	void Update(float a_deltaSeconds)
	{
		if (!Wanted()) {
			return;
		}
		g_clock += std::max(a_deltaSeconds, 0.0f);
		g_space = CurrentSpace();

		// The player's own first-person prints wait until their spot has left the view, the
		// player has walked on far enough or they waited long enough (the hidden-marks rule).
		auto* player = globals::game::player;
		if (!player || g_prints.items.empty()) {
			return;
		}
		const RE::NiPoint3 at = player->GetPosition();
		bool               anyHidden = false;
		for (const auto& print : g_prints.items) {
			anyHidden = anyHidden || print.hidden;
		}
		if (!anyHidden) {
			return;
		}
		const auto view = Clipmap::PlayerView(at);
		for (auto& print : g_prints.items) {
			if (!print.hidden) {
				continue;
			}
			if (print.space != g_space) {
				print.hidden = false;
				continue;
			}
			print.waited += std::max(a_deltaSeconds, 0.0f);
			const float point[3]{ print.box.x, print.box.y, print.box.z };
			const float dx = print.box.x - at.x;
			const float dy = print.box.y - at.y;
			const auto  release = HiddenMarks::Decide(view, point, point, print.box.halfLength + kViewMargin,
				std::sqrt(dx * dx + dy * dy), Settings::stampSyntheticRevealDistance, print.waited,
				Settings::stampSyntheticRevealSeconds);
			if (release != HiddenMarks::Release::kKeep) {
				print.hidden = false;
				// Its life starts once it can be seen.
				print.born = g_clock;
			}
		}
	}

	void NoteGroundWalk(RE::TESFormID a_actor, Surfaces::Type a_surface, float a_travel)
	{
		if (!Wanted()) {
			return;
		}
		Dirt  dirt = Dirt::kNone;
		float strength = 0.0f;
		DirtOf(a_surface, dirt, strength);
		auto found = g_boots.find(a_actor);
		if (found == g_boots.end()) {
			if (dirt == Dirt::kNone || strength <= 0.0f) {
				return;
			}
			// A long session visits many actors. Retain the most recently used boots,
			// protecting the player so an exterior-to-interior trip keeps their dirt.
			if (g_boots.size() >= kMaxBoots) {
				const auto* player = globals::game::player;
				const auto playerID = player ? player->GetFormID() : 0;
				auto oldest = g_boots.end();
				for (auto it = g_boots.begin(); it != g_boots.end(); ++it) {
					if (it->first != playerID && (oldest == g_boots.end() || it->second.lastSeen < oldest->second.lastSeen)) {
						oldest = it;
					}
				}
				if (oldest != g_boots.end()) {
					g_boots.erase(oldest);
				}
			}
			found = g_boots.try_emplace(a_actor).first;
		}
		found->second.lastSeen = g_clock;
		auto&      boots = found->second.boots;
		const auto before = boots.dirt;
		IndoorPrintRule::Walk(boots, dirt, strength, a_travel);
		if (boots.dirt != before && TakeLogLine(g_bootsLogged, kMaxBootsLogged)) {
			logger::info("Floor prints: {:08X} boots now {} ({:.2f}) after walking on {}", a_actor, Name(boots.dirt),
				boots.level, Surfaces::Name(a_surface));
		}
	}

	void NoteFloorStep(RE::TESFormID a_actor, const FloorStep& a_step)
	{
		if (!Wanted()) {
			return;
		}
		// A road, a roof or a rock top out in the snow lies under the snow too (snow on
		// objects): every step on it packs the snow down, however far the boots have come.
		// Stairs and a raised entrance shelter the ground under them, so the snow map has none
		// there: their whitened tops are asked of the snow-on-objects grid.
		const bool snowyTop = globals::PlayerInExterior() &&
		                      (SnowCoverage::At(a_step.x, a_step.y) > 0.5f ||
		                          ObjectSnow::SnowyTopAt(a_step.x, a_step.y, a_step.z));
		if (snowyTop) {
			NoteGroundWalk(a_actor, Surfaces::Type::kSnow, 0.0f);
		}
		const auto found = g_boots.find(a_actor);
		if (found == g_boots.end()) {
			return;
		}
		found->second.lastSeen = g_clock;
		auto&       boots = found->second.boots;
		const Dirt  dirt = boots.dirt;
		const float strength = IndoorPrintRule::TakeStep(boots, Settings::indoorPrintSteps);
		if (strength <= 0.0f) {
			return;
		}

		Print print{};
		const float scale = std::clamp(a_step.sizeScale, 0.25f, 4.0f);
		print.box = { a_step.x, a_step.y, a_step.z, a_step.forwardX, a_step.forwardY,
			IndoorPrintRule::kHalfLength * scale, IndoorPrintRule::kHalfWidth * scale, a_step.mirror < 0.0f ? -1.0f : 1.0f };
		print.dirt = dirt;
		print.strength = strength;
		print.born = g_clock;
		// Feet are gathered before Update updates g_space, including the first frame
		// after crossing a door. Attribute the step to the cell it was actually taken in.
		print.space = CurrentSpace();
		print.hidden = a_step.hidden;
		print.packedSnow = dirt == Dirt::kWet && snowyTop;
		g_prints.Push(print, static_cast<size_t>(Settings::maxIndoorPrints));
		Profiler::Tally(Profiler::Count::kIndoorPrintsRecorded);

		if (TakeLogLine(g_printsLogged, kMaxPrintsLogged)) {
			logger::info("Floor prints: {:08X} {} {} print at ({:.0f}, {:.0f}, {:.0f}), strength {:.2f}{}", a_actor,
				Name(dirt), a_step.mirror < 0.0f ? "left" : "right", a_step.x, a_step.y, a_step.z, strength,
				print.hidden ? ", shown once out of view" : "");
		}
	}

	void NoteGBuffer(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !Wanted() || g_prints.items.empty()) {
			return;
		}
		const uint64_t frame = globals::FrameIndex();
		if (g_capturedFrame.load(std::memory_order_acquire) == frame) {
			return;
		}
		const std::scoped_lock lock(g_captureLock);
		if (g_capturedFrame.load(std::memory_order_relaxed) == frame) {
			return;
		}
		Drop(g_albedo);
		if (!Initialize()) {
			return;
		}

		ID3D11RenderTargetView* albedo = nullptr;
		ID3D11DepthStencilView* depth = nullptr;
		a_context->OMGetRenderTargets(1, &albedo, &depth);
		UINT           count = 1;
		D3D11_VIEWPORT viewport{};
		a_context->RSGetViewports(&count, &viewport);

		// Slot 0 is the albedo (DrawWorld::MainRenderSetup; docs/FO4_PORT_NOTES.md).
		if (!albedo || !depth || count == 0 || !(viewport.Width > 0.0f)) {
			// Earlier indexed draws can belong to the depth-only prepass. They must
			// not consume this frame's capture before the actual G-buffer is bound.
			Drop(albedo);
			Drop(depth);
			return;
		}

		D3D11_RENDER_TARGET_VIEW_DESC albedoDesc{};
		albedo->GetDesc(&albedoDesc);
		bool srgb = false;
		if (albedoDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || !AlbedoFormat(albedoDesc.Format, srgb)) {
			Reject("the first G-buffer target is not an 8-bit colour target");
			Drop(albedo);
			Drop(depth);
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
			Reject("the depth target is not a 2D texture");
			Drop(albedo);
			return;
		}

		if (depthTexture != g_depthTexture) {
			Drop(g_depthView);
			Drop(g_depthTexture);
			D3D11_TEXTURE2D_DESC desc{};
			depthTexture->GetDesc(&desc);
			const DXGI_FORMAT               read = DepthReadFormat(desc.Format);
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
				logger::info("Floor prints: G-buffer albedo format {} ({}), depth format {}, {:.0f}x{:.0f} viewport",
					static_cast<uint32_t>(albedoDesc.Format), srgb ? "sRGB" : "linear", static_cast<uint32_t>(desc.Format),
					viewport.Width, viewport.Height);
			}
		} else {
			Drop(depthTexture);
		}

		g_albedo = albedo;
		g_albedoSRGB = srgb;
		g_viewport = viewport;
		g_capturedFrame.store(frame, std::memory_order_release);
	}

	void Render()
	{
		if (g_capturedFrame.load(std::memory_order_acquire) != globals::FrameIndex() ||
			!g_initialized || !Wanted() || !g_albedo || !g_depthView ||
			g_prints.items.empty()) {
			return;
		}
		auto* context = globals::d3d::context;
		auto* player = globals::game::player;
		// The world camera this G-buffer was drawn with.
		ID3D11Buffer* frame = FrameCamera::Constants();
		if (!context || !player || !frame) {
			return;
		}

		// The prints of this space still showing, in reach and in view; the nearest first when
		// there are more than a pass draws.
		const RE::NiPoint3 at = player->GetPosition();
		const auto         view = Clipmap::PlayerView(at);
		const float        reach = Settings::indoorPrintDistance;
		struct Candidate
		{
			const Print* print;
			float        distanceSq;
			float        fade;
		};
		static std::vector<Candidate> candidates;
		candidates.clear();
		for (const auto& print : g_prints.items) {
			if (print.space != g_space || print.hidden || print.strength <= 0.0f) {
				continue;
			}
			const float fade = IndoorPrintRule::Fade(static_cast<float>(g_clock - print.born),
				Settings::indoorPrintFadeSeconds, print.dirt);
			if (fade <= 0.0f) {
				continue;
			}
			const float dx = print.box.x - at.x;
			const float dy = print.box.y - at.y;
			const float dz = print.box.z - at.z;
			const float distanceSq = dx * dx + dy * dy + dz * dz;
			if (distanceSq > reach * reach) {
				continue;
			}
			const float point[3]{ print.box.x, print.box.y, print.box.z };
			if (view.valid && !HiddenMarks::InView(view, point, print.box.halfLength + kViewMargin)) {
				continue;
			}
			candidates.push_back({ &print, distanceSq, fade });
		}
		if (candidates.empty()) {
			return;
		}
		if (candidates.size() > kMaxDrawn) {
			std::nth_element(candidates.begin(), candidates.begin() + kMaxDrawn, candidates.end(),
				[](const Candidate& a_lhs, const Candidate& a_rhs) { return a_lhs.distanceSq < a_rhs.distanceSq; });
			candidates.resize(kMaxDrawn);
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(g_instances, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		auto*       gpu = static_cast<GpuPrint*>(mapped.pData);
		const float opacity = std::clamp(Settings::indoorPrintOpacity, 0.0f, 1.0f);
		for (size_t i = 0; i < candidates.size(); ++i) {
			const auto& print = *candidates[i].print;
			const auto  tint = print.packedSnow ? IndoorPrintRule::kPackedSnow : IndoorPrintRule::TintFor(print.dirt);
			auto&       out = gpu[i];
			out.centre[0] = print.box.x;
			out.centre[1] = print.box.y;
			out.centre[2] = print.box.z;
			out.centre[3] = IndoorPrintRule::kHalfHeight;
			out.axes[0] = print.box.forwardX;
			out.axes[1] = print.box.forwardY;
			out.axes[2] = print.box.halfLength;
			out.axes[3] = print.box.halfWidth;
			// An sRGB target takes linear colour and converts on write; a plain 8-bit one holds
			// display values already.
			out.colour[0] = g_albedoSRGB ? ToLinear(tint.r) : tint.r;
			out.colour[1] = g_albedoSRGB ? ToLinear(tint.g) : tint.g;
			out.colour[2] = g_albedoSRGB ? ToLinear(tint.b) : tint.b;
			out.colour[3] = tint.alpha * print.strength * candidates[i].fade * opacity;
			out.misc[0] = print.box.mirror;
			out.misc[1] = out.misc[2] = out.misc[3] = 0.0f;
		}
		context->Unmap(g_instances, 0);

		FrameParams params{};
		params.viewport[0] = g_viewport.TopLeftX;
		params.viewport[1] = g_viewport.TopLeftY;
		params.viewport[2] = g_viewport.Width;
		params.viewport[3] = g_viewport.Height;
		params.look[0] = g_mask ? 1.0f : 0.0f;
		params.look[1] = kMinUp;
		params.look[2] = kNearestPixel;
		if (FAILED(context->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, &params, sizeof(params));
		context->Unmap(g_params, 0);

		const SavedState saved(context);

		Profiler::GpuBegin(Profiler::Scope::kIndoorPrints);

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
		ID3D11ShaderResourceView* vsViews[2]{ nullptr, g_instanceView };
		context->VSSetShaderResources(0, 2, vsViews);
		ID3D11ShaderResourceView* psViews[3]{ g_depthView, g_instanceView, g_mask };
		context->PSSetShaderResources(0, 3, psViews);
		context->VSSetConstantBuffers(0, 1, &g_params);
		context->PSSetConstantBuffers(0, 1, &g_params);
		context->VSSetConstantBuffers(FrameCamera::kSlot, 1, &frame);
		context->PSSetConstantBuffers(FrameCamera::kSlot, 1, &frame);
		context->PSSetSamplers(0, 1, &g_sampler);

		context->DrawInstanced(36, static_cast<UINT>(candidates.size()), 0, 0);

		// Nothing of ours stays bound for the passes that follow.
		ID3D11ShaderResourceView* none[3]{};
		context->PSSetShaderResources(0, 3, none);
		context->VSSetShaderResources(0, 2, none);

		Profiler::GpuEnd();
		Profiler::Tally(Profiler::Count::kIndoorPrintsDrawn, static_cast<uint32_t>(candidates.size()));
	}

	void ResetDiagnostics()
	{
		g_bootsLogged.store(0);
		g_printsLogged.store(0);
		g_rejectedReported = false;
	}

	void ResetWorld()
	{
		g_prints.Clear();
		g_boots.clear();
		g_clock = 0.0;
	}

	void Shutdown()
	{
		Drop(g_albedo);
		Drop(g_depthView);
		Drop(g_depthTexture);
		Drop(g_vs);
		Drop(g_ps);
		Drop(g_params);
		Drop(g_instanceView);
		Drop(g_instances);
		Drop(g_mask);
		Drop(g_sampler);
		Drop(g_blend);
		Drop(g_noDepth);
		Drop(g_raster);
		g_initialized = false;
		g_failed = false;
	}
}
