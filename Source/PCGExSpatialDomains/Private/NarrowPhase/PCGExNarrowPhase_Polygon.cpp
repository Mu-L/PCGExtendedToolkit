// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "NarrowPhase/PCGExNarrowPhaseRegistrations.h"
#include "NarrowPhase/PCGExNarrowPhase.h"
#include "Shapes/PCGExFootprintShape.h"
#include "Math/OBB/PCGExOBB.h"
#include "Math/Geo/PCGExGeo.h"

namespace PCGExSpatial::NarrowPhase
{
	namespace
	{
		/**
		 * OBB-vs-Polygon precise overlap test. Lifted from the per-domain
		 * inner loop in FSpatialDomain_OBBList::OverlapsPolygon: project the
		 * OBB shadow into the polygon's projection frame, reject on Z band,
		 * then run 2D polygon-vs-shadow overlap (concave-safe).
		 *
		 * The world-AABB pre-cull lives at the broadphase tier, not here --
		 * by the time this is called, the broadphase has already accepted
		 * the pair as AABB-overlapping.
		 */
		bool OBBvsPolygon_Overlap(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B)
		{
			const auto& OBB     = static_cast<const FPCGExFootprintShape_OBB&>(A);
			const auto& Polygon = static_cast<const FPCGExFootprintShape_Polygon&>(B);
			const FPCGExSpatialPolygonEntry& Entry = Polygon.Entry;

			TArray<FVector2D, TInlineAllocator<8>> Shadow;
			float ShadowZMin, ShadowZMax;
			PCGExMath::Geo::ProjectOBBToFrame(
				OBB.Bounds, Entry.WorldOrigin, Entry.ProjectionQuat,
				Shadow, ShadowZMin, ShadowZMax);

			if (ShadowZMax < Entry.ZMin || ShadowZMin > Entry.ZMax) { return false; }
			return PCGExMath::Geo::PolygonsOverlap2D(Entry.Outline, Shadow);
		}

		/**
		 * Polygon-vs-Polygon precise overlap test. Lifted from the per-domain
		 * inner loop in FSpatialDomain_PolygonList::OverlapsPolygon: project
		 * candidate prism into stored polygon's frame, reject on Z band, run
		 * 2D concave-vs-concave overlap.
		 */
		bool PolygonVsPolygon_Overlap(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B)
		{
			const auto& PolyA = static_cast<const FPCGExFootprintShape_Polygon&>(A);
			const auto& PolyB = static_cast<const FPCGExFootprintShape_Polygon&>(B);
			const FPCGExSpatialPolygonEntry& Candidate = PolyA.Entry;
			const FPCGExSpatialPolygonEntry& Stored    = PolyB.Entry;

			TArray<FVector2D> CandidateInStored;
			float CandidateZMin, CandidateZMax;
			PCGExMath::Geo::ProjectPrismToFrame(
				Candidate.Outline, Candidate.ZMin, Candidate.ZMax,
				Candidate.WorldOrigin, Candidate.ProjectionQuat,
				Stored.WorldOrigin, Stored.ProjectionQuat,
				CandidateInStored, CandidateZMin, CandidateZMax);

			if (CandidateZMax < Stored.ZMin || CandidateZMin > Stored.ZMax) { return false; }
			return PCGExMath::Geo::PolygonsOverlap2D(Stored.Outline, CandidateInStored);
		}
	}

	void RegisterPolygonPairTests()
	{
		// OBB-vs-Polygon. Stored under the canonical (lower-pointer-first)
		// key by the registry; lookup handles the swap automatically.
		Register(
			FPCGExFootprintShape_OBB::StaticStruct(),
			FPCGExFootprintShape_Polygon::StaticStruct(),
			{ &OBBvsPolygon_Overlap, /*Penetration*/ nullptr });

		// Polygon-vs-Polygon.
		Register(
			FPCGExFootprintShape_Polygon::StaticStruct(),
			FPCGExFootprintShape_Polygon::StaticStruct(),
			{ &PolygonVsPolygon_Overlap, /*Penetration*/ nullptr });
	}
}
