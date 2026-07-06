// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/Collections/PCGExVariantCollectionEditor.h"

#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Framework/Application/SlateApplication.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "Collections/PCGExVariantCollection.h"
#include "Details/Collections/SPCGExVariantGridView.h"

#define LOCTEXT_NAMESPACE "PCGExVariantCollectionEditor"

FPCGExVariantCollectionEditor::FPCGExVariantCollectionEditor()
	: FPCGExAssetCollectionEditor()
{
}

void FPCGExVariantCollectionEditor::CreateTabs(TArray<PCGExAssetCollectionEditor::TabInfos>& OutTabs)
{
	// No entries tab (there is no homogeneous Entries array) — a Collection Settings tab
	// (full asset details, incl. raw Sources as escape hatch) plus the variant grid.
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bUpdatesFromSelection = false;
	DetailsArgs.bLockable = false;
	DetailsArgs.bAllowSearch = true;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NotifyHook = nullptr;
	DetailsArgs.bAllowMultipleTopLevelObjects = false;

	const TSharedPtr<IDetailsView> DetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	DetailsView->SetObject(EditedCollection.Get());

	PCGExAssetCollectionEditor::TabInfos& SettingsInfos = OutTabs.Emplace_GetRef(FName("Collection"), DetailsView, FName("Collection Settings"));
	SettingsInfos.Icon = TEXT("Settings");

	// Grid LAST so the reverse-iteration loop in InitEditor makes it the leftmost tab
	// (same ordering contract as the base editor).
	if (!ThumbnailPool.IsValid())
	{
		ThumbnailPool = MakeShared<FAssetThumbnailPool>(128);
	}

	SAssignNew(VariantGrid, SPCGExVariantGridView)
	.Collection(Cast<UPCGExVariantCollection>(EditedCollection.Get()))
	.ThumbnailPool(ThumbnailPool)
	.TileSize(192.f);

	PCGExAssetCollectionEditor::TabInfos& GridInfos = OutTabs.Emplace_GetRef(FName("Swaps"), VariantGrid, FName("Swaps"));
	GridInfos.Icon = TEXT("Entries");
	GridInfos.bIsDetailsView = false;

	FToolBarBuilder HeaderToolbarBuilder(GetToolkitCommands(), FMultiBoxCustomization::None);
	HeaderToolbarBuilder.SetStyle(&FAppStyle::Get(), FName("Toolbar"));
	BuildAssetHeaderToolbar(HeaderToolbarBuilder);
	GridInfos.Header = HeaderToolbarBuilder.MakeWidget();
}

void FPCGExVariantCollectionEditor::BuildAssetHeaderToolbar(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.AddComboButton(
		FUIAction(),
		FOnGetContent::CreateSP(this, &FPCGExVariantCollectionEditor::MakeAddSourceMenu),
		LOCTEXT("AddSource", "Add Source"),
		LOCTEXT("AddSourceTooltip", "Add a source collection to theme. Its entries appear as swap candidates in the grid."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"));

	ToolbarBuilder.AddComboButton(
		FUIAction(),
		FOnGetContent::CreateSP(this, &FPCGExVariantCollectionEditor::MakeAddAssetSwapMenu),
		LOCTEXT("AddAssetSwap", "Add Asset Swap"),
		LOCTEXT("AddAssetSwapTooltip", "Add an asset-path swap rule: every source entry staging the picked asset swaps to the rule's payload (unless an explicit per-entry swap overrides it)."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Link"));

	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateLambda([this]() { OnSyncMappings(); })),
		NAME_None,
		LOCTEXT("SyncMappings", "Sync Mappings"),
		LOCTEXT("SyncMappingsTooltip", "Re-resolve and bake the swap mappings against the live sources (also runs automatically on save)."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"));
}

TSharedRef<SWidget> FPCGExVariantCollectionEditor::MakeAddSourceMenu()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	FAssetPickerConfig PickerConfig;
	PickerConfig.SelectionMode = ESelectionMode::Single;
	PickerConfig.InitialAssetViewType = EAssetViewType::List;
	PickerConfig.bAllowNullSelection = false;
	PickerConfig.Filter.bRecursiveClasses = true;
	PickerConfig.Filter.ClassPaths.Add(UPCGExAssetCollection::StaticClass()->GetClassPathName());
	PickerConfig.OnAssetSelected = FOnAssetSelected::CreateSP(this, &FPCGExVariantCollectionEditor::OnSourceAssetPicked);
	PickerConfig.OnShouldFilterAsset = FOnShouldFilterAsset::CreateLambda(
		[WeakEdited = EditedCollection](const FAssetData& Asset)
		{
			// Variants can't source other variants (no chaining, by design), nor themselves.
			if (Asset.IsInstanceOf<UPCGExVariantCollection>())
			{
				return true;
			}
			if (const UPCGExAssetCollection* Edited = WeakEdited.Get();
				Edited && Asset.GetSoftObjectPath() == FSoftObjectPath(Edited))
			{
				return true;
			}
			return false;
		});

	return SNew(SBox)
		.WidthOverride(380.f)
		.HeightOverride(420.f)
		[
			ContentBrowserModule.Get().CreateAssetPicker(PickerConfig)
		];
}

void FPCGExVariantCollectionEditor::OnSourceAssetPicked(const FAssetData& AssetData)
{
	FSlateApplication::Get().DismissAllMenus();

	UPCGExVariantCollection* Variant = Cast<UPCGExVariantCollection>(EditedCollection.Get());
	UPCGExAssetCollection* Source = Cast<UPCGExAssetCollection>(AssetData.GetAsset());

	if (!Variant || !Source)
	{
		return;
	}

	const FSoftObjectPath SourcePath(Source);
	for (const FPCGExVariantSource& Group : Variant->Sources)
	{
		if (Group.Source.ToSoftObjectPath() == SourcePath)
		{
			FNotificationInfo Info(LOCTEXT("SourceAlreadyAdded", "This collection is already a source of the variant."));
			Info.ExpireDuration = 3.f;
			FSlateNotificationManager::Get().AddNotification(Info);
			return;
		}
	}

	{
		FScopedTransaction Transaction(LOCTEXT("AddSourceTransaction", "Add Variant Source"));
		Variant->Modify();
		FPCGExVariantSource& NewGroup = Variant->Sources.AddDefaulted_GetRef();
		NewGroup.Source = Source;
		Variant->PostEditChange();
	}

	if (VariantGrid.IsValid())
	{
		VariantGrid->RefreshGrid();
	}
}

TSharedRef<SWidget> FPCGExVariantCollectionEditor::MakeAddAssetSwapMenu()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	// No class filter: rules match any staged asset path (meshes, actors, levels, ...).
	FAssetPickerConfig PickerConfig;
	PickerConfig.SelectionMode = ESelectionMode::Single;
	PickerConfig.InitialAssetViewType = EAssetViewType::List;
	PickerConfig.bAllowNullSelection = false;
	PickerConfig.OnAssetSelected = FOnAssetSelected::CreateSP(this, &FPCGExVariantCollectionEditor::OnSwapAssetPicked);

	return SNew(SBox)
		.WidthOverride(380.f)
		.HeightOverride(420.f)
		[
			ContentBrowserModule.Get().CreateAssetPicker(PickerConfig)
		];
}

void FPCGExVariantCollectionEditor::OnSwapAssetPicked(const FAssetData& AssetData)
{
	FSlateApplication::Get().DismissAllMenus();

	UPCGExVariantCollection* Variant = Cast<UPCGExVariantCollection>(EditedCollection.Get());
	if (!Variant)
	{
		return;
	}

	const FSoftObjectPath MatchPath = AssetData.GetSoftObjectPath();

	for (const FPCGExVariantPathOverride& Rule : Variant->PathOverrides)
	{
		if (Rule.MatchAsset == MatchPath)
		{
			FNotificationInfo Info(LOCTEXT("RuleAlreadyExists", "An asset swap rule for this asset already exists."));
			Info.ExpireDuration = 3.f;
			FSlateNotificationManager::Get().AddNotification(Info);
			return;
		}
	}

	// Seed the payload from the first source entry staging this asset — gives the rule a
	// concrete type and sensible defaults. Falls back to an unset payload (typed later via
	// the Collection Settings tab) when nothing matches yet.
	const UScriptStruct* SeedStruct = nullptr;
	const FPCGExAssetCollectionEntry* SeedEntry = nullptr;

	for (const FPCGExVariantSource& Group : Variant->Sources)
	{
		const UPCGExAssetCollection* Src = Group.Source.Get();
		if (!Src)
		{
			continue;
		}

		const PCGExAssetCollection::FTypeInfo* TypeInfo = PCGExAssetCollection::FTypeRegistry::Get().FindByClass(Src->GetClass());
		if (!TypeInfo || !TypeInfo->EntryStruct)
		{
			continue;
		}

		Src->ForEachEntry([&](const FPCGExAssetCollectionEntry* Entry, int32)
		{
			if (!SeedEntry && !Entry->bIsSubCollection && Entry->Staging.Path == MatchPath)
			{
				SeedEntry = Entry;
			}
		});

		if (SeedEntry)
		{
			SeedStruct = TypeInfo->EntryStruct;
			break;
		}
	}

	{
		FScopedTransaction Transaction(LOCTEXT("AddAssetSwapTransaction", "Add Asset Swap Rule"));
		Variant->Modify();

		FPCGExVariantPathOverride& NewRule = Variant->PathOverrides.AddDefaulted_GetRef();
		NewRule.MatchAsset = MatchPath;

		if (SeedStruct && SeedEntry)
		{
			NewRule.Entry.InitializeAs(SeedStruct, reinterpret_cast<const uint8*>(SeedEntry));
			if (FPCGExAssetCollectionEntry* Payload = NewRule.Entry.GetMutablePtr<FPCGExAssetCollectionEntry>())
			{
				Payload->EntryId = 0;
			}
		}
		else
		{
			FNotificationInfo Info(LOCTEXT("RuleNoMatchYet", "No declared source currently stages this asset — the rule is inert until one does. Set its payload type via Collection Settings."));
			Info.ExpireDuration = 5.f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}

		Variant->PostEditChange();
	}

	if (VariantGrid.IsValid())
	{
		VariantGrid->RefreshGrid();
	}
}

FReply FPCGExVariantCollectionEditor::OnSyncMappings()
{
	UPCGExVariantCollection* Variant = Cast<UPCGExVariantCollection>(EditedCollection.Get());
	if (!Variant)
	{
		return FReply::Handled();
	}

	{
		FScopedTransaction Transaction(LOCTEXT("SyncMappingsTransaction", "Sync Variant Mappings"));
		Variant->Modify();
		Variant->SyncVariantMappings();
	}

	if (VariantGrid.IsValid())
	{
		VariantGrid->RefreshGrid();
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
