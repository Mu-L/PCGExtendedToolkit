// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExLevelCollectionActions.h"

#include "FileHelpers.h"
#include "ToolMenuSection.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Collections/PCGExLevelCollection.h"
#include "Details/Collections/PCGExAssetCollectionEditor.h"
#include "Details/Collections/PCGExCollectionEditorUtils.h"
#include "Details/Collections/PCGExLevelCollectionEditor.h"
#include "Misc/MessageDialog.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Views/SListView.h"

namespace PCGExLevelCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets)
	{
		PCGExCollectionEditorUtils::CreateCollectionFromSelection(
			UPCGExLevelCollection::StaticClass(),
			TEXT("SMC_NewLevelCollection"),
			SelectedAssets);
	}

	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExLevelCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection)
	{
		if (SelectedCollections.IsEmpty() || SelectedAssets.IsEmpty())
		{
			return;
		}

		for (const TObjectPtr<UPCGExLevelCollection>& Collection : SelectedCollections)
		{
			Collection->EDITOR_AddBrowserSelectionTyped(SelectedAssets);
		}
	}
}

EAssetCommandResult UAssetDefinition_PCGExLevelCollection::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UPCGExLevelCollection* Collection : OpenArgs.LoadObjects<UPCGExLevelCollection>())
	{
		TSharedRef<FPCGExLevelCollectionEditor> Editor = MakeShared<FPCGExLevelCollectionEditor>();
		Editor->InitEditor(Collection, OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost);
	}
	return EAssetCommandResult::Handled;
}
