// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Growth/PCGExConnectorConstraintResolver.h"

#include "Core/PCGExValencyConnectorSet.h"
#include "Growth/Constraints/PCGExConstraintPreset.h"
#include "Growth/Constraints/PCGExConstraint_Branch.h"
#include "Helpers/PCGExStreamingHelpers.h"

#pragma region FPCGExConstraintResolver

void FPCGExConstraintResolver::Resolve(
	const FPCGExConstraintContext& Context,
	TConstArrayView<FInstancedStruct> Constraints,
	FRandomStream& Random,
	TArray<FTransform>& OutCandidates) const
{
	// Pre-pass: flatten presets (recursive, cycle-detected)
	TArray<FInstancedStruct> Flattened;
	TSet<const UPCGExConstraintPreset*> VisitedPresets;
	FlattenPresets(Constraints, Flattened, VisitedPresets);

	// Seed pool with base attachment
	OutCandidates.Add(Context.BaseAttachment);

	// Run the ordered pipeline
	RunPipeline(Context, Flattened, Random, OutCandidates);
}

void FPCGExConstraintResolver::RunPipeline(
	const FPCGExConstraintContext& Context,
	TConstArrayView<FInstancedStruct> Constraints,
	FRandomStream& Random,
	TArray<FTransform>& Pool) const
{
	for (const FInstancedStruct& Instance : Constraints)
	{
		if (Pool.IsEmpty()) { break; }

		const FPCGExConnectorConstraint* Constraint = Instance.GetPtr<FPCGExConnectorConstraint>();
		if (!Constraint || !Constraint->bEnabled) { continue; }

		const EPCGExConstraintRole Role = Constraint->GetRole();

		if (Role == EPCGExConstraintRole::Branch)
		{
			// Branch: partition -> run sub-pipelines -> rejoin
			const auto* BranchConstraint = static_cast<const FPCGExConstraint_Branch*>(Constraint);

			const FPCGExConnectorConstraint* Condition = nullptr;
			if (BranchConstraint->Condition.IsValid())
			{
				Condition = BranchConstraint->Condition.GetPtr<FPCGExConnectorConstraint>();
			}

			TArray<FTransform> PassPool;
			TArray<FTransform> FailPool;

			// Partition pool based on condition
			for (const FTransform& Variant : Pool)
			{
				if (Condition && Condition->bEnabled && !Condition->IsValid(Context, Variant))
				{
					FailPool.Add(Variant);
				}
				else
				{
					PassPool.Add(Variant);
				}
			}

			// Run OnPass sub-pipeline
			if (!PassPool.IsEmpty() && !BranchConstraint->OnPass.IsNull())
			{
				const UPCGExConstraintPreset* PassPreset = BranchConstraint->OnPass.Get();
				if (PassPreset)
				{
					TArray<FInstancedStruct> SubFlattened;
					TSet<const UPCGExConstraintPreset*> SubVisited;
					FlattenPresets(PassPreset->Constraints, SubFlattened, SubVisited);
					RunPipeline(Context, SubFlattened, Random, PassPool);
				}
			}

			// Run OnFail sub-pipeline
			if (!FailPool.IsEmpty() && !BranchConstraint->OnFail.IsNull())
			{
				const UPCGExConstraintPreset* FailPreset = BranchConstraint->OnFail.Get();
				if (FailPreset)
				{
					TArray<FInstancedStruct> SubFlattened;
					TSet<const UPCGExConstraintPreset*> SubVisited;
					FlattenPresets(FailPreset->Constraints, SubFlattened, SubVisited);
					RunPipeline(Context, SubFlattened, Random, FailPool);
				}
			}

			// Rejoin
			Pool.Reset();
			Pool.Append(PassPool);
			Pool.Append(FailPool);

			// Cap at MaxCandidates
			while (Pool.Num() > MaxCandidates)
			{
				Pool.RemoveAtSwap(Random.RandRange(0, Pool.Num() - 1));
			}
		}
		else if (Role == EPCGExConstraintRole::Preset)
		{
			// Should never reach here after flattening, but handle gracefully
			continue;
		}
		else
		{
			ApplyConstraintStep(Constraint, Context, Random, Pool, MaxCandidates);
		}
	}
}

void FPCGExConstraintResolver::ApplyConstraintStep(
	const FPCGExConnectorConstraint* Constraint,
	const FPCGExConstraintContext& Context,
	FRandomStream& Random,
	TArray<FTransform>& Pool,
	int32 InMaxCandidates)
{
	switch (Constraint->GetRole())
	{
	case EPCGExConstraintRole::Generator:
		{
			// Cross-product expand: for each existing variant, generate N new ones
			TArray<FTransform> Expanded;
			Expanded.Reserve(Pool.Num() * Constraint->GetMaxVariants());

			for (const FTransform& Existing : Pool)
			{
				FPCGExConstraintContext SubContext = Context;
				SubContext.BaseAttachment = Existing;
				Constraint->GenerateVariants(SubContext, Random, Expanded);
			}

			Pool = MoveTemp(Expanded);

			// Cap at MaxCandidates after each generator
			while (Pool.Num() > InMaxCandidates)
			{
				Pool.RemoveAtSwap(Random.RandRange(0, Pool.Num() - 1));
			}
		}
		break;

	case EPCGExConstraintRole::Modifier:
		{
			for (FTransform& Variant : Pool)
			{
				Constraint->ApplyModification(Context, Variant, Random);
			}
		}
		break;

	case EPCGExConstraintRole::Filter:
		{
			for (int32 i = Pool.Num() - 1; i >= 0; --i)
			{
				if (!Constraint->IsValid(Context, Pool[i]))
				{
					Pool.RemoveAtSwap(i);
				}
			}
		}
		break;

	default:
		break;
	}
}

void FPCGExConstraintResolver::MergeConstraints(
	TConstArrayView<FInstancedStruct> ParentConstraints,
	TConstArrayView<FInstancedStruct> ChildConstraints,
	TArray<FInstancedStruct>& OutMerged)
{
	// Concatenate: parent constraints first, child constraints after
	OutMerged.Reserve(ParentConstraints.Num() + ChildConstraints.Num());

	for (const FInstancedStruct& Instance : ParentConstraints)
	{
		OutMerged.Add(Instance);
	}

	for (const FInstancedStruct& Instance : ChildConstraints)
	{
		OutMerged.Add(Instance);
	}
}

void FPCGExConstraintResolver::FlattenPresets(
	TConstArrayView<FInstancedStruct> Input,
	TArray<FInstancedStruct>& OutFlattened,
	TSet<const UPCGExConstraintPreset*>& VisitedPresets)
{
	for (const FInstancedStruct& Instance : Input)
	{
		const FPCGExConnectorConstraint* Constraint = Instance.GetPtr<FPCGExConnectorConstraint>();
		if (!Constraint) { continue; }

		if (Constraint->GetRole() == EPCGExConstraintRole::Preset)
		{
			const auto* PresetConstraint = static_cast<const FPCGExConstraint_Preset*>(Constraint);
			if (PresetConstraint->Preset.IsNull()) { continue; }

			const UPCGExConstraintPreset* PresetAsset = PresetConstraint->Preset.Get();
			if (!PresetAsset)
			{
				// Try blocking load
				PCGExHelpers::LoadBlocking_AnyThreadTpl(PresetConstraint->Preset);
				PresetAsset = PresetConstraint->Preset.Get();
			}

			if (!PresetAsset) { continue; }

			// Cycle detection
			if (VisitedPresets.Contains(PresetAsset))
			{
				UE_LOG(LogTemp, Warning, TEXT("[PCGEx] Circular constraint preset reference detected, skipping: %s"), *PresetAsset->GetName());
				continue;
			}

			VisitedPresets.Add(PresetAsset);
			FlattenPresets(PresetAsset->Constraints, OutFlattened, VisitedPresets);
			VisitedPresets.Remove(PresetAsset);
		}
		else
		{
			OutFlattened.Add(Instance);
		}
	}
}

#pragma endregion
