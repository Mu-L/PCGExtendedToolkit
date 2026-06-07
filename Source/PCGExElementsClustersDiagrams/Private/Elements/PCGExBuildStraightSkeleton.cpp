// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExBuildStraightSkeleton.h"

#include "Clusters/PCGExCluster.h"
#include "Data/PCGExClusterData.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Graphs/PCGExGraph.h"
#include "Graphs/PCGExGraphBuilder.h"
#include "Helpers/PCGExPointArrayDataHelpers.h"
#include "Helpers/PCGExRandomHelpers.h"
#include "Math/Geo/PCGExStraightSkeleton.h"
#include "Paths/PCGExPathsHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExGraphs"
#define PCGEX_NAMESPACE BuildStraightSkeleton

TArray<FPCGPinProperties> UPCGExBuildStraightSkeletonSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExClusters::Labels::OutputEdgesLabel, "Point data representing skeleton edges.", Required)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(BuildStraightSkeleton)
PCGEX_ELEMENT_BATCH_POINT_IMPL(BuildStraightSkeleton)

bool FPCGExBuildStraightSkeletonElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPathProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(BuildStraightSkeleton)

	if (Settings->bWriteOffsetDistance)
	{
		PCGEX_VALIDATE_NAME(Settings->OffsetDistanceAttributeName)
	}

	return true;
}

bool FPCGExBuildStraightSkeletonElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExBuildStraightSkeletonElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(BuildStraightSkeleton)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		PCGEX_ON_INVALILD_INPUTS(FTEXT("Some inputs have less than 3 points and won't be processed."))

		if (!Context->StartBatchProcessingPoints(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				if (Entry->GetNum() < 3)
				{
					bHasInvalidInputs = true;
					return false;
				}
				return true;
			}, [&](const TSharedPtr<PCGExPointsMT::IBatch>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any valid inputs to build from."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->MainPoints->StageOutputs();
	Context->MainBatch->Output();

	return Context->TryComplete();
}

namespace PCGExBuildStraightSkeleton
{
	FProcessor::~FProcessor()
	{
	}

	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExBuildStraightSkeleton::Process);

		if (!IProcessor::Process(InTaskManager))
		{
			return false;
		}

		// Straight skeleton is only defined for closed polygons.
		if (!PCGExPaths::Helpers::GetClosedLoop(PointDataFacade->GetIn()))
		{
			PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("An input path is not a closed loop and was skipped (straight skeleton requires closed polygons)."));
			return false;
		}

		ProjectionDetails = Settings->ProjectionDetails;
		if (!ProjectionDetails.Init(PointDataFacade))
		{
			return false;
		}

		// Project the input boundary to 2D, tracking the mean plane height for the inverse transform.
		const UPCGBasePointData* InData = PointDataFacade->GetIn();
		const TConstPCGValueRange<FTransform> InTransforms = InData->GetConstTransformValueRange();
		const int32 NumIn = InTransforms.Num();

		TArray<FVector2D> Outer;
		Outer.SetNumUninitialized(NumIn);
		double SumZ = 0;
		for (int32 i = 0; i < NumIn; i++)
		{
			const FVector P = ProjectionDetails.Project(InTransforms[i].GetLocation());
			Outer[i] = FVector2D(P.X, P.Y);
			SumZ += P.Z;
		}
		const double PlaneZ = NumIn > 0 ? SumZ / NumIn : 0;

		// Solve (single loop, no holes in v1).
		const TArray<TArray<FVector2D>> NoHoles;
		PCGExMath::Geo::TStraightSkeleton2 Skeleton;
		if (!Skeleton.Process(Outer, NoHoles, Settings->MergeDistance, Settings->bIncludeContour) || Skeleton.Nodes.IsEmpty())
		{
			PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("Failed to compute a straight skeleton for an input (degenerate or self-intersecting?)."));
			return false;
		}

#if !UE_BUILD_SHIPPING
		// TEMP DIAGNOSTIC (remove after dropped-edge debugging): log the ACTUAL in-editor input polygon
		// (copy-pasteable) plus solver connectivity, so the real failing shape can be reproduced exactly.
		{
			const int32 NN = Skeleton.Nodes.Num();
			TArray<int32> Parent;
			Parent.SetNumUninitialized(NN);
			for (int32 i = 0; i < NN; i++) { Parent[i] = i; }
			auto Find = [&Parent](int32 X) -> int32 { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
			for (const PCGExMath::Geo::FStraightSkeletonEdge& SE : Skeleton.Edges) { Parent[Find(SE.A)] = Find(SE.B); }
			TSet<int32> Roots;
			for (int32 i = 0; i < NN; i++) { Roots.Add(Find(i)); }

			FString Pts;
			for (const FVector2D& P : Outer) { Pts += FString::Printf(TEXT("FVector2D(%.2f, %.2f), "), P.X, P.Y); }
			UE_LOG(LogTemp, Warning, TEXT("SSNODE closed=%d inPts=%d merge=%.3f solverNodes=%d solverEdges=%d comps=%d  OUTER={ %s }"),
			       PCGExPaths::Helpers::GetClosedLoop(PointDataFacade->GetIn()) ? 1 : 0, Outer.Num(), Settings->MergeDistance, NN, Skeleton.Edges.Num(), Roots.Num(), *Pts);
		}
#endif

		// Repurpose the source IO as the cluster vertex output with brand-new points.
		if (!PointDataFacade->Source->InitializeOutput<UPCGExClusterNodesData>(PCGExData::EIOInit::New))
		{
			return false;
		}

		const int32 NumNodes = Skeleton.Nodes.Num();
		UPCGBasePointData* OutData = PointDataFacade->GetOut();
		(void)PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutData, NumNodes, PointDataFacade->GetAllocations());

		TPCGValueRange<FTransform> OutTransforms = OutData->GetTransformValueRange(true);
		TPCGValueRange<int32> OutSeeds = OutData->GetSeedValueRange(true);

		NodeTimes.SetNumUninitialized(NumNodes);
		for (int32 i = 0; i < NumNodes; i++)
		{
			const PCGExMath::Geo::FStraightSkeletonNode& Node = Skeleton.Nodes[i];
			const FVector Pos = ProjectionDetails.Unproject(FVector(Node.Pos.X, Node.Pos.Y, PlaneZ));
			OutTransforms[i].SetLocation(Pos);
			OutSeeds[i] = PCGExRandomHelpers::ComputeSpatialSeed(Pos);
			NodeTimes[i] = Node.Time;
		}

		// Build the cluster graph from the solver's (already classified & welded) edges.
		GraphBuilder = MakeShared<PCGExGraphs::FGraphBuilder>(PointDataFacade, &Settings->GraphBuilderDetails);
		GraphBuilder->bInheritNodeData = false;

		// Compilation spatially re-sorts (and would prune) the brand-new vtx points, so a custom per-node
		// attribute written by original index after compile would misalign. OutputPointIndices captures,
		// per final point, its original node index -- letting us remap NodeTimes correctly (the same
		// mechanism Delaunay uses for site identity). Every skeleton node has edges, so no node is pruned
		// and the array fills completely.
		if (Settings->bWriteOffsetDistance)
		{
			GraphBuilder->OutputPointIndices = MakeShared<TArray<int32>>();
			GraphBuilder->OutputPointIndices->Init(-1, NumNodes);
		}

		TArray<PCGExGraphs::FEdge> GraphEdges;
		GraphEdges.SetNumUninitialized(Skeleton.Edges.Num());
		for (int32 i = 0; i < Skeleton.Edges.Num(); i++)
		{
			const PCGExMath::Geo::FStraightSkeletonEdge& SE = Skeleton.Edges[i];
			GraphEdges[i] = PCGExGraphs::FEdge(i, SE.A, SE.B);
		}
		GraphBuilder->Graph->InsertEdges(GraphEdges);

		GraphBuilder->CompileAsync(InTaskManager, false);

		return true;
	}

	void FProcessor::CompleteWork()
	{
		if (!GraphBuilder || !GraphBuilder->bCompiledSuccessfully)
		{
			bIsProcessorValid = false;
			PCGEX_CLEAR_IO_VOID(PointDataFacade->Source)
			return;
		}

		if (Settings->bWriteOffsetDistance)
		{
			const int32 OutNum = PointDataFacade->GetNum();
			const TArray<int32>& Remap = *GraphBuilder->OutputPointIndices;

			if (Remap.Num() == OutNum)
			{
				OffsetWriter = PointDataFacade->GetWritable<double>(Settings->OffsetDistanceAttributeName, 0, true, PCGExData::EBufferInit::New);
				for (int32 i = 0; i < OutNum; i++)
				{
					const int32 Orig = Remap[i];
					OffsetWriter->SetValue(i, NodeTimes.IsValidIndex(Orig) ? NodeTimes[Orig] : 0.0);
				}
			}
			else
			{
				// Layout changed unexpectedly (e.g. a node was pruned). Skip rather than write misaligned data.
				PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("Offset distance attribute skipped: output vertex layout did not match the skeleton (a node may have been pruned)."));
			}
		}
	}

	void FProcessor::Write()
	{
		PointDataFacade->WriteFastest(TaskManager);
	}

	void FProcessor::Output()
	{
		GraphBuilder->StageEdgesOutputs();
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
