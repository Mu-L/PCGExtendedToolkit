// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExCollectionEditorHelpers.h"

#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/PCGExAssetCollection.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace PCGExCollectionEditorHelpers
{
	void CreateCollectionFromTyped(
		const TArray<FAssetData>& SelectedAssets,
		UClass* CollectionClass,
		const TCHAR* DefaultAssetName)
	{
		if (SelectedAssets.IsEmpty() || !CollectionClass)
		{
			return;
		}

		const FString CollectionAssetName = DefaultAssetName;
		const FString CollectionAssetPath = SelectedAssets[0].PackagePath.ToString();
		const FString PackageName = FPaths::Combine(CollectionAssetPath, CollectionAssetName);

		FText Reason;
		if (!FPackageName::IsValidObjectPath(PackageName, &Reason))
		{
			UE_LOG(LogTemp, Error, TEXT("Invalid package path '%s': %s."), *PackageName, *Reason.ToString());
			return;
		}

		UPackage* Package = FPackageName::DoesPackageExist(PackageName) ? LoadPackage(nullptr, *PackageName, LOAD_None) : nullptr;

		UPCGExAssetCollection* TargetCollection = nullptr;
		bool bIsNewCollection = false;

		if (Package)
		{
			UObject* Object = FindObjectFast<UObject>(Package, *CollectionAssetName);
			if (Object && Object->GetClass() != CollectionClass)
			{
				// Existing asset under that name isn't our collection type -- evict it.
				Object->SetFlags(RF_Transient);
				Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
				bIsNewCollection = true;
			}
			else
			{
				TargetCollection = Cast<UPCGExAssetCollection>(Object);
			}
		}
		else
		{
			Package = CreatePackage(*PackageName);
			if (!Package)
			{
				UE_LOG(LogTemp, Error, TEXT("Unable to create package with name '%s'."), *PackageName);
				return;
			}
			bIsNewCollection = true;
		}

		if (!TargetCollection)
		{
			constexpr EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;
			TargetCollection = NewObject<UPCGExAssetCollection>(Package, CollectionClass, FName(*CollectionAssetName), Flags);
		}

		if (TargetCollection)
		{
			if (bIsNewCollection)
			{
				FAssetRegistryModule::AssetCreated(TargetCollection);
			}

			TArray<TObjectPtr<UPCGExAssetCollection>> Collections;
			Collections.Add(TargetCollection);
			UpdateCollectionsFromTyped(Collections, SelectedAssets);
		}

		FEditorFileUtils::PromptForCheckoutAndSave({Package}, /*bCheckDirty=*/false, /*bPromptToSave=*/false);
	}

	void UpdateCollectionsFromTyped(
		const TArray<TObjectPtr<UPCGExAssetCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets)
	{
		if (SelectedCollections.IsEmpty() || SelectedAssets.IsEmpty())
		{
			return;
		}

		for (const TObjectPtr<UPCGExAssetCollection>& Collection : SelectedCollections)
		{
			Collection->EDITOR_AddBrowserSelectionTyped(SelectedAssets);
		}
	}
}
