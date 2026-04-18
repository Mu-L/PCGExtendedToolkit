// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Distributions/PCGExEntryPickerOperation.h"

#include "Data/PCGExData.h"

bool FPCGExEntryPickerOperation::PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget)
{
	BindContext(InContext);
	PrimaryDataFacade = InDataFacade;
	Target = InTarget;
	return Target != nullptr;
}
