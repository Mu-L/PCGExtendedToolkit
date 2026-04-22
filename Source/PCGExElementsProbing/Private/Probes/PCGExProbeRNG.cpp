// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Probes/PCGExProbeRNG.h"

#include "Core/PCGExProbingCandidates.h"

PCGEX_CREATE_PROBE_FACTORY(RNG, {}, {})

bool FPCGExProbeRNG::Prepare(FPCGExContext* InContext)
{
	if (!FPCGExProbeOperation::Prepare(InContext)) { return false; }
	HalfBeta = Config.Beta * 0.5;
	return true;
}

void FPCGExProbeRNG::ProcessCandidates(const int32 Index, TArray<PCGExProbing::FCandidate>& Candidates, TSet<uint64>* Coincidence, const FVector& ST, TSet<uint64>* OutEdges, PCGExMT::FScopedContainer* Container)
{
	const double R = GetSearchRadius(Index);
	const TArray<FVector>& Positions = *WorkingPositions;
	const FVector& Pi = Positions[Index];

	for (int32 c = 0; c < Candidates.Num(); ++c)
	{
		const PCGExProbing::FCandidate& Cj = Candidates[c];
		if (Cj.Distance > R) { break; }

		const double Dij = Cj.Distance;
		const double DijSq = Dij * Dij;
		const double RadiusSq = HalfBeta * HalfBeta * DijSq; // (β/2 * d_ij)²

		const FVector& Pj = Positions[Cj.PointIndex];

		// β-skeleton sphere centers: c1 = Pi + HalfBeta*(Pj-Pi), c2 = Pj + HalfBeta*(Pi-Pj)
		// Edge (i,j) is blocked if any k lies inside both spheres.
		// For any β ∈ [1,2], a blocking k must satisfy d_ik < d_ij (proven via triangle inequality
		// on Sphere 2), so we only examine candidates with Ck.Distance < Dij.
		const FVector C1 = Pi + HalfBeta * (Pj - Pi);
		const FVector C2 = Pj + HalfBeta * (Pi - Pj);

		bool bBlocked = false;
		for (int32 k = 0; k < c; ++k)
		{
			const PCGExProbing::FCandidate& Ck = Candidates[k];
			if (Ck.Distance >= Dij) { break; } // Candidates sorted; d_ik ≥ d_ij → cannot block

			const FVector& Pk = Positions[Ck.PointIndex];
			if (FVector::DistSquared(C1, Pk) < RadiusSq && FVector::DistSquared(C2, Pk) < RadiusSq)
			{
				bBlocked = true;
				break;
			}
		}

		if (!bBlocked) { OutEdges->Add(PCGEx::H64U(Index, Cj.PointIndex)); }
	}
}

#if WITH_EDITOR
FString UPCGExProbeRNGProviderSettings::GetDisplayName() const
{
	const double B = Config.Beta;
	if (FMath::IsNearlyEqual(B, 2.0)) { return TEXT("RNG"); }
	if (FMath::IsNearlyEqual(B, 1.0)) { return TEXT("Gabriel"); }
	return FString::Printf(TEXT("β = %.2f"), B);
}
#endif
