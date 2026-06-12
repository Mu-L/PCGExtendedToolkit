// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graphs/PCGExGraph.h"

#include "PCGExH.h"
#include "Clusters/PCGExEdge.h"
#include "Core/PCGExMTCommon.h"
#include "Graphs/PCGExSubGraph.h"

namespace PCGExGraphs
{
	FGraph::FGraph(const int32 InNumNodes)
	{
		int32 StartNodeIndex = 0;
		AddNodes(InNumNodes, StartNodeIndex);
	}

	void FGraph::ReserveForEdges(const int32 UpcomingAdditionCount)
	{
		const int32 ExpectedEdgeTotal = Edges.Num() + UpcomingAdditionCount;
		UniqueEdges.Reserve(ExpectedEdgeTotal);
		Edges.Reserve(ExpectedEdgeTotal);

		if (ExpectedEdgeTotal > EdgeMetadata.Num())
		{
			EdgeMetadata.SetNum(ExpectedEdgeTotal);
		}
		const int32 NumNodes = Nodes.Num();
		if (NumNodes > NodeMetadata.Num())
		{
			NodeMetadata.SetNum(NumNodes);
		}
	}

	bool FGraph::InsertEdge_Unsafe(const int32 A, const int32 B, FEdge& OutEdge, const int32 IOIndex)
	{
		check(A != B)

		const uint64 Hash = PCGEx::H64U(A, B);
		if (const int32* EdgeIndex = UniqueEdges.Find(Hash))
		{
			OutEdge.Index = *EdgeIndex;
			return false;
		}

		OutEdge = Edges.Emplace_GetRef(Edges.Num(), A, B, -1, IOIndex);
		UniqueEdges.Add(Hash, (OutEdge.Index = Edges.Num() - 1));

		Nodes[A].LinkEdge(OutEdge.Index);
		Nodes[B].LinkEdge(OutEdge.Index);

		return true;
	}

	bool FGraph::InsertEdge(const int32 A, const int32 B, FEdge& OutEdge, const int32 IOIndex)
	{
		FWriteScopeLock WriteLock(GraphLock);
		return InsertEdge_Unsafe(A, B, OutEdge, IOIndex);
	}

	bool FGraph::InsertEdge_Unsafe(const FEdge& Edge)
	{
		uint64 H = Edge.H64U();
		if (UniqueEdges.Contains(H))
		{
			return false;
		}

		FEdge& NewEdge = Edges.Emplace_GetRef(Edge);
		UniqueEdges.Add(H, (NewEdge.Index = Edges.Num() - 1));

		Nodes[Edge.Start].LinkEdge(NewEdge.Index);
		Nodes[Edge.End].LinkEdge(NewEdge.Index);

		return true;
	}

	bool FGraph::InsertEdge(const FEdge& Edge)
	{
		FWriteScopeLock WriteLock(GraphLock);
		return InsertEdge_Unsafe(Edge);
	}

	bool FGraph::InsertEdge_Unsafe(const FEdge& Edge, FEdge& OutEdge, const int32 InIOIndex)
	{
		return InsertEdge_Unsafe(Edge.Start, Edge.End, OutEdge, InIOIndex);
	}

	bool FGraph::InsertEdge(const FEdge& Edge, FEdge& OutEdge, const int32 InIOIndex)
	{
		return InsertEdge(Edge.Start, Edge.End, OutEdge, InIOIndex);
	}

	void FGraph::InsertEdges(const TArray<uint64>& InEdges, const int32 InIOIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGraph::InsertEdges)

		FWriteScopeLock WriteLock(GraphLock);
		uint32 A;
		uint32 B;

		UniqueEdges.Reserve(UniqueEdges.Num() + InEdges.Num());
		Edges.Reserve(Edges.Num() + InEdges.Num());

		for (const uint64 E : InEdges)
		{
			if (UniqueEdges.Contains(E))
			{
				continue;
			}

			PCGEx::H64(E, A, B);

			check(A != B)

			const int32 EdgeIndex = Edges.Emplace(Edges.Num(), A, B, -1, InIOIndex);

			UniqueEdges.Add(E, EdgeIndex);
			Nodes[A].LinkEdge(EdgeIndex);
			Nodes[B].LinkEdge(EdgeIndex);
		}

		UniqueEdges.Shrink();
	}

	int32 FGraph::InsertEdges(const TArray<FEdge>& InEdges)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGraph::InsertEdges)

		FWriteScopeLock WriteLock(GraphLock);
		const int32 StartIndex = Edges.Num();

		UniqueEdges.Reserve(UniqueEdges.Num() + InEdges.Num());
		Edges.Reserve(Edges.Num() + InEdges.Num());

		for (const FEdge& E : InEdges)
		{
			InsertEdge_Unsafe(E);
		}
		return StartIndex;
	}

	void FGraph::AdoptEdges(TArray<FEdge>& InEdges)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGraph::AdoptEdges);

		FWriteScopeLock WriteLock(GraphLock);

		Edges = MoveTemp(InEdges);
		const int32 NumEdges = Edges.Num();

		UniqueEdges.Reserve(NumEdges);

		for (int32 i = 0; i < NumEdges; i++)
		{
			FEdge& Edge = Edges[i];
			UniqueEdges.Add(Edge.H64U(), i);
			Nodes[Edge.Start].LinkEdge(i);
			Nodes[Edge.End].LinkEdge(i);
		}

		// Initialize edge metadata arrays
		EdgeMetadata.SetNum(NumEdges);
	}

	FEdge* FGraph::FindEdge_Unsafe(const uint64 Hash)
	{
		const int32* Index = UniqueEdges.Find(Hash);
		if (!Index)
		{
			return nullptr;
		}
		return (Edges.GetData() + *Index);
	}

	FEdge* FGraph::FindEdge_Unsafe(const int32 A, const int32 B)
	{
		return FindEdge(PCGEx::H64U(A, B));
	}

	FEdge* FGraph::FindEdge(const uint64 Hash)
	{
		FReadScopeLock ReadScopeLock(GraphLock);
		const int32* Index = UniqueEdges.Find(Hash);
		if (!Index)
		{
			return nullptr;
		}
		return (Edges.GetData() + *Index);
	}

	FEdge* FGraph::FindEdge(const int32 A, const int32 B)
	{
		return FindEdge(PCGEx::H64U(A, B));
	}

	FGraphEdgeMetadata& FGraph::GetOrCreateEdgeMetadata(const int32 EdgeIndex, const int32 RootIndex)
	{
		{
			FReadScopeLock ReadScopeLock(MetadataLock);
			if (EdgeIndex < EdgeMetadata.Num() && EdgeMetadata[EdgeIndex].EdgeIndex != -1)
			{
				return EdgeMetadata[EdgeIndex];
			}
		}
		{
			FWriteScopeLock WriteScopeLock(MetadataLock);
			check(EdgeIndex < EdgeMetadata.Num()) // All callers must pre-size via ReserveForEdges/AdoptEdges

			if (EdgeMetadata[EdgeIndex].EdgeIndex == -1)
			{
				EdgeMetadata[EdgeIndex] = FGraphEdgeMetadata(EdgeIndex, RootIndex);
				bHasAnyEdgeMetadata = true;
			}
			return EdgeMetadata[EdgeIndex];
		}
	}

	void FGraph::InsertEdges_Unsafe(const TSet<uint64>& InEdges, const int32 InIOIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGraph::InsertEdges_Unsafe);

		uint32 A;
		uint32 B;

		UniqueEdges.Reserve(UniqueEdges.Num() + InEdges.Num());
		Edges.Reserve(UniqueEdges.Num() + InEdges.Num());

		for (const uint64& E : InEdges)
		{
			if (UniqueEdges.Contains(E))
			{
				continue;
			}

			PCGEx::H64(E, A, B);

			check(A != B)

			const int32 EdgeIndex = Edges.Emplace(Edges.Num(), A, B);
			UniqueEdges.Add(E, EdgeIndex);
			Nodes[A].LinkEdge(EdgeIndex);
			Nodes[B].LinkEdge(EdgeIndex);
			Edges[EdgeIndex].IOIndex = InIOIndex;
		}
	}

	void FGraph::InsertEdges(const TSet<uint64>& InEdges, const int32 InIOIndex)
	{
		FWriteScopeLock WriteLock(GraphLock);
		InsertEdges_Unsafe(InEdges, InIOIndex);
	}

	TArrayView<FNode> FGraph::AddNodes(const int32 NumNewNodes, int32& OutStartIndex)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGraph::AddNodes);

		FWriteScopeLock WriteLock(GraphLock);
		OutStartIndex = Nodes.Num();
		const int32 TotalNum = OutStartIndex + NumNewNodes;
		Nodes.Reserve(TotalNum);
		for (int i = OutStartIndex; i < TotalNum; i++)
		{
			Nodes.Emplace(i, i);
		}

		// Grow node metadata arrays to match
		if (TotalNum > NodeMetadata.Num())
		{
			NodeMetadata.SetNum(TotalNum);
		}

		return MakeArrayView(Nodes.GetData() + OutStartIndex, NumNewNodes);
	}

	void FGraph::BuildSubGraphs(const FPCGExGraphBuilderDetails& Limits, TArray<int32>& OutValidNodes)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGraph::BuildSubGraphs);

		// Connected components via union-find over the flat edge list, replacing the
		// previous sequential BFS: linear scans over contiguous arrays instead of
		// pointer-chasing through per-node Links, exact component sizes known before
		// any subgraph is allocated, and components that fail size limits are culled
		// before being materialized.
		// Nodes within a component are gathered in ascending index order (the BFS
		// gathered them in traversal order); deterministic output ordering is
		// re-established downstream (Morton/radix sorts in the graph compilation).

		const int32 NumNodes = Nodes.Num();
		const int32 NumEdges = Edges.Num();

		// Linkless nodes can never belong to a subgraph
		PCGExMT::ParallelOrSequential(
			NumNodes,
			[&](const int32 i)
			{
				FNode& Node = Nodes[i];
				if (!Node.bValid || Node.IsEmpty())
				{
					Node.bValid = false;
				}
			});

		TArray<int32> Parent;
		Parent.SetNumUninitialized(NumNodes);
		PCGExMT::ParallelOrSequential(NumNodes, [&](const int32 i) { Parent[i] = i; });

		// Per-root node count while unioning (union by size keeps trees shallow)
		TArray<int32> ComponentSize;
		ComponentSize.Init(1, NumNodes);

		// Iterative find with path halving
		auto Find = [&Parent](int32 X) -> int32
		{
			while (Parent[X] != X)
			{
				Parent[X] = Parent[Parent[X]];
				X = Parent[X];
			}
			return X;
		};

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(BuildSubGraphs::Union);

			for (const FEdge& Edge : Edges)
			{
				if (!Edge.bValid)
				{
					continue;
				}

				const int32 Start = static_cast<int32>(Edge.Start);
				const int32 End = static_cast<int32>(Edge.End);

				if (!Nodes[Start].bValid || !Nodes[End].bValid)
				{
					continue;
				}

				int32 RootA = Find(Start);
				int32 RootB = Find(End);

				if (RootA == RootB)
				{
					continue;
				}

				if (ComponentSize[RootA] < ComponentSize[RootB])
				{
					Swap(RootA, RootB);
				}

				Parent[RootB] = RootA;
				ComponentSize[RootA] += ComponentSize[RootB];
			}
		}

		// Assign compact component ids ordered by minimum node index -- the same
		// order in which the BFS used to discover components.
		int32 NumComponents = 0;
		int32 TotalExportedNodes = 0;

		TArray<int32> NodeComponent;
		NodeComponent.Init(-1, NumNodes);

		TArray<int32> RootToComponent;
		RootToComponent.Init(-1, NumNodes);

		TArray<int32> ComponentNodeCounts;
		TArray<int32> ComponentEdgeCounts;

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(BuildSubGraphs::Label);

			for (int32 i = 0; i < NumNodes; i++)
			{
				if (!Nodes[i].bValid)
				{
					continue;
				}

				const int32 Root = Find(i);
				int32& Component = RootToComponent[Root];
				if (Component == -1)
				{
					Component = NumComponents++;
					ComponentNodeCounts.Add(ComponentSize[Root]);
				}
				NodeComponent[i] = Component;
			}

			ComponentEdgeCounts.Init(0, NumComponents);

			for (const FEdge& Edge : Edges)
			{
				if (!Edge.bValid)
				{
					continue;
				}

				// Both endpoints share the same component by construction; an edge
				// with an invalid endpoint belongs to none (mirrors the BFS, which
				// never collected such edges).
				const int32 Component = NodeComponent[static_cast<int32>(Edge.Start)];
				if (Component == -1 || NodeComponent[static_cast<int32>(Edge.End)] == -1)
				{
					continue;
				}

				ComponentEdgeCounts[Component]++;
			}
		}

		// Evaluate size limits on counts alone - nothing has been allocated yet.
		// Components that fail limits are invalidated; edgeless components are
		// silently dropped (not invalidated, not exported), as before.
		const int32 SubGraphsBase = SubGraphs.Num();
		int32 NumNewSubGraphs = 0;

		TArray<int8> ComponentCulled;
		ComponentCulled.SetNumUninitialized(NumComponents);

		TArray<int32> ComponentSubGraph;
		ComponentSubGraph.SetNumUninitialized(NumComponents);

		for (int32 c = 0; c < NumComponents; c++)
		{
			const bool bMeetsLimits = Limits.IsValid(ComponentNodeCounts[c], ComponentEdgeCounts[c]);
			ComponentCulled[c] = bMeetsLimits ? 0 : 1;

			if (bMeetsLimits && ComponentEdgeCounts[c] > 0)
			{
				ComponentSubGraph[c] = NumNewSubGraphs++;
				TotalExportedNodes += ComponentNodeCounts[c];
			}
			else
			{
				ComponentSubGraph[c] = -1;
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(BuildSubGraphs::Cull);

			PCGExMT::ParallelOrSequential(
				NumNodes,
				[&](const int32 i)
				{
					const int32 Component = NodeComponent[i];
					if (Component != -1 && ComponentCulled[Component])
					{
						Nodes[i].bValid = false;
					}
				});

			PCGExMT::ParallelOrSequential(
				NumEdges,
				[&](const int32 i)
				{
					FEdge& Edge = Edges[i];
					if (!Edge.bValid)
					{
						return;
					}

					const int32 Component = NodeComponent[static_cast<int32>(Edge.Start)];
					if (Component == -1 || NodeComponent[static_cast<int32>(Edge.End)] == -1)
					{
						return;
					}

					if (ComponentCulled[Component])
					{
						Edge.bValid = false;
					}
				});
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(BuildSubGraphs::Gather);

			SubGraphs.Reserve(SubGraphsBase + NumNewSubGraphs);
			for (int32 c = 0; c < NumComponents; c++)
			{
				if (ComponentSubGraph[c] == -1)
				{
					continue;
				}

				TSharedPtr<FSubGraph> SubGraph = MakeShared<FSubGraph>();
				SubGraph->WeakParentGraph = SharedThis(this);
				SubGraph->Nodes.Reserve(ComponentNodeCounts[c]);
				SubGraph->Edges.Reserve(ComponentEdgeCounts[c]);
				SubGraphs.Add(SubGraph.ToSharedRef());
			}

			for (int32 i = 0; i < NumNodes; i++)
			{
				const int32 Component = NodeComponent[i];
				if (Component == -1)
				{
					continue;
				}

				const int32 SubGraphIndex = ComponentSubGraph[Component];
				if (SubGraphIndex == -1)
				{
					continue;
				}

				SubGraphs[SubGraphsBase + SubGraphIndex]->Nodes.Add(i);
			}

			for (const FEdge& Edge : Edges)
			{
				if (!Edge.bValid)
				{
					continue;
				}

				const int32 Component = NodeComponent[static_cast<int32>(Edge.Start)];
				if (Component == -1 || NodeComponent[static_cast<int32>(Edge.End)] == -1)
				{
					continue;
				}

				const int32 SubGraphIndex = ComponentSubGraph[Component];
				if (SubGraphIndex == -1)
				{
					continue;
				}

				SubGraphs[SubGraphsBase + SubGraphIndex]->Add(Edge);
			}

			OutValidNodes.Reserve(OutValidNodes.Num() + TotalExportedNodes);
			for (int32 s = SubGraphsBase; s < SubGraphs.Num(); s++)
			{
				OutValidNodes.Append(SubGraphs[s]->Nodes);
			}
		}

		// Recompute NumExportedEdges deterministically based on actual edge connections.
		// Count valid edges per node directly from Links - parallelizable and cache-friendly.
		// Nodes that are not exported keep their default of 0.
		PCGExMT::ParallelOrSequential(
			OutValidNodes.Num(),
			[&](const int32 i)
			{
				const int32 NodeIdx = OutValidNodes[i];
				FNode& Node = Nodes[NodeIdx];
				Node.NumExportedEdges = 0;
				for (const FLink& Lk : Node.Links)
				{
					const FEdge& Edge = Edges[Lk.Edge];
					if (Edge.bValid && Nodes[Edge.Other(NodeIdx)].bValid)
					{
						Node.NumExportedEdges++;
					}
				}
			});
	}

	void FGraph::GetConnectedNodes(const int32 FromIndex, TArray<int32>& OutIndices, const int32 SearchDepth) const
	{
		const int32 NextDepth = SearchDepth - 1;
		const FNode& RootNode = Nodes[FromIndex];

		for (const FLink Lk : RootNode.Links)
		{
			const FEdge& Edge = Edges[Lk.Edge];
			if (!Edge.bValid)
			{
				continue;
			}

			int32 OtherIndex = Edge.Other(FromIndex);
			if (OutIndices.Contains(OtherIndex))
			{
				continue;
			}

			OutIndices.Add(OtherIndex);
			if (NextDepth > 0)
			{
				GetConnectedNodes(OtherIndex, OutIndices, NextDepth);
			}
		}
	}
}
