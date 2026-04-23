// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;

namespace PCGExActorDelta
{
	/**
	 * Serialize properties that differ from defaults for an actor AND its components.
	 * Actor-level properties are diffed against the actor CDO.
	 * Each instanced component is diffed against its archetype.
	 * Returns empty array if actor and all components match defaults exactly.
	 *
	 * Format is opaque -- use ApplyPropertyDelta to deserialize.
	 */
	PCGEXCOLLECTIONS_API TArray<uint8> SerializeActorDelta(AActor* Actor);

	/**
	 * Apply a previously serialized property delta to an actor and its components.
	 * Components are matched by name. Missing/renamed components are safely skipped.
	 *
	 * After delta bytes are applied, any post-apply fixups registered via
	 * RegisterPostApplyFixup() are invoked on matching components.
	 */
	PCGEXCOLLECTIONS_API void ApplyPropertyDelta(AActor* Actor, const TArray<uint8>& DeltaBytes);

	/** Compute CRC32 hash of delta bytes. Returns 0 for empty input. */
	PCGEXCOLLECTIONS_API uint32 HashDelta(const TArray<uint8>& DeltaBytes);

	/**
	 * Callback invoked on each component of the target actor after deltas are applied.
	 *
	 * Fixups exist to repair engine-managed invariants that a tagged-property delta
	 * cannot express: aliased EditAnywhere fields the engine expects to stay consistent
	 * (e.g. USplineComponent's SplineCurves and Spline in UE 5.7+), transient caches
	 * that must be rebuilt (reparam tables, bounds), etc.
	 *
	 * A fixup fires for every component whose class is or derives from the registered
	 * class, in registration order. Fixups run regardless of whether the component had
	 * delta bytes applied -- the same inconsistencies can arise from archetype cloning
	 * alone.
	 *
	 * Archetype may be null when a component has no per-actor archetype (engine-managed
	 * components). Fixups must tolerate that case.
	 */
	using FPostApplyFixup = TFunction<void(UActorComponent* Component, UObject* Archetype)>;

	/** Register a post-apply fixup. Typically called once at module startup. */
	PCGEXCOLLECTIONS_API void RegisterPostApplyFixup(UClass* ComponentClass, FPostApplyFixup Fixup);

	/** Remove all fixups previously registered for ComponentClass. */
	PCGEXCOLLECTIONS_API void UnregisterPostApplyFixupsForClass(UClass* ComponentClass);
}
