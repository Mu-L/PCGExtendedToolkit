// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Core/PCGExClusterFilter.h"
#include "Core/PCGExFilterFactoryProvider.h"
#include "Utils/PCGExCompare.h"

#include "PCGExEdgeEndpointsCheckFilter.generated.h"

UENUM()
enum class EPCGExEdgeEndpointsCheckMode : uint8
{
	None   = 0 UMETA(DisplayName = "None", Tooltip="None of the endpoint must has the expected result."),
	Both   = 1 UMETA(DisplayName = "Both", Tooltip="Both endpoints must have the expected result."),
	Any    = 2 UMETA(DisplayName = "Any Pass", Tooltip="At least one endpoint must have the expected result."),
	Start  = 3 UMETA(DisplayName = "Start", Tooltip="Start must have the expected result."),
	End    = 4 UMETA(DisplayName = "End", Tooltip="End must have the expected result."),
	SeeSaw = 20 UMETA(DisplayName = "SeeSaw", Tooltip="One must pass and the other must fail"),
};


USTRUCT(BlueprintType)
struct FPCGExEdgeEndpointsCheckFilterConfig
{
	GENERATED_BODY()

	FPCGExEdgeEndpointsCheckFilterConfig()
	{
	}

	/** Use two separate filter sets -- one per endpoint -- instead of a single set applied to both endpoints. Enables testing mixed conditions on an edge (e.g. one endpoint matches set A while the other matches set B). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName="Two Filter Sets"))
	bool bUseTwoFilterSets = false;

	/** Mode -- how the single filter set's two endpoint results are combined. Only used in single-input mode. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="!bUseTwoFilterSets", EditConditionHides))
	EPCGExEdgeEndpointsCheckMode Mode = EPCGExEdgeEndpointsCheckMode::Both;

	/** If enabled, the first set ('Vtx Filters') is tested strictly against the edge's Start endpoint and the second set ('Vtx Filters (B)') against its End endpoint. If disabled, the two sets are matched to the two endpoints in either orientation (one endpoint matches A and the other matches B). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="bUseTwoFilterSets", EditConditionHides))
	bool bRespectEdgeDirection = false;

	/** The expected result for the (first) filter set. In single-input mode this is combined per 'Mode' (e.g. mode = "Both" and Comparison = "Pass" means both endpoints must pass). In two-input mode it is the expectation for the 'Vtx Filters' set. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName="Comparison", EditCondition="bUseTwoFilterSets || Mode != EPCGExEdgeEndpointsCheckMode::SeeSaw", EditConditionHides))
	EPCGExFilterResult Expects = EPCGExFilterResult::Pass;

	/** The expected result for the second filter set ('Vtx Filters (B)'). Only used in two-input mode. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName="Comparison (B)", EditCondition="bUseTwoFilterSets", EditConditionHides))
	EPCGExFilterResult ExpectsB = EPCGExFilterResult::Pass;

	/** Invert the filter result (pass becomes fail and vice versa). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	bool bInvert = false;
};

/**
 * 
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExEdgeEndpointsCheckFilterFactory : public UPCGExEdgeFilterFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExEdgeEndpointsCheckFilterConfig Config;

	UPROPERTY()
	TArray<TObjectPtr<const UPCGExPointFilterFactoryData>> FilterFactories;

	/** Second filter set, used only when Config.bUseTwoFilterSets is enabled. */
	UPROPERTY()
	TArray<TObjectPtr<const UPCGExPointFilterFactoryData>> FilterFactoriesB;

	virtual void RegisterBuffersDependencies(FPCGExContext* InContext, PCGExData::FFacadePreloader& FacadePreloader) const override;
	virtual bool RegisterConsumableAttributes(FPCGExContext* InContext) const override;
	virtual bool RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const override;

	virtual TSharedPtr<PCGExPointFilter::IFilter> CreateFilter() const override;
};

namespace PCGExEdgeEndpointsCheck
{
	class FFilter final : public PCGExClusterFilter::IEdgeFilter
	{
	public:
		explicit FFilter(const UPCGExEdgeEndpointsCheckFilterFactory* InFactory)
			: IEdgeFilter(InFactory)
			  , TypedFilterFactory(InFactory)
		{
		}

		const UPCGExEdgeEndpointsCheckFilterFactory* TypedFilterFactory;

		int8 Expected = 0;
		int8 ExpectedB = 0;

		TArray<int8> ResultCache;
		TArray<int8> ResultCacheB;

		TSharedPtr<PCGExClusterFilter::FManager> VtxFiltersManager;
		TSharedPtr<PCGExClusterFilter::FManager> VtxFiltersManagerB;

		virtual bool Init(FPCGExContext* InContext, const TSharedRef<PCGExClusters::FCluster>& InCluster, const TSharedRef<PCGExData::FFacade>& InPointDataFacade, const TSharedRef<PCGExData::FFacade>& InEdgeDataFacade) override;
		virtual bool Test(const PCGExGraphs::FEdge& Edge) const override;

		virtual ~FFilter() override;
	};
}


/** Outputs a single GraphParam to be consumed by other nodes */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Graph|Params", meta=(PCGExNodeLibraryDoc="filters/edge-filters/edge-filter-endpoints-check"))
class UPCGExEdgeEndpointsCheckFilterProviderSettings : public UPCGExEdgeFilterProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(EdgeEndpointsCheckFilterFactory, "Edge Filter : Endpoints Check", "Uses filters applied to the edge endpoints' in order to determine whether this filter result'.", PCGEX_FACTORY_NAME_PRIORITY)

	virtual FLinearColor GetNodeTitleColor() const override
	{
		return PCGEX_NODE_COLOR_OPTIN_NAME(FilterCluster);
	}
#endif
	//~End UPCGSettings

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;

public:
	/** Test Config.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExEdgeEndpointsCheckFilterConfig Config;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
