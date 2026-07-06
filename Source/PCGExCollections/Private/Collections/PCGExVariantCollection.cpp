// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Collections/PCGExVariantCollection.h"

#include "PCGExLog.h"
#include "Helpers/PCGExStreamingHelpers.h"
#include "UObject/ObjectSaveContext.h"

PCGEX_REGISTER_COLLECTION_TYPE(Variant, UPCGExVariantCollection, FPCGExAssetCollectionEntry, "Variant Collection", Base)

namespace PCGExVariantCollection
{
	// Order-sensitive digest of the source's EntryIds in raw order. Catches same-count
	// reorders that the entry-count stamp alone would miss.
	inline uint32 ComputeSourceOrderHash(const UPCGExAssetCollection* InSource)
	{
		uint32 Hash = 0;
		InSource->ForEachEntry([&Hash](const FPCGExAssetCollectionEntry* Entry, int32)
		{
			Hash = HashCombineFast(Hash, GetTypeHash(Entry->EntryId));
		});
		return Hash;
	}
}

bool UPCGExVariantCollection::IsValidIndex(const int32 InIndex) const
{
	return InIndex >= 0 && InIndex < NumEntries();
}

int32 UPCGExVariantCollection::NumEntries() const
{
	int32 Total = 0;
	for (const FPCGExVariantSource& Group : Sources)
	{
		Total += Group.Overrides.Num();
	}
	return Total;
}

void UPCGExVariantCollection::BuildCache()
{
	// Flatten to base pointers in declaration order. Unset rows contribute a null — tolerated
	// by BuildCacheFromEntryPtrs — so raw indices stay stable under partial authoring.
	TArray<FPCGExAssetCollectionEntry*> EntryPtrs;
	EntryPtrs.Reserve(NumEntries());

	for (FPCGExVariantSource& Group : Sources)
	{
		for (FPCGExVariantEntryOverride& Row : Group.Overrides)
		{
			EntryPtrs.Add(Row.Entry.GetMutablePtr<FPCGExAssetCollectionEntry>());
		}
	}

	BuildCacheFromEntryPtrs(EntryPtrs);
}

void UPCGExVariantCollection::ForEachEntry(FForEachConstEntryFunc Iterator) const
{
	int32 FlatIndex = 0;
	for (const FPCGExVariantSource& Group : Sources)
	{
		for (const FPCGExVariantEntryOverride& Row : Group.Overrides)
		{
			// Unset rows are skipped but still consume their flat index — iterator
			// consumers rely on the index being the raw entry index.
			if (const FPCGExAssetCollectionEntry* Entry = Row.Entry.GetPtr<FPCGExAssetCollectionEntry>())
			{
				Iterator(Entry, FlatIndex);
			}
			FlatIndex++;
		}
	}
}

void UPCGExVariantCollection::ForEachEntry(FForEachEntryFunc Iterator)
{
	int32 FlatIndex = 0;
	for (FPCGExVariantSource& Group : Sources)
	{
		for (FPCGExVariantEntryOverride& Row : Group.Overrides)
		{
			if (FPCGExAssetCollectionEntry* Entry = Row.Entry.GetMutablePtr<FPCGExAssetCollectionEntry>())
			{
				Iterator(Entry, FlatIndex);
			}
			FlatIndex++;
		}
	}
}

void UPCGExVariantCollection::SyncVariantMappings()
{
	int32 FlatOffset = 0;

	for (FPCGExVariantSource& Group : Sources)
	{
		const int32 GroupCount = Group.Overrides.Num();

		Group.BakedPairs.Reset();
		Group.SourceNumEntriesAtBake = -1;
		Group.SourceOrderHashAtBake = 0;
		Group.SourceGUIDAtBake = 0;

		if (Group.Source.IsNull())
		{
			FlatOffset += GroupCount;
			continue;
		}

		PCGExHelpers::LoadBlocking_AnyThreadTpl(Group.Source);
		const UPCGExAssetCollection* Src = Group.Source.Get();

		if (!Src)
		{
			UE_LOG(LogPCGEx, Warning, TEXT("[%s] Variant source '%s' could not be loaded — group mapping not baked."),
			       *GetName(), *Group.Source.ToSoftObjectPath().ToString());
			FlatOffset += GroupCount;
			continue;
		}

		// EntryId -> raw index lookup over the live source
		TMap<int32, int32> IdToRawIndex;
		IdToRawIndex.Reserve(Src->NumEntries());
		Src->ForEachEntry([&IdToRawIndex](const FPCGExAssetCollectionEntry* Entry, const int32 RawIndex)
		{
			if (Entry->EntryId != 0)
			{
				IdToRawIndex.Add(Entry->EntryId, RawIndex);
			}
		});

		if (IdToRawIndex.IsEmpty() && Src->NumEntries() > 0)
		{
			UE_LOG(LogPCGEx, Warning, TEXT("[%s] Variant source '%s' has no EntryIds — it was never staging-rebuilt since EntryId support landed. Rebuild & save the source, then re-save this variant."),
			       *GetName(), *Src->GetName());
		}

		for (int32 o = 0; o < GroupCount; o++)
		{
			const FPCGExVariantEntryOverride& Row = Group.Overrides[o];
			if (Row.SourceEntryId == 0 || !Row.Entry.IsValid())
			{
				continue;
			}

			if (const int32* SourceRawIndex = IdToRawIndex.Find(Row.SourceEntryId))
			{
				Group.BakedPairs.Emplace(*SourceRawIndex, FlatOffset + o);
			}
			else
			{
				UE_LOG(LogPCGEx, Warning, TEXT("[%s] Variant row %d for source '%s' references EntryId %d which no longer exists — orphaned row skipped."),
				       *GetName(), o, *Src->GetName(), Row.SourceEntryId);
			}
		}

		Group.SourceNumEntriesAtBake = Src->NumEntries();
		Group.SourceOrderHashAtBake = PCGExVariantCollection::ComputeSourceOrderHash(Src);
		Group.SourceGUIDAtBake = Src->GetCollectionGUID();

		FlatOffset += GroupCount;
	}
}

bool UPCGExVariantCollection::IsMappingStale(const FPCGExVariantSource& InSourceGroup) const
{
	const UPCGExAssetCollection* Src = InSourceGroup.Source.Get();
	if (!Src)
	{
		// Unloaded source — cannot verify, report not-stale rather than blocking consumers.
		return false;
	}

	return Src->NumEntries() != InSourceGroup.SourceNumEntriesAtBake
		|| PCGExVariantCollection::ComputeSourceOrderHash(Src) != InSourceGroup.SourceOrderHashAtBake
		|| Src->GetCollectionGUID() != InSourceGroup.SourceGUIDAtBake;
}

const FPCGExVariantSource* UPCGExVariantCollection::FindSourceGroup(const FSoftObjectPath& InSourcePath) const
{
	for (const FPCGExVariantSource& Group : Sources)
	{
		if (Group.Source.ToSoftObjectPath() == InSourcePath)
		{
			return &Group;
		}
	}
	return nullptr;
}

void UPCGExVariantCollection::PreSave(FObjectPreSaveContext SaveContext)
{
	SyncVariantMappings();
	Super::PreSave(SaveContext);
}

const FPCGExAssetCollectionEntry* UPCGExVariantCollection::GetEntryAtRawIndex(const int32 Index) const
{
	return ResolveRawIndex(Index);
}

FPCGExAssetCollectionEntry* UPCGExVariantCollection::GetMutableEntryAtRawIndex(const int32 Index)
{
	return ResolveRawIndex(Index);
}

FPCGExAssetCollectionEntry* UPCGExVariantCollection::ResolveRawIndex(const int32 Index)
{
	return const_cast<FPCGExAssetCollectionEntry*>(const_cast<const UPCGExVariantCollection*>(this)->ResolveRawIndex(Index));
}

const FPCGExAssetCollectionEntry* UPCGExVariantCollection::ResolveRawIndex(const int32 Index) const
{
	if (Index < 0)
	{
		return nullptr;
	}

	int32 Offset = 0;
	for (const FPCGExVariantSource& Group : Sources)
	{
		const int32 Count = Group.Overrides.Num();
		if (Index < Offset + Count)
		{
			return Group.Overrides[Index - Offset].Entry.GetPtr<FPCGExAssetCollectionEntry>();
		}
		Offset += Count;
	}
	return nullptr;
}
