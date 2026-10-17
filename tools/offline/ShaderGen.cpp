// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.
//
// Offline shader checks. Every hull/domain variant the plugin can generate is built
// against the real Fallout 4 landscape signatures and compiled, the domain output is
// checked to link with the game's landscape pixel shader, and the compute passes are
// compiled. None of it needs the game.
//
// The signatures below are Fallout4.exe 1.10.163's own, read from ShadersFX\Shaders011.fxp:
// BSDFPrePassShader vertex technique 0x3B (and 0x0200003B, identical outputs) and pixel
// technique 0x23. docs/FO4_PORT_NOTES.md records how they were extracted.

#include "OfflinePCH.h"

#include "CameraProjection.h"
#include "ClipmapUpdateCS.h"
#include "ObjectSnowShader.h"
#include "IndoorPrintShader.h"
#include "ShelterTransition.h"
#include "StampShapeAnalysisCS.h"
#include "ShaderReflection.cpp"
#include "Tessellation.cpp"
#include "UtilityRouting.h"
#include "VRBatchRouting.h"
#include "WinterLookCS.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <thread>

namespace
{
	// The blanket cap is eroded before it is smoothed: a static's low cap must survive the
	// box filter over its own texels instead of being averaged up by the open ground.
	bool CheckCapErosion()
	{
		constexpr uint32_t n = 8;
		const auto at = [](uint32_t x, uint32_t y) { return static_cast<size_t>(y) * n + x; };
		const MapFilter::CellRect all{ 0, 0, n - 1, n - 1 };
		std::vector<uint8_t> cap(n * n, 255), low(n * n, 255), smooth(n * n, 255), plain(n * n, 255);
		std::vector<uint32_t> scratch;
		for (uint32_t y = 3; y <= 4; ++y) {
			for (uint32_t x = 3; x <= 4; ++x) {
				cap[at(x, y)] = 40;
			}
		}
		cap[at(0, 0)] = 0;  // wraps onto the opposite edges

		MapFilter::MinRect(cap, low, n, 1, all);
		MapFilter::BoxRect(low, smooth, n, 1, all, scratch);
		MapFilter::BoxRect(cap, plain, n, 1, all, scratch);

		bool ok = low[at(2, 2)] == 40 && low[at(5, 5)] == 40 && low[at(6, 6)] == 255 && low[at(1, 1)] == 0;
		ok &= low[at(7, 7)] == 0 && low[at(7, 0)] == 0 && low[at(0, 7)] == 0;
		for (uint32_t y = 3; y <= 4; ++y) {
			for (uint32_t x = 3; x <= 4; ++x) {
				ok &= smooth[at(x, y)] == 40;
			}
		}
		ok &= plain[at(3, 3)] > 100;  // what the unchanged box filter alone made of it
		ok &= smooth[at(5, 6)] > 40 && smooth[at(5, 6)] < 255;  // ramps up outside the patch
		return ok;
	}

	bool CheckVRBatchRouting()
	{
		int values[7]{};
		std::array<void*, 7> packets{ &values[0], &values[1], nullptr, &values[3], &values[4], &values[5], &values[6] };
		const auto original = packets;
		const std::array<std::uint32_t, 4> ends{ 2, 4, 6, 7 };
		bool ok = true;
		{
			VRBatchRouting::ScopedFallback route;
			unsigned visited = 0;
			ok &= route.Select(ends, packets, [&](std::uint32_t i) { visited |= 1u << i; return i == 1 || i == 5; });
			ok &= route.Count() == 2 && !packets[0] && !packets[2] && !packets[4] && packets[6] == original[6];
			ok &= packets[1] == original[1] && packets[3] == original[3] && packets[5] == original[5];
			ok &= !(visited & ((1u << 2) | (1u << 3))); // Native fallback group was left alone.
		}
		ok &= packets == original;
		for (const auto invalid : { std::array<std::uint32_t, 2>{2, 1}, {2, 8}, {2, 2}, {0, 2} }) {
			VRBatchRouting::ScopedFallback route;
			bool visited = false;
			ok &= !route.Select(invalid, packets, [&](std::uint32_t) { visited = true; return true; });
			ok &= !visited && packets == original && route.Count() == 0;
		}
		try {
			VRBatchRouting::ScopedFallback route;
			route.Select(ends, packets, [](std::uint32_t i) {
				if (i == 4) throw std::runtime_error("fixture");
				return true;
			});
			ok = false;
		} catch (const std::runtime_error&) {}
		ok &= packets == original; // Also restored when a later group fails.
		{
			VRBatchRouting::ScopedFallback route;
			ok &= route.Select(ends, packets, [](std::uint32_t) { return false; });
			ok &= packets == original && route.Count() == 0;
		}
		return ok;
	}

	Reflection::SignatureElement Element(const char* a_semantic, uint32_t a_index, uint32_t a_reg,
		uint8_t a_mask, D3D_REGISTER_COMPONENT_TYPE a_type = D3D_REGISTER_COMPONENT_FLOAT32)
	{
		Reflection::SignatureElement e{};
		e.semanticName = a_semantic;
		e.semanticIndex = a_index;
		e.registerIndex = a_reg;
		e.mask = a_mask;
		e.componentType = static_cast<uint8_t>(a_type);

		uint32_t count = 0;
		for (uint32_t i = 0; i < 4; ++i) {
			count += (a_mask >> i) & 1u;
		}
		const char* scalar = a_type == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : "float";
		e.hlslType = count == 1 ? std::string(scalar) : std::format("{}{}", scalar, count);
		return e;
	}

	// BSDFPrePassShader VS 0x3B output = PS 0x23 input (bar SV_IsFrontFace).
	Reflection::Signature LandscapeSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, 0xF),
			Element("TEXCOORD", 0, 1, 0x7),
			Element("TEXCOORD", 1, 2, 0x7),
			Element("TEXCOORD", 2, 3, 0x7),
			Element("TEXCOORD", 3, 4, 0xF),
			Element("TEXCOORD", 4, 5, 0xF),
			Element("COLOR", 0, 6, 0xF),
			Element("TEXCOORD", 6, 7, 0x3),
			Element("TEXCOORD", 7, 8, 0xF),
			Element("TEXCOORD", 8, 9, 0x7),
		};
		return s;
	}

	// What PS 0x23 reads: the linkage the domain stage must satisfy.
	struct PixelInput
	{
		const char* semantic;
		uint32_t    index;
		uint32_t    reg;
		uint8_t     mask;
	};

	constexpr PixelInput kLandscapePixelInputs[] = {
		{ "SV_POSITION", 0, 0, 0xF },
		{ "TEXCOORD", 0, 1, 0x7 },
		{ "TEXCOORD", 1, 2, 0x7 },
		{ "TEXCOORD", 2, 3, 0x7 },
		{ "TEXCOORD", 3, 4, 0xF },
		{ "TEXCOORD", 4, 5, 0xF },
		{ "COLOR", 0, 6, 0xF },
		{ "TEXCOORD", 6, 7, 0x3 },
		{ "TEXCOORD", 7, 8, 0xF },
		{ "TEXCOORD", 8, 9, 0x7 },
	};

	// Fallout 4 VR's landscape shaders (ShadersFX\Shaders012_VR.fxp in "Fallout4_VR -
	// Shaders.ba2"): VS 0x3B adds the eye index and the clip/cull distance that splits the
	// double-wide target; PS 0x23 reads the eye index.
	Reflection::Signature VRLandscapeSignature()
	{
		Reflection::Signature s = LandscapeSignature();
		s.elements.push_back(Element("EYEINDEX", 0, 10, 0x1, D3D_REGISTER_COMPONENT_UINT32));
		s.elements.push_back(Element("SV_ClipDistance", 0, 11, 0x1));
		s.elements.push_back(Element("SV_CullDistance", 0, 11, 0x2));
		return s;
	}

	constexpr PixelInput kVRLandscapePixelInputs[] = {
		{ "SV_POSITION", 0, 0, 0xF },
		{ "TEXCOORD", 0, 1, 0x7 },
		{ "TEXCOORD", 1, 2, 0x7 },
		{ "TEXCOORD", 2, 3, 0x7 },
		{ "TEXCOORD", 3, 4, 0xF },
		{ "TEXCOORD", 4, 5, 0xF },
		{ "COLOR", 0, 6, 0xF },
		{ "TEXCOORD", 6, 7, 0x3 },
		{ "TEXCOORD", 7, 8, 0xF },
		{ "TEXCOORD", 8, 9, 0x7 },
		{ "EYEINDEX", 0, 10, 0x1 },
	};

	// A signature the landscape layout does not recognise: only the clip-space lift runs.
	Reflection::Signature MinimalSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, 0xF),
			Element("TEXCOORD", 0, 1, 0x3),
		};
		return s;
	}

	int g_failures = 0;

	void Fail(const std::string& a_what)
	{
		++g_failures;
		std::printf("  FAIL  %s\n", a_what.c_str());
	}

	ID3DBlob* Compile(const std::string& a_source, const char* a_target, const std::string& a_label,
		const std::filesystem::path& a_outDir, const D3D_SHADER_MACRO* a_defines = nullptr,
		UINT a_flags = D3DCOMPILE_OPTIMIZATION_LEVEL3, const char* a_entry = "main")
	{
		const auto path = a_outDir / (a_label + ".hlsl");
		std::ofstream(path) << a_source;

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(a_source.c_str(), a_source.size(), a_label.c_str(),
			a_defines, nullptr, a_entry, a_target, a_flags, 0, &code, &errors);

		if (SUCCEEDED(hr)) {
			std::printf("  PASS  %-28s %-7s %6zu bytes -> %s\n", a_label.c_str(), a_target,
				code->GetBufferSize(), path.string().c_str());
		} else {
			++g_failures;
			std::printf("  FAIL  %-28s %-7s hr=0x%08X -> %s\n", a_label.c_str(), a_target,
				static_cast<uint32_t>(hr), path.string().c_str());
			if (errors) {
				std::printf("%.*s\n", static_cast<int>(errors->GetBufferSize()),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
		}

		if (errors) {
			errors->Release();
		}
		return SUCCEEDED(hr) ? code : nullptr;
	}

	// The domain stage feeds the game's pixel shader directly, so every element the pixel
	// shader reads has to leave the domain stage on the same register with at least the
	// same components.
	void CheckLinkage(ID3DBlob* a_domain, const std::string& a_label,
		std::span<const PixelInput> a_pixelInputs)
	{
		ID3D11ShaderReflection* reflection = nullptr;
		if (FAILED(D3DReflect(a_domain->GetBufferPointer(), a_domain->GetBufferSize(),
				IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&reflection)))) {
			Fail(a_label + ": D3DReflect failed");
			return;
		}

		D3D11_SHADER_DESC desc{};
		reflection->GetDesc(&desc);

		bool ok = true;
		for (const auto& input : a_pixelInputs) {
			bool found = false;
			for (UINT i = 0; i < desc.OutputParameters; ++i) {
				D3D11_SIGNATURE_PARAMETER_DESC out{};
				reflection->GetOutputParameterDesc(i, &out);
				if (_stricmp(out.SemanticName, input.semantic) != 0 || out.SemanticIndex != input.index) {
					continue;
				}
				found = true;
				if (out.Register != input.reg || (out.Mask & input.mask) != input.mask) {
					ok = false;
					std::printf("        %s%u: domain writes register %u mask %X, pixel shader reads "
								"register %u mask %X\n",
						input.semantic, input.index, out.Register, out.Mask, input.reg, input.mask);
				}
			}
			if (!found) {
				ok = false;
				std::printf("        %s%u: not written by the domain stage\n", input.semantic,
					input.index);
			}
		}
		reflection->Release();

		if (ok) {
			std::printf("  PASS  %-28s links to PS 0x23\n", a_label.c_str());
		} else {
			Fail(a_label + ": domain output does not link to the landscape pixel shader");
		}
	}

	// The hull stage reads the game's vertex shader output directly, so its input has to
	// sit on the same registers and components (the clip and cull distance share one).
	void CheckHullInput(ID3DBlob* a_hull, const Reflection::Signature& a_signature,
		const std::string& a_label)
	{
		ID3D11ShaderReflection* reflection = nullptr;
		if (FAILED(D3DReflect(a_hull->GetBufferPointer(), a_hull->GetBufferSize(),
				IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&reflection)))) {
			Fail(a_label + ": D3DReflect failed");
			return;
		}

		D3D11_SHADER_DESC desc{};
		reflection->GetDesc(&desc);

		// Every element but VR's stereo split has to be read, each where the vertex shader
		// put it; the split distances are the only ones the hull stage may leave out.
		const auto layout = Tessellation::LayoutFor(a_signature);
		size_t     expected = 0;
		for (size_t i = 0; i < a_signature.elements.size(); ++i) {
			expected += Tessellation::InControlPoint(layout, i) ? 1 : 0;
		}

		bool ok = desc.InputParameters == expected;
		if (!ok) {
			std::printf("        hull reads %u elements, expected %zu of the vertex shader's %zu\n",
				desc.InputParameters, expected, a_signature.elements.size());
		}
		for (UINT i = 0; ok && i < desc.InputParameters; ++i) {
			D3D11_SIGNATURE_PARAMETER_DESC in{};
			reflection->GetInputParameterDesc(i, &in);
			const auto match = std::ranges::find_if(a_signature.elements, [&](const auto& e) {
				return _stricmp(in.SemanticName, e.semanticName.c_str()) == 0 && in.SemanticIndex == e.semanticIndex;
			});
			if (match == a_signature.elements.end() || in.Register != match->registerIndex ||
				in.Mask != match->mask) {
				ok = false;
				std::printf("        hull %s%u on register %u mask %X does not match the vertex shader\n",
					in.SemanticName, in.SemanticIndex, in.Register, in.Mask);
			}
		}
		reflection->Release();

		if (ok) {
			std::printf("  PASS  %-28s reads the vertex shader's output as written\n", a_label.c_str());
		} else {
			Fail(a_label + ": hull input does not match the landscape vertex shader's output");
		}
	}

	// The plugin checks every compiled pair against the vertex shader before using it
	// (Tessellation.cpp, CreatePair); the real landscape layouts must pass that check, or
	// the terrain would silently stay vanilla.
	void CheckRuntimeLinkage(ID3DBlob* a_hull, ID3DBlob* a_domain,
		const Reflection::Signature& a_signature, const std::string& a_label)
	{
		std::string why;
		const auto  hullInput = Reflection::ReflectInputSignature(a_hull->GetBufferPointer(), a_hull->GetBufferSize());
		const auto  domainOutput = Reflection::ReflectOutputSignature(a_domain->GetBufferPointer(), a_domain->GetBufferSize());
		if (!Reflection::Covers(a_signature, hullInput, &why)) {
			Fail(a_label + ": the plugin's link check refuses the hull input (" + why + ")");
		} else if (!Reflection::Covers(domainOutput, a_signature, &why)) {
			Fail(a_label + ": the plugin's link check refuses the domain output (" + why + ")");
		} else {
			std::printf("  PASS  %-28s passes the plugin's own link check\n", a_label.c_str());
		}
	}

	// VR's rasteriser splits the double-wide target by the clip and cull distances; the
	// domain stage has to write both afresh.
	void CheckStereoSplit(ID3DBlob* a_domain, const std::string& a_label)
	{
		const auto out = Reflection::ReflectOutputSignature(a_domain->GetBufferPointer(), a_domain->GetBufferSize());
		const auto has = [&](const char* a_semantic) {
			return std::ranges::any_of(out.elements, [&](const Reflection::SignatureElement& a_element) {
				return _stricmp(a_element.semanticName.c_str(), a_semantic) == 0;
			});
		};
		if (has("SV_ClipDistance") && has("SV_CullDistance")) {
			std::printf("  PASS  %-28s writes the stereo clip and cull distances\n", a_label.c_str());
		} else {
			Fail(a_label + ": the domain stage does not write the stereo clip/cull distances");
		}
	}

	void Generate(const char* a_name, const Reflection::Signature& a_signature,
		const std::filesystem::path& a_outDir,
		std::span<const PixelInput> a_pixelInputs = kLandscapePixelInputs)
	{
		const auto mode = Tessellation::Mode::kLandscape;

		auto* hull = Compile(Tessellation::HullSource(a_signature, mode), "hs_5_0",
			std::string(a_name) + "_hs", a_outDir);
		if (hull && !a_pixelInputs.empty()) {
			CheckHullInput(hull, a_signature, std::string(a_name) + "_hs");
		}

		auto* domain = Compile(Tessellation::DomainSource(a_signature, mode), "ds_5_0",
			std::string(a_name) + "_ds", a_outDir);
		if (domain && !a_pixelInputs.empty()) {
			CheckLinkage(domain, std::string(a_name) + "_ds", a_pixelInputs);
			if (Tessellation::LayoutFor(a_signature).Stereo()) {
				CheckStereoSplit(domain, std::string(a_name) + "_ds");
			}
		}

		if (hull && domain && !a_pixelInputs.empty()) {
			CheckRuntimeLinkage(hull, domain, a_signature, a_name);
		}
		if (hull) {
			hull->Release();
		}
		if (domain) {
			domain->Release();
		}
	}

	// The plugin keeps only each vertex shader's output signature chunk and parses it
	// itself; that has to read exactly what D3DReflect reads from the whole shader.
	void CheckSignatureChunk(const std::filesystem::path& a_outDir)
	{
		const std::string source =
			"struct VSOut {\n"
			"  float4 position : SV_Position;\n"
			"  float2 a : TEXCOORD0;\n"
			"  float2 b : TEXCOORD1;\n"
			"  float3 c : TEXCOORD5;\n"
			"  nointerpolation uint eye : EYEINDEX0;\n"
			"  float clip : SV_ClipDistance0;\n"
			"  float cull : SV_CullDistance0;\n"
			"};\n"
			"VSOut main(float4 p : POSITION, uint id : SV_InstanceID) {\n"
			"  VSOut o; o.position = p; o.a = p.xy; o.b = p.zw; o.c = p.xyz; o.eye = id & 1u;\n"
			"  o.clip = p.x; o.cull = p.y; return o;\n"
			"}\n";
		auto* code = Compile(source, "vs_5_0", "signature_chunk_vs", a_outDir);
		if (!code) {
			return;
		}

		const auto reflected = Reflection::ReflectOutputSignature(code->GetBufferPointer(), code->GetBufferSize());
		const auto chunk = Reflection::OutputSignatureChunk(code->GetBufferPointer(), code->GetBufferSize());
		const auto parsed = Reflection::ParseSignatureChunk(chunk.data(), chunk.size());
		code->Release();

		bool same = reflected.valid && parsed.valid && reflected.elements.size() == parsed.elements.size();
		for (size_t i = 0; same && i < parsed.elements.size(); ++i) {
			const auto& r = reflected.elements[i];
			const auto& q = parsed.elements[i];
			same = r.semanticName == q.semanticName && r.semanticIndex == q.semanticIndex &&
			       r.registerIndex == q.registerIndex && r.mask == q.mask &&
			       r.componentType == q.componentType && r.hlslType == q.hlslType;
		}
		if (!same) {
			Fail("the output signature chunk parses differently from D3DReflect");
		} else {
			std::printf("  PASS  output signature chunk (%zu bytes) parses as D3DReflect reads it\n", chunk.size());
		}

		std::string why;
		auto        moved = parsed;
		moved.elements[1].registerIndex += 1;
		if (Reflection::Covers(parsed, parsed) && !Reflection::Covers(parsed, moved, &why) && !why.empty()) {
			std::puts("  PASS  link check: matching layouts pass, a moved register is refused");
		} else {
			Fail("the link check does not tell a moved register from a matching one");
		}
	}

	void GenerateCompute(const std::filesystem::path& a_outDir)
	{
		const std::string      maxStamps = std::to_string(Clipmap::kMaxStamps);
		const D3D_SHADER_MACRO defines[] = { { "MAX_STAMPS", maxStamps.c_str() }, { nullptr, nullptr } };

		if (auto* code = Compile(Clipmap::UpdateShaderSource(), "cs_5_0", "clipmap_update", a_outDir,
				defines)) {
			code->Release();
		}
		const D3D_SHADER_MACRO sparseDefines[] = { { "MAX_STAMPS", maxStamps.c_str() },
			{ "SPARSE_DISPATCH", "1" }, {} };
		if (auto* code = Compile(Clipmap::UpdateShaderSource(), "cs_5_0", "clipmap_sparse_update", a_outDir,
				sparseDefines)) {
			code->Release();
		}
		if (auto* code = Compile(Clipmap::UpdateShaderSource(), "cs_5_0", "clipmap_select_pairs", a_outDir,
				sparseDefines, D3DCOMPILE_OPTIMIZATION_LEVEL3, "selectPairs")) {
			code->Release();
		}
		if (auto* code = Compile(StampShapes::kAnalysisShader, "cs_5_0", "stamp_shape_analysis",
				a_outDir, nullptr, D3DCOMPILE_OPTIMIZATION_LEVEL0)) {
			code->Release();
		}
		// The winter look: one landscape texture's mean colour and share of white.
		if (auto* code = Compile(WinterLook::kMeasureShader, "cs_5_0", "winter_look", a_outDir, nullptr,
				D3DCOMPILE_OPTIMIZATION_LEVEL3)) {
			code->Release();
		}
		// The snow-on-objects G-buffer pass: a full-screen triangle and its pixel shader.
		if (auto* code = Compile(ObjectSnow::kShaderSource, "vs_5_0", "object_snow_vs", a_outDir, nullptr,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, "vsMain")) {
			code->Release();
		}
		if (auto* code = Compile(ObjectSnow::kShaderSource, "ps_5_0", "object_snow_ps", a_outDir, nullptr,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, "psMain")) {
			code->Release();
		}
		// The floor-print G-buffer pass: instanced print boxes and their pixel shader.
		if (auto* code = Compile(IndoorPrints::kShaderSource, "vs_5_0", "floor_prints_vs", a_outDir, nullptr,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, "vsMain")) {
			code->Release();
		}
		if (auto* code = Compile(IndoorPrints::kShaderSource, "ps_5_0", "floor_prints_ps", a_outDir, nullptr,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, "psMain")) {
			code->Release();
		}
	}

	void CheckLayout()
	{
		const auto layout = Tessellation::LayoutFor(LandscapeSignature());
		if (!layout.Landscape() || layout.clip != 0 || layout.tbn[0] != 1 || layout.tbn[1] != 2 ||
			layout.tbn[2] != 3 || layout.world != 4 || layout.prevWorld != 5 ||
			layout.weights != 8 || layout.colour != 6) {
			Fail("the landscape layout was not recognised from the VS 0x3B signature");
		} else {
			std::puts("  PASS  landscape layout: TBN t0-t2, world t3, previous world t4, "
					  "weights t7, colour c0");
		}

		if (Tessellation::LayoutFor(MinimalSignature()).Landscape()) {
			Fail("a two-element signature was taken for the landscape");
		} else {
			std::puts("  PASS  a foreign signature is not taken for the landscape (its draws stay vanilla)");
		}
	}

	void CheckAsyncCompiler()
	{
		const auto signature = LandscapeSignature();
		const auto mode = Tessellation::Mode::kLandscape;

		ShaderCompiler::Worker worker;
		auto                   job = std::make_shared<ShaderCompiler::Job>();
		job->hullSource = Tessellation::HullSource(signature, mode);
		job->domainSource = Tessellation::DomainSource(signature, mode);
		worker.Submit(job);

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!job->ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!job->ready.load(std::memory_order_acquire) || !job->hull || !job->domain || !job->error.empty()) {
			Fail("asynchronous compiler publication");
		} else {
			std::printf("  PASS  asynchronous compiler publication (%.2f ms off the caller thread)\n",
				job->milliseconds);
		}

		auto invalid = std::make_shared<ShaderCompiler::Job>();
		invalid->hullSource = "invalid shader";
		worker.Submit(invalid);
		const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!invalid->ready.load(std::memory_order_acquire) &&
			   std::chrono::steady_clock::now() < failureDeadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!invalid->ready.load(std::memory_order_acquire) || invalid->error.empty()) {
			Fail("asynchronous compile failure reporting");
		} else {
			std::puts("  PASS  asynchronous compile failure reporting");
		}
		worker.CancelQueued();
	}

	void CheckCameraTest()
	{
		const float orthographic[16] = {
			0.0004f, 0.0001f, 0.0000f, 0.0000f,
			0.0000f, 0.0003f, -0.0000f, 0.0000f,
			0.0001f, -0.0002f, -0.0000f, 0.0000f,
			-1.0000f, 1.0000f, -0.0001f, 1.0000f
		};
		const float perspective[16] = {
			1.2071f, 0.0000f, 0.0000f, 0.0000f,
			0.0000f, 0.0000f, 1.0000f, 1.0000f,
			0.0000f, 2.1445f, 0.0000f, 0.0000f,
			0.0000f, 0.0000f, 14.7349f, 0.0000f
		};

		bool ok = true;
		if (UtilityRouting::IsPerspectiveProjection(orthographic)) {
			Fail("an orthographic (shadow or map) pass read as the player's camera");
			ok = false;
		}
		if (!UtilityRouting::IsPerspectiveProjection(perspective)) {
			Fail("a perspective camera was refused");
			ok = false;
		}
		if (!UtilityRouting::IsPerspectiveProjection(nullptr)) {
			Fail("a missing camera must not refuse the draw");
			ok = false;
		}
		if (ok) {
			std::puts("  PASS  player-camera test: perspective yes, orthographic no, unknown yes");
		}

		// Exercise the production reader with two different thread contexts and a
		// default context. The global State camera is deliberately irrelevant.
		std::array<std::byte, CameraProjection::kContextProjection + sizeof(perspective)> world{}, map{};
		std::array<std::byte, CameraProjection::kThreadContext + sizeof(std::uintptr_t)> worldTLS{}, mapTLS{};
		const auto worldContext = reinterpret_cast<std::uintptr_t>(world.data());
		const auto mapContext = reinterpret_cast<std::uintptr_t>(map.data());
		std::memcpy(world.data() + CameraProjection::kContextProjection, perspective, sizeof(perspective));
		std::memcpy(map.data() + CameraProjection::kContextProjection, orthographic, sizeof(orthographic));
		std::memcpy(worldTLS.data() + CameraProjection::kThreadContext, &worldContext, sizeof(worldContext));
		std::memcpy(mapTLS.data() + CameraProjection::kThreadContext, &mapContext, sizeof(mapContext));
		const std::uintptr_t vector[]{ 0, reinterpret_cast<std::uintptr_t>(worldTLS.data()), reinterpret_cast<std::uintptr_t>(mapTLS.data()) };
		float projection[16]{};
		const auto read = [&](std::uint32_t index, std::uintptr_t fallback, const float* expected) {
			return CameraProjection::Flat(reinterpret_cast<std::uintptr_t>(vector), index, fallback, projection) == CameraProjection::Read::kRead &&
				std::memcmp(projection, expected, sizeof(projection)) == 0;
		};
		if (!read(1, reinterpret_cast<std::uintptr_t>(&mapContext), perspective) ||
			!read(2, reinterpret_cast<std::uintptr_t>(&worldContext), orthographic) ||
			!read(0, reinterpret_cast<std::uintptr_t>(&worldContext), perspective) ||
			CameraProjection::Flat(0, 0, 0, projection) != CameraProjection::Read::kEmpty ||
			CameraProjection::Flat(1, 0, 0, projection) != CameraProjection::Read::kFault ||
			CameraProjection::Flat(0, 0, 1, projection) != CameraProjection::Read::kFault) {
			Fail("flat draw-camera reader: thread selection, fallback, empty or invalid storage");
		} else {
			std::puts("  PASS  flat draw-camera reader: independent world/map contexts, default, empty and faults");
		}
	}
}

int main(int a_argc, char** a_argv)
{
	const std::filesystem::path outDir = a_argc > 1 ? a_argv[1] : "generated_shaders";
	std::filesystem::create_directories(outDir);

	Settings::enableTessellation = true;
	Settings::useClipmap = true;
	Settings::recomputeNormals = true;
	Settings::enableSurfaceMaterial = true;
	Settings::enableSnowRaise = false;

	std::printf("Signature layout\n");
	CheckLayout();

	std::printf("\nCompute passes\n");
	GenerateCompute(outDir);

	std::printf("\nDefault configuration (clipmap, normals, surface material)\n");
	Generate("landscape", LandscapeSignature(), outDir);

	std::printf("\nSnow raise\n");
	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 35.0f;
	Generate("raise", LandscapeSignature(), outDir);
	Settings::recomputeNormals = false;
	Generate("raise_nonormals", LandscapeSignature(), outDir);
	Settings::recomputeNormals = true;
	Settings::enableTessellationBounds = false;
	Generate("blanket_bounds_off", LandscapeSignature(), outDir);
	Settings::enableTessellationBounds = true;
	Settings::tessellationBlanketSpacing = 0.0f;
	Generate("blanket_floor_off", LandscapeSignature(), outDir);
	Settings::tessellationBlanketSpacing = 8.0f;
	Settings::shelterMeshCap = false;
	Generate("raise_nomeshcap", LandscapeSignature(), outDir);
	Settings::shelterMeshCap = true;
	Settings::snowRaiseHeight = 0.0f;
	Generate("raise_zero", LandscapeSignature(), outDir);
	Settings::enableSnowRaise = false;
	Settings::snowRaiseHeight = 35.0f;

	std::printf("\nPatch culling off\n");
	Settings::enablePatchCulling = false;
	Generate("noculling", LandscapeSignature(), outDir);
	Settings::enablePatchCulling = true;

	std::printf("\nOne clipmap level\n");
	Settings::clipmapLevels = 1;
	Generate("one_level", LandscapeSignature(), outDir);
	Settings::clipmapLevels = 2;

	std::printf("\nSurface material off\n");
	Settings::enableSurfaceMaterial = false;
	Generate("nomaterial", LandscapeSignature(), outDir);
	Settings::enableSurfaceMaterial = true;

	std::printf("\nRecomputed normals off\n");
	Settings::recomputeNormals = false;
	Generate("nonormals", LandscapeSignature(), outDir);
	Settings::recomputeNormals = true;

	std::printf("\nAnalytic wave instead of the clipmap\n");
	Settings::useClipmap = false;
	Settings::debugWaveAmplitude = 8.0f;
	Generate("wave", LandscapeSignature(), outDir);
	Settings::useClipmap = true;
	Settings::debugWaveAmplitude = 0.0f;

	std::printf("\nField debug colours and the blend-layer view\n");
	for (int mode = 1; mode <= 3; ++mode) {
		Settings::debugFieldColour = mode;
		Generate(std::format("fielddebug_{}", mode).c_str(), LandscapeSignature(), outDir);
	}
	Settings::debugFieldColour = 0;
	Settings::debugBlendLayer = 2;
	Generate("blendlayer_2", LandscapeSignature(), outDir);
	Settings::debugBlendLayer = -1;

	std::printf("\nCounter-clockwise winding\n");
	Settings::tessellationWinding = "ccw";
	Generate("ccw", LandscapeSignature(), outDir);
	Settings::tessellationWinding = "cw";

	std::printf("\nA foreign signature (the generator's clip-space lift; the plugin leaves such draws vanilla)\n");
	Generate("minimal", MinimalSignature(), outDir, {});

	std::printf("\nFallout 4 VR (single-pass stereo, Shaders012_VR.fxp VS 0x3B -> PS 0x23)\n");
	{
		const auto layout = Tessellation::LayoutFor(VRLandscapeSignature());
		if (!layout.Landscape() || !layout.Stereo() || layout.eye != 10 || layout.clipDistance != 11 ||
			layout.cullDistance != 12) {
			Fail("the VR landscape layout (eye index, clip and cull distance) was not recognised");
		} else {
			std::puts("  PASS  VR layout: eye index f10, clip distance f11, cull distance f12");
		}
	}
	Generate("vr_landscape", VRLandscapeSignature(), outDir, kVRLandscapePixelInputs);
	Settings::enableSnowRaise = true;
	Generate("vr_raise", VRLandscapeSignature(), outDir, kVRLandscapePixelInputs);
	Settings::enableSnowRaise = false;
	Settings::enableSurfaceMaterial = false;
	Settings::recomputeNormals = false;
	Generate("vr_plain", VRLandscapeSignature(), outDir, kVRLandscapePixelInputs);
	Settings::enableSurfaceMaterial = true;
	Settings::recomputeNormals = true;
	Settings::enablePatchCulling = false;
	Generate("vr_noculling", VRLandscapeSignature(), outDir, kVRLandscapePixelInputs);
	Settings::enablePatchCulling = true;

	std::printf("\nRuntime pieces\n");
	CheckSignatureChunk(outDir);
	CheckAsyncCompiler();
	CheckCameraTest();
	if (CheckVRBatchRouting()) {
		std::puts("  PASS  VR batch routing: mixed groups, native fallback, malformed bounds, restoration and exceptions");
	} else {
		Fail("VR batch routing changed an unrelated packet or failed to restore a selected group");
	}
	if (CheckCapErosion()) {
		std::puts("  PASS  blanket cap: eroded before smoothing, wraps, ramps outside the static");
	} else {
		Fail("blanket cap erosion/smoothing left a static's texels above its cap");
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
		g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}

namespace Clipmap
{
	bool Ready()
	{
		return true;
	}

	uint32_t ActiveLevels()
	{
		return LevelCount();
	}
}
