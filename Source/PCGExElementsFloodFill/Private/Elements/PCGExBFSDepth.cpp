// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExBFSDepth.h"

#include "Data/PCGExData.h"
#include "Clusters/PCGExCluster.h"

#define LOCTEXT_NAMESPACE "PCGExBFSDepth"
#define PCGEX_NAMESPACE BFSDepth

#pragma region UPCGExBFSDepthSettings

PCGExData::EIOInit UPCGExBFSDepthSettings::GetMainOutputInitMode() const { return StealData == EPCGExOptionState::Enabled ? PCGExData::EIOInit::Forward : PCGExData::EIOInit::Duplicate; }
PCGExData::EIOInit UPCGExBFSDepthSettings::GetEdgeOutputInitMode() const { return PCGExData::EIOInit::Forward; }

TArray<FPCGPinProperties> UPCGExBFSDepthSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINT(PCGExCommon::Labels::SourceSeedsLabel, "Seed points used as BFS starting positions.", Required)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(BFSDepth)
PCGEX_ELEMENT_BATCH_EDGE_IMPL_ADV(BFSDepth)

#pragma endregion

#pragma region FPCGExBFSDepthElement

bool FPCGExBFSDepthElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExClustersProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(BFSDepth)
	PCGEX_FOREACH_FIELD_BFS_DEPTH(PCGEX_OUTPUT_VALIDATE_NAME)

	Context->SeedsDataFacade = PCGExData::TryGetSingleFacade(Context, PCGExCommon::Labels::SourceSeedsLabel, false, true);
	if (!Context->SeedsDataFacade) { return false; }

	return true;
}

bool FPCGExBFSDepthElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExBFSDepthElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(BFSDepth)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartProcessingClusters(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries) { return true; },
			[&](const TSharedPtr<PCGExClusterMT::IBatch>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			}))
		{
			return Context->CancelExecution(TEXT("Could not build any clusters."));
		}
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->OutputPointsAndEdges();

	return Context->TryComplete();
}

#pragma endregion

#pragma region PCGExBFSDepth::FProcessor

namespace PCGExBFSDepth
{
	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExBFSDepth::Process);

		if (!IProcessor::Process(InTaskManager)) { return false; }

		if (Context->SeedsDataFacade->GetNum() <= 0) { return false; }
		
		Depths.Init(-1, NumNodes);
		Distances.Init(-1.0, NumNodes);
		Seeded.Init(0, NumNodes);

		if (Settings->bUseOctreeSearch) { Cluster->RebuildOctree(Settings->SeedPicking.PickingMethod); }

		
		PCGEX_ASYNC_GROUP_CHKD(TaskManager, SeedPickingGroup)

		SeedPickingGroup->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE]()
		{
			PCGEX_ASYNC_THIS
			This->RunBFS();
		};

		SeedPickingGroup->OnPrepareSubLoopsCallback = [PCGEX_ASYNC_THIS_CAPTURE](const TArray<PCGExMT::FScope>& Loops)
		{
			PCGEX_ASYNC_THIS
			This->SeedNodeIndices = MakeShared<PCGExMT::TScopedArray<int32>>(Loops);
		};

		SeedPickingGroup->OnSubLoopStartCallback = [PCGEX_ASYNC_THIS_CAPTURE](const PCGExMT::FScope& Scope)
		{
			PCGEX_ASYNC_THIS

			TConstPCGValueRange<FTransform> SeedTransforms = This->Context->SeedsDataFacade->GetIn()->GetConstTransformValueRange();
			const TArray<PCGExClusters::FNode>& Nodes = *This->Cluster->Nodes.Get();

			PCGEX_SCOPE_LOOP(Index)
			{
				const FVector SeedLocation = SeedTransforms[Index].GetLocation();
				const int32 ClosestIndex = This->Cluster->FindClosestNode(SeedLocation, This->Settings->SeedPicking.PickingMethod);

				if (ClosestIndex < 0) { continue; }

				const PCGExClusters::FNode* SeedNode = &Nodes[ClosestIndex];
				if (!This->Settings->SeedPicking.WithinDistance(This->Cluster->GetPos(SeedNode), SeedLocation) ||
					FPlatformAtomics::InterlockedCompareExchange(&This->Seeded[ClosestIndex], 1, 0) == 1)
				{
					continue;
				}

				This->SeedNodeIndices->Get(Scope)->Add(ClosestIndex);
			}
		};

		

		SeedPickingGroup->StartSubLoops(Context->SeedsDataFacade->GetNum(), PCGEX_CORE_SETTINGS.ClusterDefaultBatchChunkSize);

		return true;
	}

	void FProcessor::RunBFS()
	{
		SeedNodeIndices->Collapse(CollectedSeeds);
		SeedNodeIndices.Reset();

		if (CollectedSeeds.IsEmpty())
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("A cluster could not match any seed points. Check seed positions and picking distance."));
			bIsProcessorValid = false;
			return;
		}

		const TArray<PCGExClusters::FNode>& Nodes = *Cluster->Nodes;

		// Initialize all seeds at depth 0
		TArray<int32> Queue;
		Queue.Reserve(Nodes.Num());

		for (const int32 SeedIdx : CollectedSeeds)
		{
			Depths[SeedIdx] = 0;
			Distances[SeedIdx] = 0.0;
			Queue.Add(SeedIdx);
		}

		// BFS
		int32 Head = 0;
		while (Head < Queue.Num())
		{
			const int32 CurrentIdx = Queue[Head++];
			const PCGExClusters::FNode& Current = Nodes[CurrentIdx];

			for (const PCGExGraphs::FLink& Lk : Current.Links)
			{
				if (Depths[Lk.Node] != -1) { continue; }

				const double EdgeLen = FVector::Distance(Cluster->GetPos(CurrentIdx), Cluster->GetPos(Lk.Node));
				Depths[Lk.Node] = Depths[CurrentIdx] + 1;
				Distances[Lk.Node] = Distances[CurrentIdx] + EdgeLen;
				Queue.Add(Lk.Node);
			}
		}

		// Write outputs in parallel
		StartParallelLoopForRange(Nodes.Num());
	}

	void FProcessor::ProcessRange(const PCGExMT::FScope& Scope)
	{
		const TArray<PCGExClusters::FNode>& Nodes = *Cluster->Nodes;

		PCGEX_SCOPE_LOOP(Index)
		{
			const int32 PointIndex = Nodes[Index].PointIndex;
			PCGEX_OUTPUT_VALUE(Depth, PointIndex, Depths[Index])
			PCGEX_OUTPUT_VALUE(Distance, PointIndex, Distances[Index])
		}
	}

#pragma endregion

#pragma region PCGExBFSDepth::FBatch

	FBatch::FBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FPointIO>& InVtx, TArrayView<TSharedRef<PCGExData::FPointIO>> InEdges)
		: TBatch(InContext, InVtx, InEdges)
	{
	}

	void FBatch::OnProcessingPreparationComplete()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(BFSDepth)

		{
			const TSharedRef<PCGExData::FFacade>& OutputFacade = VtxDataFacade;
			PCGEX_FOREACH_FIELD_BFS_DEPTH(PCGEX_OUTPUT_INIT)
		}

		TBatch<FProcessor>::OnProcessingPreparationComplete();
	}

	bool FBatch::PrepareSingle(const TSharedPtr<PCGExClusterMT::IProcessor>& InProcessor)
	{
		if (!TBatch<FProcessor>::PrepareSingle(InProcessor)) { return false; }

		PCGEX_TYPED_PROCESSOR

#define PCGEX_FWD_BFS(_NAME, _TYPE, _DEFAULT_VALUE) TypedProcessor->_NAME##Writer = _NAME##Writer;
		PCGEX_FOREACH_FIELD_BFS_DEPTH(PCGEX_FWD_BFS)
#undef PCGEX_FWD_BFS

		return true;
	}

	void FBatch::Write()
	{
		VtxDataFacade->WriteFastest(TaskManager);
		TBatch<FProcessor>::Write();
	}

#pragma endregion
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
