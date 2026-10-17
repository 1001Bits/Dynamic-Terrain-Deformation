// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PCH.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <filesystem>
#include <numeric>
#include <thread>
#include "DdsLoader.h"
#include "ClipmapUpdateCS.h"
#include "ImpactPatterns.h"
#include "ShelterTransition.h"
#include "FootGeometry.h"
#include "FootprintProfiles.h"
#include "HiddenMarks.h"
#include "GroundDecalFit.h"
#include "SnowMeshCover.h"
#include "SnowSurface.h"
#include "ObjectSnowRule.h"
#include "ObjectSnowShader.h"
#include "IndoorPrintRule.h"
#include "IndoorPrintShader.h"
#include "FrameCamera.h"
#include "CorpseRule.h"
#include "BlanketTessellation.h"
#include "TessellationResources.h"
#include "ShaderRegistry.h"
#include "Tessellation.h"
#include <limits>
#include <fstream>
#include <iterator>
#include <winioctl.h>
#include "Tessellation.cpp"

using Microsoft::WRL::ComPtr;
constexpr UINT N = 64;
D3D_DRIVER_TYPE g_driver = D3D_DRIVER_TYPE_WARP;
bool g_drawHooks = false;
bool g_denseDispatch = false;
bool g_cpuOnly = false;
// In --draw-hooks mode the production context interception is active, while all
// draws stay unarmed. The field checks then verify forwarding of the real D3D API.
namespace Tessellation {
	bool Armed() { return false; }
	bool BeforeDraw(ID3D11DeviceContext*) { return false; }
	void AfterDraw(ID3D11DeviceContext*) {}
}
namespace Hooks {
	void NoteDrawContext(ID3D11DeviceContext*) {}
}
namespace Clipmap {
	bool Ready() { return true; }
	uint32_t ActiveLevels() { return LevelCount(); }
}

// The shipping update's constant buffer with the test's defaults: a field of N cells of
// one unit centred on the origin, one stamp there, no frame time.
struct Params : Clipmap::UpdateParams
{
	explicit Params(UINT a_size = N)
	{
		const float size = static_cast<float>(a_size);
		const float defaults[][4]{
			{ 0, 0, 1, size },                  // window
			{ 0, 1, size / 2 - 2, 0.55f },      // control
			{ 0, 0, 0, 0.4f },                  // weather
		};
		std::copy_n(defaults[0], 4, window);
		std::copy_n(defaults[1], 4, control);
		std::copy_n(defaults[2], 4, weather);
		const float coarseDefaults[4]{ 0, size, 0, size / 2 - 2 };
		std::copy_n(coarseDefaults, 4, coarse);
		rimShape[0] = 0.5f;
		rimShape[1] = 0.45f;
		const float snow[4]{ 0.55f, 0.4f, 0.5f, 0.45f };
		std::copy_n(snow, 4, snowRim);
		raise[1] = 1;
		raiseWindow[2] = 1.0e6f;
		raiseWindow[3] = 2.0e6f;

		stamps[0][2] = 5;
		stamps[0][3] = 2;
		stampParams[0][0] = 0.3f;
		stampParams[0][1] = 0.92f;
		stampParams[0][3] = 1;
	}
};
void Require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }

void CheckMalformedDds()
{
	struct Fixture {
		std::filesystem::path path = std::filesystem::temp_directory_path() /
			std::format("nmn-dds-{}-{}.dds", GetCurrentProcessId(), GetTickCount64());
		~Fixture() { std::error_code error; std::filesystem::remove(path, error); }
	} fixture;
	// All cases must fail before calling the device. The sentinel also makes an
	// accidental GPU call fail the CPU-only suite instead of silently passing.
	auto* device = reinterpret_cast<ID3D11Device*>(static_cast<uintptr_t>(1));
	const auto reject = [&](const std::vector<uint32_t>& words) {
		{
			std::ofstream file(fixture.path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(uint32_t));
			Require(static_cast<bool>(file), "Write malformed DDS fixture");
		}
		ID3D11ShaderResourceView* view = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(1));
		Require(FAILED(Dds::LoadTexture(device, fixture.path.c_str(), &view)) && !view,
			"Malformed DDS must return an error and clear the output before using the device");
	};
	std::vector<uint32_t> header(32);
	header[0] = 0x20534444; header[1] = 124;
	header[3] = 4; header[4] = 4; header[7] = 1;
	header[19] = 32; header[20] = 4; header[21] = 0x31545844; // BC1
	reject({});
	reject({ 0x20534444 });
	reject(header);  // complete header, missing 8-byte block
	auto bad = header; bad[4] = 16385; reject(bad);
	bad = header; bad[7] = 0xFFFFFFFF; reject(bad);
	bad = header; bad[7] = 4; reject(bad);  // 4x4 has only three mip levels
	bad = header; bad[21] = 0x30315844; bad.resize(37);
	bad[32] = DXGI_FORMAT_BC1_UNORM; bad[33] = 3; bad[35] = 0;
	reject(bad);  // DX10 array size zero
	bad[35] = 1; reject(bad);  // valid DX10 header, missing payload

	// A sparse 16 GiB non-DDS file must be rejected after its tiny header, not
	// allocated/read wholesale. Sparse storage keeps this regression cheap.
	HANDLE file = CreateFileW(fixture.path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	Require(file != INVALID_HANDLE_VALUE, "Create sparse malformed DDS");
	DWORD bytes = 0;
	const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr) != FALSE;
	LARGE_INTEGER length{}; length.QuadPart = 16ll << 30;
	const bool sized = sparse && SetFilePointerEx(file, length, nullptr, FILE_BEGIN) && SetEndOfFile(file);
	CloseHandle(file);
	Require(sized, "Create large sparse DDS fixture without disk allocation");
	ID3D11ShaderResourceView* view = nullptr;
	Require(Dds::LoadTexture(device, fixture.path.c_str(), &view) == E_FAIL && !view,
		"Huge invalid DDS must reject the magic without allocation");
	std::puts("PASS DDS rejection: truncated headers/payloads, dimensions, mip counts, arrays, sparse 16 GiB input");
}

void CheckDisplacementBound()
{
	Params p;
	p.stamps[0][3] = 256;
	p.stampParams[0][3] = 512;
	p.rimShape[1] = 8; p.weather[3] = 8;
	Require(Clipmap::SubmittedDisplacementBound(p) >= 256 * 9 + 512 * 9,
		"Bound must include allowed deep presses, tall rims and noise");
	p.stampMotion[0][2] = 1; p.snowRim[1] = 16; p.snowRim[3] = 16;
	Require(Clipmap::SubmittedDisplacementBound(p) >= 256 * 17 + 512 * 17,
		"Bound must include independent snow shaping");
	p.stampParams[0][2] = 1; p.stampParams[0][3] = -900;
	Require(Clipmap::SubmittedDisplacementBound(p) >= 900, "Melt floor must bound heat and body hollows");
	p.stampParams[0][3] = 900;
	Require(Clipmap::SubmittedDisplacementBound(p) >= 900, "Melt floor must bound burial mounds");
	p.stampParams[0][2] = 2;
	const float printed = Clipmap::SubmittedDisplacementBound(p);
	for (float mask : { -100.0f, 0.0f, 0.3f, 0.8f, 1.0f, 100.0f }) {
		Require(std::abs(Clipmap::PrintHeightFor(mask, 256, 900)) <= printed,
			"DDS mask values outside [0,1] must still fit the print bound");
	}
	p.stamps[0][3] = std::numeric_limits<float>::quiet_NaN();
	Require(std::isinf(Clipmap::SubmittedDisplacementBound(p)), "Unknown bound must disable culling");
	p.stamps[0][3] = std::numeric_limits<float>::max();
	p.stampParams[0][2] = 0;
	Require(std::isinf(Clipmap::SubmittedDisplacementBound(p)), "Overflowed bound must disable culling");
	p.control[1] = 0;
	Require(Clipmap::SubmittedDisplacementBound(p) == 0, "Empty batches add no displacement bound");
	std::puts("PASS displacement bounds: deep/noisy presses, snow, heat, burial, print normalization and unknown bounds");
}

// The world position of texel (x, y) of the test field (centred on cell 0): texel i holds
// the centre of cell i, wrapped into [-N/2, N/2).
float WorldOf(int a_texel)
{
	const int cell = a_texel < static_cast<int>(N) / 2 ? a_texel : a_texel - static_cast<int>(N);
	return static_cast<float>(cell) + 0.5f;
}

class Field
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11ComputeShader> shader;
	ComPtr<ID3D11ComputeShader> denseShader, selectShader;
	Clipmap::SparseWork work;
	bool denseDispatch{ g_denseDispatch };
	ComPtr<ID3D11Texture2D> height, metadata, readHeight, readMeta;
	ComPtr<ID3D11UnorderedAccessView> heightUAV, metaUAV;
	std::array<ComPtr<ID3D11Texture2D>, 2> activity;
	std::array<ComPtr<ID3D11UnorderedAccessView>, 2> activityUAV;
	std::array<ComPtr<ID3D11ShaderResourceView>, 2> activitySRV;
	ComPtr<ID3D11ShaderResourceView> busySRV;
	UINT n{ N };
	uint32_t current{ 0 };
	uint32_t updates{ 0 };
	ComPtr<ID3D11ShaderResourceView> coarseHeight, coarseMeta;
	ComPtr<ID3D11ShaderResourceView> shape;
	ComPtr<ID3D11Texture2D> coverage, meshCap;
	ComPtr<ID3D11ShaderResourceView> coverageSRV, meshCapSRV;
	ComPtr<ID3D11Buffer> cb;
	ID3D11Buffer* halves[2]{};
	ComPtr<ID3D11SamplerState> sampler;
public:
	explicit Field(UINT a_size = N, Field* a_shared = nullptr) :
		n(a_size)
	{
		static ComPtr<ID3D11Device> hookDevice;
		static ComPtr<ID3D11DeviceContext> hookContext;
		if (a_shared) {
			device = a_shared->device;
			context = a_shared->context;
		} else if (g_drawHooks && hookDevice) {
			device = hookDevice;
			context = hookContext;
		} else {
			const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
			Check(D3D11CreateDevice(nullptr, g_driver, nullptr, 0, &level, 1,
				D3D11_SDK_VERSION, &device, nullptr, &context), "Create requested D3D11 device (no fallback)");
			if (g_drawHooks) {
				hookDevice = device;
				hookContext = context;
			}
		}
		if (g_drawHooks) Require(ShaderRegistry::Install(device.Get()), "Install production draw hooks");
		const auto source = Clipmap::UpdateShaderSource();
		const auto count = std::to_string(Clipmap::kMaxStamps);
		const auto compile = [&](const char* entry, bool sparse, ID3D11ComputeShader** output) {
			ComPtr<ID3DBlob> code, errors;
			const D3D_SHADER_MACRO defines[]{ { "MAX_STAMPS", count.c_str() },
				{ sparse ? "SPARSE_DISPATCH" : nullptr, "1" }, {} };
			const auto hr = D3DCompile(source.data(), source.size(), "StampSurfaceTest", defines,
				nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr) && errors) { std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer())); }
			Check(hr, "Compile shipping stamp shader");
			Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, output), "Create shader");
		};
		compile("main", true, &shader);
		compile("main", false, &denseShader);
		compile("selectPairs", true, &selectShader);
		Require(work.Create(device.Get(), n), "Sparse dispatch buffers");
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = n;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		Check(device->CreateTexture2D(&desc, nullptr, &height), "Create height");
		Check(device->CreateUnorderedAccessView(height.Get(), nullptr, &heightUAV), "Height UAV");
		desc.Format = DXGI_FORMAT_R32_UINT;
		Check(device->CreateTexture2D(&desc, nullptr, &metadata), "Create metadata");
		Check(device->CreateUnorderedAccessView(metadata.Get(), nullptr, &metaUAV), "Metadata UAV");
		desc.Width = desc.Height = Clipmap::kActivityTexels;
		for (size_t i = 0; i < 2; ++i) {
			Check(device->CreateTexture2D(&desc, nullptr, &activity[i]), "Create activity");
			Check(device->CreateUnorderedAccessView(activity[i].Get(), nullptr, &activityUAV[i]), "Activity UAV");
			Check(device->CreateShaderResourceView(activity[i].Get(), nullptr, &activitySRV[i]), "Activity SRV");
		}
		// An activity map that calls every tile busy: the reference the skipping is checked against.
		std::vector<uint32_t> busy(Clipmap::kActivityTexels * Clipmap::kActivityTexels, 0x7F7FFFFFu);
		D3D11_SUBRESOURCE_DATA busyData{ busy.data(), Clipmap::kActivityTexels * sizeof(uint32_t), 0 };
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> busyTexture;
		Check(device->CreateTexture2D(&desc, &busyData, &busyTexture), "Busy activity");
		Check(device->CreateShaderResourceView(busyTexture.Get(), nullptr, &busySRV), "Busy SRV");
		desc.Width = desc.Height = n;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		Check(device->CreateTexture2D(&desc, nullptr, &readHeight), "Height readback");
		desc.Format = DXGI_FORMAT_R32_UINT;
		Check(device->CreateTexture2D(&desc, nullptr, &readMeta), "Metadata readback");
		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = sizeof(Clipmap::UpdateParams);
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		buffer.Usage = D3D11_USAGE_DYNAMIC;
		buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		Check(device->CreateBuffer(&buffer, nullptr, &cb), "Params buffer");
		Require(Clipmap::CreateHalfBuffers(device.Get(), halves), "Checkerboard buffers");
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&sd, &sampler), "Sampler");

		D3D11_TEXTURE2D_DESC map{};
		map.Width = map.Height = 4;
		map.MipLevels = map.ArraySize = map.SampleDesc.Count = 1;
		map.Format = DXGI_FORMAT_R32_FLOAT;
		map.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		Check(device->CreateTexture2D(&map, nullptr, &coverage), "Create coverage");
		Check(device->CreateTexture2D(&map, nullptr, &meshCap), "Create mesh cap");
		Check(device->CreateShaderResourceView(coverage.Get(), nullptr, &coverageSRV), "Coverage SRV");
		Check(device->CreateShaderResourceView(meshCap.Get(), nullptr, &meshCapSRV), "Mesh cap SRV");
		SetFloorMaps(1.0f, 1.0f);
		Clear();
	}
	~Field()
	{
		for (auto*& half : halves) {
			if (half) { half->Release(); half = nullptr; }
		}
	}
	ID3D11Device* Device() { return device.Get(); }
	ID3D11DeviceContext* Context() { return context.Get(); }
	UINT Size() const { return n; }
	void SetDenseDispatch(bool value) { denseDispatch = value; }
	uint32_t SelectedPairs()
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 3 * sizeof(uint32_t);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Buffer> readback;
		Check(device->CreateBuffer(&desc, nullptr, &readback), "Work count readback");
		context->CopyResource(readback.Get(), work.args);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read work count");
		const auto count = *static_cast<const uint32_t*>(mapped.pData);
		context->Unmap(readback.Get(), 0);
		return count;
	}
	void UseCoarse(Field& a_coarse)
	{
		Check(device->CreateShaderResourceView(a_coarse.height.Get(), nullptr, &coarseHeight), "Live coarse field SRV");
		Check(device->CreateShaderResourceView(a_coarse.metadata.Get(), nullptr, &coarseMeta), "Live coarse metadata SRV");
	}
	void LoadShape(const std::filesystem::path& a_path)
	{
		Check(Dds::LoadTexture(device.Get(), a_path.c_str(), shape.ReleaseAndGetAddressOf()), "Load shipping footprint DDS");
	}
	void UnloadShape() { shape.Reset(); }

	// Runs the plugin's own SavedState against occupied and empty slots, with the
	// neighbouring slots it must not touch set to the opposite pattern.
	void CheckTessellationResources()
	{
		using Tessellation::SavedState;
		ComPtr<ID3D11ShaderResourceView> view;
		Check(device->CreateShaderResourceView(height.Get(), nullptr, &view), "State test SRV");
		for (unsigned phase = 0; phase < 2; ++phase) {
			const auto pick = [&](UINT a_slot, auto* a_value) { return (a_slot + phase) % 2 ? a_value : decltype(a_value){}; };
			for (UINT slot = 0; slot < 14; ++slot) {
				auto* value = pick(slot, cb.Get());
				context->DSSetConstantBuffers(slot, 1, &value);
				context->HSSetConstantBuffers(slot, 1, &value);
			}
			for (UINT slot = 0; slot < 8; ++slot) {
				auto* srv = pick(slot, view.Get());
				auto* state = pick(slot, sampler.Get());
				context->DSSetShaderResources(slot, 1, &srv);
				context->HSSetShaderResources(slot, 1, &srv);
				context->DSSetSamplers(slot, 1, &state);
				context->HSSetSamplers(slot, 1, &state);
			}
			const auto topology = phase ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			context->IASetPrimitiveTopology(topology);

			SavedState saved;
			saved.Capture(context.Get());

			// What a routed draw does to the slots it owns.
			for (UINT slot = SavedState::kFirstCB; slot < SavedState::kFirstCB + SavedState::kCBCount; ++slot) {
				auto* value = pick(slot, cb.Get()) ? nullptr : cb.Get();
				context->DSSetConstantBuffers(slot, 1, &value);
				context->HSSetConstantBuffers(slot, 1, &value);
			}
			for (UINT slot = 0; slot < SavedState::kDomainSRVs; ++slot) {
				auto* srv = pick(slot, view.Get()) ? nullptr : view.Get();
				context->DSSetShaderResources(slot, 1, &srv);
			}
			for (UINT slot = 0; slot < SavedState::kHullSRVs; ++slot) {
				auto* srv = pick(slot, view.Get()) ? nullptr : view.Get();
				context->HSSetShaderResources(slot, 1, &srv);
			}
			for (UINT slot = 0; slot < SavedState::kDomainSamplers; ++slot) {
				auto* state = pick(slot, sampler.Get()) ? nullptr : sampler.Get();
				context->DSSetSamplers(slot, 1, &state);
			}
			for (UINT slot = 0; slot < SavedState::kHullSamplers; ++slot) {
				auto* state = pick(slot, sampler.Get()) ? nullptr : sampler.Get();
				context->HSSetSamplers(slot, 1, &state);
			}
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

			saved.Restore(context.Get());

			for (UINT slot = 0; slot < 14; ++slot) {
				ComPtr<ID3D11Buffer> domain, hull;
				context->DSGetConstantBuffers(slot, 1, &domain);
				context->HSGetConstantBuffers(slot, 1, &hull);
				auto* expected = pick(slot, cb.Get());
				Require(domain.Get() == expected && hull.Get() == expected, "Restore HS/DS constant buffers");
			}
			for (UINT slot = 0; slot < 8; ++slot) {
				ComPtr<ID3D11ShaderResourceView> domain, hull;
				ComPtr<ID3D11SamplerState> domainState, hullState;
				context->DSGetShaderResources(slot, 1, &domain);
				context->HSGetShaderResources(slot, 1, &hull);
				context->DSGetSamplers(slot, 1, &domainState);
				context->HSGetSamplers(slot, 1, &hullState);
				auto* expected = pick(slot, view.Get());
				Require(domain.Get() == expected && hull.Get() == expected, "Restore HS/DS resources");
				Require(domainState.Get() == pick(slot, sampler.Get()) && hullState.Get() == pick(slot, sampler.Get()),
					"Restore HS/DS samplers");
			}
			D3D11_PRIMITIVE_TOPOLOGY restored{};
			context->IAGetPrimitiveTopology(&restored);
			Require(restored == topology, "Restore topology");
		}
		context->ClearState();
		std::puts("PASS tessellation state restoration (the plugin's SavedState): CBs, SRVs, samplers, topology");
	}
	void CheckBlanketBudget()
	{
		const std::string source = std::string(BlanketTessellation::source) + R"(
RWTexture2D<float> Result : register(u0);
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    float values[8] = {
        BlanketDetail(128, 2, 64, 8, 64, 0),
        BlanketDetail(128, 2, 64, 8, 64, 1),
        BlanketDetail(128, 2, 64, 8, 64, 0.5),
        BlanketDetail(128, 2, 64, 0, 64, 0),
        BlanketDetail(128, 2, 8, 8, 8, 0),
        BlanketDetail(128, 2, 4, 8, 64, 0),
        BlanketDetail(128, 32, 64, 8, 64, 0),
        BlanketDetail(128, 2, 64, 8, 64, -1)
    };
    Result[id.xy] = values[id.x];
})";
		ComPtr<ID3DBlob> code, errors;
		Check(D3DCompile(source.data(), source.size(), "BlanketBudgetTest", nullptr,
			nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "Compile blanket budget");
		ComPtr<ID3D11ComputeShader> budget;
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &budget), "Create blanket budget shader");
		context->CSSetShader(budget.Get(), nullptr, 0);
		context->CSSetUnorderedAccessViews(0, 1, heightUAV.GetAddressOf(), nullptr);
		context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* empty = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
		const auto result = Read();
		const float expected[]{16, 64, 40, 2, 8, 4, 32, 16};
		for (size_t i = 0; i < 8; ++i) {
			Require(std::abs(result[i] - expected[i]) < 0.001f, "Blanket floor, detail limits and activity regression");
		}
		Clear();
		std::puts("PASS GPU blanket budget: empty snow, active marks, interpolation, off, caps and finer user baseline");
	}

	void SetFloorMaps(float a_cover, float a_cap)
	{
		std::array<float, 16> cover{};
		std::array<float, 16> cap{};
		cover.fill(a_cover);
		cap.fill(a_cap);
		context->UpdateSubresource(coverage.Get(), 0, nullptr, cover.data(), 4 * sizeof(float), 0);
		context->UpdateSubresource(meshCap.Get(), 0, nullptr, cap.data(), 4 * sizeof(float), 0);
	}
	void CheckPatchCulling()
	{
		const auto savedSettings = Settings::values;
		Settings::useClipmap = true;
		Settings::enablePatchCulling = true;
		Settings::snowRaiseHeight = 0;
		Settings::debugWorldZOffset = Settings::debugWaveAmplitude = 0;
		Reflection::Signature signature{};
		signature.valid = true;
		Reflection::SignatureElement position{};
		position.semanticName = "SV_POSITION";
		position.mask = 15;
		position.componentType = D3D_REGISTER_COMPONENT_FLOAT32;
		position.hlslType = "float4";
		signature.elements.push_back(position);
		const auto hull = Tessellation::HullSource(signature, Tessellation::Mode::kLandscape);
		Settings::values = savedSettings;
		const auto begin = hull.find("static const float kCullMargin");
		const auto end = hull.find("PatchConstants PatchConstantFn", begin);
		Require(begin != std::string::npos && end != std::string::npos, "Find production patch culling function");
		const std::string source = R"(
static const row_major float4x4 CameraViewProj = float4x4(
    1.0/1024,0,0,0, 0,0,1.0/128,0, 0,0,0,0.5, 0,0,0,1);
static float4 Raise;
)" + hull.substr(begin, end - begin) + R"(
RWTexture2D<float> Result : register(u0);
[numthreads(5,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    Raise = 0;
    if (id.x == 1) Raise.y = 256;
    if (id.x == 2) Raise.y = asfloat(0x7F800000u);
    if (id.x == 3) Raise.y = asfloat(0x7FC00000u);
    float3 p = id.x == 4 ? float3(5000,0,0) : float3(0,0,350);
    Result[id.xy] = PatchOutside(p, p+float3(1,0,0), p+float3(0,1,0)) ? 1 : 0;
})";
		ComPtr<ID3DBlob> code, errors;
		const auto hr = D3DCompile(source.data(), source.size(), "PatchCullingTest", nullptr,
			nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer()));
		Check(hr, "Compile production patch culling function");
		ComPtr<ID3D11ComputeShader> culling;
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &culling), "Create patch culling shader");
		Clear();
		context->CSSetShader(culling.Get(), nullptr, 0);
		context->CSSetUnorderedAccessViews(0, 1, heightUAV.GetAddressOf(), nullptr);
		context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* empty = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
		const auto result = Read();
		Require(result[0] == 1 && result[1] == 0 && result[2] == 0 && result[3] == 0 && result[4] == 1,
			"Patch culling must retain deep displaced geometry and unknown bounds while culling safely outside patches");
		Clear();
		std::puts("PASS production GPU patch culling: old 128-unit miss, dynamic deep bound, infinity/NaN and safe culling");
	}
	void Clear()
	{
		const float zero[4]{};
		const UINT zeroUint[4]{};
		context->ClearUnorderedAccessViewFloat(heightUAV.Get(), zero);
		context->ClearUnorderedAccessViewUint(metaUAV.Get(), zeroUint);
		context->ClearUnorderedAccessViewUint(activityUAV[0].Get(), zeroUint);
		context->ClearUnorderedAccessViewUint(activityUAV[1].Get(), zeroUint);
		// A fresh field starts the checkerboard's alternation over, as a run would.
		current = 0;
		updates = 0;
	}
	// One update, exactly as the plugin dispatches it. a_everyTile hands the shader an
	// activity map with every tile busy, so nothing is skipped.
	void Step(const Params& params, bool seed = false, bool a_everyTile = false)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map shipping update parameters");
		std::memcpy(mapped.pData, &params, sizeof(params));
		context->Unmap(cb.Get(), 0);
		ID3D11ShaderResourceView* srvs[]{ shape.Get(), nullptr, nullptr, coverageSRV.Get(), meshCapSRV.Get() };
		context->CSSetShader(denseDispatch ? denseShader.Get() : shader.Get(), nullptr, 0);
		context->CSSetShaderResources(0, 5, srvs);
		context->CSSetSamplers(0, 1, sampler.GetAddressOf());
		context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());

		const uint32_t write = current ^ 1u;
		Clipmap::LevelViews views{};
		views.field = heightUAV.Get();
		views.meta = metaUAV.Get();
		views.activity = activityUAV[write].Get();
		views.prevActivity = a_everyTile ? busySRV.Get() : activitySRV[current].Get();
		views.coarseField = seed ? coarseHeight.Get() : nullptr;
		views.coarseMeta = seed ? coarseMeta.Get() : nullptr;
		Clipmap::DispatchLevel(context.Get(), views, halves, updates++ & 1u, n,
			denseDispatch ? nullptr : &work, selectShader.Get(), shader.Get());
		current = write;

		ID3D11ShaderResourceView* nullSRVs[5]{};
		context->CSSetShaderResources(0, 5, nullSRVs);
	}
	std::vector<float> Read()
	{
		context->CopyResource(readHeight.Get(), height.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readHeight.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read height");
		std::vector<float> result(n * n);
		for (UINT y = 0; y < n; ++y) {
			const auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
			std::copy_n(row, n, result.begin() + y * n);
		}
		context->Unmap(readHeight.Get(), 0);
		return result;
	}
	std::vector<uint32_t> ReadMeta()
	{
		context->CopyResource(readMeta.Get(), metadata.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readMeta.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read metadata");
		std::vector<uint32_t> result(n * n);
		for (UINT y = 0; y < n; ++y) {
			const auto row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
			std::copy_n(row, n, result.begin() + y * n);
		}
		context->Unmap(readMeta.Get(), 0);
		return result;
	}
	std::vector<uint32_t> ReadActivity()
	{
		D3D11_TEXTURE2D_DESC desc{};
		activity[current]->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> readback;
		Check(device->CreateTexture2D(&desc, nullptr, &readback), "Activity readback texture");
		context->CopyResource(readback.Get(), activity[current].Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read activity");
		std::vector<uint32_t> result(desc.Width * desc.Height);
		for (UINT y = 0; y < desc.Height; ++y) {
			const auto row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
			std::copy_n(row, desc.Width, result.begin() + y * desc.Width);
		}
		context->Unmap(readback.Get(), 0);
		return result;
	}
	bool SnowAtOrigin()
	{
		return (ReadMeta()[0] & Clipmap::kMetaSnowBit) != 0;
	}
	void SaveCoarse()
	{
		D3D11_TEXTURE2D_DESC desc{};
		for (int i = 0; i < 2; ++i) {
			auto* source = i ? metadata.Get() : height.Get();
			source->GetDesc(&desc);
			ComPtr<ID3D11Texture2D> copy;
			Check(device->CreateTexture2D(&desc, nullptr, &copy), "Coarse copy");
			context->CopyResource(copy.Get(), source);
			Check(device->CreateShaderResourceView(copy.Get(), nullptr,
				i ? &coarseMeta : &coarseHeight), "Coarse SRV");
		}
	}
};

void CheckDisplacementBoundGpu(Field& field)
{
	field.Clear();
	float retained = 0;
	const auto step = [&](const Params& p, bool seed = false) {
		retained = std::max(retained, Clipmap::SubmittedDisplacementBound(p));
		field.Step(p, seed);
		for (const float height : field.Read()) {
			Require(std::isfinite(height) && std::abs(height) <= retained,
				"Production GPU height exceeded the retained displacement bound");
		}
	};
	Params p;
	p.stamps[0][2] = 12;
	p.stamps[0][3] = 256;
	p.stampParams[0][3] = 512;
	p.stampParams[0][1] = 1;
	p.rimShape[1] = p.weather[3] = 8;
	step(p);
	p.stampMotion[0][2] = 1;
	p.snowRim[1] = p.snowRim[3] = 16;
	step(p);
	field.SaveCoarse();
	p.control[0] = 0.25f;
	p.stampParams[0][2] = 1;
	p.stamps[0][3] = 1;
	p.stampParams[0][3] = -900;
	step(p);
	p.stampParams[0][3] = 900;
	step(p);
	p.control[1] = 0;  // original marks survive this settings change
	p.weather[0] = 0.2f; p.weather[1] = 0.5f;
	p.repose[0] = p.repose[1] = 1;
	p.raise[0] = 35; p.raise[2] = 2;
	for (unsigned i = 0; i < 12; ++i) step(p);
	p.coarse[0] = 1; p.coarse[2] = 1; p.coarse[3] = 0;
	step(p, true);  // smoothed historical coarse data, with no new stamps
	field.Clear();
	std::puts("PASS GPU displacement bound: deep/noisy stamps, snow, heat, burial, retained history, repose, floor and coarse reseeding");
}

void CheckGroundFloor(Field& field)
{
	const auto deepest = [](const std::vector<float>& a_field) {
		return *std::min_element(a_field.begin(), a_field.end());
	};

	Params p;
	p.stampMotion[0][2] = 1;
	p.stamps[0][3] = 40.0f;
	field.SetFloorMaps(1.0f, 1.0f);

	field.Clear(); field.Step(p);
	const auto unfloored = field.Read();
	Require(deepest(unfloored) < -20.0f, "The test stamp must overshoot every blanket below");

	p.raise[0] = 10.0f;
	field.Clear(); field.Step(p);
	const float floored = deepest(field.Read());
	Require(floored > -10.001f, "A snow mark cut below the blanket standing over it");
	Require(floored < -9.999f, "The floor is not the blanket - the mark stopped short of the land");

	p.raise[2] = 3.0f;
	field.Clear(); field.Step(p);
	const float bitten = deepest(field.Read());
	Require(bitten > -13.001f && bitten < -12.999f, "SnowGroundBite is not the floor's slack");
	p.raise[2] = 0.0f;

	p.raise[3] = 1.0f;
	field.SetFloorMaps(1.0f, 0.5f);
	field.Clear(); field.Step(p);
	const float capped = deepest(field.Read());
	Require(capped > -5.001f && capped < -4.999f, "The mesh cap did not carry into the floor");
	p.raise[3] = 0.0f;

	field.SetFloorMaps(0.0f, 1.0f);
	field.Clear(); field.Step(p);
	Require(deepest(field.Read()) > -0.001f, "Ground with no blanket still took a snow cut");
	field.SetFloorMaps(1.0f, 1.0f);

	p.stampMotion[0][2] = 0;
	p.raise[0] = 0.0f;
	field.Clear(); field.Step(p);
	const auto mud = field.Read();
	p.raise[0] = 10.0f;
	field.Clear(); field.Step(p);
	Require(field.Read() == mud, "The floor reached ground that no snow stamp claimed");

	field.Clear();
	std::puts("PASS snow ground floor: blanket, bite, mesh cap, bare ground and other surfaces");
}

void CheckImpactPatterns(Field& field)
{
	const auto draw = [&](const ImpactPatterns::Pattern& pattern, bool snow, float span, float lean) {
		Params p;
		p.control[1] = static_cast<float>(pattern.count);
		p.control[3] = span; p.rimShape[0] = lean;
		p.snowRim[0] = span; p.snowRim[2] = lean;
		for (size_t i = 0; i < pattern.count; ++i) {
			const auto& s = pattern.strokes[i];
			p.stamps[i][0] = s.x; p.stamps[i][1] = s.y;
			p.stamps[i][2] = ImpactPatterns::StampRadius(s.radius, 1.0f, span, lean);
			p.stamps[i][3] = s.strength * 3.0f;
			p.stampParams[i][0] = 0.25f; p.stampParams[i][1] = 0.92f;
			p.stampParams[i][3] = s.strength * 0.8f;
			p.stampMotion[i][0] = s.motionX; p.stampMotion[i][1] = s.motionY;
			p.stampMotion[i][2] = snow ? 1.0f : 0.0f;
		}
		field.Clear(); field.Step(p);
		const auto first = field.Read();
		field.Step(p);
		Require(field.Read() == first, "Repeated impact must not deepen or accumulate rims");
		return first;
	};
	Require(ImpactPatterns::BoundedRadius(100000, 2, 256) == 256, "Explosion radius cap failed");
	Require(ImpactPatterns::BoundedRadius(std::numeric_limits<float>::infinity(), 1, 256) == 0,
		"Non-finite explosion radius must be rejected");
	Require(ImpactPatterns::Directional(20, 8, 0, 0, true).count == 0 &&
		ImpactPatterns::Directional(0, 8, 0, 1, true).count == 0, "Invalid direction/range must not stamp");
	for (bool snow : { false, true }) {
		for (float span : { 0.0f, 0.8f, 4.0f }) {
			const auto explosion = draw(ImpactPatterns::Explosion(20), snow, span, 1);
			Require(*std::min_element(explosion.begin(), explosion.end()) < -0.1f,
				"Explosion must deform both land and snow");
			for (int y = 0; y < static_cast<int>(N); ++y) {
				for (int x = 0; x < static_cast<int>(N); ++x) {
					const float value = explosion[y * N + x];
					Require(std::isfinite(value), "Impact generated non-finite displacement");
					if (std::hypot(WorldOf(x), WorldOf(y)) > 21) {
						Require(value == 0, "Explosion affected ground beyond radius cap");
					}
				}
			}
		}
		const auto forward = draw(ImpactPatterns::Directional(20, 10, 0, 1, true), snow, 0.8f, 1);
		Require(forward[14 * N] < -0.1f && forward[(N - 10) * N] == 0,
			"A directional stroke must disturb ground ahead, not behind");
		const auto sideways = draw(ImpactPatterns::Directional(20, 10, 1, 0, true), snow, 0.8f, 1);
		Require(sideways[14] < -0.1f && sideways[14 * N] == 0,
			"A directional stroke did not turn with its direction");
	}
	std::puts("PASS impact patterns: land/snow, directional strokes, radius caps and idempotence");
}

// Skipping flat, untouched tiles must give exactly what updating every tile gives, over
// a run that stamps, decays, settles, moves stamps away and lets marks recover.
void CheckQuietTiles()
{
	// 512 texels: eight activity cells to a side, so marked and flat tiles coexist.
	Field field(512);
	const auto run = [&](bool a_everyTile) {
		field.SetDenseDispatch(a_everyTile);
		field.Clear();
		std::vector<std::vector<float>> frames;
		for (int frame = 0; frame < 40; ++frame) {
			Params p(field.Size());
			// Twenty frames of a moving stamp at 30 Hz, then twenty of the longest step to let
			// the trail recover completely.
			p.control[0] = frame < 20 ? 1.0f / 30.0f : 0.25f;
			p.weather[1] = 0.3f;
			p.repose[0] = Clipmap::ReposeAlpha(0.2f, p.control[0], 0);
			p.stamps[0][0] = frame < 20 ? -20.0f + 2.0f * static_cast<float>(frame) : 0.0f;
			p.stamps[0][1] = 8.0f;
			p.stamps[0][3] = 6.0f;
			p.stampParams[0][1] = 0.1f;
			p.control[1] = frame < 20 ? 1.0f : 0.0f;
			field.Step(p, false, a_everyTile);
			frames.push_back(field.Read());
		}
		return frames;
	};
	const auto skipping = run(false);
	const auto everyTile = run(true);
	Require(skipping == everyTile, "Skipping quiet tiles changed the field");
	const auto& last = skipping.back();
	Require(*std::min_element(last.begin(), last.end()) == 0.0f,
		"A recovered mark must flatten to exactly 0 so its tiles go quiet");
	std::puts("PASS quiet tiles: skipping matches updating every tile; recovered marks flatten to 0");
}

// Compare every output texel, its recovery/snow metadata and the dilated hull activity
// to the original full dispatch after each frame, including both checkerboard orders.
void CheckSparseDispatch()
{
	Field coarse(512), fine(512, &coarse), referenceCoarse(512, &coarse), reference(512, &coarse);
	fine.UseCoarse(coarse);
	reference.UseCoarse(referenceCoarse);
	referenceCoarse.SetDenseDispatch(true);
	reference.SetDenseDispatch(true);
	int previous[2][2]{};
	bool sawReducedWork = false;
	for (int frame = 0; frame < 85; ++frame) {
		for (unsigned level = 2; level-- > 0;) {
			Params p(512);
			p.window[2] = level ? 4.0f : 1.0f;
			p.window[0] = frame < 25 ? 0.0f : frame < 40 ? std::floor((-17.0f * (frame - 24)) / p.window[2]) : -77.0f;
			p.window[1] = frame < 25 ? 0.0f : std::floor(39.0f / p.window[2]);
			const int moved = std::max(std::abs(static_cast<int>(p.window[0]) - previous[level][0]),
				std::abs(static_cast<int>(p.window[1]) - previous[level][1]));
			previous[level][0] = static_cast<int>(p.window[0]);
			previous[level][1] = static_cast<int>(p.window[1]);
			p.coarse[0] = level ? 0.0f : 1.0f;
			p.coarse[1] = 512 * 4.0f;
			p.coarse[2] = 0.65f;
			p.coarse[3] = frame == 40 ? 0.0f : std::max(0.0f, 252.0f - moved);
			p.control[0] = frame == 15 ? 0.0f : frame > 45 ? 0.25f : 1.0f / 30;
			p.control[1] = frame < 20 || frame == 84 ? 64.0f : frame < 35 ? 8.0f : 0.0f;
			p.weather[0] = frame > 42 ? 0.95f : 0.0f;
			p.weather[1] = 0.3f;
			p.repose[0] = p.repose[1] = Clipmap::ReposeAlpha(0.2f, p.control[0], level);
			p.raise[0] = frame > 30 ? 4.0f : 24.0f;
			p.raise[3] = 1;
			for (unsigned i = 0; i < 64; ++i) {
				p.stamps[i][0] = -245.0f + (i % 8) * 70.0f + frame * 0.7f;
				p.stamps[i][1] = -245.0f + (i / 8) * 70.0f;
				p.stamps[i][2] = i % 3 ? 7.0f : 17.0f;
				p.stamps[i][3] = 6;
				p.stampParams[i][0] = 0.3f;
				p.stampParams[i][1] = 0.1f;
				p.stampParams[i][2] = frame >= 20 && frame < 35 ? 1.0f : 0.0f;
				p.stampParams[i][3] = p.stampParams[i][2] > 0 ? -16.0f : 2.0f;
				p.stampMotion[i][0] = -11;
				p.stampMotion[i][1] = 19;
				p.stampMotion[i][2] = static_cast<float>(i & 1u);
				p.stampShape[i][0] = 0.6f;
				p.stampShape[i][1] = 0.8f;
				p.stampShape[i][2] = i % 2 ? 4.0f : 0.0f;
				p.stampShape[i][3] = 1;
			}
			auto& actual = level ? coarse : fine;
			auto& expected = level ? referenceCoarse : reference;
			actual.Step(p, !level);
			expected.Step(p, !level);
			if (actual.Read() != expected.Read() || actual.ReadMeta() != expected.ReadMeta() ||
				actual.ReadActivity() != expected.ReadActivity()) {
				std::fprintf(stderr, "Sparse mismatch frame %d level %u\n", frame, level);
				Require(false, "Sparse/dense height, metadata and activity must match exactly");
			}
			if (frame == 83) { sawReducedWork |= actual.SelectedPairs() < (512 / 16) * (512 / 8) / 4; }
		}
	}
	Require(sawReducedWork, "Recovered field must stop dispatching its interior");
	fine.Clear();
	Params p(512);
	p.control[1] = 0;
	p.coarse[3] = 512; // No boundary work: exercise an empty indirect dispatch.
	fine.Step(p);
	Require(fine.SelectedPairs() == 0, "Flat field without boundary work must dispatch zero tile pairs");
	p.control[1] = 1;
	fine.Step(p);
	Require(fine.SelectedPairs() > 0 && fine.Read()[0] < 0, "A new stamp must wake an empty work list");
	std::puts("PASS sparse dispatch: exact dense equivalence over 85 frames, recovery, heat, weather, pause, 64 stamps, wrap, coarse reseeding and empty-list wakeup");
}

// The window follows the player: a mark stays at its world cell, and rows that wrap
// round to the far side come in flat (with no coarser level), not as the old ground.
void CheckMovingWindow(Field& field)
{
	field.Clear();
	Params p;
	p.stamps[0][0] = 0.5f; p.stamps[0][1] = 0.5f; p.stamps[0][2] = 3;
	p.stamps[1][0] = -25.5f; p.stamps[1][1] = 0.5f; p.stamps[1][2] = 3; p.stamps[1][3] = 2;
	p.stampParams[1][0] = 0.3f; p.stampParams[1][1] = 0.92f; p.stampParams[1][3] = 1;
	p.control[1] = 2;
	field.Step(p);
	const auto before = field.Read();
	const int  wrapped = 38;  // cell -26, inside the window's reseeded edge
	Require(before[0] < -0.1f && before[wrapped] < -0.1f, "Both test marks must be pressed");

	Params moved;
	moved.control[1] = 0;
	moved.window[0] = 10;  // ten cells east
	moved.coarse[3] = static_cast<float>(N / 2 - 2 - 10 - 2);
	field.Step(moved);
	const auto after = field.Read();
	Require(after[0] == before[0], "A mark moved with the window instead of staying in its cell");
	Require(after[wrapped] == 0.0f, "A row that wrapped round kept the ground it showed before the move");

	// Negative centres and the seam: the same, the other way.
	Params west;
	west.control[1] = 0;
	west.window[0] = -5;
	west.coarse[3] = static_cast<float>(N / 2 - 2 - 15 - 2);
	field.Step(west);
	Require(field.Read()[0] == before[0], "A mark was lost when the window crossed to negative cells");
	field.Clear();
	std::puts("PASS moving window: marks keep their cells, wrapped rows come in flat, negative centres");
}

// Recovery is per second whatever the frame rate, and slow rates keep their precision.
void CheckRecovery(Field& field)
{
	const auto after = [&](int a_frames, float a_rate) {
		field.Clear();
		Params p;
		p.stampParams[0][1] = a_rate;
		p.stamps[0][3] = 8.0f;
		field.Step(p);
		const float start = field.Read()[0];
		Params q;
		q.control[1] = 0;
		q.control[0] = 1.0f / static_cast<float>(a_frames);
		for (int i = 0; i < a_frames; ++i) {
			field.Step(q);
		}
		return std::pair{ start, field.Read()[0] };
	};
	const auto [start30, at30] = after(30, 0.5f);
	const auto [start144, at144] = after(144, 0.5f);
	Require(std::abs(at30 - start30 * 0.5f) < 0.01f && std::abs(at144 - start144 * 0.5f) < 0.01f,
		"A mark must keep its rate per second at 30 and 144 frames a second");

	field.Clear();
	Params p;
	p.stampParams[0][1] = 0.9999f;
	field.Step(p);
	const float stored = Clipmap::DecodeRate(field.ReadMeta()[0]);
	Require(std::abs(stored - 0.9999f) < 2e-5f && stored < 1.0f,
		"A slow recovery rate was rounded to 'never'");
	Require(Clipmap::RecoveryRate(0.99f, 1.05f) < 0.9999f && Clipmap::RecoveryRate(0.99f, 1.05f) > 0.99f,
		"A surface scale above 1 must lengthen a mark's life, not stop its recovery");
	Require(std::abs(Clipmap::RecoveryRate(0.99f, 2.0f) - std::sqrt(0.99f)) < 1e-6f,
		"A lifetime scale of 2 must double the half-life");
	field.Clear();
	std::puts("PASS recovery: per second at any frame rate, 16-bit rates, lifetime scales");
}

// Settling reads only the field as it was before the update, so it is the same every run
// and moves the same amount per second at any frame rate.
void CheckRepose(Field& field)
{
	Require(Clipmap::ReposeAlpha(0.2f, 0.0f, 0) == 0.0f, "Settling must stop while paused");
	Require(std::abs(Clipmap::ReposeAlpha(0.2f, 1.0f / 60.0f, 0) - 0.2f) < 1e-6f,
		"The repose rate is defined per 60 Hz frame");
	Require(std::abs(Clipmap::ReposeAlpha(0.2f, 1.0f / 60.0f, 1) - 0.2f / (Clipmap::kLevelScale * Clipmap::kLevelScale)) < 1e-6f,
		"The coarse level must settle at the same world-space speed");

	const auto settle = [&]() {
		field.Clear();
		// A round mark without the position-hashed rim noise and churn, so the only thing
		// that could break its symmetry is the settling itself.
		Params p;
		p.weather[3] = 0;
		p.rimShape[1] = 0;
		p.stamps[0][3] = 12.0f;
		p.stampParams[0][1] = 1.0f;
		field.Step(p);
		Params q;
		q.control[1] = 0;
		q.weather[1] = 0.05f;
		q.repose[0] = 0.5f;
		for (int i = 0; i < 12; ++i) {
			field.Step(q);
		}
		return field.Read();
	};
	const auto first = settle();
	Require(settle() == first, "Settling gave a different field on a second run");

	// Symmetric: a centred round mark settles the same in every direction.
	const auto at = [&](int x, int y) { return first[static_cast<size_t>(y) * N + static_cast<size_t>(x)]; };
	Require(std::abs(at(4, 0) - at(0, 4)) < 1e-4f && std::abs(at(4, 0) - at(N - 5, N - 1)) < 1e-3f,
		"Settling favoured a direction");
	field.Clear();
	std::puts("PASS repose: deterministic, symmetric, paused, per-second and per-level rates");
}

// Seeding from a coarser level: the coarse texel the point falls in decides the rate (no
// blend with untouched ground), at texel centres four fine cells to a coarse one.
void CheckCoarseSeeding(Field& field)
{
	// The coarse level: cells of 4 units, a mark at the origin with a slow rate.
	field.Clear();
	Params coarse;
	coarse.window[2] = 4;
	coarse.stamps[0][2] = 12;
	coarse.stamps[0][3] = 4;
	coarse.stampParams[0][1] = 0.98f;
	field.Step(coarse);
	field.SaveCoarse();

	field.Clear();
	Params fine;
	fine.control[1] = 0;
	fine.coarse[0] = 1;
	fine.coarse[1] = N * 4.0f;
	fine.coarse[3] = 0;
	field.Step(fine, true);
	const auto height = field.Read();
	const auto meta = field.ReadMeta();
	size_t marked = 0;
	for (size_t i = 0; i < height.size(); ++i) {
		if (height[i] < -0.05f) {
			++marked;
			Require(std::abs(Clipmap::DecodeRate(meta[i]) - 0.98f) < 1e-4f,
				"A seeded mark's edge took a blended (faster) recovery rate");
		}
	}
	Require(marked > 100, "Coarse seeding lost the mark");
	field.Clear();
	std::puts("PASS coarse seeding: 4x cells, texel centres, unblended rates");
}

// Scorch (a jet's burn): kept in the metadata's top bits, never changes the height, fades
// at the same rate per second at any frame rate, holds while paused, keeps its tile active
// until it is gone, and leaves the recovery rate and snow flag alone.
void CheckScorch(Field& field)
{
	const auto burn = [](float a_dt) {
		Params p;
		p.control[0] = a_dt;
		p.stamps[0][3] = 0;         // no depth: only the colour
		p.stampParams[0][3] = 0;    // no rim
		p.stampMotion[0][3] = 1;    // a full burn
		p.repose[3] = 0.25f;        // the activity a scorched tile reports
		return p;
	};
	const auto scorchAtOrigin = [&]() { return Clipmap::DecodeScorch(field.ReadMeta()[0]); };

	field.Clear();
	field.Step(burn(1.0f / 60.0f));
	const auto flat = field.Read();
	Require(std::all_of(flat.begin(), flat.end(), [](float h) { return h == 0; }),
		"A burn must not change the height");
	Require(std::abs(scorchAtOrigin() - 1.0f) < 1e-4f, "A full burn did not reach the texel under it");
	Require(Clipmap::DecodeScorch(field.ReadMeta()[N / 2 * N + N / 2]) == 0, "A burn reached far ground");
	const auto active = field.ReadActivity();
	Require(std::any_of(active.begin(), active.end(), [](uint32_t a) { return a != 0; }),
		"A scorched tile must stay active (to fade and to keep its detail)");

	// Fading: a tenth per second, the same at 30 and 144 Hz, frozen while paused.
	const auto fadeAfterOneSecond = [&](int a_frames) {
		field.Clear();
		field.Step(burn(1.0f / 60.0f));
		Params p = burn(1.0f / static_cast<float>(a_frames));
		p.control[1] = 0;
		p.weather[2] = 0.1f;
		for (int i = 0; i < a_frames; ++i) {
			p.repose[2] = static_cast<float>(i * 7919 % 65536);
			field.Step(p);
		}
		return scorchAtOrigin();
	};
	const float at30 = fadeAfterOneSecond(30);
	const float at144 = fadeAfterOneSecond(144);
	Require(std::abs(at30 - 0.9f) < 0.004f && std::abs(at144 - 0.9f) < 0.004f,
		"The scorch must fade by the same amount per second at 30 and 144 Hz");
	{
		Params paused = burn(0);
		paused.control[1] = 0;
		paused.weather[2] = 0.1f;
		const auto before = field.ReadMeta();
		field.Step(paused);
		Require(field.ReadMeta() == before, "The scorch must hold while paused");
	}

	// Gone after its fade; then the tile goes quiet again.
	{
		Params p = burn(1.0f);
		p.control[1] = 0;
		p.weather[2] = 0.5f;
		for (int i = 0; i < 3; ++i) {
			field.Step(p);
		}
		const auto meta = field.ReadMeta();
		Require(std::all_of(meta.begin(), meta.end(), [](uint32_t m) { return Clipmap::DecodeScorch(m) == 0; }),
			"A faded scorch must be gone");
		field.Step(p);
		const auto quiet = field.ReadActivity();
		Require(std::all_of(quiet.begin(), quiet.end(), [](uint32_t a) { return a == 0; }),
			"A tile with no scorch and no mark must go quiet");
	}

	// Over a mark: the burn leaves its depth, rate and snow flag as they were.
	field.Clear();
	{
		Params mark;
		mark.control[0] = 0;
		mark.stamps[0][3] = 4;
		mark.stampParams[0][1] = 0.97f;
		mark.stampMotion[0][2] = 1;  // snow
		field.Step(mark);
	}
	const auto markHeight = field.Read();
	const auto markMeta = field.ReadMeta();
	field.Step(burn(0));
	const auto burntHeight = field.Read();
	const auto burntMeta = field.ReadMeta();
	Require(burntHeight == markHeight, "A burn over a mark changed its depth");
	for (size_t i = 0; i < burntMeta.size(); ++i) {
		Require((burntMeta[i] & 0x1FFFFu) == (markMeta[i] & 0x1FFFFu),
			"A burn changed a mark's recovery rate or snow flag");
	}
	Require(scorchAtOrigin() > 0.99f, "The burn over a mark was lost");
	field.Clear();
	std::puts("PASS scorch: height untouched, 30/144 Hz fade, pause, quiet when gone, marks kept");
}

void CheckHeatAndWeather(Field& field)
{
	const auto meltAfter = [&](int frames) {
		field.Clear();
		Params p;
		p.control[0] = 1.0f / static_cast<float>(frames);
		p.stamps[0][3] = 0.85f;
		p.stampParams[0][1] = 1;
		p.stampParams[0][2] = 1;  // shipping kMelt
		p.stampParams[0][3] = -10;
		for (int i = 0; i < frames; ++i) { field.Step(p); }
		const auto melted = field.Read();
		Require(*std::max_element(melted.begin(), melted.end()) == 0, "Heat must not build a rim");
		Require(field.SnowAtOrigin(), "Heat with a snow floor must retain snow metadata");
		p.control[0] = 0;
		field.Step(p);
		Require(field.Read() == melted, "Heat must stop while paused");
		return melted[0];
	};
	Require(std::abs(meltAfter(30) + 8.5f) < 0.002f && std::abs(meltAfter(144) + 8.5f) < 0.002f,
		"Heat must approach its floor at the same rate per second at 30 and 144 Hz");
	field.Clear();
	Params heat;
	heat.control[0] = 1;
	heat.stamps[0][3] = 0.85f;
	heat.stampParams[0][2] = 1;
	heat.stampParams[0][3] = 0;
	field.Step(heat);
	const auto bare = field.Read();
	Require(std::all_of(bare.begin(), bare.end(), [](float h) { return h == 0; }),
		"Heat on bare, flat ground must not make a crater");
	std::puts("PASS heat update: snow floor, 30/144 Hz, pause, no rim and bare ground");

	// The heat flag: a fire's melt of snow bares the ground under it (the terrain shows the
	// ground there), a body's hollow does not, and ground flat again drops it.
	{
		Params fire;
		fire.control[0] = 1.0f / 60.0f;
		fire.stamps[0][3] = 0.85f;
		fire.stampParams[0][1] = 0.01f;
		fire.stampParams[0][2] = 1;
		fire.stampParams[0][3] = -10;
		fire.stampMotion[0][2] = 3;  // snow, heat
		field.Clear();
		field.Step(fire);
		Require(Clipmap::DecodeHeat(field.ReadMeta()[0]) && field.SnowAtOrigin(),
			"A fire melting snow must bare the ground and keep the snow flag");
		Require(!Clipmap::DecodeHeat(field.ReadMeta()[N / 2 * N + N / 2]), "Heat bared far ground");
		Require(std::abs(Clipmap::DecodeRate(field.ReadMeta()[0]) - 0.01f) < 1e-4f,
			"The heat flag changed the recovery rate");
		Params rest;
		rest.control[0] = 1;
		rest.control[1] = 0;
		for (int i = 0; i < 4; ++i) { field.Step(rest); }
		Require(field.Read()[0] == 0 && !Clipmap::DecodeHeat(field.ReadMeta()[0]),
			"Ground flat again must drop the heat flag");
		fire.stampMotion[0][2] = 1;  // snow: a body's hollow
		field.Clear();
		field.Step(fire);
		Require(!Clipmap::DecodeHeat(field.ReadMeta()[0]), "A melt without heat bared the ground");
		fire.stampMotion[0][2] = 3;
		fire.stampParams[0][3] = 0;  // heat on bare ground: nothing melted
		field.Clear();
		field.Step(fire);
		Require(!Clipmap::DecodeHeat(field.ReadMeta()[0]), "Heat on bare ground marked it");
		field.Clear();
	}
	std::puts("PASS heat flag: a fire bares snow, a body's hollow does not, flat ground drops it");

	const auto filledAfter = [&](int frames) {
		field.Clear();
		Params stamp;
		stamp.stamps[0][3] = 8;
		stamp.stampParams[0][1] = 1;
		field.Step(stamp);
		const auto before = field.Read();
		const auto metadata = field.ReadMeta();
		Params fill;
		fill.control[1] = 0;
		fill.weather[0] = 0.4f;
		field.Step(fill);  // dt = 0
		Require(field.Read() == before && field.ReadMeta() == metadata, "Paused weather must preserve marks exactly");
		fill.control[0] = 1.0f / static_cast<float>(frames);
		for (int i = 0; i < frames; ++i) { field.Step(fill); }
		Require(std::abs(field.Read()[0] - before[0] * 0.6f) < 0.002f,
			"Weather fill must preserve its per-second rate at any frame rate");
	};
	filledAfter(30);
	filledAfter(144);
	std::puts("PASS weather fill: 30/144 Hz and paused height/metadata preservation");
}

void CheckStampCapacity(Field& field)
{
	// The tile masks have two 32-bit words; exercise their boundary and top bits.
	for (uint32_t index : { 0u, 31u, 32u, 63u }) {
		field.Clear();
		Params p;
		for (auto& stamp : p.stamps) { std::fill_n(stamp, 4, 0.0f); }
		p.rimShape[1] = 0;
		p.control[1] = static_cast<float>(index + 1);
		p.stamps[index][2] = 5;
		p.stamps[index][3] = 3;
		p.stampParams[index][0] = 0.3f;
		p.stampParams[index][1] = 1;
		field.Step(p);
		Require(field.Read()[0] < -2.9f, "Stamp mask lost a boundary bit");
	}
	field.Clear();
	Params p;
	p.rimShape[1] = 0;
	p.control[1] = static_cast<float>(Clipmap::kMaxStamps);
	for (uint32_t i = 0; i < Clipmap::kMaxStamps; ++i) {
		p.stamps[i][0] = -24.5f + static_cast<float>(i % 8) * 7;
		p.stamps[i][1] = -24.5f + static_cast<float>(i / 8) * 7;
		p.stamps[i][2] = 2;
		p.stamps[i][3] = 1 + static_cast<float>(i) / 64;
		p.stampParams[i][0] = 0.3f;
		p.stampParams[i][1] = 1;
		p.stampParams[i][3] = 0;
	}
	field.Step(p);
	const auto full = field.Read();
	for (uint32_t i = 0; i < Clipmap::kMaxStamps; ++i) {
		const auto x = static_cast<int>(std::floor(p.stamps[i][0])) & (N - 1);
		const auto y = static_cast<int>(std::floor(p.stamps[i][1])) & (N - 1);
		Require(std::abs(full[y * N + x] + p.stamps[i][3]) < 0.001f, "Full-capacity dispatch lost a stamp");
	}
	std::puts("PASS all 64 stamps: isolated mask bits 0/31/32/63 and simultaneous distinct marks");
}

void CheckSweptAndOrientedStamps(Field& field)
{
	field.Clear();
	Params p;
	p.rimShape[1] = 0;  // constant depth makes gaps distinguishable from churn
	p.stamps[0][0] = 0.5f; p.stamps[0][1] = 12.5f; p.stamps[0][2] = 2;
	p.stampMotion[0][1] = 24;
	p.stampParams[0][3] = 0;
	field.Step(p);
	const auto trail = field.Read();
	for (int y = -12; y <= 12; ++y) {
		Require(trail[(y & (N - 1)) * N] < -1.9f, "Swept sprint stamp left a gap between endpoints");
	}
	Require(trail[18 * N] == 0 && trail[5] == 0, "Swept stamp escaped its bounds");

	p = Params{};
	p.stamps[0][2] = 8;
	p.stampShape[0][1] = 1;
	p.stampShape[0][2] = 2;
	p.stampParams[0][3] = 0;
	field.Clear(); field.Step(p);
	const auto north = field.Read();
	Require(north[4 * N] < -0.1f && north[4] == 0, "Oriented footprint lost its aspect ratio");
	p.stampShape[0][0] = 1; p.stampShape[0][1] = 0;
	field.Clear(); field.Step(p);
	const auto east = field.Read();
	Require(east[4] < -0.1f && east[4 * N] == 0, "Oriented footprint did not rotate with the actor");
	std::puts("PASS swept/oriented stamps: continuous 24-unit trail, bounds, aspect and rotation");
}

// A lying object: an oriented press over its footprint (a rifle, 32 x 4 units, lying
// east-west), whose rim must rise only past the margin, never over the object.
void CheckObjectFootprint(Field& field)
{
	constexpr float halfLength = 16, halfWidth = 2, margin = 3, depth = 1.5f;
	const auto rifle = [&](float a_forwardX, float a_forwardY) {
		Params p;
		p.rimShape[1] = 0;  // no churn: the dent is exactly its depth
		p.stamps[0][2] = halfLength;
		p.stamps[0][3] = depth;
		p.stampParams[0][3] = 1;
		p.stampShape[0][0] = a_forwardX;
		p.stampShape[0][1] = a_forwardY;
		p.stampShape[0][2] = halfWidth;
		p.stampShape[0][3] = -(1 + margin);  // what Clipmap packs for an object's press
		return p;
	};
	// The shader's own measure of how far a texel lies outside the footprint.
	const auto outside = [&](float a_x, float a_y, float a_forwardX, float a_forwardY) {
		const float along = a_x * a_forwardX + a_y * a_forwardY;
		const float across = -a_x * a_forwardY + a_y * a_forwardX;
		const float k0 = std::hypot(along / halfLength, across / halfWidth);
		const float k1 = std::hypot(along / (halfLength * halfLength), across / (halfWidth * halfWidth));
		return k1 > 1e-6f ? k0 * (k0 - 1) / k1 : -halfWidth;
	};

	for (const auto& [fx, fy] : { std::pair{ 1.0f, 0.0f }, std::pair{ 0.0f, 1.0f }, std::pair{ 0.6f, 0.8f } }) {
		field.Clear(); field.Step(rifle(fx, fy));
		const auto h = field.Read();
		float rim = 0, deepest = 0;
		for (uint32_t y = 0; y < N; ++y) {
			for (uint32_t x = 0; x < N; ++x) {
				const float height = h[y * N + x];
				const float out = outside(WorldOf(x), WorldOf(y), fx, fy);
				if (out < margin - 0.01f) {
					Require(height <= 0, "An object's rim rose inside its footprint or margin");
				} else {
					rim = std::max(rim, height);
				}
				Require(height >= -depth - 1e-4f, "An object's dent went deeper than its stamp");
				deepest = std::min(deepest, height);
			}
		}
		Require(deepest < -depth * 0.9f, "An object made no dent under itself");
		Require(rim > 0.05f, "An object pushed no soil aside");
	}
	{
		// Along its length the rifle reaches 16 units; across it, 2: a disc would not.
		field.Clear(); field.Step(rifle(1, 0));
		const auto h = field.Read();
		Require(h[12] < -0.1f && h[12 * N] >= 0, "An object's footprint lost its length or orientation");
	}
	{
		// The ordinary edge rim (a disc as wide as the object's bound sphere) is what buried
		// the ends of a lying rifle: it rises right at the edge, where the ends lie. At the
		// shipped lean 1 it peaks exactly there and the tessellated surface carries it over
		// the tip; lean 0 shows it inside the texel field itself.
		Params p = rifle(1, 0);
		p.stampShape[0][2] = 0;
		p.stampShape[0][3] = 1;
		p.rimShape[0] = 0;
		field.Clear(); field.Step(p);
		const auto h = field.Read();
		Require(h[15] > 0, "The disc's rim should rise over the footprint's tip (the old fault)");
	}
	field.Clear();
	std::puts("PASS object footprint: oriented, shallow dent, rim only past the margin, the old disc rim reproduced");
}

void CheckFootprintTexture(Field& field, const std::filesystem::path& textures)
{
	// Load both shipped textures through the production loader, not a synthetic mask.
	ComPtr<ID3D11ShaderResourceView> normal;
	Check(Dds::LoadTexture(field.Device(), (textures / "FootprintNormal.dds").c_str(), &normal), "Load shipping footprint normal DDS");
	field.LoadShape(textures / "FootprintHeight.dds");
	Params p;
	p.stamps[0][2] = 12;
	p.stamps[0][3] = 6;
	p.stampParams[0][2] = 2;  // shipping kPrint
	p.stampParams[0][3] = 2;
	p.stampShape[0][1] = 1;
	p.stampShape[0][2] = 6;
	p.stampShape[0][3] = 1;
	field.Clear(); field.Step(p);
	const auto right = field.Read();
	Require(*std::min_element(right.begin(), right.end()) < -1, "Shipping sole mask made no depression");
	Require(*std::max_element(right.begin(), right.end()) > 0.05f, "Shipping sole mask made no rim");
	p.stampShape[0][3] = -1;
	field.Clear(); field.Step(p);
	const auto left = field.Read();
	float largestDifference = 0;
	for (uint32_t y = 0; y < N; ++y) {
		for (uint32_t x = 0; x < N; ++x) {
			Require(std::abs(left[y * N + x] - right[y * N + N - 1 - x]) < 0.001f,
				"Left/right footprint masks are not mirrors");
			largestDifference = std::max(largestDifference, std::abs(left[y * N + x] - right[y * N + x]));
			if (std::abs(WorldOf(x)) > 6 || std::abs(WorldOf(y)) > 12) {
				Require(left[y * N + x] == 0, "Footprint texture escaped its rectangle");
			}
		}
	}
	Require(largestDifference > 0.01f, "Mirroring did not change the asymmetric sole mask");
	field.Step(p);
	Require(field.Read() == left, "Repeated textured prints accumulated height");
	field.UnloadShape();
	std::puts("PASS shipping footprint DDS: height/normal load, depression, rim, mirroring, bounds, idempotence");
}

void CheckCreatureFootprintMasks(Field& field, const std::filesystem::path& textures)
{
	field.LoadShape(textures / "FootprintHeight.dds");
	std::array<std::vector<float>, 5> masks;
	for (uint32_t style = 2; style <= 6; ++style) {
		Params params;
		params.control[1] = 1;
		params.stamps[0][2] = 18;
		params.stamps[0][3] = 6;
		params.stampParams[0][0] = 0.3f;
		params.stampParams[0][1] = 1;
		params.stampParams[0][2] = 2;
		params.stampParams[0][3] = 2;
		params.stampShape[0][1] = 1;
		params.stampShape[0][2] = 11;
		params.stampShape[0][3] = static_cast<float>(style);
		field.Clear();
		field.Step(params);
		masks[style - 2] = field.Read();
		Require(*std::min_element(masks[style - 2].begin(), masks[style - 2].end()) < -1,
			"A procedural creature mask made no depression");
		Require(*std::max_element(masks[style - 2].begin(), masks[style - 2].end()) > 0.05f,
			"A procedural creature mask made no raised edge");
		for (uint32_t y = 0; y < N; ++y) {
			for (uint32_t x = 0; x < N; ++x) {
				if (std::abs(WorldOf(x)) > 19 || std::abs(WorldOf(y)) > 19) {
					Require(masks[style - 2][y * N + x] == 0,
						"A procedural creature mask escaped the footprint bounds");
				}
			}
		}
	}
	for (size_t first = 0; first < masks.size(); ++first) {
		for (size_t second = first + 1; second < masks.size(); ++second) {
			Require(masks[first] != masks[second], "Creature footprint classes must have distinct silhouettes");
		}
	}
	field.UnloadShape();
	std::puts("PASS procedural creature masks: paw, claw, hoof, mechanical and heavy silhouettes stay in bounds");
}

// A bounded compute-only benchmark. Both levels share one immediate context and the
// fine level reads the live coarse level, using production resolution and dispatches.
// GPU timestamps bracket the whole two-level update; no staging readbacks are timed.
// This cannot measure engine hooks, actor scans, tessellation or total frame overhead.
void Benchmark()
{
	g_driver = D3D_DRIVER_TYPE_HARDWARE;
	Field coarse(Clipmap::kTexels);
	Field fine(Clipmap::kTexels, &coarse);
	fine.UseCoarse(coarse);
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	Check(fine.Device()->QueryInterface(IID_PPV_ARGS(&dxgi)), "Query DXGI device");
	Check(dxgi->GetAdapter(&adapter), "Get hardware adapter");
	DXGI_ADAPTER_DESC description{};
	Check(adapter->GetDesc(&description), "Get adapter description");
	char name[256]{};
	WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), nullptr, nullptr);
	std::fprintf(stderr, "Hardware: %s; dedicated VRAM %.0f MiB; %u texels per level; cell sizes %.2f/%.2f\n",
		name, description.DedicatedVideoMemory / 1048576.0, Clipmap::kTexels,
		Clipmap::CellSizeFor(0), Clipmap::CellSizeFor(1));
	std::fprintf(stderr, "Compute only; 30 warmup frames then 60 pipelined single-frame GPU samples per case. No game FPS claim.\n");
	auto* context = fine.Context();
	ComPtr<ID3D11Query> disjoint;
	std::array<ComPtr<ID3D11Query>, 60> begin, end;
	D3D11_QUERY_DESC query{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	Check(fine.Device()->CreateQuery(&query, &disjoint), "Create disjoint query");
	query.Query = D3D11_QUERY_TIMESTAMP;
	for (size_t i = 0; i < begin.size(); ++i) {
		Check(fine.Device()->CreateQuery(&query, &begin[i]), "Create begin timestamp");
		Check(fine.Device()->CreateQuery(&query, &end[i]), "Create end timestamp");
	}
	const Settings::Values defaults;
	std::puts("case,levels,stamps,samples,disjoint,mean_ms,p50_ms,p95_ms,min_ms,max_ms");
	for (const std::string_view scenario : { "idle-one-level", "idle", "feet-2", "actors-64", "winter-8", "heat-8", "old-marks", "full-reseed" }) {
		coarse.Clear(); fine.Clear();
		const unsigned levels = scenario == "idle-one-level" ? 1 : 2;
		const unsigned stamps = scenario == "feet-2" ? 2 : scenario == "actors-64" ? 64 :
			(scenario == "winter-8" || scenario == "heat-8") ? 8 : 0;
		Params base(Clipmap::kTexels);
		base.control[0] = 1.0f / 60;
		base.control[1] = static_cast<float>(stamps);
		base.control[3] = defaults.stampRimSpan;
		base.weather[1] = std::tan(defaults.stampSlopeLimit * 0.017453292f);
		base.weather[3] = defaults.stampRimNoise;
		base.rimShape[0] = defaults.stampRimLean;
		base.rimShape[1] = defaults.stampChurn;
		base.snowRim[0] = defaults.snowStampRimSpan;
		base.snowRim[1] = defaults.snowStampRimNoise;
		base.snowRim[2] = defaults.snowStampRimLean;
		base.snowRim[3] = defaults.snowStampChurn;
		base.raise[0] = scenario == "winter-8" || scenario == "heat-8" ? defaults.snowRaiseHeight : 0;
		base.raise[3] = base.raise[0] > 0 ? 1.0f : 0.0f;
		base.raiseWindow[2] = defaults.snowRaiseDistance - defaults.snowRaiseFadeBand;
		base.raiseWindow[3] = defaults.snowRaiseDistance;
		for (unsigned i = 0; i < stamps; ++i) {
			base.stamps[i][0] = -224 + static_cast<float>(i % 8) * 64;
			base.stamps[i][1] = stamps <= 8 ? 0 : -224 + static_cast<float>(i / 8) * 64;
			base.stamps[i][2] = defaults.stampFootRadius * defaults.stampFootLength;
			base.stamps[i][3] = defaults.stampDepth;
			base.stampParams[i][0] = 0.3f;
			base.stampParams[i][1] = defaults.stampDecayPerSecond;
			base.stampParams[i][3] = defaults.stampDepth * defaults.stampRimHeight;
			base.stampShape[i][1] = 1;
			base.stampShape[i][2] = base.stamps[i][2] * defaults.stampFootAspect;
			base.stampShape[i][3] = 1;
			base.stampMotion[i][1] = 4;  // swept foot travel per 60 Hz frame
			base.stampMotion[i][2] = scenario == "winter-8" ? 1.0f : 0.0f;
			if (scenario == "heat-8") {
				base.stamps[i][2] = 48;  // a fire mesh's representative bound radius
				base.stamps[i][3] = defaults.heatMeltPerSecond;
				base.stampParams[i][0] = defaults.heatShoulder;
				base.stampParams[i][2] = 1;
				base.stampParams[i][3] = -defaults.snowRaiseHeight * defaults.heatMeltDepthScale;
				std::fill_n(base.stampShape[i], 4, 0.0f);
				std::fill_n(base.stampMotion[i], 4, 0.0f);
			}
		}
		unsigned frame = 0;
		const auto step = [&] {
			for (unsigned level = levels; level-- > 0;) {
				Params p = base;
				p.window[2] = Clipmap::CellSizeFor(level);
				p.coarse[0] = level == 0 && levels == 2 ? 1.0f : 0.0f;
				p.coarse[1] = Clipmap::WorldSizeFor(level + 1);
				p.coarse[2] = defaults.clipmapSeedSmoothing;
				p.coarse[3] = base.control[2] - 2;
				p.repose[0] = Clipmap::ReposeAlpha(defaults.stampReposeRate, p.control[0], level);
				p.repose[1] = Clipmap::ReposeAlpha(defaults.snowStampReposeRate, p.control[0], level);
				if (scenario == "full-reseed") {
					p.window[0] = static_cast<float>(frame * Clipmap::kTexels);
					p.coarse[3] = 0;
				}
				(level == 0 ? fine : coarse).Step(p, p.coarse[0] > 0);
			}
			++frame;
		};
		if (scenario == "old-marks") {
			Params priming = base;
			priming.control[1] = 1;
			priming.stamps[0][2] = 600;
			priming.stamps[0][3] = 12;
			priming.window[2] = Clipmap::CellSizeFor(1); coarse.Step(priming);
			priming.window[2] = Clipmap::CellSizeFor(0); fine.Step(priming);
		}
		for (unsigned i = 0; i < 30; ++i) { step(); }
		std::vector<double> samples;
		unsigned discarded = 0;
		for (unsigned attempt = 0; samples.empty() && attempt < 3; ++attempt) {
			context->Begin(disjoint.Get());
			for (size_t i = 0; i < begin.size(); ++i) {
				context->End(begin[i].Get());
				step();
				context->End(end[i].Get());
			}
			context->End(disjoint.Get()); context->Flush();
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			const auto read = [&](ID3D11Query* q, void* data, UINT size) {
				HRESULT hr;
				while ((hr = context->GetData(q, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
					Require(std::chrono::steady_clock::now() < deadline, "GPU timestamp timeout");
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Check(hr, "Read GPU timestamp");
			};
			read(disjoint.Get(), &timing, sizeof(timing));
			if (timing.Disjoint || timing.Frequency == 0) { discarded += 60; continue; }
			for (size_t i = 0; i < begin.size(); ++i) {
				UINT64 first{}, last{};
				read(begin[i].Get(), &first, sizeof(first)); read(end[i].Get(), &last, sizeof(last));
				Require(last >= first, "GPU timestamp order invalid");
				samples.push_back(static_cast<double>(last - first) * 1000.0 / timing.Frequency);
			}
		}
		Require(samples.size() == 60, "Insufficient valid GPU samples");
		const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
		std::sort(samples.begin(), samples.end());
		std::printf("%.*s,%u,%u,%zu,%u,%.6f,%.6f,%.6f,%.6f,%.6f\n",
			static_cast<int>(scenario.size()), scenario.data(), levels, stamps, samples.size(), discarded,
			mean, samples[29], samples[56], samples.front(), samples.back());
		std::fflush(stdout);
	}
}

// Real feet or synthetic steps by press evidence, and how a foot is read off a skeleton
// moved by a full-body first-person mod. In game a leg-motion rule still switched 12 times in
// 10 s: the player's body in first person is animated only now and then, so intermittently
// moving feet that never reach the ground planted must stay on synthetic steps, without
// switching, while feet that press keep (and bring back) the real feet at once.
void CheckFootGeometry()
{
	// First person: the camera sits in the head (standing, crouching, power armour); a
	// third-person camera, over the shoulder included, sits further back.
	Require(FootGeometry::CameraInHead(3.0f, 2.0f, 120.0f, 0.0f, 0.0f, 0.0f), "A standing first-person camera is in the head");
	Require(FootGeometry::CameraInHead(0.0f, 5.0f, 75.0f, 0.0f, 0.0f, 0.0f), "A crouching first-person camera is in the head");
	Require(FootGeometry::CameraInHead(0.0f, 8.0f, 190.0f, 0.0f, 0.0f, 0.0f), "Power armour lifts the head");
	Require(!FootGeometry::CameraInHead(30.0f, -60.0f, 110.0f, 0.0f, 0.0f, 0.0f), "An over-the-shoulder camera is third person");
	Require(!FootGeometry::CameraInHead(0.0f, -250.0f, 150.0f, 0.0f, 0.0f, 0.0f), "A chase camera is third person");
	Require(!FootGeometry::CameraInHead(0.0f, 0.0f, 400.0f, 0.0f, 0.0f, 0.0f), "A camera high above is not in the head");
	// Held: in at once, out only after a run of frames out of the head.
	FootGeometry::FirstPersonState held{};
	Require(FootGeometry::UpdateFirstPerson(held, true), "First person starts at once");
	for (uint32_t i = 1; i < FootGeometry::kFirstPersonLeaveFrames; ++i) {
		Require(FootGeometry::UpdateFirstPerson(held, false), "A few frames out of the head keep first person");
	}
	Require(FootGeometry::UpdateFirstPerson(held, true) && held.outFrames == 0, "Back in the head resets the count");
	for (uint32_t i = 0; i < FootGeometry::kFirstPersonLeaveFrames; ++i) {
		FootGeometry::UpdateFirstPerson(held, false);
	}
	Require(!held.on, "A run of frames out of the head ends first person");

	constexpr float step = 4.0f;  // about 240 units/s at 60 Hz

	FootGeometry::FeetMode pressing;
	for (int i = 0; i < 400; ++i) {
		// A walk presses a planted foot on about half the frames.
		Require(!FootGeometry::UpdateFeetMode(pressing, i % 10 < 5, step), "Pressing feet must stay real");
	}

	FootGeometry::FeetMode firstPerson;
	int entered = -1, switches = 0;
	bool was = false;
	for (int i = 0; i < 600; ++i) {
		// The legs move now and then (whatever they do, they never press).
		const bool now = FootGeometry::UpdateFeetMode(firstPerson, false, step);
		if (now && entered < 0) { entered = i + 1; }
		switches += now != was;
		was = now;
	}
	Require(entered == static_cast<int>(FootGeometry::kRealFeetTravel / step), "Feet that never press must give way after 96 units");
	Require(switches == 1 && firstPerson.synthetic, "Never-pressing feet must stay on synthetic steps without switching");

	const FootGeometry::FeetMode before = firstPerson;
	FootGeometry::UpdateFeetMode(firstPerson, false, 0.0f);
	Require(firstPerson.synthetic == before.synthetic && firstPerson.sincePress == before.sincePress,
		"Standing still must change nothing");

	Require(!FootGeometry::UpdateFeetMode(firstPerson, true, step), "A foot that presses must bring the real feet back at once");
	int gone = -1;
	for (int i = 0; i < 40 && gone < 0; ++i) {
		if (FootGeometry::UpdateFeetMode(firstPerson, false, step)) { gone = i + 1; }
	}
	Require(gone == static_cast<int>(FootGeometry::kRealFeetTravel / step), "Feet that stop pressing for 96 units must give way again");

	struct Named { const char* name; int side; };
	for (const Named& n : { Named{ "LLeg_Foot", -1 }, Named{ "RLeg_Foot", 1 }, Named{ "LLeg_Toe1", -1 },
			 Named{ "Bip01 L Foot", -1 }, Named{ "Bip01 R Foot", 1 }, Named{ "FrontLLeg_Foot", -1 },
			 Named{ "BackRLeg_Foot", 1 }, Named{ "LeftFoot", -1 }, Named{ "Foot_R", 1 }, Named{ "Leg_Foot", 0 },
			 Named{ "LowerLeg_Foot", 0 }, Named{ "Root", 0 } }) {
		Require(FootGeometry::SideFromName(n.name) == n.side, (std::string("Wrong side read from ") + n.name).c_str());
	}
	Require(FootGeometry::IsCloneNode("TFP_ShadowTwin") && !FootGeometry::IsCloneNode("LLeg_Foot") &&
		!FootGeometry::IsCloneNode("NPC Root [Root]"), "Only another mod's skeleton copy is skipped");

	// A body moved 30 units off the actor's position (Full Body First Person goes to 40): from
	// the actor's position both feet read as one side; from the feet's middle, left and right.
	const float rightX = 1.0f, rightY = 0.0f;
	const float leftFootX = 30.0f - 6.0f, rightFootX = 30.0f + 6.0f;
	Require(FootGeometry::Lateral(leftFootX, 0.0f, 0.0f, 0.0f, rightX, rightY) > 0.0f, "Test setup: both feet right of the actor");
	const float middle = 0.5f * (leftFootX + rightFootX);
	Require(FootGeometry::MirrorFor(0, FootGeometry::Lateral(leftFootX, 0.0f, middle, 0.0f, rightX, rightY)) < 0.0f &&
		FootGeometry::MirrorFor(0, FootGeometry::Lateral(rightFootX, 0.0f, middle, 0.0f, rightX, rightY)) > 0.0f,
		"From the feet's middle the left foot must print left and the right foot right");
	// A crossing step puts the left foot right of the middle: its name still says left.
	Require(FootGeometry::MirrorFor(-1, 2.0f) < 0.0f && FootGeometry::MirrorFor(1, -2.0f) > 0.0f,
		"A named foot must keep its side across the middle");

	float pointX = 0.0f, pointY = 1.0f;
	Require(FootGeometry::ToeHeading(0.0f, 0.0f, 10.0f, 0.0f, pointX, pointY) && std::abs(pointX - 1.0f) < 1e-6f &&
		std::abs(pointY) < 1e-6f, "The toe must set the way a print points");
	pointX = 0.0f; pointY = 1.0f;
	Require(!FootGeometry::ToeHeading(0.0f, 0.0f, 1.0f, 1.0f, pointX, pointY) && pointX == 0.0f && pointY == 1.0f,
		"A toe on the ankle must leave the heading");
	std::printf("PASS foot evidence and geometry: never-pressing feet give way after %d frames with 1 switch, "
		"pressing feet stay real, a press brings them back at once; sides by name and feet's middle, toe heading\n",
		entered);
}

void CheckFootprintProfiles()
{
	using FootprintProfiles::Shape;
	Require(FootprintProfiles::Select(true, false, false, false, false, false, false, false) == Shape::kClaw,
		"Deathclaws use clawed tracks");
	Require(FootprintProfiles::Select(false, true, false, false, false, false, false, false) == Shape::kClaw,
		"Mirelurks use clawed tracks");
	Require(FootprintProfiles::Select(false, false, true, false, false, false, false, false) == Shape::kHoof,
		"Radstags use split-hoof tracks");
	Require(FootprintProfiles::Select(false, false, false, true, false, false, false, false) == Shape::kPaw,
		"Yao guai use paw tracks");
	Require(FootprintProfiles::Select(false, false, false, false, false, true, false, false) == Shape::kMechanical,
		"Robots use mechanical tracks");
	Require(FootprintProfiles::Select(false, false, false, false, false, false, true, false) == Shape::kHeavy,
		"Behemoths use heavy tracks");
	Require(FootprintProfiles::Select(false, false, false, false, false, false, false, true) == Shape::kBoot,
		"Booted humanoids retain boot tracks");
	Require(FootprintProfiles::Select(false, false, false, false, false, false, false, false) == Shape::kPaw,
		"Unclassified non-booted actors use the paw fallback");
	Require(FootprintProfiles::For(Shape::kClaw).lengthScale > 1.0f &&
			FootprintProfiles::For(Shape::kClaw).widthScale > FootprintProfiles::For(Shape::kPaw).widthScale &&
			FootprintProfiles::For(Shape::kClaw).depthScale > 1.0f,
		"Clawed tracks are wider, longer and deeper than the generic paw profile");
	std::puts("PASS creature footprint profiles: boots, paws, claws, hooves, robots, heavy feet and fallback");
}

// What bodies touch the ground with, on trees cut from the game's skeletons and robot leg
// parts (node names as Fallout 4 ships them).
struct ContactNode
{
	std::string              name;
	std::vector<ContactNode> children;
};

ContactNode ContactBone(const char* a_name, std::vector<ContactNode> a_children = {})
{
	return { a_name, std::move(a_children) };
}

FootGeometry::Contacts<ContactNode> ContactsOf(ContactNode& a_root)
{
	static const std::vector<std::string> words{ "foot", "toe", "paw", "hoof" };
	return FootGeometry::FindContacts(&a_root, std::span<const std::string>(words), 8,
		[](ContactNode* a_node) { return std::string_view(a_node->name); },
		[](ContactNode* a_node, auto&& a_visit) {
			for (auto& child : a_node->children) {
				a_visit(&child);
			}
		});
}

std::string ContactNames(const std::vector<ContactNode*>& a_nodes)
{
	std::string names;
	for (const auto* node : a_nodes) {
		names += names.empty() ? "" : ",";
		names += node ? node->name : std::string("-");
	}
	return names;
}

void RequireContacts(ContactNode& a_root, FootGeometry::Contact a_kind, const char* a_nodes, const char* a_tips,
	bool a_thruster, const char* a_what)
{
	const auto found = ContactsOf(a_root);
	const auto nodes = ContactNames(found.nodes);
	const auto tips = ContactNames(found.tips);
	if (found.kind != a_kind || nodes != a_nodes || tips != a_tips || found.thruster != a_thruster) {
		std::printf("FAIL %s: %s [%s] tips [%s] thruster %d\n", a_what, FootGeometry::ContactName(found.kind),
			nodes.c_str(), tips.c_str(), found.thruster ? 1 : 0);
		std::exit(1);
	}
}

// A visible foot (FRIK's) comes down once per step: on a walk recorded in VR its speed
// (units a second) swings past 120 and drops back, while standing it drifts below that.
void CheckLandings()
{
	const float walk[] = { 20, 25, 473, 60, 60, 60, 0, 0, 0, 13, 66, 101, 132, 396, 467, 593, 589, 592, 591, 416, 76,
		29, 0, 0, 0, 0, 30, 484, 507, 511, 507, 517, 490, 353, 79, 39, 0, 0, 0, 0, 34, 487, 513, 507 };
	std::uint8_t swinging = 0;
	int          landings = 0;
	for (const float speed : walk) {
		landings += FootGeometry::Landed(swinging, speed, 120.0f) ? 1 : 0;
	}
	Require(landings == 3 && swinging == 1, "A walking foot comes down once per swing");

	const float drift[] = { 39, 37, 39, 39, 38, 36, 34, 36, 35, 32, 36, 36, 31, 59, 55, 48, 44, 43, 41, 0, 2 };
	swinging = 0;
	landings = 0;
	for (const float speed : drift) {
		landings += FootGeometry::Landed(swinging, speed, 120.0f) ? 1 : 0;
	}
	Require(landings == 0, "A foot drifting on the ground never comes down again");
	std::puts("PASS visible-foot landings: one per swing, none while a planted foot drifts");

	// Heights measured on VR walks: a child's planted ankle 16 over the land (its own contact
	// 10.2), a grown-up's on a slope 18.2 (contact 14.8), FRIK's up to 35 (contact 13.1).
	Require(FootGeometry::FootContact(10.2f, 12.4f, false) >= 16.0f, "A child's ankle counts as down");
	Require(FootGeometry::FootContact(14.8f, 12.4f, false) >= 18.2f, "An ankle over a slope counts as down");
	Require(FootGeometry::FootContact(13.1f, 12.4f, true) >= 35.0f, "FRIK's ankle counts as down");
	Require(FootGeometry::FootContact(14.8f, 12.4f, false) < 25.0f, "A foot on a porch over the land is not down");
	std::puts("PASS foot contact heights: children, slopes and FRIK's feet");

	// Heights recorded in VR: pre-war Sanctuary's NPCs stand 27.6 over the land under a 35-unit
	// blanket; a road the snow buries 28 deep; a porch 41 over the land stands above the snow.
	Require(FootGeometry::OnGround(27.6f, 0.0f, 35.0f, 13.0f, 48.0f), "Inside the snow blanket is on the ground");
	Require(!FootGeometry::OnGround(27.6f, 0.0f, 0.0f, 13.0f, 48.0f), "Raised over bare land is not");
	Require(FootGeometry::OnGround(28.0f, 28.0f, 0.0f, 13.0f, 48.0f), "A road the snow buries is on the ground");
	Require(!FootGeometry::OnGround(41.0f, 0.0f, 35.0f, 13.0f, 48.0f), "A porch over the snow top is not");
	Require(FootGeometry::OnGround(0.5f, 0.0f, 0.0f, 13.0f, 48.0f), "Standing on the land is");
	Require(!FootGeometry::OnGround(60.0f, 0.0f, 35.0f, 13.0f, 48.0f), "A jump is not");
	std::puts("PASS on the ground: land, buried roads, inside the snow blanket; not porches or jumps");

	// Sizes recorded in game: a person about 1.1, a deathclaw 3.2.
	Require(FootGeometry::Heavy(1.1f) == 1.0f, "A person presses as before");
	Require(FootGeometry::Heavy(3.2f) > 1.5f && FootGeometry::Heavy(3.2f) < 1.7f, "A deathclaw presses about 1.6 times");
	Require(FootGeometry::Heavy(16.0f) == FootGeometry::kMaxHeavy, "Heaviness has a ceiling");
	std::puts("PASS heavy creatures: people as before, a deathclaw deeper, a ceiling");
}

void CheckContacts()
{
	using FootGeometry::Contact;

	ContactNode person = ContactBone("skeleton.nif", { ContactBone("Root", { ContactBone("LLeg_Thigh", { ContactBone("LLeg_Calf", { ContactBone("LLeg_Foot", { ContactBone("LLeg_Toe1") }) }) }),
		ContactBone("RLeg_Thigh", { ContactBone("RLeg_Calf", { ContactBone("RLeg_Foot", { ContactBone("RLeg_Toe1") }) }) }),
		ContactBone("TFP_ShadowTwin", { ContactBone("LLeg_Foot", { ContactBone("LLeg_Toe1") }) }) }) });
	RequireContacts(person, Contact::kFeet, "LLeg_Foot,RLeg_Foot", "LLeg_Toe1,RLeg_Toe1", false,
		"A person stands on foot bones, never a clone's");

	ContactNode dog = ContactBone("skeleton.nif", { ContactBone("Dogmeat_Front_LLeg1", { ContactBone("Dogmeat_Front_LLegAnkle", { ContactBone("Dogmeat_Front_LLegToe1") }) }),
		ContactBone("Dogmeat_Rear_RLeg1", { ContactBone("Dogmeat_Rear_RLegAnkle", { ContactBone("Dogmeat_Rear_RLegToe1") }) }) });
	RequireContacts(dog, Contact::kFeet, "Dogmeat_Front_LLegToe1,Dogmeat_Rear_RLegToe1", "-,-", false,
		"A dog stands on its toes, not the ankles above them");

	const auto paw = [](const char* a_ankle, const char* a_side) {
		const std::string s = a_side;
		std::vector<ContactNode> digits;
		for (const char* digit : { "Pinky", "Ring", "Mid", "Index", "Thumb" }) {
			digits.push_back({ s + digit + "1", { { s + digit + "2", {} } } });
		}
		return ContactNode{ a_ankle, std::move(digits) };
	};
	ContactNode yaoGuai = ContactBone("YaoGuaiPaw", { ContactBone("LLeg_Front_Thigh", { ContactBone("LLeg_Front_Knee", { paw("LLeg_Front_Ankle", "LLeg_Front_") }) }),
		ContactBone("LLeg_Front_Pole"), ContactBone("RLeg_Rear_Thigh", { ContactBone("RLeg_Rear_Knee", { paw("RLeg_Rear_Ankle", "RLeg_Rear_") }) }) });
	RequireContacts(yaoGuai, Contact::kAnkles, "LLeg_Front_Ankle,RLeg_Rear_Ankle", "LLeg_Front_Mid2,RLeg_Rear_Mid2", false,
		"A yao guai stands on its ankles, pointing to the middle digit (and its root is no paw)");

	ContactNode queen = ContactBone("Skeleton_Export", { ContactBone("LArm_Upper", { ContactBone("LArm_Claw") }),
		ContactBone("LLeg_F_Clav", { ContactBone("LLeg_F_Thigh", { ContactBone("LLeg_F_Knee", { ContactBone("LLeg_F_Ankle") }) }) }),
		ContactBone("RLeg_R_Clav", { ContactBone("RLeg_R_Thigh", { ContactBone("RLeg_R_Knee", { ContactBone("RLeg_R_Ankle") }) }) }) });
	RequireContacts(queen, Contact::kAnkles, "LLeg_F_Ankle,RLeg_R_Ankle", "-,-", false,
		"The mirelurk queen walks on her legs, not her pincers");

	ContactNode cricket = ContactBone("skeleton.nif", { ContactBone("CCRKT_BN_L_Knee_F", { ContactBone("CCRKT_BN_L_Ankle_F", { ContactBone("xCCRKT_BN_L_Ankle_F") }) }) });
	RequireContacts(cricket, Contact::kAnkles, "CCRKT_BN_L_Ankle_F", "xCCRKT_BN_L_Ankle_F", false,
		"An ankle's own tip is no second ankle");

	const auto botLegs = [](const char* a_part, std::vector<ContactNode> a_pelvis) {
		return ContactBone("skeleton.nif", { ContactBone("Root", { ContactBone("COM", { ContactBone(a_part, { ContactBone("C-BotLegs"), ContactBone("Pelvis", std::move(a_pelvis)) }) }) }) });
	};
	ContactNode protectron = botLegs("LegsProtectron.nif",
		{ ContactBone("RUpperLegBiped", { ContactBone("RLowerLegBiped", { ContactBone("RFootBiped", { ContactBone("RFootBipedTurn", { ContactBone("RFootBipedTwist", { ContactBone("RToeBiped") }) }) }) }) }),
			ContactBone("LUpperLegBiped", { ContactBone("LLowerLegBiped", { ContactBone("LFootBiped", { ContactBone("LFootBipedTurn", { ContactBone("LFootBipedTwist", { ContactBone("LToeBiped") }) }) }) }) }) });
	RequireContacts(protectron, Contact::kFeet, "RFootBiped,LFootBiped", "RToeBiped,LToeBiped", false,
		"A protectron's leg part walks on its feet");

	const auto wheel = [](const char* a_name, const std::string& a_side) {
		return ContactNode{ a_name, { { a_side + "L", {} }, { a_side + "R", {} } } };
	};
	ContactNode sentry = botLegs("LegsSentryBot.nif",
		{ ContactBone("RUpperLeg", { ContactBone("RMiddleLeg", { ContactBone("RLowerLeg", { wheel("RWheel", "RWheel") }) }) }),
			ContactBone("BUpperLeg", { ContactBone("BMiddleLeg", { wheel("BWheel", "BWheel") }) }),
			ContactBone("LUpperLeg", { ContactBone("LMiddleLeg", { ContactBone("LLowerLeg", { wheel("LWheel", "LWheel") }) }) }) });
	RequireContacts(sentry, Contact::kWheels, "RWheel,BWheel,LWheel", "-,-,-", false,
		"A sentry bot rolls on its three wheels");

	ContactNode roboBrain = botLegs("Export_LegsRoboBrain",
		{ ContactBone("TankBase", { ContactBone("LTankWheel_top"), ContactBone("RTankWheel_top"),
							ContactBone("ThreadIK_frontR1", { ContactBone("ThreadIK_frontR2", { ContactBone("ThreadIK_frontR3", { ContactBone("RTankWheel_front") }) }) }),
							ContactBone("ThreadIK_backL1", { ContactBone("ThreadIK_backL2", { ContactBone("ThreadIK_backL3", { ContactBone("LTankWheel_back") }) }) }) }) });
	RequireContacts(roboBrain, Contact::kWheels, "LTankWheel_top,RTankWheel_top,RTankWheel_front,LTankWheel_back", "-,-,-,-",
		false, "A robobrain rolls on its tread wheels; its part root is no leg");

	ContactNode handy = botLegs("LegsHandyThruster.nif", {});
	RequireContacts(handy, Contact::kNone, "", "", true, "Mr. Handy's thruster legs touch nothing and fly on a jet");
	ContactNode handyArmour = botLegs("DLC01_LegsHandyThruster.nif", { ContactBone("LegsHandyThrusterAR1A.nif", { ContactBone("Pelvis") }) });
	RequireContacts(handyArmour, Contact::kNone, "", "", true, "Armoured thruster legs are no limbs either");

	ContactNode roach = ContactBone("skeleton.nif", { ContactBone("Root", { ContactBone("1LLeg1", { ContactBone("1LLeg2", { ContactBone("1LLeg3") }) }),
		ContactBone("1RLeg1", { ContactBone("1RLeg2", { ContactBone("1RLeg3") }) }), ContactBone("Head") }) });
	RequireContacts(roach, Contact::kLegEnds, "1LLeg3,1RLeg3", "-,-", false, "A radroach stands on the ends of its legs");

	ContactNode many = ContactBone("skeleton.nif");
	for (int i = 0; i < 12; ++i) {
		many.children.push_back(ContactBone("Leg"));
	}
	RequireContacts(many, Contact::kLegEnds, "Leg,Leg,Leg,Leg,Leg,Leg,Leg,Leg", "-,-,-,-,-,-,-,-", false,
		"At most the given number of contacts");

	ContactNode eyebot = ContactBone("skeletonExport", { ContactBone("Root", { ContactBone("COM", { ContactBone("BODY", { ContactBone("LAntenna_Low1") }) }) }) });
	RequireContacts(eyebot, Contact::kNone, "", "", false, "An eyebot touches nothing");

	std::puts("PASS ground contacts: feet, toes, ankles and digits, wheels, leg ends, robot leg parts, thruster legs, "
			  "clones and the root skipped");
}
// The player's synthetic steps are not seen forming in first person: a mark waits while
// either end of it can be on screen, and is let out once both are out of view, once the
// player is far enough away or once it has waited long enough; a full queue presses its
// oldest instead of losing it, and without a camera nothing is held back.
// Blood and other ground decals: the box a static decal on the landscape is stretched to so
// that the deformed surface there (a crater, a trench, the raised snow) lies inside it.
void CheckGroundDecalFit()
{
	using GroundDecalFit::Box;
	using GroundDecalFit::Fit;
	static_assert(GroundDecalFit::kInputFloats == 40 && GroundDecalFit::kRotation22 == 26 &&
	              GroundDecalFit::kOriginZ == 30 && GroundDecalFit::kDepth == 33 && GroundDecalFit::kDynamic == 35,
		"The shader input layout read from ComputeShaderDecalsInput::Set");

	const float land = 1000.0f;
	Box box{};
	// A level decal 10 deep on bare ground: from a crater 20 below to 20 above.
	Require(Fit(1.0f, land, 10.0f, land, 0.0f, 20.0f, 24.0f, box), "A level decal on the ground is fitted");
	Require(std::abs(box.originZ - land) < 1e-3f && std::abs(box.depth - 40.0f) < 1e-3f,
		"Bare ground: the box spans the margin below and above the ground");
	// The same under a 35-unit snow blanket: the snow's top and a crater both lie inside.
	Require(Fit(-1.0f, land, 10.0f, land, 35.0f, 20.0f, 24.0f, box), "A decal under snow is fitted (axis sign ignored)");
	const float low = box.originZ - 0.5f * box.depth;
	const float high = box.originZ + 0.5f * box.depth;
	Require(low <= land - 18.0f && high >= land + 35.0f && high <= land + 55.0f + 1e-3f,
		"Under snow the box reaches from a crater floor to just above the snow's top");
	// A box the engine already sized deeper is never shrunk.
	Require(Fit(1.0f, land, 200.0f, land, 0.0f, 20.0f, 24.0f, box) && box.depth >= 200.0f - 1e-3f,
		"The engine's own box only ever grows");
	// A tilted axis needs more depth for the same vertical span.
	Require(Fit(0.8f, land, 0.0f, land, 0.0f, 20.0f, 24.0f, box) && std::abs(box.depth - 50.0f) < 1e-3f,
		"A tilted box is lengthened along its axis");
	Require(Fit(0.8f, land + 10.0f, 20.0f, land, 35.0f, 20.0f, 24.0f, box) &&
		box.originZ == land + 10.0f && box.depth >= 20.0f &&
		box.originZ - 0.4f * box.depth <= land - 20.0f &&
		box.originZ + 0.4f * box.depth >= land + 55.0f,
		"A tilted decal keeps its original centre and volume while growing to the snow and crater");
	Require(!Fit(std::numeric_limits<float>::infinity(), land, 10.0f, land, 35.0f, 20.0f, 24.0f, box),
		"An infinite decal axis is rejected");
	// Not on the ground: a wall decal, one on a table, or nothing to fit.
	Require(!Fit(0.5f, land, 10.0f, land, 35.0f, 20.0f, 24.0f, box), "A decal on a steep face keeps its box");
	Require(!Fit(1.0f, land + 80.0f, 10.0f, land, 35.0f, 20.0f, 24.0f, box), "A decal off the ground keeps its box");
	Require(!Fit(1.0f, land, 10.0f, land, 0.0f, 0.0f, 24.0f, box), "No snow and no margin: nothing to fit");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Require(!Fit(nan, land, 10.0f, land, 0.0f, 20.0f, 24.0f, box) && !Fit(1.0f, nan, 10.0f, land, 0.0f, 20.0f, 24.0f, box) &&
	            !Fit(1.0f, land, -1.0f, land, 0.0f, 20.0f, 24.0f, box),
		"Invalid input keeps the engine's box");
	// Blood soaks into snow: on a blanket the fade is scaled, on bare ground it is kept.
	Require(GroundDecalFit::SnowFade(1.0f, 35.0f, 0.75f) == 0.75f, "A decal on snow shows at the snow opacity");
	Require(GroundDecalFit::SnowFade(0.5f, 35.0f, 0.75f) == 0.375f, "The game's own fade still applies on snow");
	Require(GroundDecalFit::SnowFade(0.8f, 0.0f, 0.75f) == 0.8f, "Bare ground keeps the game's fade");
	Require(GroundDecalFit::SnowFade(1.0f, 35.0f, 7.0f) == 1.0f, "The snow opacity clamps at full");
	std::puts("PASS ground decals: on-ground test, snow and crater span, never shrinks, tilt, invalid input, soaked into snow");
}

void CheckSnowMeshCover()
{
	using SnowMeshCover::Classify;
	const float raise = 35.0f;
	const float cover = 31.0f;
	Require(SnowSurface::ApplyMeshCap(35.0f, 35.0f, 0.4f) == 14.0f &&
		SnowSurface::ApplyMeshCap(17.5f, 35.0f, 0.4f) == 14.0f &&
		SnowSurface::ApplyMeshCap(10.0f, 35.0f, 0.4f) == 10.0f &&
		SnowSurface::ApplyMeshCap(8.75f, 35.0f, 0.4f) == 8.75f,
		"A fixed mesh cap uses configured height; weather and partial ground coverage only lower the blanket");
	Require(SnowMeshCover::HitsBuriedTop(30.0f, 30.0f) && SnowMeshCover::HitsBuriedTop(8.0f, 8.0f) &&
		!SnowMeshCover::HitsBuriedTop(40.0f, 30.0f) && !SnowMeshCover::HitsBuriedTop(0.0f, 0.0f),
		"Shots use the buried mesh top even above the ordinary 16-unit land tolerance");

	auto r = Classify(-1.0f, raise, cover, true);
	Require(r.cap == 255 && r.buried == 0, "A clear column keeps the full blanket and buries nothing");
	r = Classify(std::numeric_limits<float>::quiet_NaN(), raise, cover, true);
	Require(r.cap == 255 && r.buried == 0, "An invalid ray reads as a clear column");

	// A road 8 units up: buried under the full blanket, its top kept for feet and shots.
	r = Classify(8.0f, raise, cover, true);
	Require(r.cap == 255 && r.buried > 0, "A low mesh is buried: no cap");
	Require(std::abs(SnowMeshCover::BuriedHeight(r.buried, raise) - 8.0f) < 0.2f, "The buried mesh's top is kept");

	// Switched off: the blanket stays 3 under the road, as before.
	const auto off = Classify(8.0f, raise, cover, false);
	Require(off.buried == 0 && off.cap == static_cast<uint8_t>(5.0f / raise * 255.0f + 0.5f),
		"Switched off: the old cap under the mesh");

	// A porch 33 up stays above the snow: capped either way.
	r = Classify(33.0f, raise, cover, true);
	Require(r.buried == 0 && r.cap == static_cast<uint8_t>(30.0f / raise * 255.0f + 0.5f),
		"A tall mesh keeps the blanket under its top");

	// The cover height never comes within 2 of the blanket's top.
	Require(Classify(18.0f, 20.0f, cover, true).buried > 0 && Classify(19.0f, 20.0f, cover, true).buried == 0,
		"A mesh is buried only with snow over it (cover limited to the raise less 2)");

	// The shelter map's erode and smooth: a buried road leaves the blanket whole around
	// it; the old cap spread a lowered ring two texels out.
	constexpr uint32_t n = 16;
	const auto smoothed = [&](bool a_coverLow) {
		std::vector<uint8_t>  cap(n * n, 255), low(n * n, 255), smooth(n * n, 255);
		std::vector<uint32_t> rows;
		for (uint32_t y = 6; y <= 9; ++y) {
			for (uint32_t x = 6; x <= 9; ++x) {
				cap[y * n + x] = Classify(8.0f, raise, cover, a_coverLow).cap;
			}
		}
		const MapFilter::CellRect all{ 0, 0, static_cast<int32_t>(n) - 1, static_cast<int32_t>(n) - 1 };
		MapFilter::MinRect(cap, low, n, 1, all);
		MapFilter::BoxRect(low, smooth, n, 1, all, rows);
		return smooth;
	};
	const auto buried = smoothed(true);
	Require(std::all_of(buried.begin(), buried.end(), [](uint8_t a_v) { return a_v == 255; }),
		"A buried road leaves no bare ring: the blanket is whole over and around it");
	const auto old = smoothed(false);
	Require(old[7 * n + 4] < 255 && old[7 * n + 7] < 64, "Switched off: the old lowered ring around the road");

	std::puts("PASS snow over static meshes: low meshes buried, tall ones capped, cover limit, no bare ring, off switch");
}

void CheckHiddenMarks()
{
	HiddenMarks::View ahead{};
	ahead.eye[2] = 120.0f;  // eye height over the feet
	ahead.halfX = 0.9f;     // about 84 by 62 degrees
	ahead.halfY = 0.6f;
	ahead.valid = true;     // looking north, level

	const float feet[3]{ 0.0f, 0.0f, 0.0f };
	const float farAhead[3]{ 0.0f, 300.0f, 0.0f };
	const float behind[3]{ 0.0f, -200.0f, 0.0f };
	using HiddenMarks::Release;
	using HiddenMarks::Decide;
	Require(Decide(ahead, feet, feet, 12.0f, 6.0f, 400.0f, 0.0f, 30.0f) == Release::kOutOfView,
		"Under the camera of a level view a print is out of sight and presses at once");
	Require(Decide(ahead, farAhead, farAhead, 12.0f, 300.0f, 400.0f, 0.0f, 30.0f) == Release::kKeep,
		"Ground ahead in view must keep its mark waiting");
	Require(Decide(ahead, behind, behind, 12.0f, 200.0f, 400.0f, 0.0f, 30.0f) == Release::kOutOfView,
		"Ground behind the camera is out of view");
	Require(Decide(ahead, behind, farAhead, 12.0f, 200.0f, 400.0f, 0.0f, 30.0f) == Release::kKeep,
		"A trench stretch with one end in view must wait");
	const float left[3]{ -400.0f, 300.0f, 0.0f }, right[3]{ 400.0f, 300.0f, 0.0f };
	Require(!HiddenMarks::InView(ahead, left, 12.0f) && !HiddenMarks::InView(ahead, right, 12.0f) &&
		Decide(ahead, left, right, 12.0f, 300.0f, 0.0f, 0.0f, 0.0f) == Release::kKeep,
		"A trench crossing the view waits even when both endpoints are offscreen");

	HiddenMarks::View down = ahead;  // looking straight down at the feet
	down.forward[0] = 0.0f; down.forward[1] = 0.0f; down.forward[2] = -1.0f;
	down.up[0] = 0.0f; down.up[1] = 1.0f; down.up[2] = 0.0f;
	Require(Decide(down, feet, feet, 12.0f, 6.0f, 400.0f, 0.0f, 30.0f) == Release::kKeep,
		"Looking down, a print at the feet must not be seen forming");
	Require(Decide(down, feet, feet, 12.0f, 450.0f, 400.0f, 0.0f, 30.0f) == Release::kDistance,
		"A mark the player has left far behind presses even in view");
	Require(Decide(down, feet, feet, 12.0f, 6.0f, 400.0f, 31.0f, 30.0f) == Release::kAge,
		"A mark that waited long enough presses even in view");
	Require(Decide(down, feet, feet, 12.0f, 450.0f, 0.0f, 31.0f, 0.0f) == Release::kKeep,
		"With no distance or age limit a mark in view waits");

	HiddenMarks::View none{};
	Require(Decide(none, farAhead, farAhead, 12.0f, 300.0f, 400.0f, 0.0f, 30.0f) == Release::kOutOfView,
		"Without a camera nothing is held back");

	HiddenMarks::Queue<int> queue;
	int evicted = 0, firstEvicted = -1;
	for (int i = 0; i < static_cast<int>(HiddenMarks::kCapacity) + 10; ++i) {
		if (const auto out = queue.Push(i)) {
			if (firstEvicted < 0) { firstEvicted = *out; }
			++evicted;
		}
	}
	Require(queue.items.size() == HiddenMarks::kCapacity && evicted == 10 && firstEvicted == 0 &&
		queue.items.front() == 10, "A full queue must push out its oldest marks, to be pressed, never lose them");
	std::printf("PASS hidden first-person marks: in view waits, out of view / %0.f units / %0.f s release, "
		"trench ends, no camera, queue of %zu\n", 400.0f, 30.0f, HiddenMarks::kCapacity);
}
// A 4x4 inverse (Gauss-Jordan, rows), for the snow-on-objects camera.
std::array<float, 16> Inverse4(std::array<float, 16> a_m)
{
	std::array<float, 16> inv{};
	for (int i = 0; i < 4; ++i) { inv[i * 4 + i] = 1.0f; }
	for (int col = 0; col < 4; ++col) {
		int pivot = col;
		for (int row = col + 1; row < 4; ++row) {
			if (std::abs(a_m[row * 4 + col]) > std::abs(a_m[pivot * 4 + col])) { pivot = row; }
		}
		Require(std::abs(a_m[pivot * 4 + col]) > 1e-12f, "The test camera's matrix must be invertible");
		for (int k = 0; k < 4; ++k) {
			std::swap(a_m[col * 4 + k], a_m[pivot * 4 + k]);
			std::swap(inv[col * 4 + k], inv[pivot * 4 + k]);
		}
		const float scale = 1.0f / a_m[col * 4 + col];
		for (int k = 0; k < 4; ++k) { a_m[col * 4 + k] *= scale; inv[col * 4 + k] *= scale; }
		for (int row = 0; row < 4; ++row) {
			if (row == col) { continue; }
			const float factor = a_m[row * 4 + col];
			for (int k = 0; k < 4; ++k) {
				a_m[row * 4 + k] -= factor * a_m[col * 4 + k];
				inv[row * 4 + k] -= factor * inv[col * 4 + k];
			}
		}
	}
	return inv;
}

void CheckObjectSnow()
{
	using namespace ObjectSnowRule;
	constexpr float land = 0.0f, depth = 35.0f;
	const float roof[4]{ 500.0f, kNoTop, kNoTop, kNoTop };
	const float none[4]{ kNoTop, kNoTop, kNoTop, kNoTop };
	Require(Amount(1.0f, 500.0f, roof, land, depth, 0.5f, 0.85f) > 0.84f, "A flat roof top takes the full snow");
	Require(Amount(0.1f, 500.0f, roof, land, depth, 0.5f, 1.0f) == 0.0f, "A wall at a top's height takes none");
	const float partial = Amount(0.65f, 500.0f, roof, land, depth, 0.5f, 1.0f);
	Require(partial > 0.0f && partial < 1.0f, "A face between MinUp and MinUp + 0.3 takes part of it");
	Require(Amount(1.0f, 400.0f, roof, land, depth, 0.5f, 1.0f) == 0.0f, "A porch floor under its roof stays bare");
	Require(Amount(1.0f, 130.0f, none, land, depth, 0.5f, 1.0f) == 0.0f,
		"With no top (an actor's shoulder over open ground) nothing is whitened");
	const float slope[4]{ 512.0f, kNoTop, kNoTop, kNoTop };
	Require(Amount(0.8f, 500.0f, slope, land, depth, 0.5f, 1.0f) > 0.9f,
		"A sloped roof 12 under its square's top still counts");

	// A 35-degree pitched roof running across squares: every point of it, wherever it lies in
	// its square, is on one of the four nearest probed tops (each square's centre height).
	{
		const float rise = std::tan(35.0f * 3.14159265f / 180.0f);
		const float up = std::cos(35.0f * 3.14159265f / 180.0f);
		float       least = 1.0f;
		for (float x = 8.0f; x <= 120.0f; x += 0.5f) {
			const float  z = 300.0f + rise * x;
			const float  cell = std::floor(x / kTexelSize - 0.5f);
			const float  c0 = (cell + 0.5f) * kTexelSize;
			const float  c1 = c0 + kTexelSize;
			const float  tops[4]{ 300.0f + rise * c0, 300.0f + rise * c1, 300.0f + rise * c0, 300.0f + rise * c1 };
			least = std::min(least, Amount(up, z, tops, land, depth, 0.5f, 1.0f));
		}
		Require(least > 0.99f, "A 35-degree pitched roof takes snow all across its squares");
	}

	// Only tops over the snow count; the snow surface itself is never whitened.
	Require(AboveSnow(500.0f, land, depth) && !AboveSnow(20.0f, land, depth),
		"A roof stands above the snow; a slab the blanket buries does not");
	Require(AboveSnow(10.0f, land, 0.0f), "Without a blanket a slab 10 up stands above the (bare) ground");
	const float porch[4]{ 42.0f, kNoTop, kNoTop, kNoTop };
	Require(AboveSnow(42.0f, land, depth) && Amount(1.0f, 42.0f, porch, land, depth, 0.5f, 1.0f) > 0.9f,
		"A porch top 7 over the snow takes it");
	Require(Amount(1.0f, 35.0f, porch, land, depth, 0.5f, 1.0f) == 0.0f,
		"The blanket's own surface beside a porch top stays as it is");
	const float road[4]{ 14.0f, kNoTop, kNoTop, kNoTop };
	Require(Amount(1.0f, 11.0f, road, land, depth, 0.5f, 1.0f) == 0.0f,
		"A blanket kept low beside a road is ground, not an object top");
	Require(Index(-1, -1) == kMask * kTexels + kMask && Index(static_cast<int32_t>(kTexels), 3) == Index(0, 3),
		"The top grid wraps");
	Require(kMaxDistance < kWindowSize * 0.5f, "The probed reach fits inside the wrapped window");

	if (g_cpuOnly) {
		std::puts("PASS snow-on-objects CPU rules: roofs, slopes, ground exclusion");
		return;
	}
	// The real pass on a device: a camera 1000 up looking straight down (90 degrees), a roof
	// at 500 and a porch floor at 400 on the left, open ground at 0 on the right.
	ComPtr<ID3D11Device>        device;
	ComPtr<ID3D11DeviceContext> context;
	const D3D_FEATURE_LEVEL     level = D3D_FEATURE_LEVEL_11_0;
	Check(D3D11CreateDevice(nullptr, g_driver, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context),
		"Create the snow-on-objects test device");
	const auto compile = [&](const char* a_entry, const char* a_target) {
		ComPtr<ID3DBlob> code, errors;
		const auto hr = D3DCompile(ObjectSnow::kShaderSource, std::strlen(ObjectSnow::kShaderSource), "ObjectSnow",
			nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) && errors) { std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer())); }
		Check(hr, "Compile the snow-on-objects pass");
		return code;
	};
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	const auto vsCode = compile("vsMain", "vs_5_0");
	const auto psCode = compile("psMain", "ps_5_0");
	Check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Pass VS");
	Check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Pass PS");

	constexpr UINT size = 64;
	constexpr float nearPlane = 1.0f, farPlane = 10000.0f, eye = 1000.0f;
	const float a = farPlane / (farPlane - nearPlane), c = -nearPlane * farPlane / (farPlane - nearPlane);
	const auto depthFor = [&](float a_height) { const float viewZ = eye - a_height; return a + c / viewZ; };
	constexpr float depth35 = 35.0f;
	std::vector<float> depthPixels(size * size);
	for (UINT y = 0; y < size; ++y) {
		for (UINT x = 0; x < size; ++x) {
			depthPixels[y * size + x] = x < size / 2 ? depthFor(y < size / 2 ? 500.0f : 400.0f) :
			                                     depthFor(y < size / 2 ? 0.0f : depth35);
		}
	}
	// A body on the roof: pixels 8..10 across, 9..11 down lie 8 units over it, and a limb
	// hangs 6 under its top below that (rows 12..14).
	for (UINT y = 9; y <= 14; ++y) {
		for (UINT x = 8; x <= 10; ++x) {
			depthPixels[y * size + x] = depthFor(y <= 11 ? 508.0f : 494.0f);
		}
	}
	D3D11_TEXTURE2D_DESC desc{ size, size, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
		D3D11_BIND_SHADER_RESOURCE, 0, 0 };
	ComPtr<ID3D11Texture2D>          depthTexture;
	ComPtr<ID3D11ShaderResourceView> depthView;
	D3D11_SUBRESOURCE_DATA depthData{ depthPixels.data(), size * sizeof(float), 0 };
	Check(device->CreateTexture2D(&desc, &depthData, &depthTexture), "Depth texture");
	Check(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView), "Depth view");

	// Tops (x) and the land (y, 0 everywhere): 500 over the left half (x < -16); a porch top 42
	// over the right-bottom quarter, where the blanket's surface (35) is what is drawn; nothing
	// over the right-top quarter (open ground).
	std::vector<float> tops(static_cast<size_t>(kTexels) * kTexels * 2, kNoTop);
	for (int32_t cellY = -128; cellY < 128; ++cellY) {
		for (int32_t cellX = -128; cellX < 128; ++cellX) {
			tops[2 * static_cast<size_t>(Index(cellX, cellY)) + 1] = 0.0f;
		}
	}
	for (int32_t cellY = -100; cellY < 100; ++cellY) {
		for (int32_t cellX = -100; cellX < -1; ++cellX) {
			tops[2 * static_cast<size_t>(Index(cellX, cellY))] = 500.0f;
		}
	}
	for (int32_t cellY = -100; cellY < -1; ++cellY) {
		for (int32_t cellX = 2; cellX < 100; ++cellX) {
			tops[2 * static_cast<size_t>(Index(cellX, cellY))] = 42.0f;
		}
	}
	D3D11_TEXTURE2D_DESC topsDesc = desc;
	topsDesc.Width = kTexels;
	topsDesc.Height = kTexels;
	topsDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
	ComPtr<ID3D11Texture2D>          topsTexture;
	ComPtr<ID3D11ShaderResourceView> topsView;
	D3D11_SUBRESOURCE_DATA topsData{ tops.data(), kTexels * 2 * sizeof(float), 0 };
	Check(device->CreateTexture2D(&topsDesc, &topsData, &topsTexture), "Tops texture");
	Check(device->CreateShaderResourceView(topsTexture.Get(), nullptr, &topsView), "Tops view");

	// clip = M (world - eye position), view z pointing down; the game's cb12 holds its inverse.
	const std::array<float, 16> m{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -a, c, 0, 0, -1, 0 };
	const auto inverse = Inverse4(m);
	std::array<float, 36 * 4> perFrame{};
	std::copy(inverse.begin(), inverse.end(), perFrame.begin() + 16 * 4);
	perFrame[35 * 4 + 2] = eye;
	D3D11_BUFFER_DESC cbDesc{ static_cast<UINT>(perFrame.size() * sizeof(float)), D3D11_USAGE_DEFAULT,
		D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	ComPtr<ID3D11Buffer> frameCB, paramsCB;
	D3D11_SUBRESOURCE_DATA frameData{ perFrame.data(), 0, 0 };
	Check(device->CreateBuffer(&cbDesc, &frameData, &frameCB), "PerFrame buffer");
	// The body as one capsule along the roof (world x -370 to -320, y 330, z 505), radius 50.
	float params[20 + 8 * kMaxActorCapsules]{ 0, 0, size, size, kTexelSize, static_cast<float>(kTexels), 0, 0, 1500.0f,
		225.0f, 72.0f, 0.85f, 0.5f, kFadeUp, 0.84f, kTopTolerance, depth35, 0.0f, kSurfaceBand, 1.0f, -370.0f, 330.0f,
		505.0f, 50.0f, -320.0f, 330.0f, 505.0f, 0.0f };
	cbDesc.ByteWidth = sizeof(params);
	D3D11_SUBRESOURCE_DATA paramsData{ params, 0, 0 };
	Check(device->CreateBuffer(&cbDesc, &paramsData, &paramsCB), "Params buffer");

	std::vector<uint32_t> grey(size * size, 0x80333333u);  // 0.2 grey, alpha 0.5
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D>        albedo, readback;
	ComPtr<ID3D11RenderTargetView> albedoView;
	D3D11_SUBRESOURCE_DATA greyData{ grey.data(), size * sizeof(uint32_t), 0 };
	Check(device->CreateTexture2D(&desc, &greyData, &albedo), "Albedo target");
	Check(device->CreateRenderTargetView(albedo.Get(), nullptr, &albedoView), "Albedo view");

	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
		D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
		D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE };
	ComPtr<ID3D11BlendState> blendState;
	Check(device->CreateBlendState(&blend, &blendState), "Blend state");
	D3D11_RASTERIZER_DESC raster{ D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE };
	ComPtr<ID3D11RasterizerState> rasterState;
	Check(device->CreateRasterizerState(&raster, &rasterState), "Raster state");

	const D3D11_VIEWPORT viewport{ 0, 0, size, size, 0, 1 };
	context->OMSetRenderTargets(1, albedoView.GetAddressOf(), nullptr);
	context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);
	context->RSSetState(rasterState.Get());
	context->RSSetViewports(1, &viewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs.Get(), nullptr, 0);
	context->PSSetShader(ps.Get(), nullptr, 0);
	ID3D11ShaderResourceView* views[2]{ depthView.Get(), topsView.Get() };
	context->PSSetShaderResources(0, 2, views);
	context->PSSetConstantBuffers(0, 1, paramsCB.GetAddressOf());
	context->PSSetConstantBuffers(12, 1, frameCB.GetAddressOf());
	context->Draw(3, 0);

	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	Check(device->CreateTexture2D(&desc, nullptr, &readback), "Readback");
	context->CopyResource(readback.Get(), albedo.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map readback");
	const auto at = [&](UINT a_x, UINT a_y) {
		return static_cast<const uint32_t*>(mapped.pData)[a_y * (mapped.RowPitch / 4) + a_x];
	};
	const uint32_t roofPixel = at(16, 16), floorPixel = at(16, 48), groundPixel = at(48, 16), blanketPixel = at(48, 48);
	const uint32_t bodyPixel = at(9, 10), limbPixel = at(9, 13), roofBesideBody = at(12, 10);
	context->Unmap(readback.Get(), 0);
	const auto red = [](uint32_t a_p) { return a_p & 0xFFu; };
	const auto alpha = [](uint32_t a_p) { return a_p >> 24; };
	Require(red(roofPixel) > 140 && alpha(roofPixel) == 0x80, "The roof top must turn white, its alpha kept");
	Require(bodyPixel == 0x80333333u, "A body lying on the roof keeps its own colour");
	Require(limbPixel == 0x80333333u, "A limb hanging under the roof's top keeps its own colour");
	Require(red(roofBesideBody) > 140, "The roof beside the body, inside its bound, still whitens");
	Require(ActorAllows(0.0f, true) && !ActorAllows(8.0f, true) && !ActorAllows(-6.0f, true) && ActorAllows(8.0f, false),
		"Inside an actor's body only the top's own surface is a top");
	Require(floorPixel == 0x80333333u, "The porch floor under the roof must stay as it was");
	Require(groundPixel == 0x80333333u, "Open ground (the blanket's) must stay as it was");
	Require(blanketPixel == 0x80333333u, "The blanket's surface beside a low porch top must stay as it was");
	std::printf("PASS snow on objects: tops, walls, slopes, a 35-degree pitched roof, floors under roofs, only tops "
		"over the snow, never the snow surface, wrapping; on a device the roof whitens (red %u), the floor, the "
		"ground and the blanket beside a low top keep their albedo, alpha untouched\n", red(roofPixel));
}

// The view tests and the passes over the G-buffer take the world camera from a copy of the
// engine's per-frame constants (cb12). A real camera there reads back with its eye, axes and
// projection; a buffer from no camera (zeros, an orthographic map camera, view rows out of
// place, a camera looking the other way) is refused, so nothing is hidden from it.
void CheckFrameCamera()
{
	// A camera at (-80000, 90000, 7600) looking north-east and a little down: 16:9, 70 degrees
	// of vertical view, near 15, a temporal jitter of a fraction of a pixel, laid out as
	// SetPerFrameConstants uploads it (view rows c0-c2, projection c4, camera-relative
	// view-projection c8, position adjust c35).
	const float eye[3]{ -80000.0f, 90000.0f, 7600.0f };
	const float fl = std::sqrt(0.7f * 0.7f + 0.7f * 0.7f + 0.14f * 0.14f);
	const float f[3]{ 0.7f / fl, 0.7f / fl, -0.14f / fl };
	const float rl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
	const float r[3]{ f[1] / rl, -f[0] / rl, 0.0f };
	const float u[3]{ r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
	const float sy = 1.0f / std::tan(0.5f * 70.0f * 3.14159265f / 180.0f);
	const float sx = sy / (16.0f / 9.0f);
	const float zn = 15.0f, zf = 350000.0f;
	const float a = zf / (zf - zn), b = -zn * zf / (zf - zn);
	const float projection[16]{ sx, 0, 0.0004f, 0, 0, sy, -0.0004f, 0, 0, 0, a, b, 0, 0, 1, 0 };
	const float view[16]{ r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, f[0], f[1], f[2], 0, 0, 0, 0, 1 };
	const auto  product = [](const float (&a_l)[16], const float (&a_r)[16], float (&a_out)[16]) {
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				a_out[i * 4 + j] = a_l[i * 4 + 0] * a_r[0 * 4 + j] + a_l[i * 4 + 1] * a_r[1 * 4 + j] +
				                   a_l[i * 4 + 2] * a_r[2 * 4 + j] + a_l[i * 4 + 3] * a_r[3 * 4 + j];
			}
		}
	};
	float viewProj[16];
	product(projection, view, viewProj);
	const auto upload = [&](const float (&a_view)[16], const float (&a_viewProj)[16]) {
		std::array<float, FrameCamera::kMinFloats> cb{};
		std::copy_n(a_view, 12, cb.begin() + FrameCamera::kViewRegister * 4);
		std::copy_n(projection, 16, cb.begin() + FrameCamera::kProjRegister * 4);
		std::copy_n(a_viewProj, 16, cb.begin() + FrameCamera::kViewProjRegister * 4);
		std::copy_n(eye, 3, cb.begin() + FrameCamera::kPosAdjustRegister * 4);
		return cb;
	};

	const auto          cb = upload(view, viewProj);
	FrameCamera::Camera camera{};
	Require(FrameCamera::Parse(cb.data(), cb.size(), camera), "A real camera in the per-frame constants reads back");
	Require(camera.eye[0] == eye[0] && camera.eye[1] == eye[1] && camera.eye[2] == eye[2], "Its eye is the position adjust");
	Require(camera.forward[0] == f[0] && camera.forward[1] == f[1] && camera.forward[2] == f[2] &&
				camera.right[0] == r[0] && camera.up[2] == u[2],
		"Its axes are the view's rows");
	Require(camera.proj[0] == sx && camera.proj[5] == sy, "Its projection is kept as stored");
	Require(!FrameCamera::Parse(cb.data(), cb.size() - 4, camera), "A buffer too short for the eye is refused");

	const std::array<float, FrameCamera::kMinFloats> zeros{};
	Require(!FrameCamera::Parse(zeros.data(), zeros.size(), camera), "A buffer of zeros (no camera yet) is refused");

	// A map's orthographic camera: w stays 1, so nothing ahead lands at its distance.
	const float ortho[16]{ 2.0f / 4096.0f, 0, 0, 0, 0, 2.0f / 4096.0f, 0, 0, 0, 0, 1.0f / zf, 0, 0, 0, 0, 1 };
	float       orthoViewProj[16];
	product(ortho, view, orthoViewProj);
	const auto map = upload(view, orthoViewProj);
	Require(!FrameCamera::Parse(map.data(), map.size(), camera), "An orthographic camera is refused");

	// The view's rows a register further on (a moved layout): the first row reads as zeros.
	auto moved = cb;
	std::fill_n(moved.begin(), 16, 0.0f);
	std::copy_n(view, 12, moved.begin() + 4);
	Require(!FrameCamera::Parse(moved.data(), moved.size(), camera), "View rows out of place are refused");

	// The view's forward row turned around while the view-projection is not: w goes negative.
	float turned[16];
	std::copy_n(view, 16, turned);
	for (int i = 8; i < 11; ++i) {
		turned[i] = -turned[i];
	}
	const auto away = upload(turned, viewProj);
	Require(!FrameCamera::Parse(away.data(), away.size(), camera), "A forward that disagrees with the projection is refused");

	std::printf("PASS frame camera: a real camera reads back with its eye, axes and projection; a short buffer, "
		"zeros, an orthographic camera, rows out of place and a turned-around forward are refused\n");
}

void CheckIndoorPrints()
{
	using namespace IndoorPrintRule;

	// Boots: mud loads them fully; each print takes a step, clean after the set number.
	Boots boots{};
	Walk(boots, Dirt::kMud, 1.0f, 10.0f);
	Require(boots.dirt == Dirt::kMud && boots.level == 1.0f, "Mud loads the boots fully");
	float previous = 2.0f;
	int   printCount = 0;
	for (int i = 0; i < 20; ++i) {
		const float strength = TakeStep(boots, 14);
		if (strength <= 0.0f) {
			break;
		}
		Require(strength < previous, "Each print is fainter than the one before");
		previous = strength;
		++printCount;
	}
	Require(printCount == 14 && boots.dirt == Dirt::kNone, "Fully loaded boots leave 14 prints, then are clean");
	Require(TakeStep(boots, 14) == 0.0f, "Clean boots leave no print");

	// A dirtier ground takes over; a cleaner one does not, and clean ground wipes them.
	Walk(boots, Dirt::kDust, 0.7f, 10.0f);
	Walk(boots, Dirt::kDust, 0.25f, 10.0f);
	Require(boots.dirt == Dirt::kDust && boots.level < 0.7f && boots.level > 0.68f,
		"Grass after dirt wipes the boots only by the distance walked on it");
	Walk(boots, Dirt::kWet, 1.0f, 10.0f);
	Require(boots.dirt == Dirt::kWet && boots.level == 1.0f, "Snow after dust makes the boots wet");
	Walk(boots, Dirt::kNone, 0.0f, kCleanTravel * 0.5f);
	Require(boots.level > 0.45f && boots.level < 0.55f, "Half the cleaning travel on clean ground wipes half");
	Walk(boots, Dirt::kNone, 0.0f, kCleanTravel);
	Require(boots.dirt == Dirt::kNone && boots.level == 0.0f, "Walking long enough on clean ground cleans them");

	// Fading: a wet print dries first, mud stays longest; full for the first half of a life.
	Require(Fade(10.0f, 600.0f, Dirt::kWet) == 1.0f && Fade(240.0f, 600.0f, Dirt::kWet) == 0.0f,
		"A wet print dries in 0.4 of the base life");
	const float drying = Fade(180.0f, 600.0f, Dirt::kWet);
	Require(drying > 0.0f && drying < 1.0f, "A wet print fades over the second half of its life");
	Require(Fade(800.0f, 600.0f, Dirt::kMud) > 0.0f && Fade(800.0f, 600.0f, Dirt::kDust) == 0.0f,
		"Mud outlasts dust");

	// The ring keeps the newest, overwriting the oldest, and shrinks to a lowered cap.
	Ring<int> ring;
	for (int i = 0; i < 600; ++i) {
		ring.Push(i, 512);
	}
	Require(ring.items.size() == 512 && std::ranges::find(ring.items, 599) != ring.items.end() &&
				std::ranges::find(ring.items, 87) == ring.items.end(),
		"The ring holds the 512 newest");
	ring.Push(600, 16);
	Require(ring.items.size() == 16 && std::ranges::find(ring.items, 600) != ring.items.end() &&
				std::ranges::find(ring.items, 584) == ring.items.end() &&
				std::ranges::find(ring.items, 585) != ring.items.end(),
		"A lowered cap keeps the newest");
	const auto ordered = [](const Ring<int>& a_ring) {
		std::vector<int> out;
		const size_t start = a_ring.items.size() == a_ring.limit ? a_ring.next : 0;
		for (size_t i = 0; i < a_ring.items.size(); ++i) {
			out.push_back(a_ring.items[(start + i) % a_ring.items.size()]);
		}
		return out;
	};
	ring.Clear();
	for (int i = 1; i <= 4; ++i) { ring.Push(i, 3); }
	Require(ordered(ring) == std::vector<int>{ 2, 3, 4 }, "A wrapped ring remains oldest first");
	for (int i = 5; i <= 8; ++i) { ring.Push(i, 5); }
	Require(ordered(ring) == std::vector<int>{ 4, 5, 6, 7, 8 }, "Growing a wrapped ring preserves newest order");
	ring.Clear();
	for (int i = 1; i <= 5; ++i) { ring.Push(i, 10); }
	ring.Push(6, 3);
	Require(ordered(ring) == std::vector<int>{ 4, 5, 6 }, "Shrinking a partly filled ring keeps the newest");

	// The box: inside around the sole, outside past its length or height, mirrored across.
	const Box box{ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, kHalfLength, kHalfWidth, 1.0f };
	float across = 0.0f, along = 0.0f, up = 0.0f;
	Require(Local(box, 2.0f, 6.0f, 1.0f, across, along, up), "A point on the sole is inside");
	Require(!Local(box, 0.0f, kHalfLength + 1.0f, 0.0f, across, along, up), "Past the toe is outside");
	Require(!Local(box, 0.0f, 0.0f, kHalfHeight + 1.0f, across, along, up), "Above the floor is outside");
	Box left = box;
	left.mirror = -1.0f;
	float acrossLeft = 0.0f;
	Local(box, 2.0f, 0.0f, 0.0f, across, along, up);
	Local(left, 2.0f, 0.0f, 0.0f, acrossLeft, along, up);
	Require(across == -acrossLeft, "A left print is the right one mirrored");
	if (g_cpuOnly) {
		std::puts("PASS floor-print CPU rules: boots, fading, ring resize, local geometry");
		return;
	}

	// The real pass on a device: a camera 40 up looking straight down, a light floor at 0 on
	// the left and a step at 20 on the right, one mud print on the left floor.
	ComPtr<ID3D11Device>        device;
	ComPtr<ID3D11DeviceContext> context;
	const D3D_FEATURE_LEVEL     level = D3D_FEATURE_LEVEL_11_0;
	Check(D3D11CreateDevice(nullptr, g_driver, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context),
		"Create the floor-print test device");
	const auto compile = [&](const char* a_entry, const char* a_target) {
		ComPtr<ID3DBlob> code, errors;
		const auto hr = D3DCompile(IndoorPrints::kShaderSource, std::strlen(IndoorPrints::kShaderSource),
			"IndoorPrints", nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) && errors) { std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer())); }
		Check(hr, "Compile the floor-print pass");
		return code;
	};
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	const auto vsCode = compile("vsMain", "vs_5_0");
	const auto psCode = compile("psMain", "ps_5_0");
	Check(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Print VS");
	Check(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Print PS");

	constexpr UINT  size = 64;
	constexpr float nearPlane = 1.0f, farPlane = 10000.0f, eye = 40.0f;
	const float     a = farPlane / (farPlane - nearPlane), c = -nearPlane * farPlane / (farPlane - nearPlane);
	const auto      depthFor = [&](float a_height) { const float viewZ = eye - a_height; return a + c / viewZ; };
	std::vector<float> depth(size * size);
	for (UINT y = 0; y < size; ++y) {
		for (UINT x = 0; x < size; ++x) {
			depth[y * size + x] = depthFor(x < size / 2 ? 0.0f : 20.0f);
		}
	}
	D3D11_TEXTURE2D_DESC desc{ size, size, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
		D3D11_BIND_SHADER_RESOURCE, 0, 0 };
	ComPtr<ID3D11Texture2D>          depthTexture;
	ComPtr<ID3D11ShaderResourceView> depthView;
	D3D11_SUBRESOURCE_DATA           depthData{ depth.data(), size * sizeof(float), 0 };
	Check(device->CreateTexture2D(&desc, &depthData, &depthTexture), "Depth texture");
	Check(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView), "Depth view");

	// The game's per-frame constants (cb12): clip = M (world - eye) at c8, its inverse at c16,
	// the eye (0, 0, 40) at c35; view z pointing down.
	const std::array<float, 16> m{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -a, c, 0, 0, -1, 0 };
	const auto                  inverse = Inverse4(m);
	std::array<float, 36 * 4>   perFrame{};
	std::copy(m.begin(), m.end(), perFrame.begin() + 8 * 4);
	std::copy(inverse.begin(), inverse.end(), perFrame.begin() + 16 * 4);
	perFrame[35 * 4 + 2] = eye;
	D3D11_BUFFER_DESC cbDesc{ static_cast<UINT>(perFrame.size() * sizeof(float)), D3D11_USAGE_DEFAULT,
		D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	ComPtr<ID3D11Buffer>   perFrameCB;
	D3D11_SUBRESOURCE_DATA perFrameData{ perFrame.data(), 0, 0 };
	Check(device->CreateBuffer(&cbDesc, &perFrameData, &perFrameCB), "PerFrame buffer");

	// The pass's own constants: the viewport, then no mask (the drawn sole), min up 0.6 and
	// the nearest distance kept.
	std::array<float, 8> frame{ 0.0f, 0.0f, float(size), float(size), 0.0f, 0.6f, 4.0f, 0.0f };
	cbDesc.ByteWidth = static_cast<UINT>(frame.size() * sizeof(float));
	ComPtr<ID3D11Buffer>   frameCB;
	D3D11_SUBRESOURCE_DATA frameData{ frame.data(), 0, 0 };
	Check(device->CreateBuffer(&cbDesc, &frameData, &frameCB), "Print frame buffer");

	// One right-foot mud print at (-15, 0) pointing +y; the floor there is at 0.
	const auto            tint = TintFor(Dirt::kMud);
	std::array<float, 16> print{ -15.0f, 0.0f, 0.0f, kHalfHeight, 0.0f, 1.0f, kHalfLength, kHalfWidth,
		tint.r, tint.g, tint.b, tint.alpha * 0.6f, 1.0f, 0.0f, 0.0f, 0.0f };
	D3D11_BUFFER_DESC structured{ static_cast<UINT>(sizeof(print)), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
		D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, static_cast<UINT>(sizeof(print)) };
	ComPtr<ID3D11Buffer>             prints;
	ComPtr<ID3D11ShaderResourceView> printView;
	D3D11_SUBRESOURCE_DATA           printData{ print.data(), 0, 0 };
	Check(device->CreateBuffer(&structured, &printData, &prints), "Print buffer");
	D3D11_SHADER_RESOURCE_VIEW_DESC printViewDesc{};
	printViewDesc.Format = DXGI_FORMAT_UNKNOWN;
	printViewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	printViewDesc.Buffer.NumElements = 1;
	Check(device->CreateShaderResourceView(prints.Get(), &printViewDesc, &printView), "Print view");

	std::vector<uint32_t> light(size * size, 0x80B0B0B0u);  // a light floor, alpha 0.5
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D>        albedo, readback;
	ComPtr<ID3D11RenderTargetView> albedoView;
	D3D11_SUBRESOURCE_DATA         lightData{ light.data(), size * sizeof(uint32_t), 0 };
	Check(device->CreateTexture2D(&desc, &lightData, &albedo), "Albedo target");
	Check(device->CreateRenderTargetView(albedo.Get(), nullptr, &albedoView), "Albedo view");

	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
		D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
		D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE };
	ComPtr<ID3D11BlendState> blendState;
	Check(device->CreateBlendState(&blend, &blendState), "Blend state");
	// As the plugin draws them: one side of each box, so a pixel is blended once.
	D3D11_RASTERIZER_DESC raster{ D3D11_FILL_SOLID, D3D11_CULL_FRONT, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE };
	ComPtr<ID3D11RasterizerState> rasterState;
	Check(device->CreateRasterizerState(&raster, &rasterState), "Raster state");

	const D3D11_VIEWPORT viewport{ 0, 0, size, size, 0, 1 };
	context->OMSetRenderTargets(1, albedoView.GetAddressOf(), nullptr);
	context->OMSetBlendState(blendState.Get(), nullptr, 0xFFFFFFFF);
	context->RSSetState(rasterState.Get());
	context->RSSetViewports(1, &viewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs.Get(), nullptr, 0);
	context->PSSetShader(ps.Get(), nullptr, 0);
	ID3D11ShaderResourceView* vsViews[2]{ nullptr, printView.Get() };
	context->VSSetShaderResources(0, 2, vsViews);
	ID3D11ShaderResourceView* psViews[3]{ depthView.Get(), printView.Get(), nullptr };
	context->PSSetShaderResources(0, 3, psViews);
	context->VSSetConstantBuffers(0, 1, frameCB.GetAddressOf());
	context->PSSetConstantBuffers(0, 1, frameCB.GetAddressOf());
	context->VSSetConstantBuffers(12, 1, perFrameCB.GetAddressOf());
	context->PSSetConstantBuffers(12, 1, perFrameCB.GetAddressOf());
	context->DrawInstanced(36, 1, 0, 0);

	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	Check(device->CreateTexture2D(&desc, nullptr, &readback), "Readback");
	context->CopyResource(readback.Get(), albedo.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map readback");
	const auto at = [&](UINT a_x, UINT a_y) {
		return static_cast<const uint32_t*>(mapped.pData)[a_y * (mapped.RowPitch / 4) + a_x];
	};
	// x = -15 is pixel 20; the sole's middle at y = 0 is row 32; y = +30 is row 8.
	const uint32_t sole = at(20, 32), beyondToe = at(20, 8), step = at(50, 32), corner = at(2, 2);
	context->Unmap(readback.Get(), 0);
	const auto red = [](uint32_t a_p) { return a_p & 0xFFu; };
	const auto alpha = [](uint32_t a_p) { return a_p >> 24; };
	Require(red(sole) < 0xB0 - 50 && alpha(sole) == 0x80, "The floor under the sole must turn muddy, its alpha kept");
	Require(beyondToe == 0x80B0B0B0u && corner == 0x80B0B0B0u, "The floor outside the print must stay as it was");
	Require(step == 0x80B0B0B0u, "A surface outside the print's height must stay as it was");
	std::printf("PASS floor prints: boots load, fade step by step and clean; prints dry, mud lasts; the ring keeps "
		"the newest; on a device the sole darkens the floor (red %u from %u), nothing else changes, alpha "
		"untouched\n", red(sole), 0xB0u);
}

// Corpses: the life state, settling, burial, the strength between stampings, the body's
// capsules from its bones, the clearing and the round robin.
void CheckCorpseRule()
{
	using namespace CorpseRule;

	const auto word = [](uint32_t a_state) { return (a_state << kLifeStateShift) | 0x0001FFFFu | (0xFu << 21); };
	Require(IsCorpseState(LifeStateOf(word(kDying))) && IsCorpseState(LifeStateOf(word(kDead))) &&
				IsCorpseState(LifeStateOf(word(kRecycle))),
		"Dying, dead and recycled bodies are corpses");
	for (const uint32_t state : { kAlive, kUnconscious, kReanimate, kRestrained, kEssentialDown, kBleedout }) {
		Require(!IsCorpseState(LifeStateOf(word(state))), "The living, the downed and the bleeding out are not corpses");
	}
	// The melee attack bits (25-27) sit above the life state and the knock state.
	Require(MeleeAttackOf(word(kDead) | (2u << kMeleeAttackShift)) == 2 && MeleeAttackOf(word(kDead)) == 0 &&
				LifeStateOf(word(kDead) | (7u << kMeleeAttackShift)) == kDead,
		"The melee attack state reads apart from the life state");

	// Settling: a second of stillness at 60 Hz, never while falling; a twitch keeps it, a drag
	// unsettles it; a paused frame changes nothing.
	const float dt = 1.0f / 60.0f;
	Settle settle{};
	int     frames = 0;
	while (!UpdateSettle(settle, 0.05f, 0.0f, dt) && frames < 600) {
		++frames;
	}
	Require(frames >= 58 && frames <= 61, "A still body settles after about a second");
	Settle falling{};
	for (int i = 0; i < 600; ++i) {
		UpdateSettle(falling, 3.0f, 0.0f, dt);
	}
	Require(!falling.settled, "A falling body never settles");
	Require(UpdateSettle(settle, 5.0f, 2.0f, dt), "A twitch near where it settled keeps the body at rest");
	Require(!UpdateSettle(settle, 5.0f, kMoveTolerance + 1.0f, dt), "A body dragged away is no longer at rest");
	const Settle before = settle;
	UpdateSettle(settle, 0.0f, 0.0f, 0.0f);
	Require(settle.stillSeconds == before.stillSeconds && settle.settled == before.settled, "A paused frame changes nothing");

	// Burial: none without snowfall; full snowfall refills the hollow in half the time and
	// grows the mound in the other half; light snowfall is slower.
	Require(AdvanceBurial(0.3f, 0.0f, 10.0f, 600.0f) == 0.3f, "Without snowfall the burial stays");
	float burial = 0.0f;
	for (int i = 0; i < 300 * 60; ++i) {
		burial = AdvanceBurial(burial, 1.0f, dt, 600.0f);
	}
	Require(std::abs(burial - 0.5f) < 1e-3f && HollowShare(burial) < 0.01f && MoundShare(burial) < 0.01f,
		"Half the burial time fills the hollow");
	for (int i = 0; i < 310 * 60; ++i) {
		burial = AdvanceBurial(burial, 1.0f, dt, 600.0f);
	}
	Require(burial == 1.0f && MoundShare(burial) == 1.0f, "The whole burial time grows the full mound");
	Require(std::abs(AdvanceBurial(0.0f, 0.5f, 60.0f, 600.0f) - 0.05f) < 1e-5f, "Light snowfall buries at its share");

	float previous = -1.0e9f;
	for (int i = 0; i <= 100; ++i) {
		const float b = static_cast<float>(i) / 100.0f;
		const float floor = FloorFor(b, 37.0f, 6.0f);
		Require(floor >= previous - 1e-5f, "The floor only rises as the body is buried");
		Require(MoundShare(b) <= 0.0f || HollowShare(b) <= 0.0f, "The mound grows only over a filled hollow");
		Require(b <= kRefillShare ? floor <= 0.0f : floor >= 0.0f, "Below the snow while filling, above it after");
		previous = floor;
	}
	Require(FloorFor(0.0f, 37.0f, 6.0f) == -37.0f && FloorFor(1.0f, 37.0f, 6.0f) == 6.0f, "Hollow and mound ends");

	// The strength makes a stamping every 6 frames close the gap 6 frames would, as far as the
	// per-second encoding goes; past that it saturates instead of wrapping.
	const float strength = CompensatedStrength(0.5f, 6.0f * dt, dt);
	Require(std::abs(std::pow(1.0f - strength, dt) - std::pow(0.5f, 6.0f * dt)) < 1e-4f,
		"Stamping every few frames draws the field as fast as every frame");
	const float saturated = CompensatedStrength(kDrawPerSecond, 6.0f * dt, dt);
	Require(saturated <= 0.9999f && saturated > kDrawPerSecond, "A strength past the encoding saturates");
	Require(CompensatedStrength(kDrawPerSecond, 1.0f, 0.0f) == kDrawPerSecond, "A paused frame keeps the plain strength");

	// A person lying on its back along +x (z 10 over the ground): eleven parts, the head
	// running on past its bone, every radius within its limits.
	const std::array<Bone, 18> person{ {
		{ "Pelvis", { 0, 0, 10 } }, { "SPINE2", { 20, 0, 10 } }, { "Neck", { 40, 0, 10 } }, { "HEAD", { 46, 0, 10 } },
		{ "LArm_UpperArm", { 36, 10, 10 } }, { "LArm_ForeArm1", { 24, 14, 10 } }, { "LArm_Hand", { 12, 16, 10 } },
		{ "RArm_UpperArm", { 36, -10, 10 } }, { "RArm_ForeArm1", { 24, -14, 10 } }, { "RArm_Hand", { 12, -16, 10 } },
		{ "LLeg_Thigh", { 0, 5, 10 } }, { "LLeg_Calf", { -22, 6, 10 } }, { "LLeg_Foot", { -44, 7, 10 } },
		{ "RLeg_Thigh", { 0, -5, 10 } }, { "RLeg_Calf", { -22, -6, 10 } }, { "RLeg_Foot", { -44, -7, 10 } },
		{ "LLeg_Calf_skin", { 99, 99, 99 } }, { "TFP_Unrelated", { 99, 99, 99 } },
	} };
	std::array<Capsule, kMaxCapsules> capsules{};
	Require(CapsulesFromBones(person, capsules) == 11, "A person gives eleven body parts");
	Require(capsules[2].b.x > 46.0f && capsules[2].b.x < 52.0f, "The head runs on past its bone");
	for (size_t i = 0; i < 11; ++i) {
		Require(capsules[i].radius >= 2.0f && capsules[i].radius <= 18.0f, "Body part radii stay within their limits");
	}

	// A feral ghoul's arm has no forearm bone: its upper arm runs to its hand.
	const std::array<Bone, 6> ghoul{ {
		{ "Pelvis", { 0, 0, 8 } }, { "SPINE2", { 18, 0, 8 } }, { "Neck", { 36, 0, 8 } },
		{ "LUPPERARM", { 32, 9, 8 } }, { "LHAND", { 10, 14, 8 } }, { "LTHIGH", { 0, 5, 8 } },
	} };
	const size_t ghoulParts = CapsulesFromBones(ghoul, capsules);
	Require(ghoulParts == 3 && std::abs(capsules[2].a.x - 32.0f) < 1e-4f && std::abs(capsules[2].b.x - 10.0f) < 1e-4f,
		"A ghoul's upper arm joins its hand");

	const std::array<Bone, 3> unknown{ { { "Bone01", { 0, 0, 0 } }, { "Bone02", { 9, 0, 0 } }, { "Pelvis", { 0, 0, 0 } } } };
	Require(CapsulesFromBones(unknown, capsules) < kMinNamedCapsules, "A creature the chains do not know falls back");
	const auto bound = CapsuleFromBound({ 0, 0, 0 }, 40.0f);
	Require(bound.radius == 18.0f && bound.a.x == bound.b.x, "The fallback is one disc of about half the bound");

	// Clearing: down to a body on the land (the blanket and a margin, never past the bite),
	// less for a body on a buried road, none for one on a porch above the snow.
	const Capsule torso{ { 0, 0, 9 }, { 20, 0, 9 }, 9 };
	Require(std::abs(ClearingDepth(torso, 0.0f, 35.0f, 4.0f) - 37.0f) < 1e-4f, "A body on the land is cleared to");
	const Capsule onRoad{ { 0, 0, 19 }, { 20, 0, 19 }, 9 };
	Require(std::abs(ClearingDepth(onRoad, 0.0f, 35.0f, 4.0f) - 27.0f) < 1e-4f, "A body on a buried road is cleared to the road");
	const Capsule onPorch{ { 0, 0, 49 }, { 20, 0, 49 }, 9 };
	Require(ClearingDepth(onPorch, 0.0f, 35.0f, 4.0f) == 0.0f, "A body above the snow clears nothing");
	Require(ClearingDepth(torso, 0.0f, 0.0f, 4.0f) == 0.0f, "No blanket, no clearing");
	const Capsule sunk{ { 0, 0, -30 }, { 20, 0, -30 }, 9 };
	Require(ClearingDepth(sunk, 0.0f, 35.0f, 4.0f) == 39.0f, "The clearing never passes the bite below the blanket");

	// Round robin: never past the room, a corpse whole or not at all, every due one in turn,
	// one too big for the room waits at the front instead of being passed over.
	const std::array<size_t, 3>  parts{ 11, 11, 1 };
	const std::array<uint8_t, 3> allDue{ 1, 1, 1 };
	auto                         pick = PickCorpses(parts, allDue, 20, 0);
	Require(pick.chosen.size() == 1 && pick.chosen[0] == 0 && pick.cursor == 1, "Only what fits; the next waits");
	pick = PickCorpses(parts, allDue, 20, pick.cursor);
	Require(pick.chosen.size() == 2 && pick.chosen[0] == 1 && pick.chosen[1] == 2, "The waiting corpse goes first");
	const std::array<uint8_t, 3> firstDone{ 0, 1, 1 };
	pick = PickCorpses(parts, firstDone, 64, 0);
	Require(pick.chosen.size() == 2 && pick.chosen[0] == 1, "A corpse stamped a moment ago is not due");
	Require(PickCorpses(parts, allDue, 0, 2).chosen.empty(), "No room, no corpses");
	const std::array<size_t, 0> none{};
	Require(PickCorpses(none, std::span<const uint8_t>{}, 64, 5).chosen.empty(), "No corpses, nothing picked");

	std::puts("PASS corpses: life state as IsDead(true), settling, burial (refill then mound, none without "
	          "snowfall), strength between stampings, body parts from person, ghoul and unknown skeletons, clearing, "
	          "round robin");
}

// The field itself under a body in the snow (the melt kind): the hollow forms, fills back in,
// a mound grows, and the mound outlasts its stamps; a floor of 0 (heat on bare ground) leaves
// a texel's rate and snow flag as they were.
void CheckCorpseBurial(Field& field)
{
	const auto stamp = [](Params& a_params, float a_floor, float a_strength) {
		a_params.control[1] = 1;
		a_params.stamps[0][0] = 8.0f;
		a_params.stamps[0][1] = 0.0f;
		a_params.stamps[0][2] = 6.0f;
		a_params.stamps[0][3] = a_strength;
		a_params.stampParams[0][0] = 0.6f;
		a_params.stampParams[0][1] = 0.99f;
		a_params.stampParams[0][2] = 1.0f;
		a_params.stampParams[0][3] = a_floor;
		a_params.stampShape[0][2] = 0.0f;
		a_params.stampMotion[0][0] = 16.0f;
		a_params.stampMotion[0][1] = 0.0f;
		a_params.stampMotion[0][2] = 1.0f;
	};
	const auto run = [&](float a_floor, int a_steps) {
		Params p;
		p.control[0] = 0.1f;
		stamp(p, a_floor, CorpseRule::kDrawPerSecond);
		for (int i = 0; i < a_steps; ++i) {
			field.Step(p);
		}
	};
	const auto body = [&] { return field.Read()[0]; };
	const auto outside = [&] { return field.Read()[20 * N + 20]; };

	field.Clear();
	run(-8.0f, 25);
	Require(std::abs(body() + 8.0f) < 0.1f && outside() == 0.0f, "The hollow forms under the body only");
	const uint32_t meta = field.ReadMeta()[0];
	Require((meta & Clipmap::kMetaSnowBit) != 0 && std::abs(Clipmap::DecodeRate(meta) - 0.99f) < 1e-3f,
		"The hollow is snow and keeps the stamp's rate");
	run(-4.0f, 40);
	Require(std::abs(body() + 4.0f) < 0.1f, "Snowfall fills the hollow back in");
	run(3.0f, 40);
	Require(std::abs(body() - 3.0f) < 0.1f && outside() == 0.0f, "A mound grows over the filled hollow");

	Params rest;
	rest.control[0] = 0.1f;
	rest.control[1] = 0;
	for (int i = 0; i < 10; ++i) {
		field.Step(rest);
	}
	Require(std::abs(body() - 3.0f * 0.99f) < 0.02f, "The mound outlasts its stamps at the snow's own rate");

	field.Clear();
	run(3.0f, 40);
	const uint32_t fresh = field.ReadMeta()[0];
	Require(std::abs(body() - 3.0f) < 0.1f && (fresh & Clipmap::kMetaSnowBit) != 0 &&
				std::abs(Clipmap::DecodeRate(fresh) - 0.99f) < 1e-3f,
		"A mound on untouched snow keeps its rate (it does not vanish at once)");

	field.Clear();
	run(0.0f, 10);
	Require(field.ReadMeta()[0] == 0u && body() == 0.0f, "A floor of 0 leaves the texel's rate and snow flag alone");
	field.Clear();
	std::puts("PASS corpse burial in the field: hollow, refill, mound, mound outlasts its stamps, untouched snow "
	          "keeps the mound, heat on bare ground unchanged");
}

int main(int argc, char** argv)
{
	try {
		if (argc == 2 && (std::string_view(argv[1]) == "--benchmark" ||
			std::string_view(argv[1]) == "--benchmark-dense")) {
			g_denseDispatch = std::string_view(argv[1]) == "--benchmark-dense";
			Benchmark(); return 0;
		}
		std::filesystem::path textures = "data/Data/Textures/NMN_DeformableTerrain";
		for (int i = 1; i < argc; ++i) {
			if (std::string_view(argv[i]) == "--cpu-only") { g_cpuOnly = true; }
			else if (std::string_view(argv[i]) == "--hardware") { g_driver = D3D_DRIVER_TYPE_HARDWARE; }
			else if (std::string_view(argv[i]) == "--draw-hooks") { g_drawHooks = true; }
			else if (std::string_view(argv[i]) == "--textures" && i + 1 < argc) { textures = argv[++i]; }
			else { throw std::runtime_error("Usage: StampSurfaceTest [--cpu-only] [--hardware] [--draw-hooks] [--textures directory] | --benchmark"); }
		}
		std::puts(g_cpuOnly ? "CPU rules only (no device)" :
			g_driver == D3D_DRIVER_TYPE_HARDWARE ? "Device: hardware (no WARP fallback)" : "Device: WARP");
		float at30 = 255.0f, at144 = 255.0f;
		for (int i = 0; i < 30; ++i) { ShelterTransition::Advance(at30, 0, ShelterTransition::Alpha(1.0f / 30)); }
		for (int i = 0; i < 144; ++i) { ShelterTransition::Advance(at144, 0, ShelterTransition::Alpha(1.0f / 144)); }
		Require(std::abs(at30 - at144) < 0.001f, "Shelter transition must be frame-rate independent");
		Require(at30 > 0 && at30 < 20, "Shelter must approach the target without snapping");
		const float shelterBefore = at30;
		ShelterTransition::Advance(at30, 255, ShelterTransition::Alpha(0));
		Require(at30 == shelterBefore, "Paused shelter must not move");
		ShelterTransition::Advance(at30, 255, ShelterTransition::Alpha(1.0f / 60));
		Require(at30 > shelterBefore && at30 < 255, "Reversing a ray answer must not snap or overshoot");
		Require(ShelterTransition::Alpha(std::numeric_limits<float>::quiet_NaN()) == 0, "Invalid delta must be ignored");
		std::puts("PASS shelter transitions: frame rate, pause, reversal and invalid time");
		CheckFootGeometry();
		CheckFootprintProfiles();
		CheckContacts();
		CheckLandings();
		CheckHiddenMarks();
		CheckGroundDecalFit();
		CheckSnowMeshCover();
		CheckObjectSnow();
		CheckFrameCamera();
		CheckIndoorPrints();
		CheckCorpseRule();
		CheckMalformedDds();
		CheckDisplacementBound();
		if (g_cpuOnly) {
			std::puts("All CPU rule checks passed");
			return 0;
		}
		Field field;
		field.CheckTessellationResources();
		field.CheckBlanketBudget();
		field.CheckPatchCulling();
		CheckDisplacementBoundGpu(field);
		CheckImpactPatterns(field);
		CheckGroundFloor(field);
		CheckQuietTiles();
		CheckSparseDispatch();
		CheckMovingWindow(field);
		CheckRecovery(field);
		CheckCorpseBurial(field);
		CheckRepose(field);
		CheckCoarseSeeding(field);
		CheckHeatAndWeather(field);
		CheckScorch(field);
		CheckStampCapacity(field);
		CheckSweptAndOrientedStamps(field);
		CheckObjectFootprint(field);
		CheckFootprintTexture(field, textures);
		CheckCreatureFootprintMasks(field, textures);
		field.Clear();
		Params p;
		field.Step(p);
		const auto ordinary = field.Read();
		field.Clear(); p.stampMotion[0][2] = 1; field.Step(p);
		Require(field.Read() == ordinary, "Inherited snow values must match global geometry");
		Require(field.SnowAtOrigin(), "Snow classification must be written");
		std::puts("PASS inherited snow settings preserve global geometry");
		p.control[3] = 0; p.weather[3] = 0; p.rimShape[1] = 0;
		p.snowRim[0] = 1;
		field.Clear(); p.stampMotion[0][2] = 0; field.Step(p);
		const auto noRim = field.Read();
		Require(*std::max_element(noRim.begin(), noRim.end()) == 0, "Global zero span must suppress other-ground rim");
		field.Clear(); p.stampMotion[0][2] = 1; field.Step(p);
		const auto snowRim = field.Read();
		Require(*std::max_element(snowRim.begin(), snowRim.end()) > 0.1f, "Snow span must work with global span zero");
		std::puts("PASS independent snow rim span, including global span zero");
		field.Clear(); p.stampMotion[0][2] = 0; field.Step(p);
		p.snowRim[1] = 1; p.snowRim[2] = 1; p.snowRim[3] = 1;
		field.Clear(); field.Step(p);
		Require(field.Read() == noRim, "Snow noise/lean/churn leaked into non-snow stamp");
		std::puts("PASS snow noise/lean/churn do not alter other ground");
		for (int parameter = 1; parameter <= 3; ++parameter) {
			p = Params{};
			field.Clear(); field.Step(p);
			const auto baseline = field.Read();
			p.snowRim[parameter] = parameter == 2 ? 1.0f : 0.0f;
			field.Clear(); field.Step(p);
			Require(field.Read() == baseline, "Snow override changed a non-snow rim");
			p.stampMotion[0][2] = 1;
			field.Clear(); field.Step(p);
			Require(field.Read() != baseline, "Snow override has no effect on snow");
		}
		std::puts("PASS each noise/lean/churn override changes only snow with both rims active");
		p = Params{}; p.stampMotion[0][2] = 1;
		p.snowRim[0] = 1; p.snowRim[1] = 0; p.snowRim[2] = 1;
		field.Clear(); field.Step(p);
		Require(field.Read()[12] > 0.1f, "Outward lean rim was clipped by early bounds");
		std::puts("PASS wide outward rims survive the stamp bounds check");
		p = Params{}; p.stampMotion[0][2] = 1;
		field.Clear(); field.Step(p);
		const auto before = field.Read();
		p.control[1] = 0; p.weather[1] = 0.1f; p.repose[0] = 0; p.repose[1] = 1;
		field.Step(p);
		Require(field.Read() != before && field.SnowAtOrigin(),
			"Snow must keep its own repose rate after the stamp leaves");
		p = Params{}; field.Clear(); field.Step(p);
		const auto nonSnow = field.Read();
		p.control[1] = 0; p.weather[1] = 0.1f; p.repose[0] = 0; p.repose[1] = 1;
		field.Step(p);
		Require(field.Read() == nonSnow, "Snow repose rate affected old non-snow marks");
		std::puts("PASS persistent snow/non-snow repose isolation");
		p = Params{}; p.stampMotion[0][2] = 1;
		field.Clear(); field.Step(p); field.SaveCoarse(); field.Clear();
		p.control[1] = 0; p.coarse[0] = 1; p.coarse[3] = 0;
		field.Step(p, true);
		Require(field.SnowAtOrigin() && field.Read()[0] < 0,
			"Coarse seeding lost mark or snow classification");
		std::puts("PASS coarse-to-fine seeding retains snow classification");
		std::puts("ALL STAMP SURFACE TESTS PASS");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
