// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExGeoDynMesh.h"

#include "PCGExH.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"

namespace PCGExMesh
{
	namespace Internal
	{
		/**
		 * Local vertex dedup via spatial hashing. Mirrors the logic in PCGExMesh.cpp
		 * but is self-contained here since the original is not exported.
		 */
		class FDynMeshLookup
		{
			TArray<FVector>* Vertices = nullptr;
			TArray<int32>* RawIndices = nullptr;
			FVector HashTolerance;
			bool bPrecise;
			TMap<uint64, int32> Data;

		public:
			explicit FDynMeshLookup(
				const int32 EstimatedSize,
				TArray<FVector>* InVertices,
				TArray<int32>* InRawIndices,
				const FVector& InTolerance,
				const bool bInPrecise)
				: Vertices(InVertices),
				  RawIndices(InRawIndices),
				  HashTolerance(PCGEx::SafeTolerance(InTolerance)),
				  bPrecise(bInPrecise)
			{
				Data.Reserve(EstimatedSize);
				Vertices->Reserve(EstimatedSize);
				if (RawIndices) { RawIndices->Reserve(EstimatedSize); }
			}

			uint32 Add_GetIdx(const FVector& Position, const int32 RawIndex)
			{
				const uint64 Key = PCGEx::SH3(Position, HashTolerance);

				if (const int32* IdxPtr = Data.Find(Key)) { return *IdxPtr; }

				if (bPrecise)
				{
					const uint64 OffsetKey = PCGEx::SH3(Position + (0.5 * HashTolerance), HashTolerance);
					if (OffsetKey != Key)
					{
						if (const int32* IdxPtr = Data.Find(OffsetKey)) { return *IdxPtr; }
					}

					const int32 Idx = AddVertex(Position, RawIndex);
					Data.Add(Key, Idx);
					if (OffsetKey != Key) { Data.Add(OffsetKey, Idx); }
					return Idx;
				}

				const int32 Idx = AddVertex(Position, RawIndex);
				Data.Add(Key, Idx);
				return Idx;
			}

		private:
			FORCEINLINE int32 AddVertex(const FVector& Position, const int32 RawIndex) const
			{
				const int32 Idx = Vertices->Emplace(Position);
				if (RawIndices) { RawIndices->Emplace(RawIndex); }
				return Idx;
			}
		};
	}

#pragma region FGeoDynMesh

	FGeoDynMesh::FGeoDynMesh(
		const UE::Geometry::FDynamicMesh3* InMesh,
		const FVector& InCWTolerance,
		const bool bInPreciseVertexMerge)
		: CWTolerance(InCWTolerance),
		  bPreciseVertexMerge(bInPreciseVertexMerge)
	{
		SourceMesh = InMesh;
		bIsValid = SourceMesh != nullptr && SourceMesh->TriangleCount() > 0;
	}

	void FGeoDynMesh::ExtractMeshSynchronous()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGeoDynMesh::ExtractMeshSynchronous);

		if (bIsLoaded) { return; }
		if (!bIsValid) { return; }

		const UE::Geometry::FDynamicMesh3& Mesh = *SourceMesh;
		const int32 EstimatedVertices = Mesh.MaxVertexID();

		Internal::FDynMeshLookup MeshLookup(EstimatedVertices, &Vertices, &RawIndices, CWTolerance, bPreciseVertexMerge);

		Edges.Reserve(Mesh.TriangleCount() * 3 / 2);
		VertexIDToDenseIndex.Reserve(EstimatedVertices);

		for (const int32 TriID : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriID);

			const uint32 A = MeshLookup.Add_GetIdx(Mesh.GetVertex(Tri.A), Tri.A);
			const uint32 B = MeshLookup.Add_GetIdx(Mesh.GetVertex(Tri.B), Tri.B);
			const uint32 C = MeshLookup.Add_GetIdx(Mesh.GetVertex(Tri.C), Tri.C);

			if (!VertexIDToDenseIndex.Contains(Tri.A)) { VertexIDToDenseIndex.Add(Tri.A, A); }
			if (!VertexIDToDenseIndex.Contains(Tri.B)) { VertexIDToDenseIndex.Add(Tri.B, B); }
			if (!VertexIDToDenseIndex.Contains(Tri.C)) { VertexIDToDenseIndex.Add(Tri.C, C); }

			if (A != B) { Edges.Add(PCGEx::H64U(A, B)); }
			if (B != C) { Edges.Add(PCGEx::H64U(B, C)); }
			if (C != A) { Edges.Add(PCGEx::H64U(C, A)); }
		}

		bIsLoaded = true;
	}

	void FGeoDynMesh::TriangulateMeshSynchronous()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FGeoDynMesh::TriangulateMeshSynchronous);

		if (bIsLoaded) { return; }
		if (!bIsValid) { return; }

		const UE::Geometry::FDynamicMesh3& Mesh = *SourceMesh;
		const int32 EstimatedVertices = Mesh.MaxVertexID();
		const int32 NumTriangles = Mesh.TriangleCount();

		Edges.Empty();

		Internal::FDynMeshLookup MeshLookup(EstimatedVertices, &Vertices, &RawIndices, CWTolerance, bPreciseVertexMerge);

		Triangles.Init(FIntVector3(-1), NumTriangles);
		Tri_Adjacency.Init(FIntVector3(-1), NumTriangles);

		TBitArray<> Tri_IsOnHull;
		Tri_IsOnHull.Init(true, NumTriangles);

		TMap<uint64, int32> EdgeMap;
		EdgeMap.Reserve(NumTriangles * 3 / 2);

		VertexIDToDenseIndex.Reserve(EstimatedVertices);

		auto PushAdjacency = [&](const int32 Tri, const int32 OtherTri)
		{
			FIntVector3& Adjacency = Tri_Adjacency[Tri];
			for (int i = 0; i < 3; i++)
			{
				if (Adjacency[i] == -1)
				{
					Adjacency[i] = OtherTri;
					if (i == 2) { Tri_IsOnHull[Tri] = false; }
					break;
				}
			}
		};

		auto PushEdge = [&](const int32 Tri, const uint64 Edge)
		{
			bool bIsAlreadySet = false;
			Edges.Add(Edge, &bIsAlreadySet);
			if (bIsAlreadySet)
			{
				if (int32 OtherTri = -1; EdgeMap.RemoveAndCopyValue(Edge, OtherTri))
				{
					PushAdjacency(OtherTri, Tri);
					PushAdjacency(Tri, OtherTri);
				}
			}
			else
			{
				EdgeMap.Add(Edge, Tri);
			}
		};

		int32 Ti = 0;
		for (const int32 TriID : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i SrcTri = Mesh.GetTriangle(TriID);

			const uint32 A = MeshLookup.Add_GetIdx(Mesh.GetVertex(SrcTri.A), SrcTri.A);
			const uint32 B = MeshLookup.Add_GetIdx(Mesh.GetVertex(SrcTri.B), SrcTri.B);
			const uint32 C = MeshLookup.Add_GetIdx(Mesh.GetVertex(SrcTri.C), SrcTri.C);

			if (!VertexIDToDenseIndex.Contains(SrcTri.A)) { VertexIDToDenseIndex.Add(SrcTri.A, A); }
			if (!VertexIDToDenseIndex.Contains(SrcTri.B)) { VertexIDToDenseIndex.Add(SrcTri.B, B); }
			if (!VertexIDToDenseIndex.Contains(SrcTri.C)) { VertexIDToDenseIndex.Add(SrcTri.C, C); }

			if (A == B || B == C || C == A) { continue; }

			Triangles[Ti] = FIntVector3(A, B, C);

			PushEdge(Ti, PCGEx::H64U(A, B));
			PushEdge(Ti, PCGEx::H64U(B, C));
			PushEdge(Ti, PCGEx::H64U(A, C));

			Ti++;
		}

		Triangles.SetNum(Ti);
		if (Triangles.IsEmpty())
		{
			bIsValid = false;
			return;
		}

		for (int i = 0; i < Triangles.Num(); i++)
		{
			const FIntVector3& Tri = Triangles[i];
			const int32 A = Tri[0];
			const int32 B = Tri[1];
			const int32 C = Tri[2];

			if (Tri_IsOnHull[i])
			{
				const uint64 AB = PCGEx::H64U(A, B);
				const uint64 BC = PCGEx::H64U(B, C);
				const uint64 AC = PCGEx::H64U(A, C);

				if (EdgeMap.Contains(AB))
				{
					HullIndices.Add(A);
					HullIndices.Add(B);
					HullEdges.Add(AB);
				}

				if (EdgeMap.Contains(BC))
				{
					HullIndices.Add(B);
					HullIndices.Add(C);
					HullEdges.Add(BC);
				}

				if (EdgeMap.Contains(AC))
				{
					HullIndices.Add(A);
					HullIndices.Add(C);
					HullEdges.Add(AC);
				}
			}
		}

		bIsLoaded = true;
	}

	bool FGeoDynMesh::GetAveragedVertexColors(TArray<FVector4f>& OutColors) const
	{
		if (!SourceMesh || !SourceMesh->HasAttributes()) { return false; }

		const UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = SourceMesh->Attributes()->PrimaryColors();
		if (!ColorOverlay || ColorOverlay->ElementCount() == 0) { return false; }

		const int32 NumVerts = Vertices.Num();
		OutColors.SetNumZeroed(NumVerts);
		TArray<int32> Counts;
		Counts.SetNumZeroed(NumVerts);

		for (const int32 TriID : SourceMesh->TriangleIndicesItr())
		{
			if (!ColorOverlay->IsSetTriangle(TriID)) { continue; }

			const UE::Geometry::FIndex3i SrcTri = SourceMesh->GetTriangle(TriID);
			const UE::Geometry::FIndex3i ColorTri = ColorOverlay->GetTriangle(TriID);

			for (int i = 0; i < 3; i++)
			{
				const int32* DenseIdx = VertexIDToDenseIndex.Find(SrcTri[i]);
				if (!DenseIdx) { continue; }

				const FVector4f Color = ColorOverlay->GetElement(ColorTri[i]);
				OutColors[*DenseIdx] += Color;
				Counts[*DenseIdx]++;
			}
		}

		for (int i = 0; i < NumVerts; i++)
		{
			if (Counts[i] > 0) { OutColors[i] /= static_cast<float>(Counts[i]); }
		}

		return true;
	}

	int32 FGeoDynMesh::GetNumUVChannels() const
	{
		if (!SourceMesh || !SourceMesh->HasAttributes()) { return 0; }
		return SourceMesh->Attributes()->NumUVLayers();
	}

	bool FGeoDynMesh::GetAveragedVertexUVs(const int32 Channel, TArray<FVector2f>& OutUVs) const
	{
		if (!SourceMesh || !SourceMesh->HasAttributes()) { return false; }
		if (Channel < 0 || Channel >= SourceMesh->Attributes()->NumUVLayers()) { return false; }

		const UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = SourceMesh->Attributes()->GetUVLayer(Channel);
		if (!UVOverlay || UVOverlay->ElementCount() == 0) { return false; }

		const int32 NumVerts = Vertices.Num();
		OutUVs.SetNumZeroed(NumVerts);
		TArray<int32> Counts;
		Counts.SetNumZeroed(NumVerts);

		for (const int32 TriID : SourceMesh->TriangleIndicesItr())
		{
			if (!UVOverlay->IsSetTriangle(TriID)) { continue; }

			const UE::Geometry::FIndex3i SrcTri = SourceMesh->GetTriangle(TriID);
			const UE::Geometry::FIndex3i UVTri = UVOverlay->GetTriangle(TriID);

			for (int i = 0; i < 3; i++)
			{
				const int32* DenseIdx = VertexIDToDenseIndex.Find(SrcTri[i]);
				if (!DenseIdx) { continue; }

				const FVector2f UV = UVOverlay->GetElement(UVTri[i]);
				OutUVs[*DenseIdx] += UV;
				Counts[*DenseIdx]++;
			}
		}

		for (int i = 0; i < NumVerts; i++)
		{
			if (Counts[i] > 0) { OutUVs[i] /= static_cast<float>(Counts[i]); }
		}

		return true;
	}

#pragma endregion
}
