// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Details/PCGExStagingDetails.h"
#include "Distributions/PCGExDistributionFactoryProvider.h"
#include "PCGExCollectionsCommon.h"

#include "PCGExDistributionClassic.generated.h"

/**
 * Classic (built-in) distribution factory data. Supports Index, Random, and WeightedRandom
 * picks via the Mode enum -- consistent with the Legacy inline struct UX on the consuming
 * node so users can swap between inline and factory configuration without re-learning the knobs.
 *
 * User-authored custom factories should subclass UPCGExDistributionFactoryData directly
 * and override CreateEntryOperation; this class is a reference example of that contract.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXCOLLECTIONS_API UPCGExDistributionClassicFactoryData : public UPCGExDistributionFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	EPCGExDistribution Mode = EPCGExDistribution::WeightedRandom;

	UPROPERTY()
	FPCGExAssetDistributionIndexDetails IndexConfig;

	virtual TSharedPtr<FPCGExEntryPickerOperation> CreateEntryOperation(FPCGExContext* InContext) const override;
};

/**
 * Palette node: "Distribution : Classic". Produces the built-in distribution factory for the selected Mode.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Collections|Distribution", meta=(PCGExNodeLibraryDoc="collections/distribution/classic"))
class PCGEXCOLLECTIONS_API UPCGExDistributionClassicFactoryProviderSettings : public UPCGExDistributionFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(DistributionClassic, "Distribution : Classic", "Built-in distribution factory. Supports Index, Random, and Weighted Random selection modes.")
#endif
	//~End UPCGSettings

	/** Distribution strategy. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExDistribution Mode = EPCGExDistribution::WeightedRandom;

	/** Index picking configuration. Only used when Mode is Index. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName=" └─ Index Settings", EditCondition="Mode == EPCGExDistribution::Index", EditConditionHides))
	FPCGExAssetDistributionIndexDetails IndexConfig;

	/** Shared distribution configuration (seed, entry distribution, categories). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExDistributionFactoryBaseConfig BaseConfig;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;
};
