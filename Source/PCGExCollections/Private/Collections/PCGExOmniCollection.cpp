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

			// TypeId -> globals-block struct associations for the built-in types. Consumed by
			// conversion/merge (build the matching TypeGlobals block per source) and by
			// config-block UI. Registered here rather than per-collection-cpp so the macro
			// signature stays untouched for third parties (they use the same customization API).
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
					// IsInstanceOf (not an exact class-path compare) so UBlueprint SUBCLASS
					// assets wrapping actors are claimed too.
					if (!Asset.IsInstanceOf<UBlueprint>())
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

void UPCGExOmniCollection::GetTypeGlobalsStructs(TArray<const UScriptStruct*>& OutStructs) const
{
	Super::GetTypeGlobalsStructs(OutStructs);

	// Report each stored block's concrete struct -- unlike typed collections, what an Omni
	// can answer is defined by its authored blocks, not by its own collection type.
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

	// Conflict bookkeeping. One block per type slot at most: when a source provides a block
	// type that already exists with DIFFERENT values, a single block cannot serve both --
	// behavior wins over the block. If THIS call installed the existing block, it is dropped
	// and the installer's copied entries retro-bake; either way the slot joins Conflicted so
	// every later contributor bakes too. Blocks that pre-existed on this asset are the user's
	// and are never dropped -- incoming entries bake and a warning flags the precedence gap.
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

		// Globals blocks: typed sources provide their own type's block, heterogeneous
		// sources (Omni) every stored block -- both through the same seam.
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
				// Install. The copy-out is SHALLOW for Instanced subobjects -- duplicate them
				// into this asset (sharing EditInlineNew subobjects across assets is illegal).
				FInstancedStruct& Block = TypeGlobals.Add_GetRef(MoveTemp(Incoming));
				PCGExCollectionHelpers::DuplicateInstancedSubobjects(Provided, Block.GetMutableMemory(), this);

				FInstalledBlock& Installed = InstalledThisCall.AddDefaulted_GetRef();
				Installed.Struct = Provided;
				Installed.Source = Source;
				continue;
			}

			// Value-identical blocks are not conflicts: the installed block serves this source
			// as-is. Deep comparison so equal instanced subobjects compare by value, not pointer.
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
				// This call installed the block: drop it and retro-bake the installer's rows so
				// BOTH contributors keep their exact behavior -- a retained block whose rules
				// are collection-wide (e.g. Overrule descriptor mode) would silently restyle
				// entries that were baked against different globals.
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
				// The block pre-existed on this asset -- it is the user's and stays. Baked
				// incoming entries keep their values, but a block enforcing collection-wide
				// rules still takes precedence over them.
				UE_LOG(LogPCGEx, Warning,
				       TEXT("Merging '%s': this Omni collection already has a '%s' globals block with different settings. The source's globals were baked into its copied entries, but the existing block's own rules (e.g. a collection-wide overrule) still take precedence over them."),
				       *Source->GetName(), *Existing.GetScriptStruct()->GetName());
			}
		}

		// Entries: exact-typed payload copies. Per-row struct resolution handles typed and
		// heterogeneous sources alike.
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

			// The raw payload copy is SHALLOW for Instanced subobjects (e.g. a PCGDataAsset
			// entry's embedded level export, outer'd to the SOURCE asset) -- duplicate them
			// into this asset, or saving would reference another package's private objects.
			PCGExCollectionHelpers::DuplicateInstancedSubobjects(PayloadStruct, Payload, this);

			// New identity in this collection: fresh EntryId on the next SyncEntryIds pass.
			Payload->EntryId = 0;

			// Bake the source's collection tags into the copied entry (they no longer have
			// the source as host); matches FlattenCollection semantics.
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
			// Storage without per-row payload structs (e.g. Variant collections, which theme
			// other collections rather than owning entries) cannot be merged -- say so instead
			// of silently producing nothing.
			UE_LOG(LogPCGEx, Warning,
			       TEXT("Merging '%s': no entries could be appended -- its storage does not expose per-entry payloads (Variant collections cannot be merged; merge their source collections instead)."),
			       *Source->GetName());
		}

		AppendedCount += SourceAppended;
	}

	if (AppendedCount > 0)
	{
		// Re-derive the collection-level property schema from the merged entries' overrides,
		// then rebuild staging (also re-mints EntryIds via SyncEntryIds).
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

	// Actor-typed entries get the same property-component schema scan a native actor
	// collection runs in its own post-rebuild hook.
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
	// Every registered concrete entry type. Registered order isn't user-facing; sort by
	// display name so the add menu is stable.
	PCGExAssetCollection::FTypeRegistry::Get().ForEach([&OutTypes](const PCGExAssetCollection::FTypeInfo& Info)
	{
		// Skip the base entry struct (Variant registers it as its nominal EntryStruct): a
		// base-typed row has no asset to reference, so offering it in the add menu only
		// strands users on a pickerless tile -- subcollection rows are created by dropping
		// collection assets instead.
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
		if (Entry->bIsSubCollection)
		{
			return;
		}

		// Staging.Path covers staged rows; source-asset paths cover rows added but not yet
		// staged -- both spaces match what SetAssetPath seeds on a freshly dropped row.
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
				// A detector may claim an asset its factory then rejects on closer inspection;
				// fall through to lower-priority detectors instead of dropping the asset.
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
