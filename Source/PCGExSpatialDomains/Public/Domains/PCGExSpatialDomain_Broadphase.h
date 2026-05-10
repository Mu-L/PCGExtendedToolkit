// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Domains/PCGExSpatialDomain.h"
#include "Math/OBB/PCGExOBB.h"
#include "Math/OBB/PCGExOBBCollection.h"
#include "Shapes/PCGExFootprintShape.h"
#include "StructUtils/InstancedStruct.h"

/**
 * Heterogeneous mutable spatial domain. Single concrete impl that replaces
 * the previous per-shape-typed combo (OBB list + polygon list + facade) --
 * shape-agnostic by construction:
 *
 *   - Storage: parallel arrays of (FInstancedStruct shape payload, owner idx, validity bit)
 *   - Broadphase: FDynamicCollection lensed as an AABB octree -- each entry's
 *     WorldAABB is added as an identity-orientation OBB, so the collection's
 *     SAT path collapses to AABB-vs-AABB (cheap, tight, no shape-specific math)
 *   - Narrow phase: PCGExSpatial::NarrowPhase registry, dispatched per
 *     stored entry by (Candidate.UScriptStruct, Stored.UScriptStruct)
 *
 * Adding a new shape kind requires zero edits here. Register pair tests via
 * NarrowPhase::Register from the shape's owning module's StartupModule;
 * the broadphase routes automatically.
 *
 * Owner identity:
 *   The OBB's Bounds.Index slot holds the entry's STORAGE index (into Entries[])
 *   -- not the owner index. Owner index is stored alongside the shape payload.
 *   The skip-by-owner ShouldSkip predicate lambda receives storage idx and
 *   resolves owner via Entries[idx].OwnerIndex internally; outward API still
 *   takes owner index from callers, matching the FPCGExSpatialDomain contract.
 *
 * Snapshot model:
 *   BeginSnapshotScope() returns Entries.Num() (high-water mark).
 *   RollbackToScope(handle) flips ValidMask bits past handle to invalid.
 *   O(1) amortized, no realloc.
 */
class PCGEXSPATIALDOMAINS_API FPCGExSpatialDomain_Broadphase : public FPCGExSpatialDomain
{
public:
	FPCGExSpatialDomain_Broadphase() = default;
	virtual ~FPCGExSpatialDomain_Broadphase() override = default;

	// ========== Query ==========

	virtual float QueryPoint(const FVector& Point) const override;

	// Unified shape-agnostic queries -- the canonical path.
	virtual bool Overlaps(
		const FPCGExFootprintShape& Candidate,
		int32 SkipOwnerIndex,
		TFunctionRef<bool(int32)> ShouldSkip) const override;
	using FPCGExSpatialDomain::Overlaps; // pull in 2-arg no-skip overload

	virtual bool OverlapsBeyondThreshold(
		const FPCGExFootprintShape& Candidate,
		float MaxAllowedPenetration,
		int32 SkipOwnerIndex = INDEX_NONE) const override;

	virtual FBox GetBounds() const override { return WorldBounds; }
	virtual bool IsValid() const override { return NumValidEntries > 0; }

	// ========== Mutation + snapshot ==========

	virtual bool IsMutable() const override { return true; }
	virtual int32 Append(const FPCGExFootprintShape& Shape, int32 OwnerIndex) override;
	virtual FSnapshotHandle BeginSnapshotScope() override;
	virtual void RollbackToScope(FSnapshotHandle Handle) override;

	// ========== Inspection (tests / debug) ==========

	int32 Num() const { return Entries.Num(); }
	int32 NumValid() const { return NumValidEntries; }

private:
	struct FEntry
	{
		FInstancedStruct Shape;
		int32 OwnerIndex = INDEX_NONE;
		FBox WorldAABB = FBox(ForceInit);
	};

	TArray<FEntry> Entries;
	TBitArray<> ValidMask;
	int32 NumValidEntries = 0;
	FBox WorldBounds = FBox(ForceInit);

	/**
	 * Broadphase backing -- FDynamicCollection used as an AABB octree.
	 * Each entry's WorldAABB enters as an identity-orientation FOBB
	 * centered on the AABB; collection's SAT path then collapses to
	 * AABB-vs-AABB. Bounds.Index = storage index (into Entries[]).
	 */
	PCGExMath::OBB::FDynamicCollection BroadphaseAABBs;

	void RecomputeWorldBounds();
};
