// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Selectors/PCGExSelectorDensityWeighted.h"

#include "Containers/PCGExManagedObjects.h"
#include "Data/PCGExData.h"
#include "Details/PCGExSettingsDetails.h"

#pragma region FPCGExEntryDensityWeightedPickerOp

bool FPCGExEntryDensityWeightedPickerOp::PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection)
{
	if (!FPCGExEntryPickerOperation::PrepareForData(InContext, InDataFacade, InTarget, InOwningCollection)) { return false; }

	DensityGetter = DensitySource.GetValueSetting();
	if (!DensityGetter->Init(InDataFacade)) { return false; }

	return true;
}

int32 FPCGExEntryDensityWeightedPickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty()) { return -1; }

	double Density = DensityGetter ? DensityGetter->Read(PointIndex) : 1.0;

	// Out-of-range policy: either clamp, or skip the point entirely.
	if (Density < 0.0 || Density > 1.0)
	{
		if (OutOfRangePolicy == EPCGExDensityOutOfRangePolicy::SkipPoint) { return -1; }
		Density = FMath::Clamp(Density, 0.0, 1.0);
	}

	const int32 N = Target->Num();

	// Build per-point effective weights. Inline allocator covers typical category sizes
	// without heap pressure. O(N) per pick -- acceptable for the usual small-N cases.
	TArray<double, TInlineAllocator<32>> EffectiveWeights;
	EffectiveWeights.SetNumUninitialized(N);
	double TotalWeight = 0.0;

	switch (Mode)
	{
	case EPCGExDensityWeightMode::WeightModulation:
		{
			// exponent = lerp(1, density*2, DensityInfluence)
			// DI=0               -> exp=1               -> plain weighted (parity with WeightedRandom)
			// DI=1 & density=0   -> exp=0               -> uniform (all weights become 1)
			// DI=1 & density=0.5 -> exp=1               -> plain weighted
			// DI=1 & density=1   -> exp=2               -> amplified bias toward higher weights
			const double Exponent = FMath::Lerp(1.0, Density * 2.0, DensityInfluence);
			for (int32 i = 0; i < N; ++i)
			{
				const double W = static_cast<double>(Target->Entries[i]->Weight + 1);
				EffectiveWeights[i] = FMath::Pow(W, Exponent);
				TotalWeight += EffectiveWeights[i];
			}
			break;
		}
	case EPCGExDensityWeightMode::RandomnessModulation:
		{
			// effective_density = (1 - DI) + DI * density, then effective_weight = lerp(1, W, effective_density)
			// DI=0               -> eff=1               -> plain weighted (parity with WeightedRandom)
			// DI=1 & density=1   -> eff=1               -> plain weighted
			// DI=1 & density=0   -> eff=0               -> uniform
			const double EffectiveDensity = (1.0 - DensityInfluence) + DensityInfluence * Density;
			for (int32 i = 0; i < N; ++i)
			{
				const double W = static_cast<double>(Target->Entries[i]->Weight + 1);
				EffectiveWeights[i] = FMath::Lerp(1.0, W, EffectiveDensity);
				TotalWeight += EffectiveWeights[i];
			}
			break;
		}
	}

	if (TotalWeight <= 0.0) { return -1; }

	// Roll and linear cumulative search. Mirrors FCategory::GetPickRandomWeighted's approach
	// but over the per-point effective weight table.
	const double Roll = FRandomStream(Seed).FRandRange(0.0, TotalWeight);
	double Cumulative = 0.0;
	for (int32 i = 0; i < N; ++i)
	{
		Cumulative += EffectiveWeights[i];
		if (Roll <= Cumulative) { return Target->Indices[i]; }
	}

	// Numerical fallback -- return the last entry (matches FCategory's fallback pattern).
	return Target->Indices[N - 1];
}

#pragma endregion

#pragma region UPCGExSelectorDensityWeightedFactoryData

TSharedPtr<FPCGExEntryPickerOperation> UPCGExSelectorDensityWeightedFactoryData::CreateEntryOperation(FPCGExContext* InContext) const
{
	TSharedPtr<FPCGExEntryDensityWeightedPickerOp> NewOp = MakeShared<FPCGExEntryDensityWeightedPickerOp>();
	NewOp->Mode = Mode;
	NewOp->DensityInfluence = DensityInfluence;
	NewOp->OutOfRangePolicy = OutOfRangePolicy;
	NewOp->DensitySource = DensitySource;
	return NewOp;
}

#pragma endregion

#pragma region UPCGExSelectorDensityWeightedFactoryProviderSettings

UPCGExFactoryData* UPCGExSelectorDensityWeightedFactoryProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExSelectorDensityWeightedFactoryData* NewFactory = InContext->ManagedObjects->New<UPCGExSelectorDensityWeightedFactoryData>();
	NewFactory->BaseConfig = BaseConfig;
	NewFactory->Mode = Mode;
	NewFactory->DensitySource = DensitySource;
	NewFactory->DensityInfluence = DensityInfluence;
	NewFactory->OutOfRangePolicy = OutOfRangePolicy;
	return Super::CreateFactory(InContext, NewFactory);
}

#if WITH_EDITOR
FString UPCGExSelectorDensityWeightedFactoryProviderSettings::GetDisplayName() const
{
	switch (Mode)
	{
	default:
	case EPCGExDensityWeightMode::WeightModulation:
		return TEXT("Select : Weight Mod");
	case EPCGExDensityWeightMode::RandomnessModulation:
		return TEXT("Select : Randomness Mod");
	}
}
#endif

#pragma endregion
