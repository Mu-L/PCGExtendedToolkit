﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Graph/PCGExCluster.h"
#include "PCGExHeuristicDistance.h"


#include "PCGExHeuristicNodeCount.generated.h"

USTRUCT(BlueprintType)
struct FPCGExHeuristicConfigLeastNodes : public FPCGExHeuristicConfigBase
{
	GENERATED_BODY()

	FPCGExHeuristicConfigLeastNodes() :
		FPCGExHeuristicConfigBase()
	{
	}
};

/**
 * 
 */
class FPCGExHeuristicNodeCount : public FPCGExHeuristicDistance
{
public:
	virtual double GetGlobalScore(
		const PCGExCluster::FNode& From,
		const PCGExCluster::FNode& Seed,
		const PCGExCluster::FNode& Goal) const override;

	virtual double GetEdgeScore(
		const PCGExCluster::FNode& From,
		const PCGExCluster::FNode& To,
		const PCGExGraph::FEdge& Edge,
		const PCGExCluster::FNode& Seed,
		const PCGExCluster::FNode& Goal,
		const TSharedPtr<PCGEx::FHashLookup> TravelStack = nullptr) const override;
};

////

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExHeuristicsFactoryLeastNodes : public UPCGExHeuristicsFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExHeuristicConfigLeastNodes Config;

	virtual TSharedPtr<FPCGExHeuristicOperation> CreateOperation(FPCGExContext* InContext) const override;
	PCGEX_HEURISTIC_FACTORY_BOILERPLATE
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Graph|Params", meta=(PCGExNodeLibraryDoc="TBD"))
class UPCGExHeuristicsLeastNodesProviderSettings : public UPCGExHeuristicsFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(
		HeuristicsLeastNodes, "Heuristics : Least Nodes", "Heuristics based on node count.",
		FName(GetDisplayName()))
#endif
	//~End UPCGSettings

	/** Filter Config.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExHeuristicConfigLeastNodes Config;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
