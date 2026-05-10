// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Domains/PCGExSpatialDomain_Broadphase.h"

#include "Math/OBB/PCGExOBB.h"
#include "NarrowPhase/PCGExNarrowPhase.h"
#include "Shapes/PCGExFootprintShape.h"

namespace
{
	/**
	 * Build an identity-orientation FOBB from a world AABB. Stored entries
	 * use this lensing so the FDynamicCollection's SAT pass collapses to
	 * AABB-vs-AABB. StorageIndex goes into Bounds.Index for the broadphase
	 * walk's per-entry callback to recover the entry.
	 */
	PCGExMath::OBB::FOBB MakeAABBLensedOBB(const FBox& WorldAABB, int32 StorageIndex)
	{
		const FVector Center = WorldAABB.GetCenter();
		const FVector Extents = WorldAABB.GetExtent();
		return PCGExMath::OBB::FOBB(
			PCGExMath::OBB::FBounds(Center, Extents, StorageIndex),
			PCGExMath::OBB::FOrientation(FQuat::Identity));
	}
}

float FPCGExSpatialDomain_Broadphase::QueryPoint(const FVector& Point) const
{
	// Best-effort signed distance reduced to inside/outside indication.
	// The broadphase's AABB lensing answers "is this point inside any
	// stored AABB?" without per-shape signed-distance math. Per-shape
	// distance lands when a placement condition needs the magnitude.
	return BroadphaseAABBs.IsPointInside(Point) ? -1.0f : 1.0f;
}

bool FPCGExSpatialDomain_Broadphase::Overlaps(
	const FPCGExFootprintShape& Candidate,
	int32 SkipOwnerIndex,
	TFunctionRef<bool(int32)> ShouldSkip) const
{
	if (NumValidEntries == 0) { return false; }

	const FBox CandidateAABB = Candidate.GetWorldAABB();
	if (!CandidateAABB.IsValid) { return false; }

	// Candidate enters the broadphase as an identity-orientation OBB so
	// the collection's SAT path is AABB-vs-AABB on both sides -- cheap,
	// tight, and the "real" overlap question is answered by the narrow
	// phase per surviving entry.
	const PCGExMath::OBB::FOBB CandidateOBB = MakeAABBLensedOBB(CandidateAABB, INDEX_NONE);

	// ShouldSkipOwner predicate runs per broadphase-culled entry. The
	// FOBB's Bounds.Index is our STORAGE index (set in Append); resolve
	// owner index via Entries[].OwnerIndex.
	auto SkipPredicate = [this, SkipOwnerIndex, &ShouldSkip](int32 StorageIdx) -> bool
	{
		if (!ValidMask.IsValidIndex(StorageIdx) || !ValidMask[StorageIdx]) { return true; }
		const int32 OwnerIdx = Entries[StorageIdx].OwnerIndex;
		return (SkipOwnerIndex != INDEX_NONE && OwnerIdx == SkipOwnerIndex)
			|| ShouldSkip(OwnerIdx);
	};

	// ConfirmOverlap runs per SAT-confirmed (AABB-confirmed) entry. The
	// narrow phase resolves the (Candidate.UScriptStruct, Stored.UScriptStruct)
	// pair test from the registry; first to overlap stops the walk.
	auto ConfirmOverlap = [this, &Candidate](
		const PCGExMath::OBB::FOBB&, int32 StorageIdx) -> bool
	{
		const FInstancedStruct& Wrapper = Entries[StorageIdx].Shape;
		const FPCGExFootprintShape* StoredShape = Wrapper.GetPtr<FPCGExFootprintShape>();
		if (!StoredShape) { return false; }
		return PCGExSpatial::NarrowPhase::TestOverlap(Candidate, *StoredShape);
	};

	return BroadphaseAABBs.ForEachOverlapping(CandidateOBB, INDEX_NONE,
		SkipPredicate, ConfirmOverlap);
}

bool FPCGExSpatialDomain_Broadphase::OverlapsBeyondThreshold(
	const FPCGExFootprintShape& Candidate,
	float MaxAllowedPenetration,
	int32 SkipOwnerIndex) const
{
	if (NumValidEntries == 0) { return false; }

	const FBox CandidateAABB = Candidate.GetWorldAABB();
	if (!CandidateAABB.IsValid) { return false; }

	const PCGExMath::OBB::FOBB CandidateOBB = MakeAABBLensedOBB(CandidateAABB, INDEX_NONE);

	auto SkipPredicate = [this, SkipOwnerIndex](int32 StorageIdx) -> bool
	{
		if (!ValidMask.IsValidIndex(StorageIdx) || !ValidMask[StorageIdx]) { return true; }
		const int32 OwnerIdx = Entries[StorageIdx].OwnerIndex;
		return SkipOwnerIndex != INDEX_NONE && OwnerIdx == SkipOwnerIndex;
	};

	// Per-entry penetration test via the registry. First entry whose
	// magnitude exceeds the threshold rejects the candidate.
	auto ConfirmExceedsThreshold = [this, &Candidate, MaxAllowedPenetration](
		const PCGExMath::OBB::FOBB&, int32 StorageIdx) -> bool
	{
		const FInstancedStruct& Wrapper = Entries[StorageIdx].Shape;
		const FPCGExFootprintShape* StoredShape = Wrapper.GetPtr<FPCGExFootprintShape>();
		if (!StoredShape) { return false; }
		const float Pen = PCGExSpatial::NarrowPhase::QueryPenetration(Candidate, *StoredShape);
		return Pen > MaxAllowedPenetration;
	};

	return BroadphaseAABBs.ForEachOverlapping(CandidateOBB, INDEX_NONE,
		SkipPredicate, ConfirmExceedsThreshold);
}

int32 FPCGExSpatialDomain_Broadphase::Append(const FPCGExFootprintShape& Shape, int32 OwnerIndex)
{
	// Owner-index >= 0 contract: INDEX_NONE is the skip-nothing sentinel
	// and would silently make this entry untargetable by skip-by-owner.
	check(OwnerIndex >= 0);

	const UScriptStruct* StructType = Shape.GetScriptStruct();
	if (!StructType) { return INDEX_NONE; }

	const FBox WorldAABB = Shape.GetWorldAABB();
	if (!WorldAABB.IsValid) { return INDEX_NONE; }

	const int32 StorageIdx = Entries.Num();

	FEntry& Entry = Entries.AddDefaulted_GetRef();
	// Dynamic-typed copy: reads the UScriptStruct's CppStructOps to clone
	// the runtime shape's memory into the instanced-struct wrapper.
	Entry.Shape.InitializeAs(StructType, reinterpret_cast<const uint8*>(&Shape));
	Entry.OwnerIndex = OwnerIndex;
	Entry.WorldAABB = WorldAABB;

	ValidMask.Add(true);
	++NumValidEntries;
	WorldBounds += WorldAABB;

	// Lens AABB as identity-orientation OBB; Bounds.Index = StorageIdx so
	// the broadphase walk's per-entry callbacks can recover the entry.
	BroadphaseAABBs.Add(MakeAABBLensedOBB(WorldAABB, StorageIdx));

	return StorageIdx;
}

FPCGExSpatialDomain::FSnapshotHandle FPCGExSpatialDomain_Broadphase::BeginSnapshotScope()
{
	// High-water mark; rollback flips ValidMask bits past the handle.
	// O(1) amortized, no realloc.
	return Entries.Num();
}

void FPCGExSpatialDomain_Broadphase::RollbackToScope(FSnapshotHandle Handle)
{
	const int32 RollbackTo = static_cast<int32>(Handle);
	if (RollbackTo < 0 || RollbackTo >= Entries.Num()) { return; }

	for (int32 i = RollbackTo; i < Entries.Num(); ++i)
	{
		if (ValidMask[i])
		{
			ValidMask[i] = false;
			--NumValidEntries;
		}
	}

	// Roll the broadphase backing in the same direction. FDynamicCollection's
	// Invalidate flips its own ValidMask bits past FromIndex -- same O(1)
	// amortized model. Storage indices are stable across rollback (entries
	// stay in memory; only the validity bit flips), so the FOBB's
	// Bounds.Index inside BroadphaseAABBs still maps to Entries[].
	BroadphaseAABBs.Invalidate(RollbackTo);

	RecomputeWorldBounds();
}

void FPCGExSpatialDomain_Broadphase::RecomputeWorldBounds()
{
	WorldBounds = FBox(ForceInit);
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		if (ValidMask[i]) { WorldBounds += Entries[i].WorldAABB; }
	}
}
