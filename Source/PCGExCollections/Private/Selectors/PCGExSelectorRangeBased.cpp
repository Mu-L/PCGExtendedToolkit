// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Selectors/PCGExSelectorRangeBased.h"

#include "Containers/PCGExManagedObjects.h"
#include "Core/PCGExAssetCollection.h"
#include "Core/PCGExContext.h"
#include "Data/PCGExData.h"
#include "Details/PCGExSettingsDetails.h"
#include "PCGExPropertyTypes.h"

namespace
{
	constexpr double RangeTieEpsilon = 1e-6;

	// Resolve a numeric property on an entry as double. Accepts any PCG-numeric property
	// (Double/Float/Int32/Int64/Bool) transparently via the type-erased TryGetPropertyValue.
	bool ResolveNumericAsDouble(const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Collection, FName Name, double& OutValue)
	{
		return Entry->TryGetPropertyValue<double>(Collection, Name, OutValue);
	}

	// Resolve a Vector2D-style range property on an entry. Any vector-compatible property
	// projects via TryGetPropertyValue<FVector2D> — FVector drops Z to give (X, Y),
	// FVector4 drops Z/W. X -> Min, Y -> Max.
	bool ResolveRangeFromVector(const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Collection, FName Name, double& OutMin, double& OutMax)
	{
		FVector2D XY = FVector2D::ZeroVector;
		if (!Entry->TryGetPropertyValue<FVector2D>(Collection, Name, XY)) { return false; }
		OutMin = XY.X;
		OutMax = XY.Y;
		return true;
	}
}

#pragma region FPCGExEntryRangeBasedPickerOpBase

bool FPCGExEntryRangeBasedPickerOpBase::PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection)
{
	if (!FPCGExEntryPickerOperation::PrepareForData(InContext, InDataFacade, InTarget, InOwningCollection)) { return false; }

	// Shared data is always produced by the factory's BuildSharedData (directly or via cache).
	// A null Shared here means the factory decided the collection has nothing usable for Range-Based.
	Shared = StaticCastSharedPtr<FPCGExRangeBasedSharedData>(SharedData);
	if (!Shared)
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Selector : Range-Based — no entries resolved the referenced range property. Check property names and types in the collection."));
		return false;
	}

	ValueGetter = ValueSource.GetValueSetting();
	if (!ValueGetter->Init(InDataFacade)) { return false; }

	return true;
}

// Binary-search fast path for non-overlapping range layouts. V can match at most one range,
// so all three overlap modes collapse to the same lookup. Returns raw Target index or -1.
int32 FPCGExEntryRangeBasedPickerOpBase::FastPathPick(double V) const
{
	const TArray<double>& SortedMins = Shared->SortedMins;
	const TArray<int32>& SortedIndices = Shared->SortedIndices;

	// Manual upper_bound — first k where SortedMins[k] > V.
	int32 Lo = 0;
	int32 Hi = SortedMins.Num();
	while (Lo < Hi)
	{
		const int32 Mid = (Lo + Hi) >> 1;
		if (SortedMins[Mid] <= V) { Lo = Mid + 1; }
		else { Hi = Mid; }
	}

	if (Lo <= 0) { return -1; }
	const int32 i = SortedIndices[Lo - 1];
	return Contains(V, Shared->EntryMins[i], Shared->EntryMaxs[i]) ? Target->Indices[i] : -1;
}

#pragma endregion

#pragma region FPCGExEntryRangeWeightedRandomPickerOp

int32 FPCGExEntryRangeWeightedRandomPickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty()) { return -1; }

	const double V = ValueGetter->Read(PointIndex);

	if (Shared->bNonOverlapping) { return FastPathPick(V); }

	const TArray<double>& EntryMins = Shared->EntryMins;
	const TArray<double>& EntryMaxs = Shared->EntryMaxs;
	const TArray<double>& EntryWeights = Shared->EntryWeights;
	const TArray<int32>& SortedIndices = Shared->SortedIndices;

	// Sorted early-exit scan over valid entries; accumulate cumulative weights for matches.
	TArray<int32, TInlineAllocator<32>> Matches;
	TArray<double, TInlineAllocator<32>> Cumulative;
	double TotalWeight = 0.0;

	const int32 NValid = SortedIndices.Num();
	for (int32 k = 0; k < NValid; ++k)
	{
		const int32 i = SortedIndices[k];
		if (EntryMins[i] > V) { break; }
		if (!Contains(V, EntryMins[i], EntryMaxs[i])) { continue; }
		TotalWeight += EntryWeights[i];
		Matches.Add(i);
		Cumulative.Add(TotalWeight);
	}

	if (Matches.IsEmpty() || TotalWeight <= 0.0) { return -1; }

	const double Roll = FRandomStream(Seed).FRandRange(0.0, TotalWeight);
	for (int32 k = 0; k < Matches.Num(); ++k)
	{
		if (Roll <= Cumulative[k]) { return Target->Indices[Matches[k]]; }
	}

	// Numerical fallback — last match wins (mirrors FCategory weighted fallback).
	return Target->Indices[Matches.Last()];
}

#pragma endregion

#pragma region FPCGExEntryRangeFirstMatchPickerOp

int32 FPCGExEntryRangeFirstMatchPickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty()) { return -1; }

	const double V = ValueGetter->Read(PointIndex);

	// Non-overlapping => at most one match, same answer as unsorted scan.
	if (Shared->bNonOverlapping) { return FastPathPick(V); }

	// Overlapping case: preserve "first in category order" semantics by iterating unsorted.
	const TArray<double>& EntryMins = Shared->EntryMins;
	const TArray<double>& EntryMaxs = Shared->EntryMaxs;
	const int32 N = Target->Entries.Num();
	for (int32 i = 0; i < N; ++i)
	{
		if (Contains(V, EntryMins[i], EntryMaxs[i])) { return Target->Indices[i]; }
	}

	return -1;
}

#pragma endregion

#pragma region FPCGExEntryRangeNarrowestPickerOp

int32 FPCGExEntryRangeNarrowestPickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty()) { return -1; }

	const double V = ValueGetter->Read(PointIndex);

	if (Shared->bNonOverlapping) { return FastPathPick(V); }

	const TArray<double>& EntryMins = Shared->EntryMins;
	const TArray<double>& EntryMaxs = Shared->EntryMaxs;
	const TArray<double>& EntryWeights = Shared->EntryWeights;
	const TArray<int32>& SortedIndices = Shared->SortedIndices;

	// Sorted early-exit scan; track current minimum width and accumulate same-width ties.
	double MinWidth = TNumericLimits<double>::Max();
	TArray<int32, TInlineAllocator<8>> TieBucket;

	const int32 NValid = SortedIndices.Num();
	for (int32 k = 0; k < NValid; ++k)
	{
		const int32 i = SortedIndices[k];
		if (EntryMins[i] > V) { break; }
		if (!Contains(V, EntryMins[i], EntryMaxs[i])) { continue; }
		const double W = EntryMaxs[i] - EntryMins[i];

		if (W < MinWidth - RangeTieEpsilon)
		{
			MinWidth = W;
			TieBucket.Reset();
			TieBucket.Add(i);
		}
		else if (FMath::Abs(W - MinWidth) <= RangeTieEpsilon)
		{
			TieBucket.Add(i);
		}
	}

	if (TieBucket.IsEmpty()) { return -1; }
	if (TieBucket.Num() == 1) { return Target->Indices[TieBucket[0]]; }

	// Tie-break by weight.
	double TotalWeight = 0.0;
	for (const int32 i : TieBucket) { TotalWeight += EntryWeights[i]; }
	if (TotalWeight <= 0.0) { return Target->Indices[TieBucket[0]]; }

	const double Roll = FRandomStream(Seed).FRandRange(0.0, TotalWeight);
	double Acc = 0.0;
	for (const int32 i : TieBucket)
	{
		Acc += EntryWeights[i];
		if (Roll <= Acc) { return Target->Indices[i]; }
	}

	return Target->Indices[TieBucket.Last()];
}

#pragma endregion

#pragma region UPCGExSelectorRangeBasedFactoryData

TSharedPtr<FPCGExEntryPickerOperation> UPCGExSelectorRangeBasedFactoryData::CreateEntryOperation(FPCGExContext* InContext) const
{
	TSharedPtr<FPCGExEntryRangeBasedPickerOpBase> NewOp;

	switch (OverlapMode)
	{
	default:
	case EPCGExRangeOverlapMode::WeightedRandom:
		NewOp = MakeShared<FPCGExEntryRangeWeightedRandomPickerOp>();
		break;
	case EPCGExRangeOverlapMode::FirstMatch:
		NewOp = MakeShared<FPCGExEntryRangeFirstMatchPickerOp>();
		break;
	case EPCGExRangeOverlapMode::NarrowestWins:
		NewOp = MakeShared<FPCGExEntryRangeNarrowestPickerOp>();
		break;
	}

	// BoundaryMode is needed in the hot path for Contains(); ValueSource drives the per-facade getter.
	// Everything else (MinPropertyName, SourceMode, etc.) is consumed by BuildSharedData on the factory side.
	NewOp->BoundaryMode = BoundaryMode;
	NewOp->ValueSource = ValueSource;
	return NewOp;
}

TSharedPtr<PCGExCollections::FSelectorSharedData> UPCGExSelectorRangeBasedFactoryData::BuildSharedData(
	const UPCGExAssetCollection* Collection,
	const PCGExAssetCollection::FCategory* Target) const
{
	if (!Collection || !Target) { return nullptr; }

	const int32 N = Target->Entries.Num();

	TSharedPtr<FPCGExRangeBasedSharedData> NewShared = MakeShared<FPCGExRangeBasedSharedData>();
	NewShared->EntryMins.SetNumUninitialized(N);
	NewShared->EntryMaxs.SetNumUninitialized(N);

	// Sentinel range that never matches any value (Contains() returns false for all Boundary modes).
	auto MarkInvalid = [&](int32 i) { NewShared->EntryMins[i] = 1.0; NewShared->EntryMaxs[i] = -1.0; };

	int32 ResolvedCount = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const FPCGExAssetCollectionEntry* Entry = Target->Entries[i];
		double Min = 0.0;
		double Max = 0.0;
		bool bResolved = false;

		switch (SourceMode)
		{
		case EPCGExRangeSourceMode::TwoNumerics:
			bResolved = ResolveNumericAsDouble(Entry, Collection, MinPropertyName, Min)
				&& ResolveNumericAsDouble(Entry, Collection, MaxPropertyName, Max);
			break;
		case EPCGExRangeSourceMode::Vector2:
			bResolved = ResolveRangeFromVector(Entry, Collection, RangePropertyName, Min, Max);
			break;
		}

		if (!bResolved) { MarkInvalid(i); continue; }

		// Auto-swap out-of-order ranges — matches PCGEx convention for numeric range inputs.
		if (Min > Max) { Swap(Min, Max); }
		NewShared->EntryMins[i] = Min;
		NewShared->EntryMaxs[i] = Max;
		++ResolvedCount;
	}

	// Caller (FSelectorHelper::PrepareForData) reports the error when Shared is null.
	if (ResolvedCount == 0) { return nullptr; }

	// Cache (Weight + 1) as double for the hot path.
	NewShared->EntryWeights.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		NewShared->EntryWeights[i] = (NewShared->EntryMins[i] <= NewShared->EntryMaxs[i])
			? static_cast<double>(Target->Entries[i]->Weight + 1)
			: 0.0;
	}

	// Build sorted view over valid entries (invalid entries have Min > Max sentinel).
	NewShared->SortedIndices.Reserve(ResolvedCount);
	for (int32 i = 0; i < N; ++i)
	{
		if (NewShared->EntryMins[i] <= NewShared->EntryMaxs[i]) { NewShared->SortedIndices.Add(i); }
	}
	const TArray<double>& MinsRef = NewShared->EntryMins;
	NewShared->SortedIndices.Sort([&MinsRef](int32 A, int32 B) { return MinsRef[A] < MinsRef[B]; });

	const int32 NValid = NewShared->SortedIndices.Num();
	NewShared->SortedMins.SetNumUninitialized(NValid);
	for (int32 k = 0; k < NValid; ++k)
	{
		NewShared->SortedMins[k] = NewShared->EntryMins[NewShared->SortedIndices[k]];
	}

	// Strict non-overlap detection — any adjacent pair with next_Min <= prev_Max disables the fast path.
	// Strictness avoids the shared-endpoint ambiguity (same V may land in two ranges under Closed boundaries).
	NewShared->bNonOverlapping = true;
	for (int32 k = 1; k < NValid; ++k)
	{
		const double PrevMax = NewShared->EntryMaxs[NewShared->SortedIndices[k - 1]];
		const double CurMin = NewShared->EntryMins[NewShared->SortedIndices[k]];
		if (CurMin <= PrevMax) { NewShared->bNonOverlapping = false; break; }
	}

	return NewShared;
}

#pragma endregion

#pragma region UPCGExSelectorRangeBasedFactoryProviderSettings

UPCGExFactoryData* UPCGExSelectorRangeBasedFactoryProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExSelectorRangeBasedFactoryData* NewFactory = InContext->ManagedObjects->New<UPCGExSelectorRangeBasedFactoryData>();
	NewFactory->BaseConfig = BaseConfig;
	NewFactory->ValueSource = ValueSource;
	NewFactory->SourceMode = SourceMode;
	NewFactory->MinPropertyName = MinPropertyName;
	NewFactory->MaxPropertyName = MaxPropertyName;
	NewFactory->RangePropertyName = RangePropertyName;
	NewFactory->BoundaryMode = BoundaryMode;
	NewFactory->OverlapMode = OverlapMode;
	return Super::CreateFactory(InContext, NewFactory);
}

#if WITH_EDITOR
FString UPCGExSelectorRangeBasedFactoryProviderSettings::GetDisplayName() const
{
	switch (OverlapMode)
	{
	default:
	case EPCGExRangeOverlapMode::WeightedRandom: return TEXT("Select : Range Weighted");
	case EPCGExRangeOverlapMode::FirstMatch:     return TEXT("Select : Range First");
	case EPCGExRangeOverlapMode::NarrowestWins:  return TEXT("Select : Range Narrowest");
	}
}
#endif

#pragma endregion
