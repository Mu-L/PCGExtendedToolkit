// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/PCGExPropertySchemaCollectionCustomization.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailPropertyRow.h"
#include "IPropertyUtilities.h"
#include "PCGExInlineWidgetRegistry.h"
#include "PCGExProperty.h"
#include "PCGExPropertyCollectionComponent.h"
#include "PCGExPropertySchemaAsset.h"
#include "PropertyHandle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IPropertyTypeCustomization> FPCGExPropertySchemaCollectionCustomization::MakeInstance()
{
	return MakeShareable(new FPCGExPropertySchemaCollectionCustomization());
}

FPCGExPropertySchemaCollectionCustomization::~FPCGExPropertySchemaCollectionCustomization()
{
	UnsubscribeImportedAssets();
}

void FPCGExPropertySchemaCollectionCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Store utilities and handles
	WeakPropertyUtilities = CustomizationUtils.GetPropertyUtilities();
	PropertyHandlePtr = PropertyHandle;

	// Detect instance mode: the outer object is a UPCGExPropertyCollectionComponent that was
	// inherited from a Blueprint class (SCS/UCS/Native), not added directly to the actor instance.
	// Components added per-instance (CreationMethod == Instance) own their own schema and should
	// retain full editing; only inherited components should lock the schema and redirect the user
	// to the Blueprint. Other users of FPCGExPropertySchemaCollection (Tuple nodes, data assets,
	// etc.) won't cast to the component type, so they are unaffected.
	bIsInstanceMode = false;
	if (TSharedPtr<IPropertyUtilities> Utils = WeakPropertyUtilities.Pin())
	{
		for (const TWeakObjectPtr<UObject>& ObjPtr : Utils->GetSelectedObjects())
		{
			if (const UPCGExPropertyCollectionComponent* Comp = Cast<UPCGExPropertyCollectionComponent>(ObjPtr.Get()))
			{
				if (!Comp->IsTemplate() && Comp->CreationMethod != EComponentCreationMethod::Instance)
				{
					bIsInstanceMode = true;
					break;
				}
			}
		}
	}

	HeaderRow
		.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		[
			SNew(STextBlock)
			.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertySchemaCollectionCustomization::GetHeaderText)))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
		];
}

FText FPCGExPropertySchemaCollectionCustomization::GetHeaderText() const
{
	// Called per Slate redraw -- avoid the full Resolve walk by serving a cache populated by
	// CustomizeChildren. First call (before the section is expanded) falls through to compute.
	if (CachedResolvedCount < 0)
	{
		CachedResolvedCount = 0;
		if (TSharedPtr<IPropertyHandle> Handle = PropertyHandlePtr.Pin())
		{
			TArray<void*> RawData;
			Handle->AccessRawData(RawData);
			if (!RawData.IsEmpty() && RawData[0])
			{
				TArray<FPCGExPropertyResolved> Resolved;
				static_cast<const FPCGExPropertySchemaCollection*>(RawData[0])->Resolve(Resolved);
				CachedResolvedCount = Resolved.Num();
			}
		}
	}

	return FText::FromString(FString::Printf(TEXT("%d %s"), CachedResolvedCount, CachedResolvedCount == 1 ? TEXT("property") : TEXT("properties")));
}

void FPCGExPropertySchemaCollectionCustomization::OnSchemasArrayChanged()
{
	TSharedPtr<IPropertyHandle> PropertyHandle = PropertyHandlePtr.Pin();
	if (!PropertyHandle.IsValid()) { return; }

	TArray<void*> RawData;
	PropertyHandle->AccessRawData(RawData);
	if (RawData.IsEmpty() || !RawData[0]) { return; }

	FPCGExPropertySchemaCollection* Collection = static_cast<FPCGExPropertySchemaCollection*>(RawData[0]);
	for (FPCGExPropertySchema& Schema : Collection->Schemas) { Schema.SyncPropertyName(); }
	Collection->ReconcileImportOverrides();

	CachedResolvedCount = -1;
	if (TSharedPtr<IPropertyUtilities> PropertyUtilities = WeakPropertyUtilities.Pin())
	{
		PropertyUtilities->ForceRefresh();
	}
}

void FPCGExPropertySchemaCollectionCustomization::OnImportedSchemasArrayChanged()
{
	TSharedPtr<IPropertyHandle> PropertyHandle = PropertyHandlePtr.Pin();
	if (!PropertyHandle.IsValid()) { return; }

	TArray<void*> RawData;
	PropertyHandle->AccessRawData(RawData);
	if (!RawData.IsEmpty() && RawData[0])
	{
		static_cast<FPCGExPropertySchemaCollection*>(RawData[0])->ReconcileImportOverrides();
	}

	CachedResolvedCount = -1;
	if (TSharedPtr<IPropertyUtilities> PropertyUtilities = WeakPropertyUtilities.Pin())
	{
		PropertyUtilities->ForceRefresh();
	}
}

void FPCGExPropertySchemaCollectionCustomization::OnImportedAssetChanged(UPCGExPropertySchemaAsset* /*ChangedAsset*/)
{
	ReconcileAndNotify();
}

void FPCGExPropertySchemaCollectionCustomization::ReconcileAndNotify()
{
	TSharedPtr<IPropertyHandle> PropertyHandle = PropertyHandlePtr.Pin();
	if (!PropertyHandle.IsValid()) { return; }

	TArray<void*> RawData;
	PropertyHandle->AccessRawData(RawData);
	if (!RawData.IsEmpty() && RawData[0])
	{
		static_cast<FPCGExPropertySchemaCollection*>(RawData[0])->ReconcileImportOverrides();
	}

	PropertyHandle->NotifyPostChange(EPropertyChangeType::ValueSet);

	// Deferred refresh: structural rebuild can fire during a Slate event chain, so we queue
	// instead of forcing immediately. Matches FPCGExPropertyOverridesCustomization.
	CachedResolvedCount = -1;
	if (TSharedPtr<IPropertyUtilities> PropertyUtilities = WeakPropertyUtilities.Pin())
	{
		PropertyUtilities->RequestRefresh();
	}
}

void FPCGExPropertySchemaCollectionCustomization::EmitSectionHeader(IDetailChildrenBuilder& ChildBuilder, const FString& Title) const
{
	// Left padding of -16 preserves the ZoneGraph-style offset that aligns section labels
	// with the property tree's gutter rather than the value column.
	ChildBuilder.AddCustomRow(FText::FromString(Title))
	            .WholeRowContent()
		[
			SNew(SBox)
			.Padding(FMargin(-16, 4, 0, 4))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Title.ToUpper()))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f)))
			]
		];
}

void FPCGExPropertySchemaCollectionCustomization::EmitImportSections(IDetailChildrenBuilder& ChildBuilder, TSharedRef<IPropertyHandle> CollectionHandle, const TArray<FPCGExPropertyResolved>& Resolved)
{
	TSharedPtr<IPropertyHandle> ImportOverridesHandle = CollectionHandle->GetChildHandle(TEXT("ImportOverrides"));
	if (!ImportOverridesHandle.IsValid()) { return; }
	TSharedPtr<IPropertyHandle> OverridesArrayHandle = ImportOverridesHandle->GetChildHandle(TEXT("Overrides"));
	if (!OverridesArrayHandle.IsValid()) { return; }

	// Force the detail panel to build property nodes for the ImportOverrides subtree by
	// adding the parent as a collapsed row. Without this, per-instance UPROPERTY delta
	// tracking can fail for writes through the entry handles below -- UE only builds the
	// node tree for branches reached via AddProperty, and writes through unbuilt nodes
	// silently revert on instances (CDO/archetype propagation re-reads stale memory).
	ChildBuilder.AddProperty(ImportOverridesHandle.ToSharedRef()).Visibility(EVisibility::Collapsed);

	int32 ImportIndex = 0;
	UPCGExPropertySchemaAsset* CurrentSection = nullptr;
	for (const FPCGExPropertyResolved& Entry : Resolved)
	{
		if (!Entry.OwningAsset) { continue; }

		if (Entry.OwningAsset != CurrentSection)
		{
			CurrentSection = Entry.OwningAsset;
			EmitSectionHeader(ChildBuilder, CurrentSection->GetName());
		}

		TSharedPtr<IPropertyHandle> EntryHandle = OverridesArrayHandle->GetChildHandle(static_cast<uint32>(ImportIndex));
		if (EntryHandle.IsValid())
		{
			IDetailPropertyRow& Row = ChildBuilder.AddProperty(EntryHandle.ToSharedRef());
			Row.ShowPropertyButtons(false);
		}
		++ImportIndex;
	}
}

void FPCGExPropertySchemaCollectionCustomization::SubscribeToImportedAssets(const TArray<FPCGExPropertyResolved>& Resolved)
{
	UnsubscribeImportedAssets();

	TSet<UPCGExPropertySchemaAsset*> Unique;
	for (const FPCGExPropertyResolved& Entry : Resolved)
	{
		if (Entry.OwningAsset) { Unique.Add(Entry.OwningAsset); }
	}

	for (UPCGExPropertySchemaAsset* Asset : Unique)
	{
		FDelegateHandle Handle = Asset->OnSchemaAssetChanged.AddSP(
			this, &FPCGExPropertySchemaCollectionCustomization::OnImportedAssetChanged);
		AssetDelegateHandles.Emplace(TWeakObjectPtr<UPCGExPropertySchemaAsset>(Asset), Handle);
	}
}

void FPCGExPropertySchemaCollectionCustomization::UnsubscribeImportedAssets()
{
	for (TPair<TWeakObjectPtr<UPCGExPropertySchemaAsset>, FDelegateHandle>& Pair : AssetDelegateHandles)
	{
		if (UPCGExPropertySchemaAsset* Asset = Pair.Key.Get())
		{
			Asset->OnSchemaAssetChanged.Remove(Pair.Value);
		}
	}
	AssetDelegateHandles.Reset();
}

bool FPCGExPropertySchemaCollectionCustomization::TryRenderFlatInline(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> ElementHandle)
{
	// One raw-data access for the whole schema -- derive Property, Name, and type from it
	TArray<void*> ElemRaw;
	ElementHandle->AccessRawData(ElemRaw);
	if (ElemRaw.IsEmpty() || !ElemRaw[0])
	{
		return false;
	}

	FPCGExPropertySchema* Schema = static_cast<FPCGExPropertySchema*>(ElemRaw[0]);
	if (!Schema->Property.IsValid())
	{
		return false;
	}

	UScriptStruct* InnerStruct = const_cast<UScriptStruct*>(Schema->Property.GetScriptStruct());
	if (!InnerStruct || !InnerStruct->HasMetaData(TEXT("PCGExInlineValue")))
	{
		return false;
	}

	uint8* StructMemory = Schema->Property.GetMutableMemory();
	if (!StructMemory)
	{
		return false;
	}

	FString TypeName;
	if (const FPCGExProperty* Prop = Schema->GetProperty())
	{
		TypeName = Prop->GetTypeName().ToString();
	}
	const FText LabelText = FText::FromString(
		FString::Printf(TEXT("%s (%s)"), *Schema->Name.ToString(), *TypeName));

	// Non-owning scope: memory is owned by the live component instance
	TSharedPtr<FStructOnScope> Scope = MakeShared<FStructOnScope>(InnerStruct, StructMemory);
	InstanceScopes.Add(Scope);

	const TSharedRef<SWidget> NameContent = SNew(STextBlock)
		.Text(LabelText)
		.Font(IDetailLayoutBuilder::GetDetailFont());

	return FPCGExInlineWidgetRegistry::AddCompactValueRow(
		ChildBuilder, Scope.ToSharedRef(), InnerStruct, NameContent);
}

void FPCGExPropertySchemaCollectionCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	TSharedPtr<IPropertyHandle> SchemasArrayHandle = PropertyHandle->GetChildHandle(TEXT("Schemas"));
	if (!SchemasArrayHandle.IsValid())
	{
		return;
	}

	SchemasArrayHandlePtr = SchemasArrayHandle;

	if (bIsInstanceMode)
	{
		// Schema structure is locked -- only values are editable. ReadOnlySchema on the array
		// handle propagates to children so the fallback path (complex types, delegated to
		// FPCGExPropertySchemaCustomization) enters its value-only rendering automatically.
		SchemasArrayHandle->SetInstanceMetaData(FName(TEXT("ReadOnlySchema")), TEXT("true"));

		// Reset scopes from the previous layout pass. The detail panel tears down the Slate
		// subtree before calling CustomizeChildren again, so clearing here is safe.
		InstanceScopes.Reset();

		uint32 NumElements = 0;
		SchemasArrayHandle->GetNumChildren(NumElements);

		for (uint32 i = 0; i < NumElements; ++i)
		{
			TSharedPtr<IPropertyHandle> ElementHandle = SchemasArrayHandle->GetChildHandle(i);
			if (!ElementHandle.IsValid())
			{
				continue;
			}

			if (!TryRenderFlatInline(ChildBuilder, ElementHandle.ToSharedRef()))
			{
				// Complex or unknown type: delegate to FPCGExPropertySchemaCustomization,
				// which sees ReadOnlySchema on the parent handle and renders value-only.
				ChildBuilder.AddProperty(ElementHandle.ToSharedRef()).ShowPropertyButtons(false);
			}
		}
	}
	else
	{
		// Normal mode: watch for array changes and trigger sync + refresh, then display as-is.
		SchemasArrayHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FPCGExPropertySchemaCollectionCustomization::OnSchemasArrayChanged));
		SchemasArrayHandle->SetOnChildPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FPCGExPropertySchemaCollectionCustomization::OnSchemasArrayChanged));

		ChildBuilder.AddProperty(SchemasArrayHandle.ToSharedRef());
	}

	// Resolve once and share with EmitImportSections, SubscribeToImportedAssets, and the header
	// count cache. Reconcile only on shape divergence (e.g. CDO gained an import since this
	// instance was saved) -- the no-op write would otherwise dirty unedited instances on inspection.
	TArray<FPCGExPropertyResolved> Resolved;
	TArray<void*> RawData;
	PropertyHandle->AccessRawData(RawData);
	FPCGExPropertySchemaCollection* Collection = (RawData.IsEmpty() || !RawData[0])
		                                             ? nullptr
		                                             : static_cast<FPCGExPropertySchemaCollection*>(RawData[0]);
	if (Collection)
	{
		Collection->Resolve(Resolved);

		int32 ExpectedImportCount = 0;
		for (const FPCGExPropertyResolved& Entry : Resolved) { if (Entry.OwningAsset) { ++ExpectedImportCount; } }
		if (Collection->ImportOverrides.Overrides.Num() != ExpectedImportCount)
		{
			// Reconcile reallocates ImportOverrides.Overrides, so the OverrideValue pointers
			// cached in Resolved entries are now stale -- rebuild Resolved against the new state.
			Collection->ReconcileImportOverrides(Resolved);
			Collection->Resolve(Resolved);
		}

		EmitImportSections(ChildBuilder, PropertyHandle, Resolved);
	}

	if (!bIsInstanceMode)
	{
		// ImportedSchemas array editor (managing the imports list itself) -- structural,
		// not exposed in instance mode where the schema shape is locked.
		if (TSharedPtr<IPropertyHandle> ImportedSchemasHandle = PropertyHandle->GetChildHandle(TEXT("ImportedSchemas")))
		{
			ImportedSchemasHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FPCGExPropertySchemaCollectionCustomization::OnImportedSchemasArrayChanged));
			ImportedSchemasHandle->SetOnChildPropertyValueChanged(FSimpleDelegate::CreateSP(this, &FPCGExPropertySchemaCollectionCustomization::OnImportedSchemasArrayChanged));
			ChildBuilder.AddProperty(ImportedSchemasHandle.ToSharedRef());
		}
	}

	if (Collection)
	{
		SubscribeToImportedAssets(Resolved);
		CachedResolvedCount = Resolved.Num();
	}
}
