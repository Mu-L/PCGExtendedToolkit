﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExMergeVertices.h"


#define LOCTEXT_NAMESPACE "PCGExGraphSettings"

#pragma region UPCGSettings interface

PCGExData::EIOInit UPCGExMergeVerticesSettings::GetMainOutputInitMode() const { return PCGExData::EIOInit::NoInit; }
PCGExData::EIOInit UPCGExMergeVerticesSettings::GetEdgeOutputInitMode() const { return PCGExData::EIOInit::Forward; }

#pragma endregion

void FPCGExMergeVerticesContext::ClusterProcessing_InitialProcessingDone()
{
	Merger = MakeShared<FPCGExPointIOMerger>(CompositeDataFacade.ToSharedRef());

	int32 StartOffset = 0;

	for (int i = 0; i < Batches.Num(); i++)
	{
		PCGExClusterMT::TBatch<PCGExMergeVertices::FProcessor>* Batch = static_cast<PCGExClusterMT::TBatch<PCGExMergeVertices::FProcessor>*>(Batches[i].Get());
		Merger->Append(Batch->VtxDataFacade->Source);

		for (const TSharedRef<PCGExMergeVertices::FProcessor>& Processor : Batch->Processors) { Processor->StartIndexOffset = StartOffset; }
		StartOffset += Batch->VtxDataFacade->GetNum();
	}

	Merger->MergeAsync(GetAsyncManager(), &CarryOverDetails);
	PCGExGraph::SetClusterVtx(CompositeDataFacade->Source, OutVtxId); // After merge since it forwards IDs
}

void FPCGExMergeVerticesContext::ClusterProcessing_WorkComplete()
{
	CompositeDataFacade->Write(GetAsyncManager());
}

PCGEX_INITIALIZE_ELEMENT(MergeVertices)

bool FPCGExMergeVerticesElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(MergeVertices)

	PCGEX_FWD(CarryOverDetails)
	Context->CarryOverDetails.Init();

	TSharedPtr<PCGExData::FPointIO> CompositeIO = PCGExData::NewPointIO(Context, PCGExGraph::OutputVerticesLabel, 0);
	Context->CompositeDataFacade = MakeShared<PCGExData::FFacade>(CompositeIO.ToSharedRef());
	CompositeIO->InitializeOutput<UPCGExClusterNodesData>(PCGExData::EIOInit::New);

	return true;
}

bool FPCGExMergeVerticesElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExMergeVerticesElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(MergeVertices)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartProcessingClusters<PCGExClusterMT::TBatch<PCGExMergeVertices::FProcessor>>(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries) { return true; },
			[&](const TSharedPtr<PCGExClusterMT::TBatch<PCGExMergeVertices::FProcessor>>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			}))
		{
			return Context->CancelExecution(TEXT("Could not build any clusters."));
		}
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGEx::State_Done)

	(void)Context->CompositeDataFacade->Source->StageOutput(Context);
	Context->MainEdges->StageOutputs();

	return Context->TryComplete();
}

namespace PCGExMergeVertices
{
	TSharedPtr<PCGExCluster::FCluster> FProcessor::HandleCachedCluster(const TSharedRef<PCGExCluster::FCluster>& InClusterRef)
	{
		// Create a heavy copy we'll update and forward
		return MakeShared<PCGExCluster::FCluster>(
			InClusterRef, VtxDataFacade->Source, EdgeDataFacade->Source, NodeIndexLookup,
			true, true, true);
	}

	FProcessor::~FProcessor()
	{
	}

	bool FProcessor::Process(TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExMergeVertices::Process);

		if (!FClusterProcessor::Process(InAsyncManager)) { return false; }

		Cluster->WillModifyVtxIO();

		return true;
	}

	void FProcessor::ProcessNodes(const PCGExMT::FScope& Scope)
	{
		TArray<PCGExCluster::FNode>& Nodes = *Cluster->Nodes;

		PCGEX_SCOPE_LOOP(Index)
		{
			PCGExCluster::FNode& Node = Nodes[Index];

			Node.PointIndex += StartIndexOffset;
		}
	}

	void FProcessor::ProcessEdges(const PCGExMT::FScope& Scope)
	{
		TArray<PCGExGraph::FEdge>& ClusterEdges = *Cluster->Edges;

		PCGEX_SCOPE_LOOP(Index)
		{
			PCGExGraph::FEdge& Edge = ClusterEdges[Index];

			Edge.Start += StartIndexOffset;
			Edge.End += StartIndexOffset;
		}
	}

	void FProcessor::CompleteWork()
	{
		StartParallelLoopForNodes();
		StartParallelLoopForEdges();
	}

	void FProcessor::Write()
	{
		Cluster->VtxIO = Context->CompositeDataFacade->Source;
		Cluster->NumRawVtx = Context->CompositeDataFacade->Source->GetNum(PCGExData::EIOSide::Out);

		PCGEX_INIT_IO_VOID(EdgeDataFacade->Source, PCGExData::EIOInit::Forward)

		PCGExGraph::MarkClusterEdges(EdgeDataFacade->Source, Context->OutVtxId);

		ForwardCluster();
	}
}

#undef LOCTEXT_NAMESPACE
