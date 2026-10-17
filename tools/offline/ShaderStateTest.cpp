// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "ShaderState.h"
#include "TessellationResources.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace
{
	void Check(HRESULT a_result, const char* a_what)
	{
		if (FAILED(a_result)) {
			std::fprintf(stderr, "%s failed: %08X\n", a_what, static_cast<unsigned>(a_result));
			throw std::runtime_error(a_what);
		}
	}

	void Require(bool a_ok, const char* a_what)
	{
		if (!a_ok) {
			throw std::runtime_error(a_what);
		}
	}

	constexpr char kSource[] = R"(
interface IColour { float4 Get(); };
class ConstantColour : IColour {
    float4 colour;
    float4 Get() { return colour; }
};
IColour selected;
float4 linkedPS() : SV_Target { return selected.Get(); }
float4 plainPS() : SV_Target { return float4(1, 0, 0, 1); }
RWStructuredBuffer<float4> result : register(u0);
[numthreads(1, 1, 1)]
void linkedCS(uint3 id : SV_DispatchThreadID) { result[id.x] = selected.Get(); }
struct Vertex { float4 position : SV_Position; };
[maxvertexcount(1)]
void linkedGS(point Vertex input[1], inout PointStream<Vertex> output) {
    Vertex v = input[0];
    v.position += selected.Get();
    output.Append(v);
}
)";

	ComPtr<ID3DBlob> Compile(const char* a_entry, const char* a_profile)
	{
		ComPtr<ID3DBlob> code, errors;
		const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), "ShaderStateTest", nullptr, nullptr,
			a_entry, a_profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
		if (FAILED(hr) && errors) {
			std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
		}
		Check(hr, "Compile dynamically linked shader");
		return code;
	}

	template <class Shader, auto Get>
	void Verify(ID3D11DeviceContext* a_context, Shader* a_shader, ID3D11ClassInstance* a_instance)
	{
		ComPtr<Shader> shader;
		ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
		UINT count = D3D11_SHADER_MAX_INTERFACES;
		(a_context->*Get)(&shader, instances, &count);
		const bool matches = shader.Get() == a_shader && count == (a_instance ? 1u : 0u) &&
		                     (!a_instance || instances[0] == a_instance);
		for (UINT i = 0; i < count; ++i) {
			if (instances[i]) {
				instances[i]->Release();
			}
		}
		Require(matches, "Shader and dynamic class binding must survive the pass");
	}

	ULONG References(IUnknown* a_object)
	{
		const ULONG count = a_object->AddRef();
		a_object->Release();
		return count;
	}
}

int main(int argc, char** argv)
{
	try {
		D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_WARP;
		if (argc == 2 && std::string_view(argv[1]) == "--hardware") {
			driver = D3D_DRIVER_TYPE_HARDWARE;
		} else if (argc != 1) {
			throw std::runtime_error("Usage: ShaderStateTest [--hardware]");
		}
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		constexpr D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		Check(D3D11CreateDevice(nullptr, driver, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
			&device, nullptr, &context), "Create test device");

		ComPtr<ID3D11ClassLinkage> linkage;
		Check(device->CreateClassLinkage(&linkage), "Create class linkage");
		const auto psCode = Compile("linkedPS", "ps_5_0");
		const auto plainCode = Compile("plainPS", "ps_5_0");
		const auto gsCode = Compile("linkedGS", "gs_5_0");
		const auto csCode = Compile("linkedCS", "cs_5_0");
		ComPtr<ID3D11PixelShader> ps, plain;
		ComPtr<ID3D11GeometryShader> gs;
		ComPtr<ID3D11ComputeShader> cs;
		Check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), linkage.Get(), &ps), "Linked PS");
		Check(device->CreatePixelShader(plainCode->GetBufferPointer(), plainCode->GetBufferSize(), nullptr, &plain), "Plain PS");
		Check(device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), linkage.Get(), &gs), "Linked GS");
		Check(device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), linkage.Get(), &cs), "Linked CS");
		ComPtr<ID3D11ClassInstance> instance;
		Check(linkage->CreateClassInstance("ConstantColour", 0, 0, 0, 0, &instance), "Create colour instance");
		context->PSSetShader(ps.Get(), instance.GetAddressOf(), 1);
		context->GSSetShader(gs.Get(), instance.GetAddressOf(), 1);
		context->CSSetShader(cs.Get(), instance.GetAddressOf(), 1);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		Verify<ID3D11PixelShader, &ID3D11DeviceContext::PSGetShader>(context.Get(), ps.Get(), instance.Get());
		Verify<ID3D11GeometryShader, &ID3D11DeviceContext::GSGetShader>(context.Get(), gs.Get(), instance.Get());
		const ULONG before = References(instance.Get());
		{
			ShaderState::Pixel saved;
			ShaderState::Compute compute;
			Tessellation::SavedState terrain;
			for (int i = 0; i < 256; ++i) {
				saved.Capture(context.Get());
				// Re-capture must release the previous references, including class instances.
				saved.Capture(context.Get());
				terrain.Capture(context.Get());
				compute.Capture(context.Get());
				context->PSSetShader(plain.Get(), nullptr, 0);
				context->GSSetShader(nullptr, nullptr, 0);
				context->CSSetShader(nullptr, nullptr, 0);
				saved.Restore(context.Get());
				terrain.Restore(context.Get());
				compute.Restore(context.Get());
				Verify<ID3D11PixelShader, &ID3D11DeviceContext::PSGetShader>(context.Get(), ps.Get(), instance.Get());
				Verify<ID3D11GeometryShader, &ID3D11DeviceContext::GSGetShader>(context.Get(), gs.Get(), instance.Get());
				Verify<ID3D11ComputeShader, &ID3D11DeviceContext::CSGetShader>(context.Get(), cs.Get(), instance.Get());
				saved.Release();
				compute.Release();
			}
		}
		Require(References(instance.Get()) == before, "Repeated save/restore must not leak class references");
		{
			ShaderState::Pixel saved;
			saved.Capture(context.Get());
			context->PSSetShader(plain.Get(), nullptr, 0);
			saved.Capture(context.Get());
			context->PSSetShader(nullptr, nullptr, 0);
			saved.Restore(context.Get());
			Verify<ID3D11PixelShader, &ID3D11DeviceContext::PSGetShader>(context.Get(), plain.Get(), nullptr);
			context->PSSetShader(nullptr, nullptr, 0);
			saved.Capture(context.Get());
			context->PSSetShader(ps.Get(), instance.GetAddressOf(), 1);
			saved.Restore(context.Get());
			Verify<ID3D11PixelShader, &ID3D11DeviceContext::PSGetShader>(context.Get(), nullptr, nullptr);
		}
		context->ClearState();
		std::puts("PASS shader state: linked pixel/geometry/compute instances, terrain guard, re-capture, release balance, plain and null shaders");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL shader state: %s\n", e.what());
		return 1;
	}
}
