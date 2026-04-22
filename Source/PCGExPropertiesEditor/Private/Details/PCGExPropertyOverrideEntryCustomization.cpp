// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/PCGExPropertyOverrideEntryCustomization.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "PCGExProperty.h"
#include "PCGExInlineWidgetRegistry.h"
#include "UObject/StructOnScope.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IPropertyTypeCustomization> FPCGExPropertyOverrideEntryCustomization::MakeInstance()
{
	return MakeShareable(new FPCGExPropertyOverrideEntryCustomization());
}

FText FPCGExPropertyOverrideEntryCustomization::GetEntryLabelText() const
{
	if (!PropertyHandlePtr.IsValid()) { return FText::FromString(TEXT("None (Unknown)")); }

	// Access entry data directly - THIS RUNS EACH FRAME, reads fresh data after sync
	TArray<void*> RawData;
	PropertyHandlePtr.Pin()->AccessRawData(RawData);

	FName PropertyName = NAME_None;
	FString TypeName = TEXT("Unknown");

	if (!RawData.IsEmpty() && RawData[0])
	{
		const FPCGExPropertyOverrideEntry* Entry = static_cast<FPCGExPropertyOverrideEntry*>(RawData[0]);
		if (Entry && Entry->Value.IsValid())
		{
			if (const FPCGExProperty* Prop = Entry->Value.GetPtr<FPCGExProperty>())
			{
				PropertyName = Prop->PropertyName;
				TypeName = Prop->GetTypeName().ToString();
			}
		}
	}

	return FText::FromString(FString::Printf(TEXT("%s (%s)"), *PropertyName.ToString(), *TypeName));
}

void FPCGExPropertyOverrideEntryCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Capture the entry + its well-known child handles as members. Every lambda / widget
	// created below (and in CustomizeChildren) reads through these members, so their
	// backing nodes stay reachable for the full lifetime of this customization.
	PropertyHandlePtr = PropertyHandle;
	ValueHandlePtr = PropertyHandle->GetChildHandle(TEXT("Value"));
	EnabledHandlePtr = PropertyHandle->GetChildHandle(TEXT("bEnabled"));

	// Check if this is an inline type (schema-driven, stable for the detail session)
	bool bShouldInline = false;
	if (ValueHandlePtr.IsValid())
	{
		TArray<void*> RawData;
		ValueHandlePtr->AccessRawData(RawData);
		if (!RawData.IsEmpty() && RawData[0])
		{
			FInstancedStruct* Instance = static_cast<FInstancedStruct*>(RawData[0]);
			if (Instance && Instance->IsValid())
			{
				UScriptStruct* InnerStruct = const_cast<UScriptStruct*>(Instance->GetScriptStruct());
				if (InnerStruct)
				{
					bShouldInline = InnerStruct->HasMetaData(TEXT("PCGExInlineValue"));
				}
			}
		}
	}

	// For complex (non-inline) types, show checkbox + label in header
	// For simple (inline) types, header stays empty (everything in CustomizeChildren)
	if (!bShouldInline)
	{
		HeaderRow
			.NameContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 4, 0)
				[
					EnabledHandlePtr.IsValid() ? EnabledHandlePtr->CreatePropertyValueWidget() : SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertyOverrideEntryCustomization::GetEntryLabelText)))
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
			];
	}
}

void FPCGExPropertyOverrideEntryCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// CustomizeHeader is always called first and populates the member handles,
	// but recover gracefully if we are somehow entered cold.
	if (!ValueHandlePtr.IsValid())
	{
		ValueHandlePtr = PropertyHandle->GetChildHandle(TEXT("Value"));
	}
	if (!EnabledHandlePtr.IsValid())
	{
		EnabledHandlePtr = PropertyHandle->GetChildHandle(TEXT("bEnabled"));
	}

	if (!ValueHandlePtr.IsValid()) { return; }

	// Access raw data to get the FInstancedStruct
	TArray<void*> RawData;
	ValueHandlePtr->AccessRawData(RawData);
	if (RawData.IsEmpty() || !RawData[0]) { return; }

	FInstancedStruct* Instance = static_cast<FInstancedStruct*>(RawData[0]);
	if (!Instance || !Instance->IsValid()) { return; }

	UScriptStruct* InnerStruct = const_cast<UScriptStruct*>(Instance->GetScriptStruct());
	if (!InnerStruct) { return; }

	uint8* StructMemory = Instance->GetMutableMemory();
	if (!StructMemory) { return; }

	// Check if this type should be inlined
	const bool bShouldInline = InnerStruct->HasMetaData(TEXT("PCGExInlineValue"));

	// Hold the inner scope on the customization so it is guaranteed to outlive every
	// widget / lambda that will reference it below. The detail panel tears down the
	// Slate subtree before releasing its customization instances, so this member
	// chain (customization -> InnerScope -> StructMemory) gives a structural, not
	// probabilistic, lifetime guarantee.
	// Non-owning ctor is correct: StructMemory is owned by the outer FStructOnScope
	// the grid view passes into SetStructureData, which the detail panel keeps alive
	// for the session, and the inner type is pinned by the collection schema.
	InnerScope = MakeShared<FStructOnScope>(InnerStruct, StructMemory);

	// Enabled attribute: capture the member handle by TWeakPtr so the lambda can never
	// keep a stale shared-ref alive after the customization is torn down. While this
	// customization is alive, EnabledHandlePtr is alive, and the weak pin succeeds.
	TWeakPtr<IPropertyHandle> WeakEnabledHandle = EnabledHandlePtr;
	TAttribute<bool> IsEnabledAttr = TAttribute<bool>::Create([WeakEnabledHandle]()
	{
		if (TSharedPtr<IPropertyHandle> Handle = WeakEnabledHandle.Pin())
		{
			bool bEnabled = false;
			Handle->GetValue(bEnabled);
			return bEnabled;
		}
		return true;
	});

	// Only customize if this is a simple type that should be inlined
	if (bShouldInline)
	{
		// For inline types, only show the "Value" property
		if (const FProperty* ValueProperty = InnerStruct->FindPropertyByName(TEXT("Value")))
		{
			IDetailPropertyRow& Row = *ChildBuilder.AddExternalStructureProperty(InnerScope.ToSharedRef(), ValueProperty->GetFName());

			// Get the property handle for the value widget
			TSharedPtr<IPropertyHandle> ValuePropertyHandle = Row.GetPropertyHandle();

			// Query the inline widget registry - if a factory is registered for this outer
			// struct type, use it instead of the default property value widget. The default
			// path (CreatePropertyValueWidget) works for simple scalar types but falls back
			// to expandable widgets for complex types (FVector, FRotator, etc.), which
			// break the inline contract - custom factories exist to provide compact renders.
			const FPCGExMakeInlineWidgetFn* Factory = FPCGExInlineWidgetRegistry::Find(InnerStruct->GetFName());
			TSharedRef<SWidget> ValueWidget = SNullWidget::NullWidget;
			if (ValuePropertyHandle.IsValid())
			{
				ValueWidget = Factory
					              ? (*Factory)(ValuePropertyHandle.ToSharedRef())
					              : ValuePropertyHandle->CreatePropertyValueWidget();
			}

			// Customize the row to show checkbox + label in NameContent and value widget in ValueContent.
			// When a factory widget is used (multi-field compact editor), widen the value column
			// so all sub-fields fit and fill the available horizontal space uniformly.
			const bool bHasCustomFactory = Factory != nullptr;

			Row.CustomWidget()
			   .NameContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0, 0, 4, 0)
					[
						EnabledHandlePtr.IsValid() ? EnabledHandlePtr->CreatePropertyValueWidget() : SNullWidget::NullWidget
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertyOverrideEntryCustomization::GetEntryLabelText)))
						.Font(IDetailLayoutBuilder::GetDetailFont())
					]
				]
				.ValueContent()
				.MinDesiredWidth(bHasCustomFactory ? 250.0f : 125.0f)
				.MaxDesiredWidth(bHasCustomFactory ? 3000.0f : 600.0f)
				[
					SNew(SBox)
					.IsEnabled(IsEnabledAttr)
					[
						ValueWidget
					]
				];
		}
	}
	else
	{
		// Complex type - iterate all non-metadata properties and add them as children
		for (TFieldIterator<FProperty> It(InnerStruct); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property) { continue; }

			FName PropName = Property->GetFName();

			// Skip metadata properties
			if (PropName == TEXT("PropertyName") || PropName == TEXT("HeaderId") || PropName == TEXT("OutputBuffer"))
			{
				continue;
			}

			// Add each property as a child row with enabled state
			IDetailPropertyRow& PropRow = *ChildBuilder.AddExternalStructureProperty(InnerScope.ToSharedRef(), PropName);
			PropRow.IsEnabled(IsEnabledAttr);
		}
	}
}
