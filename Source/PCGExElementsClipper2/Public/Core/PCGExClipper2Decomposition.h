// Copyright 2026 Timothe Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Clipper2Lib/clipper.h"
#include "Core/PCGExClipper2Processor.h"

// Shared geometry pipeline behind the Clipper2 *geometry* nodes (Clipper2 : Volume and
// Clipper2 : Decompose). Both nodes need the identical "projected closed paths -> boundary-respecting
// triangulation -> deduplicated vertex pool -> Hertel-Mehlhorn convex decomposition" sequence; this
// header exposes it once so neither node duplicates the geometry code.
namespace PCGExClipper2Decomposition
{
	/**
	 * A single deduplicated 2D footprint vertex, in projection space.
	 *
	 * Source-backed vertices map back to a concrete input point via (SourceIdx, SourcePointIdx),
	 * indexing PCGExClipper2::FOpData. Intersection / Steiner vertices created by Clipper2 carry no
	 * source (bHasSource == false); consumers position those by unprojecting Pos with ProjectedZ.
	 */
	struct FFootprintVertex
	{
		// 2D position in projection space (Clipper2 int coords scaled back by 1/Precision).
		FVector2D Pos = FVector2D::ZeroVector;

		// Projection-space Z of the mapped source point (0 when there is no source).
		double ProjectedZ = 0;

		// FOpData facade index of the mapped source, or INDEX_NONE.
		int32 SourceIdx = INDEX_NONE;

		// Point index within the mapped source, or INDEX_NONE.
		int32 SourcePointIdx = INDEX_NONE;

		// True only when this vertex maps back to a valid input point.
		bool bHasSource = false;
	};

	/** Outcome of a decomposition pass. */
	enum class EDecomposeResult : uint8
	{
		// Decomposition produced usable convex pieces within the cap.
		Success,
		// Clipper2 triangulation returned a non-success code or produced no triangles.
		TriangulationFailed,
		// Triangulation succeeded but no usable pieces / vertices remained after dedup.
		Empty,
		// Piece count exceeded MaxConvexPieces (Pieces is still populated, for reporting the count).
		TooManyPieces,
	};

	/** Inputs that select how the footprint is triangulated and merged. */
	struct FDecomposeParams
	{
		// Decimal precision used to scale Clipper2 int coordinates back to float (mirrors the node setting).
		int32 Precision = 100;

		// Fill rule for the triangulation (Even-Odd treats nested rings as holes).
		EPCGExClipper2FillRule FillRule = EPCGExClipper2FillRule::EvenOdd;

		// Use Delaunay refinement during triangulation.
		bool bUseDelaunay = true;

		// Greedily merge triangles into larger convex pieces (Hertel-Mehlhorn). When false, pieces stay triangles.
		bool bMergeConvexPieces = true;

		// Safety cap on convex pieces; exceeding it yields EDecomposeResult::TooManyPieces.
		int32 MaxConvexPieces = 256;
	};

	/** Result of a decomposition pass: the vertex pool, the convex piece loops, and a status. */
	struct FDecomposeResult
	{
		// Deduplicated vertex pool shared by every piece.
		TArray<FFootprintVertex> VertexPool;

		// Convex piece loops, CCW, each entry an index into VertexPool.
		TArray<TArray<int32>> Pieces;

		// Outcome; only Success guarantees Pieces/VertexPool are ready for downstream use.
		EDecomposeResult Status = EDecomposeResult::Empty;
	};

	/**
	 * Run the shared decomposition over one processing group's projected closed paths.
	 *
	 * The paths must already be projected into Clipper2 int space (as FOpData stores them) with each
	 * Point64.z encoding (PointIndex, SourceIndex) via PCGEx::H64 -- exactly what BuildDataFromCollection
	 * produces. This does NOT log; callers inspect FDecomposeResult::Status and emit their own warnings.
	 *
	 * @param SubjectPaths Closed, projected paths for the group (e.g. FProcessingGroup::SubjectPaths).
	 * @param AllOpData    Source registry, used to validate source indices and read projected Z.
	 * @param ZCallback    The group's Z-callback (preserves source encoding through Clipper2's internal union).
	 * @param Params       Precision / fill rule / delaunay / merge / cap.
	 */
	PCGEXELEMENTSCLIPPER2_API FDecomposeResult Decompose(
		const PCGExClipper2Lib::Paths64& SubjectPaths,
		const TSharedPtr<PCGExClipper2::FOpData>& AllOpData,
		const PCGExClipper2Lib::ZCallback64& ZCallback,
		const FDecomposeParams& Params);

	/**
	 * Per-group orchestration shared by the geometry nodes (Volume, Decompose): validate the group, resolve
	 * its frame subject (SubjectIndices[0], guaranteed valid in AllOpData on success), and run Decompose().
	 *
	 * Returns true only when the group yielded usable convex pieces (OutResult.Status == Success, with
	 * OutResult.VertexPool / .Pieces ready). On false, OutResult.Status reports why so the caller can emit
	 * its own (node-specific) warning: Empty for an invalid/degenerate group, or TriangulationFailed /
	 * TooManyPieces. Intentionally does NOT log -- warning wording is left to each node.
	 */
	PCGEXELEMENTSCLIPPER2_API bool TryDecomposeGroup(
		const TSharedPtr<PCGExClipper2::FProcessingGroup>& Group,
		const TSharedPtr<PCGExClipper2::FOpData>& AllOpData,
		const FDecomposeParams& Params,
		FDecomposeResult& OutResult);
}
