﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Filters/Edges/PCGExEdgeEndpointsCompareNumFilter.h"


#include "Graph/PCGExGraph.h"

#define LOCTEXT_NAMESPACE "PCGExEdgeEndpointsCompareNumFilter"
#define PCGEX_NAMESPACE EdgeEndpointsCompareNumFilter

void UPCGExEdgeEndpointsCompareNumFilterFactory::RegisterBuffersDependencies(FPCGExContext* InContext, PCGExData::FFacadePreloader& FacadePreloader) const
{
	Super::RegisterBuffersDependencies(InContext, FacadePreloader);
	FacadePreloader.Register<double>(InContext, Config.Attribute);
}

bool UPCGExEdgeEndpointsCompareNumFilterFactory::RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const
{
	if (!Super::RegisterConsumableAttributesWithData(InContext, InData)) { return false; }

	FName Consumable = NAME_None;
	PCGEX_CONSUMABLE_SELECTOR(Config.Attribute, Consumable)

	return true;
}

TSharedPtr<PCGExPointFilter::IFilter> UPCGExEdgeEndpointsCompareNumFilterFactory::CreateFilter() const
{
	return MakeShared<PCGExEdgeEndpointsCompareNum::FNeighborsCountFilter>(this);
}

namespace PCGExEdgeEndpointsCompareNum
{
	bool FNeighborsCountFilter::Init(FPCGExContext* InContext, const TSharedRef<PCGExCluster::FCluster>& InCluster, const TSharedRef<PCGExData::FFacade>& InPointDataFacade, const TSharedRef<PCGExData::FFacade>& InEdgeDataFacade)
	{
		if (!IFilter::Init(InContext, InCluster, InPointDataFacade, InEdgeDataFacade)) { return false; }

		NumericBuffer = InPointDataFacade->GetBroadcaster<double>(TypedFilterFactory->Config.Attribute);
		if (!NumericBuffer)
		{
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(FTEXT("Comparison Attribute ({0}) is not valid."), FText::FromString(PCGEx::GetSelectorDisplayName(TypedFilterFactory->Config.Attribute))));
			return false;
		}

		return true;
	}

	bool FNeighborsCountFilter::Test(const PCGExGraph::FEdge& Edge) const
	{
		const bool bResult = PCGExCompare::Compare(TypedFilterFactory->Config.Comparison, NumericBuffer->Read(Edge.Start), NumericBuffer->Read(Edge.End), TypedFilterFactory->Config.Tolerance);
		return TypedFilterFactory->Config.bInvert ? !bResult : bResult;
	}

	FNeighborsCountFilter::~FNeighborsCountFilter()
	{
		TypedFilterFactory = nullptr;
	}
}

PCGEX_CREATE_FILTER_FACTORY(EdgeEndpointsCompareNum)

#if WITH_EDITOR
FString UPCGExEdgeEndpointsCompareNumFilterProviderSettings::GetDisplayName() const
{
	return TEXT("A' ") + PCGEx::GetSelectorDisplayName(Config.Attribute) + PCGExCompare::ToString(Config.Comparison) + TEXT(" B' ") + PCGEx::GetSelectorDisplayName(Config.Attribute);
}
#endif

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
