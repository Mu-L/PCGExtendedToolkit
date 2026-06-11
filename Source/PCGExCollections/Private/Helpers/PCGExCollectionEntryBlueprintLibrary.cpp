// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExCollectionEntryBlueprintLibrary.h"

#include "PCGExProperty.h"
#include "PCGExPropertyPinMarshal.h"
#include "Core/PCGExAssetCollection.h"
#include "Helpers/PCGExCollectionPropertySetWriter.h"
#include "UObject/Stack.h"

namespace PCGExCollectionEntryBlueprintLibrary_Private
{
	const FPCGExAssetCollectionEntry* GetEntry(const UPCGExAssetCollection* Collection, int32 EntryIndex)
	{
		return Collection ? Collection->GetEntryRaw(EntryIndex).Entry : nullptr;
	}

	FPCGExAssetCollectionEntry* GetMutableEntry(UPCGExAssetCollection* Collection, int32 EntryIndex)
	{
		return Collection ? Collection->GetMutableEntryRaw(EntryIndex) : nullptr;
	}

	bool ReadInto(
		const UPCGExAssetCollection* Collection,
		int32 EntryIndex,
		FName PropertyName,
		const FProperty* OutProp,
		void* OutMem)
	{
		if (!Collection || !OutProp || !OutMem)
		{
			return false;
		}

		const FPCGExEntryAccessResult Result = Collection->GetEntryRaw(EntryIndex);
		if (!Result)
		{
			return false;
		}

		const FInstancedStruct* Source = PCGExCollections::ResolveEntrySourceProperty(Result.Entry, Result.Host, PropertyName);
		if (!Source)
		{
			return false;
		}

		const FPCGExProperty* Prop = Source->GetPtr<FPCGExProperty>();
		if (!Prop)
		{
			return false;
		}

		return PCGExPropertyPinMarshal::TryWriteToPin(Prop, OutProp, OutMem);
	}

	// Resolve the writable override slot for (entry, property). Slots are kept parallel to the
	// collection schema by SyncPropertyOverridesToEntries -- never append here. A missing slot
	// means the property isn't part of the schema, which is an authoring error worth surfacing:
	// fail with a Blueprint runtime warning rather than silently returning false.
	FPCGExPropertyOverrideEntry* ResolveWritableOverride(UPCGExAssetCollection* Collection, int32 EntryIndex, FName PropertyName)
	{
		FPCGExAssetCollectionEntry* Entry = GetMutableEntry(Collection, EntryIndex);
		if (!Entry)
		{
			return nullptr;
		}

		FPCGExPropertyOverrideEntry* Slot = Entry->PropertyOverrides.FindEntryMutableByName(PropertyName);
		if (!Slot)
		{
			FFrame::KismetExecutionMessage(
				*FString::Printf(
					TEXT("Property '%s' is not part of the schema of collection '%s' -- entry override not written."),
					*PropertyName.ToString(), *GetNameSafe(Collection)),
				ELogVerbosity::Warning);
			return nullptr;
		}

		return Slot;
	}

	// Enable the freshly-written override and dirty the owning collection. Override writes
	// don't feed the weight-sorted pick cache, so no InvalidateCache here.
	void CommitOverrideWrite(UPCGExAssetCollection* Collection, FPCGExPropertyOverrideEntry* Slot)
	{
		Slot->bEnabled = true;
		Collection->Modify();
		(void)Collection->MarkPackageDirty();
	}

	bool WriteFrom(
		UPCGExAssetCollection* Collection,
		int32 EntryIndex,
		FName PropertyName,
		const FProperty* InProp,
		const void* InMem)
	{
		if (!InProp || !InMem)
		{
			return false;
		}

		FPCGExPropertyOverrideEntry* Slot = ResolveWritableOverride(Collection, EntryIndex, PropertyName);
		if (!Slot)
		{
			return false;
		}

		FPCGExProperty* Prop = Slot->GetPropertyMutable();
		if (!Prop)
		{
			return false;
		}

		if (!PCGExPropertyPinMarshal::TryReadFromPin(Prop, InProp, InMem))
		{
			return false;
		}

		CommitOverrideWrite(Collection, Slot);
		return true;
	}

	// Soft-path lookups for the well-typed Object/Class accessors; same rationale as the
	// component library: keep Object/Class pins out of the wildcard CustomStructureParam path.
	bool ReadEntrySoftPath(
		const UPCGExAssetCollection* Collection,
		int32 EntryIndex,
		FName PropertyName,
		EPCGMetadataTypes PathType,
		void* OutPath)
	{
		if (!Collection)
		{
			return false;
		}

		const FPCGExEntryAccessResult Result = Collection->GetEntryRaw(EntryIndex);
		if (!Result)
		{
			return false;
		}

		const FInstancedStruct* Source = PCGExCollections::ResolveEntrySourceProperty(Result.Entry, Result.Host, PropertyName);
		if (!Source)
		{
			return false;
		}

		const FPCGExProperty* Prop = Source->GetPtr<FPCGExProperty>();
		if (!Prop)
		{
			return false;
		}

		return Prop->TryWriteValue(PathType, OutPath);
	}

	bool WriteEntrySoftPath(
		UPCGExAssetCollection* Collection,
		int32 EntryIndex,
		FName PropertyName,
		EPCGMetadataTypes PathType,
		const void* InPath)
	{
		FPCGExPropertyOverrideEntry* Slot = ResolveWritableOverride(Collection, EntryIndex, PropertyName);
		if (!Slot)
		{
			return false;
		}

		FPCGExProperty* Prop = Slot->GetPropertyMutable();
		if (!Prop)
		{
			return false;
		}

		if (!Prop->TryReadValue(PathType, InPath))
		{
			return false;
		}

		CommitOverrideWrite(Collection, Slot);
		return true;
	}

	// Dirty path for plain field setters: weight/category/tags feed the pick cache
	// (weights, categories) or tag queries, so invalidate alongside the undo snapshot.
	void CommitEntryFieldWrite(UPCGExAssetCollection* Collection)
	{
		Collection->Modify();
		(void)Collection->MarkPackageDirty();
		Collection->InvalidateCache();
	}
}

bool UPCGExCollectionEntryBlueprintLibrary::TryGetEntryPropertyValue(
	const UPCGExAssetCollection* Collection,
	int32 EntryIndex,
	FName PropertyName,
	int32& OutValue)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UPCGExCollectionEntryBlueprintLibrary::execTryGetEntryPropertyValue)
{
	P_GET_OBJECT(UPCGExAssetCollection, Collection);
	P_GET_PROPERTY(FIntProperty, EntryIndex);
	P_GET_PROPERTY(FNameProperty, PropertyName);

	// Wildcard OutValue: the K2 node wires its actual pin type here at compile time.
	// Read the FProperty descriptor + destination memory off the stack manually.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* OutProp = Stack.MostRecentProperty;
	void* OutMem = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
		*static_cast<bool*>(RESULT_PARAM) = PCGExCollectionEntryBlueprintLibrary_Private::ReadInto(
			Collection, EntryIndex, PropertyName, OutProp, OutMem);
	P_NATIVE_END;
}

bool UPCGExCollectionEntryBlueprintLibrary::TrySetEntryPropertyOverride(
	UPCGExAssetCollection* Collection,
	int32 EntryIndex,
	FName PropertyName,
	const int32& NewValue)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UPCGExCollectionEntryBlueprintLibrary::execTrySetEntryPropertyOverride)
{
	P_GET_OBJECT(UPCGExAssetCollection, Collection);
	P_GET_PROPERTY(FIntProperty, EntryIndex);
	P_GET_PROPERTY(FNameProperty, PropertyName);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* InProp = Stack.MostRecentProperty;
	const void* InMem = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
		*static_cast<bool*>(RESULT_PARAM) = PCGExCollectionEntryBlueprintLibrary_Private::WriteFrom(
			Collection, EntryIndex, PropertyName, InProp, InMem);
	P_NATIVE_END;
}

UObject* UPCGExCollectionEntryBlueprintLibrary::TryGetEntryPropertyObject(
	const UPCGExAssetCollection* Collection,
	int32 EntryIndex,
	FName PropertyName,
	TSubclassOf<UObject> ExpectedClass,
	bool& bSuccess)
{
	bSuccess = false;

	FSoftObjectPath SoftPath;
	if (!PCGExCollectionEntryBlueprintLibrary_Private::ReadEntrySoftPath(
		Collection, EntryIndex, PropertyName, EPCGMetadataTypes::SoftObjectPath, &SoftPath))
	{
		return nullptr;
	}

	UObject* Resolved = SoftPath.ResolveObject();
	if (!Resolved)
	{
		Resolved = SoftPath.TryLoad();
	}
	if (!Resolved)
	{
		return nullptr;
	}
	if (*ExpectedClass && !Resolved->IsA(ExpectedClass))
	{
		return nullptr;
	}

	bSuccess = true;
	return Resolved;
}

bool UPCGExCollectionEntryBlueprintLibrary::TrySetEntryPropertyObject(
	UPCGExAssetCollection* Collection,
	int32 EntryIndex,
	FName PropertyName,
	UObject* NewObject)
{
	const FSoftObjectPath SoftPath(NewObject);
	return PCGExCollectionEntryBlueprintLibrary_Private::WriteEntrySoftPath(
		Collection, EntryIndex, PropertyName, EPCGMetadataTypes::SoftObjectPath, &SoftPath);
}

TSubclassOf<UObject> UPCGExCollectionEntryBlueprintLibrary::TryGetEntryPropertyClass(
	const UPCGExAssetCollection* Collection,
	int32 EntryIndex,
	FName PropertyName,
	TSubclassOf<UObject> ExpectedClass,
	bool& bSuccess)
{
	bSuccess = false;

	FSoftClassPath SoftPath;
	if (!PCGExCollectionEntryBlueprintLibrary_Private::ReadEntrySoftPath(
		Collection, EntryIndex, PropertyName, EPCGMetadataTypes::SoftClassPath, &SoftPath))
	{
		return nullptr;
	}

	UClass* Resolved = Cast<UClass>(SoftPath.ResolveObject());
	if (!Resolved)
	{
		Resolved = SoftPath.TryLoadClass<UObject>();
	}
	if (!Resolved)
	{
		return nullptr;
	}
	if (*ExpectedClass && !Resolved->IsChildOf(ExpectedClass))
	{
		return nullptr;
	}

	bSuccess = true;
	return Resolved;
}

bool UPCGExCollectionEntryBlueprintLibrary::TrySetEntryPropertyClass(
	UPCGExAssetCollection* Collection,
	int32 EntryIndex,
	FName PropertyName,
	UClass* NewClass)
{
	const FSoftClassPath SoftPath(NewClass);
	return PCGExCollectionEntryBlueprintLibrary_Private::WriteEntrySoftPath(
		Collection, EntryIndex, PropertyName, EPCGMetadataTypes::SoftClassPath, &SoftPath);
}

int32 UPCGExCollectionEntryBlueprintLibrary::GetNumEntries(const UPCGExAssetCollection* Collection)
{
	return Collection ? Collection->NumEntries() : 0;
}

bool UPCGExCollectionEntryBlueprintLibrary::IsValidEntryIndex(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	return Collection ? Collection->IsValidIndex(EntryIndex) : false;
}

bool UPCGExCollectionEntryBlueprintLibrary::IsSubCollectionEntry(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->bIsSubCollection : false;
}

int32 UPCGExCollectionEntryBlueprintLibrary::GetEntryWeight(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->Weight : 0;
}

bool UPCGExCollectionEntryBlueprintLibrary::SetEntryWeight(UPCGExAssetCollection* Collection, int32 EntryIndex, int32 NewWeight)
{
	FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetMutableEntry(Collection, EntryIndex);
	if (!Entry)
	{
		return false;
	}

	const int32 ClampedWeight = FMath::Max(0, NewWeight);
	if (Entry->Weight == ClampedWeight)
	{
		return true;
	}

	Entry->Weight = ClampedWeight;
	PCGExCollectionEntryBlueprintLibrary_Private::CommitEntryFieldWrite(Collection);
	return true;
}

FName UPCGExCollectionEntryBlueprintLibrary::GetEntryCategory(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->Category : NAME_None;
}

bool UPCGExCollectionEntryBlueprintLibrary::SetEntryCategory(UPCGExAssetCollection* Collection, int32 EntryIndex, FName NewCategory)
{
	FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetMutableEntry(Collection, EntryIndex);
	if (!Entry)
	{
		return false;
	}

	if (Entry->Category == NewCategory)
	{
		return true;
	}

	Entry->Category = NewCategory;
	PCGExCollectionEntryBlueprintLibrary_Private::CommitEntryFieldWrite(Collection);
	return true;
}

TArray<FName> UPCGExCollectionEntryBlueprintLibrary::GetEntryTags(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->Tags.Array() : TArray<FName>();
}

bool UPCGExCollectionEntryBlueprintLibrary::AddEntryTag(UPCGExAssetCollection* Collection, int32 EntryIndex, FName Tag)
{
	FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetMutableEntry(Collection, EntryIndex);
	if (!Entry || Tag.IsNone() || Entry->Tags.Contains(Tag))
	{
		return false;
	}

	Entry->Tags.Add(Tag);
	PCGExCollectionEntryBlueprintLibrary_Private::CommitEntryFieldWrite(Collection);
	return true;
}

bool UPCGExCollectionEntryBlueprintLibrary::RemoveEntryTag(UPCGExAssetCollection* Collection, int32 EntryIndex, FName Tag)
{
	FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetMutableEntry(Collection, EntryIndex);
	if (!Entry || Entry->Tags.Remove(Tag) == 0)
	{
		return false;
	}

	PCGExCollectionEntryBlueprintLibrary_Private::CommitEntryFieldWrite(Collection);
	return true;
}

bool UPCGExCollectionEntryBlueprintLibrary::EntryHasTag(const UPCGExAssetCollection* Collection, int32 EntryIndex, FName Tag)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->Tags.Contains(Tag) : false;
}

bool UPCGExCollectionEntryBlueprintLibrary::HasEntryPropertyOverride(const UPCGExAssetCollection* Collection, int32 EntryIndex, FName PropertyName)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->PropertyOverrides.HasOverride(PropertyName) : false;
}

bool UPCGExCollectionEntryBlueprintLibrary::SetEntryPropertyOverrideEnabled(UPCGExAssetCollection* Collection, int32 EntryIndex, FName PropertyName, bool bEnabled)
{
	FPCGExPropertyOverrideEntry* Slot = PCGExCollectionEntryBlueprintLibrary_Private::ResolveWritableOverride(Collection, EntryIndex, PropertyName);
	if (!Slot)
	{
		return false;
	}

	if (Slot->bEnabled == bEnabled)
	{
		return true;
	}

	Slot->bEnabled = bEnabled;
	Collection->Modify();
	(void)Collection->MarkPackageDirty();
	return true;
}

FSoftObjectPath UPCGExCollectionEntryBlueprintLibrary::GetEntryStagingPath(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->Staging.Path : FSoftObjectPath();
}

FBox UPCGExCollectionEntryBlueprintLibrary::GetEntryStagingBounds(const UPCGExAssetCollection* Collection, int32 EntryIndex)
{
	const FPCGExAssetCollectionEntry* Entry = PCGExCollectionEntryBlueprintLibrary_Private::GetEntry(Collection, EntryIndex);
	return Entry ? Entry->Staging.Bounds : FBox(ForceInit);
}
