// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Domains/PCGExSpatialDomain_Polygon2D.h"

#include "Math/Geo/PCGExGeo.h"
#include "Paths/PCGExPolyPath.h"

FPCGExSpatialDomain_Polygon2D FPCGExSpatialDomain_Polygon2D::MakeFromOutline(
	TArray<FVector2D> InOutline,
	float InZMin,
	float InZMax,
	const FQuat& InProjectionQuat)
{
	FPCGExSpatialDomain_Polygon2D Out;
	Out.Outline = MoveTemp(InOutline);
	Out.ProjectionQuat = InProjectionQuat;
	// callers may pass Z bounds in arbitrary order; normalize so ZMax >= ZMin
	Out.ZMin = FMath::Min(InZMin, InZMax);
	Out.ZMax = FMath::Max(InZMin, InZMax);
	Out.RecomputeBounds();
	return Out;
}

FPCGExSpatialDomain_Polygon2D FPCGExSpatialDomain_Polygon2D::MakeFromFPolyPath(
	const PCGExPaths::FPolyPath& Path,
	float InZMin,
	float InZMax)
{
	// Lift the projection frame from the path itself: FPolyPath may have
	// fallen back to FBestFitPlane during construction, so the final quat
	// can differ from whatever the caller passed at construction time.
	// Reading it from FPath::GetProjection() guarantees we use the actual
	// frame the projected points are expressed in.
	return MakeFromOutline(
		Path.GetProjectedPoints(),
		InZMin,
		InZMax,
		Path.GetProjection().ProjectionQuat);
}

float FPCGExSpatialDomain_Polygon2D::QueryPoint(const FVector& Point) const
{
	// Rotate world point into projection-frame local space. Outline is
	// authored in projection-frame XY; the height band is along projection-
	// frame Z. With Identity quat (default) UnrotateVector is a no-op and
	// behavior collapses to the world-XY case.
	const FVector LocalP = ProjectionQuat.UnrotateVector(Point);
	const FVector2D P2D(LocalP.X, LocalP.Y);
	const int32 N = Outline.Num();

	// Single pass: closest-edge distance + winding-number inside test.
	// See https://iquilezles.org/articles/distfunctions/ "extrusion"
	float MinDistSq = TNumericLimits<float>::Max();
	int32 Winding = 0;

	for (int32 i = 0; i < N; ++i)
	{
		const FVector2D& A = Outline[i];
		const FVector2D& B = Outline[(i + 1) % N];
		MinDistSq = FMath::Min(MinDistSq, PCGExMath::Geo::DistancePointToSegmentSquared2D(P2D, A, B));
		const float Cross = (B.X - A.X) * (P2D.Y - A.Y) - (B.Y - A.Y) * (P2D.X - A.X);
		if (A.Y <= P2D.Y) { if (B.Y > P2D.Y  && Cross > 0.0f) ++Winding; }
		else              { if (B.Y <= P2D.Y  && Cross < 0.0f) --Winding; }
	}

	const float EdgeDist = FMath::Sqrt(MinDistSq);
	const float D2D = (Winding != 0) ? -EdgeDist : EdgeDist;
	const float DZ = FMath::Max(ZMin - LocalP.Z, LocalP.Z - ZMax);

	if (D2D > 0.0f && DZ > 0.0f) return FMath::Sqrt(MinDistSq + DZ * DZ);
	if (D2D > 0.0f) return D2D;
	if (DZ > 0.0f) return DZ;
	return FMath::Max(D2D, DZ);
}

int32 FPCGExSpatialDomain_Polygon2D::Append(const FPCGExFootprintShape& Shape, int32 OwnerIndex, uint32 ChannelMask)
{
	checkf(false, TEXT("FPCGExSpatialDomain_Polygon2D is immutable; Append() is not supported."));
	return INDEX_NONE;
}

void FPCGExSpatialDomain_Polygon2D::RecomputeBounds()
{
	Bounds2D = FBox2D(ForceInit);
	for (const FVector2D& V : Outline) { Bounds2D += V; }

	WorldBounds = PCGExMath::Geo::ProjectPrismToWorldAABB(
		Outline, ZMin, ZMax, FVector::ZeroVector, ProjectionQuat);
}
