// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/Utils/PCGExFlatten.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "PCGData.h"
#include "Data/PCGBasePointData.h"
#include "Core/PCGExMTCommon.h"

#define LOCTEXT_NAMESPACE "FlattenElement"

TArray<FPCGPinProperties> UPCGExFlattenSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_ANY(PCGPinConstants::DefaultInputLabel, "", Required)
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExFlattenSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_ANY(PCGPinConstants::DefaultOutputLabel, "", Required)
	return PinProperties;
}

FPCGElementPtr UPCGExFlattenSettings::CreateElement() const
{
	return MakeShared<FPCGExFlattenElement>();
}

bool FPCGExFlattenElement::ExecuteInternal(FPCGContext* Context) const
{
	check(Context);

	const TArray<FPCGTaggedData> Inputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	TArray<UPCGData*> Copies;
	TArray<int32> Sources; // index into Inputs, for tag/pin propagation later
	Copies.Reserve(Inputs.Num());
	Sources.Reserve(Inputs.Num());

	for (int32 i = 0; i < Inputs.Num(); ++i)
	{
		const UPCGData* InData = Inputs[i].Data;
		if (!InData) { continue; }

		UPCGData* Copy = InData->DuplicateData(Context, /*bInitializeMetadata=*/true);
		if (!Copy) { continue; }

		Copies.Add(Copy);
		Sources.Add(i);
	}

	PCGEX_PARALLEL_FOR(Copies.Num(), Copies[i]->Flatten();)

	Context->OutputData.TaggedData.Reserve(Copies.Num());
	for (int32 i = 0; i < Copies.Num(); ++i)
	{
		FPCGTaggedData& Output = Context->OutputData.TaggedData.Add_GetRef(Inputs[Sources[i]]);
		Output.Data = Copies[i];
		Output.Pin = PCGPinConstants::DefaultOutputLabel;
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
