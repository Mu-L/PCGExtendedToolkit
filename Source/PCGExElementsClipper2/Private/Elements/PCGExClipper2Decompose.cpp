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
#include "PCGExGraphs/Public/Graphs/PCGExGraph.h"
#include "PCGExGraphs/Public/Graphs/PCGExGraphBuilder.h"

#define LOCTEXT_NAMESPACE "PCGExClipper2DecomposeElement"
#define PCGEX_NAMESPACE Clipper2Decompose

namespace PCGExClipper2Decompose
{
	// Compile-wait phase: entered once every group's cluster has been authored (in Process) and its graph
	// compile launched (in OutputWork). PCGEX_ON_ASYNC_STATE_READY gates on IsWaitingForTasks(), so this
	// state stays pending until the async subgraph compiles + vtx writes fully drain.
	PCGEX_CTX_STATE(State_CompilingClusters)
}

#pragma region UPCGExClipper2DecomposeSettings

UPCGExClipper2DecomposeSettings::UPCGExClipper2DecomposeSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// One cluster per input path by default (matches Clipper2 : Volume's grouping). Switch to Merged to
	// decompose a whole group of footprints into a single cluster.
	MainInputGroupingPolicy = EPCGExGroupingPolicy::Split;

	// Geometry node: hide the inherited path-output-only parameters (blending, carry-over, hole/joint
	// tagging, open-path output, simplify/arc-tolerance). See UPCGExClipper2ProcessorSettings.
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
	PCGExClipper2Decomposition::FDecomposeParams Params;
	Params.Precision = Settings->Precision;
	Params.FillRule = Settings->FillRule;
	Params.bUseDelaunay = true;
	Params.bMergeConvexPieces = Settings->bMergeConvexPieces;
	Params.MaxConvexPieces = Settings->MaxConvexPieces;

	PCGExClipper2Decomposition::FDecomposeResult Decomposition;
	if (!PCGExClipper2Decomposition::TryDecomposeGroup(Group, AllOpData, Params, Decomposition))
	{
		if (!Settings->bQuietWarnings)
		{
			if (Decomposition.Status == PCGExClipper2Decomposition::EDecomposeResult::TriangulationFailed)
			{
				PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("A footprint could not be triangulated (degenerate or self-intersecting) and was skipped."));
			}
			else if (Decomposition.Status == PCGExClipper2Decomposition::EDecomposeResult::TooManyPieces)
			{
				PCGE_LOG_C(Warning, GraphAndLog, this, FText::Format(
					LOCTEXT("TooManyPieces", "A footprint needs {0} convex pieces (over the {1} cap) and was skipped. Raise Max Convex Pieces or simplify the path."),
					FText::AsNumber(Decomposition.Pieces.Num()), FText::AsNumber(Settings->MaxConvexPieces)));
			}
		}
		return;
	}

	// Frame subject: projection frame for unprojecting Steiner vertices + attribute template for the nodes.
	const int32 FrameSrcIdx = Group->SubjectIndices[0];
	const FPCGExGeo2DProjectionDetails& FrameProjection = AllOpData->Projections[FrameSrcIdx];

	const TArray<PCGExClipper2Decomposition::FFootprintVertex>& VertexPool = Decomposition.VertexPool;
	const TArray<TArray<int32>>& Pieces = Decomposition.Pieces;
	const int32 NumVerts = VertexPool.Num();

	// --- Edge set: the deduplicated union of every convex-piece edge (boundary + internal diagonals) ---
	// H64U canonicalizes (a,b)==(b,a), so a shared diagonal between two pieces collapses to one key, and
	// InsertEdges decodes the key back to a valid A!=B endpoint pair.
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

	// --- Author the cluster-node (vtx) data from the frame source as template (provides the attribute schema) ---
	const TSharedPtr<PCGExData::FFacade>& TemplateFacade = AllOpData->Facades[FrameSrcIdx];
	const TSharedPtr<PCGExData::FPointIO> VtxIO = MainPoints->Emplace_GetRef<UPCGExClusterNodesData>(TemplateFacade->Source, PCGExData::EIOInit::New);
	if (!VtxIO) { return; }

	VtxIO->IOIndex = Group->GroupIndex; // deterministic vtx output ordering
	VtxIO->OutputPin = PCGExClusters::Labels::OutputVerticesLabel;

	UPCGBasePointData* OutPoints = VtxIO->GetOut();
	const EPCGPointNativeProperties Allocations = TemplateFacade->GetAllocations();
	PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutPoints, NumVerts, Allocations);

	// Each node maps back to its decoded source point. Transforms are written explicitly here (so they are
	// correct even when a group merges multiple sources); attributes are carried over via ConsumeIdxMapping,
	// which resolves against the template source -- exact for the single-source (default Split) case.
	TArray<int32>& IdxMapping = VtxIO->GetIdxMapping(NumVerts);
	TPCGValueRange<FTransform> OutTransforms = OutPoints->GetTransformValueRange();

	for (int32 i = 0; i < NumVerts; i++)
	{
		const PCGExClipper2Decomposition::FFootprintVertex& V = VertexPool[i];

		if (V.bHasSource && AllOpData->Facades.IsValidIndex(V.SourceIdx))
		{
			const TSharedPtr<PCGExData::FFacade>& SrcFacade = AllOpData->Facades[V.SourceIdx];
			TConstPCGValueRange<FTransform> SrcTransforms = SrcFacade->Source->GetIn()->GetConstTransformValueRange();
			OutTransforms[i] = SrcTransforms[V.SourcePointIdx];
			IdxMapping[i] = (V.SourceIdx == FrameSrcIdx) ? V.SourcePointIdx : 0;
		}
		else
		{
			// Clipper-created intersection / Steiner vertex: no source -> position by unprojection.
			const FVector UnprojectedPos = FrameProjection.Unproject(FVector(V.Pos.X, V.Pos.Y, V.ProjectedZ));
			OutTransforms[i] = FTransform(UnprojectedPos);
			IdxMapping[i] = 0;
		}
	}

	// Carry non-transform attributes from the template source (transforms were authored explicitly above).
	EPCGPointNativeProperties CarryProperties = Allocations;
	EnumRemoveFlags(CarryProperties, EPCGPointNativeProperties::Transform);
	VtxIO->ConsumeIdxMapping(CarryProperties);

	// Build the graph AFTER the vtx OUT buffer is allocated + positioned: FGraphBuilder captures the OUT
	// transform range and sizes the graph from it. bInheritNodeData=false -- nodes are authored from scratch.
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

	// Bracket the launches so the context reliably reports "waiting for tasks" until the compiles drain;
	// the State_CompilingClusters ready-check (IsWaitingForTasks) then resumes only once they finish.
	PCGExMT::FSchedulingScope SchedulingScope(Context->GetTaskManager());
	if (!SchedulingScope.Token.IsValid())
	{
		Context->Done();
		return;
	}

	for (const FPCGExDecomposeCluster& Staged : Context->StagedClusters)
	{
		if (!Staged.GraphBuilder) { continue; }
		// bWriteNodeFacade=true: the builder flushes the vtx facade (incl. the cluster vtx-index attribute)
		// on compile completion.
		Staged.GraphBuilder->CompileAsync(Context->GetTaskManager(), true);
	}
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
