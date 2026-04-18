// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExCollectionsCommon.h"
#include "Details/PCGExInputShorthandsDetails.h"
#include "Details/PCGExStagingDetails.h"

#include "PCGExSelectorFactoryBaseConfig.generated.h"

/**
 * Shared base configuration embedded on every concrete selector factory data.
 * Mirrors the FPCGExShapeConfigBase pattern from the Shapes module.
 *
 * Holds the picker-side concerns that are orthogonal to the main distribution strategy:
 * seed configuration, the entry-level (micro) distribution used for material variants, and
 * category scoping with its missing-key behavior.
 */
USTRUCT(BlueprintType)
struct PCGEXCOLLECTIONS_API FPCGExSelectorFactoryBaseConfig
{
	GENERATED_BODY()

	FPCGExSelectorFactoryBaseConfig() = default;
	virtual ~FPCGExSelectorFactoryBaseConfig() = default;

	/** Which components contribute to per-point seed generation. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Seed", meta=(PCG_Overridable, Bitmask, BitmaskEnum="/Script/PCGExCore.EPCGExSeedComponents"))
	uint8 SeedComponents = 0;

	/** Per-factory seed offset applied on top of SeedComponents. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Seed", meta=(PCG_Overridable))
	int32 LocalSeed = 0;

	/** Entry-level (micro) distribution -- picks material variants etc. within a picked entry. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Entry Distribution", meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExMicroCacheDistributionDetails EntryDistribution;

	/** If enabled, limit picks to entries flagged with a specific category. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Category", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bUseCategories = false;

	/** Category name source (constant or per-point attribute). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Category", meta=(PCG_Overridable, EditCondition="bUseCategories"))
	FPCGExInputShorthandNameName Category = FPCGExInputShorthandNameName(FName("Category"), FName("MyCategory"), false);

	/** What to do when a point's Category attribute does not match any named category in the collection. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Category", meta=(PCG_Overridable, EditCondition="bUseCategories"))
	EPCGExMissingCategoryBehavior MissingCategoryBehavior = EPCGExMissingCategoryBehavior::Skip;
};
