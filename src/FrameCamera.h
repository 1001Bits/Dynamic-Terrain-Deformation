// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DeviceContext;

// The world camera this frame's G-buffer was drawn with, on flat runtimes: a copy of the
// engine's per-frame constant buffer (b12, BSGraphics::Renderer::SetPerFrameConstants) for
// the passes that draw into the G-buffer, and the same read back to the CPU a few frames late
// for the view tests. The engine's camera block (State::cameraState) is not the drawn camera
// there: on 1.10.163 it held a camera at the origin with a 41-degree view while the player
// stood at (-80000, 90000). DrawWorld::DeferredPrePass draws the first-person geometry with
// its own camera before the world's (DrawWorld::sp1stPersonCamera, then DrawWorld::pCamera),
// so a copy is taken only where the world camera is in use: at the first routed landscape
// draw, or (no landscape: an interior) once the pass is over, its last camera being the
// world's. VR has its own stereo constants and keeps the camera block.
namespace FrameCamera
{
	inline constexpr unsigned kSlot = 12;

	// The registers read (docs/FO4_PORT_NOTES.md): view c0 (three rows), projection c4,
	// view-projection c8 (camera-relative positions to clip) and position adjust c35 (the
	// eye). Row-vector matrices uploaded transposed: row r of each is four floats.
	inline constexpr size_t kViewRegister = 0;
	inline constexpr size_t kProjRegister = 4;
	inline constexpr size_t kViewProjRegister = 8;
	inline constexpr size_t kPosAdjustRegister = 35;
	inline constexpr size_t kMinFloats = (kPosAdjustRegister + 1) * 4;

	struct Camera
	{
		float eye[3]{};
		float right[3]{};
		float up[3]{};
		float forward[3]{};
		float proj[16]{};  // as stored, row r = four floats
	};

	// The camera in a copy of the buffer: false unless the view's rows are unit axes square to
	// each other, the eye is finite and a point straight ahead lands in the middle of the
	// screen with w its distance (a perspective's w), near and far.
	inline bool Parse(const float* a_cb, size_t a_floats, Camera& a_out)
	{
		if (!a_cb || a_floats < kMinFloats) {
			return false;
		}
		const float* view = a_cb + kViewRegister * 4;
		const float* eye = a_cb + kPosAdjustRegister * 4;
		for (int i = 0; i < 3; ++i) {
			a_out.right[i] = view[0 * 4 + i];
			a_out.up[i] = view[1 * 4 + i];
			a_out.forward[i] = view[2 * 4 + i];
			a_out.eye[i] = eye[i];
		}
		for (int i = 0; i < 16; ++i) {
			a_out.proj[i] = a_cb[kProjRegister * 4 + i];
		}

		const auto dot = [](const float (&a_lhs)[3], const float (&a_rhs)[3]) {
			return a_lhs[0] * a_rhs[0] + a_lhs[1] * a_rhs[1] + a_lhs[2] * a_rhs[2];
		};
		const auto unit = [&](const float (&a_axis)[3]) {
			const float lengthSq = dot(a_axis, a_axis);
			return lengthSq > 0.81f && lengthSq < 1.21f;
		};
		if (!unit(a_out.right) || !unit(a_out.up) || !unit(a_out.forward) ||
			std::fabs(dot(a_out.right, a_out.up)) > 0.1f || std::fabs(dot(a_out.right, a_out.forward)) > 0.1f ||
			std::fabs(dot(a_out.up, a_out.forward)) > 0.1f ||
			!std::isfinite(a_out.eye[0] + a_out.eye[1] + a_out.eye[2])) {
			return false;
		}

		const float* m = a_cb + kViewProjRegister * 4;
		for (const float distance : { 200.0f, 2000.0f }) {
			float clip[4];
			for (int r = 0; r < 4; ++r) {
				clip[r] = (m[r * 4 + 0] * a_out.forward[0] + m[r * 4 + 1] * a_out.forward[1] +
							  m[r * 4 + 2] * a_out.forward[2]) *
				              distance +
				          m[r * 4 + 3];
			}
			if (!(clip[3] > 0.5f * distance && clip[3] < 2.0f * distance) || !(std::fabs(clip[0] / clip[3]) < 0.05f) ||
				!(std::fabs(clip[1] / clip[3]) < 0.05f)) {
				return false;
			}
		}
		return a_out.proj[0] > 0.01f && a_out.proj[5] > 0.01f && std::isfinite(a_out.proj[0] + a_out.proj[5]);
	}

	// At a routed landscape draw: a_source is the per-frame buffer bound for it. The first
	// call of a frame takes the copy.
	void Note(ID3D11DeviceContext* a_context, ID3D11Buffer* a_source);

	// Once DrawWorld::DeferredPrePass is over: takes the copy from what is bound if no landscape
	// draw did, and sends this frame's copy to be read back.
	void AfterPrePass(ID3D11DeviceContext* a_context);

	// This frame's copy, for a pass drawn into this frame's G-buffer; nullptr if none was taken.
	ID3D11Buffer* Constants();

	// The newest camera read back (a few frames old; a_frame, if given, is the frame it drew);
	// false while there is none recent enough (a loading screen, VR, the first frames), so
	// nothing is hidden from it.
	bool Latest(Camera& a_out, uint64_t* a_frame = nullptr);

	// A save load or worldspace change invalidates CPU cameras still in flight. GPU
	// buffers remain allocated; subsequent copies stay ordered on their contexts.
	void Reset();
}
