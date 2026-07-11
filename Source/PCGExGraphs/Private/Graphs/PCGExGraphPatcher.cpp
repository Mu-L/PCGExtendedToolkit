// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graphs/PCGExGraphPatcher.h"

#include "PCGExH.h"
#include "PCGExLog.h"
#include "Clusters/PCGExClusterCommon.h"
#include "Clusters/PCGExClustersHelpers.h"
#include "Data/PCGExClusterData.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataTags.h"
#include "Data/PCGExPointIO.h"
#include "Data/PCGPointArrayData.h"
#include "Data/Buffers/PCGExBuffer.h"
#include "Helpers/PCGExPointArrayDataHelpers.h"
#include "Utils/PCGExIntTracker.h"
#include "Utils/PCGExPointIOMerger.h"

namespace PCGExGraphs
{
#pragma region FVtxMerger

	int32 FVtxMerger::AddSource(const TSharedPtr<PCGExData::FPointIO>& InVtxIO)
	{
		const int32 SourceIndex = Sources.Num();
		FSource& Source = Sources.Emplace_GetRef();
		Source.VtxIO = InVtxIO;
		Source.Offset = TotalNum;
		TotalNum += InVtxIO->GetNum();
		return SourceIndex;
	}

	void FVtxMerger::MergeAsync(FPCGExContext* InContext, const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager, const FPCGExCarryOverDetails* InCarryOver)
	{
		ScratchIO = PCGExData::NewPointIO(InContext);
		if (!ScratchIO->InitializeOutput<UPCGExClusterNodesData>(PCGExData::EIOInit::New))
		{
			ScratchIO.Reset();
			return;
		}

		ScratchFacade = MakeShared<PCGExData::FFacade>(ScratchIO.ToSharedRef());

		Merger = MakeShared<FPCGExPointIOMerger>(ScratchFacade.ToSharedRef());
		for (const FSource& Source : Sources)
		{
			// Sequential full-range appends keep the merger's write offsets aligned with Source.Offset.
			Merger->Append(Source.VtxIO);
		}

		Merger->MergeAsync(InTaskManager, InCarryOver, nullptr, true);
	}

	TSharedPtr<PCGExData::FFacade> FVtxMerger::Finalize(FPCGExContext* InContext, const FName InOutputPin)
	{
		if (!ScratchIO)
		{
			return nullptr;
		}

		const UPCGBasePointData* Merged = ScratchIO->GetOut();
		if (!Merged || Merged->IsEmpty())
		{
			return nullptr;
		}

		// The merged data becomes the working In (broadcaster-readable); Out is a mutable duplicate.
		// ScratchIO stays alive on this object so the managed In data survives until execution ends.
		WorkingIO = PCGExData::NewPointIO(InContext, Merged, InOutputPin);
		WorkingIO->Tags->Append(ScratchIO->Tags.ToSharedRef());

		if (!WorkingIO->InitializeOutput(PCGExData::EIOInit::Duplicate))
		{
			return nullptr;
		}

		return MakeShared<PCGExData::FFacade>(WorkingIO.ToSharedRef());
	}

#pragma endregion

#pragma region FGraphPatcher

	FGraphPatcher::FGraphPatcher(const TSharedRef<PCGExData::FFacade>& InVtxFacade)
		: VtxFacade(InVtxFacade)
	{
		NumInitialVtx = VtxFacade->GetOut()->GetNumPoints();
	}

	int32 FGraphPatcher::AddVtxSource(const TSharedPtr<PCGExData::FPointIO>& InVtxIO, const int32 InOffset)
	{
		const int32 SourceIndex = VtxSources.Num();
		FVtxSource& Source = VtxSources.Emplace_GetRef();
		Source.VtxIO = InVtxIO;
		Source.Offset = InOffset;
		return SourceIndex;
	}

	int32 FGraphPatcher::AddEdgeGroup(const TSharedPtr<PCGExData::FPointIO>& InEdgesIO, const TArray<int32>& InVtxPointIndices, const int32 InVtxSourceIndex)
	{
		const int32 GroupIndex = Groups.Num();
		FEdgeGroup& Group = Groups.Emplace_GetRef();
		Group.EdgesIO = InEdgesIO;
		Group.VtxPointIndices = InVtxPointIndices;
		Group.VtxSourceIndex = InVtxSourceIndex;
		return GroupIndex;
	}

	int32 FGraphPatcher::AddVtx(const FTransform& InTransform)
	{
		const int32 Index = NumInitialVtx + NewVtxTransforms.Num();
		NewVtxTransforms.Add(InTransform);
		return Index;
	}

	int32 FGraphPatcher::AddEdge(const int32 VtxPointIndexA, const int32 VtxPointIndexB)
	{
		const int32 Handle = PendingEdges.Num();
		PendingEdges.Add(FPendingEdge{VtxPointIndexA, VtxPointIndexB});
		return Handle;
	}

	int32 FGraphPatcher::Find(const int32 X)
	{
		int32 Root = X;
		while (DSU[Root] != Root)
		{
			Root = DSU[Root];
		}
		int32 Cur = X;
		while (DSU[Cur] != Root)
		{
			const int32 Next = DSU[Cur];
			DSU[Cur] = Root;
			Cur = Next;
		}
		return Root;
	}

	void FGraphPatcher::DSUUnion(const int32 A, const int32 B)
	{
		const int32 RA = Find(A);
		const int32 RB = Find(B);
		if (RA != RB)
		{
			DSU[RA] = RB;
		}
	}

	int32 FGraphPatcher::VtxElement(const int32 VtxPointIndex) const
	{
		const int32* E = VtxToElement.Find(VtxPointIndex);
		return E ? *E : INDEX_NONE;
	}

	void FGraphPatcher::ResolveAndMergeAsync(
		const TSharedRef<PCGExData::FPointIOCollection>& OutEdges,
		const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager,
		const FPCGExCarryOverDetails* InCarryOver)
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;

		const int32 G = Groups.Num();
		const int32 B = NewVtxTransforms.Num();
		if (G == 0)
		{
			// No edge groups means no component to anchor staged edges to. Staged edges must reference
			// registered group vtx, so this is only reachable on misuse.
			check(PendingEdges.IsEmpty());
			return;
		}

		// vtx point index -> union-find element (groups first, then staged vtx)
		VtxToElement.Reserve(NumInitialVtx + B);
		for (int32 g = 0; g < G; ++g)
		{
			for (const int32 P : Groups[g].VtxPointIndices)
			{
				// A vtx point index must belong to exactly one group; a collision would silently
				// merge two components' element mapping.
				check(!VtxToElement.Contains(P));
				VtxToElement.Add(P, g);
			}
		}
		for (int32 i = 0; i < B; ++i)
		{
			VtxToElement.Add(NumInitialVtx + i, G + i);
		}

		// union groups linked by staged edges (directly, or through a staged vtx)
		DSU.SetNumUninitialized(G + B);
		for (int32 i = 0; i < DSU.Num(); ++i)
		{
			DSU[i] = i;
		}
		for (const FPendingEdge& E : PendingEdges)
		{
			const int32 EA = VtxElement(E.A);
			const int32 EB = VtxElement(E.B);
			if (EA != INDEX_NONE && EB != INDEX_NONE)
			{
				DSUUnion(EA, EB);
			}
		}

		// one output edge collection per connected component (keyed by group-root)
		TMap<int32, int32> RootToComponent;
		RootToComponent.Reserve(G);
		auto GetComponent = [&](const int32 Element) -> int32
		{
			const int32 Root = Find(Element);
			if (const int32* C = RootToComponent.Find(Root))
			{
				return *C;
			}
			const int32 CompIdx = ComponentEdgeIOs.Num();
			RootToComponent.Add(Root, CompIdx);

			const TSharedPtr<PCGExData::FPointIO> IO = OutEdges->Emplace_GetRef<UPCGExClusterEdgesData>(PCGExData::EIOInit::New);
			const TSharedRef<PCGExData::FFacade> Facade = MakeShared<PCGExData::FFacade>(IO.ToSharedRef());

			ComponentEdgeIOs.Add(IO);
			ComponentEdgeFacades.Add(Facade);
			Mergers.Add(MakeShared<FPCGExPointIOMerger>(Facade));
			return CompIdx;
		};

		// route each input group's edges to its component's merger (merger carries the source tags over)
		for (int32 g = 0; g < G; ++g)
		{
			// Merged-sources mode: a group without a source association would keep stale endpoint ids.
			check(VtxSources.IsEmpty() || Groups[g].VtxSourceIndex != INDEX_NONE);

			const int32 CompIdx = GetComponent(g);
			Groups[g].ComponentIndex = CompIdx;
			Groups[g].OutWriteScope = Mergers[CompIdx]->Append(Groups[g].EdgesIO).Write;
		}

		// route each staged edge to its component
		for (FPendingEdge& E : PendingEdges)
		{
			const int32 Elem = VtxElement(E.A);
			if (Elem == INDEX_NONE)
			{
				continue;
			}
			E.ComponentIndex = RootToComponent[Find(Elem)];
		}

		// vtx/edge pairing happens in Commit(), which must run after these async merges (see Commit).
		for (const TSharedPtr<FPCGExPointIOMerger>& Merger : Mergers)
		{
			Merger->MergeAsync(InTaskManager, InCarryOver, nullptr, true);
		}
	}

	void FGraphPatcher::Commit()
	{
		if (bCommitted)
		{
			return;
		}
		bCommitted = true;

		UPCGBasePointData* VtxData = VtxFacade->GetOut();
		FPCGMetadataAttribute<int64>* VtxIdxAttr = VtxData->MutableMetadata()->FindOrCreateAttribute<int64>(PCGExClusters::Labels::Attr_PCGExVtxIdx);
		if (!VtxIdxAttr)
		{
			return;
		} // not compiled cluster vtx

		const int32 NumNewVtx = NewVtxTransforms.Num();

		// ---- Grow + fill staged vtx (shared, once) ----
		if (NumNewVtx > 0)
		{
			PCGExPointArrayDataHelpers::SetNumPointsAllocated(
				VtxData, NumInitialVtx + NumNewVtx,
				EPCGPointNativeProperties::Transform | EPCGPointNativeProperties::BoundsMin |
				EPCGPointNativeProperties::BoundsMax | EPCGPointNativeProperties::Seed);

			TPCGValueRange<int64> NewEntries = VtxData->GetMetadataEntryValueRange();
			TPCGValueRange<FTransform> NewTransforms = VtxData->GetTransformValueRange(false);
			for (int32 i = 0; i < NumNewVtx; ++i)
			{
				const int32 P = NumInitialVtx + i;
				VtxData->Metadata->InitializeOnSet(NewEntries[P]);
				NewTransforms[P] = NewVtxTransforms[i];
				// Endpoint id of a new vtx is its own point index; edge count is set in the bump pass below.
				VtxIdxAttr->SetValue(NewEntries[P], static_cast<int64>(PCGEx::H64(static_cast<uint32>(P), 0)));
			}
		}

		TConstPCGValueRange<int64> VtxEntries = VtxData->GetConstMetadataEntryValueRange();
		TConstPCGValueRange<FTransform> VtxTransforms = VtxData->GetConstTransformValueRange();

		// ---- Merged-sources endpoint renumbering ----
		// Per-source endpoint ids collide once datasets are merged; renumber every initial vtx to
		// H64(PointIndex, Adjacency) and rewrite the merged edge groups' endpoint refs to match.
		// Runs before anything below reads VtxIdxAttr, so staged edges & count bumps see final ids.
		if (!VtxSources.IsEmpty())
		{
			const int32 NumVtxSources = VtxSources.Num();

			// Old endpoint id -> merged point index, per source (ids are only unique per source).
			TArray<TMap<uint32, uint32>> OldToNew;
			OldToNew.SetNum(NumVtxSources);

			bool bReadFailure = false;

			for (int32 s = 0; s < NumVtxSources; ++s)
			{
				const FVtxSource& Source = VtxSources[s];

				const TUniquePtr<PCGExData::TArrayBuffer<int64>> IdBuffer = MakeUnique<PCGExData::TArrayBuffer<int64>>(Source.VtxIO.ToSharedRef(), PCGExClusters::Labels::Attr_PCGExVtxIdx);
				if (!IdBuffer->InitForRead())
				{
					bReadFailure = true;
					continue;
				}

				const TArray<int64>& OldValues = *IdBuffer->GetInValues().Get();

				TMap<uint32, uint32>& Map = OldToNew[s];
				Map.Reserve(OldValues.Num());

				for (int32 i = 0; i < OldValues.Num(); ++i)
				{
					const int32 P = Source.Offset + i;

					uint32 OldId;
					uint32 Adjacency;
					PCGEx::H64(static_cast<uint64>(OldValues[i]), OldId, Adjacency);

					Map.Add(OldId, static_cast<uint32>(P));
					VtxIdxAttr->SetValue(VtxEntries[P], static_cast<int64>(PCGEx::H64(static_cast<uint32>(P), Adjacency)));
				}
			}

			bool bUnresolvedEndpoints = bReadFailure;

			for (const FEdgeGroup& Group : Groups)
			{
				if (Group.VtxSourceIndex == INDEX_NONE || Group.ComponentIndex == INDEX_NONE || Group.OutWriteScope.Count <= 0)
				{
					continue;
				}

				const TMap<uint32, uint32>& Map = OldToNew[Group.VtxSourceIndex];

				UPCGBasePointData* GroupEdgeData = ComponentEdgeFacades[Group.ComponentIndex]->GetOut();
				FPCGMetadataAttribute<int64>* GroupEdgeIdxAttr = GroupEdgeData->MutableMetadata()->FindOrCreateAttribute<int64>(PCGExClusters::Labels::Attr_PCGExEdgeIdx);
				if (!GroupEdgeIdxAttr)
				{
					continue;
				}

				const TConstPCGValueRange<int64> GroupEdgeEntries = GroupEdgeData->GetConstMetadataEntryValueRange();

				for (int32 p = Group.OutWriteScope.Start; p < Group.OutWriteScope.End; ++p)
				{
					uint32 OldA;
					uint32 OldB;
					PCGEx::H64(static_cast<uint64>(GroupEdgeIdxAttr->GetValueFromItemKey(GroupEdgeEntries[p])), OldA, OldB);

					const uint32* NewA = Map.Find(OldA);
					const uint32* NewB = Map.Find(OldB);
					if (!NewA || !NewB)
					{
						bUnresolvedEndpoints = true;
						continue;
					}

					GroupEdgeIdxAttr->SetValue(GroupEdgeEntries[p], static_cast<int64>(PCGEx::H64(*NewA, *NewB)));
				}
			}

			UE_CLOG(bUnresolvedEndpoints, LogPCGEx, Warning, TEXT("Graph patcher: some merged endpoints could not be resolved; input vtx/edges pairing is likely corrupt."));
		}

		// Existing vtx keep their stored endpoint id; staged vtx use their own point index.
		auto VtxEndpointId = [&](const int32 V) -> uint32
		{
			if (V >= NumInitialVtx)
			{
				return static_cast<uint32>(V);
			}
			return PCGEx::H64A(static_cast<uint64>(VtxIdxAttr->GetValueFromItemKey(VtxEntries[V])));
		};

		// ---- Append + patch staged edges, per component ----
		TMap<int32, int32> EdgeCountDelta;

		const int32 NumComponents = ComponentEdgeIOs.Num();
		for (int32 c = 0; c < NumComponents; ++c)
		{
			int32 NumNewEdges = 0;
			for (const FPendingEdge& E : PendingEdges)
			{
				if (E.ComponentIndex == c)
				{
					++NumNewEdges;
				}
			}
			if (NumNewEdges == 0)
			{
				continue;
			}

			UPCGBasePointData* EdgeData = ComponentEdgeFacades[c]->GetOut();
			FPCGMetadataAttribute<int64>* EdgeIdxAttr = EdgeData->MutableMetadata()->FindOrCreateAttribute<int64>(PCGExClusters::Labels::Attr_PCGExEdgeIdx);
			if (!EdgeIdxAttr)
			{
				continue;
			}

			const int32 FirstNewEdge = EdgeData->GetNumPoints();
			EdgeData->SetNumPoints(FirstNewEdge + NumNewEdges);
			EdgeData->AllocateProperties(EPCGPointNativeProperties::Transform);

			TPCGValueRange<int64> EdgeEntries = EdgeData->GetMetadataEntryValueRange();
			TPCGValueRange<FTransform> EdgeTransforms = EdgeData->GetTransformValueRange(false);

			int32 LocalEdge = 0;
			for (FPendingEdge& E : PendingEdges)
			{
				if (E.ComponentIndex != c)
				{
					continue;
				}
				const int32 EdgeP = FirstNewEdge + LocalEdge++;
				E.EdgePointIndex = EdgeP;

				EdgeData->Metadata->InitializeOnSet(EdgeEntries[EdgeP]);
				EdgeIdxAttr->SetValue(EdgeEntries[EdgeP], static_cast<int64>(PCGEx::H64(VtxEndpointId(E.A), VtxEndpointId(E.B))));
				EdgeTransforms[EdgeP].SetLocation(FMath::Lerp(VtxTransforms[E.A].GetLocation(), VtxTransforms[E.B].GetLocation(), 0.5));

				EdgeCountDelta.FindOrAdd(E.A)++;
				EdgeCountDelta.FindOrAdd(E.B)++;
			}
		}

		// ---- Bump per-vtx edge counts (independent keys; map iteration order is irrelevant) ----
		for (const TPair<int32, int32>& It : EdgeCountDelta)
		{
			const int64 Key = VtxEntries[It.Key];
			const uint64 Current = static_cast<uint64>(VtxIdxAttr->GetValueFromItemKey(Key));
			VtxIdxAttr->SetValue(Key, static_cast<int64>(PCGEx::H64(PCGEx::H64A(Current), PCGEx::H64B(Current) + static_cast<uint32>(It.Value))));
		}

		// Pair vtx <-> edges LAST: the async merges Append the source edges' old cluster tags onto each
		// output, which would clobber an earlier pairing. Derive the PairId from the vtx, then mark both.
		const PCGExDataId PairId = VtxFacade->Source->Tags->Set<int64>(
			PCGExClusters::Labels::TagStr_PCGExCluster, VtxFacade->Source->GetOutIn()->GetUniqueID());
		PCGExClusters::Helpers::MarkClusterVtx(VtxFacade->Source, PairId);
		for (const TSharedPtr<PCGExData::FPointIO>& IO : ComponentEdgeIOs)
		{
			PCGExClusters::Helpers::MarkClusterEdges(IO, PairId);
		}
	}

	bool FGraphPatcher::GetEdgeOutput(const int32 EdgeHandle, TSharedPtr<PCGExData::FPointIO>& OutEdgesIO, int32& OutEdgePointIndex) const
	{
		if (!PendingEdges.IsValidIndex(EdgeHandle))
		{
			return false;
		}
		const FPendingEdge& E = PendingEdges[EdgeHandle];
		if (E.ComponentIndex == INDEX_NONE || E.EdgePointIndex == INDEX_NONE)
		{
			return false;
		}
		OutEdgesIO = ComponentEdgeIOs[E.ComponentIndex];
		OutEdgePointIndex = E.EdgePointIndex;
		return true;
	}

#pragma endregion
}
