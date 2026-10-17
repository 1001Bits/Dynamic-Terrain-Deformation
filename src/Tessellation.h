// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include "ShaderReflection.h"

namespace Tessellation
{
	inline constexpr uint32_t kMaterialSlot = 11;

	// Fallout 4's per-frame constant buffer; bound to VS b12 by the engine and mirrored to
	// our hull/domain stages for the draws we route.
	inline constexpr uint32_t kFrameSlot = 12;

	// Fallout 4 VR's stereo state (c0.x = 1 when one instanced draw covers both eyes).
	inline constexpr uint32_t kStereoSlot = 8;

	// Hull-stage slot of the snow coverage map (the hull stage reads both levels' activity
	// at t0/t1, see Clipmap.h, and samples with the field's sampler at s0).
	inline constexpr uint32_t kHullCoverageSlot = 2;

	enum class Mode
	{
		kLandscape,

		kCount
	};

	bool WantsDisplacement(Mode a_mode);
	bool WantsSubdivision(Mode a_mode);

	struct DrawMaterial
	{
		int revealLayer{ -1 };

		int snowLayer{ -1 };

		float strengthScale{ 1.0f };

		// Per layer, the ground (a Surfaces::Type) a snow layer shows where heat melts its
		// snow off; -1 for a layer that is not snow.
		int8_t thaw[4]{ -1, -1, -1, -1 };
	};

	struct Request
	{
		Mode         mode{ Mode::kLandscape };
		DrawMaterial material{};
		// The layer textures' WinterLook keys: the draw has them measured while they are bound.
		uint64_t     layerKeys[4]{};
		// Profiling only: time the draw with the landscape GPU scope and change nothing, so
		// vanilla terrain can be measured against the same draws tessellated.
		bool         measureOnly{ false };
	};

	// The pre-pass SetupGeometry hook arms a request; the DrawIndexed hook applies it to
	// the draw the engine issues next and RestoreGeometry disarms it. Arming is per
	// thread, so a draw on any other thread is never touched.
	void Arm(const Request& a_request);
	void Disarm();
	bool Armed();

	// Called from the DrawIndexed detour. Returns true when the pipeline was changed and
	// AfterDraw must run after the original draw.
	bool BeforeDraw(ID3D11DeviceContext* a_context);
	void AfterDraw(ID3D11DeviceContext* a_context);

	void Reset();
	void PrepareFrame();

	// True when a landscape draw was routed in the last couple of frames: the shaders exist
	// and work, so near terrain may leave the depth pre-pass.
	bool RoutedRecently();

	// Semantic layout of Fallout 4's landscape pre-pass vertex shader (techniques 0x3B and
	// 0x0200003B): the one layout the domain stage knows how to post-process beyond the
	// clip-space lift. Exposed for the offline checks.
	struct Layout
	{
		int clip{ -1 };
		int world{ -1 };
		int prevWorld{ -1 };
		int tbn[3]{ -1, -1, -1 };
		int weights{ -1 };
		int colour{ -1 };

		// Fallout 4 VR's single-pass stereo: the eye (instance id & 1) and the clip/cull
		// distance that splits the double-wide target between the eyes.
		int eye{ -1 };
		int clipDistance{ -1 };
		int cullDistance{ -1 };

		[[nodiscard]] bool Stereo() const { return eye >= 0; }

		[[nodiscard]] bool Landscape() const
		{
			return clip >= 0 && world >= 0 && prevWorld >= 0 && tbn[0] >= 0 && tbn[1] >= 0 &&
			       tbn[2] >= 0;
		}
	};

	Layout LayoutFor(const Reflection::Signature& a_signature);

	// HLSL for one routed signature, as compiled at runtime (exposed for the offline checks).
	std::string HullSource(const Reflection::Signature& a_signature, Mode a_mode);
	std::string DomainSource(const Reflection::Signature& a_signature, Mode a_mode);
}
