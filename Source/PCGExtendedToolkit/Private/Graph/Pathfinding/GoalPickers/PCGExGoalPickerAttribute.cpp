﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Graph/Pathfinding/GoalPickers/PCGExGoalPickerAttribute.h"


void UPCGExGoalPickerAttribute::CopySettingsFrom(const UPCGExInstancedFactory* Other)
{
	Super::CopySettingsFrom(Other);
	if (const UPCGExGoalPickerAttribute* TypedOther = Cast<UPCGExGoalPickerAttribute>(Other))
	{
		GoalCount = TypedOther->GoalCount;
		SingleSelector = TypedOther->SingleSelector;
		AttributeSelectors = TypedOther->AttributeSelectors;

		PCGExHelpers::AppendUniqueSelectorsFromCommaSeparatedList(TypedOther->CommaSeparatedNames, AttributeSelectors);
	}
}

bool UPCGExGoalPickerAttribute::PrepareForData(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InSeedsDataFacade, const TSharedPtr<PCGExData::FFacade>& InGoalsDataFacade)
{
	if (!Super::PrepareForData(InContext, InSeedsDataFacade, InGoalsDataFacade)) { return false; }

	if (GoalCount == EPCGExGoalPickAttributeAmount::Single)
	{
		SingleGetter = InSeedsDataFacade->GetBroadcaster<int32>(SingleSelector);

		if (!SingleGetter)
		{
			PCGEX_LOG_INVALID_SELECTOR_C(InContext, "Index (Seeds)", SingleSelector)
			return false;
		}
	}
	else
	{
		PCGExHelpers::AppendUniqueSelectorsFromCommaSeparatedList(CommaSeparatedNames, AttributeSelectors);

		AttributeGetters.Reset(AttributeSelectors.Num());
		for (const FPCGAttributePropertyInputSelector& Selector : AttributeSelectors)
		{
			TSharedPtr<PCGExData::TBuffer<int32>> Getter = InSeedsDataFacade->GetBroadcaster<int32>(Selector);
			if (!Getter)
			{
				PCGEX_LOG_INVALID_SELECTOR_C(InContext, "Index (Seeds)", SingleSelector)
				return false;
			}

			AttributeGetters.Add(Getter);
		}
	}

	return true;
}

int32 UPCGExGoalPickerAttribute::GetGoalIndex(const PCGExData::FConstPoint& Seed) const
{
	return PCGExMath::SanitizeIndex(SingleGetter ? SingleGetter->Read(Seed.Index) : -1, MaxGoalIndex, IndexSafety);
}

void UPCGExGoalPickerAttribute::GetGoalIndices(const PCGExData::FConstPoint& Seed, TArray<int32>& OutIndices) const
{
	for (const TSharedPtr<PCGExData::TBuffer<int32>>& Getter : AttributeGetters)
	{
		if (!Getter) { continue; }
		OutIndices.Add(PCGExMath::SanitizeIndex(Getter->Read(Seed.Index), MaxGoalIndex, IndexSafety));
	}
}

bool UPCGExGoalPickerAttribute::OutputMultipleGoals() const { return GoalCount != EPCGExGoalPickAttributeAmount::Single; }
