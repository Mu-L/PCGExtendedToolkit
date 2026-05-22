// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExPathHatch.h"

#include "PCGParamData.h"
#include "Blenders/PCGExUnionBlender.h"
#include "Core/PCGExUnionData.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExBlendingDetails.h"
#include "Helpers/PCGExArrayHelpers.h"
#include "Math/PCGExBestFitPlane.h"
#include "Math/PCGExMathDistances.h"
#include "Paths/PCGExPathsCommon.h"
#include "Paths/PCGExPathsHelpers.h"
#include "Sampling/PCGExSamplingUnionData.h"
#include "SubPoints/DataBlending/PCGExSubPointsBlendInterpolate.h"

#define LOCTEXT_NAMESPACE "PCGExPathHatchElement"
#define PCGEX_NAMESPACE PathHatch

namespace PCGExPathHatch
{
	// Tie at T=0.5 deterministically resolves to the smaller index so output ordering is stable
	// across runs regardless of edge orientation.
	FORCEINLINE int32 NearestOfTwo(const int32 A, const int32 B, const float T)
	{
		if (T < 0.5f) { return A; }
		if (T > 0.5f) { return B; }
		return FMath::Min(A, B);
	}

	FORCEINLINE bool IntersectLineSegment2D(
		const FVector2D& Origin, const FVector2D& Dir,
		const FVector2D& E0, const FVector2D& E1,
		double& OutT, float& OutU)
	{
		const FVector2D Edge = E1 - E0;
		const double Denom = FVector2D::CrossProduct(Dir, Edge);
		if (FMath::IsNearlyZero(Denom, 1e-12)) { return false; }
		const FVector2D Delta = E0 - Origin;
		const double U = FVector2D::CrossProduct(Delta, Dir) / Denom;
		if (U < 0.0 || U > 1.0) { return false; }
		OutT = FVector2D::CrossProduct(Delta, Edge) / Denom;
		OutU = static_cast<float>(U);
		return true;
	}
}

#if WITH_EDITOR
void UPCGExPathHatchSettings::PostInitProperties()
{
	if (!HasAnyFlags(RF_ClassDefaultObject) && IsInGameThread())
	{
		if (!Blending)
		{
			Blending = NewObject<UPCGExSubPointsBlendInterpolate>(this, TEXT("Blending"));
		}
	}
	Super::PostInitProperties();
}
#endif

TArray<FPCGPinProperties> UPCGExPathHatchSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_OPERATION_OVERRIDES(PCGExBlending::Labels::SourceOverridesBlendingOps)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(PathHatch)
PCGEX_ELEMENT_BATCH_POINT_IMPL(PathHatch)

bool FPCGExPathHatchElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPathProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(PathHatch)

	if (Settings->bWriteAlpha)
	{
		PCGEX_VALIDATE_NAME(Settings->AlphaAttributeName)
	}

	PCGEX_BIND_INSTANCED_FACTORY(Blending, UPCGExSubPointsBlendInstancedFactory, PCGExBlending::Labels::SourceOverridesBlendingOps)

	Context->EndpointBlending = Settings->EndpointBlending;

	Context->OutputPaths = MakeShared<PCGExData::FPointIOCollection>(Context);
	Context->OutputPaths->OutputPin = Settings->GetMainOutputPin();

	return true;
}

bool FPCGExPathHatchElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExPathHatchElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(PathHatch)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		PCGEX_ON_INVALILD_INPUTS(FTEXT("Some inputs were not closed loops with at least 3 points and were skipped."))

		if (!Context->StartBatchProcessingPoints(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				if (Entry->GetNum() < 3) { bHasInvalidInputs = true; return false; }
				if (!PCGExPaths::Helpers::GetClosedLoop(Entry)) { bHasInvalidInputs = true; return false; }
				return true;
			}, [&](const TSharedPtr<PCGExPointsMT::IBatch>& NewBatch)
			{
			}))
		{
			return Context->CancelExecution(TEXT("No valid closed-loop paths to hatch."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	(void)Context->OutputPaths->StageOutputs();
	PCGEX_OUTPUT_VALID_PATHS(MainPoints)

	return Context->TryComplete();
}

#pragma region PCGExPathHatch::FProcessor

namespace PCGExPathHatch
{
	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExPathHatch::Process);

		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!IProcessor::Process(InTaskManager)) { return false; }

		bUseSegmentCount = Settings->SegmentSpacingMode == EPCGExHatchSpacingMode::Count;
		bPerSegmentOutput = Settings->OutputMode == EPCGExHatchOutputMode::PerSegment;
		bWriteAlpha = Settings->bWriteAlpha;
		bRedistributeEvenly = Settings->bRedistributeEvenly;
		const bool bUseLineCount = Settings->LineSpacingMode == EPCGExHatchSpacingMode::Count;
		const bool bFilterSegments = Settings->bFilterSmallSegments;

		if (!Settings->AngleOffset.TryReadDataValue(PointDataFacade->Source, AngleOffsetDeg)) { return false; }
		if (!Settings->LineSpacing.TryReadDataValue(PointDataFacade->Source, LineSpacingValue)) { return false; }
		if (!Settings->SegmentSpacing.TryReadDataValue(PointDataFacade->Source, SegmentSpacingValue)) { return false; }
		if (bFilterSegments && !Settings->MinSegmentLength.TryReadDataValue(PointDataFacade->Source, MinSegmentLengthValue)) { return false; }

		Projection = Settings->ProjectionDetails;
		if (!Projection.Init(PointDataFacade)) { return false; }

		const TConstPCGValueRange<FTransform> InTransforms = PointDataFacade->GetIn()->GetConstTransformValueRange();
		const int32 NumPts = InTransforms.Num();
		Projected.SetNumUninitialized(NumPts);
		for (int32 i = 0; i < NumPts; ++i)
		{
			const FVector P = Projection.ProjectFlat(InTransforms[i].GetLocation());
			Projected[i] = FVector2D(P.X, P.Y);
		}

		if (Settings->BoxFitMode == EPCGExHatchBoxFitMode::BestFit)
		{
			const PCGExMath::FBestFitPlane Plane2D(MakeArrayView(Projected.GetData(), Projected.Num()));
			BoxAxisX = FVector2D(Plane2D.Axis[0].X, Plane2D.Axis[0].Y).GetSafeNormal();
			BoxAxisY = FVector2D(Plane2D.Axis[1].X, Plane2D.Axis[1].Y).GetSafeNormal();
			BoxCenter = FVector2D(Plane2D.Centroid.X, Plane2D.Centroid.Y);

			// Plane.Centroid is the data mean, not the OBB midpoint along oriented axes — re-center
			// onto the oriented AABB so line origins span the actual extent.
			double MinX = MAX_dbl, MaxX = -MAX_dbl;
			double MinY = MAX_dbl, MaxY = -MAX_dbl;
			for (const FVector2D& P : Projected)
			{
				const FVector2D D = P - BoxCenter;
				const double DX = FVector2D::DotProduct(D, BoxAxisX);
				const double DY = FVector2D::DotProduct(D, BoxAxisY);
				MinX = FMath::Min(MinX, DX); MaxX = FMath::Max(MaxX, DX);
				MinY = FMath::Min(MinY, DY); MaxY = FMath::Max(MaxY, DY);
			}
			BoxCenter += BoxAxisX * ((MinX + MaxX) * 0.5) + BoxAxisY * ((MinY + MaxY) * 0.5);
			BoxHalfExtents = FVector2D((MaxX - MinX) * 0.5, (MaxY - MinY) * 0.5);
		}
		else
		{
			FBox2D Bounds(ForceInit);
			for (const FVector2D& P : Projected) { Bounds += P; }
			BoxAxisX = FVector2D(1, 0);
			BoxAxisY = FVector2D(0, 1);
			BoxCenter = Bounds.GetCenter();
			BoxHalfExtents = Bounds.GetExtent();
		}

		const double AngleRad = FMath::DegreesToRadians(AngleOffsetDeg);
		const double CosA = FMath::Cos(AngleRad);
		const double SinA = FMath::Sin(AngleRad);
		LineDir2D = FVector2D(BoxAxisX.X * CosA - BoxAxisX.Y * SinA, BoxAxisX.X * SinA + BoxAxisX.Y * CosA).GetSafeNormal();
		LinePerp2D = FVector2D(-LineDir2D.Y, LineDir2D.X);

		// Closed-form perp-span: the OBB's extent along LinePerp2D is |HX·dot(X,P)| + |HY·dot(Y,P)|,
		// derived from projecting the four signed-corner offsets ±HX·X ±HY·Y onto LinePerp2D.
		const double DotXP = FVector2D::DotProduct(BoxAxisX, LinePerp2D);
		const double DotYP = FVector2D::DotProduct(BoxAxisY, LinePerp2D);
		const double PerpMax = FMath::Abs(BoxHalfExtents.X * DotXP) + FMath::Abs(BoxHalfExtents.Y * DotYP);
		const double PerpMin = -PerpMax;
		const double PerpRange = PerpMax - PerpMin;

		TArray<double> PerpOffsets;
		if (PerpRange > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			if (bUseLineCount)
			{
				const int32 N = FMath::Max(1, FMath::FloorToInt(LineSpacingValue));
				PerpOffsets.SetNumUninitialized(N);
				const double Step = PerpRange / static_cast<double>(N);
				const double StartOffset = (Settings->LineOrigin == EPCGExHatchLineOrigin::Center)
					? -PerpRange * 0.5 + Step * 0.5
					: PerpMin + Step * 0.5;
				for (int32 i = 0; i < N; ++i) { PerpOffsets[i] = StartOffset + i * Step; }
			}
			else
			{
				const double Step = FMath::Max(UE_DOUBLE_KINDA_SMALL_NUMBER, LineSpacingValue);
				if (Settings->LineOrigin == EPCGExHatchLineOrigin::Center)
				{
					const int32 HalfN = FMath::FloorToInt((PerpRange * 0.5) / Step);
					const int32 Total = HalfN * 2 + 1;
					PerpOffsets.SetNumUninitialized(Total);
					for (int32 i = -HalfN; i <= HalfN; ++i) { PerpOffsets[i + HalfN] = i * Step; }
				}
				else
				{
					const int32 N = FMath::FloorToInt(PerpRange / Step) + 1;
					PerpOffsets.SetNumUninitialized(N);
					for (int32 i = 0; i < N; ++i) { PerpOffsets[i] = PerpMin + i * Step; }
				}
			}
		}

		const int32 NumEdges = NumPts;
		Segments.Reset();
		// Tight upper bound for convex shapes (2 crossings/line); concave shapes will grow normally.
		Segments.Reserve(PerpOffsets.Num() * 2);

		TArray<FCrossing> LineCrossings;
		for (int32 LineIdx = 0; LineIdx < PerpOffsets.Num(); ++LineIdx)
		{
			LineCrossings.Reset();
			const FVector2D LineOrigin = BoxCenter + LinePerp2D * PerpOffsets[LineIdx];

			for (int32 EdgeIdx = 0; EdgeIdx < NumEdges; ++EdgeIdx)
			{
				const int32 I0 = EdgeIdx;
				const int32 I1 = (EdgeIdx + 1) % NumPts;

				double T = 0.0;
				float U = 0.0f;
				if (!IntersectLineSegment2D(LineOrigin, LineDir2D, Projected[I0], Projected[I1], T, U)) { continue; }

				FCrossing Crossing;
				Crossing.EdgeI0 = I0;
				Crossing.EdgeI1 = I1;
				Crossing.TAlongLine = T;
				Crossing.EdgeT = U;
				LineCrossings.Add(Crossing);
			}

			const int32 NumCrossings = LineCrossings.Num();
			if (NumCrossings < 2) { continue; }

			// Convex shapes hit each line exactly twice — fast-path the common case to skip the
			// generic sort's lambda and recursion overhead.
			if (NumCrossings == 2)
			{
				if (LineCrossings[1].TAlongLine < LineCrossings[0].TAlongLine)
				{
					Swap(LineCrossings[0], LineCrossings[1]);
				}
			}
			else
			{
				LineCrossings.Sort([](const FCrossing& A, const FCrossing& B) { return A.TAlongLine < B.TAlongLine; });
			}

			const int32 PairCount = NumCrossings / 2;
			for (int32 P = 0; P < PairCount; ++P)
			{
				const FCrossing& Entry = LineCrossings[P * 2];
				const FCrossing& Exit = LineCrossings[P * 2 + 1];

				FHatchSegment Seg;
				Seg.StartEdgeI0 = Entry.EdgeI0; Seg.StartEdgeI1 = Entry.EdgeI1; Seg.StartEdgeT = Entry.EdgeT;
				Seg.EndEdgeI0 = Exit.EdgeI0; Seg.EndEdgeI1 = Exit.EdgeI1; Seg.EndEdgeT = Exit.EdgeT;

				// World endpoints lerp the original world edges, not Projection.Unproject(2D crossing) —
				// the projection is rotation-only, so unprojecting (x,y,0) collapses depth and snaps
				// off-origin BestFit/LocalTangent inputs onto the world-origin plane.
				Seg.WorldStart = FMath::Lerp(InTransforms[Entry.EdgeI0].GetLocation(), InTransforms[Entry.EdgeI1].GetLocation(), static_cast<double>(Entry.EdgeT));
				Seg.WorldEnd = FMath::Lerp(InTransforms[Exit.EdgeI0].GetLocation(), InTransforms[Exit.EdgeI1].GetLocation(), static_cast<double>(Exit.EdgeT));
				Seg.Length = FVector::Distance(Seg.WorldStart, Seg.WorldEnd);

				Seg.SourceStart = NearestOfTwo(Entry.EdgeI0, Entry.EdgeI1, Entry.EdgeT);
				Seg.SourceEnd = NearestOfTwo(Exit.EdgeI0, Exit.EdgeI1, Exit.EdgeT);

				if (bFilterSegments && Seg.Length < MinSegmentLengthValue) { continue; }

				int32 NumInterior = 0;
				if (bUseSegmentCount)
				{
					NumInterior = FMath::Max(0, FMath::FloorToInt(SegmentSpacingValue));
				}
				else
				{
					const double Step = FMath::Max(UE_DOUBLE_KINDA_SMALL_NUMBER, SegmentSpacingValue);
					NumInterior = FMath::Max(0, FMath::FloorToInt(Seg.Length / Step));
				}
				Seg.NumPoints = NumInterior + 2;

				Segments.Add(MoveTemp(Seg));
			}
		}

		if (Segments.IsEmpty())
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("Hatch produced no kept segments for an input."));
		}

		return true;
	}

	void FProcessor::CompleteWork()
	{
		const int32 NumSegments = Segments.Num();
		if (NumSegments == 0) { return; }

		const bool bDoEndpointBlending = Settings->bDoEndpointBlending;

		int32 RunningTotal = 0;
		for (FHatchSegment& Seg : Segments)
		{
			Seg.OutStart = RunningTotal;
			RunningTotal += Seg.NumPoints;
		}
		const int32 MergedTotal = RunningTotal;

		// Reserve our writable attributes from blender modification.
		if (bWriteAlpha) { ProtectedAttributes.Add(Settings->AlphaAttributeName); }

		const int32 InputIO = PointDataFacade->Source->IOIndex;

		TArray<PCGExData::FWeightedPoint> WeightedPoints;
		TArray<PCGEx::FOpStats> Trackers;
		TSharedPtr<PCGExSampling::FSampingUnionData> TempUnion;
		if (bDoEndpointBlending)
		{
			TempUnion = MakeShared<PCGExSampling::FSampingUnionData>();
			TempUnion->WeightRange = -2; // pass weights verbatim — exact edge-T lerp
		}

		auto FillIdxMapping = [](TArray<int32>& IdxMapping, const int32 Base, const FHatchSegment& Seg)
		{
			IdxMapping[Base] = Seg.SourceStart;
			IdxMapping[Base + Seg.NumPoints - 1] = Seg.SourceEnd;
			const int32 NumInterior = Seg.NumPoints - 2;
			for (int32 i = 0; i < NumInterior; ++i)
			{
				const float Alpha = static_cast<float>(i + 1) / static_cast<float>(NumInterior + 1);
				IdxMapping[Base + 1 + i] = NearestOfTwo(Seg.SourceStart, Seg.SourceEnd, Alpha);
			}
		};

		auto BlendSegmentEndpoints = [&](
			const TSharedPtr<PCGExBlending::FUnionBlender>& Blender,
			const FHatchSegment& Seg,
			const int32 OutA,
			const int32 OutB)
		{
			TempUnion->Reset();
			TempUnion->AddWeighted_Unsafe(PCGExData::FElement(Seg.StartEdgeI0, InputIO), 1.0 - static_cast<double>(Seg.StartEdgeT));
			TempUnion->AddWeighted_Unsafe(PCGExData::FElement(Seg.StartEdgeI1, InputIO), static_cast<double>(Seg.StartEdgeT));
			Blender->MergeSingle(OutA, TempUnion, WeightedPoints, Trackers);

			TempUnion->Reset();
			TempUnion->AddWeighted_Unsafe(PCGExData::FElement(Seg.EndEdgeI0, InputIO), 1.0 - static_cast<double>(Seg.EndEdgeT));
			TempUnion->AddWeighted_Unsafe(PCGExData::FElement(Seg.EndEdgeI1, InputIO), static_cast<double>(Seg.EndEdgeT));
			Blender->MergeSingle(OutB, TempUnion, WeightedPoints, Trackers);
		};

		auto BuildEndpointBlender = [&](const TSharedPtr<PCGExData::FFacade>& TargetFacade) -> TSharedPtr<PCGExBlending::FUnionBlender>
		{
			TSharedPtr<PCGExBlending::FUnionBlender> Blender = MakeShared<PCGExBlending::FUnionBlender>(
				&Context->EndpointBlending,
				&Settings->EndpointCarryOver,
				PCGExMath::GetDistances());

			TArray<TSharedRef<PCGExData::FFacade>> UnionSources;
			UnionSources.Add(PointDataFacade);
			Blender->AddSources(UnionSources, &ProtectedAttributes);

			if (!Blender->Init(Context, TargetFacade, PCGExData::EProxyFlags::Direct)) { return nullptr; }
			Blender->InitTrackers(Trackers);
			return Blender;
		};

		auto BuildSubBlender = [&](const TSharedPtr<PCGExData::FFacade>& TargetFacade) -> TSharedPtr<FPCGExSubPointsBlendOperation>
		{
			TSharedPtr<FPCGExSubPointsBlendOperation> Op = Context->Blending->CreateOperation();
			Op->bClosedLoop = false;
			if (!Op->PrepareForData(Context, TargetFacade, &ProtectedAttributes)) { return nullptr; }
			return Op;
		};

		if (!bPerSegmentOutput)
		{
			MergedIO = Context->OutputPaths->Emplace_GetRef(PointDataFacade->Source, PCGExData::EIOInit::New);
			if (!MergedIO) { bIsProcessorValid = false; return; }

			UPCGBasePointData* OutPoints = MergedIO->GetOut();
			const UPCGBasePointData* InPoints = PointDataFacade->GetIn();
			PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutPoints, MergedTotal, InPoints->GetAllocatedProperties());

			TArray<int32>& IdxMapping = MergedIO->GetIdxMapping(MergedTotal);
			for (const FHatchSegment& Seg : Segments) { FillIdxMapping(IdxMapping, Seg.OutStart, Seg); }
			MergedIO->ConsumeIdxMapping(EPCGPointNativeProperties::All);

			PCGEX_MAKE_SHARED(MergedFacade, PCGExData::FFacade, MergedIO.ToSharedRef())
			MergedOutputFacade = MergedFacade;

			// Pre-create the alpha writer so the parallel pass's SetValue calls don't race on buffer creation.
			if (bWriteAlpha)
			{
				AlphaWriter = MergedOutputFacade->GetWritable<double>(Settings->AlphaAttributeName, 0.0, true, PCGExData::EBufferInit::New);
			}

			SubBlending = BuildSubBlender(MergedOutputFacade);
			if (!SubBlending) { bIsProcessorValid = false; return; }

			if (bDoEndpointBlending)
			{
				EndpointBlender = BuildEndpointBlender(MergedOutputFacade);
				if (!EndpointBlender) { bIsProcessorValid = false; return; }

				for (const FHatchSegment& Seg : Segments)
				{
					BlendSegmentEndpoints(EndpointBlender, Seg, Seg.OutStart, Seg.OutStart + Seg.NumPoints - 1);
				}
			}
		}
		else
		{
			SegmentIOs.SetNum(NumSegments);
			SegmentFacades.SetNum(NumSegments);
			SubBlendings.SetNum(NumSegments);
			if (bWriteAlpha) { SegmentAlphaWriters.SetNum(NumSegments); }
			if (bDoEndpointBlending) { EndpointBlenders.SetNum(NumSegments); }

			// EmplaceBatch returns false only on InitializeOutput rejection (cancellation): abort fully.
			if (!Context->OutputPaths->EmplaceBatch(SegmentIOs, PointDataFacade->Source, PCGExData::EIOInit::New))
			{
				bIsProcessorValid = false;
				return;
			}

			for (int32 SegIdx = 0; SegIdx < NumSegments; ++SegIdx)
			{
				const FHatchSegment& Seg = Segments[SegIdx];
				const TSharedPtr<PCGExData::FPointIO>& SegIO = SegmentIOs[SegIdx];

				PCGExPaths::Helpers::SetClosedLoop(SegIO, false);

				UPCGBasePointData* OutPoints = SegIO->GetOut();
				const UPCGBasePointData* InPoints = PointDataFacade->GetIn();
				PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutPoints, Seg.NumPoints, InPoints->GetAllocatedProperties());

				TArray<int32>& IdxMapping = SegIO->GetIdxMapping(Seg.NumPoints);
				FillIdxMapping(IdxMapping, 0, Seg);
				SegIO->ConsumeIdxMapping(EPCGPointNativeProperties::All);

				PCGEX_MAKE_SHARED(SegFacade, PCGExData::FFacade, SegIO.ToSharedRef())
				SegmentFacades[SegIdx] = SegFacade;

				if (bWriteAlpha)
				{
					SegmentAlphaWriters[SegIdx] = SegFacade->GetWritable<double>(Settings->AlphaAttributeName, 0.0, true, PCGExData::EBufferInit::New);
				}

				SubBlendings[SegIdx] = BuildSubBlender(SegFacade);
				if (!SubBlendings[SegIdx]) { bIsProcessorValid = false; return; }

				if (bDoEndpointBlending)
				{
					EndpointBlenders[SegIdx] = BuildEndpointBlender(SegFacade);
					if (!EndpointBlenders[SegIdx]) { bIsProcessorValid = false; return; }
					BlendSegmentEndpoints(EndpointBlenders[SegIdx], Seg, 0, Seg.NumPoints - 1);
				}
			}
		}

		StartParallelLoopForRange(NumSegments);
	}

	void FProcessor::ProcessRange(const PCGExMT::FScope& Scope)
	{
		PCGEX_SCOPE_LOOP(Index)
		{
			const FHatchSegment& Seg = Segments[Index];

			// Raw pointers (rather than TSharedPtr) to avoid atomic refcount touches each iteration.
			// The shared pointers held by the processor outlive the parallel scope.
			PCGExData::FFacade* OutFacade = bPerSegmentOutput ? SegmentFacades[Index].Get() : MergedOutputFacade.Get();
			FPCGExSubPointsBlendOperation* SegSubBlending = bPerSegmentOutput ? SubBlendings[Index].Get() : SubBlending.Get();
			const int32 OutBase = bPerSegmentOutput ? 0 : Seg.OutStart;

			TPCGValueRange<FTransform> OutTransforms = OutFacade->GetOut()->GetTransformValueRange(false);

			const int32 NumInterior = Seg.NumPoints - 2;
			const int32 OutA = OutBase;
			const int32 OutB = OutBase + Seg.NumPoints - 1;

			OutTransforms[OutA].SetLocation(Seg.WorldStart);
			OutTransforms[OutB].SetLocation(Seg.WorldEnd);

			double StepSize = 0.0;
			double StartOffset = 0.0;
			if (NumInterior > 0)
			{
				if (bUseSegmentCount || bRedistributeEvenly)
				{
					StepSize = Seg.Length / static_cast<double>(NumInterior + 1);
					StartOffset = StepSize;
				}
				else
				{
					StepSize = FMath::Max(UE_DOUBLE_KINDA_SMALL_NUMBER, SegmentSpacingValue);
					StartOffset = StepSize;
				}
			}

			const FVector Dir = Seg.Length > UE_DOUBLE_KINDA_SMALL_NUMBER
				? (Seg.WorldEnd - Seg.WorldStart) / Seg.Length
				: FVector::ZeroVector;

			PCGExPaths::FPathMetrics Metrics = PCGExPaths::FPathMetrics(Seg.WorldStart);
			for (int32 i = 0; i < NumInterior; ++i)
			{
				const FVector Position = Seg.WorldStart + Dir * (StartOffset + i * StepSize);
				OutTransforms[OutBase + 1 + i].SetLocation(Position);
				Metrics.Add(Position);
			}
			Metrics.Add(Seg.WorldEnd);

			if (bWriteAlpha)
			{
				// Alpha writers were pre-created in CompleteWork — no buffer-creation race here.
				PCGExData::TBuffer<double>* W = (bPerSegmentOutput ? SegmentAlphaWriters[Index] : AlphaWriter).Get();
				if (W)
				{
					W->SetValue(OutA, 0.0);
					W->SetValue(OutB, 1.0);
					for (int32 i = 0; i < NumInterior; ++i)
					{
						const float Alpha = static_cast<float>(i + 1) / static_cast<float>(NumInterior + 1);
						W->SetValue(OutBase + 1 + i, static_cast<double>(Alpha));
					}
				}
			}

			// Endpoints A and B already hold the correct attrs from CompleteWork (snap-to-nearest by
			// default, edge-T lerp if endpoint blending is enabled); SubBlending only fills interior.
			if (SegSubBlending && NumInterior > 0)
			{
				PCGExData::FScope SubScope = OutFacade->Source->GetOutScope(OutBase + 1, NumInterior);
				SegSubBlending->ProcessSubPoints(
					OutFacade->Source->GetOutPoint(OutA),
					OutFacade->Source->GetOutPoint(OutB),
					SubScope,
					Metrics);
			}
		}
	}

	void FProcessor::Write()
	{
		if (!bPerSegmentOutput)
		{
			if (MergedOutputFacade) { MergedOutputFacade->WriteFastest(TaskManager); }
		}
		else
		{
			for (const TSharedPtr<PCGExData::FFacade>& Facade : SegmentFacades)
			{
				if (Facade) { Facade->WriteFastest(TaskManager); }
			}
		}
	}
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
