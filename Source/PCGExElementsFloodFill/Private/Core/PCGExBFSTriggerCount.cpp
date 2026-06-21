// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Core/PCGExBFSTriggerCount.h"

#include "PCGElement.h"
#include "Core/PCGExContext.h"
#include "Data/PCGExData.h"
#include "Details/PCGExSettingsDetails.h"
#include "Helpers/PCGExMetaHelpers.h"

void FPCGExBFSTriggerCountDetails::ValidateNames(FPCGExContext* InContext)
{
	PCGEX_SOFT_VALIDATE_NAME(bOutputTriggerCount, TriggerCountAttributeName, InContext)
}

bool FPCGExBFSTriggerCountDetails::InitForBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InVtxDataFacade, const TSharedRef<PCGExData::FFacade>& InSeedsDataFacade)
{
	if (!bOutputTriggerCount)
	{
		return true;
	}

	InitialValueGetter = InitialValue.GetValueSetting();
	// Force a non-scoped (full) read: the seeds facade is never Fetched here and reads span arbitrary seed indices.
	if (!InitialValueGetter->Init(InSeedsDataFacade, false))
	{
		return false;
	}

	// New init (default 0) so vtx the BFS never reaches keep a count of 0.
	Writer = InVtxDataFacade->GetWritable<int32>(TriggerCountAttributeName, 0, true, PCGExData::EBufferInit::New);
	if (!Writer)
	{
		return false;
	}

	WritePtr = StaticCastSharedPtr<PCGExData::TArrayBuffer<int32>>(Writer)->GetOutValues()->GetData();
	return true;
}

int32 FPCGExBFSTriggerCountDetails::GetInitialValue(const int32 SeedPointIndex) const
{
	return InitialValueGetter->Read(SeedPointIndex);
}
