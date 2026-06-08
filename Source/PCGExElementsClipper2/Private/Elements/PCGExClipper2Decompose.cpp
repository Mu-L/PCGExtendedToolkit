// Copyright 2026 Timothe Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExClipper2Decompose.h"

#include "Core/PCGExClipper2Decomposition.h"
#include "Core/PCGExMT.h"

#include "Clusters/PCGExClusterCommon.h"
#include "Data/PCGExClusterData.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Graphs/PCGExGraph.h"
#include "Graphs/PCGExGraphBuilder.h"
#include "Helpers/PCGExPointArrayDataHelpers.h"
#include "Math/PCGExProjectionDetails.h"

#define LOCTEXT_NAMESPACE "PCGExClipper2DecomposeElement"
#define PCGEX_NAMESPACE Clipper2Decompose

namespace PCGExClipper2Decompose
{
	// Compile-wait phase: after every cluster is authored + its graph compile launched. Stays pending (via
	// IsWaitingForTasks) until the async subgraph compiles + vtx writes drain.
	PCGEX_CTX_STATE(State_CompilingClusters)
}

#pragma region UPCGExClipper2DecomposeSettings

UPCGExClipper2DecomposeSettings::UPCGExClipper2DecomposeSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Geometry node: one cluster per input path. Multi-source grouping/matching is disabled (forces Split) --
	// it would merge unrelated footprints and break attribute carry (cluster nodes inherit one template source).
	bExposeGroupingPolicy = false;

	// Hide the inherited path-output-only parameters (blending, carry-over, open-path, simplify). See base.
	bExposePathOutputProperties = false;
}

FPCGExGeo2DProjectionDetails UPCGExClipper2DecomposeSettings::GetProjectionDetails() const
{
	return ProjectionDetails;
}

TArray<FPCGPinProperties> UPCGExClipper2DecomposeSettings::OutputPinProperties() const
{
	// Main pin (Vtx) is declared by the base via GetMainOutputPin(); add the paired Edges pin.
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExClusters::Labels::OutputEdgesLabel, "Point data representing edges.", Required)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(Clipper2Decompose)

#pragma endregion

#pragma region FPCGExClipper2DecomposeContext

void FPCGExClipper2DecomposeContext::AddStagedCluster(const TSharedPtr<PCGExGraphs::FGraphBuilder>& InGraphBuilder, const TSharedPtr<PCGExData::FPointIO>& InVtxIO, const int32 InGroupIndex)
{
	FScopeLock Lock(&StagedClustersLock);
	FPCGExDecomposeCluster& Staged = StagedClusters.Emplace_GetRef();
	Staged.GraphBuilder = InGraphBuilder;
	Staged.VtxIO = InVtxIO;
	Staged.GroupIndex = InGroupIndex;
}

void FPCGExClipper2DecomposeContext::StageClusterOutputs()
{
	// Deterministic output ordering by group index.
	StagedClusters.Sort([](const FPCGExDecomposeCluster& A, const FPCGExDecomposeCluster& B)
	{
		return A.GroupIndex < B.GroupIndex;
	});

	for (const FPCGExDecomposeCluster& Staged : StagedClusters)
	{
		if (!Staged.GraphBuilder || !Staged.GraphBuilder->bCompiledSuccessfully)
		{
			// Compilation produced no valid subgraph (e.g. every node pruned) -- drop the orphan vtx output.
			if (Staged.VtxIO)
			{
				Staged.VtxIO->InitializeOutput(PCGExData::EIOInit::NoInit);
				Staged.VtxIO->Disable();
			}
			continue;
		}

		Staged.GraphBuilder->StageEdgesOutputs();
	}

	// Vtx outputs live in MainPoints (only the authored cluster-node IOs have OUT data; the source paths
	// remain input-only and are skipped).
	MainPoints->StageOutputs();
}

void FPCGExClipper2DecomposeContext::Process(const TSharedPtr<PCGExClipper2::FProcessingGroup>& Group)
{
	const UPCGExClipper2DecomposeSettings* Settings = GetInputSettings<UPCGExClipper2DecomposeSettings>();

	// Triangulate + deduplicate vertex pool + Hertel-Mehlhorn merge (shared with Clipper2 : Volume).
	const PCGExClipper2Decomposition::FDecomposeParams Params = PCGExClipper2Decomposition::MakeParams(Settings);

	PCGExClipper2Decomposition::FDecomposeResult Decomposition;
	if (!PCGExClipper2Decomposition::TryDecomposeGroup(Group, AllOpData, Params, Decomposition))
	{
		if (!Settings->bQuietWarnings)
		{
			const FText WarningText = PCGExClipper2Decomposition::DescribeDecomposeFailure(
				Decomposition, LOCTEXT("DecomposeSubject", "footprint"), Settings->MaxConvexPieces);
			if (!WarningText.IsEmpty()) { PCGE_LOG_C(Warning, GraphAndLog, this, WarningText); }
		}
		return;
	}

	// Frame subject: projection frame (Steiner unprojection) + attribute template for the nodes.
	const int32 FrameSrcIdx = Group->SubjectIndices[0];
	const FPCGExGeo2DProjectionDetails& FrameProjection = AllOpData->Projections[FrameSrcIdx];

	const TArray<PCGExClipper2Decomposition::FFootprintVertex>& VertexPool = Decomposition.VertexPool;
	const TArray<TArray<int32>>& Pieces = Decomposition.Pieces;
	const int32 NumVerts = VertexPool.Num();

	// --- Edge set: deduplicated union of every convex-piece edge (boundary + diagonals) ---
	// H64U canonicalizes (a,b)==(b,a), so shared diagonals collapse to one key; InsertEdges decodes back to A!=B.
	TSet<uint64> EdgeKeys;
	EdgeKeys.Reserve(NumVerts * 3);
	for (const TArray<int32>& Piece : Pieces)
	{
		const int32 N = Piece.Num();
		for (int32 i = 0; i < N; i++)
		{
			const int32 A = Piece[i];
			const int32 B = Piece[(i + 1) % N];
			if (A == B) { continue; }
			EdgeKeys.Add(PCGEx::H64U(static_cast<uint32>(A), static_cast<uint32>(B)));
		}
	}

	if (EdgeKeys.IsEmpty()) { return; }

	// --- Author vtx data from the frame source as template (provides the attribute schema) ---
	const TSharedPtr<PCGExData::FFacade>& TemplateFacade = AllOpData->Facades[FrameSrcIdx];
	const TSharedPtr<PCGExData::FPointIO> VtxIO = MainPoints->Emplace_GetRef<UPCGExClusterNodesData>(TemplateFacade->Source, PCGExData::EIOInit::New);
	if (!VtxIO) { return; }

	VtxIO->IOIndex = Group->GroupIndex; // deterministic vtx output ordering
	VtxIO->OutputPin = PCGExClusters::Labels::OutputVerticesLabel;

	UPCGBasePointData* OutPoints = VtxIO->GetOut();
	const EPCGPointNativeProperties Allocations = TemplateFacade->GetAllocations();
	PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutPoints, NumVerts, Allocations);

	// Transforms written explicitly; non-transform attributes carried via ConsumeIdxMapping. Groups are always
	// single-source (Split is forced), so the carry against the frame source is exact.
	TArray<int32>& IdxMapping = VtxIO->GetIdxMapping(NumVerts);
	TPCGValueRange<FTransform> OutTransforms = OutPoints->GetTransformValueRange();

	// Single-source group: fetch the frame source's transform view once.
	const TConstPCGValueRange<FTransform> FrameTransforms = TemplateFacade->Source->GetIn()->GetConstTransformValueRange();

	for (int32 i = 0; i < NumVerts; i++)
	{
		const PCGExClipper2Decomposition::FFootprintVertex& V = VertexPool[i];

		if (V.bHasSource && V.SourceIdx == FrameSrcIdx)
		{
			OutTransforms[i] = FrameTransforms[V.SourcePointIdx];
			IdxMapping[i] = V.SourcePointIdx;
		}
		else
		{
			// No source (Clipper-created intersection / Steiner vertex) -> position by unprojection.
			const FVector UnprojectedPos = FrameProjection.Unproject(FVector(V.Pos.X, V.Pos.Y, V.ProjectedZ));
			OutTransforms[i] = FTransform(UnprojectedPos);
			IdxMapping[i] = 0;
		}
	}

	// Carry non-transform attributes (transforms authored above).
	EPCGPointNativeProperties CarryProperties = Allocations;
	EnumRemoveFlags(CarryProperties, EPCGPointNativeProperties::Transform);
	VtxIO->ConsumeIdxMapping(CarryProperties);

	// Build the graph AFTER the vtx OUT buffer is sized/positioned (FGraphBuilder captures its transform range).
	// bInheritNodeData=false -- nodes are authored from scratch.
	const TSharedPtr<PCGExData::FFacade> VtxFacade = MakeShared<PCGExData::FFacade>(VtxIO.ToSharedRef());
	const TSharedPtr<PCGExGraphs::FGraphBuilder> GraphBuilder = MakeShared<PCGExGraphs::FGraphBuilder>(VtxFacade.ToSharedRef(), &Settings->GraphBuilderDetails);
	GraphBuilder->bInheritNodeData = false;
	GraphBuilder->Graph->InsertEdges(EdgeKeys, -1);

	AddStagedCluster(GraphBuilder, VtxIO, Group->GroupIndex);
}

#pragma endregion

#pragma region FPCGExClipper2DecomposeElement

bool FPCGExClipper2DecomposeElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExClipper2DecomposeElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(Clipper2Decompose)
	PCGEX_EXECUTION_CHECK

	PCGEX_ON_INITIAL_EXECUTION
	{
		Context->SetState(PCGExCommon::States::State_Processing);
		PCGEX_ASYNC_GROUP_CHKD_RET(Context->GetTaskManager(), WorkTasks, true)

		TWeakPtr<FPCGContextHandle> WeakHandle = Context->GetWeakSelfHandle();

		// Decompose each group + author its vtx IO and graph builder in parallel (one task per group).
		for (int32 i = 0; i < Context->ProcessingGroups.Num(); i++)
		{
			WorkTasks->AddSimpleCallback([Settings, WeakHandle, Index = i]
			{
				FPCGContext::FSharedContext<FPCGExClipper2DecomposeContext> SharedContext(WeakHandle);
				if (!SharedContext.Get()) { return; }

				const TSharedPtr<PCGExClipper2::FProcessingGroup> Group = SharedContext.Get()->ProcessingGroups[Index];
				Group->PreProcess(Settings);
				SharedContext.Get()->Process(Group);
			});
		}

		WorkTasks->StartSimpleCallbacks();
	}

	PCGEX_ON_ASYNC_STATE_READY(PCGExCommon::States::State_Processing)
	{
		// Every group authored. Orchestrate the (async) graph compiles.
		OutputWork(Context, Settings);
	}

	PCGEX_ON_ASYNC_STATE_READY(PCGExClipper2Decompose::State_CompilingClusters)
	{
		// All compiles drained -- stage vtx + edges deterministically.
		Context->StageClusterOutputs();
		Context->Done();
	}

	return Context->TryComplete();
}

void FPCGExClipper2DecomposeElement::OutputWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	PCGEX_CONTEXT_AND_SETTINGS(Clipper2Decompose)

	if (Context->StagedClusters.IsEmpty())
	{
		Context->Done();
		return;
	}

	Context->SetState(PCGExClipper2Decompose::State_CompilingClusters);

	// Keeps the context "waiting for tasks" until the compiles drain (State_CompilingClusters gates on
	// IsWaitingForTasks).
	PCGExMT::FSchedulingScope SchedulingScope(Context->GetTaskManager());
	if (!SchedulingScope.Token.IsValid())
	{
		Context->Done();
		return;
	}

	for (const FPCGExDecomposeCluster& Staged : Context->StagedClusters)
	{
		if (!Staged.GraphBuilder) { continue; }
		// true = flush the vtx facade (incl. cluster vtx-index attr) on compile completion.
		Staged.GraphBuilder->CompileAsync(Context->GetTaskManager(), true);
	}
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
