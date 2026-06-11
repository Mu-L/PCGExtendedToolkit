// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "PCGExCollectionStagingPipeline.generated.h"

class UPCGExAssetCollection;

/**
 * Editor-only post-process pipeline for asset-collection staging rebuilds.
 *
 * Subclass in C++ or Blueprint and assign to UPCGExAssetCollection::StagingPipeline to
 * post-process entry data (property overrides, tags, weights, ...) whenever the collection's
 * staging data is rebuilt in the editor. Instanced via Instanced/EditInlineNew so derived
 * classes can expose their own UPROPERTYs directly in the collection's details panel.
 *
 * Hook contract:
 *  - Fires for every editor rebuild session: toolbar Rebuild / Rebuild Recursive / Rebuild
 *    Project, single-entry restages (grid edits), stale-entry batches and property-edit
 *    triggered rebuilds. Pre/post fire once per session; OnProcessEntry fires once per
 *    restaged entry (Sanitize -> UpdateStaging -> PostUpdateStaging -> OnProcessEntry).
 *  - OnPostRebuild runs AFTER the native EDITOR_OnPostStagingRebuild extension point (actor
 *    component schema merges, shared-collection compaction), so the pipeline gets final say.
 *  - Subcollections recursed into via the runtime RebuildStagingData do NOT run their own
 *    pipelines; only the collection whose editor rebuild was triggered dispatches hooks.
 *  - Rebuilds triggered from inside a hook run WITHOUT re-firing hooks (no recursion).
 *  - Hooks never fire while cooking.
 *
 * Mutate entry VALUES only from OnProcessEntry -- never add or remove entries there, the
 * entry array is being iterated live. Structural changes belong in OnPreRebuild/OnPostRebuild.
 *
 * Events are declared unconditionally so Blueprint subclasses stay cook-safe, but they are
 * only ever invoked from WITH_EDITOR rebuild paths (the collection-side StagingPipeline
 * pointer is editor-only data and never exists in cooked targets).
 */
UCLASS(Abstract, Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced)
class PCGEXCOLLECTIONS_API UPCGExCollectionStagingPipeline : public UObject
{
	GENERATED_BODY()

public:
	/** Fired once per rebuild session, before any entry is re-staged. */
	UFUNCTION(BlueprintNativeEvent, Category = "PCGEx|Collection|Staging")
	void OnPreRebuild(UPCGExAssetCollection* Collection);

	virtual void OnPreRebuild_Implementation(UPCGExAssetCollection* Collection)
	{
	}

	/** Fired for each re-staged entry, after its UpdateStaging + PostUpdateStaging. */
	UFUNCTION(BlueprintNativeEvent, Category = "PCGEx|Collection|Staging")
	void OnProcessEntry(UPCGExAssetCollection* Collection, int32 EntryIndex);

	virtual void OnProcessEntry_Implementation(UPCGExAssetCollection* Collection, int32 EntryIndex)
	{
	}

	/** Fired once per rebuild session, after every entry has been re-staged AND the native
	 *  EDITOR_OnPostStagingRebuild extension point has run (schema merges, compaction). */
	UFUNCTION(BlueprintNativeEvent, Category = "PCGEx|Collection|Staging")
	void OnPostRebuild(UPCGExAssetCollection* Collection);

	virtual void OnPostRebuild_Implementation(UPCGExAssetCollection* Collection)
	{
	}
};
