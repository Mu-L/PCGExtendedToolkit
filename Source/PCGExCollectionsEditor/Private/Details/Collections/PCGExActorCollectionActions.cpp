// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExActorCollectionActions.h"

#include "FileHelpers.h"
#include "ToolMenuSection.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Collections/PCGExActorCollection.h"
#include "Details/Collections/PCGExActorCollectionEditor.h"
#include "Details/Collections/PCGExAssetCollectionEditor.h"
#include "Details/Collections/PCGExCollectionEditorUtils.h"
#include "Misc/MessageDialog.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Views/SListView.h"

namespace PCGExActorCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets)
	{
		PCGExCollectionEditorUtils::CreateCollectionFromSelection(
			UPCGExActorCollection::StaticClass(),
			TEXT("SMC_NewActorCollection"),
			SelectedAssets);
	}

	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExActorCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection)
	{
		if (SelectedCollections.IsEmpty() || SelectedAssets.IsEmpty())
		{
			return;
		}

		for (const TObjectPtr<UPCGExActorCollection>& Collection : SelectedCollections)
		{
			Collection->EDITOR_AddBrowserSelectionTyped(SelectedAssets);
		}
	}
}

EAssetCommandResult UAssetDefinition_PCGExActorCollection::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UPCGExActorCollection* Collection : OpenArgs.LoadObjects<UPCGExActorCollection>())
	{
		TSharedRef<FPCGExActorCollectionEditor> Editor = MakeShared<FPCGExActorCollectionEditor>();
		Editor->InitEditor(Collection, OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost);
	}
	return EAssetCommandResult::Handled;
}
