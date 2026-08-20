// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Collections/PCGExClusterSketchCollection.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetData.h"
#endif

#include "Helpers/PCGExStreamingHelpers.h"

// Registered from StartupModule, NOT from a static initializer -- see the header for why. Hand-built
// FTypeInfo rather than PCGEX_REGISTER_COLLECTION_TYPE*, which all emit static auto-registrars;
// UPCGExOmniCollection registers the same way.
void PCGExSketch::RegisterCollectionType()
{
	using namespace PCGExAssetCollection;

	FTypeInfo Info;
	Info.Id = PCGExSketch::CollectionTypeId;
	Info.CollectionClass = UPCGExClusterSketchCollection::StaticClass();
	Info.EntryStruct = FPCGExClusterSketchCollectionEntry::StaticStruct();
	Info.DisplayName = NSLOCTEXT("PCGEx", "ClusterSketchCollection", "Cluster Sketch Collection");
	Info.ParentType = TypeIds::Base;
	FTypeRegistry::Get().Register(Info);

#if WITH_EDITOR
	// Omni drag-drop ingestion, registered from HERE rather than PCGExOmniCollection.cpp so that
	// Collections keeps no dependency on the sketch module. Must follow Register above: Customize
	// mutates an existing entry.
	// Priority 15 sits between Mesh/Skinned (10) and Actor (20). No detector conflict exists:
	// UPCGExClusterSketch is a plain UDataAsset, and the PCGDataAsset detector tests UPCGDataAsset.
	FTypeRegistry::Get().Customize(
		PCGExSketch::CollectionTypeId,
		[](FTypeInfo& Info)
		{
			Info.SourceDetectPriority = 15;
			Info.DetectSourceAsset = [](const FAssetData& Asset) { return Asset.IsInstanceOf<UPCGExClusterSketch>(); };
		});
#endif
}

#pragma region FPCGExClusterSketchCollectionEntry

bool FPCGExClusterSketchCollectionEntry::Validate(const UPCGExAssetCollection* ParentCollection)
{
	if (!bIsSubCollection)
	{
		if (!Sketch.ToSoftObjectPath().IsValid() && ParentCollection->bDoNotIgnoreInvalidEntries)
		{
			return false;
		}
	}

	return FPCGExAssetCollectionEntry::Validate(ParentCollection);
}

void FPCGExClusterSketchCollectionEntry::UpdateStaging(const UPCGExAssetCollection* OwningCollection, int32 InInternalIndex, bool bRecursive)
{
	ClearManagedSockets();

	if (bIsSubCollection)
	{
		FPCGExAssetCollectionEntry::UpdateStaging(OwningCollection, InInternalIndex, bRecursive);
		return;
	}

	Staging.Path = Sketch.ToSoftObjectPath();

	TSharedPtr<FStreamableHandle> Handle = PCGExHelpers::LoadBlocking_AnyThreadTpl(Sketch);

	if (const UPCGExClusterSketch* S = Sketch.Get())
	{
		// Resolves lattice-bound vertices through the sketch's own basis, so staged bounds match the
		// printed extent rather than the authored transforms.
		Staging.Bounds = S->GetBounds();
	}
	else
	{
		Staging.Bounds = FBox(ForceInit);
	}

	FPCGExAssetCollectionEntry::UpdateStaging(OwningCollection, InInternalIndex, bRecursive);
	PCGExHelpers::SafeReleaseHandle(Handle);
}

void FPCGExClusterSketchCollectionEntry::SetAssetPath(const FSoftObjectPath& InPath)
{
	FPCGExAssetCollectionEntry::SetAssetPath(InPath);
	Sketch = TSoftObjectPtr<UPCGExClusterSketch>(InPath);
}

#pragma endregion

#pragma region UPCGExClusterSketchCollection

#if WITH_EDITOR
void UPCGExClusterSketchCollection::EDITOR_AddBrowserSelectionInternal(const TArray<FAssetData>& InAssetData)
{
	UPCGExAssetCollection::EDITOR_AddBrowserSelectionInternal(InAssetData);

	for (const FAssetData& SelectedAsset : InAssetData)
	{
		TSoftObjectPtr<UPCGExClusterSketch> Candidate = TSoftObjectPtr<UPCGExClusterSketch>(SelectedAsset.ToSoftObjectPath());
		if (!Candidate.LoadSynchronous())
		{
			continue;
		}

		bool bAlreadyExists = false;
		for (const FPCGExClusterSketchCollectionEntry& ExistingEntry : Entries)
		{
			if (ExistingEntry.Sketch == Candidate)
			{
				bAlreadyExists = true;
				break;
			}
		}

		if (bAlreadyExists)
		{
			continue;
		}

		FPCGExClusterSketchCollectionEntry Entry = FPCGExClusterSketchCollectionEntry();
		Entry.Sketch = Candidate;

		Entries.Add(Entry);
	}
}
#endif

#pragma endregion
