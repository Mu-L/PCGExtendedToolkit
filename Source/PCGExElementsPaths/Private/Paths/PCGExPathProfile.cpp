// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Paths/PCGExPathProfile.h"

#include "Details/PCGExSubdivisionDetails.h"
#include "Helpers/PCGExArrayHelpers.h"
#include "Math/Geo/PCGExGeo.h"
#include "Math/RotationMatrix.h"

namespace PCGExPaths::Profile
{
	void SubdivideLine(TArray<FVector>& Out, const FVector& A, const FVector& B, const double Factor, const bool bIsCount)
	{
		const double Dist = FVector::Dist(A, B);

		int32 SubdivCount = static_cast<int32>(Factor);
		double StepSize = 0;

		if (bIsCount)
		{
			StepSize = Dist / static_cast<double>(SubdivCount + 1);
		}
		else
		{
			// Attribute-driven amounts bypass the property clamp, guard against zero & negative values
			StepSize = FMath::Min(Dist, Factor);
			SubdivCount = Factor > KINDA_SMALL_NUMBER ? FMath::Floor(Dist / Factor) : 0;
		}

		SubdivCount = FMath::Max(0, SubdivCount);

		PCGExArrayHelpers::InitArray(Out, SubdivCount);
		const FVector Dir = (B - A).GetSafeNormal();
		for (int i = 0; i < SubdivCount; i++)
		{
			Out[i] = A + Dir * (StepSize + i * StepSize);
		}
	}

	void SubdivideLineKeepCorner(TArray<FVector>& Out, const FVector& A, const FVector& Corner, const FVector& B, const double Factor, const bool bIsCount)
	{
		const double Dist = FVector::Dist(A, Corner);

		int32 SubdivCount = static_cast<int32>(Factor);
		double StepSize = 0;

		if (bIsCount)
		{
			StepSize = Dist / static_cast<double>(SubdivCount + 1);
		}
		else
		{
			// Attribute-driven amounts bypass the property clamp, guard against zero & negative values
			StepSize = FMath::Min(Dist, Factor);
			SubdivCount = Factor > KINDA_SMALL_NUMBER ? FMath::Floor(Dist / Factor) : 0;
		}

		SubdivCount = FMath::Max(0, SubdivCount);

		PCGExArrayHelpers::InitArray(Out, SubdivCount * 2 + 1);

		if (SubdivCount == 0)
		{
			Out[0] = Corner;
		}
		else
		{
			int32 WriteIndex = 0;
			FVector Dir = (Corner - A).GetSafeNormal();
			for (int i = 0; i < SubdivCount; i++)
			{
				Out[WriteIndex++] = A + Dir * (StepSize + i * StepSize);
			}

			Out[WriteIndex++] = Corner;

			Dir = (B - Corner).GetSafeNormal();
			for (int i = 0; i < SubdivCount; i++)
			{
				Out[WriteIndex++] = Corner + Dir * (StepSize + i * StepSize);
			}
		}
	}

	void SubdivideArc(TArray<FVector>& Out, const PCGExMath::Geo::FExCenterArc& Arc, const FVector& A, const FVector& B, const double Factor, const bool bIsCount)
	{
		if (Arc.bIsLine)
		{
			// Fallback to line since we can't infer a proper radius
			SubdivideLine(Out, A, B, Factor, bIsCount);
			return;
		}

		// Attribute-driven amounts bypass the property clamp, guard against zero & negative values
		int32 SubdivCount = bIsCount ? static_cast<int32>(Factor) : 0;
		if (!bIsCount && Factor > KINDA_SMALL_NUMBER)
		{
			SubdivCount = FMath::Floor(Arc.GetLength() / Factor);
		}
		SubdivCount = FMath::Max(0, SubdivCount);

		const double StepSize = 1.0 / static_cast<double>(SubdivCount + 1);
		PCGExArrayHelpers::InitArray(Out, SubdivCount);

		for (int i = 0; i < SubdivCount; i++)
		{
			Out[i] = Arc.GetLocationOnArc(StepSize + i * StepSize);
		}
	}

	void SubdivideCustom(TArray<FVector>& Out, const TArray<FVector>& ProfilePositions, const FVector& A, const FVector& B, const FVector& PlaneNormal, const double MainAxisSize, const double CrossAxisSize)
	{
		const int32 SubdivCount = ProfilePositions.Num() - 2;

		PCGExArrayHelpers::InitArray(Out, SubdivCount);

		if (SubdivCount == 0)
		{
			return;
		}

		const double ProfileSize = FVector::Dist(B, A);
		const FVector ProjectionNormal = (B - A).GetSafeNormal(1E-08, FVector::ForwardVector);
		const FQuat ProjectionQuat = FRotationMatrix::MakeFromZX(PlaneNormal, ProjectionNormal).ToQuat();

		for (int i = 0; i < SubdivCount; i++)
		{
			FVector Pos = ProfilePositions[i + 1];
			Pos.X *= ProfileSize;
			Pos.Y *= MainAxisSize;
			Pos.Z *= CrossAxisSize;
			Out[i] = A + ProjectionQuat.RotateVector(Pos);
		}
	}

	void SubdivideManhattan(TArray<FVector>& Out, const FPCGExManhattanDetails& Details, const int32 Index, const FVector& A, const FVector& B, const FVector* Corner)
	{
		Out.Reset();
		double OutDist = 0;

		if (Corner)
		{
			Details.ComputeSubdivisions(A, *Corner, Index, Out, OutDist);
			Out.Emplace(*Corner);
			Details.ComputeSubdivisions(*Corner, B, Index, Out, OutDist);
		}
		else
		{
			Details.ComputeSubdivisions(A, B, Index, Out, OutDist);
		}
	}
}
