// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN). Fallout 4 port.

#include "PCH.h"

#include "Offsets.h"
#include "Physics.h"

namespace Physics
{
	namespace
	{
		// bhkPickData is 0xE0 bytes on 1.10.163; the buffer leaves room to spare.
		struct alignas(16) PickBuffer
		{
			std::byte bytes[0x140]{};
		};

		constexpr std::ptrdiff_t kCastWorld = 0xC0;           // hkRefPtr<hknpBSWorld>
		constexpr std::ptrdiff_t kCustomCollideLayers = 0xC8;  // std::uint64_t
		constexpr std::uint32_t  kFilter = 0x60000;            // as TES::GetMaterialType

		using CtorFn = void* (*)(void*, std::uint32_t, const RE::NiPoint3&, const RE::NiPoint3&);
		using PickFn = RE::NiAVObject* (*)(RE::TESObjectCELL*, void*);
		using HasHitFn = bool (*)(const void*);
		using FractionFn = float (*)(const void*);

		struct Functions
		{
			CtorFn     ctor{ nullptr };
			PickFn     pick{ nullptr };
			HasHitFn   hasHit{ nullptr };
			FractionFn fraction{ nullptr };
		};

		const Functions& Resolve()
		{
			static const Functions functions{
				reinterpret_cast<CtorFn>(Offsets::Address(Offsets::PickDataCtor)),
				reinterpret_cast<PickFn>(Offsets::Address(Offsets::CellPick)),
				reinterpret_cast<HasHitFn>(Offsets::Address(Offsets::PickHasHit)),
				reinterpret_cast<FractionFn>(Offsets::Address(Offsets::PickGetHitFraction)),
			};
			return functions;
		}

		// hkReferencedObject::removeReference, as TES::GetMaterialType releases the
		// world a pick keeps a reference to.
		void ReleaseCastWorld(PickBuffer& a_pick)
		{
			auto* world = *reinterpret_cast<std::byte**>(a_pick.bytes + kCastWorld);
			if (!world) {
				return;
			}
			*reinterpret_cast<std::byte**>(a_pick.bytes + kCastWorld) = nullptr;

			if (*reinterpret_cast<volatile std::uint16_t*>(world + 0xA) == 0) {
				return;
			}

			auto* counts = reinterpret_cast<volatile LONG*>(world + 0x8);
			LONG  before = 0;
			LONG  after = 0;
			do {
				before = *counts;
				after = static_cast<LONG>((static_cast<std::uint32_t>(before) & 0xFFFF0000u) |
										  ((static_cast<std::uint32_t>(before) - 1u) & 0xFFFFu));
			} while (InterlockedCompareExchange(counts, after, before) != before);

			if ((static_cast<std::uint32_t>(after) & 0xFFFFu) == 0) {
				using DeleteFn = void (*)(void*);
				auto** vtable = *reinterpret_cast<void***>(world);
				reinterpret_cast<DeleteFn>(vtable[3])(world);
			}
		}
	}

	bool Available()
	{
		const auto& f = Resolve();
		return f.ctor && f.pick && f.hasHit && f.fraction;
	}

	float CastRay(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		std::uint64_t a_layers)
	{
		if (!a_cell || !Available()) {
			return -1.0f;
		}

		const auto& f = Resolve();

		PickBuffer pick{};
		f.ctor(pick.bytes, kFilter, a_from, a_to);
		*reinterpret_cast<std::uint64_t*>(pick.bytes + kCustomCollideLayers) = a_layers;

		f.pick(a_cell, pick.bytes);

		float result = -1.0f;
		if (f.hasHit(pick.bytes)) {
			result = std::clamp(f.fraction(pick.bytes), 0.0f, 1.0f);
		}

		ReleaseCastWorld(pick);
		return result;
	}
}
