﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Graph/Pathfinding/GoalPickers/PCGExGoalPickerRandom.h"

#include "PCGExMath.h"
#include "PCGExRandom.h"


void UPCGExGoalPickerRandom::CopySettingsFrom(const UPCGExInstancedFactory* Other)
{
	Super::CopySettingsFrom(Other);
	if (const UPCGExGoalPickerRandom* TypedOther = Cast<UPCGExGoalPickerRandom>(Other))
	{
		LocalSeed = TypedOther->LocalSeed;
		GoalCount = TypedOther->GoalCount;
		NumGoalsType = TypedOther->NumGoalsType;
		NumGoals = TypedOther->NumGoals;
		NumGoalAttribute = TypedOther->NumGoalAttribute;
	}
}

bool UPCGExGoalPickerRandom::PrepareForData(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InSeedsDataFacade, const TSharedPtr<PCGExData::FFacade>& InGoalsDataFacade)
{
	if (!Super::PrepareForData(InContext, InSeedsDataFacade, InGoalsDataFacade)) { return false; }

	NumGoalsBuffer = GetValueSettingNumGoals();
	if (!NumGoalsBuffer->Init(InContext, InSeedsDataFacade, false)) { return false; }

	return true;
}

int32 UPCGExGoalPickerRandom::GetGoalIndex(const PCGExData::FConstPoint& Seed) const
{
	return PCGExMath::SanitizeIndex(FRandomStream(PCGExRandom::GetRandomStreamFromPoint(Seed.Data->GetSeed(Seed.Index), LocalSeed)).RandRange(0, MaxGoalIndex), MaxGoalIndex, IndexSafety);
}

void UPCGExGoalPickerRandom::GetGoalIndices(const PCGExData::FConstPoint& Seed, TArray<int32>& OutIndices) const
{
	int32 Picks = NumGoalsBuffer->Read(Seed.Index);

	if (GoalCount == EPCGExGoalPickRandomAmount::Random)
	{
		Picks = PCGExMath::Remap(
			FMath::PerlinNoise3D(PCGExMath::Tile(Seed.GetLocation() * 0.001 + Picks, FVector(-1), FVector(1))),
			-1, 1, 0, Picks);
	}

	Picks = FMath::Min(1, FMath::Min(Picks, MaxGoalIndex));

	for (int i = 0; i < Picks; i++)
	{
		int32 Index = static_cast<int32>(PCGExMath::Remap(
			FMath::PerlinNoise3D(PCGExMath::Tile(Seed.GetLocation() * 0.001 + i, FVector(-1), FVector(1))),
			-1, 1, 0, MaxGoalIndex));
		OutIndices.Add(PCGExMath::SanitizeIndex(Index, MaxGoalIndex, IndexSafety));
	}
}

bool UPCGExGoalPickerRandom::OutputMultipleGoals() const { return GoalCount != EPCGExGoalPickRandomAmount::Single; }

void UPCGExGoalPickerRandom::Cleanup()
{
	Super::Cleanup();
}
