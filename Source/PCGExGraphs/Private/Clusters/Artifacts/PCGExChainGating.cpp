// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Clusters/Artifacts/PCGExChainGating.h"

#include "Clusters/PCGExCluster.h"
#include "Clusters/Artifacts/PCGExChain.h"
#include "Paths/PCGExPathsCommon.h"

namespace PCGExClusters
{
	void ComputeChainMetrics(
		const FNodeChain& Chain,
		const FCluster& Cluster,
		int32& OutVtxCount,
		int32& OutEdgeCount,
		double& OutLength)
	{
		// Chain nodes are Seed followed by every Links[i].Node (this holds for single-edge, open and
		// closed chains alike). A closed loop additionally closes back onto the seed.
		OutVtxCount = Chain.Links.Num() + 1;
		OutEdgeCount = Chain.bIsClosedLoop ? OutVtxCount : OutVtxCount - 1;

		PCGExPaths::FPathMetrics Metrics(Cluster.GetPos(Chain.Seed.Node));
		for (const PCGExGraphs::FLink& Lk : Chain.Links) { Metrics.Add(Cluster.GetPos(Lk.Node)); }
		if (Chain.bIsClosedLoop) { Metrics.Add(Cluster.GetPos(Chain.Seed.Node)); }

		OutLength = FMath::Max(0.0, Metrics.Length);
	}
}

#pragma region FPCGExChainGatingDetails

bool FPCGExChainGatingDetails::IsEnabled() const
{
	return bCheckMinVtxCount || bCheckMaxVtxCount ||
		bCheckMinEdgeCount || bCheckMaxEdgeCount ||
		bCheckMinLength || bCheckMaxLength;
}

bool FPCGExChainGatingDetails::Test(const PCGExClusters::FNodeChain& Chain, const PCGExClusters::FCluster& Cluster) const
{
	if (!IsEnabled()) { return false; }

	int32 VtxCount = 0;
	int32 EdgeCount = 0;
	double Length = 0;
	PCGExClusters::ComputeChainMetrics(Chain, Cluster, VtxCount, EdgeCount, Length);

	const bool bAndLogic = (Logic == EPCGExChainGatingLogic::All);

	// AND starts true and clears on any failing criterion; OR starts false and sets on any passing one.
	// IsEnabled() guarantees at least one criterion contributes, so the seed value is always overwritten.
	bool bResult = bAndLogic;

	auto Apply = [&](const bool bPass)
	{
		if (bAndLogic) { bResult &= bPass; }
		else { bResult |= bPass; }
	};

	if (bCheckMinVtxCount) { Apply(VtxCount >= MinVtxCount); }
	if (bCheckMaxVtxCount) { Apply(VtxCount <= MaxVtxCount); }
	if (bCheckMinEdgeCount) { Apply(EdgeCount >= MinEdgeCount); }
	if (bCheckMaxEdgeCount) { Apply(EdgeCount <= MaxEdgeCount); }
	if (bCheckMinLength) { Apply(Length >= MinLength); }
	if (bCheckMaxLength) { Apply(Length <= MaxLength); }

	return bResult;
}

#pragma endregion
