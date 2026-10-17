// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <d3d11.h>

#include "ShaderState.h"

#include <cstddef>

namespace Tessellation
{
	// Everything a routed draw changes, captured before and put back after, so the
	// engine's own record of the pipeline is never wrong. Shared with the offline checks,
	// which run this exact code against a real device.
	struct SavedState
	{
		static constexpr UINT kFirstCB = 8;
		static constexpr UINT kCBCount = 6;  // 8..13 (8: VR stereo state, 11-13: ours, 12: frame)
		static constexpr UINT kDomainSRVs = 5;  // both levels' fields, coverage, mesh cap, scorch
		static constexpr UINT kHullSRVs = 3;  // both levels' activity and the snow coverage
		static constexpr UINT kDomainSamplers = 3;
		static constexpr UINT kHullSamplers = 1;

		ShaderState::Hull        hs;
		ShaderState::Domain      ds;
		ShaderState::Geometry    gs;
		D3D11_PRIMITIVE_TOPOLOGY  topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
		ID3D11Buffer*             hsCB[kCBCount]{};
		ID3D11Buffer*             dsCB[kCBCount]{};
		ID3D11ShaderResourceView* dsSRV[kDomainSRVs]{};
		ID3D11ShaderResourceView* hsSRV[kHullSRVs]{};
		ID3D11SamplerState*       dsSamplers[kDomainSamplers]{};
		ID3D11SamplerState*       hsSamplers[kHullSamplers]{};

		SavedState() = default;
		SavedState(const SavedState&) = delete;
		SavedState& operator=(const SavedState&) = delete;
		~SavedState() { Release(); }

		template <class T>
		static void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		template <class T, size_t N>
		static void Drop(T* (&a_objects)[N])
		{
			for (auto*& object : a_objects) {
				Drop(object);
			}
		}

		void Release()
		{
			hs.Release();
			ds.Release();
			gs.Release();
			Drop(hsCB);
			Drop(dsCB);
			Drop(dsSRV);
			Drop(hsSRV);
			Drop(dsSamplers);
			Drop(hsSamplers);
			topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		}

		void Capture(ID3D11DeviceContext* a_context)
		{
			Release();
			hs.Capture(a_context);
			ds.Capture(a_context);
			gs.Capture(a_context);
			a_context->IAGetPrimitiveTopology(&topology);
			a_context->HSGetConstantBuffers(kFirstCB, kCBCount, hsCB);
			a_context->DSGetConstantBuffers(kFirstCB, kCBCount, dsCB);
			a_context->DSGetShaderResources(0, kDomainSRVs, dsSRV);
			a_context->HSGetShaderResources(0, kHullSRVs, hsSRV);
			a_context->DSGetSamplers(0, kDomainSamplers, dsSamplers);
			a_context->HSGetSamplers(0, kHullSamplers, hsSamplers);
		}

		void Restore(ID3D11DeviceContext* a_context)
		{
			hs.Restore(a_context);
			ds.Restore(a_context);
			gs.Restore(a_context);
			a_context->IASetPrimitiveTopology(topology);
			a_context->HSSetConstantBuffers(kFirstCB, kCBCount, hsCB);
			a_context->DSSetConstantBuffers(kFirstCB, kCBCount, dsCB);
			a_context->DSSetShaderResources(0, kDomainSRVs, dsSRV);
			a_context->HSSetShaderResources(0, kHullSRVs, hsSRV);
			a_context->DSSetSamplers(0, kDomainSamplers, dsSamplers);
			a_context->HSSetSamplers(0, kHullSamplers, hsSamplers);
			Release();
		}
	};
}
