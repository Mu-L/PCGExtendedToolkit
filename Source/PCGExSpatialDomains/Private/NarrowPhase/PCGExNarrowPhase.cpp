// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "NarrowPhase/PCGExNarrowPhase.h"

#include "Containers/Map.h"

namespace PCGExSpatial::NarrowPhase
{
	namespace
	{
		using FPairKey = TPair<UScriptStruct*, UScriptStruct*>;

		/**
		 * The singleton registry. Module-scoped (function-local static) so it
		 * lives in the .obj of this TU regardless of unity-build mergers.
		 * Module init populates; module shutdown clears (UnregisterAll). The
		 * map is read-only during query phase, so no synchronization is needed
		 * past registration -- callers are responsible for not registering
		 * during placement runs.
		 *
		 * Storage convention: we store under the EXACT (StructA, StructB)
		 * order the registration specified -- pair-test impls distinguish
		 * args by static_cast and depend on a consistent argument layout, so
		 * canonicalizing keys would either crash on asymmetric pairs or force
		 * impls to dynamic-cast on every call. Symmetry is provided by the
		 * lookup side: missing (A, B) falls back to (B, A) with arg swap.
		 */
		TMap<FPairKey, FPairFns>& GetRegistry()
		{
			static TMap<FPairKey, FPairFns> Registry;
			return Registry;
		}

		/**
		 * Resolve a pair lookup. Tries (A, B) first; on miss, tries (B, A)
		 * and signals the caller to swap args. Missing both directions
		 * returns nullptr.
		 */
		const FPairFns* Resolve(UScriptStruct* A, UScriptStruct* B, bool& bOutSwapArgs)
		{
			bOutSwapArgs = false;
			if (!A || !B) { return nullptr; }

			const TMap<FPairKey, FPairFns>& Reg = GetRegistry();

			if (const FPairFns* Found = Reg.Find(FPairKey(A, B)))
			{
				return Found;
			}
			if (const FPairFns* Found = Reg.Find(FPairKey(B, A)))
			{
				bOutSwapArgs = true;
				return Found;
			}
			return nullptr;
		}
	}

	void Register(UScriptStruct* StructA, UScriptStruct* StructB, FPairFns Fns)
	{
		if (!ensureMsgf(StructA && StructB, TEXT("PCGExSpatial::NarrowPhase::Register: null UScriptStruct*"))) { return; }

		const FPairKey Key(StructA, StructB);

		TMap<FPairKey, FPairFns>& Reg = GetRegistry();
		if (FPairFns* Existing = Reg.Find(Key))
		{
			ensureMsgf(false,
				TEXT("PCGExSpatial::NarrowPhase: duplicate registration for (%s, %s) -- last write wins."),
				*StructA->GetName(), *StructB->GetName());
			*Existing = Fns;
			return;
		}

		// Catch the "registered both orientations" pattern -- impls expect a
		// specific arg order, so registering (B, A) after (A, B) means one of
		// them will be called with mismatched args and crash on static_cast.
		// One direction only; lookup handles the swap.
		ensureMsgf(!Reg.Contains(FPairKey(StructB, StructA)),
			TEXT("PCGExSpatial::NarrowPhase: pair (%s, %s) is already registered in the reverse direction. "
			     "Register only one orientation; lookups in the other direction resolve via arg swap."),
			*StructA->GetName(), *StructB->GetName());

		Reg.Add(Key, Fns);
	}

	void UnregisterAll()
	{
		GetRegistry().Reset();
	}

	bool TestOverlap(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B)
	{
		bool bSwap = false;
		const FPairFns* Fns = Resolve(A.GetScriptStruct(), B.GetScriptStruct(), bSwap);
		if (!Fns || !Fns->Overlap) { return false; }
		return bSwap ? Fns->Overlap(B, A) : Fns->Overlap(A, B);
	}

	float QueryPenetration(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B)
	{
		bool bSwap = false;
		const FPairFns* Fns = Resolve(A.GetScriptStruct(), B.GetScriptStruct(), bSwap);
		if (!Fns) { return TNumericLimits<float>::Max(); }

		if (Fns->Penetration)
		{
			return bSwap ? Fns->Penetration(B, A) : Fns->Penetration(A, B);
		}

		// No penetration fn: degenerate to "any overlap is infinite penetration".
		// Callers comparing against MaxAllowedPenetration get the conservative
		// reject, matching the FSpatialDomain default-base semantic.
		if (Fns->Overlap)
		{
			const bool bOverlaps = bSwap ? Fns->Overlap(B, A) : Fns->Overlap(A, B);
			return bOverlaps ? TNumericLimits<float>::Max() : 0.0f;
		}

		return TNumericLimits<float>::Max();
	}
}
