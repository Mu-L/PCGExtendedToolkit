// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Math/Geo/PCGExStraightSkeleton.h" // shared result structs (FStraightSkeletonNode/Edge, enums) -- PCGExCore

namespace PCGExMath::Geo
{
	/**
	 * Robust straight-skeleton solver built on iterative mitered offsetting (Clipper2).
	 *
	 * Premise: a MITERED inward offset is exactly the straight-skeleton wavefront, so Clipper2's
	 * battle-tested offsetter owns all the hard geometry/topology (corner merges, wavefront splits,
	 * hole->outer merges) and never produces invalid geometry. The solver marches the offset inward and
	 * follows each wavefront corner; only event *timing* is sampled at the marching step.
	 *
	 * IMPORTANT: the join type MUST be JoinType::Miter with a high miter limit. Round/Square joins are NOT
	 * the straight-skeleton wavefront (Round arcs corners into many spurious points; Square bevels them),
	 * which corrupts the corner-tracking.
	 *
	 * Output uses the same FStraightSkeletonNode/FStraightSkeletonEdge structs as the analytic solver, so
	 * the node / cluster build / classification downstream are unchanged.
	 */
	class PCGEXELEMENTSCLIPPER2_API FStraightSkeletonOffset
	{
	public:
		// Contour nodes first (indices [0, NumContourNodes)), then interior skeleton nodes.
		TArray<FStraightSkeletonNode> Nodes;
		TArray<FStraightSkeletonEdge> Edges;
		int32 NumContourNodes = 0;
		bool IsValid = false;

		// Clipper2 works in integer space; floats are scaled by this factor (higher = finer).
		double Precision = 1000.0;
		// Effectively-unlimited miter so corners reach the true bisector intersection (not beveled/squared).
		double MiterLimit = 1.0e6;
		// Offset marching step (world units). Smaller = more accurate event timing, slower. <=0 -> auto.
		double Step = 0.0;

		FStraightSkeletonOffset() = default;
		~FStraightSkeletonOffset() = default;

		/**
		 * @param Outer            Outer boundary, wound CCW, no duplicated closing point.
		 * @param Holes            Hole loops, each wound CW, no duplicated closing point.
		 * @param MergeDistance    Weld radius for near-coincident skeleton nodes.
		 * @param bIncludeContour  Also emit the original boundary segments as Contour edges.
		 */
		bool Process(
			const TArrayView<const FVector2D>& Outer,
			const TArrayView<const TArray<FVector2D>>& Holes,
			const double MergeDistance = 0.0,
			const bool bIncludeContour = false);

		/**
		 * Validation hook: offset the polygon inward by T and return the resulting wavefront loops (each
		 * with a hole flag). Used to verify the Clipper2 plumbing (scaling, miter, winding, inward-delta).
		 */
		static void DebugOffsetAt(
			const TArrayView<const FVector2D>& Outer,
			const TArrayView<const TArray<FVector2D>>& Holes,
			const double T,
			const double Precision,
			const double MiterLimit,
			TArray<TArray<FVector2D>>& OutLoops,
			TArray<bool>& OutIsHole);

	protected:
		void Clear();
	};
}
