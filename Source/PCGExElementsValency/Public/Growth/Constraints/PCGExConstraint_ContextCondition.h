// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Core/PCGExValencyConnectorSet.h"
#include "Growth/PCGExConnectorConstraintResolver.h"
#include "Utils/PCGExCompare.h"

#include "PCGExConstraint_ContextCondition.generated.h"

/**
 * Which growth state property to evaluate.
 */
UENUM(BlueprintType)
enum class EPCGExContextProperty : uint8
{
	Depth              UMETA(DisplayName = "Depth",             ToolTip = "Distance from seed (0 = seed)"),
	CumulativeWeight   UMETA(DisplayName = "Cumulative Weight", ToolTip = "Sum of module weights from seed to here"),
	ModuleIndex        UMETA(DisplayName = "Module Index",      ToolTip = "Index of the child module being placed"),
	ConnectorIndex     UMETA(DisplayName = "Connector Index",   ToolTip = "Index of the child's connector"),
	PlacedCount        UMETA(DisplayName = "Placed Count",      ToolTip = "Total placed module count at this point")
};

/**
 * Filter constraint: evaluates a growth context property against a threshold.
 * Acts as an L-system-style condition — prunes all variants when the condition fails.
 */
USTRUCT(BlueprintType, DisplayName="Context Condition")
struct PCGEXELEMENTSVALENCY_API FPCGExConstraint_ContextCondition : public FPCGExConstraintFilter
{
	GENERATED_BODY()

	/** Which property of the growth context to evaluate */
	UPROPERTY(EditAnywhere, Category = "Constraint")
	EPCGExContextProperty Property = EPCGExContextProperty::Depth;

	/** How to compare the property value against the threshold */
	UPROPERTY(EditAnywhere, Category = "Constraint")
	EPCGExComparison Comparison = EPCGExComparison::StrictlySmaller;

	/** Threshold value to compare against */
	UPROPERTY(EditAnywhere, Category = "Constraint")
	double Threshold = 5.0;

	/** Tolerance for NearlyEqual/NearlyNotEqual comparisons */
	UPROPERTY(EditAnywhere, Category = "Constraint", meta=(EditCondition="Comparison==EPCGExComparison::NearlyEqual||Comparison==EPCGExComparison::NearlyNotEqual"))
	double Tolerance = 0.01;

	virtual bool IsValid(
		const FPCGExConstraintContext& Context,
		const FTransform& CandidateTransform) const override;
};
