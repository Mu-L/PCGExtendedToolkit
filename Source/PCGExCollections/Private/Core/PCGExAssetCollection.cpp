// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Core/PCGExAssetCollection.h"

#include "PCGExLog.h"
#include "PCGExProperty.h"
#include "StaticMeshResources.h"
#include "Algo/RemoveIf.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Helpers/PCGExArrayHelpers.h"
#include "Helpers/PCGExObjectNotifyHelpers.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "Editor.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#endif

bool FPCGExEntryAccessResult::IsType(PCGExAssetCollection::FTypeId TypeId) const
{
	return Entry ? Entry->IsType(TypeId) : false;
}

#pragma region FPCGExAssetStagingData

bool FPCGExAssetStagingData::FindSocket(FName InName, const FPCGExSocket*& OutSocket) const
{
	for (const FPCGExSocket& Socket : Sockets)
	{
		if (Socket.SocketName == InName)
		{
			OutSocket = &Socket;
			return true;
		}
	}
	return false;
}

bool FPCGExAssetStagingData::FindSocket(FName InName, const FString& Tag, const FPCGExSocket*& OutSocket) const
{
	for (const FPCGExSocket& Socket : Sockets)
	{
		if (Socket.SocketName == InName && Socket.Tag == Tag)
		{
			OutSocket = &Socket;
			return true;
		}
	}
	return false;
}

#pragma endregion

namespace PCGExAssetCollection
{
	// Shared helper: sorts Order by Weights ascending, converts Weights to cumulative sums.
	// Used by both FMicroCache::BuildFromWeights() and FCategory::Compile().
	static double CompileWeightedOrder(TArray<int32>& Weights, TArray<int32>& Order)
	{
		PCGExArrayHelpers::ArrayOfIndices(Order, Weights.Num());

		Order.Sort([&Weights](int32 A, int32 B)
		{
			return Weights[A] < Weights[B];
		});
		Weights.Sort([](int32 A, int32 B)
		{
			return A < B;
		});

		double WeightSum = 0;
		for (int32 i = 0; i < Weights.Num(); i++)
		{
			WeightSum += Weights[i];
			Weights[i] = static_cast<int32>(WeightSum);
		}
		return WeightSum;
	}

#pragma region FMicroCache

	int32 FMicroCache::GetPick(int32 Index, EPCGExIndexPickMode PickMode) const
	{
		switch (PickMode)
		{
		default:
		case EPCGExIndexPickMode::Ascending:
			return GetPickAscending(Index);
		case EPCGExIndexPickMode::Descending:
			return GetPickDescending(Index);
		case EPCGExIndexPickMode::WeightAscending:
			return GetPickWeightAscending(Index);
		case EPCGExIndexPickMode::WeightDescending:
			return GetPickWeightDescending(Index);
		}
	}

	int32 FMicroCache::GetPickAscending(int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Index : -1;
	}

	int32 FMicroCache::GetPickDescending(int32 Index) const
	{
		return Order.IsValidIndex(Index) ? (Order.Num() - 1) - Index : -1;
	}

	int32 FMicroCache::GetPickWeightAscending(int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Order[Index] : -1;
	}

	int32 FMicroCache::GetPickWeightDescending(int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Order[(Order.Num() - 1) - Index] : -1;
	}

	int32 FMicroCache::GetPickRandom(int32 Seed) const
	{
		if (Order.IsEmpty())
		{
			return -1;
		}
		return Order[FRandomStream(Seed).RandRange(0, Order.Num() - 1)];
	}

	int32 FMicroCache::GetPickRandomWeighted(int32 Seed) const
	{
		if (Order.IsEmpty())
		{
			return -1;
		}

		const int32 Threshold = FRandomStream(Seed).RandRange(0, static_cast<int32>(WeightSum) - 1);
		int32 Pick = 0;
		while (Pick < Weights.Num() && Weights[Pick] <= Threshold)
		{
			Pick++;
		}
		return Order[FMath::Min(Pick, Order.Num() - 1)];
	}

	void FMicroCache::BuildFromWeights(TConstArrayView<int32> InWeights)
	{
		const int32 NumEntries = InWeights.Num();

		Weights.SetNumUninitialized(NumEntries);
		for (int32 i = 0; i < NumEntries; i++)
		{
			Weights[i] = InWeights[i] + 1; // +1 to ensure non-zero (Weight=0 entries are already excluded by Validate)
		}

		WeightSum = CompileWeightedOrder(Weights, Order);
	}

#pragma endregion

#pragma region FCategory

	int32 FCategory::GetPick(int32 Index, EPCGExIndexPickMode PickMode) const
	{
		switch (PickMode)
		{
		default:
		case EPCGExIndexPickMode::Ascending:
			return GetPickAscending(Index);
		case EPCGExIndexPickMode::Descending:
			return GetPickDescending(Index);
		case EPCGExIndexPickMode::WeightAscending:
			return GetPickWeightAscending(Index);
		case EPCGExIndexPickMode::WeightDescending:
			return GetPickWeightDescending(Index);
		}
	}

	int32 FCategory::GetPickAscending(int32 Index) const
	{
		return Indices.IsValidIndex(Index) ? Indices[Index] : -1;
	}

	int32 FCategory::GetPickDescending(int32 Index) const
	{
		return Indices.IsValidIndex(Index) ? Indices[(Indices.Num() - 1) - Index] : -1;
	}

	int32 FCategory::GetPickWeightAscending(int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Indices[Order[Index]] : -1;
	}

	int32 FCategory::GetPickWeightDescending(int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Indices[Order[(Order.Num() - 1) - Index]] : -1;
	}

	int32 FCategory::GetPickRandom(int32 Seed) const
	{
		if (Order.IsEmpty())
		{
			return -1;
		}
		return Indices[Order[FRandomStream(Seed).RandRange(0, Order.Num() - 1)]];
	}

	int32 FCategory::GetPickRandomWeighted(int32 Seed) const
	{
		if (Order.IsEmpty())
		{
			return -1;
		}
		const int32 Threshold = FRandomStream(Seed).RandRange(0, static_cast<int32>(WeightSum) - 1);
		int32 Pick = 0;
		while (Pick < Weights.Num() && Weights[Pick] <= Threshold)
		{
			Pick++;
		}
		return Indices[Order[FMath::Min(Pick, Order.Num() - 1)]];
	}

	void FCategory::Reserve(int32 InNum)
	{
		Indices.Reserve(InNum);
		Weights.Reserve(InNum);
		Order.Reserve(InNum);
	}

	void FCategory::Shrink()
	{
		Indices.Shrink();
		Weights.Shrink();
		Order.Shrink();
	}

	void FCategory::RegisterEntry(int32 Index, const FPCGExAssetCollectionEntry* InEntry)
	{
		Entries.Add(InEntry);
		const_cast<FPCGExAssetCollectionEntry*>(InEntry)->BuildMicroCache();
		Indices.Add(Index);
		Weights.Add(InEntry->Weight + 1);
	}

	void FCategory::Compile()
	{
		Shrink();
		WeightSum = CompileWeightedOrder(Weights, Order);
	}

#pragma endregion

#pragma region FCache

	FCache::FCache()
	{
		Main = MakeShared<FCategory>(NAME_None);
	}

	// Every valid entry goes into Main. Additionally, entries with a non-None Category
	// are registered to a named sub-category (created on first encounter, assigned a dense
	// index in CategoryNameToIndex). Subsequent entries on the same name reuse that slot.
	void FCache::RegisterEntry(int32 Index, const FPCGExAssetCollectionEntry* InEntry)
	{
		Main->RegisterEntry(Index, InEntry);
		if (const int32* IdxPtr = CategoryNameToIndex.Find(InEntry->Category))
		{
			Categories[*IdxPtr]->RegisterEntry(Index, InEntry);
		}
		else
		{
			TSharedPtr<FCategory> Category = MakeShared<FCategory>(InEntry->Category);
			const int32 NewIdx = Categories.Add(Category);
			CategoryNameToIndex.Add(InEntry->Category, NewIdx);
			Category->RegisterEntry(Index, InEntry);
		}
	}

	void FCache::Compile()
	{
		Main->Compile();
		for (const TSharedPtr<FCategory>& Category : Categories)
		{
			Category->Compile();
		}
	}
#pragma endregion
}

#pragma region FPCGExAssetCollectionEntry

const FPCGExProperty* FPCGExAssetCollectionEntry::GetResolvedPropertyBase(const UPCGExAssetCollection* OwningCollection, FName PropertyName) const
{
	if (const FInstancedStruct* Override = PropertyOverrides.GetOverride(PropertyName))
	{
		if (const FPCGExProperty* Base = Override->GetPtr<FPCGExProperty>())
		{
			return Base;
		}
	}

	if (OwningCollection)
	{
		if (const FInstancedStruct* Default = OwningCollection->CollectionProperties.GetPropertyByName(PropertyName))
		{
			return Default->GetPtr<FPCGExProperty>();
		}
	}

	return nullptr;
}

const FPCGExFittingVariations& FPCGExAssetCollectionEntry::GetVariations(const UPCGExAssetCollection* ParentCollection) const
{
	if (VariationMode == EPCGExEntryVariationMode::Global || ParentCollection->GlobalVariationMode == EPCGExGlobalVariationRule::Overrule)
	{
		return ParentCollection->GlobalVariations;
	}
	return Variations;
}

const FPCGExAssetGrammarDetails* FPCGExAssetCollectionEntry::GetEffectiveGrammar(const UPCGExAssetCollection* Host) const
{
	if (!bIsSubCollection)
	{
		// Leaf: Local vs Global, honoring collection-level Overrule.
		const bool bUseGlobal =
			GrammarSource == EPCGExEntryVariationMode::Global ||
			(Host && Host->GlobalGrammarMode == EPCGExGlobalVariationRule::Overrule);
		return bUseGlobal && Host ? &Host->GlobalAssetGrammar : &AssetGrammar;
	}

	// Subcollection: Inherit / Override / Flatten.
	if (!InternalSubCollection) { return nullptr; }
	switch (SubGrammarMode)
	{
	case EPCGExGrammarSubCollectionMode::Inherit:
		return &InternalSubCollection->SubCollectionGrammar;
	case EPCGExGrammarSubCollectionMode::Override:
		return &AssetGrammar;
	default: // Flatten -- no module emitted for the subcollection itself; leaves contribute directly.
		return nullptr;
	}
}

double FPCGExAssetCollectionEntry::GetGrammarSize(
	const UPCGExAssetCollection* Host,
	const EPCGExGrammarAxes Axis,
	TMap<const FPCGExAssetCollectionEntry*, double>* SizeCache) const
{
	// SizeCache is keyed by entry only -- valid because callers fix a single Axis per pass.
	if (SizeCache)
	{
		if (const double* CachedSize = SizeCache->Find(this))
		{
			return *CachedSize;
		}
	}

	const FPCGExAssetGrammarDetails* Resolved = GetEffectiveGrammar(Host);
	const double Size = !Resolved
		? 0.0
		: (bIsSubCollection
			? Resolved->GetSubCollectionSize(InternalSubCollection, Axis, SizeCache)
			: Resolved->GetLeafSize(Staging.Bounds, Axis));

	if (SizeCache) { SizeCache->Add(this, Size); }
	return Size;
}

bool FPCGExAssetCollectionEntry::FixModuleInfos(
	const UPCGExAssetCollection* Host,
	FPCGSubdivisionSubmodule& OutModule,
	const EPCGExGrammarAxes Axis,
	TMap<const FPCGExAssetCollectionEntry*, double>* SizeCache) const
{
	const FPCGExAssetGrammarDetails* Resolved = GetEffectiveGrammar(Host);
	if (!Resolved) { return false; }
	return bIsSubCollection
		? Resolved->FixSubCollection(InternalSubCollection, Axis, OutModule, SizeCache)
		: Resolved->FixLeaf(Staging.Bounds, Axis, OutModule);
}

#if WITH_EDITOR
void FPCGExAssetCollectionEntry::EDITOR_Sanitize()
{
	// Base implementation - override in derived classes
}
#endif

bool FPCGExAssetCollectionEntry::Validate(const UPCGExAssetCollection* ParentCollection)
{
	if (Weight <= 0)
	{
		return false;
	}

	if (bIsSubCollection)
	{
		if (!InternalSubCollection)
		{
			return false;
		}
		InternalSubCollection->LoadCache();
	}
	return true;
}

namespace
{
	// Aggregate child entry extents per the collection's SubcollectionBoundsMode.
	// Children must have their Staging.Bounds already filled (caller ensures this via the
	// recursive pass before this runs). Invalid or zero-volume children are skipped.
	// Returned box is centered at origin -- center offsets are intentionally not aggregated.
	FBox AggregateSubcollectionBounds(const UPCGExAssetCollection* Child, EPCGExSubcollectionBoundsMode Mode)
	{
		if (!Child)
		{
			return FBox(ForceInit);
		}

		FVector UnionMin(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector UnionMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		FVector MaxExt = FVector::ZeroVector;
		FVector SumExt = FVector::ZeroVector;
		int32 Count = 0;
		FVector WeightedSumExt = FVector::ZeroVector;
		int64 TotalWeight = 0;

		Child->ForEachEntry([&](const FPCGExAssetCollectionEntry* Entry, int32 /*Idx*/)
		{
			if (!Entry)
			{
				return;
			}
			const FBox& ChildBox = Entry->Staging.Bounds;
			if (!ChildBox.IsValid)
			{
				return;
			}

			const FVector ChildExt = ChildBox.GetExtent();
			if (ChildExt.IsNearlyZero())
			{
				return;
			}

			UnionMin = UnionMin.ComponentMin(ChildBox.Min);
			UnionMax = UnionMax.ComponentMax(ChildBox.Max);
			MaxExt = MaxExt.ComponentMax(ChildExt);
			SumExt += ChildExt;
			Count++;

			const int64 W = FMath::Max(1, Entry->Weight);
			WeightedSumExt += ChildExt * static_cast<double>(W);
			TotalWeight += W;
		});

		if (Count == 0)
		{
			return FBox(ForceInit);
		}

		FVector Extents;
		switch (Mode)
		{
		default:
		case EPCGExSubcollectionBoundsMode::UnionAABB:
			// Reconstruct extents from the union min/max, centered at origin.
			Extents = (UnionMax - UnionMin) * 0.5;
			break;
		case EPCGExSubcollectionBoundsMode::MeanExtents:
			Extents = SumExt / static_cast<double>(Count);
			break;
		case EPCGExSubcollectionBoundsMode::WeightedMean:
			Extents = (TotalWeight > 0) ? WeightedSumExt / static_cast<double>(TotalWeight) : (SumExt / static_cast<double>(Count));
			break;
		case EPCGExSubcollectionBoundsMode::MaxExtents:
			Extents = MaxExt;
			break;
		}

		return FBox(-Extents, Extents);
	}
}

void FPCGExAssetCollectionEntry::UpdateStaging(const UPCGExAssetCollection* OwningCollection, int32 InInternalIndex, bool bRecursive)
{
	Staging.InternalIndex = InInternalIndex;

	if (bIsSubCollection)
	{
		Staging.Bounds = FBox(ForceInit);
		if (InternalSubCollection)
		{
			Staging.Path = FSoftObjectPath(InternalSubCollection.GetPathName());
			if (bRecursive)
			{
				InternalSubCollection->RebuildStagingData(true);
			}

			// Aggregate child bounds per the owning collection's policy. Children are now staged
			// (either because bRecursive ran them, or because they were already up-to-date).
			const EPCGExSubcollectionBoundsMode Mode = OwningCollection
				? OwningCollection->SubcollectionBoundsMode
				: EPCGExSubcollectionBoundsMode::UnionAABB;
			Staging.Bounds = AggregateSubcollectionBounds(InternalSubCollection, Mode);
		}
		else
		{
			Staging.Path = FSoftObjectPath{};
		}
	}
}

void FPCGExAssetCollectionEntry::PostUpdateStaging()
{
	// TODO : Update grammar values where relevant
}

void FPCGExAssetCollectionEntry::SetAssetPath(const FSoftObjectPath& InPath)
{
	Staging.Path = InPath;
}

void FPCGExAssetCollectionEntry::GetAssetPaths(TSet<FSoftObjectPath>& OutPaths) const
{
	OutPaths.Emplace(Staging.Path);
}

#if WITH_EDITOR
void FPCGExAssetCollectionEntry::EDITOR_GetSourceAssetPaths(TSet<FSoftObjectPath>& OutPaths) const
{
	// Default: the staged path is the source path for most entry types.
	// Types that bake into an embedded asset (e.g. level → exported data asset)
	// must override to advertise their external source reference instead.
	if (Staging.Path.IsValid())
	{
		OutPaths.Emplace(Staging.Path);
	}
}

FSoftObjectPath FPCGExAssetCollectionEntry::EDITOR_GetThumbnailAssetPath() const
{
	return Staging.Path;
}
#endif

void FPCGExAssetCollectionEntry::BuildMicroCache()
{
	MicroCache = nullptr;
}

void FPCGExAssetCollectionEntry::ClearManagedSockets()
{
	Staging.Sockets.SetNum(Algo::RemoveIf(Staging.Sockets, [](const FPCGExSocket& Socket)
	{
		return Socket.bManaged;
	}));
}

#pragma endregion

// All API methods follow the same pattern: pick from cache → if subcollection, recurse
// into it (using weighted random for the nested pick) → otherwise return entry + host.
// Tag-inheriting variants accumulate tags from the hierarchy as they recurse.
#pragma region API

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryAt(int32 Index) const
{
	FPCGExEntryAccessResult Result;

	const int32 Pick = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPick(Index, EPCGExIndexPickMode::Ascending);
	if (const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(Pick))
	{
		Result.Entry = Entry;
		Result.Host = this;
	}
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryRaw(int32 RawIndex) const
{
	FPCGExEntryAccessResult Result;

	if (const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(RawIndex))
	{
		Result.Entry = Entry;
		Result.Host = this;
	}
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntry(int32 Index, int32 Seed, EPCGExIndexPickMode PickMode) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPick(Index, PickMode);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		return Entry->GetSubCollectionPtr()->GetEntryWeightedRandom(Seed);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryRandom(int32 Seed) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPickRandom(Seed);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		return Entry->GetSubCollectionPtr()->GetEntryRandom(Seed * 2);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryWeightedRandom(int32 Seed) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPickRandomWeighted(Seed);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		return Entry->GetSubCollectionPtr()->GetEntryWeightedRandom(Seed * 2);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

// With tag inheritance

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryAt(int32 Index, uint8 TagInheritance, TSet<FName>& OutTags) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPick(Index, EPCGExIndexPickMode::Ascending);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Collection))
		{
			OutTags.Append(Entry->GetSubCollectionPtr()->CollectionTags);
		}
	}
	if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Asset))
	{
		OutTags.Append(Entry->Tags);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryRaw(int32 RawIndex, uint8 TagInheritance, TSet<FName>& OutTags) const
{
	FPCGExEntryAccessResult Result;

	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(RawIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Collection))
		{
			OutTags.Append(Entry->GetSubCollectionPtr()->CollectionTags);
		}
	}
	if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Asset))
	{
		OutTags.Append(Entry->Tags);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntry(int32 Index, int32 Seed, EPCGExIndexPickMode PickMode, uint8 TagInheritance, TSet<FName>& OutTags) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPick(Index, PickMode);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Hierarchy))
		{
			OutTags.Append(Entry->Tags);
		}
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Collection))
		{
			OutTags.Append(Entry->GetSubCollectionPtr()->CollectionTags);
		}
		return Entry->GetSubCollectionPtr()->GetEntryWeightedRandom(Seed, TagInheritance, OutTags);
	}

	if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Asset))
	{
		OutTags.Append(Entry->Tags);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryRandom(int32 Seed, uint8 TagInheritance, TSet<FName>& OutTags) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPickRandom(Seed);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Hierarchy))
		{
			OutTags.Append(Entry->Tags);
		}
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Collection))
		{
			OutTags.Append(Entry->GetSubCollectionPtr()->CollectionTags);
		}
		return Entry->GetSubCollectionPtr()->GetEntryRandom(Seed * 2, TagInheritance, OutTags);
	}

	if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Asset))
	{
		OutTags.Append(Entry->Tags);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

FPCGExEntryAccessResult UPCGExAssetCollection::GetEntryWeightedRandom(int32 Seed, uint8 TagInheritance, TSet<FName>& OutTags) const
{
	FPCGExEntryAccessResult Result;

	const int32 PickedIndex = const_cast<UPCGExAssetCollection*>(this)->LoadCache()->Main->GetPickRandomWeighted(Seed);
	const FPCGExAssetCollectionEntry* Entry = GetEntryAtRawIndex(PickedIndex);

	if (!Entry)
	{
		return Result;
	}

	if (Entry->HasValidSubCollection())
	{
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Hierarchy))
		{
			OutTags.Append(Entry->Tags);
		}
		if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Collection))
		{
			OutTags.Append(Entry->GetSubCollectionPtr()->CollectionTags);
		}
		return Entry->GetSubCollectionPtr()->GetEntryWeightedRandom(Seed * 2, TagInheritance, OutTags);
	}

	if (TagInheritance & static_cast<uint8>(EPCGExAssetTagInheritance::Asset))
	{
		OutTags.Append(Entry->Tags);
	}

	Result.Entry = Entry;
	Result.Host = this;
	return Result;
}

#pragma endregion

#pragma region Cache

// Thread-safe lazy cache initialization. First checks under read lock (fast path),
// then builds under write lock (inside BuildCacheFromEntries) if needed.
PCGExAssetCollection::FCache* UPCGExAssetCollection::LoadCache()
{
	{
		FReadScopeLock ReadScopeLock(CacheLock);
		if (bCacheNeedsRebuild)
		{
			InvalidateCache();
		}
		if (Cache)
		{
			return Cache.Get();
		}
	}

	BuildCache();
	return Cache.Get();
}

void UPCGExAssetCollection::InvalidateCache()
{
	Cache.Reset();
	bCacheNeedsRebuild = true;
}

void UPCGExAssetCollection::BuildCache()
{
	bCacheNeedsRebuild = false;
	// Per-class implementation calls BuildCacheFromEntries(Entries)
}

#pragma endregion

void UPCGExAssetCollection::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	// Duplicates get a new identity; PIE copies keep the same GUID
	if (!bDuplicateForPIE)
	{
		CollectionGUID = GenerateNewGUID();
	}

#if WITH_EDITOR
	EDITOR_SetDirty();
#endif
}

void UPCGExAssetCollection::PostEditImport()
{
	Super::PostEditImport();

	// Paste/import gets a new identity
	CollectionGUID = GenerateNewGUID();

#if WITH_EDITOR
	EDITOR_SetDirty();
#endif
}

#if WITH_EDITOR
namespace PCGExAssetCollectionMigration
{
	static constexpr int32 CurrentGrammarSchemaVersion = 1;

	/** Migrate one entry's grammar data from v0 to v1. Returns true if a downgrade warning
	 *  should be emitted for this entry (legacy Min/Max/Average on a leaf). */
	static bool MigrateEntryGrammarV0ToV1(FPCGExAssetCollectionEntry* Entry)
	{
		if (!Entry) { return false; }

		bool bWarn = false;
		if (Entry->bIsSubCollection && Entry->SubGrammarMode == EPCGExGrammarSubCollectionMode::Override)
		{
			// Old Override stored its data in CollectionGrammar_DEPRECATED. Hoist it into AssetGrammar.
			Entry->AssetGrammar.MigrateFromLegacyCollectionGrammar(Entry->CollectionGrammar_DEPRECATED);
		}
		else if (!Entry->bIsSubCollection)
		{
			// Leaf: migrate AssetGrammar's internal _DEPRECATED fields.
			bWarn = Entry->AssetGrammar.MigrateFromV0Internal();
		}
		// Subcollection entries with Inherit/Flatten: nothing to migrate at the entry level --
		// the source data lives on the subcollection's own SubCollectionGrammar (migrated by
		// that collection's own PostLoad).
		return bWarn;
	}
}
#endif

void UPCGExAssetCollection::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	// Grammar schema migration. Runs once per collection; subsequent loads no-op.
	if (GrammarSchemaVersion < PCGExAssetCollectionMigration::CurrentGrammarSchemaVersion)
	{
		int32 DowngradedEntries = 0;
		int32 DisabledEntries = 0;

		if (GlobalAssetGrammar.MigrateFromV0Internal()) { DowngradedEntries++; }

		// SubCollectionGrammar is a new v1 field; its source data lives on the legacy CollectionGrammar slot.
		SubCollectionGrammar.MigrateFromLegacyCollectionGrammar(CollectionGrammar_DEPRECATED);

		ForEachEntry([&DowngradedEntries, &DisabledEntries](FPCGExAssetCollectionEntry* Entry, int32 /*Index*/)
		{
			if (PCGExAssetCollectionMigration::MigrateEntryGrammarV0ToV1(Entry)) { DowngradedEntries++; }
			if (Entry && !Entry->bIsSubCollection && Entry->AssetGrammar.Axes == static_cast<uint8>(EPCGExGrammarAxes::None))
			{
				DisabledEntries++;
			}
		});

		if (DowngradedEntries > 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[PCGEx] Grammar migration: %d entr%s in '%s' had legacy Min/Max/Average size mode -- downgraded to X-bounds. Review and reconfigure axes if needed."),
				DowngradedEntries, DowngradedEntries == 1 ? TEXT("y") : TEXT("ies"), *GetName());
		}
		if (DisabledEntries > 0)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[PCGEx] Grammar migration: %d entr%s in '%s' had empty Symbol -- grammar disabled (Axes=None)."),
				DisabledEntries, DisabledEntries == 1 ? TEXT("y") : TEXT("ies"), *GetName());
		}

		GrammarSchemaVersion = PCGExAssetCollectionMigration::CurrentGrammarSchemaVersion;
		
		(void)MarkPackageDirty();
	}
#endif

#if WITH_EDITOR
	// Self-heal HeaderId collisions saved to disk before the dedup pass landed (or introduced
	// later via copy-paste in a build that lacked the runtime fix). MarkPackageDirty only when
	// something actually changed, so clean assets stay clean on load.
	if (SyncPropertySchemaAndRemapEntries())
	{
		(void)MarkPackageDirty();
	}

	// Defer to next tick: the rebuild cascades into UpdateStaging -> SpawnActor, which is
	// unsafe during PostLoad (load chain may re-enter, GWorld mid-transition). Trade-off:
	// a PCG graph that triggered THIS soft-load sees pre-rebuild state for its current
	// run; subsequent runs see fresh data. Complements OnAssetUpdatedOnDisk which only
	// fires for collections already loaded when a referenced asset is saved.
	if (!GEditor || !GEditor->IsTimerManagerValid() || !bAutoRebuildStaging)
	{
		return;
	}

	TWeakObjectPtr<UPCGExAssetCollection> WeakThis(this);
	GEditor->GetTimerManager()->SetTimerForNextTick(
		[WeakThis]()
		{
			if (UPCGExAssetCollection* This = WeakThis.Get())
			{
				This->EDITOR_RebuildStaleEntries();
			}
		});
#endif
}

void UPCGExAssetCollection::BeginDestroy()
{
	InvalidateCache();
	Super::BeginDestroy();
}

void UPCGExAssetCollection::RebuildStagingData(bool bRecursive)
{
	ForEachEntry([this, bRecursive](FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		InEntry->UpdateStaging(this, i, bRecursive);
		InEntry->PostUpdateStaging();
	});
	InvalidateCache();
}

void UPCGExAssetCollection::EDITOR_RegisterTrackingKeys(FPCGExContext* Context) const
{
	Context->EDITOR_TrackPath(this);
	ForEachEntry([Context](const FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		if (!InEntry->bIsSubCollection)
		{
			return;
		}
		if (const UPCGExAssetCollection* SubCollection = InEntry->GetSubCollectionPtr())
		{
			SubCollection->EDITOR_RegisterTrackingKeys(Context);
		}
	});
}

bool UPCGExAssetCollection::HasCircularDependency(const UPCGExAssetCollection* OtherCollection) const
{
	if (!OtherCollection)
	{
		return false;
	}
	if (OtherCollection == this)
	{
		return true;
	}

	TSet<const UPCGExAssetCollection*> References;
	return OtherCollection->HasCircularDependency(References);
}

bool UPCGExAssetCollection::HasCircularDependency(TSet<const UPCGExAssetCollection*>& InReferences) const
{
	bool bCircularDependency = false;
	InReferences.Add(this, &bCircularDependency);

	if (bCircularDependency)
	{
		return true;
	}

	ForEachEntry([&](const FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		if (bCircularDependency)
		{
			return;
		}
		if (const UPCGExAssetCollection* Other = InEntry->GetSubCollectionPtr())
		{
			bCircularDependency = Other->HasCircularDependency(InReferences);
		}
	});

	return bCircularDependency;
}

void UPCGExAssetCollection::GetAssetPaths(TSet<FSoftObjectPath>& OutPaths, PCGExAssetCollection::ELoadingFlags Flags) const
{
	const bool bCollectionOnly = Flags == PCGExAssetCollection::ELoadingFlags::RecursiveCollectionsOnly;
	const bool bRecursive = bCollectionOnly || Flags == PCGExAssetCollection::ELoadingFlags::Recursive;

	ForEachEntry([&](const FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		if (InEntry->bIsSubCollection)
		{
			if (bRecursive || bCollectionOnly)
			{
				if (InEntry->InternalSubCollection)
				{
					InEntry->InternalSubCollection->GetAssetPaths(OutPaths, Flags);
				}
			}
			return;
		}
		if (bCollectionOnly)
		{
			return;
		}

		InEntry->GetAssetPaths(OutPaths);
	});
}

#if WITH_EDITOR
void UPCGExAssetCollection::GetCookDependencyAssetPaths(TSet<FSoftObjectPath>& OutPaths) const
{
	GetAssetPaths(OutPaths, PCGExAssetCollection::ELoadingFlags::Recursive);
}
#endif

void UPCGExAssetCollection::RefreshCollectionPropertiesFromEntries(
	EPCGExSchemaMergePolicy Policy,
	TConstArrayView<FInstancedStruct> InheritedDefaults)
{
	// Source ordering (see header for full reasoning):
	//   1. InheritedDefaults (caller-computed common-ancestor view) -- wins under FirstWins.
	//   2. Per-entry contributors (each entry's enabled override slots).
	//   3. Existing CollectionProperties -- loses to both above; survives only for
	//      manual-only schema entries.
	SyncPropertySchemaAndRemapEntries();

	TArray<TArray<FInstancedStruct>> Sources;
	Sources.Reserve(2 + NumEntries());

	// Source #0: caller-supplied inherited defaults. Empty when the caller has no chain context
	// (manual collection rebuilds, etc.) -- in that case the merge falls through to contributors.
	if (!InheritedDefaults.IsEmpty())
	{
		Sources.Add(TArray<FInstancedStruct>(InheritedDefaults));
	}

	// One source per entry whose enabled overrides carry self-contained (name+type+value)
	// FInstancedStructs -- the same shape BuildSchema produces on the collection side, so
	// they double as schema declarations for the union.
	ForEachEntry([&Sources](const FPCGExAssetCollectionEntry* InEntry, int32 /*Idx*/)
	{
		if (!InEntry)
		{
			return;
		}
		TArray<FInstancedStruct> EntrySource;
		for (const FPCGExPropertyOverrideEntry& Slot : InEntry->PropertyOverrides.Overrides)
		{
			if (Slot.bEnabled && Slot.Value.IsValid())
			{
				EntrySource.Add(Slot.Value);
			}
		}
		if (!EntrySource.IsEmpty())
		{
			Sources.Add(MoveTemp(EntrySource));
		}
	});

	// Existing manual schema appended LAST -- loses on name collision under FirstWins, survives as
	// the sole source for properties no entry contributes.
	Sources.Add(CollectionProperties.BuildSchema());

	// Nothing authored anywhere: leave existing state untouched (avoids no-op churn).
	if (Sources.Num() == 1 && Sources.Last().IsEmpty())
	{
		return;
	}

	const PCGExProperties::FSchemaMergeResult MergeResult = PCGExProperties::MergeSchemas(Sources, Policy);
	PCGExProperties::LogSchemaConflicts(MergeResult, this);
	PCGExProperties::ApplyMergeResultToSchemas(CollectionProperties, MergeResult.Merged);

	// Overrides may have arrived from heterogenous sources (e.g. per-actor components whose
	// schemas had their own HeaderIds), so SyncToSchema's HeaderId match would miss and
	// reset values to defaults. Realign HeaderIds by name first.
	TArray<FInstancedStruct> CanonicalSchema = CollectionProperties.BuildSchema();

#if WITH_EDITOR
	TMap<FName, int32> CanonicalHeaderIdsByName;
	CanonicalHeaderIdsByName.Reserve(CanonicalSchema.Num());
	for (const FInstancedStruct& SchemaProp : CanonicalSchema)
	{
		if (const FPCGExProperty* P = SchemaProp.GetPtr<FPCGExProperty>())
		{
			if (P->HeaderId != 0 && !P->PropertyName.IsNone())
			{
				CanonicalHeaderIdsByName.Add(P->PropertyName, P->HeaderId);
			}
		}
	}
#endif

	ForEachEntry([&CanonicalSchema
#if WITH_EDITOR
				, &CanonicalHeaderIdsByName
#endif
			](FPCGExAssetCollectionEntry* InEntry, int32 /*Idx*/)
	{
		if (!InEntry)
		{
			return;
		}
#if WITH_EDITOR
		for (FPCGExPropertyOverrideEntry& Slot : InEntry->PropertyOverrides.Overrides)
		{
			if (FPCGExProperty* P = Slot.GetPropertyMutable())
			{
				if (const int32* CanonicalId = CanonicalHeaderIdsByName.Find(P->PropertyName))
				{
					P->HeaderId = *CanonicalId;
				}
			}
		}
#endif
		InEntry->PropertyOverrides.SyncToSchema(CanonicalSchema);
	});

	RebuildPropertyRegistry();
}

bool UPCGExAssetCollection::SyncPropertySchemaAndRemapEntries()
{
	bool bRemapped = false;
	CollectionProperties.SyncAllSchemasAndRemap([this, &bRemapped](TConstArrayView<FPCGExHeaderIdRemap> Remaps)
	{
		bRemapped = true;
		ForEachEntry([&Remaps](FPCGExAssetCollectionEntry* InEntry, int32 /*i*/)
		{
			InEntry->PropertyOverrides.ApplyHeaderIdRemap(Remaps);
		});
	});
	return bRemapped;
}

#if WITH_EDITOR
void UPCGExAssetCollection::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Cheap recovery pass: the same FPCGExAssetGrammarDetails struct is reused across leaf and
	// subcollection contexts, and the editor customization filters Size enum options per context.
	// When the user flips bIsSubCollection (or SubGrammarMode to/from Override) the stored Size
	// can leave the valid set -- snap it back to Fixed. Also enforce fixed contexts on the two
	// collection-level grammar slots.
	GlobalAssetGrammar.ValidateContext(/*bIsSubCollection=*/false);
	SubCollectionGrammar.ValidateContext(/*bIsSubCollection=*/true);
	ForEachEntry([](FPCGExAssetCollectionEntry* Entry, int32 /*Index*/)
	{
		if (Entry) { Entry->AssetGrammar.ValidateContext(Entry->bIsSubCollection); }
	});

	bool bNeedsSync = false;
	bool bNeedsUIRefresh = false;

	if (PropertyChangedEvent.MemberProperty)
	{
		FName PropName = PropertyChangedEvent.MemberProperty->GetFName();
		EPropertyChangeType::Type ChangeType = PropertyChangedEvent.ChangeType;

		// Check for ANY changes in CollectionProperties
		if (PropName == GET_MEMBER_NAME_CHECKED(UPCGExAssetCollection, CollectionProperties))
		{
			bNeedsSync = true;
			bNeedsUIRefresh = true;
		}
		// Also catch changes to schema array elements (add/remove/reorder/rename/type change)
		else if (PropertyChangedEvent.MemberProperty->GetOwnerStruct() == FPCGExPropertySchema::StaticStruct() ||
			PropertyChangedEvent.MemberProperty->GetOwnerStruct() == FPCGExPropertySchemaCollection::StaticStruct())
		{
			bNeedsSync = true;
			bNeedsUIRefresh = true;
		}
	}

	// Early return if no property-related changes needed
	if (!bNeedsSync && !bNeedsUIRefresh)
	{
		Super::PostEditChangeProperty(PropertyChangedEvent);

		ForEachEntry([this](FPCGExAssetCollectionEntry* InEntry, int32 i)
		{
			const UPCGExAssetCollection* Other = InEntry->GetSubCollectionPtr();
			if (Other && HasCircularDependency(Other))
			{
				UE_LOG(LogTemp, Error, TEXT("Prevented circular dependency trying to nest \"%s\" inside \"%s\""), *GetNameSafe(Other), *GetNameSafe(this));
				InEntry->ClearSubCollection();
				(void)MarkPackageDirty();
			}
		});

		EDITOR_SetDirty();

		if (bAutoRebuildStaging)
		{
			EDITOR_RebuildStagingData();
		}

		return;
	}

	// Sync and rebuild if needed
	if (bNeedsSync)
	{
		RebuildPropertyRegistry();
		SyncPropertyOverridesToEntries();
	}

	(void)MarkPackageDirty();

	Super::PostEditChangeProperty(PropertyChangedEvent);

#if WITH_EDITOR
	// Force all details panels showing this object to rebuild
	// This ensures nested PropertyOverrides customizations detect the schema changes
	if (bNeedsSync)
	{
		FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyEditorModule.NotifyCustomizationModuleChanged();
	}
#endif

	ForEachEntry([this](FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		const UPCGExAssetCollection* Other = InEntry->GetSubCollectionPtr();
		if (Other && HasCircularDependency(Other))
		{
			UE_LOG(LogTemp, Error, TEXT("Prevented circular dependency trying to nest \"%s\" inside \"%s\""), *GetNameSafe(Other), *GetNameSafe(this));
			InEntry->ClearSubCollection();
			(void)MarkPackageDirty();
		}
	});

	EDITOR_SetDirty();

	if (bAutoRebuildStaging)
	{
		EDITOR_RebuildStagingData();
	}
}

void UPCGExAssetCollection::SyncPropertyOverridesToEntries()
{
	// Remap must happen before the SyncToSchema loop below -- SyncToSchema's HeaderId index
	// aliases collided entries otherwise, and one side's authored values fall through to
	// schema defaults.
	SyncPropertySchemaAndRemapEntries();

	TArray<FInstancedStruct> Schema = CollectionProperties.BuildSchema();
	ForEachEntry([&Schema](FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		InEntry->PropertyOverrides.SyncToSchema(Schema);
	});
}

void UPCGExAssetCollection::EDITOR_RebuildStagingData()
{
	Modify(true);
	InvalidateCache();
	EDITOR_SanitizeAndRebuildStagingData(false);
	LastRebuiltUtc = FDateTime::UtcNow();
	(void)MarkPackageDirty();
	PCGExEditor::NotifyObjectChanged(this);
	if (EDITOR_PostStagingRebuildSuppressDepth == 0)
	{
		EDITOR_OnPostStagingRebuild();
	}
}

void UPCGExAssetCollection::EDITOR_RebuildStagingData_Recursive()
{
	Modify(true);
	InvalidateCache();
	EDITOR_SanitizeAndRebuildStagingData(true);
	LastRebuiltUtc = FDateTime::UtcNow();
	(void)MarkPackageDirty();
	PCGExEditor::NotifyObjectChanged(this);
	if (EDITOR_PostStagingRebuildSuppressDepth == 0)
	{
		EDITOR_OnPostStagingRebuild();
	}
}

int32 UPCGExAssetCollection::EDITOR_RebuildStaleEntries()
{
	if (!bAutoRebuildStaging)
	{
		return 0;
	}
	// No baseline -- pre-existing collection that hasn't had a tracked rebuild yet. Skip
	// rather than treating every entry as stale (which would mass-rebuild on first open
	// after upgrade and risk silently changing serialised bounds).
	if (LastRebuiltUtc == FDateTime::MinValue())
	{
		return 0;
	}

	TArray<int32> StaleIndices;
	ForEachEntry([this, &StaleIndices](const FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		if (InEntry->bIsSubCollection)
		{
			return;
		}
		// Use EDITOR_GetSourceAssetPaths so entry types that bake into an embedded asset
		// (e.g. PCGDataAsset collection entries with Source==Level) check the real source
		// file rather than Staging.Path, which resolves to the collection's own package.
		TSet<FSoftObjectPath> SourcePaths;
		InEntry->EDITOR_GetSourceAssetPaths(SourcePaths);

		for (const FSoftObjectPath& Path : SourcePaths)
		{
			if (!Path.IsValid())
			{
				continue;
			}
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Path.GetLongPackageName(), Filename))
			{
				continue;
			}
			const FString UAsset = Filename + FPackageName::GetAssetPackageExtension();
			const FString UMap = Filename + FPackageName::GetMapPackageExtension();
			FDateTime AssetTime = IFileManager::Get().GetTimeStamp(*UAsset);
			if (AssetTime == FDateTime::MinValue())
			{
				AssetTime = IFileManager::Get().GetTimeStamp(*UMap);
			}
			if (AssetTime == FDateTime::MinValue())
			{
				continue;
			}
			if (AssetTime > LastRebuiltUtc)
			{
				StaleIndices.Add(i);
				break;
			}
		}
	});

	{
		// Suppress per-entry post-rebuild hook firings; emit one tail call after the batch.
		TGuardValue<int32> SuppressGuard(EDITOR_PostStagingRebuildSuppressDepth, EDITOR_PostStagingRebuildSuppressDepth + 1);
		for (int32 Index : StaleIndices)
		{
			EDITOR_RebuildEntryStaging(Index);
		}
	}
	if (!StaleIndices.IsEmpty())
	{
		EDITOR_OnPostStagingRebuild();
	}
	return StaleIndices.Num();
}

bool UPCGExAssetCollection::EDITOR_RebuildEntryStaging(int32 EntryIndex)
{
	if (!bAutoRebuildStaging)
	{
		return false;
	}

	bool bRebuilt = false;
	ForEachEntry([this, EntryIndex, &bRebuilt](FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		if (i != EntryIndex)
		{
			return;
		}
		Modify(true);
		InEntry->EDITOR_Sanitize();
		InEntry->UpdateStaging(this, i, false);
		InEntry->PostUpdateStaging();
		bRebuilt = true;
	});

	if (bRebuilt)
	{
		InvalidateCache();
		(void)MarkPackageDirty();
		PCGExEditor::NotifyObjectChanged(this);
		if (EDITOR_PostStagingRebuildSuppressDepth == 0)
		{
			EDITOR_OnPostStagingRebuild();
		}
	}
	return bRebuilt;
}

void UPCGExAssetCollection::EDITOR_RebuildStagingData_Project()
{
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssets(Filter, AssetDataList);

	for (const FAssetData& AssetData : AssetDataList)
	{
		if (UPCGExAssetCollection* Collection = Cast<UPCGExAssetCollection>(AssetData.GetAsset()))
		{
			Collection->EDITOR_RebuildStagingData();
		}
	}
}

void UPCGExAssetCollection::EDITOR_SanitizeAndRebuildStagingData(bool bRecursive)
{
	ForEachEntry([this, bRecursive](FPCGExAssetCollectionEntry* InEntry, int32 i)
	{
		InEntry->EDITOR_Sanitize();
		InEntry->UpdateStaging(this, i, bRecursive);
		InEntry->PostUpdateStaging();
	});
}

void UPCGExAssetCollection::EDITOR_AddBrowserSelectionTyped(const TArray<FAssetData>& InAssetData)
{
	FScopedTransaction Transaction(INVTEXT("Add Browser Selection to Collection"));
	Modify(true);

	// Partition: assets that are themselves a collection of (a subclass of) this collection's
	// own class become subcollection entries; everything else falls through to the type-specific
	// EDITOR_AddBrowserSelectionInternal. Resolving GetAsset() loads the package -- fine here
	// since this only runs from user-driven editor actions (drag-drop / browser selection).
	UClass* OwnClass = GetClass();
	TArray<FAssetData> RegularAssets;
	TArray<UPCGExAssetCollection*> SubCollectionAssets;
	RegularAssets.Reserve(InAssetData.Num());

	for (const FAssetData& AssetData : InAssetData)
	{
		UClass* AssetClass = AssetData.GetClass();
		if (!AssetClass || !AssetClass->IsChildOf(OwnClass))
		{
			RegularAssets.Add(AssetData);
			continue;
		}

		if (UPCGExAssetCollection* Sub = Cast<UPCGExAssetCollection>(AssetData.GetAsset()))
		{
			SubCollectionAssets.Add(Sub);
			continue;
		}

		RegularAssets.Add(AssetData);
	}

	if (!SubCollectionAssets.IsEmpty())
	{
		EDITOR_AddSubCollectionEntries(SubCollectionAssets);
	}

	if (!RegularAssets.IsEmpty())
	{
		EDITOR_AddBrowserSelectionInternal(RegularAssets);
	}

	SyncPropertyOverridesToEntries();
	(void)MarkPackageDirty();
	FCoreUObjectDelegates::BroadcastOnObjectModified(this);
}

void UPCGExAssetCollection::EDITOR_AddSubCollectionEntries(const TArray<UPCGExAssetCollection*>& InSubCollections)
{
	if (InSubCollections.IsEmpty())
	{
		return;
	}

	// Reflection on the entry struct. Every entry type in this plugin exposes a
	// `bIsSubCollection` bool (defined on FPCGExAssetCollectionEntry) and a typed
	// `SubCollection` UPROPERTY -- this helper relies on those names.
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(GetClass()->FindPropertyByName(FName("Entries")));
	if (!ArrayProp)
	{
		return;
	}

	FStructProperty* InnerProp = CastField<FStructProperty>(ArrayProp->Inner);
	if (!InnerProp || !InnerProp->Struct)
	{
		return;
	}
	UScriptStruct* EntryStruct = InnerProp->Struct;

	FBoolProperty* IsSubProp = CastField<FBoolProperty>(EntryStruct->FindPropertyByName(FName("bIsSubCollection")));
	FObjectProperty* SubCollProp = CastField<FObjectProperty>(EntryStruct->FindPropertyByName(FName("SubCollection")));
	if (!IsSubProp || !SubCollProp || !SubCollProp->PropertyClass)
	{
		return;
	}

	void* ArrayData = ArrayProp->ContainerPtrToValuePtr<void>(this);
	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayData);

	// Build a set of subcollections already referenced by existing subcollection entries
	// so drag-dropping the same asset twice doesn't create duplicates -- matches the
	// dedupe behavior of MeshCollection / ActorCollection / PCGDataAssetCollection's
	// EDITOR_AddBrowserSelectionInternal implementations.
	TSet<const UPCGExAssetCollection*> AlreadyReferenced;
	const int32 ExistingNum = ArrayHelper.Num();
	const int32 IsSubOffset = IsSubProp->GetOffset_ForInternal();
	const int32 SubCollOffset = SubCollProp->GetOffset_ForInternal();

	for (int32 i = 0; i < ExistingNum; ++i)
	{
		const uint8* EntryPtr = ArrayHelper.GetRawPtr(i);
		if (IsSubProp->GetPropertyValue(EntryPtr + IsSubOffset))
		{
			if (const UObject* Existing = SubCollProp->GetObjectPropertyValue(EntryPtr + SubCollOffset))
			{
				AlreadyReferenced.Add(Cast<UPCGExAssetCollection>(Existing));
			}
		}
	}

	for (UPCGExAssetCollection* Sub : InSubCollections)
	{
		if (!Sub || Sub == this)
		{
			continue;
		}
		if (!Sub->GetClass()->IsChildOf(SubCollProp->PropertyClass))
		{
			continue;
		}
		if (AlreadyReferenced.Contains(Sub))
		{
			continue;
		}
		if (HasCircularDependency(Sub))
		{
			continue;
		}

		const int32 NewIdx = ArrayHelper.AddValue();
		uint8* EntryPtr = ArrayHelper.GetRawPtr(NewIdx);
		IsSubProp->SetPropertyValue(EntryPtr + IsSubOffset, true);
		SubCollProp->SetObjectPropertyValue(EntryPtr + SubCollOffset, Sub);
		AlreadyReferenced.Add(Sub);
	}
}

void UPCGExAssetCollection::EDITOR_AddBrowserSelectionInternal(const TArray<FAssetData>& InAssetData)
{
	// Override in derived classes
}
#endif
