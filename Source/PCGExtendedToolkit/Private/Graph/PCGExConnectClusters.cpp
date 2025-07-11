﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExConnectClusters.h"
#include "Data/PCGExPointIOMerger.h"


#include "Geometry/PCGExGeoDelaunay.h"

#define LOCTEXT_NAMESPACE "PCGExConnectClusters"
#define PCGEX_NAMESPACE ConnectClusters

PCGExData::EIOInit UPCGExConnectClustersSettings::GetMainOutputInitMode() const { return PCGExData::EIOInit::NoInit; }
PCGExData::EIOInit UPCGExConnectClustersSettings::GetEdgeOutputInitMode() const { return PCGExData::EIOInit::NoInit; }

PCGEX_INITIALIZE_ELEMENT(ConnectClusters)

TArray<FPCGPinProperties> UPCGExConnectClustersSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	if (BridgeMethod == EPCGExBridgeClusterMethod::Filters)
	{
		PCGEX_PIN_FACTORIES(PCGExGraph::SourceFilterGenerators, "Nodes that don't meet requirements won't generate connections", Required, {})
		PCGEX_PIN_FACTORIES(PCGExGraph::SourceFilterConnectables, "Nodes that don't meet requirements can't receive connections", Required, {})
	}

	return PinProperties;
}

bool FPCGExConnectClustersElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(ConnectClusters)

	PCGEX_FWD(CarryOverDetails)
	Context->CarryOverDetails.Init();

	PCGEX_FWD(ProjectionDetails)
	PCGEX_FWD(GraphBuilderDetails)

	if (Settings->BridgeMethod == EPCGExBridgeClusterMethod::Filters)
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Bridge through filter is not implemented yet!"));
		return false;

		/*
		if (!GetInputFactories(
			Context, PCGExGraph::SourceFilterGenerators, Context->GeneratorsFiltersFactories,
			PCGExFactories::ClusterNodeFilters, true)) { return false; }

		if (!GetInputFactories(
			Context, PCGExGraph::SourceFilterConnectables, Context->ConnectablesFiltersFactories,
			PCGExFactories::ClusterNodeFilters, true)) { return false; }
		*/
	}

	return true;
}

bool FPCGExConnectClustersElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExConnectClustersElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(ConnectClusters)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartProcessingClusters<PCGExBridgeClusters::FBatch>(
			[&](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries)
			{
				if (Entries->Entries.Num() == 1)
				{
					// No clusters to consolidate, just dump existing points
					Context->CurrentIO->InitializeOutput(PCGExData::EIOInit::Forward);
					Entries->Entries[0]->InitializeOutput(PCGExData::EIOInit::Forward);
					return false;
				}

				return true;
			},
			[&](const TSharedPtr<PCGExBridgeClusters::FBatch>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			}))
		{
			if (!Settings->bQuietNoBridgeWarning) { PCGE_LOG(Warning, GraphAndLog, FTEXT("No bridge was created.")); }

			for (const TSharedPtr<PCGExData::FPointIO>& Vtx : Context->MainPoints->Pairs) { Vtx->InitializeOutput(PCGExData::EIOInit::Forward); }
			for (const TSharedPtr<PCGExData::FPointIO>& Edges : Context->MainEdges->Pairs) { Edges->InitializeOutput(PCGExData::EIOInit::Forward); }

			Context->OutputPointsAndEdges();
			return Context->TryComplete(true);
		}
	}


	PCGEX_CLUSTER_BATCH_PROCESSING(PCGEx::State_Done)

	for (const TSharedPtr<PCGExClusterMT::IClusterProcessorBatch>& Batch : Context->Batches)
	{
		const TSharedPtr<PCGExBridgeClusters::FBatch> BridgeBatch = StaticCastSharedPtr<PCGExBridgeClusters::FBatch>(Batch);
		PCGExData::DataIDType PairId;
		PCGExGraph::SetClusterVtx(BridgeBatch->VtxDataFacade->Source, PairId);
		PCGExGraph::MarkClusterEdges(BridgeBatch->CompoundedEdgesDataFacade->Source, PairId);
	}

	Context->OutputPointsAndEdges();

	return Context->TryComplete();
}

namespace PCGExBridgeClusters
{
	bool FProcessor::Process(TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExBridgeClusters::Process);

		if (!IClusterProcessor::Process(InAsyncManager)) { return false; }

		return true;
	}

	void FProcessor::CompleteWork()
	{
		// if mode == filter, loop through generators and find all suitable connectables
	}

	//////// BATCH

	FBatch::FBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FPointIO>& InVtx, const TArrayView<TSharedRef<PCGExData::FPointIO>> InEdges):
		TBatch(InContext, InVtx, InEdges)
	{
		InVtx->InitializeOutput(PCGExData::EIOInit::Duplicate);
	}

	void FBatch::Process()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectClusters)

		const TSharedPtr<PCGExData::FPointIO> ConsolidatedEdges = Context->MainEdges->Emplace_GetRef(PCGExData::EIOInit::New);
		CompoundedEdgesDataFacade = MakeShared<PCGExData::FFacade>(ConsolidatedEdges.ToSharedRef());


		// Start merging right away
		Merger = MakeShared<FPCGExPointIOMerger>(CompoundedEdgesDataFacade.ToSharedRef());
		Merger->Append(Edges);
		Merger->MergeAsync(AsyncManager, &Context->CarryOverDetails);

		TBatch<FProcessor>::Process();
	}

	bool FBatch::PrepareSingle(const TSharedPtr<FProcessor>& ClusterProcessor)
	{
		CompoundedEdgesDataFacade->Source->Tags->Append(ClusterProcessor->EdgeDataFacade->Source->Tags.ToSharedRef());
		return true;
	}

	void FBatch::CompleteWork()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectClusters)

		const int32 NumValidClusters = GatherValidClusters();

		if (Processors.Num() != NumValidClusters)
		{
			PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("Some vtx/edges groups have invalid clusters. Make sure to sanitize the input first."));
		}

		if (ValidClusters.IsEmpty()) { return; } // Skip work completion entirely

		CompoundedEdgesDataFacade->WriteFastest(AsyncManager); // Write base attributes value while finding bridges

		const int32 NumBounds = ValidClusters.Num();
		EPCGExBridgeClusterMethod SafeMethod = Settings->BridgeMethod;

		if (NumBounds <= 4)
		{
			if (SafeMethod == EPCGExBridgeClusterMethod::Delaunay3D) { SafeMethod = EPCGExBridgeClusterMethod::MostEdges; }
		}
		else if (NumBounds <= 3)
		{
			if (SafeMethod == EPCGExBridgeClusterMethod::Delaunay2D) { SafeMethod = EPCGExBridgeClusterMethod::MostEdges; }
		}

		// First find which cluster are connected

		TArray<FBox> Bounds;
		PCGEx::InitArray(Bounds, NumBounds);
		for (int i = 0; i < NumBounds; i++) { Bounds[i] = ValidClusters[i]->Bounds; }

		if (SafeMethod == EPCGExBridgeClusterMethod::Delaunay3D)
		{
			const TUniquePtr<PCGExGeo::TDelaunay3> Delaunay = MakeUnique<PCGExGeo::TDelaunay3>();

			TArray<FVector> Positions;
			Positions.SetNum(NumBounds);

			for (int i = 0; i < NumBounds; i++) { Positions[i] = Bounds[i].GetCenter(); }

			if (Delaunay->Process<false, false>(Positions)) { Bridges.Append(Delaunay->DelaunayEdges); }
			else { PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("Delaunay 3D failed. Are points coplanar? If so, use Delaunay 2D instead.")); }

			Positions.Empty();
		}
		else if (SafeMethod == EPCGExBridgeClusterMethod::Delaunay2D)
		{
			const TUniquePtr<PCGExGeo::TDelaunay2> Delaunay = MakeUnique<PCGExGeo::TDelaunay2>();

			TArray<FVector> Positions;
			Positions.SetNum(NumBounds);

			for (int i = 0; i < NumBounds; i++) { Positions[i] = Bounds[i].GetCenter(); }

			if (Delaunay->Process(Positions, Context->ProjectionDetails)) { Bridges.Append(Delaunay->DelaunayEdges); }
			else { PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("Delaunay 2D failed.")); }

			Positions.Empty();
		}
		else if (SafeMethod == EPCGExBridgeClusterMethod::LeastEdges)
		{
			TSet<int32> VisitedEdges;
			for (int i = 0; i < NumBounds; i++)
			{
				VisitedEdges.Add(i); // As to not connect to self or already connected
				double Distance = MAX_dbl;
				int32 ClosestIndex = -1;

				for (int j = 0; j < NumBounds; j++)
				{
					if (i == j || VisitedEdges.Contains(j)) { continue; }

					if (const double Dist = FVector::DistSquared(Bounds[i].GetCenter(), Bounds[j].GetCenter());
						Dist < Distance)
					{
						ClosestIndex = j;
						Distance = Dist;
					}
				}

				if (ClosestIndex == -1) { continue; }

				Bridges.Add(PCGEx::H64(i, ClosestIndex));
			}
		}
		else if (SafeMethod == EPCGExBridgeClusterMethod::MostEdges)
		{
			for (int i = 0; i < NumBounds; i++)
			{
				for (int j = 0; j < NumBounds; j++)
				{
					if (i == j) { continue; }
					Bridges.Add(PCGEx::H64U(i, j));
				}
			}
		}
		else if (SafeMethod == EPCGExBridgeClusterMethod::Filters)
		{
			// Let cluster processor handle it.
		}
	}

	void FBatch::Write()
	{
		BridgesList = Bridges.Array();
		NewEdges.SetNum(Bridges.Num());

		const int32 NumBridges = Bridges.Num();
		UPCGBasePointData* EdgeData = CompoundedEdgesDataFacade->Source->GetOut();
		EdgeData->SetNumPoints(EdgeData->GetNumPoints() + Bridges.Num());

		TPCGValueRange<int64> MetadataEntries = EdgeData->GetMetadataEntryValueRange();
		for (int i = 0; i < NumBridges; i++)
		{
			const int32 EdgeIndex = EdgeData->GetNumPoints() - (NumBridges - i);
			NewEdges[i] = EdgeIndex;
			EdgeData->Metadata->InitializeOnSet(MetadataEntries[EdgeIndex]);
		}

		PCGEX_ASYNC_GROUP_CHKD_VOID(AsyncManager, BuildBridges)
		BuildBridges->OnIterationCallback =
			[PCGEX_ASYNC_THIS_CAPTURE](const int32 Index, const PCGExMT::FScope& Scope)
			{
				PCGEX_ASYNC_THIS

				uint32 Start;
				uint32 End;
				PCGEx::H64(This->BridgesList[Index], Start, End);
				This->CreateBridge(This->NewEdges[Index], Start, End);
			};

		BuildBridges->StartIterations(Bridges.Num(), 1);
	}


	void FBatch::CreateBridge(const int32 EdgeIndex, const int32 FromClusterIndex, const int32 ToClusterIndex)
	{
		const TSharedPtr<PCGExCluster::FCluster> ClusterA = ValidClusters[FromClusterIndex];
		const TSharedPtr<PCGExCluster::FCluster> ClusterB = ValidClusters[ToClusterIndex];

		int32 IndexA = -1;
		int32 IndexB = -1;

		double Distance = MAX_dbl;

		const TArray<PCGExCluster::FNode>& NodesRefA = *ClusterA->Nodes;
		const TArray<PCGExCluster::FNode>& NodesRefB = *ClusterB->Nodes;

		//Brute force find closest points
		for (const PCGExCluster::FNode& Node : NodesRefA)
		{
			FVector NodePos = ClusterA->GetPos(Node);
			const PCGExCluster::FNode& OtherNode = NodesRefB[ClusterB->FindClosestNode(NodePos)];

			if (const double Dist = FVector::DistSquared(NodePos, ClusterB->GetPos(OtherNode));
				Dist < Distance)
			{
				IndexA = Node.PointIndex;
				IndexB = OtherNode.PointIndex;
				Distance = Dist;
			}
		}

		UPCGMetadata* EdgeMetadata = CompoundedEdgesDataFacade->GetOut()->Metadata;
		const TSharedRef<PCGExData::FPointIO>& VtxIO = VtxDataFacade->Source;

		const FPCGMetadataAttribute<int64>* InVtxEndpointAtt = static_cast<FPCGMetadataAttribute<int64>*>(VtxIO->GetIn()->Metadata->GetMutableAttribute(PCGExGraph::Attr_PCGExVtxIdx));

		TConstPCGValueRange<int64> VtxMetadataEntries = VtxIO->GetOut()->GetConstMetadataEntryValueRange();
		TConstPCGValueRange<FTransform> VtxTransforms = VtxIO->GetOut()->GetConstTransformValueRange();

		TPCGValueRange<int64> EdgeMetadataEntries = CompoundedEdgesDataFacade->GetOut()->GetMetadataEntryValueRange(false);
		TPCGValueRange<FTransform> EdgeTransforms = CompoundedEdgesDataFacade->GetOut()->GetTransformValueRange(false);

		EdgeTransforms[EdgeIndex].SetLocation(FMath::Lerp(VtxTransforms[IndexA].GetLocation(), VtxTransforms[IndexB].GetLocation(), 0.5));

		uint32 StartIdx;
		uint32 StartNumEdges;

		uint32 EndIdx;
		uint32 EndNumEdges;

		PCGEx::H64(InVtxEndpointAtt->GetValueFromItemKey(VtxMetadataEntries[IndexA]), StartIdx, StartNumEdges);
		PCGEx::H64(InVtxEndpointAtt->GetValueFromItemKey(VtxMetadataEntries[IndexB]), EndIdx, EndNumEdges);

		FPCGMetadataAttribute<int64>* EdgeEndpointsAtt = static_cast<FPCGMetadataAttribute<int64>*>(EdgeMetadata->GetMutableAttribute(PCGExGraph::Attr_PCGExEdgeIdx));
		FPCGMetadataAttribute<int64>* OutVtxEndpointAtt = static_cast<FPCGMetadataAttribute<int64>*>(VtxIO->GetOut()->Metadata->GetMutableAttribute(PCGExGraph::Attr_PCGExVtxIdx));

		EdgeEndpointsAtt->SetValue(EdgeMetadataEntries[EdgeIndex], PCGEx::H64(StartIdx, EndIdx));
		OutVtxEndpointAtt->SetValue(VtxMetadataEntries[IndexA], PCGEx::H64(StartIdx, StartNumEdges + 1));
		OutVtxEndpointAtt->SetValue(VtxMetadataEntries[IndexB], PCGEx::H64(EndIdx, EndNumEdges + 1));
	}
}


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
