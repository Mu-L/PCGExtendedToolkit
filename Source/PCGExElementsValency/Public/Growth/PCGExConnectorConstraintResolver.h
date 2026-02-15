// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

struct FPCGExOpenConnector;
struct FPCGExConnectorConstraint;
class UPCGExValencyConnectorSet;
enum class EPCGExConstraintRole : uint8;

/**
 * Context passed to constraint evaluation methods.
 * Contains all the information a constraint needs to generate/modify/filter transforms.
 */
struct PCGEXELEMENTSVALENCY_API FPCGExConstraintContext
{
	/** Parent connector's world-space transform */
	FTransform ParentConnectorWorld = FTransform::Identity;

	/** Computed base child placement (from ComputeAttachmentTransform) */
	FTransform BaseAttachment = FTransform::Identity;

	/** Child's local connector offset */
	FTransform ChildConnectorLocal = FTransform::Identity;

	/** Full frontier entry for the open connector */
	const FPCGExOpenConnector* OpenConnector = nullptr;

	/** Index of the child module being placed */
	int32 ChildModuleIndex = -1;

	/** Index of the child's connector being used for attachment */
	int32 ChildConnectorIndex = -1;

	// --- Growth state (populated by growth operation) ---

	/** Distance from seed (0 = seed itself) */
	int32 Depth = 0;

	/** Sum of module weights from seed to here */
	float CumulativeWeight = 0.0f;

	/** Total placed module count at this point */
	int32 PlacedCount = 0;
};

class UPCGExConstraintPreset;

/**
 * Runs the constraint pipeline in list order: each constraint is dispatched by role.
 * Produces candidate transforms for module placement.
 */
struct PCGEXELEMENTSVALENCY_API FPCGExConstraintResolver
{
	/** Maximum candidate transforms per evaluation (caps generator cross-product) */
	int32 MaxCandidates = 16;

	/**
	 * Run the full constraint pipeline (ordered execution).
	 * Pre-flattens presets, then iterates in list order dispatching by role.
	 * @param Context Evaluation context (parent/child transforms, connector info)
	 * @param Constraints Array of FInstancedStruct containing FPCGExConnectorConstraint subclasses
	 * @param Random Seeded random stream for deterministic evaluation
	 * @param OutCandidates Output candidate transforms (first = preferred)
	 */
	void Resolve(
		const FPCGExConstraintContext& Context,
		TConstArrayView<FInstancedStruct> Constraints,
		FRandomStream& Random,
		TArray<FTransform>& OutCandidates) const;

	/**
	 * Merge parent + child constraints by concatenation.
	 * Parent constraints execute first, child constraints append after.
	 */
	static void MergeConstraints(
		TConstArrayView<FInstancedStruct> ParentConstraints,
		TConstArrayView<FInstancedStruct> ChildConstraints,
		TArray<FInstancedStruct>& OutMerged);

	/**
	 * Apply a single constraint step to the variant pool.
	 * Used by the main pipeline and branch sub-pipelines.
	 */
	static void ApplyConstraintStep(
		const FPCGExConnectorConstraint* Constraint,
		const FPCGExConstraintContext& Context,
		FRandomStream& Random,
		TArray<FTransform>& Pool,
		int32 MaxCandidates);

	/**
	 * Flatten presets by recursively expanding FPCGExConstraint_Preset entries.
	 * Cycle-detects via VisitedPresets set.
	 */
	static void FlattenPresets(
		TConstArrayView<FInstancedStruct> Input,
		TArray<FInstancedStruct>& OutFlattened,
		TSet<const UPCGExConstraintPreset*>& VisitedPresets);

	/**
	 * Run the ordered pipeline on a pre-flattened constraint list.
	 * Shared between main Resolve() and branch sub-pipelines.
	 */
	void RunPipeline(
		const FPCGExConstraintContext& Context,
		TConstArrayView<FInstancedStruct> Constraints,
		FRandomStream& Random,
		TArray<FTransform>& Pool) const;
};
