// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExClusterSketch.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

bool UPCGExClusterSketch::BuildBasis(FPCGExLatticeBasis& OutBasis) const
{
	return SnapProvider ? SnapProvider->BuildBasis(OutBasis) : false;
}

void UPCGExClusterSketch::CollectAssetDependencies(TArray<FSoftObjectPath>& OutPaths) const
{
	if (SnapProvider)
	{
		SnapProvider->CollectAssetDependencies(OutPaths);
	}
	for (const TObjectPtr<UPCGExClusterSketchDecorator>& Decorator : Decorators)
	{
		if (Decorator && Decorator->bEnabled)
		{
			Decorator->CollectAssetDependencies(OutPaths);
		}
	}
}

#if WITH_EDITOR
void UPCGExClusterSketch::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName MemberName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	if (MemberName == GET_MEMBER_NAME_CHECKED(UPCGExClusterSketch, SnapProvider))
	{
		EDITOR_OnSnapProviderChanged();
		return;
	}

	if (MemberName == GET_MEMBER_NAME_CHECKED(UPCGExClusterSketch, Model))
	{
		// Coord edited -> the coord wins; anything else -> re-snap from location first. Both paths are
		// idempotent for already-coherent vertices, so over-triggering on unrelated model edits is free.
		const FName LeafName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
		const bool bCoordEdit = LeafName == GET_MEMBER_NAME_CHECKED(FPCGExClusterSketchVertex, LatticeCoord);
		EDITOR_SyncBoundVertices(!bCoordEdit);
	}
}

void UPCGExClusterSketch::PostEditUndo()
{
	Super::PostEditUndo();

	if (!IsValidChecked(this) || IsTemplate())
	{
		return;
	}

	EDITOR_SyncBoundVertices(false);
}

void UPCGExClusterSketch::EDITOR_OnSnapProviderChanged()
{
	EDITOR_SyncBoundVertices(false);
}

void UPCGExClusterSketch::EDITOR_SyncBoundVertices(const bool bResnapFromLocation)
{
	FPCGExLatticeBasis Basis;
	if (!BuildBasis(Basis))
	{
		return;
	}
	Model.SyncBoundVertices(Basis, bResnapFromLocation);
}

void UPCGExClusterSketch::MergeCollocatedVertices()
{
	FPCGExLatticeBasis Basis;
	const bool bHasBasis = BuildBasis(Basis);

	const FScopedTransaction Transaction(NSLOCTEXT("PCGExClusterSketch", "MergeCollocated", "Merge Collocated Sketch Vertices"));
	Modify();

	// Every merge remaps indices, so rescan from scratch after each one; the guard bounds the loop by
	// the only thing it can shrink.
	bool bMergedAny = true;
	int32 Guard = Model.Vertices.Num() + 1;
	while (bMergedAny && Guard-- > 0)
	{
		bMergedAny = false;
		TMap<FVector, int32> FirstAtLocation;
		FirstAtLocation.Reserve(Model.Vertices.Num());
		for (int32 i = 0; i < Model.Vertices.Num(); ++i)
		{
			const FPCGExClusterSketchVertex& V = Model.Vertices[i];
			const FVector Location = (V.bLatticeBound && bHasBasis) ? Basis.CoordToWorld(V.LatticeCoord) : V.Transform.GetLocation();
			const FVector Key = PCGExSketch::QuantizedLocationKey(Location);
			if (const int32* First = FirstAtLocation.Find(Key))
			{
				Model.MergeVertices(i, *First);
				bMergedAny = true;
				break;
			}
			FirstAtLocation.Add(Key, i);
		}
	}

	PostEditChange();
}

void UPCGExClusterSketch::RemoveInvalidEdges()
{
	const FScopedTransaction Transaction(NSLOCTEXT("PCGExClusterSketch", "RemoveInvalidEdges", "Remove Invalid Sketch Edges"));
	Modify();
	Model.RemoveInvalidEdges();
	PostEditChange();
}
#endif
