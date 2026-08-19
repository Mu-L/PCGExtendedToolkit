// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Sketch/PCGExClusterSketchDecorator.h"
#include "Sketch/PCGExClusterSketchModel.h"
#include "Sketch/PCGExClusterSnapProvider.h"

#include "PCGExClusterSketch.generated.h"

/**
 * A hand-authored, spawnable cluster: the sketch model (vertices + edges + channels), an optional snap
 * provider, and print-time decorators. Print-on-demand by design -- the asset holds NO baked point data
 * and no derived state; a consumer prints a live Vtx/Edges pair from the model at execute time and
 * duplicates it per target.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category = "PCGEx")
class PCGEXGRAPHS_API UPCGExClusterSketch : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	FPCGExClusterSketchModel Model;

	/** Snap-lattice model this sketch is authored against. None = free-form (bound vertices then fall
	 *  back to their cached locations, with a print-time warning). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Instanced, Category = Settings)
	TObjectPtr<UPCGExClusterSnapProvider> SnapProvider;

	/** Print-time attribute decorators, run in order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Instanced, Category = Settings)
	TArray<TObjectPtr<UPCGExClusterSketchDecorator>> Decorators;

	/** Basis from the snap provider; false when there is no provider or it forms no usable lattice. */
	bool BuildBasis(FPCGExLatticeBasis& OutBasis) const;

	/** Union of the provider's and enabled decorators' soft dependencies -- load these before printing. */
	void CollectAssetDependencies(TArray<FSoftObjectPath>& OutPaths) const;

#if WITH_EDITOR
	/**
	 * Coord/position coherence (the sketch's one editing rule): a bound vertex's location is derived from
	 * its coord. Editing the COORD re-derives the location (coord wins); any other model edit re-snaps
	 * coords from locations first, so a hand-edited transform can never dangle off-lattice.
	 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Undo restores model+coords wholesale (no derived state on this asset); the sync is a cheap
	 *  idempotent belt-and-braces for provider-only transactions. */
	virtual void PostEditUndo() override;

	/** Provider params moved the lattice: coords stay authoritative, bound locations re-derive. */
	void EDITOR_OnSnapProviderChanged();

	/** See PostEditChangeProperty. No-op without a usable basis. */
	void EDITOR_SyncBoundVertices(bool bResnapFromLocation);
#endif
};
