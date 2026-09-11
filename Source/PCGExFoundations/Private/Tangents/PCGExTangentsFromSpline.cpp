// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Tangents/PCGExTangentsFromSpline.h"

#include "Core/PCGExContext.h"
#include "Data/PCGExData.h"
#include "Data/PCGSplineData.h"
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS // FPCGSplineStruct
#include "Data/PCGSplineStruct.h"
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS // FPCGSplineStruct
#include "Details/PCGExSettingsDetails.h"
#include "Math/PCGExMath.h"

namespace PCGExTangentsFromSpline
{
	// Key spans below this are collapsed: both ends resolve to the same reference location.
	constexpr double KeyEpsilon = 1.0e-4;

	// Full key range of a closed loop (last key + loop offset); only meaningful when the spline is closed.
	double LoopSpan(const FPCGSplineStruct& InSpline)
	{
		return InSpline.GetInputKeyAtSegmentStart(InSpline.GetNumberOfPoints());
	}
}

#pragma region FPCGExTangentsFromSpline

bool FPCGExTangentsFromSpline::PrepareForData(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InDataFacade)
{
	if (!FPCGExTangentsOperation::PrepareForData(InContext, InDataFacade))
	{
		return false;
	}

	const int32 NumPoints = InDataFacade->GetNum();
	Samples.Init(FSample(), NumPoints);

	if (Sources.IsEmpty())
	{
		// Nothing to sample; every point uses the neighbor fallback. The factory already warned.
		return true;
	}

	TSharedPtr<PCGExDetails::TSettingValue<double>> MaxDistanceReader;
	if (bUseMaxDistance)
	{
		MaxDistanceReader = MaxDistance.GetValueSetting();
		// Whole-buffer read: this runs before any scoped fetch.
		if (!MaxDistanceReader->Init(InDataFacade, false))
		{
			return false;
		}
	}

	const TConstPCGValueRange<FTransform> InTransforms = InDataFacade->GetIn()->GetConstTransformValueRange();

	for (int32 i = 0; i < NumPoints; i++)
	{
		const FVector Location = InTransforms[i].GetLocation();
		double BestDistSquared = MaxDistanceReader ? FMath::Square(MaxDistanceReader->Read(i)) : TNumericLimits<double>::Max();
		FSample& Sample = Samples[i];

		for (int32 s = 0; s < Sources.Num(); s++)
		{
			const FPCGSplineStruct& Spline = *Sources[s];
			const float Key = Spline.FindInputKeyClosestToWorldLocation(Location);
			const double DistSquared = FVector::DistSquared(Location, Spline.GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World));

			if (DistSquared > BestDistSquared)
			{
				continue;
			}

			BestDistSquared = DistSquared;
			Sample.SourceIndex = s;
			Sample.Key = Key;
		}
	}

	return true;
}

void FPCGExTangentsFromSpline::ProcessFirstPoint(const UPCGBasePointData* InPointData, const FVector& ArriveScale, FVector& OutArrive, const FVector& LeaveScale, FVector& OutLeave) const
{
	const FSample& Sample = Samples[0];
	if (Sample.SourceIndex >= 0)
	{
		const FPCGSplineStruct& Spline = *Sources[Sample.SourceIndex];
		const double Delta = KeyDelta(Spline, Sample.Key, KeyOn(Sample.SourceIndex, 1, InPointData->GetConstTransformValueRange()));

		if (FMath::Abs(Delta) > PCGExTangentsFromSpline::KeyEpsilon)
		{
			// Single neighbour: both tangents span toward it.
			const FVector Dir = Spline.GetTangentAtSplineInputKey(Sample.Key, ESplineCoordinateSpace::World) * Delta;
			OutArrive = Dir * ArriveScale;
			OutLeave = Dir * LeaveScale;
			return;
		}
	}

	FPCGExTangentsOperation::ProcessFirstPoint(InPointData, ArriveScale, OutArrive, LeaveScale, OutLeave);
}

void FPCGExTangentsFromSpline::ProcessLastPoint(const UPCGBasePointData* InPointData, const FVector& ArriveScale, FVector& OutArrive, const FVector& LeaveScale, FVector& OutLeave) const
{
	const int32 LastIndex = InPointData->GetNumPoints() - 1;
	const FSample& Sample = Samples[LastIndex];
	if (Sample.SourceIndex >= 0)
	{
		const FPCGSplineStruct& Spline = *Sources[Sample.SourceIndex];
		const double Delta = KeyDelta(Spline, KeyOn(Sample.SourceIndex, LastIndex - 1, InPointData->GetConstTransformValueRange()), Sample.Key);

		if (FMath::Abs(Delta) > PCGExTangentsFromSpline::KeyEpsilon)
		{
			const FVector Dir = Spline.GetTangentAtSplineInputKey(Sample.Key, ESplineCoordinateSpace::World) * Delta;
			OutArrive = Dir * ArriveScale;
			OutLeave = Dir * LeaveScale;
			return;
		}
	}

	FPCGExTangentsOperation::ProcessLastPoint(InPointData, ArriveScale, OutArrive, LeaveScale, OutLeave);
}

void FPCGExTangentsFromSpline::ProcessPoint(const UPCGBasePointData* InPointData, const int32 Index, const int32 NextIndex, const int32 PrevIndex, const FVector& ArriveScale, FVector& OutArrive, const FVector& LeaveScale, FVector& OutLeave) const
{
	const TConstPCGValueRange<FTransform> InTransforms = InPointData->GetConstTransformValueRange();
	const FSample& Sample = Samples[Index];

	if (Sample.SourceIndex >= 0)
	{
		const FPCGSplineStruct& Spline = *Sources[Sample.SourceIndex];
		const double DeltaArrive = KeyDelta(Spline, KeyOn(Sample.SourceIndex, PrevIndex, InTransforms), Sample.Key);
		const double DeltaLeave = KeyDelta(Spline, Sample.Key, KeyOn(Sample.SourceIndex, NextIndex, InTransforms));

		if (FMath::Abs(DeltaArrive) > PCGExTangentsFromSpline::KeyEpsilon || FMath::Abs(DeltaLeave) > PCGExTangentsFromSpline::KeyEpsilon)
		{
			Resolve(Spline, Sample.Key, DeltaArrive, DeltaLeave, ArriveScale, OutArrive, LeaveScale, OutLeave);
			return;
		}
	}

	// No usable reference: chord through the neighbours, same as the From Neighbors module.
	const FVector Dir = (InTransforms[NextIndex].GetLocation() - InTransforms[PrevIndex].GetLocation()) * 0.5;
	OutArrive = Dir * ArriveScale;
	OutLeave = Dir * LeaveScale;
}

float FPCGExTangentsFromSpline::KeyOn(const int32 SourceIndex, const int32 PointIndex, const TConstPCGValueRange<FTransform>& InTransforms) const
{
	const FSample& Sample = Samples[PointIndex];
	if (Sample.SourceIndex == SourceIndex)
	{
		return Sample.Key;
	}

	// Neighbour snapped to another reference (or none): re-resolve it on this point's own reference.
	return Sources[SourceIndex]->FindInputKeyClosestToWorldLocation(InTransforms[PointIndex].GetLocation());
}

double FPCGExTangentsFromSpline::KeyDelta(const FPCGSplineStruct& InSpline, const float From, const float To) const
{
	const double Delta = static_cast<double>(To) - static_cast<double>(From);
	if (!InSpline.IsClosedLoop())
	{
		return Delta;
	}

	// Shortest signed span across the loop seam.
	const double Span = PCGExTangentsFromSpline::LoopSpan(InSpline);
	return PCGExMath::Tile(Delta, -Span * 0.5, Span * 0.5);
}

FVector FPCGExTangentsFromSpline::Derivative(const FPCGSplineStruct& InSpline, float Key, const bool bArriveSide) const
{
	// The arrive side reads the left limit so a reference corner (arrive != leave) keeps both directions
	// instead of leaking the leave tangent into the arrive one.
	if (bArriveSide)
	{
		Key -= static_cast<float>(PCGExTangentsFromSpline::KeyEpsilon);
		if (Key < 0)
		{
			Key = InSpline.IsClosedLoop() ? Key + static_cast<float>(PCGExTangentsFromSpline::LoopSpan(InSpline)) : 0;
		}
	}

	return InSpline.GetTangentAtSplineInputKey(Key, ESplineCoordinateSpace::World);
}

void FPCGExTangentsFromSpline::Resolve(const FPCGSplineStruct& InSpline, const float Key, double DeltaArrive, double DeltaLeave, const FVector& ArriveScale, FVector& OutArrive, const FVector& LeaveScale, FVector& OutLeave) const
{
	// A collapsed side borrows the other's span so the point keeps a continuous tangent instead of a cusp.
	if (FMath::Abs(DeltaArrive) <= PCGExTangentsFromSpline::KeyEpsilon)
	{
		DeltaArrive = DeltaLeave;
	}
	if (FMath::Abs(DeltaLeave) <= PCGExTangentsFromSpline::KeyEpsilon)
	{
		DeltaLeave = DeltaArrive;
	}

	OutArrive = Derivative(InSpline, Key, true) * DeltaArrive * ArriveScale;
	OutLeave = Derivative(InSpline, Key, false) * DeltaLeave * LeaveScale;
}

#pragma endregion

#pragma region UPCGExFromSplineTangents

void UPCGExFromSplineTangents::InitializeInContext(FPCGExContext* InContext, const FName InOverridesPinLabel)
{
	Super::InitializeInContext(InContext, InOverridesPinLabel);

	Sources.Reset();
	for (const FPCGTaggedData& TaggedData : InContext->InputData.GetInputsByPin(PCGExTangents::SourceTangentSourcesLabel))
	{
		const UPCGSplineData* SplineData = Cast<UPCGSplineData>(TaggedData.Data);
		if (!SplineData || SplineData->SplineStruct.GetNumberOfSplineSegments() <= 0)
		{
			continue;
		}

		Sources.Add(&SplineData->SplineStruct);
	}

	if (Sources.IsEmpty())
	{
		PCGE_LOG_C(Warning, GraphAndLog, InContext, FTEXT("From Spline tangents: no usable spline on the Tangent Sources pin, falling back to neighbor-based tangents."));
	}
}

void UPCGExFromSplineTangents::Cleanup()
{
	Sources.Reset();
	Super::Cleanup();
}

void UPCGExFromSplineTangents::CopySettingsFrom(const UPCGExInstancedFactory* Other)
{
	Super::CopySettingsFrom(Other);
	if (const UPCGExFromSplineTangents* TypedOther = Cast<UPCGExFromSplineTangents>(Other))
	{
		bUseMaxDistance = TypedOther->bUseMaxDistance;
		MaxDistance = TypedOther->MaxDistance;
	}
}

TSharedPtr<FPCGExTangentsOperation> UPCGExFromSplineTangents::CreateOperation() const
{
	PCGEX_FACTORY_NEW_OPERATION(TangentsFromSpline)
	NewOperation->Sources = Sources;
	NewOperation->bUseMaxDistance = bUseMaxDistance;
	NewOperation->MaxDistance = MaxDistance;
	return NewOperation;
}

#pragma endregion
