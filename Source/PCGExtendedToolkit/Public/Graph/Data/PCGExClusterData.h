﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Data/PCGExPointData.h"
#include "Data/PCGExPointIO.h"
#include "Graph/PCGExCluster.h"

#include "PCGExClusterData.generated.h"

namespace PCGExData
{
	enum class EIOInit : uint8;
}

namespace PCGExCluster
{
	class FCluster;
}

/**
 * 
 */
UCLASS(Abstract)
class PCGEXTENDEDTOOLKIT_API UPCGExClusterData : public UPCGExPointData
{
	GENERATED_BODY()
};

/**
 * 
 */
UCLASS()
class PCGEXTENDEDTOOLKIT_API UPCGExClusterNodesData : public UPCGExClusterData
{
	GENERATED_BODY()

	mutable FRWLock BoundClustersLock;

protected:
	virtual UPCGSpatialData* CopyInternal(FPCGContext* Context) const override;
};

/**
 * 
 */
UCLASS()
class PCGEXTENDEDTOOLKIT_API UPCGExClusterEdgesData : public UPCGExClusterData
{
	GENERATED_BODY()

public:
	virtual void InitializeSpatialDataInternal(const FPCGInitializeFromDataParams& InParams) override;

	virtual void SetBoundCluster(const TSharedPtr<PCGExCluster::FCluster>& InCluster);
	const TSharedPtr<PCGExCluster::FCluster>& GetBoundCluster() const;

	virtual void BeginDestroy() override;

protected:
	TSharedPtr<PCGExCluster::FCluster> Cluster;

	virtual UPCGSpatialData* CopyInternal(FPCGContext* Context) const override;
};

namespace PCGExClusterData
{
	TSharedPtr<PCGExCluster::FCluster> TryGetCachedCluster(const TSharedRef<PCGExData::FPointIO>& VtxIO, const TSharedRef<PCGExData::FPointIO>& EdgeIO);
}
