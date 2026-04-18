// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Distributions/PCGExDistributionFactoryProvider.h"

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
	// Concrete micro picker ops are provided once the built-in micro factories land in the next commit.
	// Until then, nullptr is safe -- the helper treats it as "no micro picking needed" for non-mesh collections.
	return nullptr;
}

UPCGExFactoryData* UPCGExDistributionFactoryProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	return Super::CreateFactory(InContext, InFactory);
}
