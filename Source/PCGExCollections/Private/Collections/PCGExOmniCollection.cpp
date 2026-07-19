// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Collections/PCGExOmniCollection.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetData.h"
#include "Collections/PCGExActorCollection.h"
#include "Collections/PCGExLevelCollection.h"
#include "Collections/PCGExMeshCollection.h"
#include "Collections/PCGExPCGDataAssetCollection.h"
#include "Collections/PCGExSkinnedMeshCollection.h"
#include "PCGDataAsset.h"
#include "Engine/Blueprint.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#endif

// Registered manually (not via PCGEX_REGISTER_COLLECTION_TYPE): Omni has no single entry
// struct, so EntryStruct stays null -- resolve structs per entry via Entry->GetTypeId().
namespace PCGExOmniCollection
{
	struct FTypeRegistration
	{
		FTypeRegistration()
		{
			PCGExAssetCollection::FTypeRegistry::AddPendingRegistration([]()
			{
				PCGExAssetCollection::FTypeInfo Info;
				Info.Id = PCGExAssetCollection::TypeIds::Omni;
				Info.CollectionClass = UPCGExOmniCollection::StaticClass();
				Info.EntryStruct = nullptr;
				Info.DisplayName = NSLOCTEXT("PCGEx", "OmniCollection", "Omni Collection");
				Info.ParentType = PCGExAssetCollection::TypeIds::Base;
				PCGExAssetCollection::FTypeRegistry::Get().Register(Info);
			});

#if WITH_EDITOR
			// Source-asset detection for the built-in types, consumed by Omni drag-drop
			// ingestion. Pending customizations run after ALL registrations, so ordering
			// against each type's own registration TU is irrelevant.
			using FTypeRegistry = PCGExAssetCollection::FTypeRegistry;
			using FTypeInfo = PCGExAssetCollection::FTypeInfo;
			namespace TypeIds = PCGExAssetCollection::TypeIds;

			FTypeRegistry::AddPendingCustomization(TypeIds::Mesh, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 10;
				Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.IsInstanceOf<UStaticMesh>(); };
			});

			FTypeRegistry::AddPendingCustomization(TypeIds::SkinnedMesh, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 10;
				Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.IsInstanceOf<USkinnedAsset>(); };
			});

			FTypeRegistry::AddPendingCustomization(TypeIds::Actor, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 20;
				// Loads the asset -- acceptable for a user-driven editor drop, and required to
				// inspect GeneratedClass (mirrors UPCGExActorCollection's browser ingestion).
				Info.DetectSourceAsset = [](const FAssetData& Asset)
				{
					if (Asset.AssetClassPath != UBlueprint::StaticClass()->GetClassPathName())
					{
						return false;
					}
					const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
					return Blueprint && Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass());
				};
				// The row must reference the GENERATED CLASS, not the blueprint asset --
				// the generic SetAssetPath(asset path) would bind the wrong object.
				Info.MakeEntryFromSourceAsset = [](const FAssetData& Asset, FInstancedStruct& OutPayload)
				{
					const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
					if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
					{
						return false;
					}
					OutPayload.InitializeAs(FPCGExActorCollectionEntry::StaticStruct());
					OutPayload.GetMutablePtr<FPCGExAssetCollectionEntry>()->SetAssetPath(FSoftObjectPath(Blueprint->GeneratedClass.Get()));
					return true;
				};
			});

			// Levels resolve before PCGDataAsset so a dropped UWorld becomes a Level entry,
			// not a level-sourced data asset export (that flow stays a deliberate authoring
			// choice on PCGDataAsset collections).
			FTypeRegistry::AddPendingCustomization(TypeIds::Level, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 30;
				Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName(); };
			});

			FTypeRegistry::AddPendingCustomization(TypeIds::PCGDataAsset, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 40;
				Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.IsInstanceOf<UPCGDataAsset>(); };
				// The entry struct default-constructs in Level source mode (level-exporter
				// flow); a browser-dropped UPCGDataAsset must be flipped to DataAsset mode
				// BEFORE SetAssetPath, which routes the path by Source.
				Info.MakeEntryFromSourceAsset = [](const FAssetData& Asset, FInstancedStruct& OutPayload)
				{
					OutPayload.InitializeAs(FPCGExPCGDataAssetCollectionEntry::StaticStruct());
					FPCGExPCGDataAssetCollectionEntry* Entry = OutPayload.GetMutablePtr<FPCGExPCGDataAssetCollectionEntry>();
					Entry->Source = EPCGExDataAssetEntrySource::DataAsset;
					Entry->SetAssetPath(Asset.ToSoftObjectPath());
					return true;
				};
			});
#endif
		}
	};

	FTypeRegistration GTypeRegistration;
}

#pragma region UPCGExOmniCollection

bool UPCGExOmniCollection::IsValidIndex(const int32 InIndex) const
{
	return Entries.IsValidIndex(InIndex);
}

int32 UPCGExOmniCollection::NumEntries() const
{
	return Entries.Num();
}

void UPCGExOmniCollection::InitNumEntries(const int32 InNum)
{
	PCGExArrayHelpers::InitArray(Entries, InNum);
}

void UPCGExOmniCollection::BuildCache()
{
	// Flatten to base pointers in row order. Unset payloads contribute a null -- tolerated
	// by BuildCacheFromEntryPtrs -- so raw indices stay stable under partial authoring.
	TArray<FPCGExAssetCollectionEntry*> EntryPtrs;
	EntryPtrs.Reserve(Entries.Num());

	for (FPCGExOmniCollectionEntry& Row : Entries)
	{
		EntryPtrs.Add(Row.GetPayload());
	}

	BuildCacheFromEntryPtrs(EntryPtrs);
}

void UPCGExOmniCollection::ForEachEntry(FForEachConstEntryFunc Iterator) const
{
	for (int32 i = 0; i < Entries.Num(); i++)
	{
		// Unset payloads are skipped but still consume their index -- iterator consumers
		// rely on the index being the raw entry index.
		if (const FPCGExAssetCollectionEntry* Payload = Entries[i].GetPayload())
		{
			Iterator(Payload, i);
		}
	}
}

void UPCGExOmniCollection::ForEachEntry(FForEachEntryFunc Iterator)
{
	for (int32 i = 0; i < Entries.Num(); i++)
	{
		if (FPCGExAssetCollectionEntry* Payload = Entries[i].GetPayload())
		{
			Iterator(Payload, i);
		}
	}
}

void UPCGExOmniCollection::Sort(FSortEntriesFunc Predicate)
{
	Entries.Sort([&Predicate](const FPCGExOmniCollectionEntry& A, const FPCGExOmniCollectionEntry& B)
	{
		const FPCGExAssetCollectionEntry* PayloadA = A.GetPayload();
		const FPCGExAssetCollectionEntry* PayloadB = B.GetPayload();

		// Unset payloads sort after everything, mutually equivalent.
		if (!PayloadA)
		{
			return false;
		}
		if (!PayloadB)
		{
			return true;
		}

		return Predicate(PayloadA, PayloadB);
	});
}

FPCGExAssetCollectionEntry* UPCGExOmniCollection::AddEntryOfType(const UScriptStruct* EntryStruct)
{
	if (!EntryStruct || !EntryStruct->IsChildOf(FPCGExAssetCollectionEntry::StaticStruct()))
	{
		return nullptr;
	}

	FPCGExOmniCollectionEntry& Row = Entries.Emplace_GetRef();
	Row.Entry.InitializeAs(EntryStruct);
	return Row.GetPayload();
}

bool UPCGExOmniCollection::GetTypeGlobalsInternal(const UScriptStruct* StructType, FPCGExCollectionTypeGlobals& OutGlobals) const
{
	if (StructType)
	{
		for (const FInstancedStruct& Block : TypeGlobals)
		{
			const UScriptStruct* BlockStruct = Block.GetScriptStruct();
			if (!BlockStruct || !BlockStruct->IsChildOf(StructType))
			{
				continue;
			}

			// Copy the requested portion out of the stored block. Base properties keep their
			// offsets in derived structs, so copying through StructType is valid even when
			// the block is a derived type. Shallow for subobject pointers -- intended: the
			// query is a transient read, not an ownership transfer.
			StructType->CopyScriptStruct(&OutGlobals, Block.GetMemory());
			return true;
		}
	}

	return Super::GetTypeGlobalsInternal(StructType, OutGlobals);
}

const FPCGExAssetCollectionEntry* UPCGExOmniCollection::GetEntryAtRawIndex(const int32 Index) const
{
	return Entries.IsValidIndex(Index) ? Entries[Index].GetPayload() : nullptr;
}

FPCGExAssetCollectionEntry* UPCGExOmniCollection::GetMutableEntryAtRawIndex(const int32 Index)
{
	return Entries.IsValidIndex(Index) ? Entries[Index].GetPayload() : nullptr;
}

#if WITH_EDITOR

const UScriptStruct* UPCGExOmniCollection::EDITOR_GetEntryScriptStruct(const int32 RawIndex) const
{
	return Entries.IsValidIndex(RawIndex) ? Entries[RawIndex].Entry.GetScriptStruct() : nullptr;
}

void UPCGExOmniCollection::EDITOR_GetAddableEntryTypes(TArray<const UScriptStruct*>& OutTypes) const
{
	// Every registered concrete entry type. Registered order isn't user-facing; sort by
	// display name so the add menu is stable.
	PCGExAssetCollection::FTypeRegistry::Get().ForEach([&OutTypes](const PCGExAssetCollection::FTypeInfo& Info)
	{
		if (Info.EntryStruct)
		{
			OutTypes.Add(Info.EntryStruct);
		}
	});

	OutTypes.Sort([](const UScriptStruct& A, const UScriptStruct& B)
	{
		return A.GetDisplayNameText().CompareTo(B.GetDisplayNameText()) < 0;
	});
}

FPCGExAssetCollectionEntry* UPCGExOmniCollection::EDITOR_AddEntry(const UScriptStruct* EntryStruct)
{
	// Untyped adds are meaningless on a heterogeneous host -- the caller must pick a type
	// (EDITOR_GetAddableEntryTypes drives the UI affordance).
	return EntryStruct ? AddEntryOfType(EntryStruct) : nullptr;
}

void UPCGExOmniCollection::EDITOR_AddSubCollectionEntries(const TArray<UPCGExAssetCollection*>& InSubCollections)
{
	// Base implementation reflects over a homogeneous Entries array of entry structs; Omni
	// rows wrap payloads, so append rows directly.
	TSet<const UPCGExAssetCollection*> AlreadyReferenced;
	ForEachEntry([&AlreadyReferenced](const FPCGExAssetCollectionEntry* Entry, int32)
	{
		if (Entry->HasValidSubCollection())
		{
			AlreadyReferenced.Add(Entry->GetSubCollectionPtr());
		}
	});

	for (UPCGExAssetCollection* Sub : InSubCollections)
	{
		if (!Sub || Sub == this || AlreadyReferenced.Contains(Sub) || HasCircularDependency(Sub))
		{
			continue;
		}

		// Subcollection rows adopt the referenced collection's own entry type when it has
		// one -- matching typed collections, whose subcollection entries are their native
		// struct. That way, unticking bIsSubCollection reveals the right asset picker
		// instead of stranding a base-typed row with nothing to reference. Heterogeneous
		// sources (Omni, Variant) have no single entry type and fall back to the base
		// struct, where the toggle has nothing to reveal by construction.
		const UScriptStruct* PayloadStruct = FPCGExAssetCollectionEntry::StaticStruct();
		if (const PCGExAssetCollection::FTypeInfo* SubTypeInfo = PCGExAssetCollection::FTypeRegistry::Get().FindByClass(Sub->GetClass()))
		{
			if (SubTypeInfo->EntryStruct)
			{
				PayloadStruct = SubTypeInfo->EntryStruct;
			}
		}

		FPCGExAssetCollectionEntry* Payload = AddEntryOfType(PayloadStruct);
		if (!Payload)
		{
			continue;
		}

		Payload->bIsSubCollection = true;
		Payload->SubCollection = Sub;
		AlreadyReferenced.Add(Sub);
	}
}

void UPCGExOmniCollection::EDITOR_AddBrowserSelectionInternal(const TArray<FAssetData>& InAssetData)
{
	UPCGExAssetCollection::EDITOR_AddBrowserSelectionInternal(InAssetData);

	// Gather registered detectors, lowest priority value first. Pointers into the registry
	// map are stable here: registration mutations only happen at module init / plugin load,
	// never during a user-driven editor drop.
	TArray<const PCGExAssetCollection::FTypeInfo*> Detectors;
	PCGExAssetCollection::FTypeRegistry::Get().ForEach([&Detectors](const PCGExAssetCollection::FTypeInfo& Info)
	{
		if (Info.DetectSourceAsset && Info.EntryStruct)
		{
			Detectors.Add(&Info);
		}
	});
	Detectors.Sort([](const PCGExAssetCollection::FTypeInfo& A, const PCGExAssetCollection::FTypeInfo& B)
	{
		return A.SourceDetectPriority < B.SourceDetectPriority;
	});

	// Existing (type, path) pairs so the same asset isn't added twice as the same entry type.
	auto MakeKey = [](const PCGExAssetCollection::FTypeId TypeId, const FSoftObjectPath& Path)
	{
		return HashCombineFast(GetTypeHash(TypeId), GetTypeHash(Path));
	};

	TSet<uint64> ExistingKeys;
	ForEachEntry([&ExistingKeys, &MakeKey](const FPCGExAssetCollectionEntry* Entry, int32)
	{
		if (!Entry->bIsSubCollection && Entry->Staging.Path.IsValid())
		{
			ExistingKeys.Add(MakeKey(Entry->GetTypeId(), Entry->Staging.Path));
		}
	});

	for (const FAssetData& Asset : InAssetData)
	{
		for (const PCGExAssetCollection::FTypeInfo* Info : Detectors)
		{
			if (!Info->DetectSourceAsset(Asset))
			{
				continue;
			}

			FInstancedStruct Payload;
			if (Info->MakeEntryFromSourceAsset)
			{
				if (!Info->MakeEntryFromSourceAsset(Asset, Payload))
				{
					break;
				}
			}
			else
			{
				Payload.InitializeAs(Info->EntryStruct);
				Payload.GetMutablePtr<FPCGExAssetCollectionEntry>()->SetAssetPath(Asset.ToSoftObjectPath());
			}

			const FPCGExAssetCollectionEntry* NewEntry = Payload.GetPtr<FPCGExAssetCollectionEntry>();
			if (!NewEntry)
			{
				break;
			}

			const uint64 Key = MakeKey(NewEntry->GetTypeId(), NewEntry->Staging.Path);
			if (ExistingKeys.Contains(Key))
			{
				break;
			}

			ExistingKeys.Add(Key);
			FPCGExOmniCollectionEntry& Row = Entries.Emplace_GetRef();
			Row.Entry = MoveTemp(Payload);
			break; // Asset claimed by this type.
		}
	}
}

#endif

#pragma endregion
