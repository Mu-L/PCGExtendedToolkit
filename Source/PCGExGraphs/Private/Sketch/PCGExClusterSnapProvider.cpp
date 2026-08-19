// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExClusterSnapProvider.h"

#include "Sketch/PCGExClusterSketch.h"

#pragma region UPCGExClusterSnapProvider

#if WITH_EDITOR
void UPCGExClusterSnapProvider::PostEditUndo()
{
	Super::PostEditUndo();

	// An undo that deletes this object reaches here already invalid.
	if (!IsValidChecked(this) || IsTemplate())
	{
		return;
	}

	if (UPCGExClusterSketch* Owner = GetTypedOuter<UPCGExClusterSketch>())
	{
		Owner->EDITOR_OnSnapProviderChanged();
	}
}

void UPCGExClusterSnapProvider::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (!IsValidChecked(this) || IsTemplate())
	{
		return;
	}

	if (UPCGExClusterSketch* Owner = GetTypedOuter<UPCGExClusterSketch>())
	{
		Owner->EDITOR_OnSnapProviderChanged();
	}
}
#endif

#pragma endregion

#pragma region UPCGExClusterSnapProvider_UniformGrid

bool UPCGExClusterSnapProvider_UniformGrid::BuildBasis(FPCGExLatticeBasis& OutBasis) const
{
	TArray<FVector, TInlineAllocator<3>> Directions;
	if (bSpanX)
	{
		Directions.Add(FVector::XAxisVector);
	}
	if (bSpanY)
	{
		Directions.Add(FVector::YAxisVector);
	}
	if (bSpanZ)
	{
		Directions.Add(FVector::ZAxisVector);
	}

	TArray<double, TInlineAllocator<3>> LengthMultipliers;
	LengthMultipliers.Init(1.0, Directions.Num());

	return OutBasis.BuildFromSteps(Directions, LengthMultipliers, CellSize, Origin, Rotation.Quaternion());
}

#pragma endregion
