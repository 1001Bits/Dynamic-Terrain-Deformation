// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

namespace WinterLook
{
	// One 16x16 group samples a 32x32 grid over the mip nearest 32 texels across (any
	// texture, mipped or not) and writes the mean colour and the share of white texels.
	constexpr char kMeasureShader[] = R"(
Texture2D<float4>          Source  : register(t0);
RWStructuredBuffer<float4> Results : register(u0);

cbuffer Params : register(b0)
{
	uint  Slot;
	uint  Srgb;
	float WhiteLuminance;
	float WhiteSaturation;
};

groupshared float4 g_sum[256];

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex)
{
	uint w, h, levels;
	Source.GetDimensions(0, w, h, levels);
	uint mip = 0;
	[loop] while (mip + 1 < levels && max(w >> mip, h >> mip) > 32) {
		++mip;
	}
	const uint2 size = uint2(max(w >> mip, 1u), max(h >> mip, 1u));

	float4 sum = 0;
	[unroll] for (uint sy = 0; sy < 2; ++sy) {
		[unroll] for (uint sx = 0; sx < 2; ++sx) {
			const uint2 cell = tid.xy * 2 + uint2(sx, sy);
			const uint2 texel = min((cell * size) / 32, size - 1);
			float3 c = saturate(Source.Load(int3(texel, mip)).rgb);
			if (Srgb != 0) {
				c = pow(c, 1.0 / 2.2);
			}
			const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
			const float hi = max(c.r, max(c.g, c.b));
			const float lo = min(c.r, min(c.g, c.b));
			const float sat = (hi - lo) / max(hi, 1e-3);
			sum += float4(c, lum >= WhiteLuminance && sat <= WhiteSaturation ? 1.0 : 0.0);
		}
	}

	g_sum[index] = sum;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
		if (index < stride) {
			g_sum[index] += g_sum[index + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0) {
		Results[Slot] = g_sum[0] / 1024.0;
	}
}
)";
}
