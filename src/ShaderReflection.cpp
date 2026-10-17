// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ShaderReflection.h"

#include <d3d11shader.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace Reflection
{
	namespace
	{

		uint32_t ComponentCount(uint8_t a_mask)
		{
			uint32_t count = 0;
			for (uint32_t i = 0; i < 4; ++i) {
				if (a_mask & (1u << i)) {
					++count;
				}
			}
			return count;
		}

		const char* ScalarName(uint8_t a_componentType)
		{
			switch (static_cast<D3D_REGISTER_COMPONENT_TYPE>(a_componentType)) {
			case D3D_REGISTER_COMPONENT_UINT32:
				return "uint";
			case D3D_REGISTER_COMPONENT_SINT32:
				return "int";
			case D3D_REGISTER_COMPONENT_FLOAT32:
				return "float";
			default:
				return "float";
			}
		}

		std::string MaskString(uint8_t a_mask)
		{
			std::string out;
			const char* components = "xyzw";
			for (uint32_t i = 0; i < 4; ++i) {
				if (a_mask & (1u << i)) {
					out += components[i];
				}
			}
			return out.empty() ? "-" : out;
		}
	}

	std::string SignatureElement::Semantic() const
	{
		return std::format("{}{}", semanticName, semanticIndex);
	}

	namespace
	{
		Signature Reflect(const void* a_bytecode, size_t a_size, bool a_output)
		{
			Signature signature{};

			if (!a_bytecode || a_size == 0) {
				return signature;
			}

			ID3D11ShaderReflection* reflector = nullptr;
			if (FAILED(::D3DReflect(a_bytecode, a_size, IID_ID3D11ShaderReflection,
					reinterpret_cast<void**>(&reflector))) ||
				!reflector) {
				return signature;
			}

			D3D11_SHADER_DESC desc{};
			if (FAILED(reflector->GetDesc(&desc))) {
				reflector->Release();
				return signature;
			}

			const UINT count = a_output ? desc.OutputParameters : desc.InputParameters;
			signature.elements.reserve(count);

			for (uint32_t i = 0; i < count; ++i) {
				D3D11_SIGNATURE_PARAMETER_DESC param{};
				if (FAILED(a_output ? reflector->GetOutputParameterDesc(i, &param) :
				                      reflector->GetInputParameterDesc(i, &param))) {
					continue;
				}

				SignatureElement element{};
				element.semanticName = param.SemanticName ? param.SemanticName : "";
				element.semanticIndex = param.SemanticIndex;
				element.registerIndex = param.Register;
				element.mask = param.Mask;
				element.componentType = static_cast<uint8_t>(param.ComponentType);

				const uint32_t components = ComponentCount(param.Mask);
				const char*    scalar = ScalarName(element.componentType);
				element.hlslType = components > 1 ? std::format("{}{}", scalar, components) : scalar;

				signature.elements.push_back(std::move(element));
			}

			reflector->Release();

			signature.valid = true;
			return signature;
		}
	}

	Signature ReflectOutputSignature(const void* a_bytecode, size_t a_size)
	{
		return Reflect(a_bytecode, a_size, true);
	}

	namespace
	{
		constexpr uint32_t FourCC(char a, char b, char c, char d)
		{
			return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
			       (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
		}

		constexpr uint32_t kDXBC = FourCC('D', 'X', 'B', 'C');
		constexpr uint32_t kOSGN = FourCC('O', 'S', 'G', 'N');
		constexpr uint32_t kOSG5 = FourCC('O', 'S', 'G', '5');
		constexpr uint32_t kOSG1 = FourCC('O', 'S', 'G', '1');

		uint32_t ReadU32(const uint8_t* a_at)
		{
			uint32_t value = 0;
			std::memcpy(&value, a_at, sizeof(value));
			return value;
		}
	}

	// DXBC: "DXBC", a 16-byte digest, a version, the total size and the chunk count (at
	// 0x1C), then that many chunk offsets; each chunk is a fourcc, a size and its data.
	std::vector<uint8_t> OutputSignatureChunk(const void* a_bytecode, size_t a_size)
	{
		const auto* bytes = static_cast<const uint8_t*>(a_bytecode);
		if (!bytes || a_size < 0x20 || ReadU32(bytes) != kDXBC) {
			return {};
		}

		const uint32_t chunks = ReadU32(bytes + 0x1C);
		if (chunks > 64 || 0x20 + static_cast<size_t>(chunks) * 4 > a_size) {
			return {};
		}

		for (uint32_t i = 0; i < chunks; ++i) {
			const size_t offset = ReadU32(bytes + 0x20 + i * 4);
			if (offset + 8 > a_size) {
				return {};
			}
			const uint32_t fourcc = ReadU32(bytes + offset);
			const size_t   size = ReadU32(bytes + offset + 4);
			if (fourcc != kOSGN && fourcc != kOSG5 && fourcc != kOSG1) {
				continue;
			}
			if (offset + 8 + size > a_size) {
				return {};
			}
			return { bytes + offset, bytes + offset + 8 + size };
		}
		return {};
	}

	// The chunk's data: an element count and the offset of the first element, then the
	// elements (24 bytes; OSG5 adds a leading stream index, OSG1 that and a trailing
	// minimum precision), with each name an offset from the start of the data.
	Signature ParseSignatureChunk(const void* a_chunk, size_t a_size)
	{
		Signature signature{};

		const auto* chunk = static_cast<const uint8_t*>(a_chunk);
		if (!chunk || a_size < 16) {
			return signature;
		}

		const uint32_t fourcc = ReadU32(chunk);
		const size_t   dataSize = ReadU32(chunk + 4);
		if (dataSize + 8 > a_size || dataSize < 8) {
			return signature;
		}
		const uint8_t* data = chunk + 8;

		size_t stride = 0;
		size_t lead = 0;
		if (fourcc == kOSGN) {
			stride = 24;
		} else if (fourcc == kOSG5) {
			stride = 28;
			lead = 4;
		} else if (fourcc == kOSG1) {
			stride = 32;
			lead = 4;
		} else {
			return signature;
		}

		const uint32_t count = ReadU32(data);
		const size_t   first = ReadU32(data + 4);
		if (count > 64 || first + static_cast<size_t>(count) * stride > dataSize) {
			return signature;
		}

		signature.elements.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t* element = data + first + i * stride + lead;
			const size_t   name = ReadU32(element);
			if (name >= dataSize) {
				return Signature{};
			}

			const auto* text = reinterpret_cast<const char*>(data + name);
			const auto  length = strnlen(text, dataSize - name);
			if (length == dataSize - name) {
				return Signature{};
			}

			SignatureElement out{};
			out.semanticName.assign(text, length);
			out.semanticIndex = ReadU32(element + 4);
			out.componentType = static_cast<uint8_t>(ReadU32(element + 12));
			out.registerIndex = ReadU32(element + 16);
			out.mask = element[20];

			const uint32_t components = ComponentCount(out.mask);
			const char*    scalar = ScalarName(out.componentType);
			out.hlslType = components > 1 ? std::format("{}{}", scalar, components) : scalar;

			signature.elements.push_back(std::move(out));
		}

		signature.valid = true;
		return signature;
	}

	Signature ReflectInputSignature(const void* a_bytecode, size_t a_size)
	{
		return Reflect(a_bytecode, a_size, false);
	}

	bool Covers(const Signature& a_writer, const Signature& a_reader, std::string* a_why)
	{
		if (!a_writer.valid || !a_reader.valid) {
			if (a_why) {
				*a_why = "reflection failed";
			}
			return false;
		}

		for (const auto& read : a_reader.elements) {
			const auto written = std::ranges::find_if(a_writer.elements, [&](const SignatureElement& a_element) {
				return a_element.semanticIndex == read.semanticIndex &&
				       _stricmp(a_element.semanticName.c_str(), read.semanticName.c_str()) == 0;
			});
			if (written == a_writer.elements.end()) {
				if (a_why) {
					*a_why = std::format("{} is not written", read.Semantic());
				}
				return false;
			}
			if (written->registerIndex != read.registerIndex ||
				(written->mask & read.mask) != read.mask ||
				written->componentType != read.componentType) {
				if (a_why) {
					*a_why = std::format("{} is written to register {} mask {:X} but read from "
										 "register {} mask {:X}",
						read.Semantic(), written->registerIndex, written->mask,
						read.registerIndex, read.mask);
				}
				return false;
			}
		}
		return true;
	}

	uint64_t Key(const Signature& a_signature)
	{
		uint64_t key = 0xcbf29ce484222325ull;

		const auto mix = [&key](uint64_t a_value) {
			key ^= a_value;
			key *= 0x100000001b3ull;
		};

		for (const auto& e : a_signature.elements) {
			for (const char c : e.semanticName) {
				mix(static_cast<uint64_t>(static_cast<unsigned char>(c)));
			}
			mix(e.semanticIndex);
			mix(e.registerIndex);
			mix(e.mask);
			mix(e.componentType);
		}

		return key;
	}

	std::string Describe(const Signature& a_signature)
	{
		if (!a_signature.valid) {
			return "  <reflection failed>";
		}

		std::string out;
		for (const auto& e : a_signature.elements) {
			out += std::format("\n    {:<8} {:<14} reg={:<2} mask={:<4}",
				e.hlslType, e.Semantic(), e.registerIndex, MaskString(e.mask));
		}
		return out;
	}
}
