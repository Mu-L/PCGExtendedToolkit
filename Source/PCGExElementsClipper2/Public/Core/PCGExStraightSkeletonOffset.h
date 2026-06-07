// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Math/Geo/PCGExStraightSkeleton.h" // result structs (FStraightSkeletonNode/Edge, enums) -- PCGExCore

namespace PCGExMath::Geo
{
	/**
	 * Robust straight-skeleton solver built on iterative mitered offsetting (Clipper2).
	 *
	 * Instead of computing wavefront events analytically (the fragile float approach in TStraightSkeleton2),
	 * this traces the skeleton by repeatedly shrinking the polygon inward with Clipper2's battle-tested
	 * mitered offset and following the corners. Clipper2 never produces invalid geometry and natively handles
	 * the hard topology -- corner merges, the wavefront splitting in two, holes merging into the outer
	 * boundary -- so the solver "always succeeds". A mitered offset is the exact wavefront, so the traced
	 * arcs are accurate straight segments; only event *timing* is sampled (and can be refined by bisection).
	 *
	 * Output uses the same FStraightSkeletonNode/FStraightSkeletonEdge structs as the analytic solver, so the
	 * node / cluster build / classification downstream are unchanged.
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
		// Effectively-unlimited miter so corners reach the true bisector intersection (not beveled).
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
		 * R1 scaffold / validation hook: offset the polygon inward by T and return the resulting wavefront
		 * loops (each with a hole flag). Used to verify the Clipper2 plumbing (scaling, miter, winding,
		 * inward-delta sign) before the corner-tracking extraction is built on top.
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
