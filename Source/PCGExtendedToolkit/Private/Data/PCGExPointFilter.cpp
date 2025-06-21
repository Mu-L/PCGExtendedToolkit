﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExPointFilter.h"


#include "Graph/PCGExCluster.h"
#include "Paths/PCGExShiftPath.h"

TSharedPtr<PCGExPointFilter::FFilter> UPCGExFilterFactoryData::CreateFilter() const
{
	return nullptr;
}


bool UPCGExFilterFactoryData::DomainCheck()
{
	return false;
}

bool UPCGExFilterFactoryData::Init(FPCGExContext* InContext)
{
	bOnlyUseDataDomain = DomainCheck(); // Will check selectors for @Data domain
	return true;
}

namespace PCGExPointFilter
{
	bool FFilter::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InPointDataFacade)
	{
		PointDataFacade = InPointDataFacade;
		return true;
	}

	void FFilter::PostInit()
	{
		if (!bCacheResults) { return; }
		const int32 NumResults = PointDataFacade->Source->GetNum();
		Results.Init(false, NumResults);
	}

	bool FFilter::Test(const int32 Index) const
	PCGEX_NOT_IMPLEMENTED_RET(FFilter::Test(const int32 Index), false)

	bool FFilter::Test(const PCGExData::FProxyPoint& Point) const
	PCGEX_NOT_IMPLEMENTED_RET(FFilter::Test(const PCGExData::FProxyPoint& Point), false)

	bool FFilter::Test(const PCGExCluster::FNode& Node) const { return Test(Node.PointIndex); }
	bool FFilter::Test(const PCGExGraph::FEdge& Edge) const { return Test(Edge.PointIndex); }

	bool FFilter::Test(const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection) const { return bCollectionTestResult; }

	bool FSimpleFilter::Test(const int32 Index) const
	PCGEX_NOT_IMPLEMENTED_RET(FSimpleFilter::Test(const PCGExCluster::FNode& Node), false)

	bool FSimpleFilter::Test(const PCGExData::FProxyPoint& Point) const
	PCGEX_NOT_IMPLEMENTED_RET(FSimpleFilter::TestRoamingPoint(const PCGExCluster::PCGExData::FProxyPoint& Point), false)

	bool FSimpleFilter::Test(const PCGExCluster::FNode& Node) const { return Test(Node.PointIndex); }
	bool FSimpleFilter::Test(const PCGExGraph::FEdge& Edge) const { return Test(Edge.PointIndex); }

	bool FSimpleFilter::Test(const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection) const { return bCollectionTestResult; }

	bool FCollectionFilter::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InPointDataFacade)
	{
		if (!FFilter::Init(InContext, InPointDataFacade)) { return false; }
		bCollectionTestResult = Test(InPointDataFacade->Source, nullptr);
		return true;
	}

	bool FCollectionFilter::Test(const int32 Index) const { return bCollectionTestResult; }
	bool FCollectionFilter::Test(const PCGExData::FProxyPoint& Point) const { return bCollectionTestResult; }

	bool FCollectionFilter::Test(const PCGExCluster::FNode& Node) const { return bCollectionTestResult; }
	bool FCollectionFilter::Test(const PCGExGraph::FEdge& Edge) const { return bCollectionTestResult; }

	bool FCollectionFilter::Test(const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection) const
	PCGEX_NOT_IMPLEMENTED_RET(FCollectionFilter::Test(FPCGExContext* InContext, const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection), false)

	FManager::FManager(const TSharedRef<PCGExData::FFacade>& InPointDataFacade)
		: PointDataFacade(InPointDataFacade)
	{
	}

	bool FManager::Init(FPCGExContext* InContext, const TArray<TObjectPtr<const UPCGExFilterFactoryData>>& InFactories)
	{
		for (const UPCGExFilterFactoryData* Factory : InFactories)
		{
			TSharedPtr<FFilter> NewFilter = Factory->CreateFilter();
			NewFilter->bUseDataDomainSelectorsOnly = Factory->GetOnlyUseDataDomain();
			NewFilter->bCacheResults = bCacheResultsPerFilter;
			NewFilter->bUseEdgeAsPrimary = bUseEdgeAsPrimary;
			if (!InitFilter(InContext, NewFilter))
			{
				PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(FTEXT("A filter failed to initialize properly : {0}."), FText::FromString(Factory->GetName())));
				continue;
			}
			ManagedFilters.Add(NewFilter);
		}

		return PostInit(InContext);
	}

	bool FManager::Test(const int32 Index)
	{
		for (const TSharedPtr<FFilter>& Handler : ManagedFilters) { if (!Handler->Test(Index)) { return false; } }
		return true;
	}

	bool FManager::Test(const PCGExData::FProxyPoint& Point)
	{
		for (const TSharedPtr<FFilter>& Handler : ManagedFilters) { if (!Handler->Test(Point)) { return false; } }
		return true;
	}

	bool FManager::Test(const PCGExCluster::FNode& Node)
	{
		for (const TSharedPtr<FFilter>& Handler : ManagedFilters) { if (!Handler->Test(Node)) { return false; } }
		return true;
	}

	bool FManager::Test(const PCGExGraph::FEdge& Edge)
	{
		for (const TSharedPtr<FFilter>& Handler : ManagedFilters) { if (!Handler->Test(Edge)) { return false; } }
		return true;
	}

	bool FManager::Test(const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection)
	{
		for (const TSharedPtr<FFilter>& Handler : ManagedFilters) { if (!Handler->Test(IO, ParentCollection)) { return false; } }
		return true;
	}

	int32 FManager::Test(const PCGExMT::FScope Scope, TArray<int8>& OutResults)
	{
		bool bResult = true;
		int32 NumPass = 0;
		PCGEX_SCOPE_LOOP(Index)
		{
			bResult = true;
			for (const TSharedPtr<FFilter>& Handler : ManagedFilters)
			{
				if (!Handler->Test(Index))
				{
					bResult = false;
					break;
				}
			}
			OutResults[Index] = bResult;
			NumPass += bResult;
		}

		return NumPass;
	}

	int32 FManager::Test(const PCGExMT::FScope Scope, TBitArray<>& OutResults)
	{
		bool bResult = true;
		int32 NumPass = 0;
		PCGEX_SCOPE_LOOP(Index)
		{
			bResult = true;
			for (const TSharedPtr<FFilter>& Handler : ManagedFilters)
			{
				if (!Handler->Test(Index))
				{
					bResult = false;
					break;
				}
			}
			OutResults[Index] = bResult;
			NumPass += bResult;
		}

		return NumPass;
	}

	int32 FManager::Test(const TArrayView<PCGExCluster::FNode> Items, const TArrayView<int8> OutResults)
	{
		check(Items.Num() == OutResults.Num());

		bool bResult = true;
		int32 NumPass = 0;

		for (int i = 0; i < Items.Num(); i++)
		{
			bResult = true;
			const PCGExCluster::FNode& Node = Items[i];
			for (const TSharedPtr<FFilter>& Handler : ManagedFilters)
			{
				if (!Handler->Test(Node))
				{
					bResult = false;
					break;
				}
			}
			OutResults[i] = bResult;
			NumPass += bResult;
		}

		return NumPass;
	}

	int32 FManager::Test(const TArrayView<PCGExCluster::FNode> Items, const TSharedPtr<TArray<int8>>& OutResults)
	{
		bool bResult = true;
		int32 NumPass = 0;
		TArray<int8>& OutResultsRef = *OutResults.Get();

		for (int i = 0; i < Items.Num(); i++)
		{
			bResult = true;
			const PCGExCluster::FNode& Node = Items[i];
			for (const TSharedPtr<FFilter>& Handler : ManagedFilters)
			{
				if (!Handler->Test(Node))
				{
					bResult = false;
					break;
				}
			}

			OutResultsRef[Node.PointIndex] = bResult;
			NumPass += bResult;
		}

		return NumPass;
	}

	int32 FManager::Test(const TArrayView<PCGExCluster::FEdge> Items, const TArrayView<int8> OutResults)
	{
		check(Items.Num() == OutResults.Num());
		bool bResult = true;
		int32 NumPass = 0;

		for (int i = 0; i < Items.Num(); i++)
		{
			bResult = true;
			const PCGExCluster::FEdge& Edge = Items[i];
			for (const TSharedPtr<FFilter>& Handler : ManagedFilters)
			{
				if (!Handler->Test(Edge))
				{
					bResult = false;
					break;
				}
			}
			OutResults[i] = bResult;
			NumPass += bResult;
		}

		return NumPass;
	}

	bool FManager::InitFilter(FPCGExContext* InContext, const TSharedPtr<FFilter>& Filter)
	{
		return Filter->Init(InContext, PointDataFacade);
	}

	bool FManager::PostInit(FPCGExContext* InContext)
	{
		bValid = !ManagedFilters.IsEmpty();

		if (!bValid) { return false; }

		// Sort mappings so higher priorities come last, as they have to potential to override values.
		ManagedFilters.Sort([](const TSharedPtr<FFilter>& A, const TSharedPtr<FFilter>& B) { return A->Factory->Priority < B->Factory->Priority; });

		// Update index & post-init
		for (int i = 0; i < ManagedFilters.Num(); i++)
		{
			TSharedPtr<FFilter> Filter = ManagedFilters[i];
			Filter->FilterIndex = i;
			PostInitFilter(InContext, Filter);
		}

		if (bCacheResults) { InitCache(); }

		return true;
	}

	void FManager::PostInitFilter(FPCGExContext* InContext, const TSharedPtr<FFilter>& InFilter)
	{
		InFilter->PostInit();
	}

	void FManager::InitCache()
	{
		const int32 NumResults = PointDataFacade->Source->GetNum();
		Results.Init(false, NumResults);
	}
}
