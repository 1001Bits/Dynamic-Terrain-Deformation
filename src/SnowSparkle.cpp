// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"
#include "SnowSparkle.h"

#include "FrameCamera.h"
#include "Globals.h"
#include "Settings.h"
#include "ShaderState.h"
#include "Tessellation.h"
#include "Weather.h"

#include <cmath>
#include <cstring>

namespace SnowSparkle
{
	namespace
	{

		constexpr uint32_t kMaxFlakes = 4096;

		constexpr size_t kMaxContacts = 8;

		struct Flake
		{
			float x{}, y{}, z{};
			float vx{}, vy{}, vz{};
			float age{}, life{ 1.0f };
			float size{ 1.0f };
			float seed{};

			float floorZ{};
		};

		struct Contact
		{
			float x{}, y{}, z{};

			float forwardX{}, forwardY{ 1.0f };
			float velX{}, velY{};
			float radius{ 16.0f };
			float speed{};
			float amount{ 1.0f };
		};

		std::vector<Flake> g_flakes;
		Contact            g_contacts[kMaxContacts]{};
		size_t             g_contactCount{ 0 };

		float g_budget{ 0.0f };

		bool g_hookInstalled{ false };
		bool g_reportedCount{ false };
		bool g_reportedFull{ false };
		bool g_reportedTarget{ false };

		uint32_t g_rng{ 0x9E3779B9u };

		float Random01()
		{
			g_rng ^= g_rng << 13;
			g_rng ^= g_rng >> 17;
			g_rng ^= g_rng << 5;
			return static_cast<float>(g_rng & 0xFFFFFFu) / static_cast<float>(0x1000000u);
		}

		float RandomSigned() { return Random01() * 2.0f - 1.0f; }

		ID3D11VertexShader*       g_vs{ nullptr };
		ID3D11PixelShader*        g_ps{ nullptr };
		ID3D11Buffer*             g_instanceBuffer{ nullptr };
		ID3D11ShaderResourceView* g_instanceSRV{ nullptr };
		ID3D11Buffer*             g_cb{ nullptr };
		ID3D11BlendState*         g_blend{ nullptr };
		ID3D11RasterizerState*    g_raster{ nullptr };
		ID3D11DepthStencilState*  g_depthLess{ nullptr };
		ID3D11DepthStencilState*  g_depthGreater{ nullptr };
		bool                      g_failed{ false };

		struct GpuFlake
		{
			float x{}, y{}, z{}, size{};
			float alpha{}, spin{}, sparkle{}, seed{};
		};
		static_assert(sizeof(GpuFlake) == 32);

		// The camera comes from the engine's own per-frame buffer (captured at the first
		// routed landscape draw), so the flakes use exactly the terrain's view.
		struct SparkleCB
		{
			float params[4]{};
		};

		const char* const kSparkleVS = R"(
cbuffer Sparkle : register(b0)
{
	float4 Params;
};

cbuffer PerFrame : register(b12)
{
	row_major float3x4 CameraView      : packoffset(c0);
	row_major float4x4 CameraViewProj  : packoffset(c8);
	float4             CameraPosAdjust : packoffset(c35);
};

struct Flake
{
	float3 pos;
	float  size;
	float  alpha;
	float  spin;
	float  sparkle;
	float  seed;
};

StructuredBuffer<Flake> Flakes : register(t0);

struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	nointerpolation float alpha   : TEXCOORD1;
	nointerpolation float spin    : TEXCOORD2;
	nointerpolation float sparkle : TEXCOORD3;
	nointerpolation float seed    : TEXCOORD5;

	nointerpolation float px : TEXCOORD4;
};

VSOut main(uint id : SV_VertexID)
{
	const uint index  = id / 6u;
	const uint corner = id % 6u;

	const uint2 lut[6] = { uint2(0,0), uint2(1,0), uint2(0,1),
	                       uint2(0,1), uint2(1,0), uint2(1,1) };
	const float2 c = float2(lut[corner]) * 2.0f - 1.0f;

	const Flake f = Flakes[index];

	VSOut o;
	o.uv = float2(lut[corner]);

	// World positions in Fallout 4's shaders are relative to CameraPosAdjust. The view
	// matrix's first two rows are the camera's right and up axes in the world, which
	// turn the flake to face the eye.
	const float3 rel   = f.pos - CameraPosAdjust.xyz;
	const float3 right = CameraView[0].xyz;
	const float3 up    = CameraView[1].xyz;

	const float3 world = rel + (right * c.x + up * c.y) * f.size;
	o.pos = mul(CameraViewProj, float4(world, 1.0f));

	const float4 centre = mul(CameraViewProj, float4(rel, 1.0f));
	const float  across = abs(mul(CameraViewProj, float4(right * f.size, 0.0f)).x);

	o.alpha   = f.alpha;
	o.spin    = f.spin;
	o.sparkle = f.sparkle;
	o.seed    = f.seed;
	o.px      = centre.w > 0.001f ? (across * 2.0f / centre.w) * Params.z * 0.5f : 0.0f;
	return o;
}
)";

		const char* const kSparklePS = R"(
cbuffer Sparkle : register(b0)
{
	float4 Params;
};

struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	nointerpolation float alpha   : TEXCOORD1;
	nointerpolation float spin    : TEXCOORD2;
	nointerpolation float sparkle : TEXCOORD3;
	nointerpolation float seed    : TEXCOORD5;
	nointerpolation float px      : TEXCOORD4;
};

float4 main(VSOut i) : SV_TARGET
{
	const float2 d = i.uv * 2.0f - 1.0f;
	const float  r = length(d);
	if (r > 1.0f) {
		discard;
	}

	const float show = saturate((i.px - 6.0f) / 14.0f) * saturate(Params.x);
	const float ang  = atan2(d.y, d.x) + i.spin;

	float wob = 0.45f * sin(3.0f * ang + i.seed);
	wob += 0.32f * sin(5.0f * ang + i.seed * 2.7f);
	wob += 0.23f * sin(8.0f * ang + i.seed * 5.3f);

	const float lobes = 1.0f - show * 0.42f * (1.0f - wob) * 0.5f;
	const float mask  = saturate((lobes - r) * 3.0f);

	const float core = saturate(1.0f - r * r);
	const float a    = saturate(i.alpha * mask * (0.25f + 0.75f * core));

	const float3 tint = float3(0.94f, 0.97f, 1.0f) * Params.y * i.sparkle;
	return float4(tint, a);
}
)";

		bool Compile(const char* a_source, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob*  errors = nullptr;
			const auto hr = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr,
				nullptr, "main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				logger::error("SnowSparkle: {} compile failed: {}", a_target,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				if (errors) {
					errors->Release();
				}
				return false;
			}
			if (errors) {
				errors->Release();
			}
			return true;
		}

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		struct GraphicsStateGuard
		{
			explicit GraphicsStateGuard(ID3D11DeviceContext* a_context) :
				_context(a_context)
			{
				_context->OMGetRenderTargets(8, _rtvs, &_dsv);
				_context->OMGetDepthStencilState(&_depthState, &_stencilRef);
				_context->OMGetBlendState(&_blendState, _blendFactor, &_sampleMask);
				_context->RSGetState(&_rasterState);
				_context->RSGetViewports(&_viewportCount, _viewports);
				_context->IAGetPrimitiveTopology(&_topology);
				_context->IAGetInputLayout(&_inputLayout);
				_prevVS.Capture(_context);
				_prevPS.Capture(_context);
				_prevGS.Capture(_context);
				_prevHS.Capture(_context);
				_prevDS.Capture(_context);
				_context->VSGetShaderResources(0, 1, &_vsSRV);
				_context->VSGetConstantBuffers(0, 1, &_vsCB);
				_context->VSGetConstantBuffers(Tessellation::kFrameSlot, 1, &_vsFrameCB);
				_context->PSGetConstantBuffers(0, 1, &_psCB);
			}

			~GraphicsStateGuard()
			{
				_context->OMSetRenderTargets(8, _rtvs, _dsv);
				_context->OMSetDepthStencilState(_depthState, _stencilRef);
				_context->OMSetBlendState(_blendState, _blendFactor, _sampleMask);
				_context->RSSetState(_rasterState);
				if (_viewportCount > 0) {
					_context->RSSetViewports(_viewportCount, _viewports);
				}
				_context->IASetPrimitiveTopology(_topology);
				_context->IASetInputLayout(_inputLayout);
				_prevVS.Restore(_context);
				_prevPS.Restore(_context);
				_prevGS.Restore(_context);
				_prevHS.Restore(_context);
				_prevDS.Restore(_context);
				_context->VSSetShaderResources(0, 1, &_vsSRV);
				_context->VSSetConstantBuffers(0, 1, &_vsCB);
				_context->VSSetConstantBuffers(Tessellation::kFrameSlot, 1, &_vsFrameCB);
				_context->PSSetConstantBuffers(0, 1, &_psCB);

				for (auto*& rtv : _rtvs) {
					Drop(rtv);
				}
				Drop(_dsv);
				Drop(_depthState);
				Drop(_blendState);
				Drop(_rasterState);
				Drop(_inputLayout);
				_prevVS.Release();
				_prevPS.Release();
				_prevGS.Release();
				_prevHS.Release();
				_prevDS.Release();
				Drop(_vsSRV);
				Drop(_vsCB);
				Drop(_vsFrameCB);
				Drop(_psCB);
			}

			GraphicsStateGuard(const GraphicsStateGuard&) = delete;
			GraphicsStateGuard& operator=(const GraphicsStateGuard&) = delete;

		private:
			ID3D11DeviceContext*      _context{ nullptr };
			ID3D11RenderTargetView*   _rtvs[8]{};
			ID3D11DepthStencilView*   _dsv{ nullptr };
			ID3D11DepthStencilState*  _depthState{ nullptr };
			ID3D11BlendState*         _blendState{ nullptr };
			ID3D11RasterizerState*    _rasterState{ nullptr };
			ID3D11InputLayout*        _inputLayout{ nullptr };
			ShaderState::Vertex _prevVS;
			ShaderState::Pixel _prevPS;
			ShaderState::Geometry _prevGS;
			ShaderState::Hull _prevHS;
			ShaderState::Domain _prevDS;
			ID3D11ShaderResourceView* _vsSRV{ nullptr };
			ID3D11Buffer*             _vsCB{ nullptr };
			ID3D11Buffer*             _vsFrameCB{ nullptr };
			ID3D11Buffer*             _psCB{ nullptr };
			UINT                      _stencilRef{ 0 };
			FLOAT                     _blendFactor[4]{};
			UINT                      _sampleMask{ 0 };
			UINT _viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_VIEWPORT
			_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			D3D11_PRIMITIVE_TOPOLOGY _topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
		};

		void DropObjects()
		{
			Drop(g_vs);
			Drop(g_ps);
			Drop(g_instanceSRV);
			Drop(g_instanceBuffer);
			Drop(g_cb);
			Drop(g_blend);
			Drop(g_raster);
			Drop(g_depthLess);
			Drop(g_depthGreater);
		}

		bool CreateObjects()
		{
			auto* device = globals::d3d::device;
			if (!device) {
				return false;
			}

			ID3DBlob* vsBlob = nullptr;
			ID3DBlob* psBlob = nullptr;
			if (!Compile(kSparkleVS, "vs_5_0", &vsBlob)) {
				return false;
			}
			if (!Compile(kSparklePS, "ps_5_0", &psBlob)) {
				vsBlob->Release();
				return false;
			}

			const bool ok = SUCCEEDED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
								vsBlob->GetBufferSize(), nullptr, &g_vs)) &&
			                SUCCEEDED(device->CreatePixelShader(psBlob->GetBufferPointer(),
								psBlob->GetBufferSize(), nullptr, &g_ps));
			vsBlob->Release();
			psBlob->Release();
			if (!ok) {
				logger::error("SnowSparkle: shader creation failed");
				return false;
			}

			D3D11_BUFFER_DESC instDesc{};
			instDesc.ByteWidth = sizeof(GpuFlake) * kMaxFlakes;
			instDesc.Usage = D3D11_USAGE_DYNAMIC;
			instDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			instDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			instDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			instDesc.StructureByteStride = sizeof(GpuFlake);
			if (FAILED(device->CreateBuffer(&instDesc, nullptr, &g_instanceBuffer))) {
				logger::error("SnowSparkle: instance buffer creation failed");
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = kMaxFlakes;
			if (FAILED(device->CreateShaderResourceView(g_instanceBuffer, &srvDesc,
					&g_instanceSRV))) {
				logger::error("SnowSparkle: instance SRV creation failed");
				return false;
			}

			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = sizeof(SparkleCB);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_cb))) {
				logger::error("SnowSparkle: constant buffer creation failed");
				return false;
			}

			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(device->CreateBlendState(&blendDesc, &g_blend))) {
				logger::error("SnowSparkle: blend state creation failed");
				return false;
			}

			D3D11_RASTERIZER_DESC rasterDesc{};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.DepthClipEnable = TRUE;
			if (FAILED(device->CreateRasterizerState(&rasterDesc, &g_raster))) {
				logger::error("SnowSparkle: rasteriser state creation failed");
				return false;
			}

			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = TRUE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDesc.StencilEnable = FALSE;
			depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			if (FAILED(device->CreateDepthStencilState(&depthDesc, &g_depthLess))) {
				logger::error("SnowSparkle: depth state creation failed");
				return false;
			}
			depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
			if (FAILED(device->CreateDepthStencilState(&depthDesc, &g_depthGreater))) {
				logger::error("SnowSparkle: depth state creation failed");
				return false;
			}

			logger::info("SnowSparkle ready (max {} flakes)", kMaxFlakes);
			return true;
		}

		// The shaders, buffers and states are made once and kept until Release; a settings
		// reload only clears the flakes. A failed attempt leaves nothing half made behind.
		bool Ready()
		{
			if (g_failed) {
				return false;
			}
			if (g_vs && g_ps && g_instanceSRV && g_cb && g_blend && g_raster && g_depthLess &&
				g_depthGreater) {
				return true;
			}
			if (!CreateObjects()) {
				DropObjects();
				g_failed = true;
				return false;
			}
			return true;
		}

		ID3D11DepthStencilState* DepthStateFor(float a_minDepth, float a_maxDepth)
		{
			return a_maxDepth >= a_minDepth ? g_depthLess : g_depthGreater;
		}
	}

	void NoteHookInstalled(bool a_installed) { g_hookInstalled = a_installed; }

	bool Initialize()
	{
		if (REL::Module::IsVR() || !globals::Ready()) {
			return false;
		}
		return Ready();
	}

	bool Enabled() { return Settings::snowSparkle && g_hookInstalled; }

	void NoteContact(Surfaces::Type a_surface, const RE::NiPoint3& a_at, float a_forwardX,
		float a_forwardY, float a_velX, float a_velY, float a_radius, float a_amount)
	{

		if (!Enabled() || a_surface != Surfaces::Type::kSnow) {
			return;
		}
		if (g_contactCount >= kMaxContacts || !std::isfinite(a_velX) || !std::isfinite(a_velY)) {
			return;
		}
		const float speed = std::hypot(a_velX, a_velY);
		if (speed < Settings::snowSparkleMinSpeed) {
			return;
		}
		g_contacts[g_contactCount++] = Contact{ a_at.x, a_at.y, a_at.z, a_forwardX, a_forwardY,
			a_velX, a_velY, a_radius, speed, std::isfinite(a_amount) ? std::clamp(a_amount, 0.25f, 4.0f) : 1.0f };
	}

	void Update(float a_deltaSeconds)
	{
		if (!Enabled()) {
			g_flakes.clear();
			g_contactCount = 0;
			g_budget = 0.0f;
			return;
		}

		const float dt =
			std::isfinite(a_deltaSeconds) ? std::clamp(a_deltaSeconds, 0.0f, 0.1f) : 0.0f;

		if (dt > 0.0f && g_contactCount > 0) {
			// The busiest contact sets the rate: the fastest, a heavy creature's more.
			const float reference = std::max(Settings::snowSparkleFullSpeed, 1.0f);
			float       drive = 0.0f;
			for (size_t i = 0; i < g_contactCount; ++i) {
				drive = std::max(drive, std::clamp(g_contacts[i].speed / reference, 0.0f, 1.5f) * g_contacts[i].amount);
			}
			g_budget += Settings::snowSparkleRate * drive * dt;
		}

		const float gravity = std::max(Settings::snowSparkleGravity, 0.0f);
		const float life = std::max(Settings::snowSparkleLife, 0.05f);
		const float size = std::max(Settings::snowSparkleSize, 0.05f);
		const float reference = std::max(Settings::snowSparkleFullSpeed, 1.0f);

		float totalAmount = 0.0f;
		for (size_t i = 0; i < g_contactCount; ++i) {
			totalAmount += g_contacts[i].amount;
		}

		int spawned = 0;
		while (g_budget >= 1.0f && g_contactCount > 0) {
			g_budget -= 1.0f;
			if (g_flakes.size() >= kMaxFlakes) {
				g_budget = 0.0f;

				if (!g_reportedFull) {
					g_reportedFull = true;
					logger::warn(
						"SnowSparkle: the flake pool is full at {}. SnowSparkleRate {:.0f} "
						"with SnowSparkleLife {:.2f} asks for more than that, so the spray "
						"will not get any denser - lower the rate or the life, or accept "
						"this as the ceiling.",
						kMaxFlakes, Settings::snowSparkleRate, Settings::snowSparkleLife);
				}
				break;
			}

			// Each contact throws its share of the flakes, by amount.
			float  pick = Random01() * totalAmount;
			size_t chosen = g_contactCount - 1;
			for (size_t i = 0; i < g_contactCount; ++i) {
				pick -= g_contacts[i].amount;
				if (pick <= 0.0f) {
					chosen = i;
					break;
				}
			}
			const auto& c = g_contacts[chosen];
			// A heavy creature's snow flies higher and further.
			const float heave = std::sqrt(c.amount);

			const float speedNow = std::hypot(c.velX, c.velY);
			const float travelX = speedNow > 1.0f ? c.velX / speedNow : c.forwardX;
			const float travelY = speedNow > 1.0f ? c.velY / speedNow : c.forwardY;

			const float spread = RandomSigned();
			const float dirX = travelX + (-travelY) * spread * 0.9f;
			const float dirY = travelY + (travelX) * spread * 0.9f;
			const float len = std::hypot(dirX, dirY);
			const float ux = len > 0.001f ? dirX / len : 1.0f;
			const float uy = len > 0.001f ? dirY / len : 0.0f;

			const float throwSpeed = Settings::snowSparkleThrow *
									 (0.45f + 0.55f * Random01()) *
									 std::clamp(c.speed / reference, 0.35f, 1.4f) * heave;
			const float rise = Settings::snowSparkleRise * (0.5f + 0.7f * Random01()) * heave;

			Flake f{};

			f.x = c.x + RandomSigned() * c.radius * 0.6f;
			f.y = c.y + RandomSigned() * c.radius * 0.6f;
			f.z = c.z + 2.0f;

			const float inherit = std::clamp(Settings::snowSparkleInherit, 0.0f, 4.0f);
			f.vx = ux * throwSpeed + c.velX * inherit;
			f.vy = uy * throwSpeed + c.velY * inherit;
			f.vz = rise;
			f.age = 0.0f;
			f.life = life * (0.6f + 0.8f * Random01());
			f.size = size * (0.55f + 0.9f * Random01());
			f.seed = Random01() * 6.2831853f;
			f.floorZ = c.z - 4.0f;
			g_flakes.push_back(f);
			++spawned;
		}
		g_contactCount = 0;

		if (dt > 0.0f) {
			const float drag = 1.0f - std::exp(-std::max(Settings::snowSparkleDrag, 0.0f) * dt);
			const float wander = std::max(Settings::snowSparkleSwirl, 0.0f);
			for (size_t i = 0; i < g_flakes.size();) {
				auto& f = g_flakes[i];

				f.vx -= f.vx * drag;
				f.vy -= f.vy * drag;
				f.vz -= gravity * dt;

				if (wander > 0.0f) {
					f.vx += std::sin(f.age * 5.0f + f.seed) * wander * dt;
					f.vy += std::cos(f.age * 4.3f + f.seed * 1.7f) * wander * dt;
				}

				f.x += f.vx * dt;
				f.y += f.vy * dt;
				f.z += f.vz * dt;
				f.age += dt;

				if (f.age >= f.life || f.z < f.floorZ) {
					f = g_flakes.back();
					g_flakes.pop_back();
					continue;
				}
				++i;
			}
		}

		if (spawned > 0 && !g_reportedCount) {
			g_reportedCount = true;
			logger::info(
				"SnowSparkle: first flakes thrown; rate {:.1f}/s at {:.0f} units/s, size "
				"{:.1f}, life {:.2f}s",
				Settings::snowSparkleRate, Settings::snowSparkleFullSpeed, size, life);
		}
	}

	void Render()
	{
		if (!Enabled() || g_flakes.empty()) {
			return;
		}

		// Stereo rendering has its own per-eye camera; the flakes are flat-screen only.
		if (REL::Module::IsVR()) {
			return;
		}

		auto* context = globals::d3d::context;
		auto* frameCB = FrameCamera::Constants();
		if (!context || !frameCB || !Ready()) {
			return;
		}

		// The flakes are drawn into whatever the forward pass left bound: the scene's
		// colour target and depth. Anything else (a pass that drew nothing, a menu target)
		// is refused by size, so a wrong guess costs one frame of flakes, not a corrupted
		// image.
		ID3D11RenderTargetView* target = nullptr;
		ID3D11DepthStencilView* depth = nullptr;
		context->OMGetRenderTargets(1, &target, &depth);
		const auto release = [&]() {
			Drop(target);
			Drop(depth);
		};
		if (!target || !depth) {
			release();
			return;
		}

		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT           viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&viewportCount, viewports);
		if (viewportCount == 0) {
			release();
			return;
		}
		const float minDepth = viewports[0].MinDepth;
		const float maxDepth = viewports[0].MaxDepth;
		const auto  vpWidth = static_cast<uint32_t>(viewports[0].Width);
		const auto  vpHeight = static_cast<uint32_t>(viewports[0].Height);

		D3D11_TEXTURE2D_DESC targetDesc{};
		{
			ID3D11Resource* resource = nullptr;
			target->GetResource(&resource);
			ID3D11Texture2D* texture = nullptr;
			if (resource &&
				SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
					reinterpret_cast<void**>(&texture)))) {
				texture->GetDesc(&targetDesc);
				texture->Release();
			}
			Drop(resource);
		}
		if (targetDesc.Width == 0 || vpWidth * 2 < targetDesc.Width ||
			vpHeight * 2 < targetDesc.Height) {
			release();
			return;
		}

		// Match the engine's depth convention by reading the test the forward pass used.
		ID3D11DepthStencilState* depthTest = DepthStateFor(minDepth, maxDepth);
		{
			ID3D11DepthStencilState* current = nullptr;
			UINT                     stencilRef = 0;
			context->OMGetDepthStencilState(&current, &stencilRef);
			if (current) {
				D3D11_DEPTH_STENCIL_DESC desc{};
				current->GetDesc(&desc);
				if (desc.DepthEnable) {
					depthTest = (desc.DepthFunc == D3D11_COMPARISON_GREATER ||
					                desc.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL) ?
					                g_depthGreater :
					                g_depthLess;
				}
				current->Release();
			}
		}

		if (!g_reportedTarget) {
			g_reportedTarget = true;
			logger::info("SnowSparkle: drawing into a {}x{} target (format {})",
				targetDesc.Width, targetDesc.Height, static_cast<uint32_t>(targetDesc.Format));
		}

		// Weather colour type 3 is the ambient light: the flakes dim at night.
		float ambient = 1.0f;
		if (auto* sky = Weather::Sky()) {
			const auto& c = sky->skyColor[3];
			ambient = std::clamp((c.r + c.g + c.b) / 3.0f * 2.0f, 0.25f, 1.0f);
		}

		static std::vector<GpuFlake> upload;
		upload.clear();
		upload.reserve(g_flakes.size());
		for (const auto& f : g_flakes) {
			const float t = f.life > 0.0f ? std::clamp(f.age / f.life, 0.0f, 1.0f) : 1.0f;

			const float fade = std::min(t / 0.12f, 1.0f) * (1.0f - t) * (1.0f - t);

			GpuFlake g{};
			g.x = f.x;
			g.y = f.y;
			g.z = f.z;
			g.size = f.size;
			g.alpha = std::clamp(fade * Settings::snowSparkleOpacity, 0.0f, 1.0f);
			g.spin = f.seed + f.age * 2.0f;
			g.seed = f.seed;
			g.sparkle =
				0.75f + 0.45f * (0.5f + 0.5f * std::sin(f.age * 11.0f + f.seed * 3.1f));
			upload.push_back(g);
		}
		if (upload.empty()) {
			release();
			return;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(g_instanceBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			release();
			return;
		}
		std::memcpy(mapped.pData, upload.data(), upload.size() * sizeof(GpuFlake));
		context->Unmap(g_instanceBuffer, 0);

		SparkleCB cb{};
		cb.params[0] = Settings::snowSparkleShape;
		cb.params[1] = Settings::snowSparkleBrightness * ambient;
		cb.params[2] = static_cast<float>(vpWidth);

		if (FAILED(context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			release();
			return;
		}
		std::memcpy(mapped.pData, &cb, sizeof(cb));
		context->Unmap(g_cb, 0);

		const GraphicsStateGuard guard(context);

		context->OMSetRenderTargets(1, &target, depth);
		context->OMSetDepthStencilState(depthTest, 0);
		const FLOAT blendFactor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		context->OMSetBlendState(g_blend, blendFactor, 0xFFFFFFFF);
		context->RSSetState(g_raster);

		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_vs, nullptr, 0);
		context->PSSetShader(g_ps, nullptr, 0);
		context->VSSetShaderResources(0, 1, &g_instanceSRV);
		context->VSSetConstantBuffers(0, 1, &g_cb);
		context->VSSetConstantBuffers(Tessellation::kFrameSlot, 1, &frameCB);
		context->PSSetConstantBuffers(0, 1, &g_cb);

		context->Draw(static_cast<UINT>(upload.size()) * 6u, 0);
		release();
	}

	void Reset()
	{
		g_flakes.clear();
		g_contactCount = 0;
		g_budget = 0.0f;
		// A creation that failed gets one more try after a reload; working objects stay.
		g_failed = false;
		g_reportedCount = false;
		g_reportedFull = false;
		g_reportedTarget = false;
	}

	void Release()
	{
		g_flakes.clear();
		g_contactCount = 0;
		g_budget = 0.0f;
		DropObjects();
	}
}
