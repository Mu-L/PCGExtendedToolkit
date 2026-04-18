// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Distributions/PCGExDistributionClassic.h"

#include "Distributions/PCGExBuiltinPickerOperations.h"

TSharedPtr<FPCGExEntryPickerOperation> UPCGExDistributionClassicFactoryData::CreateEntryOperation(FPCGExContext* InContext) const
{
	switch (Mode)
	{
	case EPCGExDistribution::Index:
		{
			TSharedPtr<FPCGExEntryIndexPickerOp> NewOp = MakeShared<FPCGExEntryIndexPickerOp>();
			NewOp->IndexConfig = IndexConfig;
			return NewOp;
		}
	case EPCGExDistribution::Random:
		return MakeShared<FPCGExEntryRandomPickerOp>();
	case EPCGExDistribution::WeightedRandom:
	default:
		return MakeShared<FPCGExEntryWeightedRandomPickerOp>();
	}
}

UPCGExFactoryData* UPCGExDistributionClassicFactoryProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExDistributionClassicFactoryData* NewFactory = InContext->ManagedObjects->New<UPCGExDistributionClassicFactoryData>();
	NewFactory->Mode = Mode;
	NewFactory->BaseConfig = BaseConfig;
	NewFactory->IndexConfig = IndexConfig;
	return Super::CreateFactory(InContext, NewFactory);
}
