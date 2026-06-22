// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExVtxPropertyFactoryProvider.h"
#include "Factories/PCGExFactoryProvider.h"
#include "Sorting/PCGExSortingCommon.h"
#include "Sorting/PCGExSortingDetails.h"
#include "UObject/Object.h"

#include "PCGExVtxPropertySortedNeighbor.generated.h"

///

namespace PCGExSorting
{
	class FSorter;
}

USTRUCT(BlueprintType)
struct FPCGExSortedNeighborConfig
{
	GENERATED_BODY()

	FPCGExSortedNeighborConfig()
	{
		// FAdjacencyData::Direction is stored as (vtx - neighbor), i.e. pointing away from the
		// neighbor. This node is about the direction *to* the selected neighbor, so invert by default.
		SortedNeighbor.bInvertDirection = true;
	}

	/** Which extreme to point toward, as ranked by the plugged sorting rules.
	 *  Descending points toward the highest-ranked neighbor, Ascending toward the lowest-ranked. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExSortDirection SortDirection = EPCGExSortDirection::Descending;

	/** Output for the selected neighbor (direction, length, indices, neighbor count). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, DisplayName="Output"))
	FPCGExEdgeOutputWithIndexSettings SortedNeighbor = FPCGExEdgeOutputWithIndexSettings(TEXT("Sorted"));
};

/**
 *
 */
class FPCGExVtxPropertySortedNeighbor : public FPCGExVtxPropertyOperation
{
public:
	FPCGExSortedNeighborConfig Config;

	TArray<FPCGExSortRuleConfig> SortingRules;

	virtual bool PrepareForCluster(FPCGExContext* InContext, TSharedPtr<PCGExClusters::FCluster> InCluster, const TSharedPtr<PCGExData::FFacade>& InVtxDataFacade, const TSharedPtr<PCGExData::FFacade>& InEdgeDataFacade) override;
	virtual void ProcessNode(PCGExClusters::FNode& Node, const TArray<PCGExClusters::FAdjacencyData>& Adjacency, const PCGExMath::FBestFitPlane& BFP) override;

protected:
	TSharedPtr<PCGExSorting::FSorter> Sorter;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExVtxPropertySortedNeighborFactory : public UPCGExVtxPropertyFactoryData
{
	GENERATED_BODY()

public:
	FPCGExSortedNeighborConfig Config;

	UPROPERTY()
	TArray<FPCGExSortRuleConfig> SortingRules;

	virtual TSharedPtr<FPCGExVtxPropertyOperation> CreateOperation(FPCGExContext* InContext) const override;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|VtxProperty", meta=(PCGExNodeLibraryDoc="clusters/analyze/cluster-vtx-properties/vtx-sorted-neighbor"))
class UPCGExVtxPropertySortedNeighborSettings : public UPCGExVtxPropertyProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(VtxSortedNeighbor, "Vtx : Sorted Neighbor", "Sort connected neighbors using sorting rules and output the direction to the extreme one.", FName(GetDisplayName()))
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	//~End UPCGSettings

public:
	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif

	/** Sorted Neighbor Settings. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExSortedNeighborConfig Config;
};
