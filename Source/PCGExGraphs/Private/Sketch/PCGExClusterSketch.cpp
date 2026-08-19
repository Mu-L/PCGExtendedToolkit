// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExClusterSketch.h"

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
#endif
