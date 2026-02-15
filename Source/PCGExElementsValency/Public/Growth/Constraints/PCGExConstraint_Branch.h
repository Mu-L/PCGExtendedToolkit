// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "Core/PCGExValencyConnectorSet.h"

#include "PCGExConstraint_Branch.generated.h"

class UPCGExConstraintPreset;

/**
 * Branch constraint: conditional fork-join in the constraint pipeline.
 * Partitions the variant pool based on a filter condition, runs separate
 * sub-pipelines on each partition, then rejoins the results.
 */
USTRUCT(BlueprintType, DisplayName="Branch")
struct PCGEXELEMENTSVALENCY_API FPCGExConstraint_Branch : public FPCGExConnectorConstraint
{
	GENERATED_BODY()

	/** Condition filter: evaluated on each variant to partition the pool */
	UPROPERTY(EditAnywhere, Category = "Constraint", meta=(BaseStruct="/Script/PCGExElementsValency.PCGExConstraintFilter"))
	FInstancedStruct Condition;

	/** Sub-pipeline for variants that PASS the condition */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constraint")
	TSoftObjectPtr<UPCGExConstraintPreset> OnPass;

	/** Sub-pipeline for variants that FAIL the condition */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Constraint")
	TSoftObjectPtr<UPCGExConstraintPreset> OnFail;

	virtual EPCGExConstraintRole GetRole() const override { return EPCGExConstraintRole::Branch; }
};
