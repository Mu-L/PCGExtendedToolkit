﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Edges/PCGExSubdivideEdges.h"


#include "Graph/Edges/Relaxing/PCGExRelaxClusterOperation.h"
#include "Graph/Filters/PCGExClusterFilter.h"

#define LOCTEXT_NAMESPACE "PCGExSubdivideEdges"
#define PCGEX_NAMESPACE SubdivideEdges

PCGExData::EIOInit UPCGExSubdivideEdgesSettings::GetMainOutputInitMode() const { return PCGExData::EIOInit::Duplicate; }

PCGExData::EIOInit UPCGExSubdivideEdgesSettings::GetEdgeOutputInitMode() const { return PCGExData::EIOInit::Duplicate; }

TArray<FPCGPinProperties> UPCGExSubdivideEdgesSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_OPERATION_OVERRIDES(PCGExDataBlending::SourceOverridesBlendingOps)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(SubdivideEdges)

bool FPCGExSubdivideEdgesElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(SubdivideEdges)

	if (Settings->bFlagSubVtx) { PCGEX_VALIDATE_NAME(Settings->SubVtxFlagName) }
	if (Settings->bFlagSubEdge) { PCGEX_VALIDATE_NAME(Settings->SubEdgeFlagName) }
	if (Settings->bWriteVtxAlpha) { PCGEX_VALIDATE_NAME(Settings->VtxAlphaAttributeName) }

	PCGEX_OPERATION_BIND(Blending, UPCGExSubPointsBlendInstancedFactory, PCGExDataBlending::SourceOverridesBlendingOps)

	return true;
}

bool FPCGExSubdivideEdgesElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExSubdivideEdgesElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(SubdivideEdges)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartProcessingClusters<PCGExSubdivideEdges::FBatch>(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries) { return true; },
			[&](const TSharedPtr<PCGExSubdivideEdges::FBatch>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			}))
		{
			return Context->CancelExecution(TEXT("Could not build any clusters."));
		}
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGEx::State_Done)

	Context->OutputPointsAndEdges();

	return Context->TryComplete();
}

namespace PCGExSubdivideEdges
{
	FProcessor::~FProcessor()
	{
	}

	TSharedPtr<PCGExCluster::FCluster> FProcessor::HandleCachedCluster(const TSharedRef<PCGExCluster::FCluster>& InClusterRef)
	{
		return MakeShared<PCGExCluster::FCluster>(
			InClusterRef, VtxDataFacade->Source, VtxDataFacade->Source, NodeIndexLookup,
			true, false, false);
	}

	bool FProcessor::Process(TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExSubdivideEdges::Process);

		if (!FClusterProcessor::Process(InAsyncManager)) { return false; }

		if (!DirectionSettings.InitFromParent(ExecutionContext, GetParentBatch<FBatch>()->DirectionSettings, EdgeDataFacade))
		{
			return false;
		}

		SubBlending = Context->Blending->CreateOperation();
		PCGEx::InitArray(Subdivisions, EdgeDataFacade->GetNum());

		StartParallelLoopForEdges();

		return true;
	}

	void FProcessor::ProcessEdges(const PCGExMT::FScope& Scope)
	{
		TArray<PCGExGraph::FEdge>& ClusterEdges = *Cluster->Edges;

		PCGEX_SCOPE_LOOP(Index)
		{
			PCGExGraph::FEdge& Edge = ClusterEdges[Index];

			DirectionSettings.SortEndpoints(Cluster.Get(), Edge);

			const PCGExCluster::FNode* StartNode = Cluster->GetEdgeStart(Edge);
			const PCGExCluster::FNode* EndNode = Cluster->GetEdgeEnd(Edge);

			// Create subdivision items
			FSubdivision& Sub = Subdivisions[Index];

			Sub.NumSubdivisions = 0;

			// Check if that edge should be subdivided. How depends on the test source
			// Can be:
			// - Edge start test
			// - Edge end test
			// - Edge itself test

			Sub.Start = Cluster->GetPos(StartNode);
			Sub.End = Cluster->GetPos(EndNode);
			Sub.Dist = FVector::Distance(Sub.Start, Sub.End);
		}
	}

	void FProcessor::CompleteWork()
	{
	}

	void FProcessor::Write()
	{
		FClusterProcessor::Write();
	}

	void FBatch::RegisterBuffersDependencies(PCGExData::FFacadePreloader& FacadePreloader)
	{
		TBatchWithGraphBuilder<FProcessor>::RegisterBuffersDependencies(FacadePreloader);

		PCGEX_TYPED_CONTEXT_AND_SETTINGS(SubdivideEdges)

		PCGExPointFilter::RegisterBuffersDependencies(ExecutionContext, Context->FilterFactories, FacadePreloader);
		DirectionSettings.RegisterBuffersDependencies(ExecutionContext, FacadePreloader);
	}

	void FBatch::OnProcessingPreparationComplete()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(SubdivideEdges)

		DirectionSettings = Settings->DirectionSettings;
		if (!DirectionSettings.Init(ExecutionContext, VtxDataFacade, Context->GetEdgeSortingRules()))
		{
			bIsBatchValid = false;
			return;
		}

		TBatch<FProcessor>::OnProcessingPreparationComplete();
	}
}


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
