// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/Meta/VtxProperties/PCGExVtxPropertySortedNeighbor.h"

#include "PCGPin.h"
#include "Clusters/PCGExCluster.h"
#include "Containers/PCGExManagedObjects.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Misc/ScopeLock.h"
#include "Sorting/PCGExPointSorter.h"
#include "Sorting/PCGExSortingDetails.h"


#define LOCTEXT_NAMESPACE "PCGExVtxPropertySortedNeighbor"
#define PCGEX_NAMESPACE PCGExVtxPropertySortedNeighbor

#pragma region FPCGExVtxPropertySortedNeighbor

bool FPCGExVtxPropertySortedNeighbor::PrepareForCluster(FPCGExContext* InContext, TSharedPtr<PCGExClusters::FCluster> InCluster, const TSharedPtr<PCGExData::FFacade>& InVtxDataFacade, const TSharedPtr<PCGExData::FFacade>& InEdgeDataFacade)
{
	if (!FPCGExVtxPropertyOperation::PrepareForCluster(InContext, InCluster, InVtxDataFacade, InEdgeDataFacade))
	{
		return false;
	}

	if (!Config.SortedNeighbor.Validate(InContext))
	{
		bIsValidOperation = false;
		return false;
	}

	// The sorter is shared across all clusters of this vtx facade (built once, see GetOrBuildSorter).
	check(Factory);
	Sorter = Factory->GetOrBuildSorter(InContext, InVtxDataFacade.ToSharedRef());
	if (!Sorter)
	{
		bIsValidOperation = false;
		return false;
	}

	Config.SortedNeighbor.Init(InVtxDataFacade.ToSharedRef());

	return bIsValidOperation;
}

void FPCGExVtxPropertySortedNeighbor::ProcessNode(PCGExClusters::FNode& Node, const TArray<PCGExClusters::FAdjacencyData>& Adjacency, const PCGExMath::FBestFitPlane& BFP)
{
	if (Adjacency.IsEmpty())
	{
		Config.SortedNeighbor.Set(Node.PointIndex, 0, FVector::ZeroVector, -1, -1, 0);
		return;
	}

	// Keep the single most extreme neighbor; Sort(A, B) is true when A ranks before B under the chosen direction.
	int32 IBest = 0;
	for (int i = 1; i < Adjacency.Num(); i++)
	{
		if (Sorter->Sort(Adjacency[i].NodePointIndex, Adjacency[IBest].NodePointIndex))
		{
			IBest = i;
		}
	}

	Config.SortedNeighbor.Set(Node.PointIndex, Adjacency[IBest], Cluster->GetNode(Adjacency[IBest].NodeIndex)->Num());
}

#pragma endregion

#pragma region UPCGExVtxPropertySortedNeighborFactory

TSharedPtr<FPCGExVtxPropertyOperation> UPCGExVtxPropertySortedNeighborFactory::CreateOperation(FPCGExContext* InContext) const
{
	PCGEX_FACTORY_NEW_OPERATION(VtxPropertySortedNeighbor)
	PCGEX_VTX_EXTRA_CREATE
	NewOperation->Factory = this;
	return NewOperation;
}

TSharedPtr<PCGExSorting::FSorter> UPCGExVtxPropertySortedNeighborFactory::GetOrBuildSorter(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InVtxDataFacade) const
{
	PCGExData::FFacade* Key = &InVtxDataFacade.Get();

	FScopeLock Lock(&SorterLock);

	if (const TSharedPtr<PCGExSorting::FSorter>* Existing = SortersByFacade.Find(Key))
	{
		return *Existing;
	}

	TSharedPtr<PCGExSorting::FSorter> NewSorter;

	if (SortingRules.IsEmpty())
	{
		PCGE_LOG_C(Warning, GraphAndLog, InContext, FTEXT("Vtx : Sorted Neighbor -- no sorting rules provided."));
	}
	else
	{
		NewSorter = MakeShared<PCGExSorting::FSorter>(InContext, InVtxDataFacade, SortingRules);
		NewSorter->SortDirection = Config.SortDirection;
		if (!NewSorter->Init(InContext))
		{
			NewSorter = nullptr;
		}
	}

	// Cache the result (even a null failure) so we don't retry the build for every cluster.
	SortersByFacade.Add(Key, NewSorter);
	return NewSorter;
}

#pragma endregion

#pragma region UPCGExVtxPropertySortedNeighborSettings

#if WITH_EDITOR
FString UPCGExVtxPropertySortedNeighborSettings::GetDisplayName() const
{
	return TEXT("");
}
#endif

TArray<FPCGPinProperties> UPCGExVtxPropertySortedNeighborSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGExSorting::DeclareSortingRulesInputs(PinProperties, EPCGPinStatus::Required);
	return PinProperties;
}

UPCGExFactoryData* UPCGExVtxPropertySortedNeighborSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExVtxPropertySortedNeighborFactory* NewFactory = InContext->ManagedObjects->New<UPCGExVtxPropertySortedNeighborFactory>();
	NewFactory->Config = Config;
	NewFactory->SortingRules = PCGExSorting::GetSortingRules(InContext, PCGExSorting::Labels::SourceSortingRules);
	return Super::CreateFactory(InContext, NewFactory);
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
