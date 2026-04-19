// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Details/PCGExInputShorthandsDetails.h"
#include "Selectors/PCGExSelectorFactoryProvider.h"
#include "Selectors/PCGExEntryPickerOperation.h"
#include "Data/PCGBasePointData.h"

#include "PCGExSelectorBestFit.generated.h"

/** How entry extents are scored against per-point extents. */
UENUM()
enum class EPCGExBestFitMetric : uint8
{
	ClosestVolume      = 0 UMETA(DisplayName = "Closest Volume", ToolTip="Score by absolute volume difference (normalized). Scale-sensitive; ignores axis mask."),
	ClosestPerAxis     = 1 UMETA(DisplayName = "Closest Per-Axis", ToolTip="Score by per-axis extent differences on selected axes. Scale-sensitive."),
	ClosestAspectRatio = 2 UMETA(DisplayName = "Closest Aspect Ratio", ToolTip="Score by shape similarity on selected axes (max-normalized extents). Scale-agnostic, with optional volume influence."),
};

/** Aggregation of per-axis scores into a single scalar when multiple axes are selected. */
UENUM()
enum class EPCGExBestFitAxisAggregation : uint8
{
	Sum = 0 UMETA(DisplayName = "Sum (L1)", ToolTip="Errors accumulate across axes. Averaging behavior."),
	Max = 1 UMETA(DisplayName = "Max (L∞)", ToolTip="Largest single-axis error dominates. Any axis blowout disqualifies."),
};

/** How the pool of candidates is selected from the scored entries. */
UENUM()
enum class EPCGExBestFitPoolStrategy : uint8
{
	TopK      = 0 UMETA(DisplayName = "Top K", ToolTip="Pool = the K best-scoring entries. Pool size is constant (per point)."),
	Tolerance = 1 UMETA(DisplayName = "Tolerance", ToolTip="Pool = entries within PoolSize ratio of the best score. Pool size varies with data."),
};

/** Bitmask matching FVector component indices. */
UENUM(meta=(Bitflags))
enum class EPCGExAxisMask : uint8
{
	None = 0 UMETA(Hidden),
	X    = 1 << 0,
	Y    = 1 << 1,
	Z    = 1 << 2,
};
ENUM_CLASS_FLAGS(EPCGExAxisMask)

/**
 * Shared base for Best Fit picker operations. Caches per-entry extents/volumes/weights at Init,
 * scores all valid entries per point, and produces a pool for the concrete subclass to weighted-pick from.
 *
 * Concrete subclasses differ only in pool selection (TopK vs Tolerance). Scoring is uniform across them.
 */
class FPCGExEntryBestFitPickerOpBase : public FPCGExEntryPickerOperation
{
public:
	// Copied from factory before PrepareForData
	EPCGExBestFitMetric Metric = EPCGExBestFitMetric::ClosestVolume;
	uint8 AxisMask = static_cast<uint8>(EPCGExAxisMask::X) | static_cast<uint8>(EPCGExAxisMask::Y) | static_cast<uint8>(EPCGExAxisMask::Z);
	EPCGExBestFitAxisAggregation AxisAggregation = EPCGExBestFitAxisAggregation::Sum;
	double VolumeInfluence = 0.0;
	bool bApplyPointScale = false;
	FPCGExInputShorthandSelectorDouble PoolSize;

	// Resolved at PrepareForData — parallel to Target->Entries (invalid entries have Volume<=0)
	TArray<FVector> EntryExtents;
	TArray<FVector> EntryExtentsMaxNorm; // max-component normalized, for AspectRatio
	TArray<double> EntryVolumes;
	TArray<double> EntryWeights;

	// Valid entries only (Volume > 0). Provides the iteration set for scoring.
	TArray<int32> ValidEntryIndices;

	// Per-point extent sources — cached at PrepareForData, read directly in the hot path.
	TConstPCGValueRange<FVector> BoundsMinRange;
	TConstPCGValueRange<FVector> BoundsMaxRange;
	TConstPCGValueRange<FTransform> TransformRange;

	// Per-point pool size driver.
	TSharedPtr<PCGExDetails::TSettingValue<double>> PoolSizeGetter;

	virtual bool PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection) override;
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override = 0;

protected:
	/** Resolve the point's extent (half-size) from its bounds data; optionally apply transform scale. */
	FVector GetPointExtents(int32 PointIndex) const;

	/** Score a single entry against the given point extents. Lower is better. */
	double ComputeScore(const FVector& PointExtents, int32 EntryIndex) const;
};

/** Pool = K best-scoring entries; weighted random among the pool. */
class FPCGExEntryBestFitTopKPickerOp : public FPCGExEntryBestFitPickerOpBase
{
public:
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override;
};

/** Pool = entries within tolerance * best_score of the best score; weighted random among the pool. */
class FPCGExEntryBestFitTolerancePickerOp : public FPCGExEntryBestFitPickerOpBase
{
public:
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override;
};

/**
 * Factory data for Best Fit selection. Scores all entries in the target category against the
 * per-point extent, builds a pool per PoolStrategy, and weighted-random picks from it.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXCOLLECTIONS_API UPCGExSelectorBestFitFactoryData : public UPCGExSelectorFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	EPCGExBestFitMetric Metric = EPCGExBestFitMetric::ClosestVolume;

	UPROPERTY()
	uint8 AxisMask = 7;

	UPROPERTY()
	EPCGExBestFitAxisAggregation AxisAggregation = EPCGExBestFitAxisAggregation::Sum;

	UPROPERTY()
	double VolumeInfluence = 0.0;

	UPROPERTY()
	bool bApplyPointScale = false;

	UPROPERTY()
	EPCGExBestFitPoolStrategy PoolStrategy = EPCGExBestFitPoolStrategy::TopK;

	UPROPERTY()
	FPCGExInputShorthandSelectorDouble PoolSize = FPCGExInputShorthandSelectorDouble(NAME_None, 3.0, false);

	virtual TSharedPtr<FPCGExEntryPickerOperation> CreateEntryOperation(FPCGExContext* InContext) const override;
};

/**
 * Palette node: "Selector : Best Fit".
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Collections|Distribution", meta=(PCGExNodeLibraryDoc="collections/selector/best-fit"))
class PCGEXCOLLECTIONS_API UPCGExSelectorBestFitFactoryProviderSettings : public UPCGExSelectorFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(
		SelectorBestFit, "Selector : Best Fit",
		"Pick entries whose bounds extents best match the per-point extent. Three metrics (volume / per-axis / aspect-ratio) with configurable pool strategy.",
		FName(GetDisplayName()))
#endif
	//~End UPCGSettings

	/** How entry extents score against the point's extent. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExBestFitMetric Metric = EPCGExBestFitMetric::ClosestVolume;

	/** Which axes contribute to the score. Ignored by ClosestVolume. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, Bitmask, BitmaskEnum="/Script/PCGExCollections.EPCGExAxisMask", EditCondition="Metric != EPCGExBestFitMetric::ClosestVolume", EditConditionHides))
	uint8 AxisMask = 7;

	/** How per-axis scores combine when more than one axis is selected. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="Metric != EPCGExBestFitMetric::ClosestVolume", EditConditionHides))
	EPCGExBestFitAxisAggregation AxisAggregation = EPCGExBestFitAxisAggregation::Sum;

	/** For ClosestAspectRatio: blends volume distance into the aspect score. 0 = pure aspect. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ClampMin=0, UIMin=0, UIMax=1, EditCondition="Metric == EPCGExBestFitMetric::ClosestAspectRatio", EditConditionHides))
	double VolumeInfluence = 0.0;

	/** If true, multiply the point's local extents by its transform scale before comparing. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	bool bApplyPointScale = false;

	/** Pool selection strategy. TopK uses a fixed-size pool; Tolerance pools all entries within a ratio of the best score. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExBestFitPoolStrategy PoolStrategy = EPCGExBestFitPoolStrategy::TopK;

	/** Pool size driver. Interpreted as K (rounded, clamped to [1, N]) when TopK, tolerance ratio when Tolerance. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExInputShorthandSelectorDouble PoolSize = FPCGExInputShorthandSelectorDouble(NAME_None, 3.0, false);

	/** Shared distribution configuration (seed, entry distribution, categories). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExSelectorFactoryBaseConfig BaseConfig;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
