// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "Clipmap.h"

#include "Shelter.h"
#include "SnowCoverage.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>

namespace Clipmap
{

	inline constexpr float kPrintCoreLo = 0.06f;
	inline constexpr float kPrintCoreHi = 0.96f;
	inline constexpr float kPrintBandRiseLo = 0.14f;
	inline constexpr float kPrintBandRiseHi = 0.34f;
	inline constexpr float kPrintBandFallLo = 0.52f;
	inline constexpr float kPrintBandFallHi = 0.84f;

	inline float PrintHeightFor(float a_mask, float a_depth, float a_rim)
	{
		const auto smoothstep = [](float a_lo, float a_hi, float a_x) {
			const float t = std::clamp((a_x - a_lo) / (a_hi - a_lo), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		};

		const float core = smoothstep(kPrintCoreLo, kPrintCoreHi, a_mask);
		const float band = std::clamp(smoothstep(kPrintBandRiseLo, kPrintBandRiseHi, a_mask) -
										  smoothstep(kPrintBandFallLo, kPrintBandFallHi, a_mask),
			0.0f, 1.0f);

		return a_rim * band - a_depth * core;
	}

	// A texel's metadata, one R32_UINT per texel: the rate it recovers at (fraction of its
	// height left after a second) in the low 15 bits, and whether it is snow in bit 16.
	// Untouched ground is 0, a rate of 0: flat again at once. Fifteen bits keep the slow
	// rates apart (0.99 and 0.995 differ by 163 steps, 0.9999 stays below 1); the old 8-bit
	// store rounded every rate above 0.998 to 1, a mark that never recovered.
	// Bit 15 is set where heat (a fire, a jet) melted the snow: the terrain shows the ground
	// there once the snow is gone. The top 15 bits hold the scorch, 0 to 1: how black a jet
	// burned the ground there.
	inline constexpr uint32_t kMetaRateMask = 0x7FFFu;
	inline constexpr float    kMetaRateMax = 32767.0f;
	inline constexpr uint32_t kMetaHeatBit = 0x8000u;
	inline constexpr uint32_t kMetaSnowBit = 0x10000u;
	inline constexpr uint32_t kMetaScorchShift = 17u;
	inline constexpr float    kMetaScorchMax = 32767.0f;

	inline uint32_t EncodeMeta(float a_rate, bool a_snow, float a_scorch = 0.0f, bool a_heat = false)
	{
		const float rate = std::clamp(a_rate, 0.0f, 1.0f);
		const float scorch = std::clamp(a_scorch, 0.0f, 1.0f);
		return static_cast<uint32_t>(rate * kMetaRateMax + 0.5f) | (a_heat ? kMetaHeatBit : 0u) |
		       (a_snow ? kMetaSnowBit : 0u) |
		       (static_cast<uint32_t>(scorch * kMetaScorchMax + 0.5f) << kMetaScorchShift);
	}

	inline float DecodeRate(uint32_t a_meta)
	{
		return static_cast<float>(a_meta & kMetaRateMask) * (1.0f / kMetaRateMax);
	}

	inline bool DecodeHeat(uint32_t a_meta)
	{
		return (a_meta & kMetaHeatBit) != 0u;
	}

	inline float DecodeScorch(uint32_t a_meta)
	{
		return static_cast<float>(a_meta >> kMetaScorchShift) * (1.0f / kMetaScorchMax);
	}

	// The update's constant buffer (b0), shared by the plugin and the offline checks.
	struct UpdateParams
	{
		// xy window centre (cells), z cell size (world units), w texels per side
		float window[4]{};
		// x dt, y stamp count, z reseed threshold (texels from the centre), w rim span
		float control[4]{};
		// x weather fill per second, y repose slope (tan), z scorch faded per second, w rim
		// noise
		float weather[4]{};
		float stamps[kMaxStamps][4]{};
		float stampParams[kMaxStamps][4]{};
		float stampShape[kMaxStamps][4]{};
		// xy motion, z 1 for snow plus 2 for heat (a fire's or a jet's melt), w scorch
		float stampMotion[kMaxStamps][4]{};
		// x has a coarser level, y its world size, z seed smoothing, w reseed threshold
		float coarse[4]{};
		// x lean, y churn
		float rimShape[4]{};
		float snowRim[4]{};
		float raise[4]{};
		float raiseWindow[4]{};
		// x ordinary ground, y snow: the share of the excess slope one update moves,
		// already scaled for the frame time and the level's cell size (0 = no settling);
		// z a per-update seed for the scorch's dithered rounding; w the activity a scorched
		// tile reports, so the hull stage keeps detail there for the colour to follow
		float repose[4]{};
	};
	static_assert(sizeof(UpdateParams) % 16 == 0);

	// Absolute height introduced by this batch, not a clamp on its appearance. The
	// shader's smoothstep/core/band are in [0,1], its noise/churn in [-1,1]. Presses
	// select extrema rather than accumulate; heat lerps towards its floor. Decay,
	// filling, slope settling and coarse reseeding are convex, and the snow floor
	// only moves negative heights towards zero. Retaining the largest batch bound
	// therefore encloses both levels until they are actually cleared. DDS mask values
	// may exceed [0,1], but PrintProfile's smoothsteps still normalize them.
	inline float SubmittedDisplacementBound(const UpdateParams& a_params)
	{
		constexpr float unknown = std::numeric_limits<float>::infinity();
		if (!std::isfinite(a_params.control[1]) || a_params.control[1] < 0.0f) {
			return unknown;
		}
		const auto count = static_cast<uint32_t>(std::min(a_params.control[1], static_cast<float>(kMaxStamps)));
		double bound = 0.0;
		for (uint32_t i = 0; i < count; ++i) {
			const float depth = a_params.stamps[i][3];
			const float rim = a_params.stampParams[i][3];
			const float kind = a_params.stampParams[i][2];
			const bool snow = a_params.stampMotion[i][2] > 0.5f;
			const float noise = snow ? a_params.snowRim[1] : a_params.weather[3];
			const float churn = snow ? a_params.snowRim[3] : a_params.rimShape[1];
			if (!std::isfinite(depth) || !std::isfinite(rim) || !std::isfinite(kind) ||
				!std::isfinite(noise) || !std::isfinite(churn)) {
				return unknown;
			}
			double introduced = std::abs(static_cast<double>(rim));
			if (!(kind > 0.5f && kind < 1.5f)) {
				introduced += std::abs(static_cast<double>(depth));
				if (kind <= 1.5f) {
					introduced = std::abs(static_cast<double>(depth)) * (1.0 + std::abs(churn)) +
						std::max(static_cast<double>(rim), 0.0) * (1.0 + std::abs(noise));
				}
			}
			bound = std::max(bound, introduced);
		}
		// Round outwards and leave slack for shader float arithmetic/interpolation.
		bound = bound > 0.0 ? bound * 1.001 + 1.0 : 0.0;
		return bound >= std::numeric_limits<float>::max() ? unknown : static_cast<float>(bound);
	}

	// The fraction of a slope's excess one update removes, for a rate tuned per 60 Hz
	// frame: the same settling per second at any frame rate, none while paused. A coarse
	// cell is kLevelScale times wider, so its step moves kLevelScale squared less, which
	// keeps the spread in world units the same on both levels.
	inline float ReposeAlpha(float a_ratePerFrame, float a_deltaSeconds, uint32_t a_level)
	{
		const float rate = std::clamp(a_ratePerFrame, 0.0f, 1.0f);
		if (rate <= 0.0f || !(a_deltaSeconds > 0.0f)) {
			return 0.0f;
		}
		float alpha = 1.0f - std::pow(1.0f - rate, a_deltaSeconds * 60.0f);
		for (uint32_t i = 0; i < a_level; ++i) {
			alpha /= kLevelScale * kLevelScale;
		}
		return std::clamp(alpha, 0.0f, 1.0f);
	}

	static_assert(kMaxStamps <= 64, "the update culls stamps per tile with a 64-bit mask");

	// One level's views for a dispatch. The field and metadata are updated in place; the
	// activity written this frame and read from the previous one are separate textures.
	struct LevelViews
	{
		ID3D11UnorderedAccessView* field{ nullptr };
		ID3D11UnorderedAccessView* meta{ nullptr };
		ID3D11UnorderedAccessView* activity{ nullptr };
		ID3D11ShaderResourceView*  prevActivity{ nullptr };
		ID3D11ShaderResourceView*  coarseField{ nullptr };
		ID3D11ShaderResourceView*  coarseMeta{ nullptr };
	};

	// Reused between levels. The GPU appends pairs of neighbouring checkerboard tiles
	// and copies the append count straight into DispatchIndirect: no CPU readback or wait.
	struct SparseWork
	{
		ID3D11Buffer* pairs{ nullptr };
		ID3D11ShaderResourceView* srv{ nullptr };
		ID3D11UnorderedAccessView* uav{ nullptr };
		ID3D11Buffer* args{ nullptr };

		SparseWork() = default;
		SparseWork(const SparseWork&) = delete;
		SparseWork& operator=(const SparseWork&) = delete;
		~SparseWork() { Release(); }
		void Release()
		{
			if (srv) { srv->Release(); srv = nullptr; }
			if (uav) { uav->Release(); uav = nullptr; }
			if (pairs) { pairs->Release(); pairs = nullptr; }
			if (args) { args->Release(); args = nullptr; }
		}
		bool Create(ID3D11Device* a_device, uint32_t a_texels)
		{
			Release();
			const UINT count = (a_texels / 16) * (a_texels / 8);
			if (!count || count > 65535) { return false; }
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = count * 2 * sizeof(uint32_t);
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = 2 * sizeof(uint32_t);
			D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
			view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			view.Buffer.NumElements = count;
			view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
			if (FAILED(a_device->CreateBuffer(&desc, nullptr, &pairs)) ||
				FAILED(a_device->CreateShaderResourceView(pairs, nullptr, &srv)) ||
				FAILED(a_device->CreateUnorderedAccessView(pairs, &view, &uav))) {
				Release();
				return false;
			}
			desc = {};
			desc.ByteWidth = 3 * sizeof(uint32_t);
			desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			const uint32_t counts[3]{ 0, 1, 1 };
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = counts;
			if (FAILED(a_device->CreateBuffer(&desc, &initial, &args))) {
				Release();
				return false;
			}
			return true;
		}
	};

	// Two 16-byte constant buffers telling the shader which half of the tile
	// checkerboard a dispatch updates.
	inline bool CreateHalfBuffers(ID3D11Device* a_device, ID3D11Buffer* (&a_out)[2])
	{
		for (uint32_t half = 0; half < 2; ++half) {
			const uint32_t data[4]{ half, 0, 0, 0 };
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = sizeof(data);
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = data;
			if (FAILED(a_device->CreateBuffer(&desc, &initial, &a_out[half]))) {
				return false;
			}
		}
		return true;
	}

	// Runs one level's update (shader, b0, s0 and the shape/floor maps already bound):
	// clears this frame's activity, then updates the two halves of the 8x8 tile
	// checkerboard one after the other. Within a dispatch no tile touches a texel another
	// tile writes, so the settling step reads its neighbours race-free. a_firstHalf
	// alternates per frame so neither half always sees the other's new values.
	inline void DispatchLevel(ID3D11DeviceContext* a_context, const LevelViews& a_views,
		ID3D11Buffer* const (&a_halves)[2], uint32_t a_firstHalf, uint32_t a_texels,
		const SparseWork* a_work = nullptr, ID3D11ComputeShader* a_select = nullptr,
		ID3D11ComputeShader* a_update = nullptr)
	{
		const UINT zero[4]{};
		a_context->ClearUnorderedAccessViewUint(a_views.activity, zero);

		ID3D11ShaderResourceView* seeds[2]{ a_views.coarseField, a_views.coarseMeta };
		a_context->CSSetShaderResources(1, 2, seeds);
		a_context->CSSetShaderResources(5, 1, &a_views.prevActivity);

		if (a_work) {
			a_context->CSSetShader(a_select, nullptr, 0);
			a_context->CSSetUnorderedAccessViews(3, 1, &a_work->uav, zero);
			a_context->Dispatch((a_texels / 16 + 7) / 8, (a_texels / 8 + 7) / 8, 1);
			ID3D11UnorderedAccessView* empty = nullptr;
			a_context->CSSetUnorderedAccessViews(3, 1, &empty, nullptr);
			a_context->CopyStructureCount(a_work->args, 0, a_work->uav);
			a_context->CSSetShaderResources(6, 1, &a_work->srv);
			a_context->CSSetShader(a_update, nullptr, 0);
		}

		const UINT                 noOffset[3]{ ~0u, ~0u, ~0u };
		ID3D11UnorderedAccessView* uavs[3]{ a_views.field, a_views.meta, a_views.activity };
		a_context->CSSetUnorderedAccessViews(0, 3, uavs, noOffset);

		const UINT tiles = a_texels / 8;
		for (uint32_t i = 0; i < 2; ++i) {
			a_context->CSSetConstantBuffers(1, 1, &a_halves[(a_firstHalf + i) & 1u]);
			if (a_work) {
				a_context->DispatchIndirect(a_work->args, 0);
			} else {
				a_context->Dispatch(tiles / 2, tiles, 1);
			}
		}

		ID3D11UnorderedAccessView* nullUAVs[3]{};
		a_context->CSSetUnorderedAccessViews(0, 3, nullUAVs, noOffset);
		ID3D11ShaderResourceView* nullSRVs[2]{};
		a_context->CSSetShaderResources(1, 2, nullSRVs);
		a_context->CSSetShaderResources(5, 1, nullSRVs);
		if (a_work) { a_context->CSSetShaderResources(6, 1, nullSRVs); }
	}

	constexpr char kUpdateShader[] = R"(
RWTexture2D<float> Field : register(u0);

// Recovery rate (low 15 bits), the heat flag (bit 15), the snow flag (bit 16) and the
// scorch (bits 17-31); see EncodeMeta.
RWTexture2D<uint> Meta : register(u1);

RWTexture2D<uint> Activity : register(u2);

Texture2D<float4> ShapeMask : register(t0);
SamplerState      ShapeSampler : register(s0);

Texture2D<float> CoarseField : register(t1);
Texture2D<uint>  CoarseMeta : register(t2);

Texture2D<float> SnowCoverageMap : register(t3);
Texture2D<float> SnowMeshCapMap : register(t4);

// This level's activity from its previous update: 0 where the field was flat.
Texture2D<uint> PrevActivity : register(t5);
StructuredBuffer<uint2> WorkPairs : register(t6);
AppendStructuredBuffer<uint2> SelectedPairs : register(u3);

cbuffer Params : register(b0)
{

	float4 Window;

	float4 Control;

	float4 Weather;

	float4 Stamps[MAX_STAMPS];

	float4 StampParams[MAX_STAMPS];

	float4 StampShape[MAX_STAMPS];

	float4 StampMotion[MAX_STAMPS];

	float4 Coarse;

	float4 RimShape;

	float4 SnowRim;

	float4 Raise;

	float4 RaiseWindow;

	float4 Repose;
};

cbuffer Half : register(b1)
{
	// x: which half of the tile checkerboard this dispatch updates
	uint4 HalfInfo;
};

static const uint kSnowBit = 0x10000u;
static const uint kHeatBit = 0x8000u;

// A recovered mark is flattened once it is this shallow, so its tile goes quiet.
static const float kQuietHeight = 0.01f;

float DecodeRate(uint meta)
{
	return (float)(meta & 0x7FFFu) * (1.0f / 32767.0f);
}

static const uint  kScorchShift = 17u;
static const float kScorchMax   = 32767.0f;

// The scorch in whole steps of 1/kScorchMax, kept as a float (exact at this size).
float ScorchSteps(uint meta)
{
	return (float)(meta >> kScorchShift);
}

// A stamp's flags in StampMotion.z: 1 for snow, plus 2 for heat (a fire's or a jet's melt).
bool StampIsSnow(uint i)
{
	return fmod(StampMotion[i].z, 2.0f) > 0.5f;
}

bool StampIsHeat(uint i)
{
	return StampMotion[i].z > 1.5f;
}

uint EncodeMeta(float rate, float snow, float scorchSteps, float heat)
{
	return (uint)(saturate(rate) * 32767.0f + 0.5f) | (heat > 0.5f ? kHeatBit : 0u) |
		(snow > 0.5f ? kSnowBit : 0u) |
		((uint)clamp(scorchSteps, 0.0f, kScorchMax) << kScorchShift);
}

)"

		R"(

uint HashCell(int2 cell)
{
	uint2 q = uint2(cell) * uint2(1597334673u, 3812015801u);
	uint  n = (q.x ^ q.y) * 1597334673u;
	n ^= n >> 15;
	return n * 2246822519u;
}

float ValueNoise(float2 p)
{
	const float2 base = floor(p);
	float2       f = p - base;
	f = f * f * (3.0f - 2.0f * f);

	const int2 c = int2(base);
	const float a = (float)HashCell(c)                  * (1.0f / 4294967296.0f);
	const float b = (float)HashCell(c + int2(1, 0))     * (1.0f / 4294967296.0f);
	const float d = (float)HashCell(c + int2(0, 1))     * (1.0f / 4294967296.0f);
	const float e = (float)HashCell(c + int2(1, 1))     * (1.0f / 4294967296.0f);

	return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}

float FractalNoise(float2 worldXY, float wavelength)
{
	const float2x2 turn = float2x2(0.7986f, -0.6018f, 0.6018f, 0.7986f);

	float2 p = worldXY / max(wavelength, 1e-3f);
	float  n = ValueNoise(p);

	p = mul(turn, p) * 2.03f;
	n += 0.5f * ValueNoise(p);

	return (n * (1.0f / 1.5f)) * 2.0f - 1.0f;
}

float RimNoise(float2 worldXY)
{
	return FractalNoise(worldXY, 6.0f);
}

float ChurnNoise(float2 worldXY)
{

	float n = FractalNoise(worldXY, 12.0f);

	n *= abs(n);

	const float patch = 0.65f + 0.35f * FractalNoise(worldXY, 70.0f);

	return n * patch;
}

float SnowBlanket(float2 worldXY)
{
	const float2 fromCentre = abs(worldXY - RaiseWindow.xy);
	const float  reach      = max(fromCentre.x, fromCentre.y);
	const float  fade       = 1.0f - saturate(
		(reach - RaiseWindow.z) / max(RaiseWindow.w - RaiseWindow.z, 1e-3f));
	if (fade <= 0.0f) {
		return 0.0f;
	}

	float2 uv = worldXY / kCoverageWorldSize;
	const float2 t = uv * kCoverageTexels + 0.5f;
	const float2 i = floor(t);
	float2 f = t - i;
	f = f * f * (3.0f - 2.0f * f);
	uv = (i + f - 0.5f) / kCoverageTexels;

	const float cover = SnowCoverageMap.SampleLevel(ShapeSampler, uv, 0.0f);
	const float snowy = smoothstep(0.5f, 1.0f, cover);

	float lift = snowy * Raise.x * Raise.y;

	if (Raise.w > 0.5f) {
		float2 cuv = worldXY / kMeshCapWorldSize;
		const float2 ct = cuv * kMeshCapTexels + 0.5f;
		const float2 ci = floor(ct);
		float2 cf = ct - ci;
		cf = cf * cf * (3.0f - 2.0f * cf);
		cuv = (ci + cf - 0.5f) / kMeshCapTexels;

		float cap = SnowMeshCapMap.SampleLevel(ShapeSampler, cuv, 0.0f);
		cap = lerp(cap, 1.0f, saturate(
			(reach - kMeshCapFadeStart) /
			max(kMeshCapFadeEnd - kMeshCapFadeStart, 1e-3f)));

		lift = min(lift, cap * Raise.x);
	}

	return lift * fade;
}

float2 SweptDelta(float2 worldXY, float4 s, float2 motion)
{
	const float2 delta = worldXY - s.xy;
	const float  lenSq = dot(motion, motion);
	if (lenSq <= 1e-4f) {
		return delta;
	}

	const float t = saturate(dot(delta + motion, motion) / lenSq);
	return delta + motion * (1.0f - t);
}

float EllipseMask(float2 uv, float2 centre, float2 radius)
{
	const float distance = length((uv - centre) / radius);
	return 1.0f - smoothstep(0.82f, 1.0f, distance);
}

float TaperedMask(float2 uv, float2 tip, float2 base, float radius)
{
	const float2 axis = base - tip;
	const float  along = saturate(dot(uv - tip, axis) / max(dot(axis, axis), 1e-5f));
	const float2 closest = tip + axis * along;
	const float  width = lerp(radius * 0.12f, radius, along);
	return 1.0f - smoothstep(width * 0.75f, width, length(uv - closest));
}

float RoundedBoxMask(float2 uv, float2 centre, float2 halfSize, float radius)
{
	const float2 q = abs(uv - centre) - halfSize + radius;
	const float distance = length(max(q, 0.0f.xx)) + min(max(q.x, q.y), 0.0f) - radius;
	return 1.0f - smoothstep(-0.015f, 0.015f, distance);
}

float CreatureFootMask(float2 uv, float style)
{
	float mask = 0.0f;
	if (style < 2.5f) {
		mask = EllipseMask(uv, float2(0.50f, 0.68f), float2(0.25f, 0.22f));
		mask = max(mask, EllipseMask(uv, float2(0.21f, 0.39f), float2(0.09f, 0.11f)));
		mask = max(mask, EllipseMask(uv, float2(0.40f, 0.32f), float2(0.09f, 0.12f)));
		mask = max(mask, EllipseMask(uv, float2(0.60f, 0.32f), float2(0.09f, 0.12f)));
		mask = max(mask, EllipseMask(uv, float2(0.79f, 0.39f), float2(0.09f, 0.11f)));
		mask = max(mask, TaperedMask(uv, float2(0.21f, 0.13f), float2(0.21f, 0.31f), 0.035f));
		mask = max(mask, TaperedMask(uv, float2(0.40f, 0.11f), float2(0.40f, 0.28f), 0.035f));
		mask = max(mask, TaperedMask(uv, float2(0.60f, 0.11f), float2(0.60f, 0.28f), 0.035f));
		mask = max(mask, TaperedMask(uv, float2(0.79f, 0.13f), float2(0.79f, 0.31f), 0.035f));
	} else if (style < 3.5f) {
		mask = EllipseMask(uv, float2(0.50f, 0.72f), float2(0.29f, 0.21f));
		mask = max(mask, EllipseMask(uv, float2(0.22f, 0.47f), float2(0.10f, 0.18f)));
		mask = max(mask, EllipseMask(uv, float2(0.50f, 0.43f), float2(0.11f, 0.21f)));
		mask = max(mask, EllipseMask(uv, float2(0.78f, 0.47f), float2(0.10f, 0.18f)));
		mask = max(mask, TaperedMask(uv, float2(0.17f, 0.10f), float2(0.22f, 0.37f), 0.055f));
		mask = max(mask, TaperedMask(uv, float2(0.50f, 0.04f), float2(0.50f, 0.34f), 0.065f));
		mask = max(mask, TaperedMask(uv, float2(0.83f, 0.10f), float2(0.78f, 0.37f), 0.055f));
	} else if (style < 4.5f) {
		mask = EllipseMask(uv, float2(0.37f, 0.52f), float2(0.13f, 0.28f));
		mask = max(mask, EllipseMask(uv, float2(0.63f, 0.52f), float2(0.13f, 0.28f)));
	} else if (style < 5.5f) {
		mask = RoundedBoxMask(uv, float2(0.50f, 0.62f), float2(0.25f, 0.22f), 0.07f);
		mask = max(mask, RoundedBoxMask(uv, float2(0.34f, 0.30f), float2(0.11f, 0.12f), 0.04f));
		mask = max(mask, RoundedBoxMask(uv, float2(0.66f, 0.30f), float2(0.11f, 0.12f), 0.04f));
	} else {
		mask = EllipseMask(uv, float2(0.50f, 0.69f), float2(0.30f, 0.22f));
		mask = max(mask, EllipseMask(uv, float2(0.16f, 0.38f), float2(0.08f, 0.12f)));
		mask = max(mask, EllipseMask(uv, float2(0.33f, 0.32f), float2(0.08f, 0.12f)));
		mask = max(mask, EllipseMask(uv, float2(0.50f, 0.29f), float2(0.08f, 0.12f)));
		mask = max(mask, EllipseMask(uv, float2(0.67f, 0.32f), float2(0.08f, 0.12f)));
		mask = max(mask, EllipseMask(uv, float2(0.84f, 0.38f), float2(0.08f, 0.12f)));
	}
	return mask;
}

float PrintProfile(float2 worldXY, float4 s, float4 p, float4 shape, float2 motion)
{
	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float2 delta = SweptDelta(worldXY, s, motion);

	const float2 local = float2(dot(delta, right), dot(delta, forward));
	const float  halfWidth = max(shape.z, 1e-3f);
	const float  halfLength = max(s.z, 1e-3f);
	const float  mirror = shape.w < 0.0f ? -1.0f : 1.0f;

	const float2 uv = float2(
		0.5f + (local.x * mirror) / (halfWidth * 2.0f),
		0.5f - local.y / (halfLength * 2.0f));

	if (any(uv < 0.0f) || any(uv > 1.0f)) {
		return 0.0f;
	}

	const float style = floor(abs(shape.w) + 0.5f);
	const float m = style < 1.5f ? ShapeMask.SampleLevel(ShapeSampler, uv, 0).r :
		CreatureFootMask(uv, style);

	const float core = smoothstep(kPrintCoreLo, kPrintCoreHi, m);
	const float band = saturate(
		smoothstep(kPrintBandRiseLo, kPrintBandRiseHi, m) -
		smoothstep(kPrintBandFallLo, kPrintBandFallHi, m));

	return p.w * band - s.w * core;
}

float StampDistance(float2 worldXY, float4 s, float4 shape, float2 motion)
{
	const float2 delta = SweptDelta(worldXY, s, motion);
	if (shape.z <= 0.0f) {
		return length(delta);
	}

	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float halfLength = max(s.z, 1e-3f);
	const float halfWidth = max(shape.z, 1e-3f);

	const float2 n = float2(dot(delta, forward) / halfLength,
		dot(delta, right) / halfWidth);

	return length(n) * halfLength;
}

// A press under a resting object carries -(1 + its rim margin) where a print keeps its
// mirror; anything else answers -1.
float ObjectRimMargin(float4 p, float4 shape)
{
	return p.z < 0.5f && shape.w <= -1.0f ? -shape.w - 1.0f : -1.0f;
}

// How far a point lies outside a stamp's footprint in world units, negative inside:
// exact for a disc and along an ellipse's axes, Quilez's approximation between them.
float FootprintOutside(float2 worldXY, float4 s, float4 shape, float2 motion)
{
	const float2 delta = SweptDelta(worldXY, s, motion);
	if (shape.z <= 0.0f) {
		return length(delta) - s.z;
	}

	const float2 forward = normalize(shape.xy);
	const float2 right = float2(-forward.y, forward.x);

	const float2 r = float2(max(s.z, 1e-3f), max(shape.z, 1e-3f));
	const float2 q = float2(dot(delta, forward), dot(delta, right));

	const float k0 = length(q / r);
	const float k1 = length(q / (r * r));
	return k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -min(r.x, r.y);
}

// How far from its centre (before its sweep) a stamp can change the field: its size and
// rim, and an object's rim past its margin.
float StampReach(float4 s, float4 p, float4 shape, float4 rim)
{
	const float size = max(s.z, shape.z);
	float reach = size * (1.0f + max(rim.x, 0.0f) * (1.0f + saturate(rim.z)));

	const float margin = ObjectRimMargin(p, shape);
	if (margin >= 0.0f) {
		reach = max(reach, size * (1.0f + 2.0f * max(rim.x, 0.0f)) + margin);
	}
	return reach;
}

)"

		R"(
groupshared uint  gBlockMax;
groupshared uint  gStampMask[2];
groupshared float gTile[10][10];

// Whether stamp i can reach any texel whose centre lies in [tileMin, tileMax]: the same
// bounds the per-texel loop below rejects with, grown to the tile.
bool StampReachesTile(uint i, float2 tileMin, float2 tileMax)
{
	const float4 s = Stamps[i];
	const float2 motion = StampMotion[i].xy;
	const float4 rim = StampIsSnow(i) ? SnowRim :
		float4(Control.w, Weather.w, RimShape.x, RimShape.y);

	const float reach = StampReach(s, StampParams[i], StampShape[i], rim);
	const float2 lo = s.xy + min(0.0f.xx, -motion) - reach;
	const float2 hi = s.xy + max(0.0f.xx, -motion) + reach;
	return all(tileMax >= lo) && all(tileMin <= hi);
}

// One cheap thread tests a 16x8 pair before either checkerboard half runs. Include
// every pair that either tile could update: old marks (including their apron), new
// stamps, wrap seams, and boundary reseeding from the already-updated coarse level.
// Pair order in the append buffer does not affect the race-free checkerboard update.
[numthreads(8, 8, 1)]
void selectPairs(uint3 id : SV_DispatchThreadID)
{
	const int n = (int)Window.w;
	if (id.x >= (uint)n / 16u || id.y >= (uint)n / 8u) { return; }
	const uint2 tile = id.xy * uint2(2u, 1u);
	const int2 centre = int2(Window.xy);
	const int2 base = centre - (n >> 1);
	const int2 lo = base + ((int2(tile * 8u) - base) & (n - 1));
	const int2 hi = base + ((int2(tile * 8u) + int2(15, 7) - base) & (n - 1));
	const int2 farthest = max(abs(lo - centre), abs(lo + int2(15, 7) - centre));
	bool active = any(hi != lo + int2(15, 7)) ||
		max(farthest.x, farthest.y) >= (int)Coarse.w ||
		PrevActivity.Load(int3(tile / kActivityGroups, 0)) != 0u;
	if (!active) {
		const float2 tileMin = (float2(lo) + 0.5f) * Window.z;
		const float2 tileMax = (float2(hi) + 0.5f) * Window.z;
		[loop] for (uint i = 0; i < (uint)Control.y; ++i) {
			if (StampReachesTile(i, tileMin, tileMax)) { active = true; break; }
		}
	}
	if (active) { SelectedPairs.Append(id.xy); }
}

// One thread per texel, one group per 8x8 tile, and only half the tiles per dispatch:
// tile (x, y) belongs to half (x + y) & 1. A tile keeps a one-texel apron of its
// neighbours in group memory for the settling step; those neighbours are all in the
// other half, so nothing this dispatch writes is read by another group.
[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID,
	uint groupIndex : SV_GroupIndex)
{
	const int n = (int)Window.w;
	const int mask = n - 1;

#ifdef SPARSE_DISPATCH
	const uint2 pair = WorkPairs[gid.x];
#else
	const uint2 pair = gid.xy;
#endif
	const uint2 tile = uint2(pair.x * 2u + ((pair.y + HalfInfo.x) & 1u), pair.y);
	const uint2 tileBase = tile * 8u;
	const uint2 id = tileBase + gtid.xy;

	const int2 centreCell = int2(Window.xy);
	const int2 base = centreCell - (n >> 1);
	const int2 cell = base + int2(
		((int)id.x - base.x) & mask,
		((int)id.y - base.y) & mask);

	// Texel i holds the field at the centre of world cell i, where the hull and domain
	// stages' bilinear reads put it.
	const float2 worldXY = (float2(cell) + 0.5f) * Window.z;

	// The tile's cells run on from its first texel unless the window's wrap seam cuts
	// through it; a cut tile is never culled or skipped.
	const int2 cellLo = base + int2(
		((int)tileBase.x - base.x) & mask,
		((int)tileBase.y - base.y) & mask);
	const int2 cellHi = base + int2(
		((int)tileBase.x + 7 - base.x) & mask,
		((int)tileBase.y + 7 - base.y) & mask);
	const bool seam = any(cellHi != cellLo + 7);

	const float2 tileMin = (float2(cellLo) + 0.5f) * Window.z;
	const float2 tileMax = (float2(cellLo + 7) + 0.5f) * Window.z;

	const int2 tileFar = max(abs(cellLo - centreCell), abs(cellLo + 7 - centreCell));
	const bool reseeds = seam || max(tileFar.x, tileFar.y) >= (int)Coarse.w;

	if (groupIndex == 0) {
		gBlockMax = 0;
		gStampMask[0] = 0;
		gStampMask[1] = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	// Each thread tests one stamp against the whole tile.
	const uint count = (uint)Control.y;
	if (groupIndex < count && (seam || StampReachesTile(groupIndex, tileMin, tileMax))) {
		InterlockedOr(gStampMask[groupIndex >> 5], 1u << (groupIndex & 31u));
	}
	GroupMemoryBarrierWithGroupSync();

	// A tile that was flat after the last update (the activity is dilated over the
	// neighbouring cells, so its apron was flat too), that no stamp reaches and that is
	// not reseeded stays flat: skip it whole.
	const uint2 activityCell = tile / kActivityGroups;
	const bool  quiet = !reseeds && gStampMask[0] == 0u && gStampMask[1] == 0u &&
		PrevActivity.Load(int3(activityCell, 0)) == 0u;

	if (!quiet) {
		for (uint k = groupIndex; k < 100u; k += 64u) {
			const uint2 a = uint2(k % 10u, k / 10u);
			gTile[a.y][a.x] = Field[(tileBase + a - 1u) & (uint)mask];
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (!quiet) {
		const uint2 at = gtid.xy + 1u;

		float rimNoise = 0.0f;
		float churn = 0.0f;
		bool  haveNoise = false;

		float h = gTile[at.y][at.x];
		const uint meta = Meta[id.xy];
		float rate = DecodeRate(meta);
		float snow = (meta & kSnowBit) != 0u ? 1.0f : 0.0f;
		float heat = (meta & kHeatBit) != 0u ? 1.0f : 0.0f;
		// The fade is rounded with a dither in [0, 1): at a high frame rate a slow fade is
		// less than one step per update, which plain rounding would never take off.
		float scorch = ScorchSteps(meta);
		if (Control.x > 0.0f && scorch > 0.0f) {
			const float dither = (float)(HashCell(cell + int2((int)Repose.z, 0)) >> 8) *
				(1.0f / 16777216.0f);
			scorch = max(scorch - floor(Weather.z * Control.x * kScorchMax + dither), 0.0f);
		}

		if (Control.x > 0.0f) {
			h *= pow(max(saturate(rate), 1e-6f), Control.x);
		}

		if (Weather.x > 0.0f && Control.x > 0.0f) {
			h *= pow(max(1.0f - Weather.x, 0.0f), Control.x);
		}

		float melt = 0.0f;

		float meltFloor = 0.0f;

		float meltRate = 0.0f;

		float target = 0.0f;
		float targetRate = 0.0f;
		float targetSnow = 0.0f;

		[loop] for (uint word = 0; word < 2u; ++word) {
			uint bits = gStampMask[word];
			[loop] while (bits != 0u) {
				const uint i = word * 32u + firstbitlow(bits);
				bits &= bits - 1u;

				const float4 s = Stamps[i];
				const float4 p = StampParams[i];
				const float2 motion = StampMotion[i].xy;
				const float stampSnow = StampIsSnow(i) ? 1.0f : 0.0f;
				const float4 rim = stampSnow > 0.5f ? SnowRim :
					float4(Control.w, Weather.w, RimShape.x, RimShape.y);

				const float2 delta = worldXY - s.xy;

				const float reach = StampReach(s, p, StampShape[i], rim);
				const float2 lo = min(0.0f.xx, -motion) - reach;
				const float2 hi = max(0.0f.xx, -motion) + reach;
				if (any(delta < lo) || any(delta > hi)) {
					continue;
				}

				if (!haveNoise) {
					haveNoise = true;
					rimNoise = max(Weather.w, SnowRim.y) > 0.0f ? RimNoise(worldXY) : 0.0f;
					churn = max(RimShape.y, SnowRim.w) > 0.0f ? ChurnNoise(worldXY) : 0.0f;
				}

				const float  d = StampDistance(worldXY, s, StampShape[i], motion);

				const float f = 1.0f - smoothstep(s.z * p.x, s.z, d);

				scorch = max(scorch, round(saturate(StampMotion[i].w * f) * kScorchMax));

				if (p.z > 0.5f && p.z < 1.5f) {
					const float meltHere = s.w * f;
					// Heat melting snow (a floor below the blanket's top) bares the ground;
					// a body's hollow does not.
					if (StampIsHeat(i) && meltHere > 0.0f && p.w < 0.0f) {
						heat = 1.0f;
					}
					if (meltHere > melt) {
						melt = meltHere;

						meltFloor = p.w * f;
						meltRate = p.y;
					}
					continue;
				}

				if (p.z > 1.5f) {
					const float printed =
						PrintProfile(worldXY, s, p, StampShape[i], motion);
					if (abs(printed) > abs(target)) {
						target = printed;
						targetRate = p.y;
						targetSnow = stampSnow;
					}
					continue;
				}

				float profile = -s.w * f * max(1.0f + rim.w * churn, 0.0f);

				const float objectMargin = ObjectRimMargin(p, StampShape[i]);
				if (p.w > 0.0f && rim.x > 0.0f && objectMargin >= 0.0f) {
					// Soil an object pressed aside: a band as wide as the object is narrow,
					// rising only past its margin, so none of it lies over the object.
					const float narrow = StampShape[i].z > 0.0f ? min(s.z, StampShape[i].z) : s.z;
					const float width = max(narrow * rim.x, 1e-4f);
					const float outside = FootprintOutside(worldXY, s, StampShape[i], motion);
					const float u = (outside - objectMargin) / width - 1.0f;

					const float bump = saturate(1.0f - u * u);

					profile += p.w * bump * bump * max(1.0f + rim.y * rimNoise, 0.0f);
				} else if (p.w > 0.0f && rim.x > 0.0f) {
					const float span = max(s.z * rim.x, 1e-4f);

					const float lean  = saturate(rim.z);
					const float rimReach = span * (d >= s.z ? 1.0f + lean : 1.0f - lean);
					const float u = (d - s.z) / max(rimReach, 1e-4f);

					const float bump = saturate(1.0f - u * u);

					profile += p.w * bump * bump * max(1.0f + rim.y * rimNoise, 0.0f);
				}

				if (abs(profile) > abs(target)) {
					target = profile;
					targetRate = p.y;
					targetSnow = stampSnow;
				}
			}
		}

		if (target < 0.0f && target < h) {
			h = target;
			rate = targetRate;
			snow = targetSnow;
		} else if (target > 0.0f && target > h && h >= 0.0f) {
			h = target;
			rate = targetRate;
			snow = targetSnow;
		}

		if (melt > 0.0f && Control.x > 0.0f) {
			const float remains = pow(max(1.0f - melt, 0.0f), Control.x);
			h = meltFloor + (h - meltFloor) * remains;

			// A floor below the blanket's top is snow melted away (a fire, a jet, a body's
			// hollow); one above it is snow too (the mound grown over a corpse). Either keeps
			// the stamp's recovery rate, so the mark outlasts its stamps; a floor of 0 (heat on
			// bare ground) changes neither.
			if (meltFloor != 0.0f) {
				rate = max(rate, meltRate);
				snow = 1.0f;
			}
		}

)"

		R"(

		// Settling: ground steeper than the repose slope towards a neighbour slides part
		// of the excess down. The neighbours are read as they were before this update.
		const float repose = snow > 0.5f ? Repose.y : Repose.x;
		if (Weather.y > 0.0f && repose > 0.0f) {
			const float maxStep = Window.z * Weather.y;

			const float n0 = gTile[at.y][at.x - 1u];
			const float n1 = gTile[at.y][at.x + 1u];
			const float n2 = gTile[at.y - 1u][at.x];
			const float n3 = gTile[at.y + 1u][at.x];

			float pull = 0.0f;
			pull += sign(n0 - h) * max(abs(n0 - h) - maxStep, 0.0f);
			pull += sign(n1 - h) * max(abs(n1 - h) - maxStep, 0.0f);
			pull += sign(n2 - h) * max(abs(n2 - h) - maxStep, 0.0f);
			pull += sign(n3 - h) * max(abs(n3 - h) - maxStep, 0.0f);

			h += pull * 0.25f * saturate(repose);
		}

		const int2 delta = abs(cell - centreCell);
		if (max(delta.x, delta.y) >= (int)Coarse.w) {
			if (Coarse.x > 0.5f) {

				const float2 uv = worldXY / max(Coarse.y, 1e-3f);

				const float o = (0.5f / Window.w) * Coarse.z;

				float sum = CoarseField.SampleLevel(ShapeSampler, uv + float2(-o, -o), 0).r;
				sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(o, -o), 0).r;
				sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(-o, o), 0).r;
				sum += CoarseField.SampleLevel(ShapeSampler, uv + float2(o, o), 0).r;
				h = sum * 0.25f;

				// The metadata is not blended: of the four coarse texels around this
				// point, a mark's rate wins over untouched ground's 0 (a blend would make
				// a mark's edge recover in a second), and the deepest one's snow flag wins.
				const int2 c0 = int2(floor(uv * Window.w - 0.5f));
				float bestDepth = -1.0f;
				uint  bestMeta = 0u;
				float bestRate = 0.0f;
				float bestScorch = 0.0f;
				bool  anyHeat = false;
				[unroll] for (int tap = 0; tap < 4; ++tap) {
					const int2  c = (c0 + int2(tap & 1, tap >> 1)) & mask;
					const uint  m = CoarseMeta.Load(int3(c, 0));
					const float depth = abs(CoarseField.Load(int3(c, 0)));
					bestRate = max(bestRate, DecodeRate(m));
					bestScorch = max(bestScorch, ScorchSteps(m));
					anyHeat = anyHeat || (m & kHeatBit) != 0u;
					if (depth > bestDepth) {
						bestDepth = depth;
						bestMeta = m;
					}
				}
				rate = bestRate;
				snow = (bestMeta & kSnowBit) != 0u ? 1.0f : 0.0f;
				scorch = bestScorch;
				heat = anyHeat ? 1.0f : 0.0f;
			} else {
				h = 0.0f;
				rate = 0.0f;
				snow = 0.0f;
				scorch = 0.0f;
				heat = 0.0f;
			}
		}

		if (Raise.x > 0.0f && snow > 0.5f && h < 0.0f) {
			h = max(h, -(SnowBlanket(worldXY) + Raise.z));
		}

		// Flat again (the snow back): nothing bare to show.
		if (abs(h) < kQuietHeight) {
			h = 0.0f;
			heat = 0.0f;
		}

		Field[id.xy] = h;
		Meta[id.xy] = EncodeMeta(rate, snow, scorch, heat);

		// A scorched tile stays active, to keep fading and to keep its detail.
		const float bound = max(abs(h), scorch > 0.0f ? Repose.w : 0.0f);
		if (bound > 0.0f) {
			InterlockedMax(gBlockMax, asuint(bound));
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (groupIndex == 0 && gBlockMax != 0u) {
		const int2 centre = int2(activityCell);
		for (int dy = -1; dy <= 1; ++dy) {
			for (int dx = -1; dx <= 1; ++dx) {

				InterlockedMax(
					Activity[uint2((centre + int2(dx, dy)) & kActivityWrap)], gBlockMax);
			}
		}
	}
}
)";

	inline std::string UpdateShaderSource()
	{
		return std::format(
				"static const float kPrintCoreLo = {:.6f};\n"
				"static const float kPrintCoreHi = {:.6f};\n"
				"static const float kPrintBandRiseLo = {:.6f};\n"
				"static const float kPrintBandRiseHi = {:.6f};\n"
				"static const float kPrintBandFallLo = {:.6f};\n"
				"static const float kPrintBandFallHi = {:.6f};\n"

				"static const uint2 kActivityGroups = uint2({}, {});\n"

				"static const int2 kActivityWrap = int2({}, {});\n"

				"static const float kCoverageWorldSize = {:.4f}f;\n"
				"static const float kCoverageTexels    = {:.1f}f;\n"
				"static const float kMeshCapWorldSize  = {:.4f}f;\n"
				"static const float kMeshCapTexels     = {:.1f}f;\n"

				"static const float kMeshCapFadeStart  = {:.4f}f;\n"
				"static const float kMeshCapFadeEnd    = {:.4f}f;\n",
				kPrintCoreLo, kPrintCoreHi, kPrintBandRiseLo, kPrintBandRiseHi,
				kPrintBandFallLo, kPrintBandFallHi,
				kActivityRatio / 8, kActivityRatio / 8,
				kActivityTexels - 1, kActivityTexels - 1,
				SnowCoverage::kWorldSize, static_cast<float>(SnowCoverage::kTexels),
				Shelter::kWorldSize, static_cast<float>(Shelter::kTexels),
				Shelter::kWorldSize * 0.39f, Shelter::kWorldSize * 0.47f) +
			kUpdateShader;
	}
}
