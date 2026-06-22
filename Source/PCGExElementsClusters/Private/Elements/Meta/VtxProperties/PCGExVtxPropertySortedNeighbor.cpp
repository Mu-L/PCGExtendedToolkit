// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/Meta/VtxProperties/PCGExVtxPropertySortedNeighbor.h"

#include "PCGPin.h"
#include "Clusters/PCGExCluster.h"
#include "Containers/PCGExManagedObjects.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Sorting/PCGExPointSorter.h"
#include "Sorting/PCGExSortingDetails.h"


#define LOCTEXT_NAMESPACE "PCGExVtxPropertySortedNeighbor"
#define PCGEX_NAMESPACE PCGExVtxPropertySortedNeighbor

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

	if (SortingRules.IsEmpty())
	{
		PCGE_LOG_C(Warning, GraphAndLog, InContext, FTEXT("Vtx : Sorted Neighbor -- no sorting rules provided."));
		bIsValidOperation = false;
		return false;
	}

	// Sorter reads the vtx attributes through Direct proxies (immutable input, random-access safe).
	Sorter = MakeShared<PCGExSorting::FSorter>(InContext, InVtxDataFacade.ToSharedRef(), SortingRules);
	Sorter->SortDirection = Config.SortDirection;
	if (!Sorter->Init(InContext))
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

#if WITH_EDITOR
FString UPCGExVtxPropertySortedNeighborSettings::GetDisplayName() const
{
	return TEXT("");
}
#endif

TSharedPtr<FPCGExVtxPropertyOperation> UPCGExVtxPropertySortedNeighborFactory::CreateOperation(FPCGExContext* InContext) const
{
	PCGEX_FACTORY_NEW_OPERATION(VtxPropertySortedNeighbor)
	PCGEX_VTX_EXTRA_CREATE
	NewOperation->SortingRules = SortingRules;
	return NewOperation;
}

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

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
