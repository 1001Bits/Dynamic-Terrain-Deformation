// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace StampShapes
{

	inline constexpr uint32_t kBuckets = 20;

	inline constexpr uint32_t kGrid = 128;

	constexpr char kAnalysisShader[] = R"(
Texture2D<float4>        Shape   : register(t0);
SamplerState             Samp    : register(s0);
RWStructuredBuffer<uint> Buckets : register(u0);

cbuffer Params : register(b0)
{
	float4 Grid;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)Grid.x || id.y >= (uint)Grid.y) {
		return;
	}

	const float2 uv = (float2(id.xy) + 0.5f) / Grid.xy;
	const float  m  = saturate(Shape.SampleLevel(Samp, uv, 0).r);

	const uint bucket = min((uint)(m * Grid.z), (uint)Grid.z - 1);
	InterlockedAdd(Buckets[bucket], 1);
}
)";
}
