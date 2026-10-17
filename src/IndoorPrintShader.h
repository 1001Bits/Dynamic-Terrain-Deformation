// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// The floor-print pass: once the game's G-buffer geometry pass is done, before its decals and
// lighting, each print in view is drawn as a box around its sole (instanced, 36 vertices). A
// covered pixel rebuilds its position from the depth, keeps it if it lies inside the box on a
// face that points up, and blends the albedo toward the print's tint through the sole's
// shape (the terrain prints' own mask, or a drawn sole without it). It writes the albedo
// target only, colour and not alpha. The camera is the one the world was drawn with: the
// game's per-frame constants (cb12, FrameCamera), camera-relative positions as the game
// renders them.
namespace IndoorPrints
{
	inline constexpr const char* kShaderSource = R"(
cbuffer PerFrame : register(b12)
{
	row_major float4x4 CameraViewProj        : packoffset(c8);   // camera-relative position -> clip
	row_major float4x4 CameraViewProjInverse : packoffset(c16);  // clip -> camera-relative position
	float4             CameraPosAdjust       : packoffset(c35);  // the eye (world)
};

cbuffer PrintFrame : register(b0)
{
	float4 Viewport;  // left, top, width, height (pixels)
	float4 Look;      // has mask, min up, nearest distance kept, -
};

struct Print
{
	float4 centre;  // sole (world), half height
	float4 axes;    // forward x, forward y, half length, half width
	float4 colour;  // tint in the target's space, strength
	float4 misc;    // mirror (1 right, -1 left)
};

Texture2D<float>         Depth  : register(t0);
StructuredBuffer<Print>  Prints : register(t1);
Texture2D<float4>        Mask   : register(t2);
SamplerState             Linear : register(s0);

struct Vertex
{
	float4 position : SV_Position;
	nointerpolation uint print : PRINT;
};

static const float3 kCorners[8] = {
	float3(-1, -1, -1), float3(1, -1, -1), float3(1, 1, -1), float3(-1, 1, -1),
	float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1) };
static const uint kFaces[36] = {
	0, 1, 2, 0, 2, 3,  4, 6, 5, 4, 7, 6,  0, 4, 5, 0, 5, 1,
	1, 5, 6, 1, 6, 2,  2, 6, 7, 2, 7, 3,  3, 7, 4, 3, 4, 0 };

float3 RightOf(float2 a_forward)
{
	return float3(-a_forward.y, a_forward.x, 0.0f);
}

Vertex vsMain(uint a_vertex : SV_VertexID, uint a_instance : SV_InstanceID)
{
	const Print  p = Prints[a_instance];
	const float3 corner = kCorners[kFaces[a_vertex % 36u]];
	const float3 forward = float3(p.axes.xy, 0.0f);
	// Grown a little so a sloped or bumpy floor inside the box is still covered.
	const float3 world = p.centre.xyz + forward * (corner.y * p.axes.z * 1.05f) +
		RightOf(p.axes.xy) * (corner.x * p.axes.w * 1.05f) + float3(0.0f, 0.0f, corner.z * p.centre.w * 1.05f);
	Vertex o;
	o.position = mul(CameraViewProj, float4(world - CameraPosAdjust.xyz, 1.0f));
	o.print = a_instance;
	return o;
}

float3 RelativeAt(int2 a_pixel)
{
	const float  depth = Depth.Load(int3(a_pixel, 0));
	const float2 uv = (float2(a_pixel) + 0.5f - Viewport.xy) / Viewport.zw;
	const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
	const float4 relative = mul(CameraViewProjInverse, clip);
	return relative.xyz / relative.w;
}

// A sole drawn without the mask: a ball and a heel, narrower at the arch.
float DrawnSole(float a_across, float a_along)
{
	const float ball = length(float2(a_across / 0.95f, (a_along - 0.35f) / 0.62f));
	const float heel = length(float2(a_across / 0.78f, (a_along + 0.55f) / 0.42f));
	return 1.0f - smoothstep(0.82f, 1.0f, min(ball, heel));
}

float4 psMain(Vertex a_in) : SV_Target0
{
	const Print  p = Prints[a_in.print];
	const int2   pixel = int2(a_in.position.xy);
	const float3 centre = RelativeAt(pixel);
	const float  distance = length(centre);
	// The first-person weapon and hands, drawn with their own projection, and the sky.
	if (!(distance > Look.z)) {
		discard;
	}
	const float3 world = centre + CameraPosAdjust.xyz;

	const float2 forward = p.axes.xy;
	const float2 delta = world.xy - p.centre.xy;
	const float  across = dot(delta, float2(-forward.y, forward.x)) / max(p.axes.w, 1e-3f) * p.misc.x;
	const float  along = dot(delta, forward) / max(p.axes.z, 1e-3f);
	const float  up = (world.z - p.centre.z) / max(p.centre.w, 1e-3f);
	if (abs(across) > 1.0f || abs(along) > 1.0f || abs(up) > 1.0f) {
		discard;
	}

	// Only a floor takes it: the face's direction from the neighbour on each axis nearer this
	// pixel's distance, so an edge against something far behind does not bend it.
	const float3 left = RelativeAt(pixel - int2(1, 0));
	const float3 right = RelativeAt(pixel + int2(1, 0));
	const float3 above = RelativeAt(pixel - int2(0, 1));
	const float3 below = RelativeAt(pixel + int2(0, 1));
	const float3 alongX = abs(length(right) - distance) < abs(distance - length(left)) ? right - centre : centre - left;
	const float3 alongY = abs(length(below) - distance) < abs(distance - length(above)) ? below - centre : centre - above;
	float3 normal = cross(alongX, alongY);
	const float span = length(normal);
	if (!(span > 1e-8f)) {
		discard;
	}
	normal /= span;
	if (dot(normal, -centre) < 0.0f) {
		normal = -normal;
	}
	const float floorShare = smoothstep(Look.y, min(Look.y + 0.2f, 1.0f), normal.z);

	// The terrain prints' frame: x across (mirrored for a left foot), y from the toe down.
	const float2 uv = float2(0.5f + across * 0.5f, 0.5f - along * 0.5f);
	const float  sole = Look.x > 0.5f ? smoothstep(0.08f, 0.5f, Mask.SampleLevel(Linear, uv, 0.0f).r) :
	                                    DrawnSole(across, along);
	const float  edge = 1.0f - smoothstep(0.6f, 1.0f, abs(up));
	const float  amount = saturate(p.colour.a) * sole * edge * floorShare;
	if (amount < 1.0f / 255.0f) {
		discard;
	}
	return float4(p.colour.rgb, amount);
}
)";
}
