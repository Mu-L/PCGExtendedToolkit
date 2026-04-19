// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Selectors/PCGExSelectorBestFit.h"

#include "Core/PCGExAssetCollection.h"
#include "Core/PCGExContext.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExSettingsDetails.h"
#include "Data/PCGBasePointData.h"

#pragma region FPCGExEntryBestFitPickerOpBase

bool FPCGExEntryBestFitPickerOpBase::PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection)
{
	if (!FPCGExEntryPickerOperation::PrepareForData(InContext, InDataFacade, InTarget, InOwningCollection)) { return false; }

	const UPCGBasePointData* PointData = InDataFacade->Source->GetIn();
	if (!PointData) { return false; }

	BoundsMinRange = PointData->GetConstBoundsMinValueRange();
	BoundsMaxRange = PointData->GetConstBoundsMaxValueRange();
	if (bApplyPointScale) { TransformRange = PointData->GetConstTransformValueRange(); }

	PoolSizeGetter = PoolSize.GetValueSetting();
	if (!PoolSizeGetter->Init(InDataFacade)) { return false; }

	// Clamp AxisMask to a valid non-zero bitmask. Zero mask would make all scores identical.
	if (Metric != EPCGExBestFitMetric::ClosestVolume && (AxisMask & 0b111) == 0)
	{
		AxisMask = 0b111;
	}

	const int32 N = Target->Entries.Num();
	EntryExtents.SetNumUninitialized(N);
	EntryExtentsMaxNorm.SetNumUninitialized(N);
	EntryVolumes.SetNumUninitialized(N);
	EntryWeights.SetNumUninitialized(N);
	ValidEntryIndices.Reserve(N);

	for (int32 i = 0; i < N; ++i)
	{
		const FPCGExAssetCollectionEntry* Entry = Target->Entries[i];
		FVector Ext = FVector::ZeroVector;
		double Vol = 0.0;

		if (Entry && Entry->Staging.Bounds.IsValid)
		{
			Ext = Entry->Staging.Bounds.GetExtent();
			Vol = Ext.X * Ext.Y * Ext.Z;
		}

		EntryExtents[i] = Ext;
		EntryVolumes[i] = Vol;
		EntryWeights[i] = Entry ? static_cast<double>(Entry->Weight + 1) : 0.0;

		// Max-component normalized for AspectRatio metric. Zero when Ext is degenerate.
		const double MaxExt = FMath::Max3(Ext.X, Ext.Y, Ext.Z);
		EntryExtentsMaxNorm[i] = MaxExt > UE_DOUBLE_SMALL_NUMBER ? Ext / MaxExt : FVector::ZeroVector;

		if (Vol > UE_DOUBLE_SMALL_NUMBER) { ValidEntryIndices.Add(i); }
	}

	if (ValidEntryIndices.IsEmpty())
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Selector : Best Fit — no entries with valid bounds. Check that the collection's entries have non-zero Staging.Bounds."));
		return false;
	}

	return true;
}

FVector FPCGExEntryBestFitPickerOpBase::GetPointExtents(int32 PointIndex) const
{
	FVector Ext = (BoundsMaxRange[PointIndex] - BoundsMinRange[PointIndex]) * 0.5;
	if (bApplyPointScale) { Ext *= TransformRange[PointIndex].GetScale3D().GetAbs(); }
	return Ext;
}

double FPCGExEntryBestFitPickerOpBase::ComputeScore(const FVector& P, int32 EntryIndex) const
{
	const FVector& E = EntryExtents[EntryIndex];
	const double Ve = EntryVolumes[EntryIndex];

	switch (Metric)
	{
	default:
	case EPCGExBestFitMetric::ClosestVolume:
		{
			const double Vp = P.X * P.Y * P.Z;
			const double MaxV = FMath::Max3(Ve, Vp, UE_DOUBLE_SMALL_NUMBER);
			return FMath::Abs(Ve - Vp) / MaxV;
		}
	case EPCGExBestFitMetric::ClosestPerAxis:
		{
			const double PerAxis[3] = {
				FMath::Abs(E.X - P.X),
				FMath::Abs(E.Y - P.Y),
				FMath::Abs(E.Z - P.Z)
			};
			double Accum = 0.0;
			for (int32 a = 0; a < 3; ++a)
			{
				if (!(AxisMask & (1 << a))) { continue; }
				Accum = (AxisAggregation == EPCGExBestFitAxisAggregation::Sum)
					        ? Accum + PerAxis[a]
					        : FMath::Max(Accum, PerAxis[a]);
			}
			return Accum;
		}
	case EPCGExBestFitMetric::ClosestAspectRatio:
		{
			const FVector& Enorm = EntryExtentsMaxNorm[EntryIndex];
			const double PMax = FMath::Max3(P.X, P.Y, P.Z);
			const FVector Pnorm = PMax > UE_DOUBLE_SMALL_NUMBER ? P / PMax : FVector::ZeroVector;

			const double PerAxis[3] = {
				FMath::Abs(Enorm.X - Pnorm.X),
				FMath::Abs(Enorm.Y - Pnorm.Y),
				FMath::Abs(Enorm.Z - Pnorm.Z)
			};
			double Accum = 0.0;
			for (int32 a = 0; a < 3; ++a)
			{
				if (!(AxisMask & (1 << a))) { continue; }
				Accum = (AxisAggregation == EPCGExBestFitAxisAggregation::Sum)
					        ? Accum + PerAxis[a]
					        : FMath::Max(Accum, PerAxis[a]);
			}

			if (VolumeInfluence > 0.0)
			{
				const double Vp = P.X * P.Y * P.Z;
				const double MaxV = FMath::Max3(Ve, Vp, UE_DOUBLE_SMALL_NUMBER);
				const double VolumeScore = FMath::Abs(Ve - Vp) / MaxV;
				Accum += VolumeInfluence * VolumeScore;
			}
			return Accum;
		}
	}
}

#pragma endregion

#pragma region FPCGExEntryBestFitTopKPickerOp

int32 FPCGExEntryBestFitTopKPickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty() || ValidEntryIndices.IsEmpty()) { return -1; }

	const FVector P = GetPointExtents(PointIndex);
	const int32 N = ValidEntryIndices.Num();
	const int32 K = FMath::Clamp(FMath::RoundToInt(PoolSizeGetter->Read(PointIndex)), 1, N);

	// Score + sort. Inline allocator covers typical category sizes.
	TArray<TPair<double, int32>, TInlineAllocator<32>> Scored;
	Scored.Reserve(N);
	for (const int32 i : ValidEntryIndices) { Scored.Emplace(ComputeScore(P, i), i); }
	Scored.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });

	// Pool = first K, plus any ties at the K-th score to avoid arbitrary tie-breaking.
	const double Cutoff = Scored[K - 1].Key;
	TArray<int32, TInlineAllocator<32>> Pool;
	TArray<double, TInlineAllocator<32>> Cumulative;
	double TotalWeight = 0.0;

	for (const TPair<double, int32>& S : Scored)
	{
		if (S.Key > Cutoff + UE_DOUBLE_KINDA_SMALL_NUMBER) { break; }
		Pool.Add(S.Value);
		TotalWeight += EntryWeights[S.Value];
		Cumulative.Add(TotalWeight);
	}

	if (Pool.IsEmpty() || TotalWeight <= 0.0) { return -1; }

	const double Roll = FRandomStream(Seed).FRandRange(0.0, TotalWeight);
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (Roll <= Cumulative[k]) { return Target->Indices[Pool[k]]; }
	}
	return Target->Indices[Pool.Last()];
}

#pragma endregion

#pragma region FPCGExEntryBestFitTolerancePickerOp

int32 FPCGExEntryBestFitTolerancePickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty() || ValidEntryIndices.IsEmpty()) { return -1; }

	const FVector P = GetPointExtents(PointIndex);
	const double Tolerance = FMath::Max(0.0, PoolSizeGetter->Read(PointIndex));

	// Single pass: score and track best.
	TArray<TPair<double, int32>, TInlineAllocator<32>> Scored;
	Scored.Reserve(ValidEntryIndices.Num());
	double BestScore = TNumericLimits<double>::Max();
	for (const int32 i : ValidEntryIndices)
	{
		const double S = ComputeScore(P, i);
		Scored.Emplace(S, i);
		if (S < BestScore) { BestScore = S; }
	}

	// Pool = entries within Tolerance * BestScore of the best score.
	// When BestScore is ~0 (perfect fit), the cutoff also ~0 → only perfect-tie entries pool together.
	const double Cutoff = BestScore * (1.0 + Tolerance) + UE_DOUBLE_KINDA_SMALL_NUMBER;

	TArray<int32, TInlineAllocator<32>> Pool;
	TArray<double, TInlineAllocator<32>> Cumulative;
	double TotalWeight = 0.0;

	for (const TPair<double, int32>& S : Scored)
	{
		if (S.Key > Cutoff) { continue; }
		Pool.Add(S.Value);
		TotalWeight += EntryWeights[S.Value];
		Cumulative.Add(TotalWeight);
	}

	if (Pool.IsEmpty() || TotalWeight <= 0.0) { return -1; }

	const double Roll = FRandomStream(Seed).FRandRange(0.0, TotalWeight);
	for (int32 k = 0; k < Pool.Num(); ++k)
	{
		if (Roll <= Cumulative[k]) { return Target->Indices[Pool[k]]; }
	}
	return Target->Indices[Pool.Last()];
}

#pragma endregion

#pragma region UPCGExSelectorBestFitFactoryData

TSharedPtr<FPCGExEntryPickerOperation> UPCGExSelectorBestFitFactoryData::CreateEntryOperation(FPCGExContext* InContext) const
{
	TSharedPtr<FPCGExEntryBestFitPickerOpBase> NewOp;

	switch (PoolStrategy)
	{
	default:
	case EPCGExBestFitPoolStrategy::TopK:
		NewOp = MakeShared<FPCGExEntryBestFitTopKPickerOp>();
		break;
	case EPCGExBestFitPoolStrategy::Tolerance:
		NewOp = MakeShared<FPCGExEntryBestFitTolerancePickerOp>();
		break;
	}

	NewOp->Metric = Metric;
	NewOp->AxisMask = AxisMask;
	NewOp->AxisAggregation = AxisAggregation;
	NewOp->VolumeInfluence = VolumeInfluence;
	NewOp->bApplyPointScale = bApplyPointScale;
	NewOp->PoolSize = PoolSize;
	return NewOp;
}

#pragma endregion

#pragma region UPCGExSelectorBestFitFactoryProviderSettings

UPCGExFactoryData* UPCGExSelectorBestFitFactoryProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExSelectorBestFitFactoryData* NewFactory = InContext->ManagedObjects->New<UPCGExSelectorBestFitFactoryData>();
	NewFactory->BaseConfig = BaseConfig;
	NewFactory->Metric = Metric;
	NewFactory->AxisMask = AxisMask;
	NewFactory->AxisAggregation = AxisAggregation;
	NewFactory->VolumeInfluence = VolumeInfluence;
	NewFactory->bApplyPointScale = bApplyPointScale;
	NewFactory->PoolStrategy = PoolStrategy;
	NewFactory->PoolSize = PoolSize;
	return Super::CreateFactory(InContext, NewFactory);
}

#if WITH_EDITOR
FString UPCGExSelectorBestFitFactoryProviderSettings::GetDisplayName() const
{
	switch (Metric)
	{
	default:
	case EPCGExBestFitMetric::ClosestVolume:      return TEXT("Select : Fit Volume");
	case EPCGExBestFitMetric::ClosestPerAxis:     return TEXT("Select : Fit Per-Axis");
	case EPCGExBestFitMetric::ClosestAspectRatio: return TEXT("Select : Fit Aspect");
	}
}
#endif

#pragma endregion
