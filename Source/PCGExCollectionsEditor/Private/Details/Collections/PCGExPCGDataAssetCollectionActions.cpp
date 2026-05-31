// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExPCGDataAssetCollectionActions.h"

#include "FileHelpers.h"
#include "ToolMenuSection.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Collections/PCGExPCGDataAssetCollection.h"
#include "Details/Collections/PCGExAssetCollectionEditor.h"
#include "Details/Collections/PCGExCollectionEditorUtils.h"
#include "Details/Collections/PCGExPCGDataAssetCollectionEditor.h"
#include "Misc/MessageDialog.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Views/SListView.h"

namespace PCGExPCGDataAssetCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets)
	{
		PCGExCollectionEditorUtils::CreateCollectionFromSelection(
			UPCGExPCGDataAssetCollection::StaticClass(),
			TEXT("SMC_NewPCGDataAssetCollection"),
			SelectedAssets);
	}

	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExPCGDataAssetCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection)
	{
		if (SelectedCollections.IsEmpty() || SelectedAssets.IsEmpty())
		{
			return;
		}

		for (const TObjectPtr<UPCGExPCGDataAssetCollection>& Collection : SelectedCollections)
		{
			Collection->EDITOR_AddBrowserSelectionTyped(SelectedAssets);
		}
	}
}

EAssetCommandResult UAssetDefinition_PCGExPCGDataAssetCollection::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UPCGExPCGDataAssetCollection* Collection : OpenArgs.LoadObjects<UPCGExPCGDataAssetCollection>())
	{
		TSharedRef<FPCGExPCGDataAssetCollectionEditor> Editor = MakeShared<FPCGExPCGDataAssetCollectionEditor>();
		Editor->InitEditor(Collection, OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost);
	}
	return EAssetCommandResult::Handled;
}
