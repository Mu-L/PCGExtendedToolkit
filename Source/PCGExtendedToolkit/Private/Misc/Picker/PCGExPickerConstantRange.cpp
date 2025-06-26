﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Misc/Pickers/PCGExPickerConstantRange.h"

#define LOCTEXT_NAMESPACE "PCGExCreatePickerConstantRange"
#define PCGEX_NAMESPACE CreatePickerConstantRange

PCGEX_PICKER_BOILERPLATE(ConstantRange, {}, {})

#if WITH_EDITOR
FString UPCGExPickerConstantRangeSettings::GetDisplayName() const
{
	FString DisplayName = TEXT("Pick [");

	if (Config.bTreatAsNormalized)
	{
		DisplayName += FString::Printf(TEXT("%.2f"), Config.RelativeStartIndex);
		DisplayName += TEXT(":");
		DisplayName += FString::Printf(TEXT("%.2f"), Config.RelativeEndIndex);
	}
	else
	{
		DisplayName += FString::Printf(TEXT("%d"), Config.DiscreteStartIndex);
		DisplayName += TEXT(":");
		DisplayName += FString::Printf(TEXT("%d"), Config.DiscreteEndIndex);
	}

	return DisplayName + TEXT("]");
}
#endif

void UPCGExPickerConstantRangeFactory::AddPicksFromConfig(const FPCGExPickerConstantRangeConfig& InConfig, int32 InNum, TSet<int32>& OutPicks)
{
	int32 TargetStartIndex = 0;
	int32 TargetEndIndex = 0;
	const int32 MaxIndex = InNum - 1;

	if (!InConfig.bTreatAsNormalized)
	{
		TargetStartIndex = InConfig.DiscreteStartIndex;
		TargetEndIndex = InConfig.DiscreteEndIndex;
	}
	else
	{
		TargetStartIndex = PCGEx::TruncateDbl(static_cast<double>(MaxIndex) * InConfig.RelativeStartIndex, InConfig.TruncateMode);
		TargetEndIndex = PCGEx::TruncateDbl(static_cast<double>(MaxIndex) * InConfig.RelativeEndIndex, InConfig.TruncateMode);
	}

	if (TargetStartIndex < 0) { TargetStartIndex = InNum + TargetStartIndex; }
	TargetStartIndex = PCGExMath::SanitizeIndex(TargetStartIndex, MaxIndex, InConfig.Safety);

	if (TargetEndIndex < 0) { TargetEndIndex = InNum + TargetEndIndex; }
	TargetEndIndex = PCGExMath::SanitizeIndex(TargetEndIndex, MaxIndex, InConfig.Safety);

	if (!FMath::IsWithin(TargetStartIndex, 0, InNum) ||
		!FMath::IsWithin(TargetEndIndex, 0, InNum)) { return; }

	if (TargetStartIndex > TargetEndIndex) { std::swap(TargetStartIndex, TargetEndIndex); }

	if ((TargetEndIndex - TargetStartIndex) == 0) { return; }

	OutPicks.Reserve(OutPicks.Num() + TargetEndIndex - TargetStartIndex + 1);
	for (int i = TargetStartIndex; i <= TargetEndIndex; i++) { OutPicks.Add(i); }
}

void UPCGExPickerConstantRangeFactory::AddPicks(const int32 InNum, TSet<int32>& OutPicks) const
{
	AddPicksFromConfig(Config, InNum, OutPicks);
}

bool UPCGExPickerConstantRangeFactory::InitInternalData(FPCGExContext* InContext)
{
	if (!Super::InitInternalData(InContext)) { return false; }
	return true;
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
