// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPropertyWriter.h"
#include "Core/PCGExPointsProcessor.h"
#include "Data/Utils/PCGExDataFilterDetails.h"
#include "Elements/Grammar/PCGSubdivisionBase.h"
#include "Elements/PCGExAssetCollectionToSet.h"
#include "Helpers/PCGExCollectionsHelpers.h"
#include "PCGExGetCollectionData.generated.h"

class UPCGExAssetCollection;
struct FPCGExAssetCollectionEntry;

UENUM()
enum class EPCGExGetCollectionDataSourceMode : uint8
{
	Collection = 0 UMETA(DisplayName = "Collection", Tooltip="Use a single collection asset selected on this node."),
	FromInputs = 1 UMETA(DisplayName = "From Inputs", Tooltip="Resolve collections from input attribute sets / points (one or many)."),
};

UENUM()
enum class EPCGExGetCollectionDataSourceShape : uint8
{
	SoftPath      = 0 UMETA(DisplayName = "Soft Object Path", Tooltip="Read collection references as FSoftObjectPath attributes."),
	EntryIdAndMap = 1 UMETA(DisplayName = "Entry Id + Map", Tooltip="Read int64 entry hashes resolved against an upstream Collection Map. Each resolved entry must be a sub-collection; leaves produce an empty output."),
};

UENUM()
enum class EPCGExGetCollectionDataFanout : uint8
{
	PerEntry     = 0 UMETA(DisplayName = "Per Entry", Tooltip="Emit one attribute set per row across all input data. Output count = total rows."),
	PerInputData = 1 UMETA(DisplayName = "Per Input Data", Tooltip="Emit one attribute set per input data. The source attribute MUST live on the @Data domain (single value per data)."),
	Merged       = 2 UMETA(DisplayName = "Merged", Tooltip="Emit a single attribute set containing entries from every unique resolved collection, appended in encounter order. No entry-level deduplication (caller's responsibility)."),
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), meta=(PCGExNodeLibraryDoc="staging/utilities/get-collection-data"))
class UPCGExGetCollectionDataSettings : public UPCGExSettings
{
	GENERATED_BODY()

	friend class FPCGExGetCollectionDataElement;

	//~Begin UObject interface
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~End UObject interface

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(GetCollectionData, "Get Collection Data", "Unified read of asset collection contents into an attribute set. Supports static asset selection, soft-path-driven inputs, and recursive grammar via upstream Collection Maps.");

	virtual EPCGSettingsType GetType() const override
	{
		return EPCGSettingsType::Param;
	}

	virtual bool CanDynamicallyTrackKeys() const override
	{
		return true;
	}
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

public:
	// Data settings are public (BlueprintReadWrite anyway) so that helper functions outside the
	// element class -- e.g. PCGExGetCollectionData::ProcessUniqueOutput -- can read them without
	// needing a friend declaration.


	// Source

	/** Where collections come from: a single asset on this node, or resolved from input data. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Source", meta=(PCG_NotOverridable))
	EPCGExGetCollectionDataSourceMode SourceMode = EPCGExGetCollectionDataSourceMode::Collection;

	/** The asset collection to read from. Hard reference -- the asset is guaranteed loaded by the time this node executes. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Source", meta=(PCG_Overridable, EditCondition="SourceMode == EPCGExGetCollectionDataSourceMode::Collection", EditConditionHides))
	TObjectPtr<UPCGExAssetCollection> AssetCollection;

	/** How the source attribute is interpreted on each input row. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Source", meta=(PCG_NotOverridable, EditCondition="SourceMode == EPCGExGetCollectionDataSourceMode::FromInputs", EditConditionHides))
	EPCGExGetCollectionDataSourceShape SourceShape = EPCGExGetCollectionDataSourceShape::SoftPath;

	/** Name of the attribute holding the collection reference (soft path or entry hash). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Source", meta=(PCG_Overridable, EditCondition="SourceMode == EPCGExGetCollectionDataSourceMode::FromInputs", EditConditionHides))
	FName SourceAttribute = PCGExCollections::Labels::Tag_EntryIdx;

	/** How many outputs to emit per input. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Source", meta=(PCG_NotOverridable, EditCondition="SourceMode == EPCGExGetCollectionDataSourceMode::FromInputs", EditConditionHides))
	EPCGExGetCollectionDataFanout Fanout = EPCGExGetCollectionDataFanout::PerInputData;

	/** Forward source input data tags to the corresponding output attribute set. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Source", meta=(PCG_Overridable, EditCondition="SourceMode == EPCGExGetCollectionDataSourceMode::FromInputs", EditConditionHides))
	bool bForwardInputTags = false;


	// Recursion / filtering

	/** How sub-collection entries are handled during flattening. Grammar defers to each entry's own SubGrammarMode (Flatten recurses, Inherit/Override emit one row). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Recursion", meta=(PCG_Overridable))
	EPCGExSubCollectionToSet SubCollectionHandling = EPCGExSubCollectionToSet::Grammar;

	/** Allow duplicate entries (same entry pointer) to appear in the output. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Recursion", meta=(PCG_Overridable))
	bool bAllowDuplicates = true;

	/** Drop entries whose resolved Symbol is None (only relevant when Symbol output is enabled). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Recursion", meta=(PCG_Overridable))
	bool bSkipEmptySymbol = true;

	/** Drop entries that resolve to invalid / empty data instead of keeping them as placeholders. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Recursion", meta=(PCG_Overridable))
	bool bOmitInvalidAndEmpty = true;

	/** Optionally include/exclude entries by category name. Tested against both leaf entries and
	 *  sub-collection containers; excluding a sub-collection skips its descendants entirely. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Recursion", meta=(PCG_Overridable))
	FPCGExNameFiltersDetails CategoryFilters;


	// Asset outputs

	/** Write the asset path. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteAssetPath = true;

	UPROPERTY(meta=(PCG_NotOverridable))
	bool bWriteAssetClass = true;

	/** Name of the attribute the asset path is written to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="Asset Path", EditCondition="bWriteAssetPath"))
	FName AssetPathAttributeName = FName("AssetPath");

	UPROPERTY()
	FName AssetClassAttributeName = NAME_None;

	/** Write the asset weight. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteWeight = true;

	/** Name of the attribute the asset weight is written to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="Weight", EditCondition="bWriteWeight"))
	FName WeightAttributeName = FName("Weight");

	/** How (and whether) to normalize the weight value. When set to anything other than None, the
	 *  Weight attribute is written as a float in [0..1] instead of the raw int32. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName = " └─ Normalize", EditCondition="bWriteWeight", HideEditConditionToggle))
	EPCGExWeightNormalization WeightNormalization = EPCGExWeightNormalization::None;

	/** Write the asset category. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteCategory = false;

	/** Name of the attribute the asset category is written to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="Category", EditCondition="bWriteCategory"))
	FName CategoryAttributeName = FName("Category");

	/** How (and whether) to fold parent sub-collection categories down into descendant entries during
	 *  expansion. Also applies to Per Category weight normalization. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName = " └─ Inheritance", EditCondition="bWriteCategory", HideEditConditionToggle))
	EPCGExCategoryInheritance CategoryInheritance = EPCGExCategoryInheritance::None;

	/** Write the asset bounds extents. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteExtents = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="Extents", EditCondition="bWriteExtents"))
	FName ExtentsAttributeName = FName("Extents");

	/** Write the asset minimum bounds. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteBoundsMin = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="BoundsMin", EditCondition="bWriteBoundsMin"))
	FName BoundsMinAttributeName = FName("BoundsMin");

	/** Write the asset maximum bounds. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteBoundsMax = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="BoundsMax", EditCondition="bWriteBoundsMax"))
	FName BoundsMaxAttributeName = FName("BoundsMax");

	/** Write the asset nesting depth (0 = top-level). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteNestingDepth = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Asset", meta=(PCG_Overridable, DisplayName="Nesting Depth", EditCondition="bWriteNestingDepth"))
	FName NestingDepthAttributeName = FName("NestingDepth");


	// Grammar outputs

	/** Write the resolved grammar Symbol. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteSymbol = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, DisplayName="Symbol", EditCondition="bWriteSymbol"))
	FName SymbolAttributeName = FName("Symbol");

	/** Write the resolved grammar Size. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteSize = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, DisplayName="Size", EditCondition="bWriteSize"))
	FName SizeAttributeName = FName("Size");

	/** Write the resolved grammar Scalable flag. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteScalable = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, DisplayName="Scalable", EditCondition="bWriteScalable"))
	FName ScalableAttributeName = FName("Scalable");

	/** Write the resolved grammar DebugColor. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bWriteDebugColor = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs|Grammar", meta=(PCG_Overridable, DisplayName="DebugColor", EditCondition="bWriteDebugColor"))
	FName DebugColorAttributeName = FName("DebugColor");


	// Always-on outputs

	/** Name of the entry hash attribute. Always written; downstream consumers (LoadPCGData, LoadProperties, etc.) read this fixed name. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Settings|Outputs", meta=(DisplayName="Entry"))
	FName EntryAttributeName = PCGExCollections::Labels::Tag_EntryIdx;


	// Custom properties

	/** Additional custom properties to extract from each collection's CollectionProperties (and per-entry overrides). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Outputs", meta=(DisplayName="Properties"))
	FPCGExPropertyOutputSettings PropertyOutputSettings;
};

/**
 * Per-element context. Carries state across Boot → PostLoadAssetsDependencies → AdvanceWork.
 * Lets us register asset dependencies in Boot, resolve them in PostLoadAssetsDependencies (after
 * the framework's async load completes), and run the actual work in AdvanceWork without any
 * blocking loads on the hot path.
 */
struct FPCGExGetCollectionDataContext final : FPCGExContext
{
	friend class FPCGExGetCollectionDataElement;

	/** Soft path -> resolved collection. Populated in PostLoadAssetsDependencies. */
	TMap<FSoftObjectPath, UPCGExAssetCollection*> ResolvedCollections;

	/** One entry per planned output slot. Paths are resolved into ResolvedCollections lazily in PostLoad. */
	struct FSlot
	{
		FSoftObjectPath Path;                // empty path == empty/leaf-failed slot
		int32 SourceInputIndex = INDEX_NONE; // index into the SourcesPin input list, or INDEX_NONE for Collection mode
	};

	TArray<FSlot> Slots;
};

class FPCGExGetCollectionDataElement final : public IPCGExElement
{
public:
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override;

protected:
	virtual FPCGContext* CreateContext() override
	{
		return new FPCGExGetCollectionDataContext();
	}

	/** Pin to main thread: lets LoadBlocking_AnyThread take the IsInGameThread() short-circuit
	 *  instead of dispatching to game thread and waiting for the next tick, which removes the
	 *  worker->main roundtrip latency from cold-load runs. The actual work the node does is
	 *  minimal (orchestration + attribute writes), so we don't lose much by running on main. */
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* Context) const override
	{
		return true;
	}

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};
