﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExMath.h"
#include "PCGExTangentsInstancedFactory.h"

#include "PCGExTangentsFromTransform.generated.h"

class FPCGExTangentsFromTransform : public FPCGExTangentsOperation
{
public:
	EPCGExAxis Axis = EPCGExAxis::Forward;

	virtual void ProcessFirstPoint(
		const UPCGBasePointData* InPointData,
		const FVector& ArriveScale, FVector& OutArrive,
		const FVector& LeaveScale, FVector& OutLeave) const override
	{
		const TConstPCGValueRange<FTransform> InTransforms = InPointData->GetConstTransformValueRange();

		const FVector Dir = PCGExMath::GetDirection(InTransforms[0].GetRotation(), Axis) * -1;
		OutArrive = Dir * ArriveScale;
		OutLeave = Dir * LeaveScale;
	}

	virtual void ProcessLastPoint(
		const UPCGBasePointData* InPointData,
		const FVector& ArriveScale, FVector& OutArrive,
		const FVector& LeaveScale, FVector& OutLeave) const override
	{
		const TConstPCGValueRange<FTransform> InTransforms = InPointData->GetConstTransformValueRange();
		const int32 LastIndex = InPointData->GetNumPoints() - 1;

		const FVector Dir = PCGExMath::GetDirection(InTransforms[LastIndex].GetRotation(), Axis) * -1;
		OutArrive = Dir * ArriveScale;
		OutLeave = Dir * LeaveScale;
	}

	virtual void ProcessPoint(
		const UPCGBasePointData* InPointData,
		const int32 Index, const int32 NextIndex, const int32 PrevIndex,
		const FVector& ArriveScale, FVector& OutArrive,
		const FVector& LeaveScale, FVector& OutLeave) const override
	{
		const TConstPCGValueRange<FTransform> InTransforms = InPointData->GetConstTransformValueRange();

		const FVector Dir = PCGExMath::GetDirection(InTransforms[Index].GetRotation(), Axis) * -1;
		OutArrive = Dir * ArriveScale;
		OutLeave = Dir * LeaveScale;
	}
};

/**
 * 
 */
UCLASS(MinimalAPI, meta=(DisplayName = "From Transform", PCGExNodeLibraryDoc="paths/write-tangents/tangents-transform"))
class UPCGExFromTransformTangents : public UPCGExTangentsInstancedFactory
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Tangents)
	EPCGExAxis Axis = EPCGExAxis::Forward;

	virtual void CopySettingsFrom(const UPCGExInstancedFactory* Other) override
	{
		Super::CopySettingsFrom(Other);
		if (const UPCGExFromTransformTangents* TypedOther = Cast<UPCGExFromTransformTangents>(Other))
		{
			Axis = TypedOther->Axis;
		}
	}

	virtual TSharedPtr<FPCGExTangentsOperation> CreateOperation() const override
	{
		PCGEX_FACTORY_NEW_OPERATION(TangentsFromTransform)
		return NewOperation;
	}
};
