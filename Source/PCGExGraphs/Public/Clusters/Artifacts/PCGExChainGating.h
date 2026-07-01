// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExChainGating.generated.h"

namespace PCGExClusters
{
	class FCluster;
	class FNodeChain;
}

UENUM()
enum class EPCGExChainGatingLogic : uint8
{
	All = 0 UMETA(DisplayName = "All (AND)", ToolTip="A chain matches only if every enabled criterion is satisfied."),
	Any = 1 UMETA(DisplayName = "Any (OR)", ToolTip="A chain matches if at least one enabled criterion is satisfied."),
};

namespace PCGExClusters
{
	/** Computes the vtx count, edge count and summed length of a chain in a single walk. */
	PCGEXGRAPHS_API void ComputeChainMetrics(
		const FNodeChain& Chain,
		const FCluster& Cluster,
		int32& OutVtxCount,
		int32& OutEdgeCount,
		double& OutLength);
}

/**
 * Reusable gating criteria for node chains.
 * Selects chains based on vtx count, edge count and/or length, combined with an AND/OR logic.
 * Designed to be embedded by any chain-consuming operation (refinements, simplification, ...).
 */
USTRUCT(BlueprintType)
struct PCGEXGRAPHS_API FPCGExChainGatingDetails
{
	GENERATED_BODY()

	FPCGExChainGatingDetails() = default;

	/** How enabled criteria are combined to decide whether a chain matches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	EPCGExChainGatingLogic Logic = EPCGExChainGatingLogic::All;

	/** Gate on a lower bound for the vertex (node) count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(InlineEditConditionToggle))
	bool bCheckMinVtxCount = false;

	/** Inclusive lower bound for the vertex count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(DisplayName=" ┌─ Min Vtx Count", EditCondition="bCheckMinVtxCount", ClampMin=0))
	int32 MinVtxCount = 0;

	/** Gate on an upper bound for the vertex (node) count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(InlineEditConditionToggle))
	bool bCheckMaxVtxCount = false;

	/** Inclusive upper bound for the vertex count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(DisplayName=" └─ Max Vtx Count", EditCondition="bCheckMaxVtxCount", ClampMin=0))
	int32 MaxVtxCount = 100;

	/** Gate on a lower bound for the edge count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(InlineEditConditionToggle))
	bool bCheckMinEdgeCount = false;

	/** Inclusive lower bound for the edge count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(DisplayName=" ┌─ Min Edge Count", EditCondition="bCheckMinEdgeCount", ClampMin=0))
	int32 MinEdgeCount = 0;

	/** Gate on an upper bound for the edge count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(InlineEditConditionToggle))
	bool bCheckMaxEdgeCount = false;

	/** Inclusive upper bound for the edge count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(DisplayName=" └─ Max Edge Count", EditCondition="bCheckMaxEdgeCount", ClampMin=0))
	int32 MaxEdgeCount = 100;

	/** Gate on a lower bound for the total chain length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(InlineEditConditionToggle))
	bool bCheckMinLength = false;

	/** Inclusive lower bound for the chain length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(DisplayName=" ┌─ Min Length", EditCondition="bCheckMinLength", ClampMin=0))
	double MinLength = 0;

	/** Gate on an upper bound for the total chain length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(InlineEditConditionToggle))
	bool bCheckMaxLength = false;

	/** Inclusive upper bound for the chain length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta=(DisplayName=" └─ Max Length", EditCondition="bCheckMaxLength", ClampMin=0))
	double MaxLength = 1000;

	/** Whether any criterion is enabled at all. */
	bool IsEnabled() const;

	/** Returns true if the chain matches the enabled criteria under the current logic. Returns false when no criterion is enabled. */
	bool Test(const PCGExClusters::FNodeChain& Chain, const PCGExClusters::FCluster& Cluster) const;
};
