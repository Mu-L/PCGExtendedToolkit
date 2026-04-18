// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Selectors/PCGExSelectorRangeBased.h"

#include "Core/PCGExAssetCollection.h"
#include "Core/PCGExContext.h"
#include "Data/PCGExData.h"
#include "Details/PCGExSettingsDetails.h"
#include "PCGExPropertyTypes.h"

namespace
{
	constexpr double RangeTieEpsilon = 1e-6;

	// Resolve a numeric property on an entry as double, walking the accepted-type ladder
	// (Double -> Float -> Int32 -> Int64). Returns true on first successful resolution.
	bool ResolveNumericAsDouble(const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Collection, FName Name, double& OutValue)
	{
		if (const FPCGExProperty_Double* P = Entry->GetResolvedProperty<FPCGExProperty_Double>(Collection, Name)) { OutValue = P->Value; return true; }
		if (const FPCGExProperty_Float* P = Entry->GetResolvedProperty<FPCGExProperty_Float>(Collection, Name)) { OutValue = static_cast<double>(P->Value); return true; }
		if (const FPCGExProperty_Int32* P = Entry->GetResolvedProperty<FPCGExProperty_Int32>(Collection, Name)) { OutValue = static_cast<double>(P->Value); return true; }
		if (const FPCGExProperty_Int64* P = Entry->GetResolvedProperty<FPCGExProperty_Int64>(Collection, Name)) { OutValue = static_cast<double>(P->Value); return true; }
		return false;
	}

	// Resolve a Vector2D-style range property on an entry. Accepts Vector2 natively and Vector
	// (Z component ignored). Returns true on first successful resolution.
	bool ResolveRangeFromVector(const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Collection, FName Name, double& OutMin, double& OutMax)
	{
		if (const FPCGExProperty_Vector2* P = Entry->GetResolvedProperty<FPCGExProperty_Vector2>(Collection, Name)) { OutMin = P->Value.X; OutMax = P->Value.Y; return true; }
		if (const FPCGExProperty_Vector* P = Entry->GetResolvedProperty<FPCGExProperty_Vector>(Collection, Name)) { OutMin = P->Value.X; OutMax = P->Value.Y; return true; }
		return false;
	}
}

#pragma region FPCGExEntryRangeBasedPickerOpBase

bool FPCGExEntryRangeBasedPickerOpBase::PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection)
{
	if (!FPCGExEntryPickerOperation::PrepareForData(InContext, InDataFacade, InTarget, InOwningCollection)) { return false; }
	if (!OwningCollection) { return false; }

	ValueGetter = ValueSource.GetValueSetting();
	if (!ValueGetter->Init(InDataFacade)) { return false; }

	const int32 N = Target->Entries.Num();
	EntryMins.SetNumUninitialized(N);
	EntryMaxs.SetNumUninitialized(N);

	// Sentinel range that never matches any value (Contains() returns false for all Boundary modes).
	auto MarkInvalid = [&](int32 i) { EntryMins[i] = 1.0; EntryMaxs[i] = -1.0; };

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
			bResolved = ResolveNumericAsDouble(Entry, OwningCollection, MinPropertyName, Min)
				&& ResolveNumericAsDouble(Entry, OwningCollection, MaxPropertyName, Max);
			break;
		case EPCGExRangeSourceMode::Vector2:
			bResolved = ResolveRangeFromVector(Entry, OwningCollection, RangePropertyName, Min, Max);
			break;
		}

		if (!bResolved) { MarkInvalid(i); continue; }

		// Auto-swap out-of-order ranges — matches PCGEx convention for numeric range inputs.
		if (Min > Max) { Swap(Min, Max); }
		EntryMins[i] = Min;
		EntryMaxs[i] = Max;
		++ResolvedCount;
	}

	if (ResolvedCount == 0)
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Selector : Range-Based — no entries resolved the referenced range property. Check property names and types in the collection."));
		return false;
	}

	// Cache (Weight + 1) as double for the hot path.
	EntryWeights.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		EntryWeights[i] = (EntryMins[i] <= EntryMaxs[i])
			? static_cast<double>(Target->Entries[i]->Weight + 1)
			: 0.0;
	}

	// Build sorted view over valid entries (invalid entries have Min > Max sentinel).
	SortedIndices.Reserve(ResolvedCount);
	for (int32 i = 0; i < N; ++i)
	{
		if (EntryMins[i] <= EntryMaxs[i]) { SortedIndices.Add(i); }
	}
	SortedIndices.Sort([this](int32 A, int32 B) { return EntryMins[A] < EntryMins[B]; });

	const int32 NValid = SortedIndices.Num();
	SortedMins.SetNumUninitialized(NValid);
	for (int32 k = 0; k < NValid; ++k) { SortedMins[k] = EntryMins[SortedIndices[k]]; }

	// Strict non-overlap detection — any adjacent pair with next_Min <= prev_Max disables the fast path.
	// Strictness avoids the shared-endpoint ambiguity (same V may land in two ranges under Closed boundaries).
	bNonOverlapping = true;
	for (int32 k = 1; k < NValid; ++k)
	{
		const double PrevMax = EntryMaxs[SortedIndices[k - 1]];
		const double CurMin = EntryMins[SortedIndices[k]];
		if (CurMin <= PrevMax) { bNonOverlapping = false; break; }
	}

	return true;
}

// Binary-search fast path for non-overlapping range layouts. V can match at most one range,
// so all three overlap modes collapse to the same lookup. Returns raw Target index or -1.
int32 FPCGExEntryRangeBasedPickerOpBase::FastPathPick(double V) const
{
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
	return Contains(V, EntryMins[i], EntryMaxs[i]) ? Target->Indices[i] : -1;
}

#pragma endregion

#pragma region FPCGExEntryRangeWeightedRandomPickerOp

int32 FPCGExEntryRangeWeightedRandomPickerOp::Pick(int32 PointIndex, int32 Seed) const
{
	if (!Target || Target->IsEmpty()) { return -1; }

	const double V = ValueGetter->Read(PointIndex);

	if (bNonOverlapping) { return FastPathPick(V); }

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
	if (bNonOverlapping) { return FastPathPick(V); }

	// Overlapping case: preserve "first in category order" semantics by iterating unsorted.
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

	if (bNonOverlapping) { return FastPathPick(V); }

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

	NewOp->SourceMode = SourceMode;
	NewOp->BoundaryMode = BoundaryMode;
	NewOp->ValueSource = ValueSource;
	NewOp->MinPropertyName = MinPropertyName;
	NewOp->MaxPropertyName = MaxPropertyName;
	NewOp->RangePropertyName = RangePropertyName;
	return NewOp;
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
