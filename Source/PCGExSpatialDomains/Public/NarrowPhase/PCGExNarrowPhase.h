// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Shapes/PCGExFootprintShape.h"

namespace PCGExSpatial::NarrowPhase
{
	/**
	 * Pair-test function signatures. Free functions, registered by shape pair
	 * at module-init time. Receive both operands as base-class refs; impls
	 * static_cast to their concrete shape types after the registry has matched
	 * the (StructA, StructB) keys -- the cast is type-safe by construction.
	 */
	using FPairOverlapFn     = bool (*)(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B);
	using FPairPenetrationFn = float (*)(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B);

	/**
	 * Per-pair function bundle. Penetration is optional -- when null, the
	 * registry's QueryPenetration() falls back to a binary "any overlap is
	 * infinite penetration" semantic via Overlap.
	 */
	struct PCGEXSPATIALDOMAINS_API FPairFns
	{
		FPairOverlapFn     Overlap     = nullptr;
		FPairPenetrationFn Penetration = nullptr;
	};

	/**
	 * Register the pair test for (StructA, StructB). Symmetric -- callers
	 * may pass A,B and B,A interchangeably; registration stores under the
	 * canonical (lower-pointer-first) key, lookup tries both orientations.
	 *
	 * Registering the same pair twice is a programmer error: ensures in
	 * debug, last-write-wins in shipping. Module load order: register
	 * during StartupModule; queries from any phase after that are safe.
	 *
	 * Either of the Fns members may be null. A null Overlap effectively
	 * disables the pair (TestOverlap will return false / not-overlapping);
	 * a null Penetration falls back to Overlap.
	 */
	PCGEXSPATIALDOMAINS_API void Register(
		UScriptStruct* StructA,
		UScriptStruct* StructB,
		FPairFns Fns);

	/** Drop all registrations. Test/utility hook -- production code never calls this. */
	PCGEXSPATIALDOMAINS_API void UnregisterAll();

	/**
	 * Symmetric overlap query. Resolves (A.GetScriptStruct(), B.GetScriptStruct())
	 * in the registry -- exact pair preferred, fallback to swapped-order with
	 * args automatically swapped. Missing pair returns false (the safe direction
	 * for "no opinion"). Stack-safe and lock-free post-registration.
	 */
	PCGEXSPATIALDOMAINS_API bool TestOverlap(
		const FPCGExFootprintShape& A,
		const FPCGExFootprintShape& B);

	/**
	 * Symmetric penetration query. When the resolved pair has no Penetration
	 * fn, falls back to the Overlap fn -- any overlap counts as infinite
	 * penetration (the conservative direction for "did this exceed the
	 * tolerance?"). Returns +INFINITY for the missing-pair case.
	 */
	PCGEXSPATIALDOMAINS_API float QueryPenetration(
		const FPCGExFootprintShape& A,
		const FPCGExFootprintShape& B);
}
