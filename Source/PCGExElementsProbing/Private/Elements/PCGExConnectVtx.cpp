// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExConnectVtx.h"

#include "PCGExH.h"
#include "Clusters/PCGExCluster.h"
#include "Core/PCGExClusterFilter.h"
#include "Core/PCGExFilterTypeSets.h"
#include "Core/PCGExPointFilter.h"
#include "Core/PCGExProbeFactoryProvider.h"
#include "Core/PCGExProbeOperation.h"
#include "Core/PCGExProbingEngine.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExSettingsDetails.h"
#include "Graphs/PCGExGraphPatcher.h"

#define LOCTEXT_NAMESPACE "PCGExConnectVtx"
#define PCGEX_NAMESPACE ConnectVtx

PCGExData::EIOInit UPCGExConnectVtxSettings::GetMainOutputInitMode() const
{
	return PCGExData::EIOInit::NoInit;
}

PCGExData::EIOInit UPCGExConnectVtxSettings::GetEdgeOutputInitMode() const
{
	return PCGExData::EIOInit::NoInit;
}

TArray<FPCGPinProperties> UPCGExConnectVtxSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_FACTORIES(PCGExClusters::Labels::SourceProbesLabel, "Probes used to connect vtx", Required, FPCGExDataTypeInfoProbe::AsId())

	PCGEX_PIN_FILTERS(PCGExClusters::Labels::SourceFilterGenerators, "Vtx that don't meet requirements won't generate connections. Supports both point & vtx filters.", Normal)
	PCGEX_PIN_FILTERS(PCGExClusters::Labels::SourceFilterConnectables, "Vtx that don't meet requirements can't receive connections. Supports both point & vtx filters.", Normal)

	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(ConnectVtx)
PCGEX_ELEMENT_BATCH_EDGE_IMPL_ADV(ConnectVtx)

FPCGExConnectVtxContext::~FPCGExConnectVtxContext()
{
}

bool FPCGExConnectVtxElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExClustersProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(ConnectVtx)

	if (!PCGExFactories::GetInputFactories<UPCGExProbeFactoryData>(Context, PCGExClusters::Labels::SourceProbesLabel, Context->ProbeFactories, {FPCGExDataTypeInfoProbe::AsId()}))
	{
		return false;
	}

	PCGExFactories::GetInputFactories(Context, PCGExClusters::Labels::SourceFilterGenerators, Context->GeneratorsFiltersFactories, PCGExFactories::ClusterNodeFilters(), false);
	PCGExFactories::GetInputFactories(Context, PCGExClusters::Labels::SourceFilterConnectables, Context->ConnectablesFiltersFactories, PCGExFactories::ClusterNodeFilters(), false);

	Context->CWCoincidenceTolerance = FVector(Settings->CoincidenceTolerance);

	PCGEX_FWD(CarryOverDetails)
	Context->CarryOverDetails.Init();

	PCGEX_FWD(VtxCarryOverDetails)
	Context->VtxCarryOverDetails.Init();

	// The relation dropdown is hidden under Cluster scope (same-cluster is enforced there), so a
	// stale value is ignored rather than rejected. Cross-Data is selectable under Vtx Group scope
	// but unsatisfiable (a group is a single vtx dataset) - reject it.
	if (Settings->EdgeRelation == EPCGExConnectVtxEdgeRelation::CrossData && Settings->Scope != EPCGExConnectVtxScope::All)
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Cross-Data edge relation requires the All Inputs scope."));
		return false;
	}

	if (Settings->bFlagVtxConnector)
	{
		PCGEX_VALIDATE_NAME(Settings->VtxConnectorFlagName)
	}
	if (Settings->bFlagEdgeConnector)
	{
		PCGEX_VALIDATE_NAME(Settings->EdgeConnectorFlagName)
	}

	return true;
}

bool FPCGExConnectVtxElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExConnectVtxElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(ConnectVtx)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		const bool bWantsWriteStep = Settings->Scope != EPCGExConnectVtxScope::All;
		if (!Context->StartProcessingClusters(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries)
			{
				return true;
			}, [bWantsWriteStep](const TSharedPtr<PCGExClusterMT::IBatch>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = bWantsWriteStep;
			}))
		{
			return Context->CancelExecution(TEXT("Could not build any clusters."));
		}
	}

	if (Settings->Scope == EPCGExConnectVtxScope::All)
	{
		PCGEX_CLUSTER_BATCH_PROCESSING(PCGExConnectVtx::State_LaunchingMerge)

		PCGEX_ON_STATE(PCGExConnectVtx::State_LaunchingMerge)
		{
			if (!LaunchVtxMerge(Context, Settings))
			{
				return true;
			}
		}

		PCGEX_ON_ASYNC_STATE_READY(PCGExConnectVtx::State_MergingVtx)
		{
			if (!LaunchProbing(Context, Settings))
			{
				return true;
			}
		}

		PCGEX_ON_ASYNC_STATE_READY(PCGExConnectVtx::State_Probing)
		{
			if (!LaunchPatching(Context, Settings))
			{
				return true;
			}
		}

		PCGEX_ON_ASYNC_STATE_READY(PCGExConnectVtx::State_Patching)
		{
			if (!CommitAndOutput(Context, Settings))
			{
				return true;
			}
		}

		return Context->TryComplete();
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->OutputPointsAndEdges();

	return Context->TryComplete();
}

bool FPCGExConnectVtxElement::LaunchVtxMerge(FPCGExConnectVtxContext* Context, const UPCGExConnectVtxSettings* Settings) const
{
	using namespace PCGExConnectVtx;

	Context->VtxMerger = MakeShared<PCGExGraphs::FVtxMerger>();

	int32 ClusterIdOffset = 0;

	for (const TSharedPtr<PCGExClusterMT::IBatch>& Batch : Context->Batches)
	{
		const TSharedPtr<FBatch> TypedBatch = StaticCastSharedPtr<FBatch>(Batch);

		if (!TypedBatch->bIsBatchValid || TypedBatch->ValidClusters.IsEmpty())
		{
			// No usable clusters: this group can't join the merge, forward it untouched.
			TypedBatch->VtxDataFacade->Source->InitializeOutput(PCGExData::EIOInit::Forward);
			for (const TSharedPtr<PCGExData::FPointIO>& EdgesIO : TypedBatch->Edges)
			{
				EdgesIO->InitializeOutput(PCGExData::EIOInit::Forward);
			}
			continue;
		}

		const int32 SourceIndex = Context->VtxMerger->AddSource(TypedBatch->VtxDataFacade->Source);
		TypedBatch->MergeOffset = Context->VtxMerger->GetOffset(SourceIndex);
		TypedBatch->GroupIdOffset = ClusterIdOffset;
		ClusterIdOffset += TypedBatch->ValidClusters.Num();

		Context->ValidBatches.Add(TypedBatch);
	}

	if (Context->ValidBatches.IsEmpty())
	{
		if (!Settings->bQuietNoConnectionWarning)
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("No valid clusters to connect."));
		}
		Context->OutputPointsAndEdges();
		Context->Done();
		return true;
	}

	Context->VtxMerger->MergeAsync(Context, Context->GetTaskManager(), &Context->VtxCarryOverDetails);
	Context->SetState(State_MergingVtx);

	return true;
}

bool FPCGExConnectVtxElement::LaunchProbing(FPCGExConnectVtxContext* Context, const UPCGExConnectVtxSettings* Settings) const
{
	using namespace PCGExConnectVtx;

	Context->MergedFacade = Context->VtxMerger->Finalize(Context, PCGExClusters::Labels::OutputVerticesLabel);
	if (!Context->MergedFacade)
	{
		Context->CancelExecution(TEXT("Vtx merge failed."));
		return false;
	}

	Context->Engine = MakeShared<PCGExProbing::FProbingEngine>(Context->MergedFacade.ToSharedRef());
	Context->Engine->SetCoincidence(Settings->bPreventCoincidence, Context->CWCoincidenceTolerance);
	if (Settings->bProjectPoints)
	{
		Context->Engine->SetProjection(Settings->ProjectionDetails);
	}

	if (!Context->Engine->Init(Context, Context->ProbeFactories))
	{
		Context->CancelExecution(TEXT("No probe could be initialized."));
		return false;
	}

	// Assemble the merged-domain masks, group ids & existing-edge hashes from the per-batch data.
	const bool bWantsGroupIds = Settings->EdgeRelation != EPCGExConnectVtxEdgeRelation::Any;
	if (bWantsGroupIds)
	{
		Context->GroupIds.Init(INDEX_NONE, Context->MergedFacade->GetNum());
	}

	for (int32 b = 0; b < Context->ValidBatches.Num(); ++b)
	{
		const TSharedPtr<FBatch>& Batch = Context->ValidBatches[b];
		const int32 Offset = Batch->MergeOffset;
		const int32 Num = Batch->VtxDataFacade->GetNum();

		FMemory::Memcpy(Context->Engine->CanGenerate.GetData() + Offset, Batch->CanGenerateCache->GetData(), Num);
		FMemory::Memcpy(Context->Engine->AcceptConnections.GetData() + Offset, Batch->AcceptConnectionsCache->GetData(), Num);

		if (Settings->EdgeRelation == EPCGExConnectVtxEdgeRelation::CrossCluster)
		{
			for (int32 i = 0; i < Num; ++i)
			{
				const int32 ClusterId = Batch->ClusterIds[i];
				Context->GroupIds[Offset + i] = ClusterId == INDEX_NONE ? INDEX_NONE : Batch->GroupIdOffset + ClusterId;
			}
		}
		else if (Settings->EdgeRelation == EPCGExConnectVtxEdgeRelation::CrossData)
		{
			for (int32 i = 0; i < Num; ++i)
			{
				Context->GroupIds[Offset + i] = b;
			}
		}

		Context->ExistingEdges.Reserve(Context->ExistingEdges.Num() + Batch->ExistingEdges.Num());
		for (const uint64 E : Batch->ExistingEdges)
		{
			uint32 A;
			uint32 B;
			PCGEx::H64(E, A, B);
			Context->ExistingEdges.Add(PCGEx::H64U(A + Offset, B + Offset));
		}
	}

	if (bWantsGroupIds)
	{
		Context->Engine->PointGroupIds = &Context->GroupIds;
		Context->Engine->EdgeRelation = PCGExProbing::EEdgeRelation::DifferentGroup;
	}

	Context->Engine->PrepareWorkingData();

	const TSharedPtr<PCGExMT::FTaskManager> TaskManager = Context->GetTaskManager();

	if (Context->Engine->HasLocalWork())
	{
		PCGEX_ASYNC_GROUP_CHKD(TaskManager, ProbingLoopTask)

		ProbingLoopTask->OnPrepareSubLoopsCallback = [Ctx = Context](const TArray<PCGExMT::FScope>& Loops)
		{
			Ctx->Engine->PrepareScopes(Loops);
		};

		ProbingLoopTask->OnSubLoopStartCallback = [Ctx = Context](const PCGExMT::FScope& Scope)
		{
			Ctx->Engine->ProcessScope(Scope);
		};

		ProbingLoopTask->StartSubLoops(Context->Engine->GetNumIterations(), PCGEX_CORE_SETTINGS.GetPointsBatchChunkSize());
	}

	if (Context->Engine->HasGlobalWork())
	{
		PCGEX_ASYNC_GROUP_CHKD(TaskManager, GlobalOpsTasks)

		for (FPCGExProbeOperation* Operation : Context->Engine->GetGlobalOperations())
		{
			GlobalOpsTasks->AddSimpleCallback([Ctx = Context, Op = Operation]()
			{
				TSet<uint64> LocalEdges;
				Op->ProcessAll(LocalEdges);
				if (!LocalEdges.IsEmpty())
				{
					Ctx->Engine->AppendEdges(LocalEdges);
				}
			});
		}

		GlobalOpsTasks->StartSimpleCallbacks();
	}

	Context->SetState(State_Probing);

	return true;
}

bool FPCGExConnectVtxElement::LaunchPatching(FPCGExConnectVtxContext* Context, const UPCGExConnectVtxSettings* Settings) const
{
	using namespace PCGExConnectVtx;

	Context->Engine->CollapseScopedEdges();

	Context->Patcher = MakeShared<PCGExGraphs::FGraphPatcher>(Context->MergedFacade.ToSharedRef());

	for (const TSharedPtr<FBatch>& Batch : Context->ValidBatches)
	{
		const int32 SourceIndex = Context->Patcher->AddVtxSource(Batch->VtxDataFacade->Source, Batch->MergeOffset);

		for (const TSharedPtr<PCGExClusters::FCluster>& Cl : Batch->ValidClusters)
		{
			TArray<int32> VtxIndices;
			VtxIndices.Reserve(Cl->Nodes->Num());
			for (const PCGExClusters::FNode& Node : *Cl->Nodes)
			{
				VtxIndices.Add(Batch->MergeOffset + Node.PointIndex);
			}
			Context->Patcher->AddEdgeGroup(Cl->EdgesIO.Pin(), VtxIndices, SourceIndex);
		}
	}

	const TSet<uint64>& NewEdges = Context->Engine->GetUniqueEdges();
	Context->ConnectorEdgeHandles.Reserve(NewEdges.Num());
	Context->ConnectorEndpoints.Reserve(NewEdges.Num());

	for (const uint64 E : NewEdges)
	{
		if (Context->ExistingEdges.Contains(E))
		{
			continue;
		}

		uint32 A;
		uint32 B;
		PCGEx::H64(E, A, B);

		if (A == B)
		{
			continue;
		}

		Context->ConnectorEdgeHandles.Add(Context->Patcher->AddEdge(A, B));
		Context->ConnectorEndpoints.Add(E);
	}

	if (Context->ConnectorEdgeHandles.IsEmpty() && !Settings->bQuietNoConnectionWarning)
	{
		PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("No connection was created."));
	}

	Context->Patcher->ResolveAndMergeAsync(Context->MainEdges.ToSharedRef(), Context->GetTaskManager(), &Context->CarryOverDetails);
	Context->SetState(State_Patching);

	return true;
}

namespace PCGExConnectVtx
{
	void WriteConnectorFlags(
		const UPCGExConnectVtxSettings* Settings,
		PCGExGraphs::FGraphPatcher* Patcher,
		const TSharedPtr<PCGExData::FFacade>& VtxFacade,
		const TArray<int32>& EdgeHandles,
		const TArray<uint64>& Endpoints)
	{
		if (!Settings->bFlagVtxConnector && !Settings->bFlagEdgeConnector)
		{
			return;
		}

		FPCGMetadataAttribute<int32>* VtxConnectorFlagAttribute = Settings->bFlagVtxConnector
			                                                          ? VtxFacade->GetOut()->MutableMetadata()->FindOrCreateAttribute<int32>(Settings->VtxConnectorFlagName, 0)
			                                                          : nullptr;

		TConstPCGValueRange<int64> VtxMetadataEntries = VtxFacade->GetOut()->GetConstMetadataEntryValueRange();

		for (int32 i = 0; i < EdgeHandles.Num(); ++i)
		{
			if (Settings->bFlagEdgeConnector)
			{
				TSharedPtr<PCGExData::FPointIO> EdgesIO;
				int32 EdgePointIndex = -1;
				if (Patcher->GetEdgeOutput(EdgeHandles[i], EdgesIO, EdgePointIndex) && EdgesIO)
				{
					FPCGMetadataAttribute<bool>* EdgeConnectorFlagAttribute = EdgesIO->GetOut()->MutableMetadata()->FindOrCreateAttribute<bool>(Settings->EdgeConnectorFlagName, false);
					const TConstPCGValueRange<int64> EdgeMetadataEntries = EdgesIO->GetOut()->GetConstMetadataEntryValueRange();
					EdgeConnectorFlagAttribute->SetValue(EdgeMetadataEntries[EdgePointIndex], true);
				}
			}

			if (VtxConnectorFlagAttribute)
			{
				const int64 VtxKeyA = VtxMetadataEntries[PCGEx::H64A(Endpoints[i])];
				const int64 VtxKeyB = VtxMetadataEntries[PCGEx::H64B(Endpoints[i])];
				VtxConnectorFlagAttribute->SetValue(VtxKeyA, VtxConnectorFlagAttribute->GetValueFromItemKey(VtxKeyA) + 1);
				VtxConnectorFlagAttribute->SetValue(VtxKeyB, VtxConnectorFlagAttribute->GetValueFromItemKey(VtxKeyB) + 1);
			}
		}
	}
}

bool FPCGExConnectVtxElement::CommitAndOutput(FPCGExConnectVtxContext* Context, const UPCGExConnectVtxSettings* Settings) const
{
	Context->Patcher->Commit();

	PCGExConnectVtx::WriteConnectorFlags(Settings, Context->Patcher.Get(), Context->MergedFacade, Context->ConnectorEdgeHandles, Context->ConnectorEndpoints);

	(void)Context->MergedFacade->Source->StageOutput(Context);
	Context->OutputPointsAndEdges();
	Context->Done();

	return true;
}

namespace PCGExConnectVtx
{
#pragma region FProcessor

	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExConnectVtx::Process);

		if (!IProcessor::Process(InTaskManager))
		{
			return false;
		}

		if (!Context->GeneratorsFiltersFactories.IsEmpty())
		{
			GeneratorsFilter = MakeShared<PCGExClusterFilter::FManager>(Cluster.ToSharedRef(), VtxDataFacade, EdgeDataFacade);
			GeneratorsFilter->SetSupportedTypes(&PCGExFactories::ClusterNodeFilters());
			if (!GeneratorsFilter->Init(ExecutionContext, Context->GeneratorsFiltersFactories))
			{
				return false;
			}
		}

		if (!Context->ConnectablesFiltersFactories.IsEmpty())
		{
			ConnectablesFilter = MakeShared<PCGExClusterFilter::FManager>(Cluster.ToSharedRef(), VtxDataFacade, EdgeDataFacade);
			ConnectablesFilter->SetSupportedTypes(&PCGExFactories::ClusterNodeFilters());
			if (!ConnectablesFilter->Init(ExecutionContext, Context->ConnectablesFiltersFactories))
			{
				return false;
			}
		}

		StartParallelLoopForNodes();

		return true;
	}

	void FProcessor::ProcessNodes(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExConnectVtx::ProcessNodes);

		FBatch* Parent = GetParentBatch<FBatch>();
		const TArrayView<PCGExClusters::FNode> NodesView = Scope.GetView(*Cluster->Nodes.Get());

		// Fill the batch-level participation masks (point-index space) for this cluster's members.
		// Vtx shared by several clusters in one group (degenerate input) take the last-tested result.

		if (GeneratorsFilter)
		{
			GeneratorsFilter->Test(NodesView, Parent->CanGenerateCache, false);
		}
		else
		{
			TArray<int8>& CanGenerate = *Parent->CanGenerateCache;
			for (const PCGExClusters::FNode& Node : NodesView)
			{
				CanGenerate[Node.PointIndex] = 1;
			}
		}

		if (ConnectablesFilter)
		{
			ConnectablesFilter->Test(NodesView, Parent->AcceptConnectionsCache, false);
		}
		else
		{
			TArray<int8>& AcceptConnections = *Parent->AcceptConnectionsCache;
			for (const PCGExClusters::FNode& Node : NodesView)
			{
				AcceptConnections[Node.PointIndex] = 1;
			}
		}
	}

	void FProcessor::Cleanup()
	{
		TProcessor<FPCGExConnectVtxContext, UPCGExConnectVtxSettings>::Cleanup();
		GeneratorsFilter.Reset();
		ConnectablesFilter.Reset();
	}

#pragma endregion

#pragma region FBatch

	FBatch::FBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FPointIO>& InVtx, const TArrayView<TSharedRef<PCGExData::FPointIO>> InEdges)
		: TBatch(InContext, InVtx, InEdges)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectVtx)

		if (Settings->Scope != EPCGExConnectVtxScope::All)
		{
			InVtx->InitializeOutput(PCGExData::EIOInit::Duplicate);
		}
	}

	void FBatch::RegisterBuffersDependencies(PCGExData::FFacadePreloader& FacadePreloader)
	{
		TBatch<FProcessor>::RegisterBuffersDependencies(FacadePreloader);

		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectVtx)

		PCGExPointFilter::RegisterBuffersDependencies(ExecutionContext, Context->GeneratorsFiltersFactories, FacadePreloader);
		PCGExPointFilter::RegisterBuffersDependencies(ExecutionContext, Context->ConnectablesFiltersFactories, FacadePreloader);
	}

	void FBatch::Process()
	{
		const int32 NumVtx = VtxDataFacade->GetNum();

		CanGenerateCache = MakeShared<TArray<int8>>();
		CanGenerateCache->Init(0, NumVtx);

		AcceptConnectionsCache = MakeShared<TArray<int8>>();
		AcceptConnectionsCache->Init(0, NumVtx);

		TBatch<FProcessor>::Process();
	}

	void FBatch::BuildTopologyData()
	{
		const int32 NumVtx = VtxDataFacade->GetNum();
		ClusterIds.Init(INDEX_NONE, NumVtx);

		for (int32 ClusterIndex = 0; ClusterIndex < ValidClusters.Num(); ++ClusterIndex)
		{
			const TSharedPtr<PCGExClusters::FCluster>& Cl = ValidClusters[ClusterIndex];

			for (const PCGExClusters::FNode& Node : *Cl->Nodes)
			{
				ClusterIds[Node.PointIndex] = ClusterIndex;
			}

			ExistingEdges.Reserve(ExistingEdges.Num() + Cl->Edges->Num());
			for (const PCGExGraphs::FEdge& E : *Cl->Edges)
			{
				ExistingEdges.Add(PCGEx::H64U(E.Start, E.End));
			}
		}
	}

	void FBatch::CompleteWork()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectVtx)

		TBatch<FProcessor>::CompleteWork();

		const int32 NumValidClusters = GatherValidClusters();

		if (Processors.Num() != NumValidClusters)
		{
			PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("Some vtx/edges groups have invalid clusters. Make sure to sanitize the input first."));
		}

		if (ValidClusters.IsEmpty())
		{
			return;
		}

		BuildTopologyData();

		if (Settings->Scope == EPCGExConnectVtxScope::All)
		{
			// The context-level pipeline takes over once every batch is done.
			return;
		}

		Engine = MakeShared<PCGExProbing::FProbingEngine>(VtxDataFacade);
		Engine->SetCoincidence(Settings->bPreventCoincidence, Context->CWCoincidenceTolerance);
		if (Settings->bProjectPoints)
		{
			Engine->SetProjection(Settings->ProjectionDetails);
		}

		if (!Engine->Init(ExecutionContext, Context->ProbeFactories))
		{
			PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("No probe could be initialized; clusters are forwarded untouched."));
			Engine.Reset();
			for (const TSharedPtr<PCGExClusters::FCluster>& Cl : ValidClusters)
			{
				if (const TSharedPtr<PCGExData::FPointIO> EdgesIO = Cl->EdgesIO.Pin())
				{
					EdgesIO->InitializeOutput(PCGExData::EIOInit::Forward);
				}
			}
			return;
		}

		Engine->CanGenerate = *CanGenerateCache;
		Engine->AcceptConnections = *AcceptConnectionsCache;

		if (Settings->Scope == EPCGExConnectVtxScope::Cluster)
		{
			Engine->PointGroupIds = &ClusterIds;
			Engine->EdgeRelation = PCGExProbing::EEdgeRelation::SameGroup;
		}
		else if (Settings->EdgeRelation == EPCGExConnectVtxEdgeRelation::CrossCluster)
		{
			Engine->PointGroupIds = &ClusterIds;
			Engine->EdgeRelation = PCGExProbing::EEdgeRelation::DifferentGroup;
		}

		Engine->PrepareWorkingData();

		NumCompletions = (Engine->HasLocalWork() ? 1 : 0) + (Engine->HasGlobalWork() ? 1 : 0);

		if (Engine->HasLocalWork())
		{
			PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, ProbingLoopTask)

			ProbingLoopTask->OnPrepareSubLoopsCallback = [PCGEX_ASYNC_THIS_CAPTURE](const TArray<PCGExMT::FScope>& Loops)
			{
				PCGEX_ASYNC_THIS
				This->Engine->PrepareScopes(Loops);
			};

			ProbingLoopTask->OnSubLoopStartCallback = [PCGEX_ASYNC_THIS_CAPTURE](const PCGExMT::FScope& Scope)
			{
				PCGEX_ASYNC_THIS
				This->Engine->ProcessScope(Scope);
			};

			ProbingLoopTask->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE]()
			{
				PCGEX_ASYNC_THIS
				This->Engine->CollapseScopedEdges();
				This->AdvanceCompletion();
			};

			ProbingLoopTask->StartSubLoops(Engine->GetNumIterations(), PCGEX_CORE_SETTINGS.GetPointsBatchChunkSize());
		}

		if (Engine->HasGlobalWork())
		{
			PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, GlobalOpsTasks)

			GlobalOpsTasks->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE]()
			{
				PCGEX_ASYNC_THIS
				This->AdvanceCompletion();
			};

			for (FPCGExProbeOperation* Operation : Engine->GetGlobalOperations())
			{
				GlobalOpsTasks->AddSimpleCallback([PCGEX_ASYNC_THIS_CAPTURE, Op = Operation]()
				{
					PCGEX_ASYNC_THIS
					TSet<uint64> LocalEdges;
					Op->ProcessAll(LocalEdges);
					if (!LocalEdges.IsEmpty())
					{
						This->Engine->AppendEdges(LocalEdges);
					}
				});
			}

			GlobalOpsTasks->StartSimpleCallbacks();
		}
	}

	void FBatch::AdvanceCompletion()
	{
		if (FPlatformAtomics::InterlockedDecrement(&NumCompletions))
		{
			return;
		}

		StageAndResolve();
	}

	void FBatch::StageAndResolve()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectVtx)

		Patcher = MakeShared<PCGExGraphs::FGraphPatcher>(VtxDataFacade);

		for (const TSharedPtr<PCGExClusters::FCluster>& Cl : ValidClusters)
		{
			TArray<int32> VtxIndices;
			VtxIndices.Reserve(Cl->Nodes->Num());
			for (const PCGExClusters::FNode& Node : *Cl->Nodes)
			{
				VtxIndices.Add(Node.PointIndex);
			}
			Patcher->AddEdgeGroup(Cl->EdgesIO.Pin(), VtxIndices);
		}

		const TSet<uint64>& NewEdges = Engine->GetUniqueEdges();
		ConnectorEdgeHandles.Reserve(NewEdges.Num());
		ConnectorEndpoints.Reserve(NewEdges.Num());

		for (const uint64 E : NewEdges)
		{
			if (ExistingEdges.Contains(E))
			{
				continue;
			}

			uint32 A;
			uint32 B;
			PCGEx::H64(E, A, B);

			if (A == B)
			{
				continue;
			}

			ConnectorEdgeHandles.Add(Patcher->AddEdge(A, B));
			ConnectorEndpoints.Add(E);
		}

		if (ConnectorEdgeHandles.IsEmpty() && !Settings->bQuietNoConnectionWarning)
		{
			PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FTEXT("No connection was created."));
		}

		Patcher->ResolveAndMergeAsync(Context->MainEdges.ToSharedRef(), TaskManager, &Context->CarryOverDetails);
	}

	void FBatch::Write()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ConnectVtx)

		if (!Patcher)
		{
			return;
		}

		Patcher->Commit();

		WriteConnectorFlags(Settings, Patcher.Get(), VtxDataFacade, ConnectorEdgeHandles, ConnectorEndpoints);
	}

#pragma endregion
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
