// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Growth/PCGExConnectorConstraintResolver.h"

#include "Core/PCGExValencyConnectorSet.h"
#include "Growth/Constraints/PCGExConstraintPreset.h"
#include "Growth/Constraints/PCGExConstraint_Branch.h"
#include "Helpers/PCGExStreamingHelpers.h"

#pragma region FPCGExConstraintResolver

// ========== Cache Building ==========

void FPCGExConstraintResolver::CacheConstraintList(const TArray<FInstancedStruct>& Source)
{
	if (Source.IsEmpty() || Cache.Contains(&Source)) { return; }

	FConstraintPtrArray& Cached = Cache.Add(&Source);

	// Check if flattening is needed (any Preset entries?)
	bool bNeedsFlatten = false;
	for (const FInstancedStruct& Instance : Source)
	{
		const FPCGExConnectorConstraint* C = Instance.GetPtr<FPCGExConnectorConstraint>();
		if (C && C->GetRole() == EPCGExConstraintRole::Preset)
		{
			bNeedsFlatten = true;
			break;
		}
	}

	if (bNeedsFlatten)
	{
		TSet<const UPCGExConstraintPreset*> Visited;
		CollectConstraints(Source, Cached, Visited);
	}
	else
	{
		// No presets — extract pointers directly from source
		Cached.Reserve(Source.Num());
		for (const FInstancedStruct& Instance : Source)
		{
			const FPCGExConnectorConstraint* C = Instance.GetPtr<FPCGExConnectorConstraint>();
			if (C) { Cached.Add(C); }
		}
	}

	// Discover branches in the cached result
	CacheBranchesIn(Cached);
}

void FPCGExConstraintResolver::CacheBranchesIn(TConstArrayView<const FPCGExConnectorConstraint*> Constraints)
{
	for (const FPCGExConnectorConstraint* Constraint : Constraints)
	{
		if (!Constraint || Constraint->GetRole() != EPCGExConstraintRole::Branch) { continue; }

		const auto* BranchConstraint = static_cast<const FPCGExConstraint_Branch*>(Constraint);

		if (!BranchConstraint->OnPass.IsNull())
		{
			CacheBranchPreset(BranchConstraint->OnPass.Get());
		}
		if (!BranchConstraint->OnFail.IsNull())
		{
			CacheBranchPreset(BranchConstraint->OnFail.Get());
		}
	}
}

void FPCGExConstraintResolver::CacheBranchPreset(const UPCGExConstraintPreset* Preset)
{
	if (!Preset || PresetCache.Contains(Preset)) { return; }

	FConstraintPtrArray& Cached = PresetCache.Add(Preset);
	TSet<const UPCGExConstraintPreset*> Visited;
	CollectConstraints(Preset->Constraints, Cached, Visited);

	// Recursively discover nested branches
	CacheBranchesIn(Cached);
}

TConstArrayView<const FPCGExConnectorConstraint*> FPCGExConstraintResolver::GetCached(const TArray<FInstancedStruct>& Source) const
{
	if (const FConstraintPtrArray* Cached = Cache.Find(&Source))
	{
		return *Cached;
	}
	return {};
}

TConstArrayView<const FPCGExConnectorConstraint*> FPCGExConstraintResolver::GetCachedPreset(const UPCGExConstraintPreset* Preset) const
{
	if (const FConstraintPtrArray* Cached = PresetCache.Find(Preset))
	{
		return *Cached;
	}
	return {};
}

// ========== Pipeline Execution ==========

void FPCGExConstraintResolver::Resolve(
	const FPCGExConstraintContext& Context,
	TConstArrayView<const TArray<FInstancedStruct>*> ConstraintLists,
	FRandomStream& Random,
	TArray<FTransform>& OutCandidates) const
{
	OutCandidates.Add(Context.BaseAttachment);

	for (const TArray<FInstancedStruct>* List : ConstraintLists)
	{
		if (OutCandidates.IsEmpty()) { break; }

		const TConstArrayView<const FPCGExConnectorConstraint*> Cached = GetCached(*List);
		if (Cached.Num() > 0)
		{
			RunPipeline(Context, Cached, Random, OutCandidates);
		}
	}
}

void FPCGExConstraintResolver::RunPipeline(
	const FPCGExConstraintContext& Context,
	TConstArrayView<const FPCGExConnectorConstraint*> Constraints,
	FRandomStream& Random,
	TArray<FTransform>& Pool) const
{
	for (const FPCGExConnectorConstraint* Constraint : Constraints)
	{
		if (Pool.IsEmpty()) { break; }
		if (!Constraint || !Constraint->bEnabled) { continue; }

		const EPCGExConstraintRole Role = Constraint->GetRole();

		if (Role == EPCGExConstraintRole::Branch)
		{
			const auto* BranchConstraint = static_cast<const FPCGExConstraint_Branch*>(Constraint);

			// Extract condition filter
			const FPCGExConnectorConstraint* Condition = nullptr;
			if (BranchConstraint->Condition.IsValid())
			{
				Condition = BranchConstraint->Condition.GetPtr<FPCGExConnectorConstraint>();
			}

			// Partition pool
			TArray<FTransform> PassPool;
			TArray<FTransform> FailPool;
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

			// Run sub-pipelines using cached preset pointers
			auto RunBranchArm = [this, &Context, &Random](const TSoftObjectPtr<UPCGExConstraintPreset>& PresetRef, TArray<FTransform>& ArmPool)
			{
				if (ArmPool.IsEmpty() || PresetRef.IsNull()) { return; }
				const UPCGExConstraintPreset* Preset = PresetRef.Get();
				if (!Preset) { return; }

				const TConstArrayView<const FPCGExConnectorConstraint*> Cached = GetCachedPreset(Preset);
				if (Cached.Num() > 0)
				{
					RunPipeline(Context, Cached, Random, ArmPool);
				}
			};

			RunBranchArm(BranchConstraint->OnPass, PassPool);
			RunBranchArm(BranchConstraint->OnFail, FailPool);

			// Rejoin
			Pool.Reset();
			Pool.Append(PassPool);
			Pool.Append(FailPool);

			while (Pool.Num() > MaxCandidates)
			{
				Pool.RemoveAtSwap(Random.RandRange(0, Pool.Num() - 1));
			}
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
			TArray<FTransform> Expanded;
			Expanded.Reserve(Pool.Num() * Constraint->GetMaxVariants());

			for (const FTransform& Existing : Pool)
			{
				FPCGExConstraintContext SubContext = Context;
				SubContext.BaseAttachment = Existing;
				Constraint->GenerateVariants(SubContext, Random, Expanded);
			}

			Pool = MoveTemp(Expanded);

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

// ========== Cache Building Helpers ==========

void FPCGExConstraintResolver::CollectConstraints(
	TConstArrayView<FInstancedStruct> Input,
	FConstraintPtrArray& OutConstraints,
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
				PCGExHelpers::LoadBlocking_AnyThreadTpl(PresetConstraint->Preset);
				PresetAsset = PresetConstraint->Preset.Get();
			}

			if (!PresetAsset) { continue; }

			if (VisitedPresets.Contains(PresetAsset))
			{
				UE_LOG(LogTemp, Warning, TEXT("[PCGEx] Circular constraint preset reference detected, skipping: %s"), *PresetAsset->GetName());
				continue;
			}

			VisitedPresets.Add(PresetAsset);
			CollectConstraints(PresetAsset->Constraints, OutConstraints, VisitedPresets);
			VisitedPresets.Remove(PresetAsset);
		}
		else
		{
			OutConstraints.Add(Constraint);
		}
	}
}

#pragma endregion
