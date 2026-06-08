// Copyright 2026 Timothe Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Core/PCGExClipper2Decomposition.h"

#include "Algo/Reverse.h"
#include "Clipper2Lib/clipper.triangulation.h"
#include "Internationalization/Text.h" // FText / LOCTEXT used by DescribeDecomposeFailure

#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Math/Geo/PCGExGeo.h"

#define LOCTEXT_NAMESPACE "PCGExClipper2Decomposition"

// File-local helpers live in the namespace matching this file (Unity-build safe -- see project build notes).
namespace PCGExClipper2Decomposition
{
	// 2D cross of (A-O) and (B-O); positive when O->A->B turns left (CCW). Shares the plugin-wide 2D
	// determinant primitive instead of carrying a private copy.
	FORCEINLINE double Cross2D(const FVector2D& O, const FVector2D& A, const FVector2D& B)
	{
		return PCGExMath::Geo::Det(A - O, B - O);
	}

	double SignedArea(const TArray<int32>& Loop, const TArray<FFootprintVertex>& Pool)
	{
		double Area = 0;
		const int32 N = Loop.Num();
		for (int32 i = 0; i < N; i++)
		{
			const FVector2D& A = Pool[Loop[i]].Pos;
			const FVector2D& B = Pool[Loop[(i + 1) % N]].Pos;
			Area += PCGExMath::Geo::Det(A, B); // shoelace term == 2D determinant of consecutive vertices
		}
		return 0.5 * Area;
	}

	// Reorder a vertex-index loop to CCW (positive signed area) in projection X/Y.
	void EnsureCCW(TArray<int32>& Loop, const TArray<FFootprintVertex>& Pool)
	{
		if (SignedArea(Loop, Pool) < 0) { Algo::Reverse(Loop); }
	}

	// True if the loop is convex assuming CCW winding (reflex vertices -> false). Near-collinear is allowed.
	bool IsConvexCCW(const TArray<int32>& Loop, const TArray<FFootprintVertex>& Pool)
	{
		const int32 N = Loop.Num();
		if (N < 3) { return false; }
		for (int32 i = 0; i < N; i++)
		{
			const FVector2D& O = Pool[Loop[i]].Pos;
			const FVector2D& A = Pool[Loop[(i + 1) % N]].Pos;
			const FVector2D& B = Pool[Loop[(i + 2) % N]].Pos;
			if (Cross2D(O, A, B) < -UE_KINDA_SMALL_NUMBER) { return false; }
		}
		return true;
	}

	// If A and B (both CCW loops) share an edge (u->v in A, v->u in B), merge along it.
	// Returns true and fills OutMerged only when the merged polygon is convex.
	//
	// Two convex polygons with disjoint interiors share AT MOST one edge (sharing two would force one of
	// them concave around the shared corner), so the first matching half-edge is the only merge candidate:
	// bailing on it -- rather than continuing to scan for another shared edge -- cannot miss a valid merge.
	bool TryMergeConvex(const TArray<int32>& A, const TArray<int32>& B, const TArray<FFootprintVertex>& Pool, TArray<int32>& OutMerged)
	{
		const int32 NA = A.Num();
		const int32 NB = B.Num();

		for (int32 ia = 0; ia < NA; ia++)
		{
			const int32 U = A[ia];
			const int32 V = A[(ia + 1) % NA];

			for (int32 ib = 0; ib < NB; ib++)
			{
				if (B[ib] != V || B[(ib + 1) % NB] != U) { continue; }

				// Shared edge found (unique between two simple polygons). Build the merged loop:
				// walk A from V around to U, then B's interior from after U to before V.
				TArray<int32> Merged;
				Merged.Reserve(NA + NB - 2);
				for (int32 k = 0; k < NA; k++) { Merged.Add(A[(ia + 1 + k) % NA]); } // V ... U
				for (int32 k = 0; k < NB - 2; k++) { Merged.Add(B[(ib + 2 + k) % NB]); } // interior of B

				if (Merged.Num() >= 3 && IsConvexCCW(Merged, Pool))
				{
					OutMerged = MoveTemp(Merged);
					return true;
				}
				return false; // single shared edge, not convex -> cannot merge these two
			}
		}
		return false;
	}

	// Greedy Hertel-Mehlhorn-style convex merge of a triangulation into fewer convex pieces.
	//
	// IMPORTANT: every successful merge restarts the whole i/j scan from the start (that is what the
	// `&& !bMerged` loop guards + the outer while do). The restart is INTENTIONAL and load-bearing for output
	// QUALITY -- it is NOT a missed optimization. Greedy convex merging is order-sensitive: "merge the first
	// available pair, then re-scan from the beginning" immediately re-pairs a freshly-grown piece against the
	// earlier pieces, which yields markedly FEWER convex pieces than absorbing each piece's neighbours in one
	// forward sweep. It is O(n^3) in the piece count (bounded by MaxConvexPieces), but the better
	// decomposition is worth it -- do NOT replace the restart with a forward-sweep/fixpoint variant (measured
	// to produce MORE pieces).
	void MergeIntoConvexPieces(TArray<TArray<int32>>& Pieces, const TArray<FFootprintVertex>& Pool)
	{
		bool bMerged = true;
		while (bMerged && Pieces.Num() > 1)
		{
			bMerged = false;
			for (int32 i = 0; i < Pieces.Num() && !bMerged; i++)
			{
				for (int32 j = i + 1; j < Pieces.Num() && !bMerged; j++)
				{
					TArray<int32> Result;
					if (TryMergeConvex(Pieces[i], Pieces[j], Pool, Result))
					{
						Pieces[i] = MoveTemp(Result);
						Pieces.RemoveAt(j);
						bMerged = true;
					}
				}
			}
		}
	}

	FDecomposeResult Decompose(
		const PCGExClipper2Lib::Paths64& SubjectPaths,
		const TSharedPtr<PCGExClipper2::FOpData>& AllOpData,
		const PCGExClipper2Lib::ZCallback64& ZCallback,
		const FDecomposeParams& Params)
	{
		FDecomposeResult Out;

		const double InvScale = 1.0 / static_cast<double>(Params.Precision);

		// --- Boundary-respecting triangulation (holes honored via fill rule) ---
		PCGExClipper2Lib::Paths64 CombinedPaths;
		CombinedPaths.reserve(SubjectPaths.size());
		for (const auto& Path : SubjectPaths) { CombinedPaths.push_back(Path); }

		PCGExClipper2Lib::Paths64 TrianglePaths;
		const PCGExClipper2Lib::TriangulateResult Result = PCGExClipper2Lib::TriangulateWithHoles(
			CombinedPaths, TrianglePaths, PCGExClipper2::ConvertFillRule(Params.FillRule), Params.bUseDelaunay, ZCallback);

		if (Result != PCGExClipper2Lib::TriangulateResult::success || TrianglePaths.empty())
		{
			Out.Status = EDecomposeResult::TriangulationFailed;
			return Out;
		}

		// --- Deduplicated 2D vertex pool, each vertex mapped back to its source point when possible ---
		const int32 EstimatedVerts = static_cast<int32>(TrianglePaths.size()) * 3;
		TMap<uint64, int32> VertexMap;
		Out.VertexPool.Reserve(EstimatedVerts);
		VertexMap.Reserve(EstimatedVerts);

		auto FindOrAddVertex = [&](const PCGExClipper2Lib::Point64& Pt) -> int32
		{
			// Dedup key = low 32 bits of each int64 Clipper coordinate, packed. This is exact (injective) only
			// while |Pt.x|,|Pt.y| < 2^31 SCALED units, i.e. ~+-21M / Precision in world cm (about +-215km at the
			// default Precision=100, but only +-2km at Precision=10000). Beyond that the high bits are dropped and
			// two genuinely distinct vertices can collide -> silently welded mesh (degenerate prisms / lost edges).
			// If that ever bites in production (huge footprints or very high Precision), key on the full int64 pair
			// (e.g. FIntVector2) or a properly-mixed 64-bit hash of both full coords instead of truncating.
			const uint64 Hash = PCGEx::H64(static_cast<uint32>(Pt.x & 0xFFFFFFFF), static_cast<uint32>(Pt.y & 0xFFFFFFFF));
			if (const int32* Found = VertexMap.Find(Hash)) { return *Found; }

			FFootprintVertex V;
			V.Pos = FVector2D(static_cast<double>(Pt.x) * InvScale, static_cast<double>(Pt.y) * InvScale);

			uint32 RawPointIdx, RawSourceIdx;
			PCGEx::H64(static_cast<uint64>(Pt.z), RawPointIdx, RawSourceIdx);

			if (RawPointIdx != PCGExClipper2::INTERSECTION_MARKER)
			{
				const int32 SrcIdx = static_cast<int32>(RawSourceIdx);
				const int32 PtIdx = static_cast<int32>(RawPointIdx);

				if (AllOpData->Facades.IsValidIndex(SrcIdx))
				{
					const int32 SrcNum = AllOpData->Facades[SrcIdx]->Source->GetNum(PCGExData::EIOSide::In);
					if (PtIdx < SrcNum)
					{
						V.SourceIdx = SrcIdx;
						V.SourcePointIdx = PtIdx;
						V.bHasSource = true;

						if (AllOpData->ProjectedZValues.IsValidIndex(SrcIdx) && AllOpData->ProjectedZValues[SrcIdx].IsValidIndex(PtIdx))
						{
							V.ProjectedZ = AllOpData->ProjectedZValues[SrcIdx][PtIdx];
						}
					}
				}
			}

			const int32 Index = Out.VertexPool.Num();
			VertexMap.Add(Hash, Index);
			Out.VertexPool.Add(V);
			return Index;
		};

		Out.Pieces.Reserve(static_cast<int32>(TrianglePaths.size()));
		for (const auto& Tri : TrianglePaths)
		{
			if (Tri.size() != 3) { continue; }
			const int32 A = FindOrAddVertex(Tri[0]);
			const int32 B = FindOrAddVertex(Tri[1]);
			const int32 C = FindOrAddVertex(Tri[2]);
			if (A == B || B == C || C == A) { continue; }

			TArray<int32> Piece = {A, B, C};
			EnsureCCW(Piece, Out.VertexPool);
			Out.Pieces.Add(MoveTemp(Piece));
		}

		if (Out.Pieces.IsEmpty() || Out.VertexPool.IsEmpty())
		{
			Out.Status = EDecomposeResult::Empty;
			return Out;
		}

		if (Params.bMergeConvexPieces)
		{
			MergeIntoConvexPieces(Out.Pieces, Out.VertexPool);
		}

		if (Out.Pieces.Num() > Params.MaxConvexPieces)
		{
			Out.Status = EDecomposeResult::TooManyPieces;
			return Out;
		}

		Out.Status = EDecomposeResult::Success;
		return Out;
	}

	bool TryDecomposeGroup(
		const TSharedPtr<PCGExClipper2::FProcessingGroup>& Group,
		const TSharedPtr<PCGExClipper2::FOpData>& AllOpData,
		const FDecomposeParams& Params,
		FDecomposeResult& OutResult)
	{
		OutResult = FDecomposeResult(); // Status == Empty (silent skip) by default

		if (!Group->IsValid() || Group->SubjectPaths.empty() || Group->SubjectIndices.IsEmpty()) { return false; }

		// Frame subject indexes the parallel AllOpData arrays (Projections / Facades / ProjectedZValues);
		// validity is identical across them, so one check covers every later lookup the callers do.
		const int32 FrameSrcIdx = Group->SubjectIndices[0];
		if (!AllOpData->Projections.IsValidIndex(FrameSrcIdx) || !AllOpData->Facades.IsValidIndex(FrameSrcIdx)) { return false; }

		OutResult = Decompose(Group->SubjectPaths, AllOpData, Group->CreateZCallback(), Params);
		return OutResult.Status == EDecomposeResult::Success;
	}

	FText DescribeDecomposeFailure(const FDecomposeResult& Result, const FText& Subject, const int32 MaxConvexPieces)
	{
		switch (Result.Status)
		{
		case EDecomposeResult::TriangulationFailed:
			return FText::Format(
				LOCTEXT("TriangulationFailed", "A {0} could not be triangulated (degenerate or self-intersecting) and was skipped."),
				Subject);
		case EDecomposeResult::TooManyPieces:
			return FText::Format(
				LOCTEXT("TooManyPieces", "A {0} needs {1} convex pieces (over the {2} cap) and was skipped. Raise Max Convex Pieces or simplify the path."),
				Subject, FText::AsNumber(Result.Pieces.Num()), FText::AsNumber(MaxConvexPieces));
		default:
			// Success / Empty -- Empty is an intentional silent skip (degenerate/invalid group), nothing to report.
			return FText::GetEmpty();
		}
	}
}

#undef LOCTEXT_NAMESPACE
