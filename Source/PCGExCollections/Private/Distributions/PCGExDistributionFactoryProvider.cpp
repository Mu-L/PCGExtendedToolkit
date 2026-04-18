// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Distributions/PCGExDistributionFactoryProvider.h"

#include "Distributions/PCGExBuiltinPickerOperations.h"
#include "Distributions/PCGExEntryPickerOperation.h"
#include "Distributions/PCGExMicroEntryPickerOperation.h"

PCG_DEFINE_TYPE_INFO(FPCGExDataTypeInfoDistribution, UPCGExDistributionFactoryData)

TSharedPtr<FPCGExEntryPickerOperation> UPCGExDistributionFactoryData::CreateEntryOperation(FPCGExContext* InContext) const
{
	// Abstract -- concrete subclasses (Index / Random / WeightedRandom / user) override.
	return nullptr;
}

TSharedPtr<FPCGExMicroEntryPickerOperation> UPCGExDistributionFactoryData::CreateMicroOperation(FPCGExContext* InContext) const
{
	// Default dispatch on BaseConfig.EntryDistribution.Distribution -- concrete main-mode
	// factories rarely need to override this; it's shared across all built-in modes and
	// user-authored factories that configure micro via the standard BaseConfig path.
	switch (BaseConfig.EntryDistribution.Distribution)
	{
	case EPCGExDistribution::Index:
		{
			TSharedPtr<FPCGExMicroIndexPickerOp> Op = MakeShared<FPCGExMicroIndexPickerOp>();
			Op->IndexConfig = BaseConfig.EntryDistribution.IndexSettings;
			return Op;
		}
	case EPCGExDistribution::Random:
		return MakeShared<FPCGExMicroRandomPickerOp>();
	case EPCGExDistribution::WeightedRandom:
	default:
		return MakeShared<FPCGExMicroWeightedRandomPickerOp>();
	}
}

UPCGExFactoryData* UPCGExDistributionFactoryProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	return Super::CreateFactory(InContext, InFactory);
}
