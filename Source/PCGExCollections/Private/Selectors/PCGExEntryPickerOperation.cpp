// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Selectors/PCGExEntryPickerOperation.h"

#include "Data/PCGExData.h"

bool FPCGExEntryPickerOperation::PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection)
{
	BindContext(InContext);
	PrimaryDataFacade = InDataFacade;
	Target = InTarget;
	OwningCollection = InOwningCollection;
	return Target != nullptr;
}
