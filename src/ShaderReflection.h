// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <string>
#include <vector>

namespace Reflection
{
	struct SignatureElement
	{
		std::string   semanticName;
		uint32_t      semanticIndex;
		uint32_t      registerIndex;
		uint8_t       mask;
		uint8_t       componentType;
		std::string   hlslType;

		std::string Semantic() const;
	};

	struct Signature
	{
		std::vector<SignatureElement> elements;
		bool                          valid{ false };
	};

	Signature ReflectOutputSignature(const void* a_bytecode, size_t a_size);
	Signature ReflectInputSignature(const void* a_bytecode, size_t a_size);

	// The output signature chunk (OSGN, OSG5 or OSG1, header included) of a DXBC
	// container, copied out so the rest of the bytecode need not be kept; empty if there
	// is none. ParseSignatureChunk reads it back into the same Signature that
	// ReflectOutputSignature gives for the whole shader.
	std::vector<uint8_t> OutputSignatureChunk(const void* a_bytecode, size_t a_size);
	Signature            ParseSignatureChunk(const void* a_chunk, size_t a_size);

	// Whether every element a_reader takes is written by a_writer on the same register,
	// with the same component type and at least the components read (semantics compare
	// case-insensitively, as HLSL does). a_why names the first mismatch.
	bool Covers(const Signature& a_writer, const Signature& a_reader, std::string* a_why = nullptr);

	std::string Describe(const Signature& a_signature);

	uint64_t Key(const Signature& a_signature);
}
