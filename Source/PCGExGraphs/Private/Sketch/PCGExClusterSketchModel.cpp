// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExClusterSketchModel.h"

#include "PCGExH.h"
#include "Clusters/PCGExClusterCommon.h"

#pragma region FPCGExClusterDataChannel

int32 FPCGExClusterDataChannel::Num() const
{
	switch (Type)
	{
	case EPCGExClusterDataChannelType::Double: return DoubleValues.Num();
	case EPCGExClusterDataChannelType::Integer: return IntegerValues.Num();
	case EPCGExClusterDataChannelType::Name: return NameValues.Num();
	case EPCGExClusterDataChannelType::Vector: return VectorValues.Num();
	default: checkNoEntry();
		return 0;
	}
}

void FPCGExClusterDataChannel::SetNumDefaulted(const int32 InNum)
{
	DoubleValues.Empty();
	IntegerValues.Empty();
	NameValues.Empty();
	VectorValues.Empty();
	switch (Type)
	{
	case EPCGExClusterDataChannelType::Double: DoubleValues.SetNumZeroed(InNum);
		break;
	case EPCGExClusterDataChannelType::Integer: IntegerValues.SetNumZeroed(InNum);
		break;
	case EPCGExClusterDataChannelType::Name: NameValues.SetNum(InNum);
		break;
	case EPCGExClusterDataChannelType::Vector: VectorValues.SetNumZeroed(InNum);
		break;
	default: checkNoEntry();
		break;
	}
}

void FPCGExClusterDataChannel::RemoveAt(const int32 Index)
{
	switch (Type)
	{
	case EPCGExClusterDataChannelType::Double: if (DoubleValues.IsValidIndex(Index)) { DoubleValues.RemoveAt(Index); }
		break;
	case EPCGExClusterDataChannelType::Integer: if (IntegerValues.IsValidIndex(Index)) { IntegerValues.RemoveAt(Index); }
		break;
	case EPCGExClusterDataChannelType::Name: if (NameValues.IsValidIndex(Index)) { NameValues.RemoveAt(Index); }
		break;
	case EPCGExClusterDataChannelType::Vector: if (VectorValues.IsValidIndex(Index)) { VectorValues.RemoveAt(Index); }
		break;
	default: checkNoEntry();
		break;
	}
}

void FPCGExClusterDataChannel::InsertDefaulted(const int32 Index)
{
	switch (Type)
	{
	case EPCGExClusterDataChannelType::Double: DoubleValues.Insert(0.0, FMath::Clamp(Index, 0, DoubleValues.Num()));
		break;
	case EPCGExClusterDataChannelType::Integer: IntegerValues.Insert(0, FMath::Clamp(Index, 0, IntegerValues.Num()));
		break;
	case EPCGExClusterDataChannelType::Name: NameValues.Insert(NAME_None, FMath::Clamp(Index, 0, NameValues.Num()));
		break;
	case EPCGExClusterDataChannelType::Vector: VectorValues.Insert(FVector::ZeroVector, FMath::Clamp(Index, 0, VectorValues.Num()));
		break;
	default: checkNoEntry();
		break;
	}
}

#pragma endregion

#pragma region FPCGExClusterSketchModel

int32 FPCGExClusterSketchModel::AddVertex(const FTransform& InTransform)
{
	const int32 Index = Vertices.Num();
	FPCGExClusterSketchVertex& V = Vertices.AddDefaulted_GetRef();
	V.Transform = InTransform;
	for (FPCGExClusterDataChannel& Channel : VertexChannels)
	{
		Channel.InsertDefaulted(Index);
	}
	return Index;
}

int32 FPCGExClusterSketchModel::AddLatticeVertex(const FIntVector& InCoord, const FPCGExLatticeBasis& InBasis)
{
	const int32 Index = AddVertex(FTransform(InBasis.CoordToWorld(InCoord)));
	FPCGExClusterSketchVertex& V = Vertices[Index];
	V.bLatticeBound = true;
	V.LatticeCoord = InCoord;
	return Index;
}

bool FPCGExClusterSketchModel::RemoveVertex(const int32 Index)
{
	if (!Vertices.IsValidIndex(Index))
	{
		return false;
	}

	// Drop touching edges (descending, so earlier indices stay valid), then remap the survivors.
	for (int32 e = Edges.Num() - 1; e >= 0; --e)
	{
		if (Edges[e].A == Index || Edges[e].B == Index)
		{
			Edges.RemoveAt(e);
			for (FPCGExClusterDataChannel& Channel : EdgeChannels)
			{
				Channel.RemoveAt(e);
			}
		}
	}
	for (FPCGExClusterSketchEdge& E : Edges)
	{
		if (E.A > Index)
		{
			--E.A;
		}
		if (E.B > Index)
		{
			--E.B;
		}
	}

	Vertices.RemoveAt(Index);
	for (FPCGExClusterDataChannel& Channel : VertexChannels)
	{
		Channel.RemoveAt(Index);
	}
	return true;
}

int32 FPCGExClusterSketchModel::Connect(const int32 A, const int32 B)
{
	if (A == B || !Vertices.IsValidIndex(A) || !Vertices.IsValidIndex(B))
	{
		return INDEX_NONE;
	}

	const int32 Existing = FindEdge(A, B);
	if (Existing != INDEX_NONE)
	{
		return Existing;
	}

	const int32 Index = Edges.Num();
	FPCGExClusterSketchEdge& E = Edges.AddDefaulted_GetRef();
	E.A = A;
	E.B = B;
	for (FPCGExClusterDataChannel& Channel : EdgeChannels)
	{
		Channel.InsertDefaulted(Index);
	}
	return Index;
}

bool FPCGExClusterSketchModel::Disconnect(const int32 A, const int32 B)
{
	const int32 Index = FindEdge(A, B);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	Edges.RemoveAt(Index);
	for (FPCGExClusterDataChannel& Channel : EdgeChannels)
	{
		Channel.RemoveAt(Index);
	}
	return true;
}

int32 FPCGExClusterSketchModel::FindEdge(const int32 A, const int32 B) const
{
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const FPCGExClusterSketchEdge& E = Edges[e];
		if ((E.A == A && E.B == B) || (E.A == B && E.B == A))
		{
			return e;
		}
	}
	return INDEX_NONE;
}

bool FPCGExClusterSketchModel::SetLatticeBound(const int32 Index, const bool bBound, const FPCGExLatticeBasis& InBasis)
{
	if (!Vertices.IsValidIndex(Index))
	{
		return false;
	}

	FPCGExClusterSketchVertex& V = Vertices[Index];
	if (V.bLatticeBound == bBound)
	{
		return true;
	}

	V.bLatticeBound = bBound;
	if (bBound)
	{
		V.LatticeCoord = InBasis.SnapWorldToCoord(V.Transform.GetLocation());
		V.Transform.SetLocation(InBasis.CoordToWorld(V.LatticeCoord));
	}
	// Unbinding keeps the current derived location as the free position -- nothing to do.
	return true;
}

void FPCGExClusterSketchModel::SyncBoundVertices(const FPCGExLatticeBasis& InBasis, const bool bResnapFromLocation)
{
	for (FPCGExClusterSketchVertex& V : Vertices)
	{
		if (!V.bLatticeBound)
		{
			continue;
		}
		if (bResnapFromLocation)
		{
			V.LatticeCoord = InBasis.SnapWorldToCoord(V.Transform.GetLocation());
		}
		V.Transform.SetLocation(InBasis.CoordToWorld(V.LatticeCoord));
	}
}

void FPCGExClusterSketchModel::Validate(FPCGExClusterSketchValidation& OutSummary) const
{
	OutSummary = FPCGExClusterSketchValidation();

	const int32 NumVtx = Vertices.Num();

	TSet<uint64> Seen;
	TArray<int32> Degree;
	Degree.SetNumZeroed(NumVtx);
	for (const FPCGExClusterSketchEdge& E : Edges)
	{
		if (E.A < 0 || E.B < 0 || E.A >= NumVtx || E.B >= NumVtx)
		{
			++OutSummary.InvalidEdges;
			continue;
		}
		if (E.A == E.B)
		{
			++OutSummary.SelfLoops;
			continue;
		}
		bool bAlreadySeen = false;
		Seen.Add(PCGEx::H64U(static_cast<uint32>(E.A), static_cast<uint32>(E.B)), &bAlreadySeen);
		if (bAlreadySeen)
		{
			++OutSummary.DuplicateEdges;
			continue;
		}
		++Degree[E.A];
		++Degree[E.B];
	}

	for (int32 i = 0; i < NumVtx; ++i)
	{
		if (Degree[i] == 0)
		{
			++OutSummary.IsolatedVertices;
		}
	}

	auto ValidateChannels = [&OutSummary](const TArray<FPCGExClusterDataChannel>& Channels, const int32 DomainCount)
	{
		TSet<FName> Names;
		for (const FPCGExClusterDataChannel& Channel : Channels)
		{
			bool bAlreadySeen = false;
			Names.Add(Channel.Name, &bAlreadySeen);
			if (Channel.Name.IsNone() || bAlreadySeen || PCGExClusters::Labels::ProtectedClusterAttributes.Contains(Channel.Name))
			{
				OutSummary.InvalidChannelNames.AddUnique(Channel.Name);
			}
			if (Channel.Num() != DomainCount)
			{
				OutSummary.MisalignedChannels.AddUnique(Channel.Name);
			}
		}
	};
	ValidateChannels(VertexChannels, NumVtx);
	ValidateChannels(EdgeChannels, Edges.Num());
}

#pragma endregion
