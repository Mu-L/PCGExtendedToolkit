// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

#include "Core/PCGExAssetCollection.h"
#include "Helpers/PCGExArrayHelpers.h"

#include "PCGExOmniCollection.generated.h"

/**
 * One row of an Omni collection: a thin wrapper around a type-erased entry payload.
 * Wrapped (rather than a raw TArray<FInstancedStruct>) so future row-level metadata can be
 * added without a serialization break.
 *
 * An unset payload is a tolerated authoring state: the row still consumes its raw index
 * (skipped by iteration and cache build) so indices stay stable under partial authoring --
 * same contract as Variant collection rows.
 */
USTRUCT(BlueprintType, DisplayName="[PCGEx] Omni Collection Entry")
struct PCGEXCOLLECTIONS_API FPCGExOmniCollectionEntry
{
	GENERATED_BODY()

	/** Entry payload. Any concrete FPCGExAssetCollectionEntry type -- rows of different types coexist. */
	UPROPERTY(EditAnywhere, Category = Settings, meta=(BaseStruct="/Script/PCGExCollections.PCGExAssetCollectionEntry", ExcludeBaseStruct, ShowOnlyInnerProperties))
	FInstancedStruct Entry;

	FPCGExAssetCollectionEntry* GetPayload()
	{
		return Entry.GetMutablePtr<FPCGExAssetCollectionEntry>();
	}

	const FPCGExAssetCollectionEntry* GetPayload() const
	{
		return Entry.GetPtr<FPCGExAssetCollectionEntry>();
	}
};

/**
 * Type-erased asset collection: entries of ANY registered type coexist in one authored list
 * (mesh + actor + level + data asset + custom third-party types).
 *
 * It is a first-class UPCGExAssetCollection -- pick transport is type-blind and staged
 * consumers dispatch per entry, so everything downstream (staging, fitting, selectors,
 * spawning) works off the payloads directly. Consumers that need a specific type simply
 * skip foreign entries (or route per type through Staging Type Filter).
 *
 * Collection-level settings that typed collections expose (global descriptors, bounds
 * evaluators, content filters, level exporter) are provided through TypeGlobals blocks:
 * entries query them via the type-globals seam (UPCGExAssetCollection::GetTypeGlobals),
 * so an Omni host with a matching block behaves exactly like the native typed collection.
 */
UCLASS(BlueprintType, DisplayName="[PCGEx] Collection | Omni", meta=(ToolTip = "A weighted collection of mixed entry types -- meshes, actors, levels, data assets and custom types in a single list."))
class PCGEXCOLLECTIONS_API UPCGExOmniCollection : public UPCGExAssetCollection
{
	GENERATED_BODY()

public:
	// Type System
	virtual PCGExAssetCollection::FTypeId GetTypeId() const override
	{
		return PCGExAssetCollection::TypeIds::Omni;
	}

	// Entries Array
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TArray<FPCGExOmniCollectionEntry> Entries;

	/**
	 * Per-type collection-level globals blocks (see FPCGExCollectionTypeGlobals). One block
	 * per entry type at most -- the first block whose struct matches a query wins. Entries
	 * whose type has no block here fall back to their entry-local settings, exactly as if
	 * the matching typed collection had default globals.
	 *
	 * NOTE: blocks may carry Instanced subobjects (bounds evaluators, content filters).
	 * Raw FInstancedStruct copies are SHALLOW for those -- cross-asset copies must
	 * DuplicateObject the subobjects into the destination asset.
	 */
	UPROPERTY(EditAnywhere, Category = "Settings|Global", meta=(BaseStruct="/Script/PCGExCollections.PCGExCollectionTypeGlobals", ExcludeBaseStruct))
	TArray<FInstancedStruct> TypeGlobals;

	//~ UPCGExAssetCollection storage interface -- rows wrap payloads, so the
	// PCGEX_ASSET_COLLECTION_BODY macro doesn't apply; virtuals resolve through the wrapper.
	virtual bool IsValidIndex(int32 InIndex) const override;
	virtual int32 NumEntries() const override;
	virtual void InitNumEntries(int32 InNum) override;
	virtual void BuildCache() override;
	virtual void ForEachEntry(FForEachConstEntryFunc Iterator) const override;
	virtual void ForEachEntry(FForEachEntryFunc Iterator) override;
	virtual void Sort(FSortEntriesFunc Predicate) override;

	/**
	 * Append a row whose payload is default-initialized as EntryStruct (must derive from
	 * FPCGExAssetCollectionEntry; the base struct itself is allowed -- that's what
	 * subcollection rows use). Returns the new payload, or null if EntryStruct is invalid.
	 * Caller owns Modify / MarkPackageDirty / staging rebuild.
	 */
	FPCGExAssetCollectionEntry* AddEntryOfType(const UScriptStruct* EntryStruct);

#if WITH_EDITOR
	/**
	 * Append every entry of the given source collections into this Omni (conversion when
	 * called on a fresh asset with one source; merge with several). Sources are untouched.
	 *
	 * Per source:
	 * - Entries are copied as payload rows of their exact type (works for typed AND
	 *   heterogeneous sources). Copies are new identities (EntryId re-minted on rebuild);
	 *   the source's CollectionTags are baked into each copied entry's Tags (matching
	 *   FlattenCollection semantics). Subcollection rows keep referencing their collection
	 *   asset (shared reference, not duplicated).
	 * - The source's globals become a TypeGlobals block (Instanced subobjects are
	 *   DuplicateObject'd into this asset -- raw copies would illegally share them). When a
	 *   block of that type already exists (multi-merge conflicts), the source's globals are
	 *   baked into its copied entries via ResolveGlobalsToLocal instead, so behavior is
	 *   preserved either way.
	 * - Entry property overrides ride along; the collection-level property schema is
	 *   re-derived from the merged entries afterwards.
	 *
	 * Wraps everything in Modify + staging rebuild. Returns the number of entries appended.
	 */
	int32 EDITOR_AppendCollections(TConstArrayView<UPCGExAssetCollection*> InSources);

	virtual void EDITOR_AddSubCollectionEntries(const TArray<UPCGExAssetCollection*>& InSubCollections) override;

	virtual const UScriptStruct* EDITOR_GetEntryScriptStruct(int32 RawIndex) const override;
	virtual void EDITOR_GetAddableEntryTypes(TArray<const UScriptStruct*>& OutTypes) const override;
	virtual FPCGExAssetCollectionEntry* EDITOR_AddEntry(const UScriptStruct* EntryStruct = nullptr) override;

	/** Runs the actor-component property scan over actor-typed entries, matching what a
	 *  native actor collection does (merge policy from the actor globals block, if any). */
	virtual void EDITOR_OnPostStagingRebuild() override;

protected:
	/**
	 * Routes each dropped asset to the highest-priority registered type whose
	 * DetectSourceAsset claims it (see FTypeInfo), then seeds a row via
	 * MakeEntryFromSourceAsset or the generic InitializeAs + SetAssetPath path.
	 * Assets already referenced by a row of the same entry type are skipped.
	 */
	virtual void EDITOR_AddBrowserSelectionInternal(const TArray<FAssetData>& InAssetData) override;

public:
#endif

protected:
	virtual bool GetTypeGlobalsInternal(const UScriptStruct* StructType, FPCGExCollectionTypeGlobals& OutGlobals) const override;

	virtual const FPCGExAssetCollectionEntry* GetEntryAtRawIndex(int32 Index) const override;
	virtual FPCGExAssetCollectionEntry* GetMutableEntryAtRawIndex(int32 Index) override;
};
