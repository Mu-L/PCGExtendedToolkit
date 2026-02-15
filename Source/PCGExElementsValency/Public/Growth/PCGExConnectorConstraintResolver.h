// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

struct FPCGExOpenConnector;
struct FPCGExConnectorConstraint;
class UPCGExValencyConnectorSet;
class UPCGExConstraintPreset;
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

/**
 * Runs the constraint pipeline in list order: each constraint is dispatched by role.
 * Produces candidate transforms for module placement.
 *
 * Supports a pre-flatten cache: call CacheConstraintList() for all known constraint
 * lists during initialization. Resolve() then uses cached flattened arrays,
 * eliminating per-call recursion, cycle detection, and FInstancedStruct copies.
 */
struct PCGEXELEMENTSVALENCY_API FPCGExConstraintResolver
{
	/** Maximum candidate transforms per evaluation (caps generator cross-product) */
	int32 MaxCandidates = 16;

	/**
	 * Pre-flatten a constraint list into the cache.
	 * Call during initialization for each unique constraint list.
	 * Recursively discovers and caches Branch sub-pipeline presets.
	 */
	void CacheConstraintList(const TArray<FInstancedStruct>& Source);

	/**
	 * Run the full constraint pipeline (ordered execution).
	 * Executes each constraint list sequentially; the pool carries over between lists.
	 * Caller builds the list order based on override modes (e.g. parent defaults,
	 * parent overrides, child defaults, child overrides).
	 * @param Context Evaluation context (parent/child transforms, connector info)
	 * @param ConstraintLists Ordered sequence of constraint arrays to execute
	 * @param Random Seeded random stream for deterministic evaluation
	 * @param OutCandidates Output candidate transforms (first = preferred)
	 */
	void Resolve(
		const FPCGExConstraintContext& Context,
		TConstArrayView<const TArray<FInstancedStruct>*> ConstraintLists,
		FRandomStream& Random,
		TArray<FTransform>& OutCandidates) const;

private:
	/** Cached constraint pointer arrays — source FInstancedStruct data outlives the resolver. */
	using FConstraintPtrArray = TArray<const FPCGExConnectorConstraint*>;

	/** Get cached pointer array for a constraint list. Returns empty if not cached. */
	TConstArrayView<const FPCGExConnectorConstraint*> GetCached(const TArray<FInstancedStruct>& Source) const;

	/** Get cached pointer array for a preset asset. Returns empty if not cached. */
	TConstArrayView<const FPCGExConnectorConstraint*> GetCachedPreset(const UPCGExConstraintPreset* Preset) const;

	/** Cache a branch arm's preset (recursive: discovers nested branches). */
	void CacheBranchPreset(const UPCGExConstraintPreset* Preset);

	/** Scan cached constraint pointers for branches and cache their sub-pipeline presets. */
	void CacheBranchesIn(TConstArrayView<const FPCGExConnectorConstraint*> Constraints);

	/** Run the ordered pipeline on a cached pointer array. */
	void RunPipeline(
		const FPCGExConstraintContext& Context,
		TConstArrayView<const FPCGExConnectorConstraint*> Constraints,
		FRandomStream& Random,
		TArray<FTransform>& Pool) const;

	/** Apply a single Generator/Modifier/Filter step to the pool. */
	static void ApplyConstraintStep(
		const FPCGExConnectorConstraint* Constraint,
		const FPCGExConstraintContext& Context,
		FRandomStream& Random,
		TArray<FTransform>& Pool,
		int32 InMaxCandidates);

	/** Collect constraint pointers from FInstancedStruct source, expanding presets recursively. */
	static void CollectConstraints(
		TConstArrayView<FInstancedStruct> Input,
		FConstraintPtrArray& OutConstraints,
		TSet<const UPCGExConstraintPreset*>& VisitedPresets);

	/** Cached constraint pointers keyed by source array address. */
	TMap<const TArray<FInstancedStruct>*, FConstraintPtrArray> Cache;

	/** Cached constraint pointers for preset assets. */
	TMap<const UPCGExConstraintPreset*, FConstraintPtrArray> PresetCache;
};
