// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "StructUtils/InstancedStruct.h"
#include "Core/PCGExValencyConnectorSet.h"

#include "PCGExConstraintPreset.generated.h"

/**
 * Reusable constraint pipeline fragment as a data asset.
 * Contains a list of constraints that can be inline-expanded by the resolver.
 */
UCLASS(BlueprintType, DisplayName="[PCGEx] Valency | Constraint Preset")
class PCGEXELEMENTSVALENCY_API UPCGExConstraintPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Constraints in this preset (executed in list order when expanded) */
	UPROPERTY(EditAnywhere, Category = "Constraints", meta=(BaseStruct="/Script/PCGExElementsValency.PCGExConnectorConstraint", ExcludeBaseStruct))
	TArray<FInstancedStruct> Constraints;
};

/**
 * Constraint that expands to the contents of a Constraint Preset data asset.
 * Flattened during the pre-pass before pipeline execution.
 */
USTRUCT(BlueprintType, DisplayName="Preset")
struct PCGEXELEMENTSVALENCY_API FPCGExConstraint_Preset : public FPCGExConnectorConstraint
{
	GENERATED_BODY()

	/** Constraint preset asset to expand inline */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constraint")
	TSoftObjectPtr<UPCGExConstraintPreset> Preset;

	virtual EPCGExConstraintRole GetRole() const override { return EPCGExConstraintRole::Preset; }
};
