// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExSkinnedMeshCollectionActions.h"

#include "FileHelpers.h"
#include "ToolMenuSection.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Collections/PCGExSkinnedMeshCollection.h"
#include "Details/Collections/PCGExAssetCollectionEditor.h"
#include "Details/Collections/PCGExSkinnedMeshCollectionEditor.h"
#include "Misc/MessageDialog.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Views/SListView.h"

namespace PCGExSkinnedMeshCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets)
	{
		if (SelectedAssets.IsEmpty())
		{
			return;
		}

		FString CollectionAssetName = TEXT("SMC_NewSkinnedMeshCollection");
		FString CollectionAssetPath = SelectedAssets[0].PackagePath.ToString();
		FString PackageName = FPaths::Combine(CollectionAssetPath, CollectionAssetName);

		FText Reason;
		if (!FPackageName::IsValidObjectPath(PackageName, &Reason))
		{
			UE_LOG(LogTemp, Error, TEXT("Invalid package path '%s': %s."), *PackageName, *Reason.ToString());
			return;
		}

		UPackage* Package = FPackageName::DoesPackageExist(PackageName) ? LoadPackage(nullptr, *PackageName, LOAD_None) : nullptr;

		UPCGExSkinnedMeshCollection* TargetCollection = nullptr;
		bool bIsNewCollection = false;

		if (Package)
		{
			UObject* Object = FindObjectFast<UObject>(Package, *CollectionAssetName);
			if (Object && Object->GetClass() != UPCGExSkinnedMeshCollection::StaticClass())
			{
				Object->SetFlags(RF_Transient);
				Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
				bIsNewCollection = true;
			}
			else
			{
				TargetCollection = Cast<UPCGExSkinnedMeshCollection>(Object);
			}
		}
		else
		{
			Package = CreatePackage(*PackageName);

			if (Package)
			{
				bIsNewCollection = true;
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("Unable to create package with name '%s'."), *PackageName);
				return;
			}
		}

		if (!TargetCollection)
		{
			constexpr EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;
			TargetCollection = NewObject<UPCGExSkinnedMeshCollection>(Package, UPCGExSkinnedMeshCollection::StaticClass(), FName(*CollectionAssetName), Flags);
		}

		if (TargetCollection)
		{
			if (bIsNewCollection)
			{
				FAssetRegistryModule::AssetCreated(TargetCollection);
			}

			TArray<TObjectPtr<UPCGExSkinnedMeshCollection>> SelectedCollections;
			SelectedCollections.Add(TargetCollection);

			UpdateCollectionsFrom(SelectedCollections, SelectedAssets, bIsNewCollection);
		}

		if (Package)
		{
			FEditorFileUtils::PromptForCheckoutAndSave({Package}, /*bCheckDirty=*/false, /*bPromptToSave=*/false);
		}
	}

	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExSkinnedMeshCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection)
	{
		if (SelectedCollections.IsEmpty() || SelectedAssets.IsEmpty())
		{
			return;
		}

		for (const TObjectPtr<UPCGExSkinnedMeshCollection>& Collection : SelectedCollections)
		{
			Collection->EDITOR_AddBrowserSelectionTyped(SelectedAssets);
		}
	}
}

EAssetCommandResult UAssetDefinition_PCGExSkinnedMeshCollection::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UPCGExSkinnedMeshCollection* Collection : OpenArgs.LoadObjects<UPCGExSkinnedMeshCollection>())
	{
		TSharedRef<FPCGExSkinnedMeshCollectionEditor> Editor = MakeShared<FPCGExSkinnedMeshCollectionEditor>();
		Editor->InitEditor(Collection, OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost);
	}
	return EAssetCommandResult::Handled;
}
