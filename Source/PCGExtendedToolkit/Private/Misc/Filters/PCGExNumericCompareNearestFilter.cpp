﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Misc/Filters/PCGExNumericCompareNearestFilter.h"


#define LOCTEXT_NAMESPACE "PCGExCompareFilterDefinition"
#define PCGEX_NAMESPACE CompareFilterDefinition

bool UPCGExNumericCompareNearestFilterFactory::Init(FPCGExContext* InContext)
{
	if (!Super::Init(InContext)) { return false; }

	TargetDataFacade = PCGExData::TryGetSingleFacade(InContext, PCGEx::SourceTargetsLabel, false, true);
	if (!TargetDataFacade) { return false; }

	return true;
}

TSharedPtr<PCGExPointFilter::IFilter> UPCGExNumericCompareNearestFilterFactory::CreateFilter() const
{
	return MakeShared<PCGExPointFilter::FNumericCompareNearestFilter>(this);
}

bool UPCGExNumericCompareNearestFilterFactory::RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const
{
	if (!Super::RegisterConsumableAttributesWithData(InContext, InData)) { return false; }

	FName Consumable = NAME_None;
	PCGEX_CONSUMABLE_CONDITIONAL(Config.CompareAgainst == EPCGExInputValueType::Attribute, Config.OperandB, Consumable)

	return true;
}

void UPCGExNumericCompareNearestFilterFactory::BeginDestroy()
{
	TargetDataFacade.Reset();
	Super::BeginDestroy();
}

bool PCGExPointFilter::FNumericCompareNearestFilter::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InPointDataFacade)
{
	if (!IFilter::Init(InContext, InPointDataFacade)) { return false; }

	if (!TargetDataFacade) { return false; }

	Distances = TypedFilterFactory->Config.DistanceDetails.MakeDistances();

	OperandA = TargetDataFacade->GetBroadcaster<double>(TypedFilterFactory->Config.OperandA, true);

	if (!OperandA)
	{
		PCGEX_LOG_INVALID_SELECTOR_C(InContext, "Operand A", TypedFilterFactory->Config.OperandA)
		return false;
	}

	OperandB = TypedFilterFactory->Config.GetValueSettingOperandB();
	if (!OperandB->Init(InContext, PointDataFacade, false)) { return false; }

	TargetOctree = &TargetDataFacade->GetIn()->GetPointOctree();

	return true;
}

bool PCGExPointFilter::FNumericCompareNearestFilter::Test(const int32 PointIndex) const
{
	const double B = OperandB->Read(PointIndex);

	const UPCGBasePointData* TargetsData = TargetDataFacade->GetIn();
	const PCGExData::FConstPoint SourcePt = PointDataFacade->GetInPoint(PointIndex);

	double BestDist = MAX_dbl;
	int32 TargetIndex = -1;

	TargetOctree->FindNearbyElements(
		SourcePt.GetTransform().GetLocation(), [&](const PCGPointOctree::FPointRef& PointRef)
		{
			const int32 OtherIndex = PointRef.Index;
			const double Dist = Distances->GetDistSquared(SourcePt, PCGExData::FConstPoint(TargetsData, OtherIndex));

			if (Dist > BestDist) { return; }

			BestDist = Dist;
			TargetIndex = OtherIndex;
		});

	if (TargetIndex == -1) { return false; }

	return PCGExCompare::Compare(TypedFilterFactory->Config.Comparison, OperandA->Read(TargetIndex), B, TypedFilterFactory->Config.Tolerance);
}

TArray<FPCGPinProperties> UPCGExNumericCompareNearestFilterProviderSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINT(PCGEx::SourceTargetsLabel, TEXT("Target points to read operand B from"), Required, {})
	return PinProperties;
}

PCGEX_CREATE_FILTER_FACTORY(NumericCompareNearest)

#if WITH_EDITOR
FString UPCGExNumericCompareNearestFilterProviderSettings::GetDisplayName() const
{
	FString DisplayName = PCGEx::GetSelectorDisplayName(Config.OperandA) + PCGExCompare::ToString(Config.Comparison);

	if (Config.CompareAgainst == EPCGExInputValueType::Attribute) { DisplayName += PCGEx::GetSelectorDisplayName(Config.OperandB); }
	else { DisplayName += FString::Printf(TEXT("%.3f"), (static_cast<int32>(1000 * Config.OperandBConstant) / 1000.0)); }

	return DisplayName;
}
#endif

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
