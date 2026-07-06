// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExVariantCollectionActions.h"

#include "Collections/PCGExVariantCollection.h"
#include "Details/Collections/PCGExCollectionEditorTypeRegistry.h"
#include "Toolkits/SimpleAssetEditor.h"

// Variant collections are registered by hand rather than via PCGEX_REGISTER_COLLECTION_EDITOR_TYPE:
// the macro wires OpenEditor to a grid-view toolkit whose tile picker assumes homogeneous entries
// with one known asset property — meaningless for heterogeneous override rows. Until the dedicated
// mirror-against-source editor exists, the plain details panel (FSimpleAssetEditor) is the correct
// authoring surface: FInstancedStruct rows are fully editable there. Variants are also asset-only —
// they never participate in the content-browser "Create from selection" flow.
namespace
{
	struct FRegisterVariantEditorTypeInfo
	{
		FRegisterVariantEditorTypeInfo()
		{
			FCollectionEditorTypeRegistry::AddPendingRegistration([]()
			{
				FCollectionEditorTypeInfo Info;
				Info.Id = PCGExAssetCollection::TypeIds::Variant;
				Info.CollectionClass = UPCGExVariantCollection::StaticClass();
				Info.SourceAssetClass = nullptr;
				Info.DefaultAssetNamePrefix = TEXT("SMC_NewVariantCollection");
				Info.AssetColor = FLinearColor(FColor(200, 120, 220));
				Info.DisplayName = INVTEXT("Variant Collection");
				Info.AssetDescription = INVTEXT("Per-entry overrides for one or more source collections, for end-of-pipeline asset swapping (biomes, themes).");
				Info.bSupportsMenuCreation = false;
				Info.DetectSourceAsset = [](const FAssetData&) { return false; };
				Info.DetectCollectionAsset = [](const FAssetData& Asset) { return Asset.IsInstanceOf<UPCGExVariantCollection>(); };
				Info.OpenEditor = [](UPCGExAssetCollection* Collection, const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host)
				{
					FSimpleAssetEditor::CreateEditor(Mode, Host, Collection);
				};
				Info.CreateCollection = [](const TArray<FAssetData>&) {};
				Info.UpdateCollections = [](const TArray<TObjectPtr<UPCGExAssetCollection>>&, const TArray<FAssetData>&) {};
				FCollectionEditorTypeRegistry::Get().Register(MoveTemp(Info));
			});
		}
	} GRegisterVariantEditorTypeInfo;
}
