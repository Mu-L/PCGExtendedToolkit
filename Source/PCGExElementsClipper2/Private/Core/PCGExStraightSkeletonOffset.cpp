// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Core/PCGExStraightSkeletonOffset.h"

#include "Algo/Reverse.h"
#include "PCGExH.h"
#include "Clipper2Lib/clipper.h"

// =====================================================================================================
// Offset-based straight-skeleton solver.
//
// Traces the wavefront by iteratively shrinking the polygon with Clipper2's mitered offset and following
// the corners. A mitered offset IS the exact straight-skeleton wavefront, so Clipper2 owns all the hard
// geometry/topology (corner merges, splits, hole->outer merges) and never produces invalid geometry.
//
// Extraction (per-step trace model): every wavefront corner is identified by its (edgeA,edgeB) original-
// edge pair. Each step, every corner gets a fresh node connected to its previous node -- i.e. its trace is
// recorded incrementally. A corner that disappears merged into a surviving neighbour that shares one of
// its edges, so we connect it there. A final collinear-dissolve pass collapses each straight trace run
// into one clean skeleton edge. This is robust to occasional edge-label slips: a spurious one-step corner
// just produces a zero-length trace that dissolves away, rather than a stranded node.
//
// The remaining skeleton edges that are NOT vertex traces are RIDGES (two parallel wavefront edges meeting
// -- e.g. a rectangle's mid-line). A ridge is the wavefront edge that survives a collapse, so it joins two
// corners that share an original edge: we recover it from shared-edge adjacency among the corners present
// when a region collapses.
//
// Conventions: outer CCW, holes CW (solid on the left). Work is in LOCAL coords (input translated to its
// bounds-min) for precision at large world offsets, then translated back.
// =====================================================================================================

namespace PCGExStraightSkeletonOffset
{
	constexpr double DIR_DOT_MIN = 0.999;  // |dot| threshold: ring edge parallel to an original edge
	constexpr double SMALL_LEN = 1e-4;
	constexpr double COLLINEAR_EPS = 0.02; // |cross| of unit dirs to treat a degree-2 node as collinear

	FORCEINLINE FVector2D LeftNormal(const FVector2D& D) { return FVector2D(-D.Y, D.X); }

	FORCEINLINE PCGExClipper2Lib::Point64 ToPoint(const FVector2D& V, const double S)
	{
		return PCGExClipper2Lib::Point64(
			static_cast<int64>(FMath::RoundToDouble(V.X * S)),
			static_cast<int64>(FMath::RoundToDouble(V.Y * S)));
	}

	PCGExClipper2Lib::Path64 ToPath(const TArrayView<const FVector2D>& Loop, const double S)
	{
		PCGExClipper2Lib::Path64 Path;
		Path.reserve(Loop.Num());
		for (const FVector2D& V : Loop) { Path.push_back(ToPoint(V, S)); }
		return Path;
	}

	PCGExClipper2Lib::Paths64 BuildSubject(const TArrayView<const FVector2D>& Outer, const TArrayView<const TArray<FVector2D>>& Holes, const double S)
	{
		using namespace PCGExClipper2Lib;
		Paths64 Subject;

		Path64 OuterPath = ToPath(Outer, S);
		if (!IsPositive(OuterPath)) { std::reverse(OuterPath.begin(), OuterPath.end()); }
		Subject.push_back(OuterPath);

		for (const TArray<FVector2D>& Hole : Holes)
		{
			if (Hole.Num() < 3) { continue; }
			Path64 HolePath = ToPath(Hole, S);
			if (IsPositive(HolePath)) { std::reverse(HolePath.begin(), HolePath.end()); }
			Subject.push_back(HolePath);
		}

		return Subject;
	}

	void OffsetSubject(const PCGExClipper2Lib::Paths64& Subject, const double DeltaScaled, const double MiterLimit, const double InvS, TArray<TArray<FVector2D>>& OutRings, TArray<bool>* OutIsHole = nullptr)
	{
		using namespace PCGExClipper2Lib;

		OutRings.Reset();
		if (OutIsHole) { OutIsHole->Reset(); }

		ClipperOffset Offsetter(MiterLimit, 0.0, true, false);
		Offsetter.AddPaths(Subject, JoinType::Miter, EndType::Polygon);

		PolyTree64 Tree;
		Offsetter.Execute(DeltaScaled, Tree);

		TFunction<void(const PolyPath64*)> Walk = [&](const PolyPath64* Node)
		{
			for (size_t i = 0; i < Node->Count(); i++)
			{
				const PolyPath64* Child = Node->Child(i);
				const Path64& Poly = Child->Polygon();
				if (Poly.size() >= 3)
				{
					TArray<FVector2D> Loop;
					Loop.Reserve(static_cast<int32>(Poly.size()));
					for (const Point64& Pt : Poly) { Loop.Emplace(static_cast<double>(Pt.x) * InvS, static_cast<double>(Pt.y) * InvS); }
					OutRings.Add(MoveTemp(Loop));
					if (OutIsHole) { OutIsHole->Add(Child->IsHole()); }
				}
				Walk(Child);
			}
		};
		Walk(&Tree);
	}

	struct FOrigEdge
	{
		FVector2D P = FVector2D::ZeroVector;
		FVector2D Dir = FVector2D::ZeroVector;
		FVector2D Normal = FVector2D::ZeroVector;
	};

	int32 MatchEdge(const TArray<FOrigEdge>& Edges, const FVector2D& A, const FVector2D& B, const double T, const double DistTol)
	{
		FVector2D D = B - A;
		const double Len = D.Size();
		if (Len < SMALL_LEN) { return -1; }
		D /= Len;
		const FVector2D M = (A + B) * 0.5;

		int32 Best = -1;
		double BestErr = TNumericLimits<double>::Max();
		for (int32 j = 0; j < Edges.Num(); j++)
		{
			const FOrigEdge& E = Edges[j];
			if (FMath::Abs(FVector2D::DotProduct(D, E.Dir)) < DIR_DOT_MIN) { continue; }
			const double Err = FMath::Abs(FVector2D::DotProduct(M - E.P, E.Normal) - T);
			if (Err < DistTol && Err < BestErr) { BestErr = Err; Best = j; }
		}
		return Best;
	}

	struct FRawCorner
	{
		int32 EA = -1;
		int32 EB = -1;
		FVector2D Pos = FVector2D::ZeroVector;
	};

	struct FActiveCorner
	{
		int32 EA = -1;
		int32 EB = -1;
		FVector2D Pos = FVector2D::ZeroVector;
		int32 Node = -1; // current trace node
		bool bSeen = false;
	};

	FORCEINLINE bool ShareEdge(const int32 AEA, const int32 AEB, const int32 BEA, const int32 BEB)
	{
		return AEA == BEA || AEA == BEB || AEB == BEA || AEB == BEB;
	}
}

namespace PCGExMath::Geo
{
	void FStraightSkeletonOffset::Clear()
	{
		Nodes.Reset();
		Edges.Reset();
		NumContourNodes = 0;
		IsValid = false;
	}

	void FStraightSkeletonOffset::DebugOffsetAt(
		const TArrayView<const FVector2D>& Outer, const TArrayView<const TArray<FVector2D>>& Holes,
		const double T, const double Precision, const double MiterLimit,
		TArray<TArray<FVector2D>>& OutLoops, TArray<bool>& OutIsHole)
	{
		OutLoops.Reset();
		OutIsHole.Reset();
		if (Outer.Num() < 3) { return; }

		const double S = Precision;
		const PCGExClipper2Lib::Paths64 Subject = PCGExStraightSkeletonOffset::BuildSubject(Outer, Holes, S);
		PCGExStraightSkeletonOffset::OffsetSubject(Subject, -T * S, MiterLimit, 1.0 / S, OutLoops, &OutIsHole);
	}

	bool FStraightSkeletonOffset::Process(
		const TArrayView<const FVector2D>& Outer, const TArrayView<const TArray<FVector2D>>& Holes,
		const double MergeDistance, const bool bIncludeContour)
	{
		using namespace PCGExStraightSkeletonOffset;
		Clear();
		if (Outer.Num() < 3) { return false; }

		// ---- 1. Normalize winding + translate to local ------------------------------------------------
		auto Shoelace = [](const TArray<FVector2D>& L) -> double
		{
			double A = 0;
			const int32 N = L.Num();
			for (int32 i = 0; i < N; i++) { A += L[i].X * L[(i + 1) % N].Y - L[(i + 1) % N].X * L[i].Y; }
			return A * 0.5;
		};
		auto Sanitize = [](const TArrayView<const FVector2D>& In, TArray<FVector2D>& Out) -> bool
		{
			Out.Reset(In.Num());
			for (const FVector2D& P : In) { if (Out.Num() == 0 || FVector2D::DistSquared(Out.Last(), P) > 1e-6) { Out.Add(P); } }
			if (Out.Num() > 1 && FVector2D::DistSquared(Out.Last(), Out[0]) <= 1e-6) { Out.Pop(); }
			return Out.Num() >= 3;
		};

		TArray<TArray<FVector2D>> Loops;
		{
			TArray<FVector2D> Clean;
			if (!Sanitize(Outer, Clean)) { return false; }
			if (Shoelace(Clean) < 0) { Algo::Reverse(Clean); }
			Loops.Add(MoveTemp(Clean));
			for (const TArray<FVector2D>& Hole : Holes)
			{
				TArray<FVector2D> CleanHole;
				if (!Sanitize(Hole, CleanHole)) { continue; }
				if (Shoelace(CleanHole) > 0) { Algo::Reverse(CleanHole); }
				Loops.Add(MoveTemp(CleanHole));
			}
		}

		FVector2D Ref(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
		for (const TArray<FVector2D>& L : Loops) { for (const FVector2D& P : L) { Ref.X = FMath::Min(Ref.X, P.X); Ref.Y = FMath::Min(Ref.Y, P.Y); } }
		for (TArray<FVector2D>& L : Loops) { for (FVector2D& P : L) { P -= Ref; } }

		// ---- 2. Original edges + contour nodes + seed corners -----------------------------------------
		TArray<FOrigEdge> OrigEdges;
		TArray<FVector2D> NodePos;
		TArray<bool> NodeContour;
		TArray<double> NodeTime;
		TArray<FActiveCorner> Active;

		FVector2D BoundsMax(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
		for (const TArray<FVector2D>& L : Loops)
		{
			const int32 N = L.Num();
			const int32 VStart = NodePos.Num();
			const int32 EStart = OrigEdges.Num();
			for (int32 i = 0; i < N; i++)
			{
				NodePos.Add(L[i]);
				NodeContour.Add(true);
				NodeTime.Add(0.0);
				BoundsMax.X = FMath::Max(BoundsMax.X, L[i].X);
				BoundsMax.Y = FMath::Max(BoundsMax.Y, L[i].Y);
			}
			for (int32 i = 0; i < N; i++)
			{
				FOrigEdge E;
				E.P = L[i];
				E.Dir = (L[(i + 1) % N] - L[i]).GetSafeNormal();
				E.Normal = LeftNormal(E.Dir);
				OrigEdges.Add(E);
			}
			for (int32 i = 0; i < N; i++)
			{
				const int32 PrevE = EStart + (i + N - 1) % N;
				const int32 NextE = EStart + i;
				FActiveCorner AC;
				AC.EA = FMath::Min(PrevE, NextE);
				AC.EB = FMath::Max(PrevE, NextE);
				AC.Pos = L[i];
				AC.Node = VStart + i; // current trace node starts at the contour vertex
				Active.Add(AC);
			}
		}
		NumContourNodes = NodePos.Num();

		const double Diag = BoundsMax.Size();
		const double S = Precision;
		const double Delta = Step > 0 ? Step : FMath::Max(Diag / 500.0, KINDA_SMALL_NUMBER);
		const double DistTol = FMath::Max(2.0, Delta * 0.5);
		const int32 MaxSteps = 8000;

		const PCGExClipper2Lib::Paths64 Subject = BuildSubject(Loops[0], TArrayView<const TArray<FVector2D>>(Loops.GetData() + 1, Loops.Num() - 1), S);

		struct FArcRec { int32 A; int32 B; };
		TArray<FArcRec> Arcs;

		auto AddNode = [&](const FVector2D& P, const double T) -> int32
		{
			NodePos.Add(P);
			NodeContour.Add(false);
			NodeTime.Add(T);
			return NodePos.Num() - 1;
		};

		// ---- 3. March: per-step corner traces ---------------------------------------------------------
		TArray<TArray<FVector2D>> Rings;
		double LastT = 0.0;
		for (int32 StepIdx = 0; StepIdx < MaxSteps; StepIdx++)
		{
			const double T = Delta * (StepIdx + 1);
			OffsetSubject(Subject, -T * S, MiterLimit, 1.0 / S, Rings);
			if (Rings.IsEmpty()) { break; }
			LastT = T;

			TArray<FRawCorner> Raw;
			for (const TArray<FVector2D>& Ring : Rings)
			{
				const int32 N = Ring.Num();
				if (N < 3) { continue; }
				TArray<int32> Labels;
				Labels.SetNumUninitialized(N);
				for (int32 k = 0; k < N; k++) { Labels[k] = MatchEdge(OrigEdges, Ring[k], Ring[(k + 1) % N], T, DistTol); }
				for (int32 k = 0; k < N; k++)
				{
					const int32 Lp = Labels[(k + N - 1) % N];
					const int32 Ln = Labels[k];
					if (Lp < 0 || Ln < 0 || Lp == Ln) { continue; }
					Raw.Add({FMath::Min(Lp, Ln), FMath::Max(Lp, Ln), Ring[k]});
				}
			}

			for (FActiveCorner& AC : Active) { AC.bSeen = false; }

			// Match each raw corner to an active corner (same edge-pair, nearest); give it a fresh node and
			// extend its trace. Unmatched raw corners are new branches.
			TArray<FActiveCorner> StepNodes; // EA,EB,Pos,Node for this step's corners (for merge resolution)
			for (const FRawCorner& RC : Raw)
			{
				int32 Best = -1;
				double BestD = TNumericLimits<double>::Max();
				for (int32 ai = 0; ai < Active.Num(); ai++)
				{
					if (Active[ai].bSeen || Active[ai].EA != RC.EA || Active[ai].EB != RC.EB) { continue; }
					const double D = FVector2D::DistSquared(Active[ai].Pos, RC.Pos);
					if (D < BestD) { BestD = D; Best = ai; }
				}
				const int32 NewNode = AddNode(RC.Pos, T);
				if (Best >= 0)
				{
					Arcs.Add({Active[Best].Node, NewNode});
					Active[Best].Node = NewNode;
					Active[Best].Pos = RC.Pos;
					Active[Best].bSeen = true;
				}
				else
				{
					Active.Add({RC.EA, RC.EB, RC.Pos, NewNode, true});
				}
				StepNodes.Add({RC.EA, RC.EB, RC.Pos, NewNode, true});
			}

			// Dying corners merged into a surviving neighbour that shares one of their edges.
			for (FActiveCorner& AC : Active)
			{
				if (AC.bSeen) { continue; }
				int32 MergeNode = INDEX_NONE;
				double BestD = TNumericLimits<double>::Max();
				for (const FActiveCorner& SN : StepNodes)
				{
					if (!ShareEdge(AC.EA, AC.EB, SN.EA, SN.EB)) { continue; }
					const double D = FVector2D::DistSquared(AC.Pos, SN.Pos);
					if (D < BestD) { BestD = D; MergeNode = SN.Node; }
				}
				if (MergeNode == INDEX_NONE)
				{
					for (const FActiveCorner& SN : StepNodes)
					{
						const double D = FVector2D::DistSquared(AC.Pos, SN.Pos);
						if (D < BestD) { BestD = D; MergeNode = SN.Node; }
					}
				}
				if (MergeNode != INDEX_NONE && MergeNode != AC.Node) { Arcs.Add({AC.Node, MergeNode}); }
			}

			for (int32 ai = Active.Num() - 1; ai >= 0; ai--) { if (!Active[ai].bSeen) { Active.RemoveAtSwap(ai, 1, EAllowShrinking::No); } }
		}

		// ---- 4. Final collapse: ridge edges from shared-edge adjacency among the survivors -------------
		for (int32 i = 0; i < Active.Num(); i++)
		{
			for (int32 j = i + 1; j < Active.Num(); j++)
			{
				if (Active[i].Node == Active[j].Node) { continue; }
				if (ShareEdge(Active[i].EA, Active[i].EB, Active[j].EA, Active[j].EB)) { Arcs.Add({Active[i].Node, Active[j].Node}); }
			}
		}

		// ---- 5. Collinear-dissolve: collapse each straight trace run into one edge ---------------------
		// Adjacency over the raw arc graph; repeatedly remove an interior degree-2 node whose two edges are
		// collinear, splicing them into one. Trace intermediates vanish; event nodes (degree != 2, or a
		// genuine bend) stay.
		{
			const int32 NN = NodePos.Num();
			TArray<TArray<int32>> Adj; // node -> incident arc indices (live)
			Adj.SetNum(NN);
			TBitArray<> ArcAlive(true, Arcs.Num());
			TBitArray<> NodeAlive(true, NN);
			for (int32 e = 0; e < Arcs.Num(); e++)
			{
				if (Arcs[e].A == Arcs[e].B) { ArcAlive[e] = false; continue; }
				Adj[Arcs[e].A].Add(e);
				Adj[Arcs[e].B].Add(e);
			}

			auto OtherEnd = [&](const int32 e, const int32 n) { return Arcs[e].A == n ? Arcs[e].B : Arcs[e].A; };

			bool bChanged = true;
			while (bChanged)
			{
				bChanged = false;
				for (int32 n = NumContourNodes; n < NN; n++)
				{
					if (!NodeAlive[n]) { continue; }
					TArray<int32> Live;
					for (const int32 e : Adj[n]) { if (ArcAlive[e]) { Live.Add(e); } }
					if (Live.Num() != 2) { continue; }
					const int32 A = OtherEnd(Live[0], n);
					const int32 B = OtherEnd(Live[1], n);
					if (A == B || A == n || B == n) { continue; }
					const FVector2D D0 = (NodePos[n] - NodePos[A]).GetSafeNormal();
					const FVector2D D1 = (NodePos[B] - NodePos[n]).GetSafeNormal();
					if (FMath::Abs(D0.X * D1.Y - D0.Y * D1.X) > COLLINEAR_EPS) { continue; } // not collinear
					// Splice: repurpose Live[0] as A-B, kill Live[1] + node n.
					Arcs[Live[0]].A = A;
					Arcs[Live[0]].B = B;
					ArcAlive[Live[1]] = false;
					NodeAlive[n] = false;
					Adj[B].Remove(Live[1]);
					Adj[B].Add(Live[0]);
					bChanged = true;
				}
			}

			// Compact dead arcs.
			TArray<FArcRec> Kept;
			Kept.Reserve(Arcs.Num());
			for (int32 e = 0; e < Arcs.Num(); e++) { if (ArcAlive[e] && Arcs[e].A != Arcs[e].B) { Kept.Add(Arcs[e]); } }
			Arcs = MoveTemp(Kept);
		}

		// ---- 6. Weld near-coincident nodes + assemble output (translate back) --------------------------
		const int32 NumWork = NodePos.Num();
		const double WeldR = FMath::Max(MergeDistance, Delta * 0.5);
		const double WeldR2 = WeldR * WeldR;
		TMap<FIntPoint, TArray<int32>> Grid;
		TArray<int32> OldToRep;
		OldToRep.Init(-1, NumWork);
		auto CellOf = [WeldR](const FVector2D& P) { return FIntPoint(FMath::FloorToInt(P.X / WeldR), FMath::FloorToInt(P.Y / WeldR)); };
		auto FindRep = [&](const FVector2D& P) -> int32
		{
			const FIntPoint C = CellOf(P);
			int32 Rep = -1;
			double BestD2 = WeldR2;
			for (int32 dx = -1; dx <= 1; dx++) { for (int32 dy = -1; dy <= 1; dy++) { if (const TArray<int32>* Cell = Grid.Find(FIntPoint(C.X + dx, C.Y + dy))) { for (const int32 R : *Cell) { const double D2 = FVector2D::DistSquared(NodePos[R], P); if (D2 <= BestD2) { BestD2 = D2; Rep = R; } } } } }
			return Rep;
		};
		auto Register = [&](const int32 Old) { OldToRep[Old] = Old; Grid.FindOrAdd(CellOf(NodePos[Old])).Add(Old); };

		// Only nodes referenced by a surviving arc (or contour) need to exist.
		TBitArray<> Referenced(false, NumWork);
		for (int32 i = 0; i < NumContourNodes; i++) { Referenced[i] = true; }
		for (const FArcRec& Arc : Arcs) { Referenced[Arc.A] = true; Referenced[Arc.B] = true; }

		for (int32 i = 0; i < NumContourNodes; i++) { const int32 R = FindRep(NodePos[i]); OldToRep[i] = R >= 0 ? R : (Register(i), i); }
		for (int32 i = NumContourNodes; i < NumWork; i++) { if (!Referenced[i]) { continue; } const int32 R = FindRep(NodePos[i]); OldToRep[i] = R >= 0 ? R : (Register(i), i); }

		TArray<int32> RepToNew;
		RepToNew.Init(-1, NumWork);
		int32 OutContour = 0;
		for (int32 i = 0; i < NumWork; i++)
		{
			if (OldToRep[i] != i) { continue; }
			RepToNew[i] = Nodes.Num();
			Nodes.Add(FStraightSkeletonNode(NodePos[i] + Ref, NodeTime[i], NodeContour[i]));
			if (NodeContour[i]) { OutContour++; }
		}
		NumContourNodes = OutContour;

		auto Final = [&](const int32 Old) { return RepToNew[OldToRep[Old]]; };

		TMap<uint64, int32> EdgeMap;
		auto AddEdge = [&](const int32 A, const int32 B, const ESkeletonEdgeType Type)
		{
			if (A < 0 || B < 0 || A == B) { return; }
			const uint64 Key = PCGEx::H64U(static_cast<uint32>(A), static_cast<uint32>(B));
			if (const int32* Existing = EdgeMap.Find(Key)) { if (Type == ESkeletonEdgeType::Contour) { Edges[*Existing].Type = ESkeletonEdgeType::Contour; } return; }
			const bool bPerimeter = Nodes[A].bIsContour || Nodes[B].bIsContour;
			EdgeMap.Add(Key, Edges.Num());
			Edges.Add(FStraightSkeletonEdge(A, B, Type, Type == ESkeletonEdgeType::Contour ? ESkeletonEventType::None : ESkeletonEventType::Initial, bPerimeter));
		};

		for (const FArcRec& Arc : Arcs) { AddEdge(Final(Arc.A), Final(Arc.B), ESkeletonEdgeType::Skeleton); }

		if (bIncludeContour)
		{
			int32 VOff = 0;
			for (const TArray<FVector2D>& L : Loops)
			{
				const int32 N = L.Num();
				for (int32 i = 0; i < N; i++) { AddEdge(Final(VOff + i), Final(VOff + (i + 1) % N), ESkeletonEdgeType::Contour); }
				VOff += N;
			}
		}

		IsValid = Edges.Num() > 0;
		return IsValid;
	}
}
