// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGData.h"
#include "Components/ActorComponent.h"
#include "Misc/TransactionallySafeRWLock.h"
#include "UObject/SoftObjectPath.h"

#include "PCGExDataCacheComponent.generated.h"

UENUM()
enum class EPCGExDataCacheWriteMode : uint8
{
	Replace  = 0 UMETA(DisplayName = "Replace", Tooltip = "Replace whatever is stored under the cache ID with the input data."),
	Append   = 1 UMETA(DisplayName = "Append", Tooltip = "Append the input data to whatever is already stored under the cache ID."),
	Clear    = 2 UMETA(DisplayName = "Clear", Tooltip = "Remove the entry stored under the cache ID."),
	ClearAll = 3 UMETA(DisplayName = "Clear All", Tooltip = "Remove every entry on the target cache. Cache ID is ignored."),
};

/** One cached collection. Data objects are outered to the owning cache component so they serialize with the actor. */
USTRUCT()
struct PCGEXELEMENTSBRIDGES_API FPCGExDataCacheEntry
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Data Cache")
	FPCGDataCollection Data;

	/** Diagnostic: execution source (usually a PCG component) that last wrote this entry. */
	UPROPERTY(VisibleAnywhere, Category = "Data Cache")
	FSoftObjectPath Writer;
};

/**
 * Holds PCG data collections under named IDs, persisted with the owning actor the same way a PCG component
 * persists its generated output. One per actor (see Find / FindOrCreate), created only by Set Cached Data.
 * Never a PCG managed resource: cleanup and regeneration leave it alone, so a refresh can read back what a
 * previous pass stored.
 *
 * Only self-contained data is cached (point, spline, volume, attribute sets...). Composites whose network
 * reaches other data objects (unions, intersections, projections) are refused: they would either steal live
 * upstream objects mid-execution or serialize with null sources.
 *
 * Reads are safe from any thread. Writes are game-thread only: adopting data re-outers it (Rename) and
 * flattens it (Modify), neither of which is thread-safe.
 */
UCLASS(ClassGroup = (Procedural), meta = (DisplayName = "PCGEx Data Cache"))
class PCGEXELEMENTSBRIDGES_API UPCGExDataCacheComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPCGExDataCacheComponent();

	/** The actor's cache component, or null. Also finds instance components a level copy-paste left unregistered. */
	static UPCGExDataCacheComponent* Find(const AActor* InActor);

	/**
	 * The actor's cache component, created as an instance component if missing. Game thread only.
	 * bTransient (preview-mode writers) creates an RF_Transient component; a later persistent write promotes it.
	 */
	static UPCGExDataCacheComponent* FindOrCreate(AActor* InActor, const bool bTransient);

	/** Copies the entry stored under InId (preview entries shadow persisted ones). Any thread. */
	bool Read(const FName InId, TArray<FPCGTaggedData>& OutData) const;

	/** Copies every entry, preview entries shadowing persisted ones with the same ID. Any thread. */
	void ReadAll(TArray<TPair<FName, TArray<FPCGTaggedData>>>& OutEntries) const;

	/**
	 * Applies InMode under InId. Game thread only. For Replace / Append, every data object must be a private
	 * duplicate outered to the transient package: it is flattened and re-outered to this component (composites
	 * are refused, see class comment). bPreview confines the write to the transient preview map (data flagged
	 * RF_Transient, package never dirtied); a persistent write evicts the preview shadow of the same ID.
	 * bNotify: see NotifyChanged.
	 */
	void Write(const FName InId, const EPCGExDataCacheWriteMode InMode, TArray<FPCGTaggedData>&& InData, const UObject* InWriter, const bool bPreview, const bool bNotify = false);

	/** Drops the entry under InId; from the preview map only when bPreview. Game thread only. Notifies only if something was removed. */
	void Clear(const FName InId, const UObject* InWriter, const bool bPreview, const bool bNotify);

	/** Drops every entry; from the preview map only when bPreview. Game thread only. Notifies only if something was removed. */
	void ClearAll(const UObject* InWriter, const bool bPreview, const bool bNotify);

	/**
	 * Editor only, no-op otherwise. Broadcasts the standard object-changed pair so PCG components tracking the
	 * owner actor dirty and refresh. InWriter's original PCG component (if any) is bracketed with
	 * StartIgnoringChangeOriginDuringGeneration for the synchronous dispatch, so a writer never refreshes itself.
	 */
	void NotifyChanged(const UObject* InWriter) const;

#if WITH_EDITOR
	UFUNCTION(CallInEditor, Category = "Data Cache", meta = (DisplayName = "Clear Cache", ShortToolTip = "Remove every cached entry from this component. Not undoable."))
	void EDITOR_ClearCache();
#endif

protected:
	/** Persisted entries. */
	UPROPERTY(VisibleAnywhere, Category = "Data Cache")
	TMap<FName, FPCGExDataCacheEntry> Entries;

	/** Entries written by preview-mode generations. Never saved; shadow persisted entries on read. */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Data Cache")
	TMap<FName, FPCGExDataCacheEntry> PreviewEntries;

	/** Guards both maps. Held only around map access, never around Rename/Flatten. */
	mutable FTransactionallySafeRWLock Lock;

	/** Re-outers every data object this component owns in InData back to the transient package so GC can reclaim it. */
	void ReleaseData(const FPCGDataCollection& InData) const;

	/** Flattens and re-outers each adoptable data object to this component. Returns the data that was adopted. */
	TArray<FPCGTaggedData> AdoptData(TArray<FPCGTaggedData>&& InData, const bool bPreview) const;
};
