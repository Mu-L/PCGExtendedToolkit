// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Core/PCGExStraightSkeletonOffset.h"

#include "Algo/Reverse.h"
#include "PCGExH.h"
#include "PCGExLog.h"
#include "Clipper2Lib/clipper.h"

// =====================================================================================================
// Offset-based straight-skeleton solver.
//
// Traces the wavefront by iteratively shrinking the polygon with Clipper2's MITERED offset and following
// the corners. A mitered offset IS the exact straight-skeleton wavefront, so Clipper2 owns all the hard
// geometry/topology (corner merges, splits, hole->outer merges) and never produces invalid geometry.
//
// JOIN TYPE MUST BE MITER (with a high miter limit). Round arcs every corner into many spurious points and
// Square bevels them -- either corrupts the corner identity the extraction depends on.
//
// Extraction (per-step trace model): every wavefront corner is identified by its (edgeA,edgeB) original-
// edge pair. Each step every corner gets a fresh node connected to its previous node -- i.e. its trace is
// recorded incrementally. A corner that disappears merged into a surviving neighbour that shares one of its
// edges, so we connect it there. A final collinear-dissolve collapses each straight trace run into one
// clean skeleton edge. Robust to occasional edge-label slips: a spurious one-step corner produces a
// zero-length trace that dissolves away rather than a stranded node.
//
// Conventions: outer CCW, holes CW (solid on the left). Work is done in LOCAL coordinates (input
// translated to its bounds-min) for precision at large world offsets, then translated back.
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

	// Outer positive, holes negative (Clipper2 orientation), so a negative delta shrinks the solid inward.
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

	// Run one MITERED offset and read every resulting loop (ordered, unscaled). OutIsHole optional.
	void OffsetSubject(const PCGExClipper2Lib::Paths64& Subject, const double DeltaScaled, const double MiterLimit, const double InvS, TArray<TArray<FVector2D>>& OutRings, TArray<bool>* OutIsHole = nullptr)
	{
		using namespace PCGExClipper2Lib;

		OutRings.Reset();
		if (OutIsHole) { OutIsHole->Reset(); }

		// miter_limit high so corners reach the true bisector intersection; arc_tolerance irrelevant for Miter.
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
		FVector2D P = FVector2D::ZeroVector;      // a point on the edge (its start vertex)
		FVector2D Dir = FVector2D::ZeroVector;    // unit direction
		FVector2D Normal = FVector2D::ZeroVector; // inward (into-solid) unit normal
		double Len = 0.0;                         // segment length (P .. P + Dir*Len)
	};

	// Best original edge whose offset line at distance t coincides with the ring edge A->B. -1 if none.
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
			const double Err = FMath::Abs(FVector2D::DotProduct(M - E.P, E.Normal) - T); // ring edge is +t inward
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

		// ---- 1. Normalize winding + translate to local coords ------------------------------------------
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

		TArray<TArray<FVector2D>> Loops; // [outer, holes...], CCW outer / CW holes
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

		// ---- 2. Original edges + contour nodes + seed corners ------------------------------------------
		TArray<FOrigEdge> OrigEdges;
		TArray<FVector2D> NodePos;   // local; contour first
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
				const FVector2D Seg = L[(i + 1) % N] - L[i];
				E.Len = Seg.Size();
				E.Dir = E.Len > SMALL_LEN ? Seg / E.Len : FVector2D::ZeroVector;
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
		// Stable tolerance scale: the step the DEFAULT resolution would use. The weld/merge/edge-match gates key
		// off this (never the user's Resolution), so raising Resolution sharpens sampling without shrinking the
		// gates that hold topology together (e.g. the Hexagon's near-degenerate weld cluster needs ~BaseDelta*5).
		const double BaseDelta = FMath::Max(Diag / DefaultResolution, KINDA_SMALL_NUMBER);
		// Marching step: how finely the inward offset is sampled. Step (world units) overrides Resolution when set.
		const double Delta = Step > 0.0 ? Step : FMath::Max(Diag / FMath::Max(Resolution, 1.0), KINDA_SMALL_NUMBER);
		const double DistTol = FMath::Max(2.0, BaseDelta * 0.5);
		// Empty Rings (wavefront collapsed) is the real terminator (below). This cap only bounds a non-collapsing
		// march and scales with the step so fine sampling is never truncated: T <= inradius <= Diag/2 => the real
		// step count is <= Diag/(2*Delta); ~8x slack for numerical tail rings. Warned on hit, never silently cut.
		const int32 MaxSteps = FMath::Max(256, static_cast<int32>(FMath::CeilToDouble(Diag / Delta)) * 4);

		const PCGExClipper2Lib::Paths64 Subject = BuildSubject(Loops[0], TArrayView<const TArray<FVector2D>>(Loops.GetData() + 1, Loops.Num() - 1), S);

		struct FArcRec { int32 A; int32 B; };
		TArray<FArcRec> Arcs;
		// Parallel/feature-collapse medials (a pinched neck's centerline). Recorded here during the march and
		// added after it ONLY where they bridge two otherwise-disconnected components -- so a genuine bridge
		// (the neck) is restored, while a medial already implied by the surrounding skeleton (a hole's ring
		// routes around the gap) is not duplicated into a crossing edge.
		TArray<FArcRec> CollapseCandidates;

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
		int32 StepIdx = 0;
		for (; StepIdx < MaxSteps; StepIdx++)
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

			// Match each raw corner to an active corner (same edge-pair, nearest); extend its trace.
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

			// Dying corners merged into a surviving neighbour. The target is the survivor this corner became
			// coincident with, so it must be GEOMETRICALLY LOCAL. Prefer the nearest survivor sharing an edge,
			// but only within a few default-resolution steps (stable BaseDelta, not the user's Resolution step) --
			// a far "shared edge" is a label coincidence that would draw a spurious diameter edge. Otherwise take
			// the nearest survivor of any kind.
			const double MergeGateSq = FMath::Square(FMath::Max(MergeDistance, BaseDelta * 8.0));
			for (FActiveCorner& AC : Active)
			{
				if (AC.bSeen) { continue; }
				int32 ShareNode = INDEX_NONE;
				double ShareD = TNumericLimits<double>::Max();
				int32 AnyNode = INDEX_NONE;
				double AnyD = TNumericLimits<double>::Max();
				for (const FActiveCorner& SN : StepNodes)
				{
					const double D = FVector2D::DistSquared(AC.Pos, SN.Pos);
					if (D < AnyD) { AnyD = D; AnyNode = SN.Node; }
					if (D < ShareD && ShareEdge(AC.EA, AC.EB, SN.EA, SN.EB)) { ShareD = D; ShareNode = SN.Node; }
				}
				const int32 MergeNode = (ShareNode != INDEX_NONE && ShareD <= MergeGateSq) ? ShareNode : AnyNode;
				if (MergeNode != INDEX_NONE && MergeNode != AC.Node) { Arcs.Add({AC.Node, MergeNode}); }
			}

			// Feature collapse: an original edge whose corners ALL die this step (none survive in StepNodes) has
			// pinched to its medial -- a thin neck's centerline, a slot, etc. The per-step merge wires each dying
			// corner to a continuing ridge but never to its ridge partner, and the final-collapse pass only runs on
			// the very last step, so this mid-process ridge is otherwise lost. Connect the edge's dying corners
			// consecutively along it (extent-gated, exactly like the final collapse). For an ordinary edge-event the
			// two dying corners coincide at the merge point, so the added arc is near-zero-length and dissolves.
			{
				TSet<int32> SurvivingEdges;
				for (const FActiveCorner& SN : StepNodes) { SurvivingEdges.Add(SN.EA); SurvivingEdges.Add(SN.EB); }
				TMap<int32, TArray<int32>> DyingOnEdge;
				for (int32 ai = 0; ai < Active.Num(); ai++)
				{
					if (Active[ai].bSeen) { continue; }
					if (Active[ai].EA >= 0 && !SurvivingEdges.Contains(Active[ai].EA)) { DyingOnEdge.FindOrAdd(Active[ai].EA).Add(ai); }
					if (Active[ai].EB >= 0 && !SurvivingEdges.Contains(Active[ai].EB)) { DyingOnEdge.FindOrAdd(Active[ai].EB).Add(ai); }
				}
				const double CollapseSlack = FMath::Max(MergeDistance, T * 2.0);
				for (TPair<int32, TArray<int32>>& Pair : DyingOnEdge)
				{
					TArray<int32>& OnE = Pair.Value;
					const FVector2D EP = OrigEdges[Pair.Key].P;
					const FVector2D EDir = OrigEdges[Pair.Key].Dir;
					OnE.RemoveAll([&](const int32 ai) { const double Pj = FVector2D::DotProduct(Active[ai].Pos - EP, EDir); return Pj < -CollapseSlack || Pj > OrigEdges[Pair.Key].Len + CollapseSlack; });
					if (OnE.Num() < 2) { continue; }
					OnE.Sort([&](const int32 A, const int32 B) { return FVector2D::DotProduct(Active[A].Pos, EDir) < FVector2D::DotProduct(Active[B].Pos, EDir); });
					for (int32 k = 0; k + 1 < OnE.Num(); k++) { if (Active[OnE[k]].Node != Active[OnE[k + 1]].Node) { CollapseCandidates.Add({Active[OnE[k]].Node, Active[OnE[k + 1]].Node}); } }
				}
			}
			for (int32 ai = Active.Num() - 1; ai >= 0; ai--) { if (!Active[ai].bSeen) { Active.RemoveAtSwap(ai, 1, EAllowShrinking::No); } }
		}
		if (StepIdx >= MaxSteps)
		{
			// Warn-and-continue (no ensure: most users run without a debugger, where an ensure can surface as a
			// crash). The wavefront should always empty first; reaching the cap means degenerate/self-intersecting
			// input or a precision stall, and the skeleton may be truncated.
			UE_LOG(LogPCGEx, Warning,
				TEXT("FStraightSkeletonOffset: inward march hit the %d-step cap without the wavefront collapsing ")
				TEXT("(Delta=%.4f, Diag=%.2f); the skeleton may be truncated -- check for degenerate / self-")
				TEXT("intersecting input, or lower Resolution."), MaxSteps, Delta, Diag);
		}

		// ---- 4. Final collapse: ridge edges. For each ORIGINAL edge, connect the surviving corners lying
		// on it CONSECUTIVELY (sorted along the edge) -- the real ridge. An all-pairs shared-edge join would
		// bridge non-adjacent corners on symmetric collapses (e.g. opposite arms of a '+'), and those
		// "diameter" edges cross everything. A clean ridge has exactly 2 corners per edge, so this is a
		// no-op there; a symmetric remnant gets routed through its in-between corner instead of skipping it.
		{
			for (int32 e = 0; e < OrigEdges.Num(); e++)
			{
				TArray<int32> OnE; // indices into Active whose corner uses edge e
				// Collinear stacked edges (an H-beam's two x=40 sides, both y=0 box bottoms) share one offset
				// LINE, so MatchEdge can tag a far-box corner with this edge -- connecting it would draw a ridge
				// clear across the shape. A corner only lies on e if it projects within e's segment (+ slack for
				// miter overhang at reflex corners).
				const double ExtentSlack = FMath::Max(MergeDistance, LastT * 2.0);
				for (int32 ai = 0; ai < Active.Num(); ai++)
				{
					if (Active[ai].EA != e && Active[ai].EB != e) { continue; }
					const double Proj = FVector2D::DotProduct(Active[ai].Pos - OrigEdges[e].P, OrigEdges[e].Dir);
					if (Proj >= -ExtentSlack && Proj <= OrigEdges[e].Len + ExtentSlack) { OnE.Add(ai); }
				}
				if (OnE.Num() < 2) { continue; }
				const FVector2D EDir = OrigEdges[e].Dir;
				OnE.Sort([&](const int32 A, const int32 B) { return FVector2D::DotProduct(Active[A].Pos, EDir) < FVector2D::DotProduct(Active[B].Pos, EDir); });
				for (int32 k = 0; k + 1 < OnE.Num(); k++)
				{
					const int32 NA = Active[OnE[k]].Node;
					const int32 NB = Active[OnE[k + 1]].Node;
					if (NA != NB) { Arcs.Add({NA, NB}); }
				}
			}
		}

		// ---- 4b. Connectivity repair: add a recorded collapse-medial only where it joins two components -----
		// A pinched feature (a thin neck) can be the ONLY link between two parts of the shape, and its medial is
		// captured by none of the passes above. Restore exactly those bridges: union the arcs so far, then add
		// each candidate whose endpoints are still in different components. A medial already implied by the
		// surrounding skeleton (a hole's encircling ring) joins same-component nodes and is skipped, so it never
		// becomes a crossing duplicate.
		if (CollapseCandidates.Num() > 0)
		{
			TArray<int32> Parent;
			Parent.SetNumUninitialized(NodePos.Num());
			for (int32 i = 0; i < Parent.Num(); i++) { Parent[i] = i; }
			auto Find = [&Parent](int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
			auto Union = [&](const int32 A, const int32 B) { const int32 RA = Find(A); const int32 RB = Find(B); if (RA != RB) { Parent[RA] = RB; } };
			for (const FArcRec& Arc : Arcs) { Union(Arc.A, Arc.B); }
			for (const FArcRec& C : CollapseCandidates) { if (Find(C.A) != Find(C.B)) { Arcs.Add(C); Union(C.A, C.B); } }
		}

		// ---- 5. Collinear-dissolve: collapse each straight trace run into one edge ---------------------
		{
			auto Dir = [&](const int32 A, const int32 B) { return (NodePos[B] - NodePos[A]).GetSafeNormal(); };

			auto Dissolve = [&](TArray<FArcRec>& E)
			{
				const int32 NNlocal = NodePos.Num();
				TArray<TArray<int32>> Adj;
				Adj.SetNum(NNlocal);
				TBitArray<> Alive(true, E.Num());
				for (int32 e = 0; e < E.Num(); e++) { if (E[e].A == E[e].B) { Alive[e] = false; continue; } Adj[E[e].A].Add(e); Adj[E[e].B].Add(e); }
				auto Other = [&](const int32 e, const int32 n) { return E[e].A == n ? E[e].B : E[e].A; };
				bool bCh = true;
				while (bCh)
				{
					bCh = false;
					for (int32 n = NumContourNodes; n < NNlocal; n++)
					{
						TArray<int32> Live;
						for (const int32 e : Adj[n]) { if (Alive[e]) { Live.Add(e); } }
						if (Live.Num() != 2) { continue; }
						const int32 A = Other(Live[0], n);
						const int32 B = Other(Live[1], n);
						if (A == B || A == n || B == n) { continue; }
						const FVector2D D0 = Dir(A, n);
						const FVector2D D1 = Dir(n, B);
						if (FMath::Abs(D0.X * D1.Y - D0.Y * D1.X) > COLLINEAR_EPS) { continue; }
						E[Live[0]].A = A;
						E[Live[0]].B = B;
						Alive[Live[1]] = false;
						Adj[B].Remove(Live[1]);
						Adj[B].Add(Live[0]);
						bCh = true;
					}
				}
				TArray<FArcRec> Kept;
				for (int32 e = 0; e < E.Num(); e++) { if (Alive[e] && E[e].A != E[e].B) { Kept.Add(E[e]); } }
				E = MoveTemp(Kept);
			};

			// Drop interior degree-1 leaves (short merge-step / sampling stubs). A real skeleton has none, and
			// removing a leaf can't disconnect the rest, so comps==1 is preserved. Iterative because removing
			// one stub can expose another. Only touches shapes that actually have stubs.
			auto StubPrune = [&](TArray<FArcRec>& E)
			{
				bool bCh = true;
				while (bCh)
				{
					bCh = false;
					TArray<int32> Deg;
					Deg.Init(0, NodePos.Num());
					for (const FArcRec& e : E) { Deg[e.A]++; Deg[e.B]++; }
					TArray<FArcRec> Kept;
					Kept.Reserve(E.Num());
					for (const FArcRec& e : E)
					{
						if ((e.A >= NumContourNodes && Deg[e.A] == 1) || (e.B >= NumContourNodes && Deg[e.B] == 1)) { bCh = true; continue; }
						Kept.Add(e);
					}
					E = MoveTemp(Kept);
				}
			};

			Dissolve(Arcs);
			StubPrune(Arcs);
			Dissolve(Arcs);
		}

		// ---- 6. Weld near-coincident nodes + assemble output (translate back) --------------------------
		// Single-linkage (union-find) weld. A near-degenerate event spreads what should be ONE junction over
		// several marching samples -- e.g. the Hexagon's slow reflex collapse emits 3 nodes ~15-30 apart over
		// t=318..344. A single-rep weld leaves that as a tiny self-crossing cluster (the chain's far end never
		// reaches the first rep); chaining collapses the whole run. Radius = max(user merge distance, ~5x the
		// DEFAULT-resolution step) -- a fixed fraction of the shape, INDEPENDENT of the sampling Resolution so
		// fine sampling cannot shrink it below an event's spread. Contour nodes are the lowest indices and we
		// root each set at its min index, so a cluster touching the boundary stays anchored there (spokes preserved).
		const int32 NumWork = NodePos.Num();
		const double WeldR = FMath::Max(MergeDistance, BaseDelta * 5.0);
		const double WeldR2 = WeldR * WeldR;

		// Only nodes referenced by a surviving arc (or contour) need to exist.
		TBitArray<> Referenced(false, NumWork);
		for (int32 i = 0; i < NumContourNodes; i++) { Referenced[i] = true; }
		for (const FArcRec& Arc : Arcs) { Referenced[Arc.A] = true; Referenced[Arc.B] = true; }

		TArray<int32> Parent;
		Parent.SetNumUninitialized(NumWork);
		for (int32 i = 0; i < NumWork; i++) { Parent[i] = i; }
		auto Find = [&Parent](int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
		auto Union = [&](const int32 A, const int32 B) { const int32 RA = Find(A); const int32 RB = Find(B); if (RA != RB) { Parent[FMath::Max(RA, RB)] = FMath::Min(RA, RB); } };

		// Spatial grid (cell = WeldR) over referenced nodes; union every pair within WeldR as we insert.
		TMap<FIntPoint, TArray<int32>> Grid;
		auto CellOf = [WeldR](const FVector2D& P) { return FIntPoint(FMath::FloorToInt(P.X / WeldR), FMath::FloorToInt(P.Y / WeldR)); };
		for (int32 i = 0; i < NumWork; i++)
		{
			if (!Referenced[i]) { continue; }
			const FIntPoint C = CellOf(NodePos[i]);
			for (int32 dx = -1; dx <= 1; dx++) { for (int32 dy = -1; dy <= 1; dy++) { if (const TArray<int32>* Cell = Grid.Find(FIntPoint(C.X + dx, C.Y + dy))) { for (const int32 J : *Cell) { if (FVector2D::DistSquared(NodePos[i], NodePos[J]) <= WeldR2) { Union(i, J); } } } } }
			Grid.FindOrAdd(C).Add(i);
		}

		TArray<int32> OldToRep;
		OldToRep.Init(-1, NumWork);
		for (int32 i = 0; i < NumWork; i++) { if (Referenced[i]) { OldToRep[i] = Find(i); } }

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

		// ---- 6b. Planarity repair: a wavefront merge can attach an arc to the FAR end of a short junction
		// ridge (the live hub) instead of the near BEND it actually meets, crossing a neighbouring spoke. Where
		// two skeleton edges properly cross, slide the over-reaching edge's endpoint onto an endpoint of the
		// other edge -- but ONLY when that SHORTENS it. The shortening test singles out the over-reaching edge
		// and never disturbs a correctly-placed one. Two slide modes, strict first: (a) the target is directly
		// adjacent to the slid-from endpoint (trivially connectivity-safe); (b) FALLBACK, only when no strict move
		// exists (e.g. a fine-sampled near-degenerate hub resolves into a 2-node ridge, so the correct target is
		// 2+ hops away) -- reconnect across the gap, with a reachability check guaranteeing the slide never
		// disconnects the skeleton (the over-reaching edge can be the SOLE bridge to a whole feature). The
		// fallback is gated on the strict pass finding nothing, so every already-clean result is untouched.
		// Iterate to clear cascades.
		{
			auto Orient = [](const FVector2D& P, const FVector2D& Q, const FVector2D& R) { return (Q.X - P.X) * (R.Y - P.Y) - (Q.Y - P.Y) * (R.X - P.X); };
			auto Crosses = [&](const int32 a, const int32 b, const int32 cc, const int32 dd)
			{
				const double D1 = Orient(Nodes[cc].Pos, Nodes[dd].Pos, Nodes[a].Pos), D2 = Orient(Nodes[cc].Pos, Nodes[dd].Pos, Nodes[b].Pos);
				const double D3 = Orient(Nodes[a].Pos, Nodes[b].Pos, Nodes[cc].Pos), D4 = Orient(Nodes[a].Pos, Nodes[b].Pos, Nodes[dd].Pos);
				return ((D1 > 0 && D2 < 0) || (D1 < 0 && D2 > 0)) && ((D3 > 0 && D4 < 0) || (D3 < 0 && D4 > 0));
			};
			for (int32 Iter = 0; Iter < 16; Iter++)
			{
				TArray<TSet<int32>> Adj;
				Adj.SetNum(Nodes.Num());
				for (const FStraightSkeletonEdge& E : Edges) { Adj[E.A].Add(E.B); Adj[E.B].Add(E.A); }

				// Can Slide still reach New if the single edge (AvoidA,AvoidB) is deleted? The fallback slide below
				// uses this to guarantee a reconnection never splits the skeleton -- essential because an over-
				// reaching edge can be the SOLE bridge to a whole feature (a pinwheel arm, a hole's region).
				auto Reachable = [&](const int32 Start, const int32 Target, const int32 AvoidA, const int32 AvoidB) -> bool
				{
					if (Start == Target) { return true; }
					TBitArray<> Visited(false, Nodes.Num());
					Visited[Start] = true;
					TArray<int32> Stack;
					Stack.Add(Start);
					while (Stack.Num() > 0)
					{
						const int32 U = Stack.Pop(EAllowShrinking::No);
						for (const int32 V : Adj[U])
						{
							if ((U == AvoidA && V == AvoidB) || (U == AvoidB && V == AvoidA)) { continue; }
							if (Visited[V]) { continue; }
							if (V == Target) { return true; }
							Visited[V] = true;
							Stack.Add(V);
						}
					}
					return false;
				};

				bool bFixed = false;
				for (int32 i = 0; i < Edges.Num() && !bFixed; i++)
				{
					for (int32 j = i + 1; j < Edges.Num() && !bFixed; j++)
					{
						const int32 A = Edges[i].A, B = Edges[i].B, X = Edges[j].A, Y = Edges[j].B;
						if (A == X || A == Y || B == X || B == Y) { continue; }
						if (!Crosses(A, B, X, Y)) { continue; }
						int32 BestE = -1, BestKeep = -1, BestNew = -1;
						double BestLen = TNumericLimits<double>::Max();
						// Slide edge Ei's far endpoint Slide -> New (New = an endpoint of the OTHER crossing edge, so
						// the reconnected edge shares a node with it and can no longer cross it), keeping Keep, ONLY
						// when that SHORTENS. bRelaxed=false: New must be directly adjacent to Slide (trivially
						// connectivity-safe). bRelaxed=true: New need only stay reachable from Slide without the
						// removed edge -- a 2+-hop reconnect that still cannot disconnect the graph.
						auto Try = [&](const int32 Ei, const int32 Keep, const int32 Slide, const int32 New, const bool bRelaxed)
						{
							if (New == Keep || Adj[Keep].Contains(New)) { return; }
							const bool bLinkOk = bRelaxed ? Reachable(Slide, New, Keep, Slide) : Adj[Slide].Contains(New);
							if (!bLinkOk) { return; }
							const double OldL = FVector2D::DistSquared(Nodes[Keep].Pos, Nodes[Slide].Pos);
							const double NewL = FVector2D::DistSquared(Nodes[Keep].Pos, Nodes[New].Pos);
							if (NewL >= OldL || NewL >= BestLen) { return; }
							BestLen = NewL; BestE = Ei; BestKeep = Keep; BestNew = New;
						};
						// Primary: the strict adjacent slide (original behaviour). Every already-clean result is
						// produced here, so gating the fallback on "primary found nothing" leaves them untouched.
						Try(i, A, B, X, false); Try(i, A, B, Y, false); Try(i, B, A, X, false); Try(i, B, A, Y, false);
						Try(j, X, Y, A, false); Try(j, X, Y, B, false); Try(j, Y, X, A, false); Try(j, Y, X, B, false);
						if (BestE < 0)
						{
							// Fallback: the over-reaching edge has no adjacent endpoint to collapse onto -- its correct
							// target is 2+ hops away (a fine-sampled near-degenerate hub resolved into a short ridge).
							// Reconnect across the gap; Reachable keeps it connectivity-safe.
							Try(i, A, B, X, true); Try(i, A, B, Y, true); Try(i, B, A, X, true); Try(i, B, A, Y, true);
							Try(j, X, Y, A, true); Try(j, X, Y, B, true); Try(j, Y, X, A, true); Try(j, Y, X, B, true);
						}
						if (BestE >= 0)
						{
							Edges[BestE].A = BestKeep;
							Edges[BestE].B = BestNew;
							Edges[BestE].bPerimeterIncident = Nodes[BestKeep].bIsContour || Nodes[BestNew].bIsContour;
							bFixed = true;
						}
					}
				}
				if (!bFixed) { break; }
			}
			TSet<uint64> Seen;
			TArray<FStraightSkeletonEdge> Kept;
			Kept.Reserve(Edges.Num());
			for (const FStraightSkeletonEdge& E : Edges)
			{
				if (E.A == E.B) { continue; }
				const uint64 K = PCGEx::H64U(static_cast<uint32>(E.A), static_cast<uint32>(E.B));
				if (Seen.Contains(K)) { continue; }
				Seen.Add(K);
				Kept.Add(E);
			}
			Edges = MoveTemp(Kept);
		}

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
