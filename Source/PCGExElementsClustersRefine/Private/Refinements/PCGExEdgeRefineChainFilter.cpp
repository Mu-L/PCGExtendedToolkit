// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Refinements/PCGExEdgeRefineChainFilter.h"

#include "Clusters/PCGExCluster.h"
#include "Clusters/Artifacts/PCGExChain.h"
#include "Clusters/Artifacts/PCGExCachedChain.h"

#pragma region FPCGExEdgeRefineChainFilter

void FPCGExEdgeRefineChainFilter::Process()
{
	// Build (or reuse cached) topological chains. No breakpoints: we want the base cluster topology.
	TArray<TSharedPtr<PCGExClusters::FNodeChain>> Chains;
	if (!PCGExClusters::ChainHelpers::GetOrBuildChains(Cluster.ToSharedRef(), Chains, nullptr, false))
	{
		return;
	}

	// The edge filter cache defaults to all-true when the (optional) filter pin is disconnected,
	// so the override must only kick in when filters are actually present.
	const bool bUseFilters = bHasEdgeFilters && EdgeFilterCache;

	for (const TSharedPtr<PCGExClusters::FNodeChain>& Chain : Chains)
	{
		if (!Chain)
		{
			continue;
		}

		// bInvert only flips an actual gating verdict; with no criteria enabled the node stays inert
		// (the optional filter override below can still act).
		const bool bMatch = Gating.Test(*Chain, *Cluster);
		bool bRemove = Gating.IsEnabled() ? (bInvert ? !bMatch : bMatch) : false;

		if (bUseFilters)
		{
			bool bAnyPass = false;
			bool bAllPass = true;

			auto AccumulateFilter = [&](const int32 EdgeIndex)
			{
				const bool bPass = static_cast<bool>((*EdgeFilterCache)[EdgeIndex]);
				bAnyPass |= bPass;
				bAllPass &= bPass;
			};

			for (const PCGExGraphs::FLink& Lk : Chain->Links) { AccumulateFilter(Lk.Edge); }
			if (Chain->bIsClosedLoop) { AccumulateFilter(Chain->Seed.Edge); }

			if (bRequireAllEdgesPass ? bAllPass : bAnyPass)
			{
				bRemove = (FilterOverride == EPCGExChainFilterOverride::ForceRemove);
			}
		}

		// Process() runs single-threaded per cluster (invoked from the edge-loop completion callback),
		// so a plain write is sufficient here.
		const int8 Validity = bRemove ? 0 : 1;
		for (const PCGExGraphs::FLink& Lk : Chain->Links) { Cluster->GetEdge(Lk.Edge)->bValid = Validity; }
		if (Chain->bIsClosedLoop) { Cluster->GetEdge(Chain->Seed.Edge)->bValid = Validity; }
	}
}

#pragma endregion

#pragma region UPCGExEdgeRefineChainFilter

void UPCGExEdgeRefineChainFilter::CopySettingsFrom(const UPCGExInstancedFactory* Other)
{
	Super::CopySettingsFrom(Other);
	if (const UPCGExEdgeRefineChainFilter* TypedOther = Cast<UPCGExEdgeRefineChainFilter>(Other))
	{
		Gating = TypedOther->Gating;
		bInvert = TypedOther->bInvert;
		FilterOverride = TypedOther->FilterOverride;
		bRequireAllEdgesPass = TypedOther->bRequireAllEdgesPass;
	}
}

#pragma endregion
