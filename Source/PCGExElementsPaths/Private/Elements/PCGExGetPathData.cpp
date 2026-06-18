// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExGetPathData.h"

#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"

#include "PCGCommon.h"
#include "PCGContext.h"
#include "PCGData.h"
#include "PCGPin.h"
#include "Data/PCGBasePointData.h"
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS // FPCGSplineStruct
#include "Data/PCGSplineData.h"
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS // FPCGSplineStruct
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Helpers/PCGHelpers.h"                  // DefaultPCGTag

#include "Core/PCGExMTCommon.h"                  // ParallelOrSequential
#include "Data/PCGExDataHelpers.h"               // SetDataValue
#include "Data/PCGExDataTags.h"                  // FTags
#include "Details/PCGExFilterDetails.h"          // PCGEx::TagsToData
#include "Helpers/PCGExPointArrayDataHelpers.h"  // SetNumPointsAllocated
#include "Helpers/PCGExRandomHelpers.h"          // ComputeSpatialSeed
#include "Paths/PCGExPathsHelpers.h"             // SetClosedLoop

#define LOCTEXT_NAMESPACE "PCGExGetPathData"

namespace PCGExGetPathData
{
	const FName OutputPathsLabel = TEXT("Paths");
	const FName OutputSplinesLabel = TEXT("Splines");

	// One per spline component that will emit a path. Everything here is created single-threaded in
	// PrepareActor (Phase 1); FillPath (Phase 2) only writes per-point values into these pre-allocated
	// buffers, so concurrent work items never touch shared state.
	struct FPathWork
	{
		const UPCGSplineData* SplineData = nullptr;
		UPCGBasePointData* PathData = nullptr;
		FPCGMetadataAttribute<FVector>* ArriveAttr = nullptr;
		FPCGMetadataAttribute<FVector>* LeaveAttr = nullptr;
		FPCGMetadataAttribute<double>* LengthAttr = nullptr;
		FPCGMetadataAttribute<double>* AlphaAttr = nullptr;
		FPCGMetadataAttribute<int32>* PointTypeAttr = nullptr;
	};

	int32 SplinePointTypeToInt(const EInterpCurveMode Mode)
	{
		switch (Mode)
		{
		case CIM_Linear: return 0;
		case CIM_CurveAuto: return 1;
		case CIM_Constant: return 2;
		case CIM_CurveAutoClamped: return 3;
		case CIM_CurveUser: return 4;
		default: case CIM_Unknown: case CIM_CurveBreak: return -1;
		}
	}

	// Phase 2 -- runs on a worker thread. Writes only into Work's own pre-allocated buffers/attributes.
	void FillPath(const FPathWork& Work, const UPCGExGetPathDataSettings* Settings)
	{
		const FPCGSplineStruct& Spline = Work.SplineData->SplineStruct;
		const FInterpCurveVector& SplinePositions = Spline.GetSplinePointsPosition();
		const FTransform SplineTransform = Spline.GetTransform();
		const double TotalLength = Spline.GetSplineLength();
		const int32 NumSegments = Spline.GetNumberOfSplineSegments();

		UPCGBasePointData* OutData = Work.PathData;
		TPCGValueRange<FTransform> OutTransforms = OutData->GetTransformValueRange(false);
		TPCGValueRange<int32> OutSeeds = OutData->GetSeedValueRange(false);
		TPCGValueRange<int64> OutMeta = OutData->GetMetadataEntryValueRange();

		auto ApplyTransform = [&](const int32 Index, const FTransform& Transform)
		{
			if (Settings->TransformDetails.bInheritRotation && Settings->TransformDetails.bInheritScale)
			{
				OutTransforms[Index] = Transform;
			}
			else if (Settings->TransformDetails.bInheritRotation)
			{
				OutTransforms[Index].SetLocation(Transform.GetLocation());
				OutTransforms[Index].SetRotation(Transform.GetRotation());
			}
			else if (Settings->TransformDetails.bInheritScale)
			{
				OutTransforms[Index].SetLocation(Transform.GetLocation());
				OutTransforms[Index].SetScale3D(Transform.GetScale3D());
			}
			else
			{
				OutTransforms[Index].SetLocation(Transform.GetLocation());
			}

			OutSeeds[Index] = PCGExRandomHelpers::ComputeSpatialSeed(OutTransforms[Index].GetLocation());
		};

		auto WriteAttributes = [&](const int32 PointIndex, const int32 SplinePointIndex, const double LengthAtPoint)
		{
			const PCGMetadataEntryKey Key = OutMeta[PointIndex];
			if (Work.ArriveAttr) { Work.ArriveAttr->SetValue(Key, SplineTransform.TransformVector(SplinePositions.Points[SplinePointIndex].ArriveTangent)); }
			if (Work.LeaveAttr) { Work.LeaveAttr->SetValue(Key, SplineTransform.TransformVector(SplinePositions.Points[SplinePointIndex].LeaveTangent)); }
			if (Work.LengthAttr) { Work.LengthAttr->SetValue(Key, LengthAtPoint); }
			if (Work.AlphaAttr) { Work.AlphaAttr->SetValue(Key, TotalLength > 0 ? LengthAtPoint / TotalLength : 0); }
			if (Work.PointTypeAttr) { Work.PointTypeAttr->SetValue(Key, SplinePointTypeToInt(SplinePositions.Points[SplinePointIndex].InterpMode)); }
		};

		for (int32 i = 0; i < NumSegments; i++)
		{
			const double LengthAtPoint = Spline.GetDistanceAlongSplineAtSplinePoint(i);
			ApplyTransform(i, Spline.GetTransformAtDistanceAlongSpline(LengthAtPoint, ESplineCoordinateSpace::World, true));
			WriteAttributes(i, i, LengthAtPoint);
		}

		if (!Spline.bClosedLoop)
		{
			ApplyTransform(NumSegments, Spline.GetTransformAtDistanceAlongSpline(TotalLength, ESplineCoordinateSpace::World, true));
			WriteAttributes(NumSegments, NumSegments, TotalLength);
		}
	}

	// Phase 1 -- runs on the game thread. Creates every UObject + metadata structure for one actor's
	// spline components, emits outputs, and queues path-fill work items. No per-point values yet.
	void PrepareActor(FPCGDataFromActorContext* Context, const UPCGExGetPathDataSettings* Settings, AActor* Actor, TArray<FPathWork>& OutPathWork)
	{
		if (!IsValid(Actor)) { return; }

		const FSoftObjectPath ActorPath(Actor);

		TInlineComponentArray<USplineComponent*, 4> SplineComponents;
		Actor->GetComponents(SplineComponents);

		const bool bWriteAnyAttr =
			Settings->bWriteArriveTangent || Settings->bWriteLeaveTangent ||
			Settings->bWriteLengthAtPoint || Settings->bWriteAlpha || Settings->bWritePointType;

		for (USplineComponent* SplineComp : SplineComponents)
		{
			if (!SplineComp) { continue; }
			if (!Context->ComponentSelector.FilterComponent(SplineComp)) { continue; }
			// Mirror the engine getter: skip components PCG spawned (tagged DefaultPCGTag).
			if (Settings->bIgnorePCGGeneratedComponents && SplineComp->ComponentTags.Contains(PCGHelpers::DefaultPCGTag)) { continue; }

			// Build engine spline data from the raw component; this bakes the component world transform.
			UPCGSplineData* SplineData = FPCGContext::NewObject_AnyThread<UPCGSplineData>(Context);
			SplineData->Initialize(SplineComp);

			const FPCGSplineStruct& Spline = SplineData->SplineStruct;
			const int32 NumSegments = Spline.GetNumberOfSplineSegments();
			if (NumSegments <= 0) { continue; }

			if (Settings->SampleInputs == EPCGExSplineSamplingIncludeMode::ClosedLoopOnly && !Spline.bClosedLoop) { continue; }
			if (Settings->SampleInputs == EPCGExSplineSamplingIncludeMode::OpenSplineOnly && Spline.bClosedLoop) { continue; }

			// Actor ref + tags are shared by both outputs; compute once and stamp whichever are enabled.
			TSet<FString> GatheredTags;
			if (Settings->bForwardSourceTags)
			{
				for (const FName& Tag : Actor->Tags) { GatheredTags.Add(Tag.ToString()); }
				for (const FName& Tag : SplineComp->ComponentTags) { GatheredTags.Add(Tag.ToString()); }
			}
			const TSharedPtr<PCGExData::FTags> Tags = MakeShared<PCGExData::FTags>(GatheredTags);

			auto StampAndEmit = [&](UPCGData* Data, const FName Pin)
			{
				if (Settings->bWriteActorReference)
				{
					const FName ActorRefName = Settings->ActorReferenceAttributeName.IsNone() ? PCGPointDataConstants::ActorReferenceAttribute : Settings->ActorReferenceAttributeName;
					PCGExData::Helpers::SetDataValue<FSoftObjectPath>(Data, ActorRefName, ActorPath);
				}
				PCGEx::TagsToData(Data, Tags, Settings->TagsToData);

				FPCGTaggedData& Output = Context->OutputData.TaggedData.Emplace_GetRef();
				Output.Data = Data;
				Output.Pin = Pin;
				Tags->DumpTo(Output.Tags);
			};

			// Spline data is complete after Initialize; emit it directly when requested (no fill needed).
			if (Settings->bOutputSplines) { StampAndEmit(SplineData, OutputSplinesLabel); }

			if (!Settings->bOutputPaths) { continue; }

			const int32 NumPoints = Spline.bClosedLoop ? NumSegments : NumSegments + 1;

			UPCGBasePointData* PathData = FPCGContext::NewPointData_AnyThread(Context);
			PCGExPointArrayDataHelpers::SetNumPointsAllocated(
				PathData, NumPoints,
				EPCGPointNativeProperties::Transform | EPCGPointNativeProperties::Seed | EPCGPointNativeProperties::MetadataEntry);

			FPathWork Work;
			Work.SplineData = SplineData;
			Work.PathData = PathData;

			if (bWriteAnyAttr)
			{
				// Each path point needs a real metadata entry before Phase 2 can set per-point values.
				TPCGValueRange<int64> OutMeta = PathData->GetMetadataEntryValueRange();
				for (int32 i = 0; i < NumPoints; i++) { OutMeta[i] = PathData->Metadata->AddEntry(); }

				if (Settings->bWriteArriveTangent) { Work.ArriveAttr = PathData->Metadata->FindOrCreateAttribute<FVector>(Settings->ArriveTangentAttributeName, FVector::ZeroVector, false, false, true); }
				if (Settings->bWriteLeaveTangent) { Work.LeaveAttr = PathData->Metadata->FindOrCreateAttribute<FVector>(Settings->LeaveTangentAttributeName, FVector::ZeroVector, false, false, true); }
				if (Settings->bWriteLengthAtPoint) { Work.LengthAttr = PathData->Metadata->FindOrCreateAttribute<double>(Settings->LengthAtPointAttributeName, 0, false, false, true); }
				if (Settings->bWriteAlpha) { Work.AlphaAttr = PathData->Metadata->FindOrCreateAttribute<double>(Settings->AlphaAttributeName, 0, false, false, true); }
				if (Settings->bWritePointType) { Work.PointTypeAttr = PathData->Metadata->FindOrCreateAttribute<int32>(Settings->PointTypeAttributeName, 0, false, false, true); }
			}

			// Closed-loop marker + actor ref + tags are metadata structure -> set single-threaded now.
			// Emitting here is fine: Phase 2 fills PathData's value ranges in place (no realloc).
			PCGExPaths::Helpers::SetClosedLoop(PathData, Spline.bClosedLoop);
			StampAndEmit(PathData, OutputPathsLabel);

			OutPathWork.Add(Work);
		}
	}
}

#pragma region UPCGExGetPathDataSettings

#if WITH_EDITOR
FText UPCGExGetPathDataSettings::GetNodeTooltipText() const
{
	return LOCTEXT("NodeTooltip", "Reads spline components directly off the selected actors and outputs each as a path, stamped with the source actor reference. Replaces the GetSplineData -> SplineToPath flow when the actor reference matters.");
}
#endif

TArray<FPCGPinProperties> UPCGExGetPathDataSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Emplace(PCGExGetPathData::OutputPathsLabel, EPCGDataType::Point, true, true, LOCTEXT("PathsPinTooltip", "One point path per spline component."));
	PinProperties.Emplace(PCGExGetPathData::OutputSplinesLabel, EPCGDataType::PolyLine, true, true, LOCTEXT("SplinesPinTooltip", "The source spline data, stamped with the actor reference."));
	return PinProperties;
}

bool UPCGExGetPathDataSettings::IsPinStaticallyActive(const FName& PinLabel) const
{
	if (PinLabel == PCGExGetPathData::OutputPathsLabel) { return bOutputPaths; }
	if (PinLabel == PCGExGetPathData::OutputSplinesLabel) { return bOutputSplines; }
	return Super::IsPinStaticallyActive(PinLabel);
}

bool UPCGExGetPathDataSettings::IsPinUsedByNodeExecution(const UPCGPin* InPin) const
{
	if (InPin && InPin->IsOutputPin())
	{
		if (InPin->Properties.Label == PCGExGetPathData::OutputPathsLabel) { return bOutputPaths; }
		if (InPin->Properties.Label == PCGExGetPathData::OutputSplinesLabel) { return bOutputSplines; }
	}
	return Super::IsPinUsedByNodeExecution(InPin);
}

FPCGElementPtr UPCGExGetPathDataSettings::CreateElement() const
{
	return MakeShared<FPCGExGetPathDataElement>();
}

#pragma endregion

#pragma region FPCGExGetPathDataElement

void FPCGExGetPathDataElement::ProcessActors(FPCGContext* InContext, const UPCGDataFromActorSettings* InSettings, const TArray<AActor*>& FoundActors) const
{
	FPCGDataFromActorContext* Context = static_cast<FPCGDataFromActorContext*>(InContext);
	const UPCGExGetPathDataSettings* Settings = Cast<UPCGExGetPathDataSettings>(InSettings);
	check(Settings);

	// Tell the executor which output pins produced nothing (bit j == output pin index j: 0=Paths, 1=Splines).
	uint64 InactiveMask = 0;
	if (!Settings->bOutputPaths) { InactiveMask |= 1ull << 0; }
	if (!Settings->bOutputSplines) { InactiveMask |= 1ull << 1; }
	Context->OutputData.InactiveOutputPinBitmask = InactiveMask;

	if (!Settings->bOutputPaths && !Settings->bOutputSplines) { return; }

	// Phase 1 (single-threaded): create every UObject + metadata structure and emit outputs.
	// NewObject_AnyThread and metadata mutation are not safe to call from multiple threads at once.
	TArray<PCGExGetPathData::FPathWork> PathWork;
	for (AActor* Actor : FoundActors)
	{
		PCGExGetPathData::PrepareActor(Context, Settings, Actor, PathWork);
	}

	// Phase 2 (parallel): fill per-point values into the pre-allocated path buffers. Each work item owns
	// its output exclusively, so writes never collide; runs sequentially below the internal threshold.
	PCGExMT::ParallelOrSequential(
		PathWork.Num(),
		[&](const int32 i) { PCGExGetPathData::FillPath(PathWork[i], Settings); });
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
