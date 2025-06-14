﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "PCGExFactoryProvider.h"
#include "PCGExVtxPropertyFactoryProvider.h"


#include "Graph/PCGExCluster.h"
#include "Graph/PCGExGraph.h"

#include "PCGExVtxPropertySpecialNeighbors.generated.h"

USTRUCT(BlueprintType)
struct FPCGExSpecialNeighborsConfig
{
	GENERATED_BODY()

	/** Shortest edge. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FPCGExEdgeOutputWithIndexSettings LargestNeighbor = FPCGExEdgeOutputWithIndexSettings(TEXT("Largest"));

	/** Longest edge. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FPCGExEdgeOutputWithIndexSettings SmallestNeighbor = FPCGExEdgeOutputWithIndexSettings(TEXT("Smallest"));
};

/**
 * 
 */
class FPCGExVtxPropertySpecialNeighbors : public FPCGExVtxPropertyOperation
{
public:
	FPCGExSpecialNeighborsConfig Config;

	virtual bool PrepareForCluster(
		const FPCGExContext* InContext,
		TSharedPtr<PCGExCluster::FCluster> InCluster,
		const TSharedPtr<PCGExData::FFacade>& InVtxDataFacade,
		const TSharedPtr<PCGExData::FFacade>& InEdgeDataFacade) override;
	virtual void ProcessNode(PCGExCluster::FNode& Node, const TArray<PCGExCluster::FAdjacencyData>& Adjacency) override;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExVtxPropertySpecialNeighborsFactory : public UPCGExVtxPropertyFactoryData
{
	GENERATED_BODY()

public:
	FPCGExSpecialNeighborsConfig Config;
	virtual TSharedPtr<FPCGExVtxPropertyOperation> CreateOperation(FPCGExContext* InContext) const override;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|VtxProperty", meta=(PCGExNodeLibraryDoc="clusters/metadata/vtx-properties/vtx-special-neighbors"))
class UPCGExVtxPropertySpecialNeighborsSettings : public UPCGExVtxPropertyProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(VtxSpecialNeighbors, "Vtx : Special Neighbors", "Fetch data from neighbors")
#endif
	//~End UPCGSettings

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif

	/** Direction Settings. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExSpecialNeighborsConfig Config;
};
