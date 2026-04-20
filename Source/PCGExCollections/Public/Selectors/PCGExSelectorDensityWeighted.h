// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Details/PCGExInputShorthandsDetails.h"
#include "Selectors/PCGExSelectorFactoryProvider.h"
#include "Selectors/PCGExEntryPickerOperation.h"

#include "PCGExSelectorDensityWeighted.generated.h"

/** Algorithm flavor for Density-Weighted distribution. */
UENUM()
enum class EPCGExDensityWeightMode : uint8
{
	WeightModulation     = 0 UMETA(DisplayName = "Weight Modulation", ToolTip="Per-point effective weight = entry.Weight * f(density). Higher density prefers higher-weight entries."),
	RandomnessModulation = 1 UMETA(DisplayName = "Randomness Modulation", ToolTip="density=1 picks purely weighted-random; density=0 picks uniform-random; blends linearly between."),
};

/** What to do when the density value falls outside the expected [0, 1] range. */
UENUM()
enum class EPCGExDensityOutOfRangePolicy : uint8
{
	Clamp     = 0 UMETA(DisplayName = "Clamp", ToolTip="Clamp the density value to [0, 1] before picking."),
	SkipPoint = 1 UMETA(DisplayName = "Skip Point", ToolTip="Return an invalid pick result so the consuming node skips the point."),
};

/**
 * Density-driven weighted pick operation. Reads a per-point density value and modulates the
 * pick using one of two algorithms (see EPCGExDensityWeightMode).
 *
 * Not shared with the base factory's micro dispatch (density-weighting applies to entry picks
 * only, not to material variant picks). Lives alongside the factory UCLASSes.
 */
class FPCGExEntryDensityWeightedPickerOp : public FPCGExEntryPickerOperation
{
public:
	EPCGExDensityWeightMode Mode = EPCGExDensityWeightMode::RandomnessModulation;
	double DensityInfluence = 1.0;
	EPCGExDensityOutOfRangePolicy OutOfRangePolicy = EPCGExDensityOutOfRangePolicy::Clamp;
	FPCGExInputShorthandSelectorDouble DensitySource;

	TSharedPtr<PCGExDetails::TSettingValue<double>> DensityGetter;

	virtual bool PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection) override;
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override;
};

/**
 * Factory data for Density-Weighted distribution. Per-point density modulates the pick
 * distribution. Two algorithms are available via Mode (see EPCGExDensityWeightMode).
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXCOLLECTIONS_API UPCGExSelectorDensityWeightedFactoryData : public UPCGExSelectorFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	EPCGExDensityWeightMode Mode = EPCGExDensityWeightMode::RandomnessModulation;

	UPROPERTY()
	FPCGExInputShorthandSelectorDouble DensitySource = FPCGExInputShorthandSelectorDouble(FName("$Density"), 1.0, true);

	UPROPERTY()
	double DensityInfluence = 1.0;

	UPROPERTY()
	EPCGExDensityOutOfRangePolicy OutOfRangePolicy = EPCGExDensityOutOfRangePolicy::Clamp;

	virtual TSharedPtr<FPCGExEntryPickerOperation> CreateEntryOperation(FPCGExContext* InContext) const override;
};

/**
 * Palette node: "Selector : Density-Weighted".
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Collections|Distribution", meta=(PCGExNodeLibraryDoc="collections/selector/density-weighted"))
class PCGEXCOLLECTIONS_API UPCGExSelectorDensityWeightedFactoryProviderSettings : public UPCGExSelectorFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(
		SelectorDensityWeighted, "Selector : Density-Weighted",
		"Per-point density modulates the pick distribution. Supports weight-modulation and randomness-modulation algorithms.",
		FName(GetDisplayName()))
#endif
	//~End UPCGSettings

	/** Algorithm for how density influences the pick. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExDensityWeightMode Mode = EPCGExDensityWeightMode::RandomnessModulation;

	/** Per-point density source (attribute or constant). Defaults to the point's $Density property. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExInputShorthandSelectorDouble DensitySource = FPCGExInputShorthandSelectorDouble(FName("$Density"), 1.0, true);

	/** Blend factor (0..1) between plain weighted random (0) and full density-driven pick (1). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ClampMin=0, ClampMax=1, UIMin=0, UIMax=1))
	double DensityInfluence = 1.0;

	/** Behavior when density falls outside the expected [0, 1] range. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExDensityOutOfRangePolicy OutOfRangePolicy = EPCGExDensityOutOfRangePolicy::Clamp;

	/** Shared distribution configuration (seed, entry distribution, categories). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExSelectorFactoryBaseConfig BaseConfig;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
