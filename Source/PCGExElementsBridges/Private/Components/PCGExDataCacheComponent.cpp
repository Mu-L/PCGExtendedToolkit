// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Components/PCGExDataCacheComponent.h"

#include "GameFramework/Actor.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/Package.h"

#include "PCGExLog.h"

#if WITH_EDITOR
#include "PCGComponent.h"
#include "PCGWorldActor.h"
#include "Helpers/PCGExObjectNotifyHelpers.h"
#endif

namespace PCGExDataCacheComponent
{
	// Moves the entry's data out of InMap under a write lock the caller already holds.
	bool TakeEntry(TMap<FName, FPCGExDataCacheEntry>& InMap, const FName InId, FPCGDataCollection& OutData)
	{
		FPCGExDataCacheEntry Entry;
		if (!InMap.RemoveAndCopyValue(InId, Entry)) { return false; }
		OutData = MoveTemp(Entry.Data);
		return true;
	}

	// Moves every entry's data out of InMap under a write lock the caller already holds.
	void TakeAll(TMap<FName, FPCGExDataCacheEntry>& InMap, TArray<FPCGDataCollection>& OutData)
	{
		for (TPair<FName, FPCGExDataCacheEntry>& Pair : InMap) { OutData.Add(MoveTemp(Pair.Value.Data)); }
		InMap.Reset();
	}
}

#pragma region UPCGExDataCacheComponent

UPCGExDataCacheComponent::UPCGExDataCacheComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

UPCGExDataCacheComponent* UPCGExDataCacheComponent::Find(const AActor* InActor)
{
	if (!IsValid(InActor)) { return nullptr; }
	if (UPCGExDataCacheComponent* Component = InActor->FindComponentByClass<UPCGExDataCacheComponent>()) { return Component; }

	// Level copy-paste can leave an instance component in InstanceComponents without registering it.
	for (UActorComponent* InstanceComponent : InActor->GetInstanceComponents())
	{
		if (UPCGExDataCacheComponent* Component = Cast<UPCGExDataCacheComponent>(InstanceComponent)) { return Component; }
	}

	return nullptr;
}

UPCGExDataCacheComponent* UPCGExDataCacheComponent::FindOrCreate(AActor* InActor, const bool bTransient)
{
	check(IsInGameThread());

	if (!IsValid(InActor)) { return nullptr; }

	if (UPCGExDataCacheComponent* Existing = Find(InActor))
	{
		// Born in preview, promoted by the first persistent write. Never demoted.
		if (!bTransient && Existing->HasAnyFlags(RF_Transient))
		{
			Existing->ClearFlags(RF_Transient);
			Existing->SetFlags(RF_Transactional);
			Existing->MarkPackageDirty();
		}
		return Existing;
	}

	// Instance components survive construction-script reruns and are not touched by PCG cleanup; same path as
	// the engine's Add Component node, including RF_Transient for preview-mode writers.
	UPCGExDataCacheComponent* Component = NewObject<UPCGExDataCacheComponent>(InActor, NAME_None, bTransient ? RF_Transient : RF_Transactional);
	Component->RegisterComponent();
	InActor->AddInstanceComponent(Component);
	return Component;
}

bool UPCGExDataCacheComponent::Read(const FName InId, TArray<FPCGTaggedData>& OutData) const
{
	UE::TReadScopeLock ScopedReadLock(Lock);

	const FPCGExDataCacheEntry* Entry = PreviewEntries.Find(InId);
	if (!Entry) { Entry = Entries.Find(InId); }
	if (!Entry) { return false; }

	OutData = Entry->Data.TaggedData;
	return true;
}

void UPCGExDataCacheComponent::ReadAll(TArray<TPair<FName, TArray<FPCGTaggedData>>>& OutEntries) const
{
	UE::TReadScopeLock ScopedReadLock(Lock);

	OutEntries.Reserve(OutEntries.Num() + Entries.Num() + PreviewEntries.Num());
	for (const TPair<FName, FPCGExDataCacheEntry>& Pair : PreviewEntries) { OutEntries.Emplace(Pair.Key, Pair.Value.Data.TaggedData); }
	for (const TPair<FName, FPCGExDataCacheEntry>& Pair : Entries)
	{
		if (!PreviewEntries.Contains(Pair.Key)) { OutEntries.Emplace(Pair.Key, Pair.Value.Data.TaggedData); }
	}
}

void UPCGExDataCacheComponent::Write(const FName InId, const EPCGExDataCacheWriteMode InMode, TArray<FPCGTaggedData>&& InData, const UObject* InWriter, const bool bPreview, const bool bNotify)
{
	check(IsInGameThread());

	bool bReplace = false;
	switch (InMode)
	{
	case EPCGExDataCacheWriteMode::Replace:
		bReplace = true;
		break;
	case EPCGExDataCacheWriteMode::Append:
		break;
	case EPCGExDataCacheWriteMode::Clear:
		Clear(InId, InWriter, bPreview, bNotify);
		return;
	case EPCGExDataCacheWriteMode::ClearAll:
		ClearAll(InWriter, bPreview, bNotify);
		return;
	default:
		UE_LOG(LogPCGEx, Error, TEXT("[Data Cache] Unresolvable write mode (%d); nothing written."), static_cast<int32>(InMode));
		return;
	}

	// Rename/Flatten happen outside the lock; the lock only brackets the map swap.
	TArray<FPCGTaggedData> Adopted = AdoptData(MoveTemp(InData), bPreview);

	FPCGDataCollection Released;
	FPCGDataCollection ReleasedShadow;
	{
		UE::TWriteScopeLock ScopedWriteLock(Lock);

		FPCGExDataCacheEntry& Entry = (bPreview ? PreviewEntries : Entries).FindOrAdd(InId);
		if (bReplace) { Released = MoveTemp(Entry.Data); }

		Entry.Data.TaggedData.Append(MoveTemp(Adopted));
		Entry.Writer = FSoftObjectPath(InWriter);

		// A persistent write supersedes the preview shadow of the same ID; otherwise reads would keep returning it.
		if (!bPreview) { PCGExDataCacheComponent::TakeEntry(PreviewEntries, InId, ReleasedShadow); }
	}

	ReleaseData(Released);
	ReleaseData(ReleasedShadow);

	if (!bPreview) { MarkPackageDirty(); }
	if (bNotify) { NotifyChanged(InWriter); }
}

void UPCGExDataCacheComponent::Clear(const FName InId, const UObject* InWriter, const bool bPreview, const bool bNotify)
{
	check(IsInGameThread());

	FPCGDataCollection ReleasedPersisted;
	FPCGDataCollection ReleasedPreview;
	bool bRemovedPersisted = false;
	bool bRemovedPreview = false;
	{
		UE::TWriteScopeLock ScopedWriteLock(Lock);
		// A preview generation never touches persisted data.
		if (!bPreview) { bRemovedPersisted = PCGExDataCacheComponent::TakeEntry(Entries, InId, ReleasedPersisted); }
		bRemovedPreview = PCGExDataCacheComponent::TakeEntry(PreviewEntries, InId, ReleasedPreview);
	}

	ReleaseData(ReleasedPersisted);
	ReleaseData(ReleasedPreview);

	if (bRemovedPersisted) { MarkPackageDirty(); }
	if (bNotify && (bRemovedPersisted || bRemovedPreview)) { NotifyChanged(InWriter); }
}

void UPCGExDataCacheComponent::ClearAll(const UObject* InWriter, const bool bPreview, const bool bNotify)
{
	check(IsInGameThread());

	TArray<FPCGDataCollection> Released;
	bool bRemovedPersisted = false;
	{
		UE::TWriteScopeLock ScopedWriteLock(Lock);
		Released.Reserve(Entries.Num() + PreviewEntries.Num());
		// A preview generation never touches persisted data.
		if (!bPreview)
		{
			bRemovedPersisted = !Entries.IsEmpty();
			PCGExDataCacheComponent::TakeAll(Entries, Released);
		}
		PCGExDataCacheComponent::TakeAll(PreviewEntries, Released);
	}

	for (const FPCGDataCollection& Collection : Released) { ReleaseData(Collection); }

	if (bRemovedPersisted) { MarkPackageDirty(); }
	if (bNotify && !Released.IsEmpty()) { NotifyChanged(InWriter); }
}

void UPCGExDataCacheComponent::NotifyChanged(const UObject* InWriter) const
{
#if WITH_EDITOR
	check(IsInGameThread());

	AActor* Owner = GetOwner();
	if (!Owner) { return; }

	// FPCGActorTracker::ShouldIgnoreActor drops the PCG World Actor (and editor preview actors) before dispatch.
	if (Owner->IsA<APCGWorldActor>())
	{
		UE_LOG(LogPCGEx, Verbose, TEXT("[Data Cache] Change notifications on the PCG World Actor are ignored by PCG tracking."));
		return;
	}

	// FPCGActorTracker defers actors still registering components to its Tick; that dispatch would land after the
	// bracket below closed and re-trigger the writer. Skip rather than loop. (An unloaded level-instance hierarchy
	// defers the same way and is not detected here.)
	if (!Owner->HasActorRegisteredAllComponents())
	{
		UE_LOG(LogPCGEx, Verbose, TEXT("[Data Cache] '%s' is still registering components; change notification skipped."), *Owner->GetName());
		return;
	}

	// The actor tracker maps this component to its owner, then FPCGComponentChangeHandler::ShouldDiscardComponent
	// consults the tracked component's ORIGINAL component ignore list against {owner actor, this}. Dispatch is
	// synchronous from here, so the bracket only needs to span the broadcast and stays balanced.
	UPCGComponent* WriterOriginal = nullptr;
	if (const UPCGComponent* WriterComponent = Cast<UPCGComponent>(InWriter))
	{
		WriterOriginal = WriterComponent->GetOriginalComponent();
	}

	if (WriterOriginal) { WriterOriginal->StartIgnoringChangeOriginDuringGeneration(Owner); }
	PCGExEditor::NotifyObjectChanged(const_cast<UPCGExDataCacheComponent*>(this));
	if (WriterOriginal) { WriterOriginal->StopIgnoringChangeOriginDuringGeneration(Owner); }
#else
	(void)InWriter;
#endif
}

#if WITH_EDITOR
void UPCGExDataCacheComponent::EDITOR_ClearCache()
{
	// Not transacted: the data release renames non-transactionally, so an undo would restore dangling references.
	ClearAll(/*InWriter=*/nullptr, /*bPreview=*/false, /*bNotify=*/true);
}
#endif

void UPCGExDataCacheComponent::ReleaseData(const FPCGDataCollection& InData) const
{
	// Mirrors UPCGComponent::ClearGraphGeneratedOutput: only objects we own go back to the transient package. Any
	// context still holding the data keeps it alive; GC reclaims it once the last reference drops.
	for (const FPCGTaggedData& TaggedData : InData.TaggedData)
	{
		if (TaggedData.Data && TaggedData.Data->GetOuter() == this)
		{
			const_cast<UPCGData*>(TaggedData.Data.Get())->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
		}
	}
}

TArray<FPCGTaggedData> UPCGExDataCacheComponent::AdoptData(TArray<FPCGTaggedData>&& InData, const bool bPreview) const
{
	TArray<FPCGTaggedData> Adopted;
	Adopted.Reserve(InData.Num());

	const ERenameFlags RenameFlags = bPreview ? REN_DoNotDirty : REN_None;
	UPCGExDataCacheComponent* NewOuter = const_cast<UPCGExDataCacheComponent*>(this);

	for (FPCGTaggedData& TaggedData : InData)
	{
		if (!TaggedData.Data) { continue; }
		UPCGData* Data = const_cast<UPCGData*>(TaggedData.Data.Get());

		// Proxies (render targets, ...) hold non-serializable resources; the engine refuses them on component output too.
		if (!Data->CanBeSerialized())
		{
			UE_LOG(LogPCGEx, Warning, TEXT("[Data Cache] '%s' cannot be serialized and was not cached."), *Data->GetName());
			continue;
		}

		// A fresh duplicate may still be a lazy copy of its source; Flatten severs that link before the network check.
		Data->Flatten();

		// Only self-contained data is adopted. UPCGUnionData::VisitDataNetwork visits the sources and not the union,
		// so any visited object other than Data itself means shared upstream objects we must not steal or serialize.
		bool bSelfContained = true;
		Data->VisitDataNetwork([Data, &bSelfContained](const UPCGData* InNetworkData)
		{
			if (InNetworkData != Data) { bSelfContained = false; }
		});

		if (!bSelfContained)
		{
			UE_LOG(LogPCGEx, Warning, TEXT("[Data Cache] '%s' is composite data (union, intersection, projection...) and was not cached; convert it to points first."), *Data->GetName());
			continue;
		}

		if (bPreview) { Data->SetFlags(RF_Transient); }
		else { Data->ClearFlags(RF_Transient); }
		Data->Rename(nullptr, NewOuter, RenameFlags);

		Adopted.Add(MoveTemp(TaggedData));
	}

	return Adopted;
}

#pragma endregion
