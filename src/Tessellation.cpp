// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"
#include "ShaderCompiler.h"

#include "BlanketTessellation.h"
#include "Clipmap.h"
#include "FrameCamera.h"
#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "ShaderRegistry.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SurfaceTypes.h"
#include "Tessellation.h"
#include "ObjectSnow.h"
#include "TessellationResources.h"
#include "WinterLook.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace Tessellation
{
	namespace
	{
		int IndexOf(const Reflection::Signature& a_signature, std::string_view a_name,
			uint32_t a_index)
		{
			for (size_t i = 0; i < a_signature.elements.size(); ++i) {
				const auto& e = a_signature.elements[i];
				if (e.semanticIndex == a_index && e.semanticName.size() == a_name.size() &&
					_strnicmp(e.semanticName.data(), a_name.data(), a_name.size()) == 0) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		uint32_t Components(const Reflection::SignatureElement& a_element)
		{
			uint32_t count = 0;
			for (uint32_t i = 0; i < 4; ++i) {
				count += (a_element.mask >> i) & 1u;
			}
			return count;
		}

		bool Has(const Reflection::Signature& a_signature, int a_index, uint32_t a_components)
		{
			return a_index >= 0 && Components(a_signature.elements[a_index]) >= a_components &&
			       static_cast<D3D_REGISTER_COMPONENT_TYPE>(a_signature.elements[a_index].componentType) ==
			           D3D_REGISTER_COMPONENT_FLOAT32;
		}
	}

	Layout LayoutFor(const Reflection::Signature& a_signature)
	{
		Layout out{};
		out.clip = IndexOf(a_signature, "SV_POSITION", 0);

		// Fallout 4's landscape pre-pass vertex shader writes (see docs/FO4_PORT_NOTES.md):
		//   TEXCOORD0..2.xyz  rows of the view-space tangent frame (columns: X, Y, normal)
		//   TEXCOORD3.xyz     camera-relative world position (.w carries u)
		//   TEXCOORD4.xyz     the previous frame's camera-relative world position (.w v)
		//   TEXCOORD7         the four layer blend weights
		//   COLOR0            vertex colour
		// Only a signature with all of that is post-processed beyond the clip-space lift.
		const int t0 = IndexOf(a_signature, "TEXCOORD", 0);
		const int t1 = IndexOf(a_signature, "TEXCOORD", 1);
		const int t2 = IndexOf(a_signature, "TEXCOORD", 2);
		const int world = IndexOf(a_signature, "TEXCOORD", 3);
		const int prev = IndexOf(a_signature, "TEXCOORD", 4);

		if (Has(a_signature, t0, 3) && Has(a_signature, t1, 3) && Has(a_signature, t2, 3) &&
			Has(a_signature, world, 4) && Has(a_signature, prev, 4)) {
			out.tbn[0] = t0;
			out.tbn[1] = t1;
			out.tbn[2] = t2;
			out.world = world;
			out.prevWorld = prev;

			const int weights = IndexOf(a_signature, "TEXCOORD", 7);
			if (Has(a_signature, weights, 4)) {
				out.weights = weights;
			}
		}

		const int colour = IndexOf(a_signature, "COLOR", 0);
		if (Has(a_signature, colour, 3)) {
			out.colour = colour;
		}

		// VR's landscape shader adds EYEINDEX (uint) and SV_ClipDistance/SV_CullDistance.
		const int eye = IndexOf(a_signature, "EYEINDEX", 0);
		if (eye >= 0 && static_cast<D3D_REGISTER_COMPONENT_TYPE>(a_signature.elements[eye].componentType) ==
							D3D_REGISTER_COMPONENT_UINT32) {
			out.eye = eye;
			out.clipDistance = IndexOf(a_signature, "SV_ClipDistance", 0);
			out.cullDistance = IndexOf(a_signature, "SV_CullDistance", 0);
		}

		return out;
	}

	namespace
	{
		// The clip and cull distance of VR's stereo split cannot be hull-shader control
		// point inputs. The hull stage reads the vertex shader's output without them (a
		// stage may read a prefix of the previous one's registers), and the domain stage
		// writes them afresh for the tessellated vertex.
		bool InControlPoint(const Layout& a_layout, size_t a_index)
		{
			const int index = static_cast<int>(a_index);
			return !a_layout.Stereo() || (index != a_layout.clipDistance && index != a_layout.cullDistance);
		}

		std::string EmitStruct(const Reflection::Signature& a_signature, const Layout& a_layout)
		{
			const auto members = [&](bool a_controlPoint) {
				std::string body;
				for (size_t i = 0; i < a_signature.elements.size(); ++i) {
					if (a_controlPoint && !InControlPoint(a_layout, i)) {
						continue;
					}
					const auto&       e = a_signature.elements[i];
					const std::string semantic =
						_stricmp(e.semanticName.c_str(), "SV_POSITION") == 0 ? "SV_POSITION" : e.Semantic();
					body += std::format("\t{} f{} : {};\n", e.hlslType, i, semantic);
				}
				return body;
			};

			std::string out = "struct CP\n{\n" + members(true) + "};\n\n";
			out += a_layout.Stereo() ? "struct DSOut\n{\n" + members(false) + "};\n\n" :
			                           "typedef CP DSOut;\n\n";
			return out;
		}

		// World position of a control point or domain vertex, camera-relative. The
		// landscape shader hands it to us directly; anything else is reconstructed from
		// clip space through the engine's inverse view-projection.
		std::string EmitWorldAccess(const Layout& a_layout)
		{
			const auto one = [&](const char* a_type) {
				if (a_layout.world >= 0) {
					return std::format(
						"float3 WorldOf({} p)\n"
						"{{\n"
						"\treturn p.f{}.xyz;\n"
						"}}\n\n",
						a_type, a_layout.world);
				}
				return std::format(
					"float3 WorldOf({} p)\n"
					"{{\n"
					"\treturn ReconstructWorld(p.f{});\n"
					"}}\n\n",
					a_type, std::max(a_layout.clip, 0));
			};

			std::string out = one("CP");
			if (a_layout.Stereo()) {
				out += one("DSOut");

				// What VR's landscape vertex shader does with an eye's clip position in stereo:
				// x squeezed into the eye's half of the double-wide target, and the halves
				// split by a clip and cull distance of -x + w (left eye) or x + w (right eye).
				// Mono (the view through a scope) keeps x and a distance of 1.
				out +=
					"float4 StereoClip(float3 world, out float split)\n"
					"{\n"
					"\tconst float4 eyeClip = mul(CameraViewProj, float4(world, 1.0f));\n"
					"\tconst float  stereo  = StereoFlags.x;\n"
					"\tconst float  side    = gEye == 0 ? 1.0f : -1.0f;\n"
					"\tsplit = stereo > 0.0f ? -side * eyeClip.x + eyeClip.w : 1.0f;\n"
					"\treturn float4(eyeClip.x * (2.0f - stereo) * 0.5f - eyeClip.w * 0.5f * side * stereo,\n"
					"\t              eyeClip.y, eyeClip.z, eyeClip.w);\n"
					"}\n\n";
			}
			return out;
		}

		// Whether the domain stage darkens burnt ground: the landscape with the clipmap and
		// a vertex colour to darken.
		bool WantsScorch(Mode a_mode, const Layout& a_layout)
		{
			return a_mode == Mode::kLandscape && Settings::useClipmap && Settings::enableScorch &&
			       a_layout.colour >= 0;
		}

		// Whether the snow blanket raises this mode's surface (the domain stage's SnowRaise,
		// and the hull stage's extra detail where the coverage map has snow).
		bool RaisesSnow(Mode a_mode)
		{
			return a_mode == Mode::kLandscape && Settings::useClipmap && Settings::enableSnowRaise &&
			       Settings::snowRaiseHeight > 0.0f;
		}

		// Whether the domain stage shows the ground where heat melted the snow off: the snow
		// blanket, the per-draw layer colours, the layer weights and a vertex colour to tint.
		bool WantsThaw(Mode a_mode, const Layout& a_layout)
		{
			return RaisesSnow(a_mode) && Settings::meltShowsGround && Settings::enableSurfaceMaterial &&
			       a_layout.colour >= 0 && a_layout.weights >= 0;
		}

		// Where the snow counts as gone, as the share of the blanket melted: the tint starts
		// at kThawStart and is full by kThawFull (a fire melts HeatMeltDepthScale, all of it;
		// footprints reach half).
		constexpr float kThawStart = 0.45f;
		constexpr float kThawFull = 0.75f;

		std::string EmitPrologue(bool a_displace, Mode a_mode, const Layout& a_layout)
		{
			// Fallout 4's cb12 (per frame): view rows c0-c2, view-projection c8-c11,
			// inverse view-projection c16-c19, camera position adjust c35
			// (BSGraphics::Renderer::SetPerFrameConstants).
			//
			// VR fills it for both eyes: view rows of eye 0 at c0 (the landscape vertex shader
			// builds both eyes' tangent frames with them), view-projection per eye at c12/c16,
			// inverse per eye at c24/c28, position adjust per eye at c59/c60; cb8 c0.x is 1
			// when both eyes are drawn by one instanced call into a double-wide target. Every
			// use goes through gEye, which the hull and domain stages set from EYEINDEX.
			std::string out;
			if (a_layout.Stereo()) {
				out =
					"cbuffer PerFrame : register(b12)\n"
					"{\n"
					"\trow_major float3x4 CameraView                  : packoffset(c0);\n"
					"\trow_major float4x4 CameraViewProjEye[2]        : packoffset(c12);\n"
					"\trow_major float4x4 CameraViewProjInverseEye[2] : packoffset(c24);\n"
					"\tfloat4             CameraPosAdjustEye[2]       : packoffset(c59);\n"
					"};\n\n"
					"cbuffer StereoState : register(b8)\n"
					"{\n"
					"\tfloat4 StereoFlags : packoffset(c0);\n"
					"};\n\n"
					"static uint gEye = 0;\n"
					"#define CameraViewProj        (CameraViewProjEye[gEye])\n"
					"#define CameraViewProjInverse (CameraViewProjInverseEye[gEye])\n"
					"#define CameraPosAdjust       (CameraPosAdjustEye[gEye])\n\n";
			} else {
				out =
					"cbuffer PerFrame : register(b12)\n"
					"{\n"
					"\trow_major float3x4 CameraView            : packoffset(c0);\n"
					"\trow_major float4x4 CameraViewProj        : packoffset(c8);\n"
					"\trow_major float4x4 CameraViewProjInverse : packoffset(c16);\n"
					"\tfloat4             CameraPosAdjust       : packoffset(c35);\n"
					"};\n\n";
			}

			out += std::format(
				"static const float kMaxTess       = {:.1f}f;\n"
				"static const float kTargetSpacing = {:.4f}f;\n"
				"static const float kRaiseSpacing  = {:.4f}f;\n"
				"static const float kLift          = {:.6f}f;\n"
				"static const float kWaveAmp       = {:.6f}f;\n"
				"static const float kWaveLen       = {:.4f}f;\n"
				"static const float kGradEps       = {:.4f}f;\n"
				"static const float kGradFlat      = 1.0e-4f;\n\n",
				Settings::tessellationMaxFactor, Settings::tessellationTargetSpacing,
				Settings::tessellationRaiseSpacing, Settings::debugWorldZOffset,
				Settings::debugWaveAmplitude,
				Settings::debugWaveLength, Settings::normalGradientEpsilon);

			const bool blanket = RaisesSnow(a_mode);
			out += std::format("static const float kBlanketSpacing = {:.4f}f;\n",
				blanket ? Settings::tessellationBlanketSpacing : 0.0f);
			out += BlanketTessellation::source;

			if (Settings::enableSurfaceMaterial) {
				out += std::format(
					"cbuffer SurfaceMaterial : register(b{})\n"
					"{{\n"
					"\t// x reveal layer, y full-blend depth, z snow layer, w strength\n"
					"\tfloat4 Material;\n"
					"\t// per layer: rgb the colour bared ground shows, a 1 for a snow layer\n"
					"\tfloat4 Thaw[4];\n"
					"}};\n\n",
					kMaterialSlot);
			}

			out +=
				"float3 ReconstructWorld(float4 clipPosition)\n"
				"{\n"
				"\tfloat4 world = mul(CameraViewProjInverse, clipPosition);\n"
				"\treturn world.xyz / world.w;\n"
				"}\n\n";

			out += EmitWorldAccess(a_layout);

			if (a_displace) {
				const bool field = a_mode == Mode::kLandscape;

				const char* const lift = "\tfloat  d  = kLift;\n";

				if (Settings::useClipmap && field) {
					const bool coarse = Clipmap::ActiveLevels() > 1;

					out += std::format(
						"Texture2D<float> ClipmapHeight : register(t0);\n"
						"SamplerState     ClipmapSampler : register(s0);\n\n"
						"cbuffer ClipmapWindow : register(b{})\n"
						"{{\n"
						"\t// xy window centre (world), z fade start, w fade end\n"
						"\tfloat4 Window;\n"
						"\t// x weather scale on the snow raise, y absolute field displacement bound\n"
						"\tfloat4 Raise;\n"
						"{}"
						"}};\n\n"
						"static const float kClipmapWorldSize = {:.4f}f;\n\n",
						Clipmap::kParamsSlot,
						coarse ? "\t// the coarse level's window, same layout\n"
						         "\tfloat4 Window1;\n" :
						         "",
						Clipmap::kWorldSize);

					if (coarse) {
						out += std::format(
							"Texture2D<float> ClipmapHeight1 : register(t{});\n"
							"static const float kClipmapWorldSize1 = {:.4f}f;\n\n",
							Clipmap::kLevel1Slot, Clipmap::WorldSizeFor(1));
					}

					if (WantsScorch(a_mode, a_layout) || WantsThaw(a_mode, a_layout)) {
						// The scorch is in the top 15 bits of level 0's metadata and the heat
						// flag in bit 15 (see ClipmapUpdateCS.h); texel i is the centre of world
						// cell i, as for the height, and the texels wrap.
						out += std::format(
							"Texture2D<uint> ClipmapMeta : register(t{0});\n"
							"static const float kScorchCell = {1:.6f}f;\n"
							"static const int   kScorchMask = {2};\n\n"
							"float ScorchTexel(int2 c)\n"
							"{{\n"
							"\treturn (float)(ClipmapMeta.Load(int3(c & kScorchMask, 0)) >> 17u) * (1.0f / 32767.0f);\n"
							"}}\n\n"
							"// How burnt the ground is at a camera-relative world position, 0 to 1.\n"
							"float ScorchAt(float3 worldPosition)\n"
							"{{\n"
							"\tconst float2 xy = worldPosition.xy + CameraPosAdjust.xy;\n"
							"\tconst float2 fromCentre = abs(xy - Window.xy);\n"
							"\tconst float  inWindow = 1.0f - saturate(\n"
							"\t\t(max(fromCentre.x, fromCentre.y) - Window.z) / max(Window.w - Window.z, 1e-3f));\n"
							"\tif (inWindow <= 0.0f) {{\n"
							"\t\treturn 0.0f;\n"
							"\t}}\n"
							"\tconst float2 t = xy / kScorchCell - 0.5f;\n"
							"\tconst float2 i = floor(t);\n"
							"\tconst float2 f = t - i;\n"
							"\tconst int2   c = int2(i);\n"
							"\tconst float  s = lerp(lerp(ScorchTexel(c), ScorchTexel(c + int2(1, 0)), f.x),\n"
							"\t                      lerp(ScorchTexel(c + int2(0, 1)), ScorchTexel(c + int2(1, 1)), f.x), f.y);\n"
							"\treturn s * inWindow;\n"
							"}}\n\n",
							Clipmap::kScorchSlot, Clipmap::CellSizeFor(0),
							static_cast<int>(Clipmap::kTexels) - 1);

						out +=
							"float HeatTexel(int2 c)\n"
							"{\n"
							"\treturn (float)((ClipmapMeta.Load(int3(c & kScorchMask, 0)) >> 15u) & 1u);\n"
							"}\n\n"
							"// How much of the ground at a camera-relative world position heat bared, 0 to 1.\n"
							"float HeatAt(float3 worldPosition)\n"
							"{\n"
							"\tconst float2 xy = worldPosition.xy + CameraPosAdjust.xy;\n"
							"\tconst float2 fromCentre = abs(xy - Window.xy);\n"
							"\tconst float  inWindow = 1.0f - saturate(\n"
							"\t\t(max(fromCentre.x, fromCentre.y) - Window.z) / max(Window.w - Window.z, 1e-3f));\n"
							"\tif (inWindow <= 0.0f) {\n"
							"\t\treturn 0.0f;\n"
							"\t}\n"
							"\tconst float2 t = xy / kScorchCell - 0.5f;\n"
							"\tconst float2 i = floor(t);\n"
							"\tconst float2 f = t - i;\n"
							"\tconst int2   c = int2(i);\n"
							"\tconst float  s = lerp(lerp(HeatTexel(c), HeatTexel(c + int2(1, 0)), f.x),\n"
							"\t                      lerp(HeatTexel(c + int2(0, 1)), HeatTexel(c + int2(1, 1)), f.x), f.y);\n"
							"\treturn s * inWindow;\n"
							"}\n\n"
							"// Value noise over patches about 48 units across: wetter and drier thawed ground.\n"
							"float ThawNoiseAt(int2 c)\n"
							"{\n"
							"\tuint h = (uint)c.x * 73856093u ^ (uint)c.y * 19349663u;\n"
							"\th = h * 1664525u + 1013904223u;\n"
							"\th ^= h >> 16;\n"
							"\treturn (float)(h & 0xFFFFu) * (1.0f / 65535.0f);\n"
							"}\n\n"
							"float ThawNoise(float2 xy)\n"
							"{\n"
							"\tconst float2 t = xy / 48.0f;\n"
							"\tconst float2 i = floor(t);\n"
							"\tfloat2 f = t - i;\n"
							"\tf = f * f * (3.0f - 2.0f * f);\n"
							"\tconst int2 c = int2(i);\n"
							"\treturn lerp(lerp(ThawNoiseAt(c), ThawNoiseAt(c + int2(1, 0)), f.x),\n"
							"\t            lerp(ThawNoiseAt(c + int2(0, 1)), ThawNoiseAt(c + int2(1, 1)), f.x), f.y);\n"
							"}\n\n";
					}
				}

				const bool raising = RaisesSnow(a_mode);

				if (raising) {
					out += std::format(
						"Texture2D<float> SnowCoverageMap : register(t{0});\n"
						"SamplerState     SnowCoverageSampler : register(s{1});\n\n"
						"static const float kCoverageWorldSize = {2:.4f}f;\n"
						"static const float kCoverageTexels    = {6:.1f}f;\n"
						"static const float kRaiseHeight       = {3:.4f}f;\n"
						"static const float kRaiseFadeEnd      = {4:.4f}f;\n"
						"static const float kRaiseFadeStart    = {5:.4f}f;\n\n",
						SnowCoverage::kSlot, SnowCoverage::kSamplerSlot,
						SnowCoverage::kWorldSize, Settings::snowRaiseHeight,
						Settings::snowRaiseDistance,
						std::max(Settings::snowRaiseDistance - Settings::snowRaiseFadeBand, 0.0f),
						static_cast<float>(SnowCoverage::kTexels));

					if (Settings::shelterMeshCap) {
						out += std::format(
							"Texture2D<float> SnowMeshCapMap : register(t{0});\n"
							"SamplerState     SnowMeshCapSampler : register(s{1});\n\n"
							"static const float kMeshCapWorldSize = {2:.4f}f;\n"
							"static const float kMeshCapTexels    = {3:.1f}f;\n"
							"static const float kMeshCapFadeStart = {4:.4f}f;\n"
							"static const float kMeshCapFadeEnd   = {5:.4f}f;\n\n",
							Shelter::kSlot, Shelter::kSamplerSlot, Shelter::kWorldSize,
							static_cast<float>(Shelter::kTexels),
							Shelter::kWorldSize * 0.39f,
							Shelter::kWorldSize * 0.47f);
					}
				}

				out += "float SnowRaise(float3 worldPosition)\n{\n";

				if (!raising) {
					out += "\treturn 0.0f;\n}\n\n";
				} else {
					out +=
						"\tconst float2 xy = worldPosition.xy + CameraPosAdjust.xy;\n\n"
						"\tconst float2 fromCentre = abs(xy - Window.xy);\n"
						"\tconst float  reach      = max(fromCentre.x, fromCentre.y);\n"
						"\tconst float  fade       = 1.0f - saturate(\n"
						"\t\t(reach - kRaiseFadeStart) /\n"
						"\t\tmax(kRaiseFadeEnd - kRaiseFadeStart, 1e-3f));\n"
						"\tif (fade <= 0.0f) {\n"
						"\t\treturn 0.0f;\n"
						"\t}\n\n"
						"\tfloat2 uv = xy / kCoverageWorldSize;\n"
						"\tconst float2 t = uv * kCoverageTexels + 0.5f;\n"
						"\tconst float2 i = floor(t);\n"
						"\tfloat2 f = t - i;\n"
						"\tf = f * f * (3.0f - 2.0f * f);\n"
						"\tuv = (i + f - 0.5f) / kCoverageTexels;\n\n"
						"\tconst float cover = SnowCoverageMap.SampleLevel(\n"
						"\t\tSnowCoverageSampler, uv, 0.0f);\n\n"
						"\tconst float snowy = smoothstep(0.5f, 1.0f, cover);\n"
						"\tfloat lift = snowy * kRaiseHeight * Raise.x;\n\n";

					if (Settings::shelterMeshCap) {
						out +=
							"\tfloat2 cuv = xy / kMeshCapWorldSize;\n"
							"\tconst float2 ct = cuv * kMeshCapTexels + 0.5f;\n"
							"\tconst float2 ci = floor(ct);\n"
							"\tfloat2 cf = ct - ci;\n"
							"\tcf = cf * cf * (3.0f - 2.0f * cf);\n"
							"\tcuv = (ci + cf - 0.5f) / kMeshCapTexels;\n\n"
							"\tfloat cap = SnowMeshCapMap.SampleLevel(\n"
							"\t\tSnowMeshCapSampler, cuv, 0.0f);\n\n"
							"\tcap = lerp(cap, 1.0f, saturate(\n"
							"\t\t(reach - kMeshCapFadeStart) /\n"
							"\t\tmax(kMeshCapFadeEnd - kMeshCapFadeStart, 1e-3f)));\n\n"
							"\tlift = min(lift, cap * kRaiseHeight);\n\n";
					}

					out +=
						"\treturn lift * fade;\n"
						"}\n\n";
				}

				out +=
					"// Height of the deformation field at a camera-relative world position.\n"
					"float Displacement(float3 worldPosition)\n"
					"{\n"
					"\tfloat2 xy = worldPosition.xy + CameraPosAdjust.xy;\n";
				out += lift;

				if (Settings::useClipmap && field) {
					out +=
						"\tconst float2 fromCentre = abs(xy - Window.xy);\n"
						"\tconst float  edgeDist   = max(fromCentre.x, fromCentre.y);\n"
						"\tconst float  inWindow   = 1.0f - saturate(\n"
						"\t\t(edgeDist - Window.z) / max(Window.w - Window.z, 1e-3f));\n\n";

					if (Clipmap::ActiveLevels() > 1) {
						out +=
							"\tconst float2 fromCentre1 = abs(xy - Window1.xy);\n"
							"\tconst float  edgeDist1   = max(fromCentre1.x, fromCentre1.y);\n"
							"\tconst float  inWindow1   = 1.0f - saturate(\n"
							"\t\t(edgeDist1 - Window1.z) / max(Window1.w - Window1.z, 1e-3f));\n\n"
							"\tif (inWindow1 > 0.0f) {\n"
							"\t\tconst float fine = ClipmapHeight.SampleLevel(ClipmapSampler,\n"
							"\t\t                                             xy / kClipmapWorldSize, 0.0f);\n"
							"\t\tconst float coarse = ClipmapHeight1.SampleLevel(ClipmapSampler,\n"
							"\t\t                                                xy / kClipmapWorldSize1, 0.0f);\n"
							"\t\td += lerp(coarse, fine, inWindow) * inWindow1;\n"
							"\t}\n";
					} else {
						out +=
							"\tif (inWindow > 0.0f) {\n"
							"\t\td += ClipmapHeight.SampleLevel(ClipmapSampler,\n"
							"\t\t                               xy / kClipmapWorldSize, 0.0f) * inWindow;\n"
							"\t}\n";
					}
				} else if (field) {
					out +=
						"\tif (kWaveAmp != 0.0f) {\n"
						"\t\tconst float k = 6.2831853f / kWaveLen;\n"
						"\t\td += kWaveAmp * sin(xy.x * k) * sin(xy.y * k);\n"
						"\t}\n";
				}

				out +=
					"\treturn d;\n"
					"}\n\n"
					"float SurfaceOffset(float3 worldPosition)\n"
					"{\n"
					"\treturn Displacement(worldPosition) + SnowRaise(worldPosition);\n"
					"}\n\n";
			}

			return out;
		}

		std::string EmitPatchConstants(bool a_tessellate, const Layout& a_layout)
		{
			std::string out =
				"struct PatchConstants\n"
				"{\n"
				"\tfloat edges[3] : SV_TessFactor;\n"
				"\tfloat inside   : SV_InsideTessFactor;\n"
				"};\n\n";

			if (!a_tessellate) {
				out +=
					"PatchConstants PatchConstantFn(InputPatch<CP, 3> patch)\n"
					"{\n"
					"\tPatchConstants o;\n"
					"\to.edges[0] = 1.0f;\n"
					"\to.edges[1] = 1.0f;\n"
					"\to.edges[2] = 1.0f;\n"
					"\to.inside   = 1.0f;\n"
					"\treturn o;\n"
					"}\n\n";
				return out;
			}

			const bool bounded = Settings::useClipmap && Settings::enableTessellationBounds;
			const bool coarse = Settings::useClipmap && Clipmap::ActiveLevels() > 1;
			const bool raising = RaisesSnow(Mode::kLandscape);

			if (bounded) {
				out += std::format(
					"Texture2D<uint> FieldActivity : register(t{});\n"
					"{}"
					"\n"
					"static const float kActivityCell  = {:.6f}f;\n"
					"static const float kActivityCell1 = {:.6f}f;\n"
					"static const int   kActivityMask  = {};\n"
					"static const float kBoundDepth    = {:.4f}f;\n\n"
					"float ActivityAt(Texture2D<uint> map, float cellSize, float2 xy)\n"
					"{{\n"
					"\tconst int2 cell = int2(floor(xy / cellSize)) & kActivityMask;\n"
					"\treturn asfloat(map.Load(int3(cell, 0)));\n"
					"}}\n\n"
					"// The largest mark near an edge. The points are the same whichever way round\n"
					"// the edge is given, so the two patches sharing it agree on its factor.\n"
					"float EdgeBound(Texture2D<uint> map, float cellSize, float2 a, float2 b)\n"
					"{{\n"
					"\tfloat m = max(ActivityAt(map, cellSize, a), ActivityAt(map, cellSize, b));\n"
					"\tm = max(m, ActivityAt(map, cellSize, (a + b) * 0.5f));\n"
					"\tm = max(m, ActivityAt(map, cellSize, a * 0.75f + b * 0.25f));\n"
					"\tm = max(m, ActivityAt(map, cellSize, a * 0.25f + b * 0.75f));\n"
					"\treturn m;\n"
					"}}\n\n",
					Clipmap::kActivitySlot,
					coarse ? std::format("Texture2D<uint> FieldActivity1 : register(t{});\n",
								 Clipmap::kActivity1Slot) :
							 std::string{},
					Clipmap::CellSizeFor(0) * static_cast<float>(Clipmap::kActivityRatio),
					Clipmap::CellSizeFor(1) * static_cast<float>(Clipmap::kActivityRatio),
					Clipmap::kActivityTexels - 1,
					std::max(Settings::tessellationBoundDepth, 1e-4f));
			}

			if (raising) {
				// The blanket only needs vertices where there is snow to raise: an edge is
				// snowy when the coverage map has snow at either end or the middle, within
				// the raise's reach and with a non-zero weather scale.
				out += std::format(
					"Texture2D<float> HullCoverage : register(t{});\n\n"
					"float SnowyEdge(float2 a, float2 b, float2 mid)\n"
					"{{\n"
					"\tconst float2 from  = abs(mid - Window.xy);\n"
					"\tif (Raise.x <= 0.0f || max(from.x, from.y) > kRaiseFadeEnd) {{\n"
					"\t\treturn 0.0f;\n"
					"\t}}\n"
					"\tfloat c = max(HullCoverage.SampleLevel(ClipmapSampler, a / kCoverageWorldSize, 0.0f),\n"
					"\t              HullCoverage.SampleLevel(ClipmapSampler, b / kCoverageWorldSize, 0.0f));\n"
					"\tc = max(c, HullCoverage.SampleLevel(ClipmapSampler, mid / kCoverageWorldSize, 0.0f));\n"
					"\treturn c > 0.25f ? 1.0f : 0.0f;\n"
					"}}\n\n",
					kHullCoverageSlot);
			}

			if (Settings::useClipmap) {
				out += std::format(
					"float EdgeFactor(float3 a, float3 b)\n"
					"{{\n"
					"\t// World positions on a 1/16-unit grid: both patches on an edge, and both\n"
					"\t// landscape draws on a quadrant border, then compute the same factor.\n"
					"\tconst float3 wa = round((a + CameraPosAdjust.xyz) * 16.0f) * 0.0625f;\n"
					"\tconst float3 wb = round((b + CameraPosAdjust.xyz) * 16.0f) * 0.0625f;\n"
					"\tconst float2 mid = (wa.xy + wb.xy) * 0.5f;\n\n"
					"\tconst float length2 = length(wb - wa);\n"
					"\tconst float near    =\n"
					"\t\tclamp(length2 / kTargetSpacing, 1.0f, kMaxTess);\n"
					"\tconst float snowy   = {};\n"
					"\tconst float far     = kRaiseSpacing > 0.0f ?\n"
					"\t\tlerp(1.0f, clamp(length2 / kRaiseSpacing, 1.0f, kMaxTess), snowy) : 1.0f;\n\n"
					"\tconst float2 from = abs(mid - Window.xy);\n"
					"\tconst float  reach = max(from.x, from.y);\n\n"
					"\tconst float away = saturate(\n"
					"\t\t(reach - Window.z) / max(Window.w - Window.z, 1e-3f));\n\n",
					raising ? "SnowyEdge(wa.xy, wb.xy, mid)" : "0.0f");

				if (bounded) {
					out +=
						"\tconst float active = saturate(\n"
						"\t\tEdgeBound(FieldActivity, kActivityCell, wa.xy, wb.xy) / kBoundDepth);\n"
						"\tconst float gated = BlanketDetail(length2, far, near,\n"
						"\t\tsnowy > 0.0f ? kBlanketSpacing : 0.0f, kMaxTess, active);\n\n";
				}

				const char* const nearTerm = bounded ? "gated" : "near";

				if (coarse) {
					out += std::format(
						"\tconst float mid1    =\n"
						"\t\tclamp(length2 / (kTargetSpacing * {0:.1f}f), 1.0f, kMaxTess);\n",
						Clipmap::kLevelScale);

					// The coarse ring only needs its detail where the coarse level holds marks.
					out += bounded ?
					           "\tconst float active1 = saturate(\n"
					           "\t\tEdgeBound(FieldActivity1, kActivityCell1, wa.xy, wb.xy) / kBoundDepth);\n"
					           "\tconst float ring = lerp(far, max(far, mid1), active1);\n\n" :
					           "\tconst float ring = mid1;\n\n";

					out += std::format(
						"\tconst float2 from1  = abs(mid - Window1.xy);\n"
						"\tconst float  reach1 = max(from1.x, from1.y);\n"
						"\tconst float  away1  = saturate(\n"
						"\t\t(reach1 - Window1.z) / max(Window1.w - Window1.z, 1e-3f));\n\n"
						"\treturn lerp(lerp({0}, ring, away), far, away1);\n"
						"}}\n\n",
						nearTerm);
				} else {
					out += std::format(
						"\treturn lerp({}, far, away);\n"
						"}}\n\n",
						nearTerm);
				}
			} else {
				out +=
					"float EdgeFactor(float3 a, float3 b)\n"
					"{\n"
					"\treturn clamp(length(b - a) / kTargetSpacing, 1.0f, kMaxTess);\n"
					"}\n\n";
			}

			// Frustum culling: a patch only moves vertically, by at most the blanket plus the
			// deepest mark, so the prism between its corners lifted and lowered by that much
			// holds everything it can become. When all six prism corners lie outside one side
			// plane, or behind the eye, none of it can reach the screen and a zero factor drops
			// it before any vertex is generated. The tests are linear in clip space, so they
			// hold whatever the depth convention.
			const bool culling = Settings::enablePatchCulling;
			if (culling) {
				const float margin = std::max(Settings::snowRaiseHeight, 0.0f) +
				                     std::abs(Settings::debugWorldZOffset) +
				                     std::abs(Settings::debugWaveAmplitude) + 128.0f;
				out += std::format(
					"static const float kCullMargin = {0:.1f}f;\n\n"
					"bool PatchOutside(float3 p0, float3 p1, float3 p2)\n"
					"{{\n"
					"\tconst float margin = kCullMargin{1};\n"
					"\tif (!isfinite(margin)) {{ return false; }}\n"
					"\tconst float3 up = float3(0.0f, 0.0f, margin);\n"
					"\tfloat4 c[6];\n"
					"\tc[0] = mul(CameraViewProj, float4(p0 + up, 1.0f));\n"
					"\tc[1] = mul(CameraViewProj, float4(p0 - up, 1.0f));\n"
					"\tc[2] = mul(CameraViewProj, float4(p1 + up, 1.0f));\n"
					"\tc[3] = mul(CameraViewProj, float4(p1 - up, 1.0f));\n"
					"\tc[4] = mul(CameraViewProj, float4(p2 + up, 1.0f));\n"
					"\tc[5] = mul(CameraViewProj, float4(p2 - up, 1.0f));\n\n"
					"\tuint left = 0, right = 0, below = 0, above = 0, behind = 0;\n"
					"\t[unroll] for (int i = 0; i < 6; ++i) {{\n"
					"\t\tleft   += c[i].x < -c[i].w ? 1u : 0u;\n"
					"\t\tright  += c[i].x >  c[i].w ? 1u : 0u;\n"
					"\t\tbelow  += c[i].y < -c[i].w ? 1u : 0u;\n"
					"\t\tabove  += c[i].y >  c[i].w ? 1u : 0u;\n"
					"\t\tbehind += c[i].w <= 0.0f ? 1u : 0u;\n"
					"\t}}\n"
					"\treturn left == 6u || right == 6u || below == 6u || above == 6u || behind == 6u;\n"
					"}}\n\n",
					margin, Settings::useClipmap ? " + Raise.y" : "");
			}

			out +=
				"PatchConstants PatchConstantFn(InputPatch<CP, 3> patch)\n"
				"{\n"
				"\tPatchConstants o;\n";
			if (a_layout.Stereo()) {
				out += std::format("\tgEye = patch[0].f{} & 1u;\n", a_layout.eye);
			}
			out +=
				"\tconst float3 p0 = WorldOf(patch[0]);\n"
				"\tconst float3 p1 = WorldOf(patch[1]);\n"
				"\tconst float3 p2 = WorldOf(patch[2]);\n\n";

			if (culling) {
				out +=
					"\tif (PatchOutside(p0, p1, p2)) {\n"
					"\t\to.edges[0] = 0.0f;\n"
					"\t\to.edges[1] = 0.0f;\n"
					"\t\to.edges[2] = 0.0f;\n"
					"\t\to.inside   = 0.0f;\n"
					"\t\treturn o;\n"
					"\t}\n\n";
			}

			out +=
				"\to.edges[0] = EdgeFactor(p1, p2);\n"
				"\to.edges[1] = EdgeFactor(p2, p0);\n"
				"\to.edges[2] = EdgeFactor(p0, p1);\n"
				"\to.inside   = max(o.edges[0], max(o.edges[1], o.edges[2]));\n"
				"\treturn o;\n"
				"}\n\n";

			return out;
		}

		std::string EmitHull(const std::string& a_winding)
		{
			return std::format(
				"[domain(\"tri\")]\n"
				"[partitioning(\"integer\")]\n"
				"[outputtopology(\"triangle_{}\")]\n"
				"[outputcontrolpoints(3)]\n"
				"[patchconstantfunc(\"PatchConstantFn\")]\n"
				"CP main(InputPatch<CP, 3> patch, uint id : SV_OutputControlPointID)\n"
				"{{\n"
				"\treturn patch[id];\n"
				"}}\n",
				a_winding);
		}

		// The landscape shader's tangent frame is in VIEW space: each of TEXCOORD0..2 is a
		// row, so a column (x, y or z of all three) is one axis. The field gradient is a
		// world-space quantity, so the frame goes to world through the view rotation,
		// is rebuilt around the new normal there, and comes back.
		std::string EmitNormalRecompute(const Layout& a_layout)
		{
			if (!Settings::recomputeNormals || !a_layout.Landscape()) {
				return {};
			}

			const int r0 = a_layout.tbn[0];
			const int r1 = a_layout.tbn[1];
			const int r2 = a_layout.tbn[2];

			return std::format(
				"\n\t// Rebuild the tangent frame around the displaced surface's normal.\n"
				"\tconst float2 hNeighbour = float2(\n"
				"\t\tSurfaceOffset(basePos + float3(kGradEps, 0.0f, 0.0f)),\n"
				"\t\tSurfaceOffset(basePos + float3(0.0f, kGradEps, 0.0f)));\n"
				"\tconst float2 fieldGrad = (hNeighbour - rise) / kGradEps;\n\n"
				"\tif (dot(fieldGrad, fieldGrad) > kGradFlat * kGradFlat) {{\n"
				"\t\tconst float3 T0 = ViewToWorld(float3(o.f{0}.x, o.f{1}.x, o.f{2}.x));\n"
				"\t\tconst float3 B0 = ViewToWorld(float3(o.f{0}.y, o.f{1}.y, o.f{2}.y));\n"
				"\t\tconst float3 N0 = normalize(ViewToWorld(float3(o.f{0}.z, o.f{1}.z, o.f{2}.z)));\n\n"
				"\t\tconst float nz = (abs(N0.z) < 1e-4f) ? (N0.z >= 0.0f ? 1e-4f : -1e-4f) : N0.z;\n"
				"\t\tconst float2 grad = (-N0.xy / nz) + fieldGrad;\n"
				"\t\tconst float3 N = normalize(float3(-grad, 1.0f)) * (N0.z >= 0.0f ? 1.0f : -1.0f);\n\n"
				"\t\tconst float  handed = (dot(cross(N0, T0), B0) < 0.0f) ? -1.0f : 1.0f;\n"
				"\t\tfloat3 T = T0 - N * dot(N, T0);\n"
				"\t\tT = (dot(T, T) > 1e-8f) ? normalize(T) : normalize(cross(float3(0.0f, 1.0f, 0.0f), N));\n"
				"\t\tconst float3 B = cross(N, T) * handed;\n\n"
				"\t\tconst float3 Tv = WorldToView(T);\n"
				"\t\tconst float3 Bv = WorldToView(B);\n"
				"\t\tconst float3 Nv = WorldToView(N);\n"
				"\t\to.f{0} = float3(Tv.x, Bv.x, Nv.x);\n"
				"\t\to.f{1} = float3(Tv.y, Bv.y, Nv.y);\n"
				"\t\to.f{2} = float3(Tv.z, Bv.z, Nv.z);\n"
				"\t}}\n",
				r0, r1, r2);
		}

		// Fallout 4 blends four layers per landscape draw, with their weights in one float4.
		std::string EmitBlendWeights(const Layout& a_layout)
		{
			if (!Settings::enableSurfaceMaterial || a_layout.weights < 0) {
				return {};
			}

			return std::format(
				"\n\t// Pressed ground shows the layer underneath.\n"
				"\tconst int revealLayer = (int)Material.x;\n"
				"\tconst int snowLayer   = (int)Material.z;\n"
				"\tif (revealLayer >= 0) {{\n"
				"\t\tconst float4 w = o.f{0};\n"
				"\t\tconst float  total = dot(w, 1.0f);\n"
				"\t\tconst float4 snowSel = float4(\n"
				"\t\t\tsnowLayer == 0 ? 1.0f : 0.0f, snowLayer == 1 ? 1.0f : 0.0f,\n"
				"\t\t\tsnowLayer == 2 ? 1.0f : 0.0f, snowLayer == 3 ? 1.0f : 0.0f);\n"
				"\t\tconst float snowFrac =\n"
				"\t\t\tsnowLayer >= 0 ? saturate(dot(w, snowSel) / max(total, 1e-4f)) : 1.0f;\n\n"
				"\t\tconst float blendAmount =\n"
				"\t\t\tsaturate(-shift / Material.y) * Material.w * snowFrac;\n"
				"\t\tconst float4 target = float4(\n"
				"\t\t\trevealLayer == 0 ? 1.0f : 0.0f, revealLayer == 1 ? 1.0f : 0.0f,\n"
				"\t\t\trevealLayer == 2 ? 1.0f : 0.0f, revealLayer == 3 ? 1.0f : 0.0f);\n"
				"\t\to.f{0} = lerp(w, target * total, blendAmount);\n"
				"\t}}\n",
				a_layout.weights);
		}

		// Burnt ground: the vertex colour darkened towards a warm charcoal.
		std::string EmitScorch(Mode a_mode, const Layout& a_layout)
		{
			if (!WantsScorch(a_mode, a_layout)) {
				return {};
			}

			const float keep = 1.0f - std::clamp(Settings::scorchDarkness, 0.0f, 1.0f);
			return std::format(
				"\n\t// Burnt ground (a jet's trail) is darkened.\n"
				"\tconst float scorched = saturate(ScorchAt(basePos));\n"
				"\tif (scorched > 0.0f) {{\n"
				"\t\to.f{0}.rgb *= lerp(1.0f.xxx, float3({1:.4f}f, {2:.4f}f, {3:.4f}f), scorched);\n"
				"\t}}\n",
				a_layout.colour, keep, keep * 0.9f, keep * 0.8f);
		}

		// Ground heat melted the snow off: a winter mod's white repaint has no bare version to
		// reveal, so the vertex colour tints each snow layer to its own ground's colour, as far
		// as the blanket is gone.
		std::string EmitThaw(Mode a_mode, const Layout& a_layout)
		{
			if (!WantsThaw(a_mode, a_layout)) {
				return {};
			}

			return std::format(
				"\n\t// Snow melted away by heat shows the ground beneath, coloured per layer.\n"
				"\tconst float snowLift = rise - shift;\n"
				"\tif (snowLift > 1.0f && shift < 0.0f) {{\n"
				"\t\tconst float thaw = HeatAt(basePos) *\n"
				"\t\t\tsmoothstep({2:.3f}f, {3:.3f}f, saturate(-shift / snowLift));\n"
				"\t\tif (thaw > 0.0f) {{\n"
				"\t\t\tconst float4 tw = o.f{1};\n"
				"\t\t\tconst float  wetness = 0.8f + 0.4f * ThawNoise(basePos.xy + CameraPosAdjust.xy);\n"
				"\t\t\tfloat3 tint = 0.0f.xxx;\n"
				"\t\t\t[unroll] for (int layer = 0; layer < 4; ++layer) {{\n"
				"\t\t\t\ttint += tw[layer] * lerp(1.0f.xxx, Thaw[layer].rgb * wetness, Thaw[layer].a);\n"
				"\t\t\t}}\n"
				"\t\t\ttint /= max(dot(tw, 1.0f), 1e-4f);\n"
				"\t\t\to.f{0}.rgb *= lerp(1.0f.xxx, tint, thaw);\n"
				"\t\t}}\n"
				"\t}}\n",
				a_layout.colour, a_layout.weights, kThawStart, kThawFull);
		}

		std::string EmitFieldDebug(const Layout& a_layout)
		{
			const int mode = Settings::debugFieldColour;
			if (mode <= 0) {
				return {};
			}

			const float scale = std::max(Settings::stampDepth, 1.0f);

			std::string out = std::format(
				"\n\t// Field debug: red where the ground went down, green where it rose.\n"
				"\tconst float kDebugScale = {:.4f}f;\n"
				"\tconst float kDebugFlat  = {:.4f}f;\n"
				"\tconst float debugDepth  = saturate(abs(shift) / kDebugScale);\n",
				scale, scale * 0.02f);

			if (mode == 3) {
				if (a_layout.weights < 0) {
					return {};
				}

				out += std::format(
					"\tif (shift < -kDebugFlat) {{\n"
					"\t\to.f{0} = float4(1.0f, 0.0f, 0.0f, 0.0f);\n"
					"\t}} else if (shift > kDebugFlat) {{\n"
					"\t\to.f{0} = float4(0.0f, 1.0f, 0.0f, 0.0f);\n"
					"\t}}\n",
					a_layout.weights);
				return out;
			}

			if (a_layout.colour < 0) {
				return {};
			}

			out += mode == 1 ?
			           "\tconst float debugAmount = 0.35f + 0.65f * debugDepth;\n" :
			           "\tconst float debugAmount = 1.0f;\n";

			out += std::format(
				"\tif (shift < -kDebugFlat) {{\n"
				"\t\to.f{0}.rgb *= float3(1.0f, 1.0f - debugAmount, 1.0f - debugAmount);\n"
				"\t}} else if (shift > kDebugFlat) {{\n"
				"\t\to.f{0}.rgb *= float3(1.0f - debugAmount, 1.0f, 1.0f - debugAmount);\n"
				"\t}}\n",
				a_layout.colour);

			return out;
		}

		std::string EmitDomain(const Reflection::Signature& a_signature, const Layout& a_layout,
			bool a_wantDisplace, Mode a_mode)
		{
			const bool displace = a_wantDisplace && a_layout.clip >= 0;

			std::string out;
			out +=
				"float3 ViewToWorld(float3 v)\n"
				"{\n"
				"\treturn CameraView[0].xyz * v.x + CameraView[1].xyz * v.y + CameraView[2].xyz * v.z;\n"
				"}\n\n"
				"float3 WorldToView(float3 w)\n"
				"{\n"
				"\treturn float3(dot(CameraView[0].xyz, w), dot(CameraView[1].xyz, w), dot(CameraView[2].xyz, w));\n"
				"}\n\n"
				"[domain(\"tri\")]\n"
				"DSOut main(PatchConstants pc, float3 bary : SV_DomainLocation,\n"
				"           const OutputPatch<CP, 3> patch)\n"
				"{\n"
				"\tDSOut o;\n";

			for (size_t i = 0; i < a_signature.elements.size(); ++i) {
				if (!InControlPoint(a_layout, i)) {
					continue;  // the stereo split, written below
				}
				const auto& e = a_signature.elements[i];

				const bool isFloat =
					static_cast<D3D_REGISTER_COMPONENT_TYPE>(e.componentType) ==
					D3D_REGISTER_COMPONENT_FLOAT32;

				if (isFloat) {
					out += std::format(
						"\to.f{0} = patch[0].f{0} * bary.x + patch[1].f{0} * bary.y + patch[2].f{0} * bary.z;\n",
						i);
				} else {
					out += std::format("\to.f{0} = patch[0].f{0};\n", i);
				}
			}

			if (a_layout.Stereo()) {
				out += std::format("\n\tgEye = o.f{} & 1u;\n", a_layout.eye);
			}

			if (a_layout.Stereo()) {
				// VR: the eye's clip position is rebuilt from the (displaced) world position and
				// put through the vertex shader's stereo projection, which also yields the split
				// distances the rasteriser needs for this vertex.
				if (displace) {
					out +=
						"\n\tconst float3 basePos = WorldOf(o);\n"
						"\tconst float  shift   = Displacement(basePos);\n"
						"\tconst float  rise    = shift + SnowRaise(basePos);\n";
				}
				out += std::format(
					"\n\tfloat split;\n"
					"\to.f{0} = StereoClip(WorldOf(o){1}, split);\n",
					a_layout.clip, displace ? " + float3(0.0f, 0.0f, rise)" : "");
				if (a_layout.clipDistance >= 0) {
					out += std::format("\to.f{} = split;\n", a_layout.clipDistance);
				}
				if (a_layout.cullDistance >= 0) {
					out += std::format("\to.f{} = split;\n", a_layout.cullDistance);
				}
			} else if (displace) {
				out += std::format(
					"\n\tconst float3 basePos = WorldOf(o);\n"
					"\tconst float  shift   = Displacement(basePos);\n"
					"\tconst float  rise    = shift + SnowRaise(basePos);\n\n"
					"\to.f{0} += mul(CameraViewProj, float4(0.0f, 0.0f, rise, 0.0f));\n",
					a_layout.clip);
			}

			if (displace) {

				// Both world positions move with the surface so the motion vectors the pixel
				// shader derives from them describe the displaced geometry.
				if (a_layout.world >= 0) {
					out += std::format("\to.f{}.z += rise;\n", a_layout.world);
				}
				if (a_layout.prevWorld >= 0) {
					out += std::format("\to.f{}.z += rise;\n", a_layout.prevWorld);
				}

				if (a_mode == Mode::kLandscape) {
					out += EmitNormalRecompute(a_layout);
					out += EmitBlendWeights(a_layout);
					out += EmitThaw(a_mode, a_layout);
					out += EmitScorch(a_mode, a_layout);
					out += EmitFieldDebug(a_layout);
				}
			}

			out += "\treturn o;\n}\n";
			return out;
		}

		std::string Common(const Reflection::Signature& a_signature, const Layout& a_layout, Mode a_mode)
		{
			return EmitStruct(a_signature, a_layout) +
			       EmitPrologue(WantsDisplacement(a_mode), a_mode, a_layout) +
			       EmitPatchConstants(WantsSubdivision(a_mode), a_layout);
		}
	}

	std::string HullSource(const Reflection::Signature& a_signature, Mode a_mode)
	{
		const auto layout = LayoutFor(a_signature);
		return Common(a_signature, layout, a_mode) + EmitHull(Settings::tessellationWinding);
	}

	std::string DomainSource(const Reflection::Signature& a_signature, Mode a_mode)
	{
		const auto layout = LayoutFor(a_signature);
		return Common(a_signature, layout, a_mode) +
		       EmitDomain(a_signature, layout, WantsDisplacement(a_mode), a_mode);
	}

	bool WantsDisplacement(Mode a_mode)
	{
		switch (a_mode) {
		case Mode::kLandscape:
			return Settings::debugWorldZOffset != 0.0f ||
			       Settings::debugWaveAmplitude != 0.0f ||
			       (Settings::useClipmap && Clipmap::Ready());
		default:
			return false;
		}
	}

	bool WantsSubdivision(Mode a_mode)
	{
		return a_mode == Mode::kLandscape && WantsDisplacement(a_mode);
	}

#ifndef NMN_OFFLINE
	namespace
	{
		struct Pair
		{
			ID3D11HullShader*                            hs{ nullptr };
			ID3D11DomainShader*                          ds{ nullptr };
			bool                                         failed{ false };
			std::shared_ptr<ShaderCompiler::Job>         pending;
			std::shared_ptr<const Reflection::Signature> signature;
			uint64_t                                     sourceHash{ 0 };
		};

		std::mutex                         g_cacheLock;
		std::unordered_map<uint64_t, Pair> g_caches[static_cast<size_t>(Mode::kCount)];

		// Bumped by Reset; a pointer into the caches is only good for the generation it
		// was found in.
		std::atomic<uint64_t> g_generation{ 1 };

		// Compiled bytecode by source, kept across Reset: saving the INI regenerates every
		// pair, but most of them come out the same and are created again without a compile.
		struct Compiled
		{
			ID3DBlob* hull{ nullptr };
			ID3DBlob* domain{ nullptr };
		};
		std::unordered_map<uint64_t, Compiled> g_compiled;
		constexpr size_t                       kMaxCompiled = 32;

		std::unordered_map<uint64_t, Pair>& CacheFor(Mode a_mode)
		{
			return g_caches[static_cast<size_t>(a_mode)];
		}

		uint64_t SourceHash(const std::string& a_hull, const std::string& a_domain)
		{
			uint64_t hash = 0xcbf29ce484222325ull;
			for (const auto* text : { &a_hull, &a_domain }) {
				for (const char c : *text) {
					hash ^= static_cast<unsigned char>(c);
					hash *= 0x100000001b3ull;
				}
				hash ^= 0xFF;
				hash *= 0x100000001b3ull;
			}
			return hash;
		}

		void Remember(uint64_t a_hash, ID3DBlob* a_hull, ID3DBlob* a_domain)
		{
			if (g_compiled.size() >= kMaxCompiled) {
				for (auto& [hash, blobs] : g_compiled) {
					blobs.hull->Release();
					blobs.domain->Release();
				}
				g_compiled.clear();
			}
			a_hull->AddRef();
			a_domain->AddRef();
			g_compiled[a_hash] = { a_hull, a_domain };
		}

		// Creates the pair's shaders from bytecode and checks the bytecode against the vertex
		// shader it will run behind: the hull stage has to read every element where the
		// vertex shader wrote it, and the domain stage has to write everything the vertex
		// shader did (the pixel shader reads that), each on the same register. Anything else
		// (a replaced landscape shader packing its outputs differently) stays vanilla.
		bool CreatePair(Pair& a_pair, ID3DBlob* a_hull, ID3DBlob* a_domain)
		{
			std::string why;
			if (a_pair.signature) {
				const auto hullInput = Reflection::ReflectInputSignature(
					a_hull->GetBufferPointer(), a_hull->GetBufferSize());
				const auto domainOutput = Reflection::ReflectOutputSignature(
					a_domain->GetBufferPointer(), a_domain->GetBufferSize());
				if (!Reflection::Covers(*a_pair.signature, hullInput, &why)) {
					why = "hull input: " + why;
				} else if (!Reflection::Covers(domainOutput, *a_pair.signature, &why)) {
					why = "domain output: " + why;
				}
			}
			if (!why.empty()) {
				logger::error("Terrain shader pair does not link to the vertex shader ({}) - "
							  "that shader's draws stay vanilla",
					why);
				a_pair.failed = true;
				return false;
			}

			const auto hs = globals::d3d::device->CreateHullShader(a_hull->GetBufferPointer(),
				a_hull->GetBufferSize(), nullptr, &a_pair.hs);
			const auto ds = globals::d3d::device->CreateDomainShader(a_domain->GetBufferPointer(),
				a_domain->GetBufferSize(), nullptr, &a_pair.ds);
			a_pair.failed = FAILED(hs) || FAILED(ds);
			if (a_pair.failed) {
				if (a_pair.hs) {
					a_pair.hs->Release();
					a_pair.hs = nullptr;
				}
				if (a_pair.ds) {
					a_pair.ds->Release();
					a_pair.ds = nullptr;
				}
				logger::error("Terrain shader creation failed (hs={:08X}, ds={:08X})",
					static_cast<uint32_t>(hs), static_cast<uint32_t>(ds));
			}
			return !a_pair.failed;
		}

		bool FinishPair(Pair& a_pair)
		{
			const auto& job = a_pair.pending;
			if (!job || !job->ready.load(std::memory_order_acquire)) {
				return false;
			}
			if (!job->hull || !job->domain || !job->error.empty()) {
				logger::error("Terrain shader compilation failed: {}", job->error);
				a_pair.failed = true;
			} else if (CreatePair(a_pair, job->hull, job->domain)) {
				Remember(a_pair.sourceHash, job->hull, job->domain);
			}
			logger::info("Terrain shader pair: compiler {:.2f} ms, {}", job->milliseconds,
				a_pair.failed ? "failed (vanilla fallback)" : "ready");
			a_pair.pending.reset();
			return !a_pair.failed;
		}

		const Pair* GetOrCreate(const std::shared_ptr<const Reflection::Signature>& a_signature,
			Mode a_mode)
		{
			const std::scoped_lock lock(g_cacheLock);

			auto&          cache = CacheFor(a_mode);
			const uint64_t key = Reflection::Key(*a_signature);

			if (const auto it = cache.find(key); it != cache.end()) {
				return it->second.failed || it->second.pending ? nullptr : &it->second;
			}

			Profiler::Tally(Profiler::Count::kShaderBuilds);

			Pair pair{};
			pair.signature = a_signature;
			if (!globals::Ready() || !a_signature->valid || a_signature->elements.empty()) {
				pair.failed = true;
				cache[key] = std::move(pair);
				return nullptr;
			}

			const auto hsSource = HullSource(*a_signature, a_mode);
			const auto dsSource = DomainSource(*a_signature, a_mode);
			pair.sourceHash = SourceHash(hsSource, dsSource);

			if (Settings::logGeneratedShaders) {
				logger::info("Generated HS for {:016X}:\n{}", key, hsSource);
				logger::info("Generated DS for {:016X}:\n{}", key, dsSource);
			}

			if (const auto known = g_compiled.find(pair.sourceHash); known != g_compiled.end()) {
				CreatePair(pair, known->second.hull, known->second.domain);
				logger::info("Terrain shader pair: reused compiled bytecode, {}",
					pair.failed ? "failed (vanilla fallback)" : "ready");
			} else {
				pair.pending = std::make_shared<ShaderCompiler::Job>();
				pair.pending->hullSource = hsSource;
				pair.pending->domainSource = dsSource;
				if (Settings::asyncShaderCompilation) {
					ShaderCompiler::GetWorker().Submit(pair.pending);
				} else {
					ShaderCompiler::Compile(*pair.pending);
					FinishPair(pair);
				}
			}
			cache[key] = std::move(pair);
			const auto& stored = cache.at(key);
			return stored.failed || stored.pending ? nullptr : &stored;
		}

		// ── the vertex shader's output signature, per bound shader ─────────────

		std::mutex g_signatureLock;
		std::unordered_map<ID3D11VertexShader*, std::shared_ptr<const Reflection::Signature>> g_signatures;

		std::shared_ptr<const Reflection::Signature> SignatureFor(ID3D11VertexShader* a_shader)
		{
			if (!a_shader) {
				return nullptr;
			}

			{
				const std::scoped_lock lock(g_signatureLock);
				if (const auto it = g_signatures.find(a_shader); it != g_signatures.end()) {
					return it->second;
				}
			}

			std::shared_ptr<const Reflection::Signature> result;
			if (const auto chunk = ShaderRegistry::OutputSignature(a_shader); !chunk.empty()) {
				auto signature = Reflection::ParseSignatureChunk(chunk.data(), chunk.size());
				if (signature.valid) {
					result = std::make_shared<const Reflection::Signature>(std::move(signature));
				}
			}

			if (!result) {
				static bool reported = false;
				if (!reported) {
					reported = true;
					logger::warn("A routed draw's vertex shader has no recorded bytecode ({} "
								 "recorded) - it was created before the device hook, so the "
								 "draw is left vanilla. Another mod creating the device first "
								 "would cause this.",
						ShaderRegistry::Count());
				}
			} else {
				const auto layout = LayoutFor(*result);
				logger::info("Routed vertex shader {:p}: {} outputs, landscape layout {}{}",
					static_cast<const void*>(a_shader), result->elements.size(),
					layout.Landscape() ? "recognised" : "NOT recognised (left vanilla)",
					Settings::logDraws ? Reflection::Describe(*result) : std::string{});
			}

			const std::scoped_lock lock(g_signatureLock);
			g_signatures[a_shader] = result;
			return result;
		}

		// What a routed draw needs about its vertex shader, remembered per thread for the
		// shader the last draw used: landscape draws come in long runs under one shader,
		// and after the first the lookups and locks are skipped.
		struct Route
		{
			ID3D11VertexShader* shader{ nullptr };
			uint64_t            generation{ 0 };
			Mode                mode{ Mode::kLandscape };
			const Pair*         pair{ nullptr };
			bool                stereo{ false };
		};

		thread_local Route t_route;

		const Pair* Resolve(ID3D11VertexShader* a_shader, Mode a_mode, bool& a_stereo)
		{
			const uint64_t generation = g_generation.load(std::memory_order_acquire);
			auto&          route = t_route;
			if (route.pair && route.shader == a_shader && route.mode == a_mode &&
				route.generation == generation) {
				a_stereo = route.stereo;
				return route.pair;
			}

			route.pair = nullptr;

			const auto signature = SignatureFor(a_shader);
			if (!signature) {
				return nullptr;
			}

			// Only the landscape layout is post-processed: the domain stage rebuilds the
			// clip position from its world position and keeps its motion vectors and frame
			// in step. Any other signature (a replaced landscape shader) draws vanilla.
			const auto layout = LayoutFor(*signature);
			if (!layout.Landscape()) {
				return nullptr;
			}

			const Pair* pair = GetOrCreate(signature, a_mode);
			if (!pair) {
				return nullptr;
			}

			route = { a_shader, generation, a_mode, pair, layout.Stereo() };
			a_stereo = route.stereo;
			return pair;
		}

		// ── per-draw state ───────────────────────────────────────────────────

		struct MaterialCB
		{
			float material[4]{ -1.0f, 1.0f, -1.0f, 0.0f };
			// Per layer: the colour heat-bared ground shows (rgb), 1 for a snow layer (a).
			float thaw[4][4]{ { 1.0f, 1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 0.0f },
				{ 1.0f, 1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 0.0f } };
		};
		static_assert(sizeof(MaterialCB) % 16 == 0);

		// One immutable constant buffer per distinct material: a landscape frame uses a
		// handful, so a routed draw binds one instead of mapping a buffer. Keyed by the
		// buffer's exact bytes.
		struct MaterialKey
		{
			std::array<uint32_t, sizeof(MaterialCB) / sizeof(uint32_t)> bits{};

			bool operator==(const MaterialKey&) const = default;
		};

		struct MaterialKeyHash
		{
			size_t operator()(const MaterialKey& a_key) const
			{
				uint64_t hash = 0xcbf29ce484222325ull;
				for (const auto word : a_key.bits) {
					hash = (hash ^ word) * 0x100000001b3ull;
				}
				return static_cast<size_t>(hash);
			}
		};

		using MaterialMap = std::unordered_map<MaterialKey, ID3D11Buffer*, MaterialKeyHash>;

		std::shared_mutex g_materialLock;
		MaterialMap       g_materials;
		constexpr size_t  kMaxMaterials = 256;

		ID3D11Buffer* MaterialBuffer(const MaterialCB& a_data)
		{
			MaterialKey key{};
			std::memcpy(key.bits.data(), &a_data, sizeof(key.bits));

			{
				const std::shared_lock lock(g_materialLock);
				if (const auto it = g_materials.find(key); it != g_materials.end()) {
					return it->second;
				}
			}

			const std::unique_lock lock(g_materialLock);
			if (const auto it = g_materials.find(key); it != g_materials.end()) {
				return it->second;
			}
			if (g_materials.size() >= kMaxMaterials || !globals::d3d::device) {
				return nullptr;
			}

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = sizeof(MaterialCB);
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			D3D11_SUBRESOURCE_DATA initial{};
			initial.pSysMem = &a_data;

			ID3D11Buffer* buffer = nullptr;
			const HRESULT hr = globals::d3d::device->CreateBuffer(&desc, &initial, &buffer);
			if (FAILED(hr)) {
				logger::error("Failed to create a per-draw material buffer (0x{:08X})",
					static_cast<uint32_t>(hr));
				return nullptr;
			}
			g_materials[key] = buffer;
			return buffer;
		}

		// What Reset replaces is kept for a few frames before it is released: a draw on
		// another thread that resolved its pair or material just before the reset still
		// finds them (moving a map keeps its elements where they are).
		struct Retired
		{
			uint64_t                           frame{ 0 };
			std::unordered_map<uint64_t, Pair> caches[static_cast<size_t>(Mode::kCount)];
			MaterialMap                        materials;

			~Retired()
			{
				for (auto& cache : caches) {
					for (auto& [key, pair] : cache) {
						if (pair.hs) {
							pair.hs->Release();
						}
						if (pair.ds) {
							pair.ds->Release();
						}
					}
				}
				for (auto& [key, buffer] : materials) {
					buffer->Release();
				}
			}
		};

		constexpr uint64_t                    kRetireFrames = 4;
		std::vector<std::unique_ptr<Retired>> g_retired;  // under g_cacheLock

		void ReleaseRetired(bool a_all)
		{
			const uint64_t now = globals::FrameIndex();
			std::erase_if(g_retired, [&](const std::unique_ptr<Retired>& a_retired) {
				return a_all || a_retired->frame + kRetireFrames <= now;
			});
		}

		// The colour each kind of ground shows where heat melted the snow off it, as a
		// multiplier on a winter mod's white repaint: matted wet grass, dark wet soil.
		std::array<float, 3> ThawTint(Surfaces::Type a_type)
		{
			switch (a_type) {
			case Surfaces::Type::kGrass:
				return { 0.26f, 0.32f, 0.13f };
			case Surfaces::Type::kDirt:
				return { 0.30f, 0.23f, 0.15f };
			case Surfaces::Type::kMud:
				return { 0.20f, 0.15f, 0.10f };
			case Surfaces::Type::kSand:
				return { 0.55f, 0.47f, 0.33f };
			case Surfaces::Type::kAsh:
				return { 0.24f, 0.23f, 0.22f };
			case Surfaces::Type::kGravel:
				return { 0.36f, 0.34f, 0.31f };
			case Surfaces::Type::kStone:
				return { 0.42f, 0.41f, 0.39f };
			default:
				// A name that says nothing: wet earth with dead grass.
				return { 0.28f, 0.27f, 0.15f };
			}
		}

		ID3D11Buffer* LandscapeMaterial(const DrawMaterial& a_material)
		{
			if (!Settings::enableSurfaceMaterial) {
				return nullptr;
			}

			const bool  forced = Settings::debugBlendLayer >= 0;
			const int   layer = forced ? Settings::debugBlendLayer : a_material.revealLayer;
			const int   snow = forced ? -1 : a_material.snowLayer;
			const float scale = forced ? 1.0f : a_material.strengthScale;

			MaterialCB material{};
			material.material[0] = static_cast<float>(layer);
			material.material[1] = Settings::blendFullDepth;
			material.material[2] = static_cast<float>(snow);
			material.material[3] = std::clamp(Settings::blendStrength * scale, 0.0f, 1.0f);
			if (Settings::meltShowsGround && !forced) {
				for (size_t i = 0; i < 4; ++i) {
					if (a_material.thaw[i] < 0) {
						continue;
					}
					const auto tint = ThawTint(static_cast<Surfaces::Type>(a_material.thaw[i]));
					material.thaw[i][0] = tint[0];
					material.thaw[i][1] = tint[1];
					material.thaw[i][2] = tint[2];
					material.thaw[i][3] = 1.0f;
				}
			}

			return MaterialBuffer(material);
		}

		struct ThreadState
		{
			bool       armed{ false };
			Request    request{};
			bool       applied{ false };
			bool       profiled{ false };
			SavedState saved{};
		};

		thread_local ThreadState t_state;

		std::atomic<uint64_t> g_lastRoutedFrame{ 0 };

		struct BracketCpuTimer
		{
			int64_t start{ Profiler::Ticks() };

			~BracketCpuTimer()
			{
				Profiler::AddCpuTicks(Profiler::CpuScope::kDrawBracket, Profiler::Ticks() - start);
			}
		};

		template <class T>
		struct Held
		{
			T* object{ nullptr };
			~Held()
			{
				if (object) {
					object->Release();
				}
			}
		};
	}

	void Arm(const Request& a_request)
	{
		t_state.armed = true;
		t_state.request = a_request;
	}

	void Disarm()
	{
		t_state.armed = false;
	}

	bool Armed()
	{
		return t_state.armed;
	}

	bool BeforeDraw(ID3D11DeviceContext* a_context)
	{
		const BracketCpuTimer cpuTimer;

		auto& state = t_state;
		if (state.armed && !state.applied && a_context && state.request.measureOnly) {
			// The same scope a routed draw gets, around the untouched vanilla draw.
			if (!Settings::profileDrawScopes || a_context != globals::d3d::context) {
				return false;
			}
			state.profiled = Profiler::GpuBegin(Profiler::Scope::kLandscape);
			if (!state.profiled) {
				return false;
			}
			state.applied = true;
			return true;
		}
		if (!state.armed || state.applied || !a_context || !Settings::enableTessellation ||
			!globals::Ready()) {
			return false;
		}

		// Whether or not the draw is routed, its layer textures are bound now.
		if (state.request.mode == Mode::kLandscape) {
			WinterLook::Capture(a_context, state.request.layerKeys);
		}

		// The control points are the draw's triangles; any other topology would be
		// regrouped into garbage.
		D3D11_PRIMITIVE_TOPOLOGY topology{};
		a_context->IAGetPrimitiveTopology(&topology);
		if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
			return false;
		}

		Held<ID3D11VertexShader> vertexShader;
		a_context->VSGetShader(&vertexShader.object, nullptr, nullptr);

		bool        stereo = false;
		const Mode  mode = state.request.mode;
		const Pair* pair = Resolve(vertexShader.object, mode, stereo);
		if (!pair) {
			return false;
		}

		// Without the engine's per-frame constants (or, in VR, its stereo state) the
		// stages would project every patch to nothing: leave the draw vanilla instead.
		Held<ID3D11Buffer> frameCB;
		a_context->VSGetConstantBuffers(kFrameSlot, 1, &frameCB.object);
		if (!frameCB.object) {
			a_context->PSGetConstantBuffers(kFrameSlot, 1, &frameCB.object);
		}
		Held<ID3D11Buffer> stereoCB;
		if (stereo) {
			a_context->VSGetConstantBuffers(kStereoSlot, 1, &stereoCB.object);
		}
		if (!frameCB.object || (stereo && !stereoCB.object)) {
			return false;
		}

		// A missing material buffer must leave the whole draw vanilla: the generated
		// domain shader cannot interpret whatever the engine last bound at b11.
		auto* material = LandscapeMaterial(state.request.material);
		if (Settings::enableSurfaceMaterial && !material) {
			return false;
		}

		state.saved.Capture(a_context);
		state.applied = true;

		a_context->GSSetShader(nullptr, nullptr, 0);

		a_context->HSSetConstantBuffers(kFrameSlot, 1, &frameCB.object);
		a_context->DSSetConstantBuffers(kFrameSlot, 1, &frameCB.object);
		if (!REL::Module::IsVR()) {
			// The passes over the G-buffer and the snow spray (flat only) draw with this frame's
			// world camera, and the view tests read it back.
			FrameCamera::Note(a_context, frameCB.object);
		}

		// VR's vertex shader reads whether both eyes share this draw from cb8; the domain
		// stage repeats its stereo projection, so it gets the same buffer.
		if (stereo) {
			a_context->DSSetConstantBuffers(kStereoSlot, 1, &stereoCB.object);
		}

		if (Settings::useClipmap) {
			Clipmap::BindDomain(a_context);
			SnowCoverage::BindDomain(a_context);
			Shelter::BindDomain(a_context);

			ID3D11ShaderResourceView* coverage = SnowCoverage::View();
			a_context->HSSetShaderResources(kHullCoverageSlot, 1, &coverage);
		}
		if (material) {
			a_context->DSSetConstantBuffers(kMaterialSlot, 1, &material);
		}

		a_context->HSSetShader(pair->hs, nullptr, 0);
		a_context->DSSetShader(pair->ds, nullptr, 0);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		g_lastRoutedFrame.store(globals::FrameIndex(), std::memory_order_relaxed);
		Profiler::Tally(Profiler::Count::kLandscapeRouted);

		// The G-buffer targets are bound for this draw: the snow-on-objects pass after the
		// geometry pass writes into the same albedo.
		if (mode == Mode::kLandscape && !REL::Module::IsVR()) {
			ObjectSnow::NoteGBuffer(a_context);
		}

		// Timestamp queries belong to the profiler's immediate context. Deferred
		// terrain still routes, but recording it must not issue queries on another
		// thread/context or mutate the immediate context's open timing scope.
		state.profiled = a_context == globals::d3d::context &&
			Profiler::GpuBegin(Profiler::Scope::kLandscape);
		return true;
	}

	void AfterDraw(ID3D11DeviceContext* a_context)
	{
		const BracketCpuTimer cpuTimer;

		auto& state = t_state;
		if (!state.applied) {
			return;
		}

		if (state.profiled) {
			Profiler::GpuEnd();
		}
		if (!state.request.measureOnly) {
			state.saved.Restore(a_context);
		}
		state.applied = false;
		state.profiled = false;
	}

	bool RoutedRecently()
	{
		const uint64_t last = g_lastRoutedFrame.load(std::memory_order_relaxed);
		return last != 0 && globals::FrameIndex() <= last + 2;
	}

	void PrepareFrame()
	{
		if (!globals::Ready()) {
			return;
		}
		const int64_t          started = Profiler::Ticks();
		const std::scoped_lock lock(g_cacheLock);
		for (auto& cache : g_caches) {
			for (auto& [key, pair] : cache) {
				if (pair.pending) {
					FinishPair(pair);
				}
			}
		}
		if (!g_retired.empty()) {
			ReleaseRetired(false);
		}
		Profiler::AddCpuTicks(Profiler::CpuScope::kShaderPrepare, Profiler::Ticks() - started);
	}

	void Reset()
	{
		t_state.armed = false;

		{
			const std::scoped_lock lock(g_cacheLock);
			g_generation.fetch_add(1, std::memory_order_acq_rel);
			ShaderCompiler::GetWorker().CancelQueued();

			auto retired = std::make_unique<Retired>();
			retired->frame = globals::FrameIndex();
			for (size_t mode = 0; mode < std::size(g_caches); ++mode) {
				retired->caches[mode] = std::move(g_caches[mode]);
				g_caches[mode].clear();
			}
			{
				const std::unique_lock materialLock(g_materialLock);
				retired->materials = std::move(g_materials);
				g_materials.clear();
			}
			g_retired.push_back(std::move(retired));
		}

		g_lastRoutedFrame.store(0, std::memory_order_relaxed);
	}
#endif
}
