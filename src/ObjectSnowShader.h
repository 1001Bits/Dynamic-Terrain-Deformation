// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

// The snow-on-objects pass: one full-screen triangle over the G-buffer once the game's
// geometry pass is done, before its decals and lighting. It rebuilds each pixel's position
// from the depth with the camera the terrain was drawn with (the game's per-frame constants,
// cb12), takes the face's direction from its neighbours' positions (the G-buffer's own
// normal encoding is not needed), and blends the albedo toward snow where ObjectSnowRule says
// a top is - never at or under the snow surface (the land plus the blanket), so the ground
// keeps its own texture. It writes the albedo target only, colour and not alpha.
namespace ObjectSnow
{
	inline constexpr const char* kShaderSource = R"(
cbuffer PerFrame : register(b12)
{
	row_major float4x4 CameraViewProjInverse : packoffset(c16);
	float4             CameraPosAdjust       : packoffset(c35);
};

cbuffer ObjectSnowParams : register(b0)
{
	float4 Viewport;  // left, top, width, height (pixels)
	float4 Grid;      // texel size, texels, window centre x, window centre y
	float4 Reach;     // furthest probed distance, its fade band, nearest distance, strength
	float4 Look;      // min up, fade up, snow albedo, top tolerance
	float4 Snow;      // snow depth over the land, debug colours, surface band, actor capsules
	float4 Capsules[128];  // nearby actors' body parts: (end a, radius), (end b, 1 for a fire), Snow.w of them
};

// ObjectSnowRule::kActorTopAbove.
static const float kActorTopAbove = 2.0f;

Texture2D<float>  Depth : register(t0);
Texture2D<float2> Tops  : register(t1);  // x the square's top (above the snow), y the land under it

struct Vertex
{
	float4 position : SV_Position;
};

Vertex vsMain(uint a_id : SV_VertexID)
{
	Vertex o;
	const float2 uv = float2((a_id << 1) & 2, a_id & 2);
	o.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return o;
}

// Camera-relative position (world minus CameraPosAdjust) of a pixel's depth.
float3 RelativeAt(int2 a_pixel)
{
	const float  depth = Depth.Load(int3(a_pixel, 0));
	const float2 uv = (float2(a_pixel) + 0.5f - Viewport.xy) / Viewport.zw;
	const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
	const float4 relative = mul(CameraViewProjInverse, clip);
	return relative.xyz / relative.w;
}

float Hash(float2 a_cell)
{
	return frac(sin(dot(a_cell, float2(12.9898f, 78.233f))) * 43758.5453f);
}

// A soft value noise over 24-unit cells: the snow breaks up a little instead of lying as a
// flat sheet of one shade.
float Breakup(float2 a_world)
{
	const float2 p = a_world / 24.0f;
	const float2 cell = floor(p);
	const float2 f = smoothstep(0.0f, 1.0f, p - cell);
	const float a = Hash(cell);
	const float b = Hash(cell + float2(1.0f, 0.0f));
	const float c = Hash(cell + float2(0.0f, 1.0f));
	const float d = Hash(cell + float2(1.0f, 1.0f));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 psMain(Vertex a_in) : SV_Target0
{
	const int2   pixel = int2(a_in.position.xy);
	const float3 centre = RelativeAt(pixel);
	const float  distance = length(centre);

	const float3 world = centre + CameraPosAdjust.xyz;
	const float  reach = length(world.xy - Grid.zw);
	// The first-person weapon and hands are drawn with their own projection; anything this
	// close to the eye is left alone, as is the sky and everything past the probed tops.
	if (!(distance > Reach.z) || !(reach < Reach.x)) {
		discard;
	}

	// The face's direction from the neighbour on each axis whose distance is nearer this
	// pixel's, so an edge against something far behind does not bend it.
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

	const float minUp = saturate(Look.x);
	const float upShare = smoothstep(minUp, min(minUp + Look.y, 1.0f), normal.z);
	if (upShare <= 0.0f) {
		discard;
	}

	// The four nearest probed squares: a pixel on one of their tops is on a top, and the land
	// under it, interpolated between them, sets the snow surface.
	const float2 cellPosition = world.xy / Grid.x - 0.5f;
	const int2   origin = int2(floor(cellPosition));
	const float2 within = cellPosition - float2(origin);
	const uint   mask = (uint)Grid.y - 1u;
	float nearest = 1.0e30f;
	float overTop = 0.0f;
	bool  anyTop = false;
	float land = 0.0f;
	float landWeight = 0.0f;
	[unroll] for (int k = 0; k < 4; ++k) {
		const int2   cell = origin + int2(k & 1, k >> 1);
		const float2 square = Tops.Load(int3(uint2(cell) & mask, 0));
		if (square.x > -0.5e30f) {
			const float over = world.z - square.x;
			if (abs(over) < nearest) {
				nearest = abs(over);
				overTop = over;
			}
			anyTop = true;
		}
		if (square.y > -0.5e30f) {
			const float w = ((k & 1) ? within.x : 1.0f - within.x) * ((k >> 1) ? within.y : 1.0f - within.y) + 1e-4f;
			land += square.y * w;
			landWeight += w;
		}
	}
	if (!(landWeight > 0.0f)) {
		discard;
	}
	land /= landWeight;
	// At or under the snow surface lies the ground or the blanket itself, never an object top.
	const float aboveSurface = smoothstep(Snow.z, Snow.z * 2.0f, world.z - (land + max(Snow.x, 0.0f)));

	const float up = clamp(normal.z, 0.2f, 1.0f);
	const float tolerance = Look.w + Grid.x * sqrt(max(1.0f - up * up, 0.0f)) / up;
	float onTop = 1.0f - smoothstep(tolerance * 0.6f, tolerance, nearest);

	// An actor (living, dead, the player) on or over a top is no top: inside its body only the
	// top's own surface is the top.
	// A fire takes none at all: it melts the snow off itself.
	bool insideActor = false;
	bool insideFire = false;
	[loop] for (uint i = 0; i < (uint)Snow.w && !insideActor && !insideFire; ++i) {
		const float3 a = Capsules[2 * i].xyz;
		const float3 ab = Capsules[2 * i + 1].xyz - a;
		const float  t = saturate(dot(world - a, ab) / max(dot(ab, ab), 1e-4f));
		const float3 d = world - (a + ab * t);
		const bool   inside = dot(d, d) < Capsules[2 * i].w * Capsules[2 * i].w;
		insideFire = inside && Capsules[2 * i + 1].w > 0.5f;
		insideActor = inside && !insideFire;
	}
	if (insideFire || (insideActor && abs(overTop) > kActorTopAbove)) {
		onTop = 0.0f;
	}

	const float fade = 1.0f - smoothstep(Reach.x - Reach.y, Reach.x, reach);
	const float amount = saturate(Reach.w) * upShare * onTop * aboveSurface * fade * lerp(0.82f, 1.0f, Breakup(world.xy));

	// Debug colours: what the pass decided for each face that points up. Green: snow; red: over
	// the snow surface but on no probed top (a top whose collision is far from the drawn
	// surface); yellow: over the snow surface with no top on any of the four squares; blue: at
	// or under the snow surface (the ground, the blanket, a buried mesh).
	if (Snow.y > 0.5f) {
		const float3 tint = amount >= 1.0f / 255.0f ? float3(0.0f, 1.0f, 0.0f) :
		                    aboveSurface <= 0.0f     ? float3(0.0f, 0.2f, 1.0f) :
		                    !anyTop                  ? float3(1.0f, 1.0f, 0.0f) :
		                                               float3(1.0f, 0.0f, 0.0f);
		return float4(tint, 0.6f * upShare * fade);
	}

	if (amount < 1.0f / 255.0f) {
		discard;
	}

	// Slightly cool white: the lighting that follows shades it like any other surface.
	return float4(Look.zzz * float3(0.96f, 0.98f, 1.0f), amount);
}
)";
}
