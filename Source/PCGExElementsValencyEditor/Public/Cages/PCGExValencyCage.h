// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExValencyCageBase.h"
#include "PCGExValencyEditorCommon.h"
#include "Core/PCGExValencyCommon.h"

#include "PCGExValencyCage.generated.h"

class UStaticMeshComponent;
class APCGExValencyAssetPalette;

/**
 * Abstract base for Valency cages that can register assets.
 * Subclasses must implement IsActorInside() for containment detection.
 *
 * Use APCGExValencyCageSimple for built-in shape-based containment,
 * or subclass this directly for custom containment logic.
 */
UCLASS(Abstract, HideCategories = (Rendering, Replication, Collision, HLOD, Physics, Networking, Input, LOD, Cooking))
class PCGEXELEMENTSVALENCYEDITOR_API APCGExValencyCage : public APCGExValencyCageBase
{
	GENERATED_BODY()

public:
	APCGExValencyCage();

	//~ Begin AActor Interface
	virtual void PostLoad() override;
	virtual void PostEditMove(bool bFinished) override;
	//~ End AActor Interface

	//~ Begin APCGExValencyCageBase Interface
	virtual FString GetCageDisplayName() const override;
	//~ End APCGExValencyCageBase Interface

	/** Get simple asset list (without transforms) for backward compatibility */
	TArray<TSoftObjectPtr<UObject>> GetRegisteredAssets() const;

	/** Manually register an asset (user-defined, persisted) */
	void RegisterManualAsset(const TSoftObjectPtr<UObject>& Asset, AActor* SourceActor = nullptr);

	/** Unregister a manually added asset */
	void UnregisterManualAsset(const TSoftObjectPtr<UObject>& Asset);

	/** Clear manually registered assets */
	void ClearManualAssets();

	/** Scan for assets within cage bounds and register them as scanned */
	void ScanAndRegisterContainedAssets();

public:
	/** Color for editor visualization (mirror connections, debug drawing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage")
	FLinearColor CageColor = FLinearColor(0.2f, 0.6f, 0.9f, 1.0f);

	/**
	 * Mirror sources - cages or asset palettes whose content this cage references.
	 * Each entry controls which content types (assets, connectors, properties, tags)
	 * to mirror and which to resolve recursively.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage|Mirror",
		meta=(TitleProperty="Source", PCGEX_ValencyGhostRefresh))
	TArray<FPCGExMirrorSource> MirrorSources;

	/**
	 * Whether to show ghost preview meshes when mirroring.
	 * Ghost meshes appear as translucent versions of the mirrored content.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage|Mirror")
	bool bShowMirrorGhostMeshes = true;

	/**
	 * Controls how the solver treats modules derived from this cage.
	 * - Normal: Standard participation with full constraints.
	 * - Filler: Only placed when no constrained module fits. Does not propagate constraints.
	 * - Excluded: Never placed by solver. Module exists for sockets/metadata only.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage|Module", meta=(PCGEX_ValencyRebuild))
	EPCGExModulePlacementPolicy PlacementPolicy = EPCGExModulePlacementPolicy::Normal;

	/**
	 * Optional name for modules created from this cage.
	 * Used for fixed picks - vertices with a matching FixedPick attribute value
	 * will be forced to use a module from this cage.
	 * Multiple cages can share the same name (selection uses weights).
	 * Empty = no name (cannot be fixed-picked by name).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage|Module", meta=(PCGEX_ValencyRebuild))
	FName ModuleName;

	/**
	 * Mark this cage as a template (boilerplate for connectors, properties, tags).
	 * Template cages are intentionally empty — they exist only to provide inheritable
	 * content to other cages via mirror sources. No module is created for them,
	 * and "no assets" warnings are suppressed in both build output and validation.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage|Module", meta=(PCGEX_ValencyRebuild))
	bool bIsTemplate = false;

	/**
	 * Default behavior for orbitals without explicit connections.
	 * Applied during BuildNeighborRelationships when an orbital has no connected cage.
	 * - Unconstrained: No constraint (accepts any neighbor or none) - current default
	 * - Boundary: Treat as boundary (must have NO neighbor at that orbital)
	 * - Wildcard: Treat as wildcard (must have ANY neighbor at that orbital)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cage|Module", AdvancedDisplay, meta = (PCGEX_ValencyRebuild))
	EPCGExMissingConnectionBehavior MissingConnectionBehavior = EPCGExMissingConnectionBehavior::Unconstrained;

	//~ Begin APCGExValencyCageBase Interface
	virtual void RefreshGhostMeshes() override;
	//~ End APCGExValencyCageBase Interface

	/**
	 * Trigger rebuild for cages that mirror this cage.
	 * Called when this cage's MirrorSources changes to cascade the update.
	 * @return True if any rebuild was triggered
	 */
	bool TriggerAutoRebuildForMirroringCages();

	//~ Begin APCGExValencyCageBase Interface
	virtual void OnPostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End APCGExValencyCageBase Interface

protected:
	//~ Begin APCGExValencyAssetContainerBase Interface
	virtual void OnAssetRegistrationChanged() override;
	//~ End APCGExValencyAssetContainerBase Interface
};
