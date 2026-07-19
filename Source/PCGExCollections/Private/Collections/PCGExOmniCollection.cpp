// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Collections/PCGExOmniCollection.h"

#include "PCGExLog.h"
#include "Collections/PCGExActorCollection.h"
#include "Collections/PCGExLevelCollection.h"
#include "Collections/PCGExMeshCollection.h"
#include "Collections/PCGExPCGDataAssetCollection.h"
#include "Collections/PCGExSkinnedMeshCollection.h"
#include "Core/PCGExCollectionHelpers.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetData.h"
#include "PCGDataAsset.h"
#include "UObject/UnrealType.h"
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

			// TypeId -> globals-block struct for the built-in types; registered here so the
			// per-collection macro signature stays untouched.
			{
				using FTypeRegistry = PCGExAssetCollection::FTypeRegistry;
				using FTypeInfo = PCGExAssetCollection::FTypeInfo;
				namespace TypeIds = PCGExAssetCollection::TypeIds;

				FTypeRegistry::AddPendingCustomization(TypeIds::Mesh, [](FTypeInfo& Info) { Info.GlobalsStruct = FPCGExMeshCollectionGlobals::StaticStruct(); });
				FTypeRegistry::AddPendingCustomization(TypeIds::SkinnedMesh, [](FTypeInfo& Info) { Info.GlobalsStruct = FPCGExSkinnedMeshCollectionGlobals::StaticStruct(); });
				FTypeRegistry::AddPendingCustomization(TypeIds::Actor, [](FTypeInfo& Info) { Info.GlobalsStruct = FPCGExActorCollectionGlobals::StaticStruct(); });
				FTypeRegistry::AddPendingCustomization(TypeIds::Level, [](FTypeInfo& Info) { Info.GlobalsStruct = FPCGExLevelCollectionGlobals::StaticStruct(); });
				FTypeRegistry::AddPendingCustomization(TypeIds::PCGDataAsset, [](FTypeInfo& Info) { Info.GlobalsStruct = FPCGExPCGDataAssetCollectionGlobals::StaticStruct(); });
			}

#if WITH_EDITOR
			// Source-asset detection for the built-in types (Omni drag-drop ingestion).
			// Pending customizations run after ALL registrations -- order-safe.
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
				// Loads the asset to inspect GeneratedClass -- acceptable for a user-driven drop.
				Info.DetectSourceAsset = [](const FAssetData& Asset)
				{
					// IsInstanceOf so UBlueprint subclass assets are claimed too.
					if (!Asset.IsInstanceOf<UBlueprint>())
					{
						return false;
					}
					const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
					return Blueprint && Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass());
				};
				// The row must reference the GENERATED CLASS, not the blueprint asset.
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

			// Levels resolve before PCGDataAsset: a dropped UWorld becomes a Level entry.
			FTypeRegistry::AddPendingCustomization(TypeIds::Level, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 30;
				Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName(); };
			});

			FTypeRegistry::AddPendingCustomization(TypeIds::PCGDataAsset, [](FTypeInfo& Info)
			{
				Info.SourceDetectPriority = 40;
				Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.IsInstanceOf<UPCGDataAsset>(); };
				// Flip to DataAsset mode BEFORE SetAssetPath -- it routes the path by Source.
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
	// Unset payloads contribute a null (tolerated) so raw indices stay stable.
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
		// Unset payloads skip but still consume their raw index.
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

			// Copy the StructType portion out (valid for derived blocks: base offsets are
			// shared). Shallow for subobjects -- transient read, not ownership transfer.
			StructType->CopyScriptStruct(&OutGlobals, Block.GetMemory());
			return true;
		}
	}

	return Super::GetTypeGlobalsInternal(StructType, OutGlobals);
}

void UPCGExOmniCollection::GetTypeGlobalsStructs(TArray<const UScriptStruct*>& OutStructs) const
{
	Super::GetTypeGlobalsStructs(OutStructs);

	// What an Omni answers is defined by its authored blocks, not its collection type.
	for (const FInstancedStruct& Block : TypeGlobals)
	{
		if (const UScriptStruct* BlockStruct = Block.GetScriptStruct())
		{
			OutStructs.AddUnique(BlockStruct);
		}
	}
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

int32 UPCGExOmniCollection::EDITOR_AppendCollections(TConstArrayView<UPCGExAssetCollection*> InSources)
{
	int32 AppendedCount = 0;

	Modify(true);

	// One block per type slot: on a value conflict behavior wins over the block -- a block
	// THIS call installed is dropped (installer rows retro-bake) and the slot stays
	// block-less; a pre-existing block is the user's and stays (incoming bakes + warning).
	struct FInstalledBlock
	{
		const UScriptStruct* Struct = nullptr;
		UPCGExAssetCollection* Source = nullptr;
		int32 RowStart = INDEX_NONE;
		int32 RowEnd = INDEX_NONE;
	};
	TArray<FInstalledBlock> InstalledThisCall;
	TArray<const UScriptStruct*> Conflicted;

	// Base and derived globals blocks answer the same queries, so they share one type slot.
	auto MatchesSlot = [](const UScriptStruct* A, const UScriptStruct* B)
	{
		return A && B && (A->IsChildOf(B) || B->IsChildOf(A));
	};

	// Would an entry of this type read the block struct S through the seam?
	auto CoversEntry = [&MatchesSlot](const UScriptStruct* S, const FPCGExAssetCollectionEntry* Entry)
	{
		const PCGExAssetCollection::FTypeInfo* TypeInfo = PCGExAssetCollection::FTypeRegistry::Get().Find(Entry->GetTypeId());
		return TypeInfo && MatchesSlot(TypeInfo->GlobalsStruct, S);
	};

	for (UPCGExAssetCollection* Source : InSources)
	{
		if (!Source || Source == this)
		{
			continue;
		}

		// Typed sources provide their own block; heterogeneous sources every stored block.
		TArray<const UScriptStruct*> ProvidedStructs;
		Source->GetTypeGlobalsStructs(ProvidedStructs);

		// Slots this source must bake into its entries instead of relying on a block.
		TArray<const UScriptStruct*> BakeStructs;

		for (const UScriptStruct* Provided : ProvidedStructs)
		{
			if (!Provided)
			{
				continue;
			}

			FInstancedStruct Incoming;
			Incoming.InitializeAs(Provided);
			if (!Source->GetTypeGlobals(Provided, *Incoming.GetMutablePtr<FPCGExCollectionTypeGlobals>()))
			{
				continue;
			}

			// A slot that already conflicted stays block-less: bake.
			if (Conflicted.ContainsByPredicate([&](const UScriptStruct* C) { return MatchesSlot(C, Provided); }))
			{
				BakeStructs.Add(Provided);
				continue;
			}

			const int32 ExistingIdx = TypeGlobals.IndexOfByPredicate([&](const FInstancedStruct& Block)
			{
				return MatchesSlot(Block.GetScriptStruct(), Provided);
			});

			if (ExistingIdx == INDEX_NONE)
			{
				// Install; copy-out is SHALLOW for Instanced subobjects -- duplicate into this asset.
				FInstancedStruct& Block = TypeGlobals.Add_GetRef(MoveTemp(Incoming));
				PCGExCollectionHelpers::DuplicateInstancedSubobjects(Provided, Block.GetMutableMemory(), this);

				FInstalledBlock& Installed = InstalledThisCall.AddDefaulted_GetRef();
				Installed.Struct = Provided;
				Installed.Source = Source;
				continue;
			}

			// Value-identical (deep-compared) blocks are not conflicts.
			const FInstancedStruct& Existing = TypeGlobals[ExistingIdx];
			if (Existing.GetScriptStruct() == Provided &&
				Provided->CompareScriptStruct(Existing.GetMemory(), Incoming.GetMemory(), PPF_DeepComparison))
			{
				continue;
			}

			Conflicted.AddUnique(Provided);
			BakeStructs.Add(Provided);

			const int32 InstalledIdx = InstalledThisCall.IndexOfByPredicate([&](const FInstalledBlock& I) { return MatchesSlot(I.Struct, Provided); });
			if (InstalledIdx != INDEX_NONE)
			{
				// This call installed the block: drop it and retro-bake the installer's rows
				// so BOTH contributors keep their exact behavior.
				const FInstalledBlock Installed = InstalledThisCall[InstalledIdx];
				InstalledThisCall.RemoveAt(InstalledIdx);
				TypeGlobals.RemoveAt(ExistingIdx);
				Conflicted.AddUnique(Installed.Struct);

				for (int32 Row = FMath::Max(Installed.RowStart, 0); Row < Installed.RowEnd && Entries.IsValidIndex(Row); Row++)
				{
					FPCGExAssetCollectionEntry* Payload = Entries[Row].GetPayload();
					if (Payload && !Payload->bIsSubCollection && CoversEntry(Installed.Struct, Payload))
					{
						Payload->ResolveGlobalsToLocal(Installed.Source);
					}
				}
			}
			else
			{
				// Pre-existing block is the user's -- keep it, bake incoming, warn.
				UE_LOG(LogPCGEx, Warning,
				       TEXT("Merging '%s': this Omni collection already has a '%s' globals block with different settings. The source's globals were baked into its copied entries, but the existing block's own rules (e.g. a collection-wide overrule) still take precedence over them."),
				       *Source->GetName(), *Existing.GetScriptStruct()->GetName());
			}
		}

		// Entries: exact-typed payload copies.
		const int32 RowStart = Entries.Num();
		int32 SourceAppended = 0;
		int32 SourceSkipped = 0;

		Source->ForEachEntry([&](const FPCGExAssetCollectionEntry* Entry, const int32 Index)
		{
			const UScriptStruct* PayloadStruct = Source->EDITOR_GetEntryScriptStruct(Index);
			if (!PayloadStruct)
			{
				SourceSkipped++;
				return;
			}

			FPCGExOmniCollectionEntry& Row = Entries.Emplace_GetRef();
			Row.Entry.InitializeAs(PayloadStruct, reinterpret_cast<const uint8*>(Entry));

			FPCGExAssetCollectionEntry* Payload = Row.GetPayload();

			// Payload copy is SHALLOW for Instanced subobjects -- duplicate into this asset.
			PCGExCollectionHelpers::DuplicateInstancedSubobjects(PayloadStruct, Payload, this);

			// New identity: fresh EntryId on the next SyncEntryIds pass.
			Payload->EntryId = 0;

			// Bake source CollectionTags into the copy (FlattenCollection semantics).
			Payload->Tags.Append(Source->CollectionTags);

			if (!Payload->bIsSubCollection)
			{
				for (const UScriptStruct* Bake : BakeStructs)
				{
					if (CoversEntry(Bake, Payload))
					{
						Payload->ResolveGlobalsToLocal(Source);
						break;
					}
				}
			}

			SourceAppended++;
		});

		// Row ranges for blocks this source installed -- retro-bake targets on later conflicts.
		for (FInstalledBlock& Installed : InstalledThisCall)
		{
			if (Installed.Source == Source)
			{
				Installed.RowStart = RowStart;
				Installed.RowEnd = Entries.Num();
			}
		}

		if (SourceAppended == 0 && SourceSkipped > 0)
		{
			// Storage without per-row payload structs (Variant) cannot be merged -- say so.
			UE_LOG(LogPCGEx, Warning,
			       TEXT("Merging '%s': no entries could be appended -- its storage does not expose per-entry payloads (Variant collections cannot be merged; merge their source collections instead)."),
			       *Source->GetName());
		}

		AppendedCount += SourceAppended;
	}

	if (AppendedCount > 0)
	{
		// Re-derive the property schema, rebuild staging (re-mints EntryIds).
		RefreshCollectionPropertiesFromEntries();
		EDITOR_RebuildStagingData();
	}

	return AppendedCount;
}

const UScriptStruct* UPCGExOmniCollection::EDITOR_GetEntryScriptStruct(const int32 RawIndex) const
{
	return Entries.IsValidIndex(RawIndex) ? Entries[RawIndex].Entry.GetScriptStruct() : nullptr;
}

void UPCGExOmniCollection::EDITOR_OnPostStagingRebuild()
{
	Super::EDITOR_OnPostStagingRebuild();

	// Actor-typed entries get the same schema scan a native actor collection runs.
	bool bAnyActorEntry = false;
	ForEachEntry([&bAnyActorEntry](const FPCGExAssetCollectionEntry* Entry, int32)
	{
		bAnyActorEntry |= !Entry->bIsSubCollection && Entry->IsType(PCGExAssetCollection::TypeIds::Actor);
	});

	if (!bAnyActorEntry)
	{
		return;
	}

	// Merge policy from the actor globals block when one is present; struct default otherwise.
	FPCGExActorCollectionGlobals ActorGlobals;
	GetTypeGlobals(ActorGlobals);

	UPCGExActorCollection::RebuildActorPropertiesFromComponents(this, ActorGlobals.SchemaMergePolicy);
}

void UPCGExOmniCollection::EDITOR_GetAddableEntryTypes(TArray<const UScriptStruct*>& OutTypes) const
{
	// Every registered concrete entry type, sorted for a stable menu.
	PCGExAssetCollection::FTypeRegistry::Get().ForEach([&OutTypes](const PCGExAssetCollection::FTypeInfo& Info)
	{
		// Skip the base entry struct (Variant's nominal EntryStruct): a base row has no
		// asset picker; subcollection rows come from dropping collection assets instead.
		if (Info.EntryStruct && Info.EntryStruct != FPCGExAssetCollectionEntry::StaticStruct())
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
	// Untyped adds are meaningless on a heterogeneous host -- the caller must pick a type.
	return EntryStruct ? AddEntryOfType(EntryStruct) : nullptr;
}

void UPCGExOmniCollection::EDITOR_AddSubCollectionEntries(const TArray<UPCGExAssetCollection*>& InSubCollections)
{
	// Base reflects over a homogeneous Entries array; wrapper rows append directly.
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

		// Subcollection rows adopt the referenced collection's entry type when it has one
		// (unticking bIsSubCollection then reveals the right picker); heterogeneous sources
		// have no single type and fall back to the base struct.
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

	// Detectors by ascending priority. Registry pointers are stable here (mutations only
	// happen at module init / plugin load).
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
		if (Entry->bIsSubCollection)
		{
			return;
		}

		// Staging.Path covers staged rows; source paths cover not-yet-staged rows.
		if (Entry->Staging.Path.IsValid())
		{
			ExistingKeys.Add(MakeKey(Entry->GetTypeId(), Entry->Staging.Path));
		}

		TSet<FSoftObjectPath> SourcePaths;
		Entry->EDITOR_GetSourceAssetPaths(SourcePaths);
		for (const FSoftObjectPath& Path : SourcePaths)
		{
			ExistingKeys.Add(MakeKey(Entry->GetTypeId(), Path));
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
				// Factory rejected on closer inspection: fall through to lower-priority detectors.
				if (!Info->MakeEntryFromSourceAsset(Asset, Payload))
				{
					continue;
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
				continue;
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
