﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExHeuristicsFactoryProvider.h"
#include "UObject/Object.h"
#include "PCGExHeuristicOperation.h"


#include "Graph/PCGExCluster.h"
#include "PCGExHeuristicInertia.generated.h"

USTRUCT(BlueprintType)
struct FPCGExHeuristicConfigInertia : public FPCGExHeuristicConfigBase
{
	GENERATED_BODY()

	FPCGExHeuristicConfigInertia() :
		FPCGExHeuristicConfigBase()
	{
	}

	/** How many previous edges should be averaged to compute the inertia. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ClampMin=1))
	int32 Samples = 1;

	/** If enabled, use fallback score if there is less samples than the specified number. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ClampMin=1))
	bool bIgnoreIfNotEnoughSamples = true;

	/** Value used for global score. Primarily used by A* Star to do initial sorting. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Fallbacks", meta=(PCG_Overridable, DisplayPriority=-1, ClampMin=0, ClampMax=1))
	double GlobalInertiaScore = 0;

	/** Fallback heuristic score for when no inertia value can be computed (no previous node). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Fallbacks", meta=(PCG_Overridable, DisplayPriority=-1, ClampMin=0, ClampMax=1))
	double FallbackInertiaScore = 0;
};

/**
 * 
 */
class FPCGExHeuristicInertia : public FPCGExHeuristicOperation
{
public:
	double GlobalInertiaScore = 0;
	double FallbackInertiaScore = 0;
	int32 MaxSamples = 1;
	bool bIgnoreIfNotEnoughSamples = true;

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
		const TSharedPtr<PCGEx::FHashLookup> TravelStack) const override;
};

////

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExHeuristicsFactoryInertia : public UPCGExHeuristicsFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExHeuristicConfigInertia Config;

	virtual TSharedPtr<FPCGExHeuristicOperation> CreateOperation(FPCGExContext* InContext) const override;
	PCGEX_HEURISTIC_FACTORY_BOILERPLATE
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Graph|Params", meta=(PCGExNodeLibraryDoc="pathfinding/heuristics/hx-inertia"))
class UPCGExHeuristicsInertiaProviderSettings : public UPCGExHeuristicsFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(
		HeuristicsInertia, "Heuristics : Inertia", "Heuristics based on direction inertia from last visited node. NOTE: Can be quite expensive.",
		FName(GetDisplayName()))
#endif
	//~End UPCGSettings

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

	/** Filter Config.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExHeuristicConfigInertia Config;


#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
