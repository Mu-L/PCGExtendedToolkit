// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PCGExProperty.h"

#include "PCGExPropertySchemaAsset.generated.h"

class FDataValidationContext;

/**
 * Reusable schema asset.
 *
 * Wraps an FPCGExPropertySchemaCollection so other collections can pull in a shared
 * set of property definitions through their own ImportedSchemas array. Because the
 * wrapped collection itself carries an ImportedSchemas array, composition is recursive.
 *
 * Resolution semantics (when imported into another collection):
 * - Name-keyed, first-wins. Locals in the importing collection beat imports;
 *   earlier entries in ImportedSchemas beat later ones.
 * - Walk order is depth-first: importing collection's locals first, then each
 *   ImportedSchemas entry expanded in array order (locals of the imported
 *   collection, then its own imports, etc.).
 * - Cycles are detected during resolution and skipped with a LogPCGEx warning.
 *   IsDataValid surfaces them in the editor as a Warning result.
 */
UCLASS(BlueprintType)
class PCGEXPROPERTIES_API UPCGExPropertySchemaAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Schemas (and further imports) carried by this asset. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Settings)
	FPCGExPropertySchemaCollection Collection;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
