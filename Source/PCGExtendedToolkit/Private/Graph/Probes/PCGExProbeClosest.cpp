﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Probes/PCGExProbeClosest.h"


#include "Graph/Probes/PCGExProbing.h"

PCGEX_CREATE_PROBE_FACTORY(Closest, {}, {})

bool FPCGExProbeClosest::PrepareForPoints(const TSharedPtr<PCGExData::FPointIO>& InPointIO)
{
	if (!FPCGExProbeOperation::PrepareForPoints(InPointIO)) { return false; }

	if (Config.MaxConnectionsInput == EPCGExInputValueType::Constant)
	{
		MaxConnections = Config.MaxConnectionsConstant;
	}
	else
	{
		MaxConnectionsCache = PrimaryDataFacade->GetBroadcaster<int32>(Config.MaxConnectionsAttribute, true);

		if (!MaxConnectionsCache)
		{
			PCGEX_LOG_INVALID_SELECTOR_C(Context, "Max Connections", Config.MaxConnectionsAttribute)
			return false;
		}
	}

	CWCoincidenceTolerance = FVector(1 / Config.CoincidencePreventionTolerance);

	return true;
}

void FPCGExProbeClosest::ProcessCandidates(const int32 Index, const FTransform& WorkingTransform, TArray<PCGExProbing::FCandidate>& Candidates, TSet<FInt32Vector>* Coincidence, const FVector& ST, TSet<uint64>* OutEdges)
{
	bool bIsAlreadyConnected;
	const int32 MaxIterations = FMath::Min(MaxConnectionsCache ? MaxConnectionsCache->Read(Index) : MaxConnections, Candidates.Num());
	const double R = GetSearchRadius(Index);

	if (MaxIterations <= 0) { return; }

	TSet<FInt32Vector> LocalCoincidence;
	int32 Additions = 0;

	for (PCGExProbing::FCandidate& C : Candidates)
	{
		if (C.Distance > R) { return; } // Candidates are sorted, stop there.

		if (Coincidence)
		{
			Coincidence->Add(C.GH, &bIsAlreadyConnected);
			if (bIsAlreadyConnected) { continue; }
		}

		if (Config.bPreventCoincidence)
		{
			LocalCoincidence.Add(PCGEx::I323(C.Direction, CWCoincidenceTolerance), &bIsAlreadyConnected);
			if (bIsAlreadyConnected) { continue; }
		}

		OutEdges->Add(PCGEx::H64U(Index, C.PointIndex));

		Additions++;
		if (Additions >= MaxIterations) { return; }
	}
}

void FPCGExProbeClosest::ProcessNode(const int32 Index, const FTransform& WorkingTransform, TSet<FInt32Vector>* Coincidence, const FVector& ST, TSet<uint64>* OutEdges, const TArray<int8>& AcceptConnections)
{
	FPCGExProbeOperation::ProcessNode(Index, WorkingTransform, nullptr, FVector::ZeroVector, OutEdges, AcceptConnections);
}

#if WITH_EDITOR
FString UPCGExProbeClosestProviderSettings::GetDisplayName() const
{
	return TEXT("");
	/*
	return GetDefaultNodeName().ToString()
		+ TEXT(" @ ")
		+ FString::Printf(TEXT("%.3f"), (static_cast<int32>(1000 * Config.WeightFactor) / 1000.0));
		*/
}
#endif
