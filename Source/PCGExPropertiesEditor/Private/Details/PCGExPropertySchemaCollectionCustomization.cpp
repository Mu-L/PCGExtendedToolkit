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
#include "PropertyHandle.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IPropertyTypeCustomization> FPCGExPropertySchemaCollectionCustomization::MakeInstance()
{
	return MakeShareable(new FPCGExPropertySchemaCollectionCustomization());
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
	if (!PropertyHandlePtr.IsValid())
	{
		return FText::FromString(TEXT("0 properties"));
	}

	// Get raw access to count schemas
	TArray<void*> RawData;
	PropertyHandlePtr.Pin()->AccessRawData(RawData);
	int32 SchemaCount = 0;
	if (!RawData.IsEmpty() && RawData[0])
	{
		const FPCGExPropertySchemaCollection* Collection = static_cast<FPCGExPropertySchemaCollection*>(RawData[0]);
		SchemaCount = Collection->Schemas.Num();
	}

	return FText::FromString(FString::Printf(TEXT("%d %s"), SchemaCount, SchemaCount == 1 ? TEXT("property") : TEXT("properties")));
}

void FPCGExPropertySchemaCollectionCustomization::OnSchemasArrayChanged()
{
	if (!PropertyHandlePtr.IsValid())
	{
		return;
	}

	// Access the collection to sync schemas
	TArray<void*> RawData;
	PropertyHandlePtr.Pin()->AccessRawData(RawData);
	if (RawData.IsEmpty() || !RawData[0])
	{
		return;
	}

	FPCGExPropertySchemaCollection* Collection = static_cast<FPCGExPropertySchemaCollection*>(RawData[0]);

	// Sync PropertyName and HeaderId for all schemas
	for (FPCGExPropertySchema& Schema : Collection->Schemas)
	{
		Schema.SyncPropertyName();
	}

	if (TSharedPtr<IPropertyUtilities> PropertyUtilities = WeakPropertyUtilities.Pin())
	{
		PropertyUtilities->ForceRefresh();
	}
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

		// Imports are structural -- excluded from instance mode where the schema is locked.
		if (TSharedPtr<IPropertyHandle> ImportedSchemasHandle = PropertyHandle->GetChildHandle(TEXT("ImportedSchemas")))
		{
			ChildBuilder.AddProperty(ImportedSchemasHandle.ToSharedRef());
		}
	}
}
