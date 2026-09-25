// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Fitting/PCGExFittingTasks.h"

#include "CoreMinimal.h"
#include "Data/PCGExPointIO.h"
#include "Fitting/PCGExFitting.h"

namespace PCGExFitting::Tasks
{
	FBox ComputeFitBounds(const UPCGBasePointData* InPointData, const bool bIgnoreBounds, const PCGExMT::FScope& InScope)
	{
		const TConstPCGValueRange<FTransform> Transforms = InPointData->GetConstTransformValueRange();
		const int32 Start = InScope.IsValid() ? InScope.Start : 0;
		const int32 End = InScope.IsValid() ? InScope.End : Transforms.Num();

		FBox Bounds = FBox(ForceInit);

		if (!bIgnoreBounds)
		{
			for (int i = Start; i < End; i++)
			{
				Bounds += InPointData->GetLocalBounds(i).TransformBy(Transforms[i]);
			}
		}
		else
		{
			for (int i = Start; i < End; i++)
			{
				Bounds += Transforms[i].GetLocation();
			}
		}

		return Bounds;
	}

	FTransformPointIO::FTransformPointIO(const int32 InTaskIndex, const TSharedPtr<PCGExData::FPointIO>& InPointIO, const TSharedPtr<PCGExData::FPointIO>& InToBeTransformedIO, FPCGExTransformDetails* InTransformDetails, bool bAllocate)
		: FPCGExIndexedTask(InTaskIndex)
		  , PointIO(InPointIO)
		  , ToBeTransformedIO(InToBeTransformedIO)
		  , TransformDetails(InTransformDetails)
	{
	}

	FTransformPointIO::FTransformPointIO(const int32 InTaskIndex, const TSharedPtr<PCGExData::FPointIO>& InPointIO, const TSharedPtr<PCGExData::FPointIO>& InToBeTransformedIO, FPCGExTransformDetails* InTransformDetails, const PCGExMT::FScope& InWriteScope, const FBox& InFitBounds)
		: FPCGExIndexedTask(InTaskIndex)
		  , PointIO(InPointIO)
		  , ToBeTransformedIO(InToBeTransformedIO)
		  , TransformDetails(InTransformDetails)
		  , WriteScope(InWriteScope)
		  , FitBounds(InFitBounds)
		  , bHasFitBounds(true)
	{
	}

	void FTransformPointIO::ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager)
	{
		UPCGBasePointData* OutPointData = ToBeTransformedIO->GetOut();
		// Ranged tasks share one pre-allocated output: an allocating getter would race on it
		TPCGValueRange<FTransform> OutTransforms = OutPointData->GetTransformValueRange(!WriteScope.IsValid());
		FTransform TargetTransform = FTransform::Identity;

		const int32 Start = WriteScope.IsValid() ? WriteScope.Start : 0;
		const int32 Count = WriteScope.IsValid() ? WriteScope.Count : OutTransforms.Num();

		FBox PointBounds = bHasFitBounds ? FitBounds : ComputeFitBounds(OutPointData, TransformDetails->bIgnoreBounds, WriteScope);
		FVector Translation = FVector::ZeroVector;

		PointBounds = PointBounds.ExpandBy(0.1); // Avoid NaN
		TransformDetails->ComputeTransform(TaskIndex, TargetTransform, PointBounds, Translation);

		const int Strategy = (TransformDetails->bInheritRotation ? 2 : 0)
			+ (TransformDetails->bInheritScale ? 1 : 0);

		switch (Strategy)
		{
		case 3: // Inherit rotation + inherit scale
			PCGEX_PARALLEL_FOR(
				Count,
				OutTransforms[Start + i] *= TargetTransform;
				)
			break;
		case 2: // Inherit rotation only
			PCGEX_PARALLEL_FOR(
				Count,
				FTransform& Transform = OutTransforms[Start + i];
				FQuat OriginalRot = Transform.GetRotation();
				Transform *= TargetTransform;
				Transform.SetRotation(OriginalRot);
				)
			break;
		case 1: // Inherit scale only
			PCGEX_PARALLEL_FOR(
				Count,
				FTransform& Transform = OutTransforms[Start + i];
				FVector OriginalScale = Transform.GetScale3D();
				Transform *= TargetTransform;
				Transform.SetScale3D(OriginalScale);
				)
			break;
		default:
			PCGEX_PARALLEL_FOR(
				Count,
				FTransform& Transform = OutTransforms[Start + i];
				Transform.SetLocation(TargetTransform.TransformPosition(Transform.GetLocation()));
				)
			break;
		}
	}
}
