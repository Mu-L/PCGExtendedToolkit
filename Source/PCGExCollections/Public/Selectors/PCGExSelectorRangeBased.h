// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Details/PCGExInputShorthandsDetails.h"
#include "Selectors/PCGExSelectorFactoryProvider.h"
#include "Selectors/PCGExEntryPickerOperation.h"

#include "PCGExSelectorRangeBased.generated.h"

/** How per-entry Min/Max are sourced from CollectionProperties. */
UENUM()
enum class EPCGExRangeSourceMode : uint8
{
	TwoNumerics = 0 UMETA(DisplayName = "Two Numerics", ToolTip="Pick two numeric properties (Double/Float/Int) for Min and Max."),
	Vector2     = 1 UMETA(DisplayName = "Vector2", ToolTip="Pick a single Vector2D/Vector property; X is Min, Y is Max."),
};

/** What to do when a point's value falls inside multiple entries' ranges. */
UENUM()
enum class EPCGExRangeOverlapMode : uint8
{
	WeightedRandom = 0 UMETA(DisplayName = "Weighted Random", ToolTip="Pick weighted-random by Entry.Weight among all entries whose range contains the value."),
	FirstMatch     = 1 UMETA(DisplayName = "First Match", ToolTip="Pick the first entry (in category order) whose range contains the value. Weight is ignored."),
	NarrowestWins  = 2 UMETA(DisplayName = "Narrowest Wins", ToolTip="Pick the entry with the smallest (Max - Min) among matches. Weight breaks ties on equal widths."),
};

/** Inclusivity at range boundaries. */
UENUM()
enum class EPCGExRangeBoundaryMode : uint8
{
	ClosedClosed = 0 UMETA(DisplayName = "[Min, Max]", ToolTip="Both endpoints match."),
	ClosedOpen   = 1 UMETA(DisplayName = "[Min, Max)", ToolTip="Min matches, Max does not. Bucket-style — disambiguates shared endpoints."),
	OpenClosed   = 2 UMETA(DisplayName = "(Min, Max]", ToolTip="Min does not match, Max does."),
	OpenOpen     = 3 UMETA(DisplayName = "(Min, Max)", ToolTip="Neither endpoint matches."),
};

/**
 * Shared base for Range-Based picker operations. Holds the per-entry resolved Min/Max
 * arrays and the per-point value getter. Concrete subclasses implement Pick() according
 * to their overlap policy.
 *
 * Resolution happens once at PrepareForData and writes EntryMins / EntryMaxs parallel
 * to Target->Entries. The hot path never re-reads property metadata.
 */
class FPCGExEntryRangeBasedPickerOpBase : public FPCGExEntryPickerOperation
{
public:
	// Copied from factory before PrepareForData
	EPCGExRangeSourceMode SourceMode = EPCGExRangeSourceMode::TwoNumerics;
	EPCGExRangeBoundaryMode BoundaryMode = EPCGExRangeBoundaryMode::ClosedOpen;
	FPCGExInputShorthandSelectorDouble ValueSource;
	FName MinPropertyName = NAME_None;
	FName MaxPropertyName = NAME_None;
	FName RangePropertyName = NAME_None;

	// Resolved at PrepareForData — parallel to Target->Entries
	TArray<double> EntryMins;
	TArray<double> EntryMaxs;

	TSharedPtr<PCGExDetails::TSettingValue<double>> ValueGetter;

	virtual bool PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection) override;
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override = 0;

protected:
	/** Apply BoundaryMode to a single (Value, Min, Max) check. */
	FORCEINLINE bool Contains(double V, double Min, double Max) const
	{
		switch (BoundaryMode)
		{
		default:
		case EPCGExRangeBoundaryMode::ClosedOpen:   return V >= Min && V <  Max;
		case EPCGExRangeBoundaryMode::ClosedClosed: return V >= Min && V <= Max;
		case EPCGExRangeBoundaryMode::OpenClosed:   return V >  Min && V <= Max;
		case EPCGExRangeBoundaryMode::OpenOpen:     return V >  Min && V <  Max;
		}
	}
};

/** Weighted-random pick among entries whose range contains the value. */
class FPCGExEntryRangeWeightedRandomPickerOp : public FPCGExEntryRangeBasedPickerOpBase
{
public:
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override;
};

/** First (in category order) entry whose range contains the value. */
class FPCGExEntryRangeFirstMatchPickerOp : public FPCGExEntryRangeBasedPickerOpBase
{
public:
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override;
};

/** Narrowest (smallest Max - Min) entry whose range contains the value; weighted-random on width ties. */
class FPCGExEntryRangeNarrowestPickerOp : public FPCGExEntryRangeBasedPickerOpBase
{
public:
	virtual int32 Pick(int32 PointIndex, int32 Seed) const override;
};

/**
 * Factory data for Range-Based selection. Per-entry ranges are sourced from
 * CollectionProperties (either two numeric properties for Min/Max, or a single
 * Vector2D property where X=Min, Y=Max). Per-point scalar value drives the pick.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXCOLLECTIONS_API UPCGExSelectorRangeBasedFactoryData : public UPCGExSelectorFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExInputShorthandSelectorDouble ValueSource = FPCGExInputShorthandSelectorDouble(FName("$Density"), 1.0, true);

	UPROPERTY()
	EPCGExRangeSourceMode SourceMode = EPCGExRangeSourceMode::TwoNumerics;

	UPROPERTY()
	FName MinPropertyName = NAME_None;

	UPROPERTY()
	FName MaxPropertyName = NAME_None;

	UPROPERTY()
	FName RangePropertyName = NAME_None;

	UPROPERTY()
	EPCGExRangeBoundaryMode BoundaryMode = EPCGExRangeBoundaryMode::ClosedOpen;

	UPROPERTY()
	EPCGExRangeOverlapMode OverlapMode = EPCGExRangeOverlapMode::WeightedRandom;

	virtual TSharedPtr<FPCGExEntryPickerOperation> CreateEntryOperation(FPCGExContext* InContext) const override;
};

/**
 * Palette node: "Selector : Range-Based".
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Collections|Distribution", meta=(PCGExNodeLibraryDoc="collections/selector/range-based"))
class PCGEXCOLLECTIONS_API UPCGExSelectorRangeBasedFactoryProviderSettings : public UPCGExSelectorFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(
		SelectorRangeBased, "Selector : Range-Based",
		"Pick entries whose authored range contains a per-point value. Supports two source modes and three overlap policies.",
		FName(GetDisplayName()))
#endif
	//~End UPCGSettings

	/** Per-point value matched against each entry's authored range. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExInputShorthandSelectorDouble ValueSource = FPCGExInputShorthandSelectorDouble(FName("$Density"), 1.0, true);

	/** How per-entry Min/Max are sourced from CollectionProperties. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExRangeSourceMode SourceMode = EPCGExRangeSourceMode::TwoNumerics;

	/** Name of the numeric property that defines each entry's Min. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="SourceMode == EPCGExRangeSourceMode::TwoNumerics", EditConditionHides))
	FName MinPropertyName = NAME_None;

	/** Name of the numeric property that defines each entry's Max. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="SourceMode == EPCGExRangeSourceMode::TwoNumerics", EditConditionHides))
	FName MaxPropertyName = NAME_None;

	/** Name of the Vector2D/Vector property whose X=Min, Y=Max. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="SourceMode == EPCGExRangeSourceMode::Vector2", EditConditionHides))
	FName RangePropertyName = NAME_None;

	/** Inclusivity at range boundaries. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExRangeBoundaryMode BoundaryMode = EPCGExRangeBoundaryMode::ClosedOpen;

	/** What to do when the point value falls in multiple entries' ranges. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExRangeOverlapMode OverlapMode = EPCGExRangeOverlapMode::WeightedRandom;

	/** Shared distribution configuration (seed, entry distribution, categories). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExSelectorFactoryBaseConfig BaseConfig;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
