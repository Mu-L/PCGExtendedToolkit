// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/Filtering/PCGExCullOnEmpty.h"

#include "PCGContext.h"
#include "PCGParamData.h"
#include "PCGPin.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGPolyLineData.h"
#include "Metadata/PCGMetadata.h"

#define LOCTEXT_NAMESPACE "CullOnEmptyElement"

TArray<FPCGPinProperties> UPCGExCullOnEmptySettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	FPCGPinProperties& Pin = PinProperties.Emplace_GetRef(PCGPinConstants::DefaultInputLabel, FPCGDataTypeInfo::AsId());
	Pin.SetRequiredPin();
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExCullOnEmptySettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	(void)PinProperties.Emplace_GetRef(PCGPinConstants::DefaultOutputLabel, FPCGDataTypeInfo::AsId());
	return PinProperties;
}

FPCGElementPtr UPCGExCullOnEmptySettings::CreateElement() const
{
	return MakeShared<FPCGExCullOnEmptyElement>();
}

namespace PCGExCullOnEmpty
{
	static bool IsDataNonEmpty(const UPCGData* Data)
	{
		if (!Data) { return false; }

		// Point data: check point count
		if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data))
		{
			return PointData->GetNumPoints() > 0;
		}

		// Spline/polyline data: check segment count
		if (const UPCGPolyLineData* PolyLineData = Cast<UPCGPolyLineData>(Data))
		{
			return PolyLineData->GetNumSegments() > 0;
		}

		// Attribute set: check metadata entry count
		if (const UPCGParamData* ParamData = Cast<UPCGParamData>(Data))
		{
			const UPCGMetadata* Metadata = ParamData->ConstMetadata();
			return Metadata && Metadata->GetLocalItemCount() > 0;
		}

		// Other data types: valid if they exist
		return true;
	}
}

bool FPCGExCullOnEmptyElement::ExecuteInternal(FPCGContext* Context) const
{
	check(Context);

	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	bool bHasValidData = false;

	for (const FPCGTaggedData& TaggedData : Inputs)
	{
		if (PCGExCullOnEmpty::IsDataNonEmpty(TaggedData.Data))
		{
			FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(TaggedData);
			Output.Pin = PCGPinConstants::DefaultOutputLabel;
			bHasValidData = true;
		}
	}

	if (!bHasValidData)
	{
		Context->OutputData.InactiveOutputPinBitmask |= 1ULL << 0;
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
