// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Math/Geo/PCGExStraightSkeleton.h"

#include "Algo/Reverse.h"
#include "PCGExH.h"

// =====================================================================================================
// Straight-skeleton solver -- wavefront propagation (SLAV / Felkel-Obdrzalek).
//
// Conventions (the whole file relies on these being consistent):
//   * The solid interior is ALWAYS to the LEFT of a directed edge. Callers must therefore feed the
//     outer boundary counter-clockwise (signed area > 0) and holes clockwise (signed area < 0). The
//     solver re-normalizes winding defensively so it is also correct when used standalone.
//   * For a unit edge direction D, the inward (into-solid) unit normal is LeftNormal(D) = (-D.y, D.x).
//   * A vertex's travel direction (bisector) is normalize(Nleft + Nright) of its two incident edge
//     normals. This is provably identical to the classic edge-direction bisector (edge_right.dir -
//     edge_left.dir, negated for reflex) for both convex and reflex vertices.
//   * Event "time" == inward offset distance == dot(point - edge.P, edge.Normal).
//
// The highest-risk area for correctness is the split-event predicate (ComputeSplitCandidate) and its
// re-resolution at handling time (HandleSplit). Those are exercised by the M1c unit tests.
//
// File-local types/helpers live in a uniquely named namespace (Unity-build safe -- see CLAUDE.md).
// =====================================================================================================

namespace PCGExStraightSkeleton
{
	using namespace PCGExMath::Geo;

	// Dimensionless threshold for "parallel" tests on unit-length directions.
	constexpr double PARALLEL_EPS = 1e-7;
	// Side/region test slack (works on normalized vectors, so effectively dimensionless).
	constexpr double SIDE_EPS = 1e-4;
	// Distance tolerance in world units (cm) for "in the future" / behind-edge comparisons.
	constexpr double TIME_EPS = 1e-3;

	FORCEINLINE double Cross(const FVector2D& A, const FVector2D& B)
	{
		return A.X * B.Y - A.Y * B.X;
	}

	// Inward (into-solid) unit normal for a CCW-wound edge direction.
	FORCEINLINE FVector2D LeftNormal(const FVector2D& D)
	{
		return FVector2D(-D.Y, D.X);
	}

	// Intersection of two infinite lines P1+t*D1 and P2+s*D2. False if near-parallel.
	FORCEINLINE bool IntersectLines(const FVector2D& P1, const FVector2D& D1, const FVector2D& P2, const FVector2D& D2, FVector2D& Out)
	{
		const double Den = Cross(D1, D2);
		if (FMath::Abs(Den) < PARALLEL_EPS) { return false; }
		const FVector2D Delta = P2 - P1;
		Out = P1 + D1 * (Cross(Delta, D2) / Den);
		return true;
	}

	// Intersection of two rays (origins P1/P2, unit directions D1/D2); requires the hit to be ahead of both.
	FORCEINLINE bool IntersectRays(const FVector2D& P1, const FVector2D& D1, const FVector2D& P2, const FVector2D& D2, FVector2D& Out)
	{
		const double Den = Cross(D1, D2);
		if (FMath::Abs(Den) < PARALLEL_EPS) { return false; }
		const FVector2D Delta = P2 - P1;
		const double T = Cross(Delta, D2) / Den;
		const double S = Cross(Delta, D1) / Den;
		if (T < -TIME_EPS || S < -TIME_EPS) { return false; }
		Out = P1 + D1 * T;
		return true;
	}

	struct FEdgeData
	{
		FVector2D P = FVector2D::ZeroVector;      // Start endpoint (original vertex).
		FVector2D Dir = FVector2D::ZeroVector;    // Unit direction (start -> end).
		FVector2D Normal = FVector2D::ZeroVector; // Inward unit normal (into solid).

		// Bisector rays at the edge's two endpoints, captured at init. They bound the region this
		// edge sweeps as the wavefront advances and are used to validate split events.
		FVector2D BisStartP = FVector2D::ZeroVector, BisStartD = FVector2D::ZeroVector; // at start endpoint
		FVector2D BisEndP = FVector2D::ZeroVector, BisEndD = FVector2D::ZeroVector;     // at end endpoint

		int32 StartVtx = -1; // init-time only
		int32 EndVtx = -1;   // init-time only
	};

	struct FVertex
	{
		FVector2D Origin = FVector2D::ZeroVector; // Position at StartTime.
		double StartTime = 0.0;
		FVector2D Bisector = FVector2D::ZeroVector; // Unit travel direction.
		bool bReflex = false;

		int32 Prev = -1;
		int32 Next = -1;
		int32 LeftEdge = -1;  // Original edge shared with Prev.
		int32 RightEdge = -1; // Original edge shared with Next.
		int32 NodeId = -1;    // Skeleton node this vertex currently emanates from.
		bool bValid = true;
	};

	enum class EKind : uint8 { Edge, Split };

	struct FEvent
	{
		double Distance = 0.0; // Heap key (offset time).
		FVector2D Point = FVector2D::ZeroVector;
		int32 Va = -1;       // Edge: first endpoint. Split: the reflex vertex.
		int32 Vb = -1;       // Edge: second endpoint (== Va.Next). Split: unused.
		int32 EdgeIdx = -1;  // Split: opposite edge. Edge: unused.
		EKind Kind = EKind::Edge;
	};

	// Min-heap order: earliest (smallest offset) event first.
	struct FEventLess
	{
		FORCEINLINE bool operator()(const FEvent& A, const FEvent& B) const { return A.Distance < B.Distance; }
	};

	struct FArc
	{
		int32 A = -1;
		int32 B = -1;
		ESkeletonEventType Event = ESkeletonEventType::None;
	};

	struct FSolver
	{
		TArray<FEdgeData> Edges;
		TArray<FVertex> Verts;
		TArray<FEvent> Heap;

		// Working node table (welded & re-indexed at the end).
		TArray<FVector2D> NodePos;
		TArray<double> NodeTime;
		TArray<bool> NodeContour;
		TArray<FArc> Arcs;

		int32 NumContour = 0;
		TArray<TArray<int32>> LoopNodes; // contour node ids per loop, in order (for contour edges)

		int32 AddNode(const FVector2D& P, const double Time, const bool bContour)
		{
			NodePos.Add(P);
			NodeTime.Add(Time);
			NodeContour.Add(bContour);
			return NodePos.Num() - 1;
		}

		// Signed area (shoelace); > 0 == CCW.
		static double SignedArea(const TArray<FVector2D>& Loop)
		{
			double A = 0;
			const int32 N = Loop.Num();
			for (int32 i = 0; i < N; i++) { A += Cross(Loop[i], Loop[(i + 1) % N]); }
			return A * 0.5;
		}

		// Drops consecutive duplicate points; returns false if fewer than 3 remain.
		static bool Sanitize(const TArrayView<const FVector2D>& In, TArray<FVector2D>& Out)
		{
			Out.Reset(In.Num());
			for (const FVector2D& P : In)
			{
				if (Out.Num() == 0 || FVector2D::DistSquared(Out.Last(), P) > TIME_EPS * TIME_EPS) { Out.Add(P); }
			}
			// Closing duplicate.
			if (Out.Num() > 1 && FVector2D::DistSquared(Out.Last(), Out[0]) <= TIME_EPS * TIME_EPS) { Out.Pop(); }
			return Out.Num() >= 3;
		}

		void ComputeVertexGeometry(const int32 Vi)
		{
			FVertex& V = Verts[Vi];
			const FEdgeData& EL = Edges[V.LeftEdge];
			const FEdgeData& ER = Edges[V.RightEdge];

			// Bisector == normalize(Nleft + Nright). When the two edges are parallel and face each other
			// (antiparallel normals -> Sum ~ 0, the "ridge" case common in rectilinear shapes) the vertex
			// instead travels ALONG the edges; the forward traversal tangent is the outgoing edge direction.
			const FVector2D Sum = EL.Normal + ER.Normal;
			V.Bisector = Sum.SizeSquared() < PARALLEL_EPS ? ER.Dir : Sum.GetSafeNormal();
			V.bReflex = Cross(EL.Dir, ER.Dir) < -PARALLEL_EPS;
		}

		// Position of a vertex at offset time t == intersection of its two edges offset inward by t.
		FVector2D PosAt(const int32 Vi, const double T) const
		{
			const FVertex& V = Verts[Vi];
			const FEdgeData& EL = Edges[V.LeftEdge];
			const FEdgeData& ER = Edges[V.RightEdge];
			FVector2D Out;
			if (IntersectLines(EL.P + EL.Normal * T, EL.Dir, ER.P + ER.Normal * T, ER.Dir, Out)) { return Out; }
			return V.Origin;
		}

		bool ComputeSplitCandidate(const int32 Vi, const int32 e, FVector2D& OutB, double& OutDist) const
		{
			const FVertex& V = Verts[Vi];
			const FEdgeData& E = Edges[e];
			const FEdgeData& EL = Edges[V.LeftEdge];
			const FEdgeData& ER = Edges[V.RightEdge];

			// Use the incident edge that is least parallel to E to find the apex of the swept wedge.
			const double LDot = FMath::Abs(FVector2D::DotProduct(EL.Dir, E.Dir));
			const double RDot = FMath::Abs(FVector2D::DotProduct(ER.Dir, E.Dir));
			const FEdgeData& SelfEdge = LDot < RDot ? EL : ER;

			FVector2D Apex;
			if (!IntersectLines(SelfEdge.P, SelfEdge.Dir, E.P, E.Dir, Apex)) { return false; }

			FVector2D LinVec = V.Origin - Apex;
			if (LinVec.SizeSquared() < PARALLEL_EPS) { return false; }
			LinVec = LinVec.GetSafeNormal();

			FVector2D EdVec = E.Dir;
			if (FVector2D::DotProduct(LinVec, EdVec) < 0) { EdVec = -EdVec; }

			const FVector2D BisVec = EdVec + LinVec;
			if (BisVec.SizeSquared() < PARALLEL_EPS) { return false; }

			// Candidate point: where the apex bisector meets the reflex vertex's bisector.
			FVector2D B;
			if (!IntersectLines(Apex, BisVec, V.Origin, V.Bisector, B)) { return false; }
			if (FVector2D::DotProduct(B - V.Origin, V.Bisector) < -TIME_EPS) { return false; } // must be ahead of V

			// B must lie inside the region edge E sweeps: solid-side of E and between its endpoint bisectors.
			if (Cross(E.Dir, (B - E.P).GetSafeNormal()) <= SIDE_EPS) { return false; } // strictly solid-side

			// Left wall (at start endpoint): B must be on the same side as the edge's far (end) endpoint.
			{
				const double Ref = Cross(E.BisStartD, E.Dir); // E.Dir == direction toward end endpoint
				const double Tst = Cross(E.BisStartD, (B - E.BisStartP).GetSafeNormal());
				if (FMath::Abs(Ref) > PARALLEL_EPS && Tst * Ref < -SIDE_EPS) { return false; }
			}
			// Right wall (at end endpoint): B must be on the same side as the start endpoint.
			{
				const double Ref = Cross(E.BisEndD, -E.Dir); // toward start endpoint
				const double Tst = Cross(E.BisEndD, (B - E.BisEndP).GetSafeNormal());
				if (FMath::Abs(Ref) > PARALLEL_EPS && Tst * Ref < -SIDE_EPS) { return false; }
			}

			const double Dist = FVector2D::DotProduct(B - E.P, E.Normal);
			if (Dist < -TIME_EPS) { return false; }
			if (Dist + TIME_EPS < V.StartTime) { return false; }

			OutB = B;
			OutDist = Dist;
			return true;
		}

		bool ComputeEvent(const int32 Vi, FEvent& Out) const
		{
			const FVertex& V = Verts[Vi];
			if (!V.bValid || V.Next == Vi || V.Prev == Vi) { return false; }

			double BestDist = TNumericLimits<double>::Max();
			bool bFound = false;
			FEvent Best;

			auto ConsiderEdge = [&](const int32 Ua, const int32 Ub, const int32 SharedEdge)
			{
				if (Ua < 0 || Ub < 0 || !Verts[Ua].bValid || !Verts[Ub].bValid) { return; }
				FVector2D I;
				if (!IntersectRays(Verts[Ua].Origin, Verts[Ua].Bisector, Verts[Ub].Origin, Verts[Ub].Bisector, I))
					{
						// Bisectors are parallel. This is the "opposing ridge vertices" case: when two parallel
						// input edges' wavefronts meet, the vertices on the resulting ridge move collinearly
						// toward each other, so their shared edge DOES collapse -- but ray intersection can't
						// see it. Detect the head-on approach and collapse them at their midpoint; otherwise the
						// edge genuinely never collapses and there is no event.
						const FVector2D D = Verts[Ub].Origin - Verts[Ua].Origin;
						if (D.SizeSquared() < TIME_EPS * TIME_EPS)
						{
							I = Verts[Ua].Origin; // already coincident
						}
						else if (FVector2D::DotProduct(Verts[Ua].Bisector, D) > SIDE_EPS && FVector2D::DotProduct(Verts[Ub].Bisector, D) < -SIDE_EPS)
						{
							I = (Verts[Ua].Origin + Verts[Ub].Origin) * 0.5; // head-on -> meet midway
						}
						else
						{
							return;
						}
					}
				const FEdgeData& Ed = Edges[SharedEdge];
				const double Dist = FVector2D::DotProduct(I - Ed.P, Ed.Normal);
				if (Dist < -TIME_EPS) { return; }
				if (Dist + TIME_EPS < FMath::Max(Verts[Ua].StartTime, Verts[Ub].StartTime)) { return; }
				if (Dist < BestDist)
				{
					BestDist = Dist;
					Best.Kind = EKind::Edge;
					Best.Distance = Dist;
					Best.Point = I;
					Best.Va = Ua;
					Best.Vb = Ub;
					Best.EdgeIdx = -1;
					bFound = true;
				}
			};

			ConsiderEdge(V.Prev, Vi, V.LeftEdge);  // edge between Prev and V
			ConsiderEdge(Vi, V.Next, V.RightEdge); // edge between V and Next

			if (V.bReflex)
			{
				for (int32 e = 0; e < Edges.Num(); e++)
				{
					if (e == V.LeftEdge || e == V.RightEdge) { continue; }
					FVector2D B;
					double Dist;
					if (ComputeSplitCandidate(Vi, e, B, Dist) && Dist < BestDist)
					{
						BestDist = Dist;
						Best.Kind = EKind::Split;
						Best.Distance = Dist;
						Best.Point = B;
						Best.Va = Vi;
						Best.Vb = -1;
						Best.EdgeIdx = e;
						bFound = true;
					}
				}
			}

			if (bFound) { Out = Best; }
			return bFound;
		}

		void PushEvent(const int32 Vi)
		{
			FEvent E;
			if (ComputeEvent(Vi, E)) { Heap.HeapPush(E, FEventLess()); }
		}

		bool Init(const TArrayView<const FVector2D>& Outer, const TArrayView<const TArray<FVector2D>>& Holes)
		{
			TArray<TArray<FVector2D>> Loops;

			TArray<FVector2D> CleanOuter;
			if (!Sanitize(Outer, CleanOuter)) { return false; }
			if (SignedArea(CleanOuter) < 0) { Algo::Reverse(CleanOuter); } // ensure CCW
			Loops.Add(MoveTemp(CleanOuter));

			for (const TArray<FVector2D>& Hole : Holes)
			{
				TArray<FVector2D> CleanHole;
				if (!Sanitize(Hole, CleanHole)) { continue; } // skip degenerate holes
				if (SignedArea(CleanHole) > 0) { Algo::Reverse(CleanHole); } // ensure CW
				Loops.Add(MoveTemp(CleanHole));
			}

			LoopNodes.Reserve(Loops.Num());

			// First pass: contour nodes + vertices + edges per loop.
			for (const TArray<FVector2D>& Loop : Loops)
			{
				const int32 N = Loop.Num();
				const int32 VtxStart = Verts.Num();
				const int32 EdgeStart = Edges.Num();

				TArray<int32>& LoopNodeIds = LoopNodes.Emplace_GetRef();
				LoopNodeIds.Reserve(N);

				for (int32 i = 0; i < N; i++)
				{
					const int32 NodeId = AddNode(Loop[i], 0.0, true);
					LoopNodeIds.Add(NodeId);

					FVertex V;
					V.Origin = Loop[i];
					V.StartTime = 0.0;
					V.NodeId = NodeId;
					V.Prev = VtxStart + (i + N - 1) % N;
					V.Next = VtxStart + (i + 1) % N;
					V.LeftEdge = EdgeStart + (i + N - 1) % N;
					V.RightEdge = EdgeStart + i;
					Verts.Add(V);
				}

				for (int32 i = 0; i < N; i++)
				{
					const FVector2D A = Loop[i];
					const FVector2D B = Loop[(i + 1) % N];
					FEdgeData Ed;
					Ed.P = A;
					Ed.Dir = (B - A).GetSafeNormal();
					Ed.Normal = LeftNormal(Ed.Dir);
					Ed.StartVtx = VtxStart + i;
					Ed.EndVtx = VtxStart + (i + 1) % N;
					Edges.Add(Ed);
				}
			}

			NumContour = NodePos.Num();

			// Second pass: vertex bisectors/reflex (needs both edges set).
			for (int32 i = 0; i < Verts.Num(); i++) { ComputeVertexGeometry(i); }

			// Third pass: capture each edge's endpoint bisectors (needs vertex bisectors).
			for (FEdgeData& Ed : Edges)
			{
				Ed.BisStartP = Verts[Ed.StartVtx].Origin;
				Ed.BisStartD = Verts[Ed.StartVtx].Bisector;
				Ed.BisEndP = Verts[Ed.EndVtx].Origin;
				Ed.BisEndD = Verts[Ed.EndVtx].Bisector;
			}

			return true;
		}

		void HandleEdgeEvent(const FEvent& E)
		{
			const int32 Va = E.Va;
			const int32 Vb = E.Vb;
			if (!Verts[Va].bValid || !Verts[Vb].bValid || Verts[Va].Next != Vb) { return; } // stale

			const int32 NewNode = AddNode(E.Point, E.Distance, false);

			// 2-gon: the whole loop is just {Va, Vb}.
			if (Verts[Vb].Next == Va)
			{
				Arcs.Add({Verts[Va].NodeId, NewNode, ESkeletonEventType::EdgeEvent});
				Arcs.Add({Verts[Vb].NodeId, NewNode, ESkeletonEventType::EdgeEvent});
				Verts[Va].bValid = false;
				Verts[Vb].bValid = false;
				return;
			}

			// 3-gon (triangle peak): {Va, Vb, X} where X == Va.Prev == Vb.Next.
			const int32 P = Verts[Va].Prev;
			const int32 N = Verts[Vb].Next;
			if (P == N)
			{
				Arcs.Add({Verts[Va].NodeId, NewNode, ESkeletonEventType::EdgeEvent});
				Arcs.Add({Verts[Vb].NodeId, NewNode, ESkeletonEventType::EdgeEvent});
				Arcs.Add({Verts[P].NodeId, NewNode, ESkeletonEventType::EdgeEvent});
				Verts[Va].bValid = false;
				Verts[Vb].bValid = false;
				Verts[P].bValid = false;
				return;
			}

			// General case: collapse the shared edge, splice in a replacement vertex.
			Arcs.Add({Verts[Va].NodeId, NewNode, ESkeletonEventType::EdgeEvent});
			Arcs.Add({Verts[Vb].NodeId, NewNode, ESkeletonEventType::EdgeEvent});

			FVertex Vn;
			Vn.Origin = E.Point;
			Vn.StartTime = E.Distance;
			Vn.NodeId = NewNode;
			Vn.Prev = P;
			Vn.Next = N;
			Vn.LeftEdge = Verts[Va].LeftEdge;
			Vn.RightEdge = Verts[Vb].RightEdge;
			const int32 Vni = Verts.Add(Vn);

			Verts[P].Next = Vni;
			Verts[N].Prev = Vni;
			Verts[Va].bValid = false;
			Verts[Vb].bValid = false;

			ComputeVertexGeometry(Vni);

			// Re-schedule the new vertex AND its two neighbours. The neighbours' previously-queued events
			// were paired with the now-consumed Va/Vb and are stale; without fresh events they can be left
			// stranded when several events land at the same offset (common on symmetric / rectilinear input),
			// which silently drops their skeleton arcs.
			PushEvent(Vni);
			PushEvent(P);
			PushEvent(N);
		}

		void HandleSplitEvent(const FEvent& E)
		{
			const int32 V = E.Va;
			const int32 e = E.EdgeIdx;
			if (!Verts[V].bValid) { return; } // stale

			// Re-resolve which wavefront segment currently lying on original edge e contains the split point.
			int32 X = -1;
			for (int32 c = 0; c < Verts.Num(); c++)
			{
				if (!Verts[c].bValid || Verts[c].RightEdge != e) { continue; }
				const int32 Y = Verts[c].Next;
				if (Y < 0 || !Verts[Y].bValid) { continue; }
				const FVector2D XP = PosAt(c, E.Distance);
				const FVector2D YP = PosAt(Y, E.Distance);
				const FVector2D Seg = YP - XP;
				const double L2 = Seg.SizeSquared();
				const double Proj = L2 > PARALLEL_EPS ? FVector2D::DotProduct(E.Point - XP, Seg) / L2 : 0.0;
				if (Proj >= -0.02 && Proj <= 1.02) { X = c; break; }
			}
			if (X < 0) { return; } // stale / no valid bracket

			const int32 Y = Verts[X].Next;
			const int32 NewNode = AddNode(E.Point, E.Distance, false);
			Arcs.Add({Verts[V].NodeId, NewNode, ESkeletonEventType::SplitEvent});

			const int32 Pv = Verts[V].Prev;
			const int32 Nv = Verts[V].Next;

			// V1 carries (V.LeftEdge, e); V2 carries (e, V.RightEdge). Splicing them around e splits the
			// LAV into two cycles -- or merges two cycles into one when V and e came from different loops.
			FVertex V1;
			V1.Origin = E.Point;
			V1.StartTime = E.Distance;
			V1.NodeId = NewNode;
			V1.LeftEdge = Verts[V].LeftEdge;
			V1.RightEdge = e;
			V1.Prev = Pv;
			V1.Next = Y;
			const int32 V1i = Verts.Add(V1);

			FVertex V2;
			V2.Origin = E.Point;
			V2.StartTime = E.Distance;
			V2.NodeId = NewNode;
			V2.LeftEdge = e;
			V2.RightEdge = Verts[V].RightEdge;
			V2.Prev = X;
			V2.Next = Nv;
			const int32 V2i = Verts.Add(V2);

			Verts[Pv].Next = V1i;
			Verts[Y].Prev = V1i;
			Verts[X].Next = V2i;
			Verts[Nv].Prev = V2i;
			Verts[V].bValid = false;

			ComputeVertexGeometry(V1i);
			ComputeVertexGeometry(V2i);

			// Re-schedule the two new vertices AND every neighbour whose adjacency we just rewired (Pv, Y, X,
			// Nv). Any of them may have been holding a stale event paired with the consumed V; leaving them
			// unscheduled is the stranding that disconnects skeletons when events coincide. PushEvent is a
			// no-op on already-invalid vertices, so spurious pushes here are harmless.
			PushEvent(V1i);
			PushEvent(V2i);
			PushEvent(Pv);
			PushEvent(Y);
			PushEvent(X);
			PushEvent(Nv);
		}

		void Run()
		{
			const int32 NumInitial = Verts.Num();
			for (int32 i = 0; i < NumInitial; i++) { PushEvent(i); }

			const int32 MaxIters = NumInitial * 64 + 512; // progress backstop; re-scheduling neighbours adds stale-skip churn
			int32 Iters = 0;

			FEvent E;
			while (Heap.Num() > 0)
			{
				if (++Iters > MaxIters) { break; }
				Heap.HeapPop(E, FEventLess(), EAllowShrinking::No);
				if (E.Kind == EKind::Edge) { HandleEdgeEvent(E); }
				else { HandleSplitEvent(E); }
			}
		}

		// Weld near-coincident nodes, re-index, classify, and emit into the public result arrays.
		void Build(const double MergeDistance, const bool bIncludeContour, TArray<FStraightSkeletonNode>& OutNodes, TArray<FStraightSkeletonEdge>& OutEdges, int32& OutNumContour)
		{
			const int32 NumNodes = NodePos.Num();
			const double WeldR = FMath::Max(MergeDistance, TIME_EPS);
			const double WeldR2 = WeldR * WeldR;

			TMap<FIntPoint, TArray<int32>> Grid; // cell -> representative old-node ids
			TArray<int32> OldToRep;
			OldToRep.Init(-1, NumNodes);

			auto CellOf = [WeldR](const FVector2D& P) { return FIntPoint(FMath::FloorToInt(P.X / WeldR), FMath::FloorToInt(P.Y / WeldR)); };

			auto FindRep = [&](const FVector2D& P) -> int32
			{
				const FIntPoint C = CellOf(P);
				int32 BestRep = -1;
				double BestD2 = WeldR2;
				for (int32 dx = -1; dx <= 1; dx++)
				{
					for (int32 dy = -1; dy <= 1; dy++)
					{
						if (const TArray<int32>* Cell = Grid.Find(FIntPoint(C.X + dx, C.Y + dy)))
						{
							for (const int32 Rep : *Cell)
							{
								const double D2 = FVector2D::DistSquared(NodePos[Rep], P);
								if (D2 <= BestD2) { BestD2 = D2; BestRep = Rep; }
							}
						}
					}
				}
				return BestRep;
			};

			auto Register = [&](const int32 Old)
			{
				OldToRep[Old] = Old;
				Grid.FindOrAdd(CellOf(NodePos[Old])).Add(Old);
			};

			// Contour nodes anchor positions: register them first so skeleton nodes weld onto them.
			for (int32 i = 0; i < NumContour; i++)
			{
				const int32 Rep = FindRep(NodePos[i]);
				if (Rep >= 0) { OldToRep[i] = Rep; }
				else { Register(i); }
			}
			for (int32 i = NumContour; i < NumNodes; i++)
			{
				const int32 Rep = FindRep(NodePos[i]);
				if (Rep >= 0) { OldToRep[i] = Rep; }
				else { Register(i); }
			}

			// Compact representatives into final node indices (contour reps come first by construction).
			TArray<int32> RepToNew;
			RepToNew.Init(-1, NumNodes);
			OutNodes.Reset();
			OutNumContour = 0;
			for (int32 i = 0; i < NumNodes; i++)
			{
				if (OldToRep[i] != i) { continue; } // not a representative
				RepToNew[i] = OutNodes.Num();
				OutNodes.Add(FStraightSkeletonNode(NodePos[i], NodeTime[i], NodeContour[i]));
				if (NodeContour[i]) { OutNumContour++; }
			}

			auto Final = [&](const int32 Old) { return RepToNew[OldToRep[Old]]; };

			// Deduplicate edges; on collision prefer Contour type and the more specific event.
			TMap<uint64, int32> EdgeMap; // H64U(min,max) -> index into OutEdges
			OutEdges.Reset();

			auto AddEdge = [&](const int32 A, const int32 B, const ESkeletonEdgeType Type, const ESkeletonEventType Event)
			{
				if (A < 0 || B < 0 || A == B) { return; }
				const uint64 Key = PCGEx::H64U(static_cast<uint32>(A), static_cast<uint32>(B));
				const bool bPerimeter = OutNodes[A].bIsContour || OutNodes[B].bIsContour;
				if (const int32* Existing = EdgeMap.Find(Key))
				{
					FStraightSkeletonEdge& Prev = OutEdges[*Existing];
					if (Type == ESkeletonEdgeType::Contour) { Prev.Type = ESkeletonEdgeType::Contour; }
					if (static_cast<uint8>(Event) > static_cast<uint8>(Prev.Event)) { Prev.Event = Event; }
					return;
				}
				EdgeMap.Add(Key, OutEdges.Num());
				OutEdges.Add(FStraightSkeletonEdge(A, B, Type, Event, bPerimeter));
			};

			for (const FArc& Arc : Arcs) { AddEdge(Final(Arc.A), Final(Arc.B), ESkeletonEdgeType::Skeleton, Arc.Event); }

			if (bIncludeContour)
			{
				for (const TArray<int32>& Loop : LoopNodes)
				{
					const int32 N = Loop.Num();
					for (int32 i = 0; i < N; i++) { AddEdge(Final(Loop[i]), Final(Loop[(i + 1) % N]), ESkeletonEdgeType::Contour, ESkeletonEventType::None); }
				}
			}
		}
	};
}

namespace PCGExMath::Geo
{
	void TStraightSkeleton2::Clear()
	{
		Nodes.Reset();
		Edges.Reset();
		NumContourNodes = 0;
		IsValid = false;
	}

	bool TStraightSkeleton2::Process(
		const TArrayView<const FVector2D>& Outer,
		const TArrayView<const TArray<FVector2D>>& Holes,
		const double MergeDistance,
		const bool bIncludeContour)
	{
		Clear();

		PCGExStraightSkeleton::FSolver Solver;
		if (!Solver.Init(Outer, Holes)) { return false; }

		Solver.Run();
		Solver.Build(MergeDistance, bIncludeContour, Nodes, Edges, NumContourNodes);

		IsValid = Edges.Num() > 0;
		return IsValid;
	}
}
