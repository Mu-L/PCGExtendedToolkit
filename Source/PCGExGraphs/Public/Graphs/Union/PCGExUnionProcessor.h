// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Details/PCGExBlendingDetails.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExIntersectionDetails.h"
#include "Graphs/PCGExGraphDetails.h"
#include "Graphs/PCGExGraphMetadata.h"

struct FPCGExCarryOverDetails;

namespace PCGExData
{
	class FUnionTable;
}

namespace PCGExBlending
{
	class FMetadataBlender;
	class IUnionBlender;
}

namespace PCGExMT
{
	template <typename T>
	class TScopedPtr;
}

namespace PCGExGraphs
{
	class FEdgeEdgeIntersections;
	class FPointEdgeIntersections;
	class FGraphBuilder;
	struct FEdge;

	class PCGEXGRAPHS_API FUnionProcessor : public TSharedFromThis<FUnionProcessor>
	{
		bool bCompilingFinalGraph = false;

	public:
		FPCGExContext* Context = nullptr;

		TSharedRef<PCGExData::FFacade> UnionDataFacade;

		// Pre-built by the cluster element's Phase 1+2 streaming build, handed off via SetUnionData
		// before StartExecution. NodesTable provides per-node source-element groups (drives P/P blend);
		// EdgesTable provides per-edge source groups (drives CompileRange edge blending). Edges is the
		// flat edge array decoded from EdgesTable's packed (Start, End) keys; AdoptEdges takes ownership.
		TSharedPtr<PCGExData::FUnionTable> NodesTable;
		TSharedPtr<PCGExData::FUnionTable> EdgesTable;
		TArray<FEdge> Edges;
		FBox Bounds = FBox(ForceInit);

		const TArray<TSharedRef<PCGExData::FFacade>>* SourceEdgesIO = nullptr;

		FPCGExPointPointIntersectionDetails PointPointIntersectionDetails;
		const FPCGExCarryOverDetails* VtxCarryOverDetails = nullptr;
		const FPCGExCarryOverDetails* EdgesCarryOverDetails = nullptr;

		bool bDoPointEdge = false;
		FPCGExPointEdgeIntersectionDetails PointEdgeIntersectionDetails;
		bool bUseCustomPointEdgeBlending = false;
		FPCGExBlendingDetails CustomPointEdgeBlendingDetails;

		bool bDoEdgeEdge = false;
		FPCGExEdgeEdgeIntersectionDetails EdgeEdgeIntersectionDetails;
		bool bUseCustomEdgeEdgeBlending = false;
		FPCGExBlendingDetails CustomEdgeEdgeBlendingDetails;

		FPCGExGraphBuilderDetails GraphBuilderDetails;


		TSharedPtr<PCGExBlending::IUnionBlender> UnionBlender;

		explicit FUnionProcessor(
			FPCGExContext* InContext,
			TSharedRef<PCGExData::FFacade> InUnionDataFacade,
			FPCGExPointPointIntersectionDetails PointPointIntersectionDetails,
			FPCGExBlendingDetails InDefaultPointsBlending,
			FPCGExBlendingDetails InDefaultEdgesBlending);

		~FUnionProcessor();

		void InitPointEdge(const FPCGExPointEdgeIntersectionDetails& InDetails, const bool bUseCustom = false, const FPCGExBlendingDetails* InOverride = nullptr);

		void InitEdgeEdge(const FPCGExEdgeEdgeIntersectionDetails& InDetails, const bool bUseCustom = false, const FPCGExBlendingDetails* InOverride = nullptr);

		// Hand off the streaming-built tables and decoded edges before StartExecution. Edges is moved.
		void SetUnionData(
			const TSharedPtr<PCGExData::FUnionTable>& InNodesTable,
			const TSharedPtr<PCGExData::FUnionTable>& InEdgesTable,
			TArray<FEdge>&& InEdges,
			const FBox& InBounds);

		bool StartExecution(const TArray<TSharedRef<PCGExData::FFacade>>& InFacades, const FPCGExGraphBuilderDetails& InBuilderDetails);

		bool Execute();

	protected:
		bool bRunning = false;

		int32 PENum = 0;
		int32 EENum = 0;

		void OnNodesProcessingComplete();
		void InternalStartExecution();

		FPCGExGraphBuilderDetails BuilderDetails;
		FPCGExBlendingDetails DefaultPointsBlendingDetails;
		FPCGExBlendingDetails DefaultEdgesBlendingDetails;

		TSharedPtr<FGraphBuilder> GraphBuilder;

		FGraphMetadataDetails GraphMetadataDetails;

		TSharedPtr<FPointEdgeIntersections> PointEdgeIntersections;

		TSharedPtr<PCGExMT::TScopedPtr<FEdgeEdgeIntersections>> ScopedEdgeEdgeIntersections;
		TSharedPtr<FEdgeEdgeIntersections> EdgeEdgeIntersections;

		TSharedPtr<PCGExBlending::FMetadataBlender> MetadataBlender;

		void FindPointEdgeIntersections();
		void OnPointEdgeIntersectionsFound();
		void OnPointEdgeIntersectionsComplete();

		void FindEdgeEdgeIntersections();
		void OnEdgeEdgeIntersectionsFound();
		void OnEdgeEdgeIntersectionsComplete();
		void CompileFinalGraph();
	};
}
