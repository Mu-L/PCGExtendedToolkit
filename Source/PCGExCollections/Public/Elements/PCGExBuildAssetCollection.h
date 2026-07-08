// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGManagedResource.h"

#include "PCGExCoreMacros.h"
#include "PCGExCoreSettingsCache.h"
#include "Core/PCGExContext.h"
#include "Core/PCGExElement.h"
#include "Core/PCGExSettings.h"
#include "Details/PCGExRoamingAssetCollectionDetails.h"

#include "PCGExBuildAssetCollection.generated.h"

class UPCGExAssetCollection;

/**
 * PCG managed resource owning a transient UPCGExAssetCollection built by the Build Asset Collection node.
 *
 * Lifetime: the Collection UPROPERTY keeps the collection alive; the component's managed-resource set keeps
 * this alive. PCG marks all resources unused each (re)generation; the node re-marks the ones it still needs
 * (by CRC) and PCG Release()s the rest -- so stale configs drop and identical ones are reused. The CRC lives
 * on the base (SetCrc/GetCrc); Config holds the source config string to rule out CRC collisions on reuse.
 */
UCLASS()
class UPCGExManagedAssetCollection : public UPCGManagedResource
{
	GENERATED_BODY()

public:
	//~ Begin UPCGManagedResource interface
	virtual bool Release(bool bHardRelease, TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete) override;
	//~ End UPCGManagedResource interface

	/** The transient collection, kept alive by this strong reference for the resource's lifetime. */
	UPROPERTY(Transient)
	TObjectPtr<UPCGExAssetCollection> Collection = nullptr;

	/** The exact build-config string this collection was built from -- used to rule out CRC collisions on reuse. */
	UPROPERTY(Transient)
	FString Config;
};

/**
 * Build Asset Collection. Reads one input attribute set (asset path + optional weight/category attributes),
 * materializes a transient UPCGExAssetCollection of the configured type with baked staging data, and outputs
 * its soft object path so a Staging : Distribute node can consume it via its SourceCollection (Constant)
 * override pin -- exactly as if it were a saved collection asset. Inverse of the Asset Collection to Set node.
 *
 * The collection is anchored by a UPCGExManagedAssetCollection on the owning component, so identical
 * (attribute-set + config) inputs dedup to one managed resource by CRC and survive regeneration.
 *
 * Main-thread-only + non-cacheable: NewObject, managed-resource registration and the blocking staging load
 * all run on the game thread, and the reuse/mark-used pass must run every generation.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category = "PCGEx|Collections", meta = (PCGExNodeLibraryDoc = "collections/build-asset-collection"))
class UPCGExBuildAssetCollectionSettings : public UPCGExSettings
{
	GENERATED_BODY()

public:
	UPCGExBuildAssetCollectionSettings();

	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(BuildAssetCollection, "Build Asset Collection", "Builds a transient asset collection from an input attribute set and outputs its soft path for a Staging : Distribute SourceCollection (Constant) override.")

	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Generic; }

	virtual FLinearColor GetNodeTitleColor() const override { return PCGEX_NODE_COLOR_NAME(Misc); }
#endif

	// Non-cacheable so the managed-resource reuse/mark-used pass runs every generation (drives the PCGEx
	// element's IsCacheable via ShouldCache).
	virtual bool IsCacheable() const override { return false; }

	// Type the output pin's payload as a soft object path so it connects cleanly to soft-path override pins
	// (e.g. Staging : Distribute's SourceCollection/Constant).
	virtual FPCGDataTypeIdentifier GetCurrentPinTypesID(const UPCGPin* InPin) const override;
	//~End UPCGSettings

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;

public:
	/** How to interpret the input attribute set: which collection type to build, and which attributes hold the asset path / weight / category. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FPCGExRoamingAssetCollectionDetails AttributeSetDetails;

	/** Name of the output FSoftObjectPath attribute carrying the built collection's soft path. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FName OutputAttributeName = FName("Collection");
};

struct FPCGExBuildAssetCollectionContext final : FPCGExContext
{
};

class FPCGExBuildAssetCollectionElement final : public IPCGExElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(BuildAssetCollection)

	// NewObject, managed-resource registration and the blocking staging load are all game-thread only; run
	// the whole (lightweight) node there so a load can never deadlock under PCG cancellation.
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* Context) const override { return true; }

	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};
