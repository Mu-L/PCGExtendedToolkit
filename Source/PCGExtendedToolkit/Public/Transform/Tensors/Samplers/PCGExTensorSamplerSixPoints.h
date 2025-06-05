﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PCGExTensorSampler.h"

#include "Transform/Tensors/PCGExTensor.h"
#include "Transform/Tensors/PCGExTensorOperation.h"

#include "PCGExTensorSamplerSixPoints.generated.h"

/**
 * 
 */
UCLASS(MinimalAPI, DisplayName = "Six Points", meta=(DisplayName = "Six Points", ToolTip ="Samples the field using six points around the sampling target location, and averaging the results."))
class UPCGExTensorSamplerSixPoints : public UPCGExTensorSampler
{
	GENERATED_BODY()

public:
	virtual void CopySettingsFrom(const UPCGExInstancedFactory* Other) override;
	virtual bool PrepareForData(FPCGExContext* InContext) override;
	virtual PCGExTensor::FTensorSample Sample(const TArray<TSharedPtr<PCGExTensorOperation>>& InTensors, int32 InSeedIndex, const FTransform& InProbe, bool& OutSuccess) const override;

protected:
	const FVector Points[6] = {FVector::ForwardVector, FVector::BackwardVector, FVector::UpVector, FVector::DownVector, FVector::LeftVector, FVector::RightVector};
};
