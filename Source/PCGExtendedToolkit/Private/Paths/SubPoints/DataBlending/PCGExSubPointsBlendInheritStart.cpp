﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Paths/SubPoints/DataBlending/PCGExSubPointsBlendInheritStart.h"

#include "Data/Blending/PCGExMetadataBlender.h"
#include "Sampling/PCGExGetTextureData.h"


void FPCGExSubPointsBlendInheritStart::BlendSubPoints(
	const PCGExData::FConstPoint& From, const PCGExData::FConstPoint& To,
	PCGExData::FScope& Scope, const PCGExPaths::FPathMetrics& Metrics) const
{
	TPCGValueRange<FTransform> OutTransform = Scope.Data->GetTransformValueRange(false);

	PCGEX_SCOPE_LOOP(Index)
	{
		FVector Location = OutTransform[Index].GetLocation();
		MetadataBlender->Blend(From.Index, To.Index, Index, 0);
		OutTransform[Index].SetLocation(Location);
	}
}

TSharedPtr<FPCGExSubPointsBlendOperation> UPCGExSubPointsBlendInheritStart::CreateOperation() const
{
	PCGEX_CREATE_SUBPOINTBLEND_OPERATION(InheritStart)
	return NewOperation;
}
