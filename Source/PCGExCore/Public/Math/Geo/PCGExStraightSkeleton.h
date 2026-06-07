// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

namespace PCGExMath::Geo
{
	/** What an output skeleton edge represents -- used for downstream filtering. */
	enum class ESkeletonEdgeType : uint8
	{
		Contour  = 0, // Original input boundary segment (outer loop or a hole loop).
		Skeleton = 1, // Inner straight-skeleton arc (the trace of a wavefront vertex).
	};

	/** The wavefront event that produced a skeleton arc's terminating node. */
	enum class ESkeletonEventType : uint8
	{
		None       = 0, // Contour edges (no event).
		Initial    = 1, // Arc emanating from an original vertex, ended by an unspecified/peak event.
		EdgeEvent  = 2, // Ended by an edge event (a wavefront edge collapsed).
		SplitEvent = 3, // Ended by a split event (a reflex vertex split a wavefront edge).
	};

	struct PCGEXCORE_API FStraightSkeletonNode
	{
		FVector2D Pos = FVector2D::ZeroVector;
		double Time = 0.0;       // Wavefront time == inward offset distance (0 on the contour).
		bool bIsContour = false; // True for the original input vertices.

		FStraightSkeletonNode() = default;

		FStraightSkeletonNode(const FVector2D& InPos, const double InTime, const bool bInContour)
			: Pos(InPos), Time(InTime), bIsContour(bInContour)
		{
		}
	};

	struct PCGEXCORE_API FStraightSkeletonEdge
	{
		int32 A = -1; // Index into Nodes.
		int32 B = -1; // Index into Nodes.
		ESkeletonEdgeType Type = ESkeletonEdgeType::Skeleton;
		ESkeletonEventType Event = ESkeletonEventType::None;
		bool bPerimeterIncident = false; // At least one endpoint is a contour node.

		FStraightSkeletonEdge() = default;

		FStraightSkeletonEdge(const int32 InA, const int32 InB, const ESkeletonEdgeType InType, const ESkeletonEventType InEvent, const bool bInPerimeterIncident)
			: A(InA), B(InB), Type(InType), Event(InEvent), bPerimeterIncident(bInPerimeterIncident)
		{
		}
	};

	/**
	 * 2D straight-skeleton solver (wavefront propagation, SLAV / Felkel-Obdrzalek style).
	 *
	 * Unlike TDelaunay2 / TVoronoi2 -- which consume unordered point clouds -- the straight
	 * skeleton requires ORDERED polygon loops: an outer boundary wound counter-clockwise plus
	 * zero or more holes wound clockwise (so the solid interior is consistently to the left of
	 * every directed edge). Callers own projection-to-2D and winding normalization.
	 *
	 * This is a floating-point implementation that relies on tolerances and a post-pass weld
	 * rather than exact arithmetic. It targets artist-authored input, not adversarial GIS data;
	 * pathological / self-intersecting input may produce partial results (IsValid stays true,
	 * unresolved features are dropped rather than crashing).
	 */
	class PCGEXCORE_API TStraightSkeleton2
	{
	public:
		// Contour nodes are emitted first -- indices [0, NumContourNodes) -- in input order
		// (outer loop, then each hole loop), followed by the interior skeleton nodes.
		TArray<FStraightSkeletonNode> Nodes;
		TArray<FStraightSkeletonEdge> Edges;
		int32 NumContourNodes = 0;
		bool IsValid = false;

		TStraightSkeleton2() = default;
		~TStraightSkeleton2() = default;

		/**
		 * @param Outer           Outer boundary, wound CCW, WITHOUT a duplicated closing point.
		 * @param Holes           Hole loops, each wound CW, WITHOUT a duplicated closing point.
		 * @param MergeDistance   Weld radius for near-coincident skeleton nodes (0 = weld exact dupes only).
		 * @param bIncludeContour Also emit the original boundary segments as Contour-typed edges.
		 * @return                True if a (possibly partial) skeleton was produced.
		 */
		bool Process(
			const TArrayView<const FVector2D>& Outer,
			const TArrayView<const TArray<FVector2D>>& Holes,
			const double MergeDistance = 0.0,
			const bool bIncludeContour = false);

	protected:
		void Clear();
	};
}
