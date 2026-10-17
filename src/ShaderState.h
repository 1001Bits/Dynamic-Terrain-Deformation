// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#pragma once

#include <d3d11.h>

// A shader binding includes its dynamic-linkage class instances. Restoring only
// the shader pointer silently removes those instances from another renderer's
// shader. GetShader adds references to both; retain only the returned prefix.
namespace ShaderState
{
	template <class Shader, auto Get, auto Set>
	class Binding
	{
	public:
		Binding() = default;
		Binding(const Binding&) = delete;
		Binding& operator=(const Binding&) = delete;
		~Binding() { Release(); }

		void Capture(ID3D11DeviceContext* a_context)
		{
			Release();
			_count = D3D11_SHADER_MAX_INTERFACES;
			(a_context->*Get)(&_shader, _instances, &_count);
		}

		void Restore(ID3D11DeviceContext* a_context) const
		{
			(a_context->*Set)(_shader, _instances, _count);
		}

		void Release()
		{
			if (_shader) {
				_shader->Release();
				_shader = nullptr;
			}
			for (UINT i = 0; i < _count; ++i) {
				if (_instances[i]) {
					_instances[i]->Release();
				}
			}
			_count = 0;
		}

	private:
		Shader* _shader{ nullptr };
		UINT _count{ 0 };
		// No zeroing is needed: GetShader initializes exactly _count entries.
		ID3D11ClassInstance* _instances[D3D11_SHADER_MAX_INTERFACES];
	};

	using Vertex = Binding<ID3D11VertexShader, &ID3D11DeviceContext::VSGetShader, &ID3D11DeviceContext::VSSetShader>;
	using Pixel = Binding<ID3D11PixelShader, &ID3D11DeviceContext::PSGetShader, &ID3D11DeviceContext::PSSetShader>;
	using Geometry = Binding<ID3D11GeometryShader, &ID3D11DeviceContext::GSGetShader, &ID3D11DeviceContext::GSSetShader>;
	using Hull = Binding<ID3D11HullShader, &ID3D11DeviceContext::HSGetShader, &ID3D11DeviceContext::HSSetShader>;
	using Domain = Binding<ID3D11DomainShader, &ID3D11DeviceContext::DSGetShader, &ID3D11DeviceContext::DSSetShader>;
	using Compute = Binding<ID3D11ComputeShader, &ID3D11DeviceContext::CSGetShader, &ID3D11DeviceContext::CSSetShader>;
}
