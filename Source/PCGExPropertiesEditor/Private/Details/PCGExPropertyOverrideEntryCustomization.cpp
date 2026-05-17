// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/PCGExPropertyOverrideEntryCustomization.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IPropertyUtilities.h"
#include "PCGExInlineWidgetRegistry.h"
#include "PCGExProperty.h"
#include "PCGExPropertyType_Struct.h"
#include "PropertyHandle.h"
#include "StructUtilsDelegates.h"
#include "Details/PCGExPropertyLabelRow.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/StructOnScope.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IPropertyTypeCustomization> FPCGExPropertyOverrideEntryCustomization::MakeInstance()
{
	return MakeShareable(new FPCGExPropertyOverrideEntryCustomization());
}

FPCGExPropertyOverrideEntryCustomization::~FPCGExPropertyOverrideEntryCustomization()
{
	if (UserDefinedStructReinstancedHandle.IsValid())
	{
		UE::StructUtils::Delegates::OnUserDefinedStructReinstanced.Remove(UserDefinedStructReinstancedHandle);
	}
}

void FPCGExPropertyOverrideEntryCustomization::OnUserDefinedStructReinstanced(const UUserDefinedStruct& Struct)
{
	// UDS reinstance: UScriptStruct* survives but GetStructureSize() may change, leaving the
	// FInstancedStruct allocation too small. Reinit drops payload but prevents OOB on next edit.
	if (!ValueHandlePtr.IsValid())
	{
		return;
	}

	TArray<void*> RawData;
	ValueHandlePtr->AccessRawData(RawData);
	if (RawData.IsEmpty() || !RawData[0])
	{
		return;
	}

	FInstancedStruct* OuterInstance = static_cast<FInstancedStruct*>(RawData[0]);
	if (!OuterInstance->IsValid())
	{
		return;
	}
	if (OuterInstance->GetScriptStruct() != FPCGExProperty_Struct::StaticStruct())
	{
		return;
	}

	FPCGExProperty_Struct* StructProp = reinterpret_cast<FPCGExProperty_Struct*>(OuterInstance->GetMutableMemory());
	if (StructProp->Value.GetScriptStruct() != &Struct)
	{
		return;
	}

	StructProp->Value.InitializeAs(&Struct);

	// Deferred refresh avoids tearing down the widget tree mid-event-handler stack.
	if (TSharedPtr<IPropertyUtilities> Util = WeakPropertyUtilities.Pin())
	{
		Util->RequestRefresh();
	}
}

const FPCGExProperty* FPCGExPropertyOverrideEntryCustomization::AccessEntryProperty() const
{
	if (!PropertyHandlePtr.IsValid())
	{
		return nullptr;
	}
	TArray<void*> RawData;
	PropertyHandlePtr.Pin()->AccessRawData(RawData);
	if (RawData.IsEmpty() || !RawData[0])
	{
		return nullptr;
	}
	const FPCGExPropertyOverrideEntry* Entry = static_cast<FPCGExPropertyOverrideEntry*>(RawData[0]);
	if (!Entry || !Entry->Value.IsValid())
	{
		return nullptr;
	}
	return Entry->Value.GetPtr<FPCGExProperty>();
}

FText FPCGExPropertyOverrideEntryCustomization::GetEntryNameText() const
{
	const FPCGExProperty* Prop = AccessEntryProperty();
	return Prop ? FText::FromName(Prop->PropertyName) : FText::FromString(TEXT("None"));
}

FText FPCGExPropertyOverrideEntryCustomization::GetEntryTypeText() const
{
	const FPCGExProperty* Prop = AccessEntryProperty();
	return Prop ? FText::FromName(Prop->GetDisplayTypeName()) : FText::FromString(TEXT("Unknown"));
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

	WeakPropertyUtilities = CustomizationUtils.GetPropertyUtilities();
	UserDefinedStructReinstancedHandle = UE::StructUtils::Delegates::OnUserDefinedStructReinstanced.AddSP(
		this, &FPCGExPropertyOverrideEntryCustomization::OnUserDefinedStructReinstanced);

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
					PCGExPropertyLabelRow::Build(
						TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertyOverrideEntryCustomization::GetEntryNameText)),
						TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertyOverrideEntryCustomization::GetEntryTypeText)))
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

	if (!ValueHandlePtr.IsValid())
	{
		return;
	}

	// Access raw data to get the FInstancedStruct
	TArray<void*> RawData;
	ValueHandlePtr->AccessRawData(RawData);
	if (RawData.IsEmpty() || !RawData[0])
	{
		return;
	}

	FInstancedStruct* Instance = static_cast<FInstancedStruct*>(RawData[0]);
	if (!Instance || !Instance->IsValid())
	{
		return;
	}

	UScriptStruct* InnerStruct = const_cast<UScriptStruct*>(Instance->GetScriptStruct());
	if (!InnerStruct)
	{
		return;
	}

	uint8* StructMemory = Instance->GetMutableMemory();
	if (!StructMemory)
	{
		return;
	}

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

	if (bShouldInline)
	{
		const TSharedRef<SWidget> NameContent = SNew(SHorizontalBox)
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
				PCGExPropertyLabelRow::Build(
					TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertyOverrideEntryCustomization::GetEntryNameText)),
					TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &FPCGExPropertyOverrideEntryCustomization::GetEntryTypeText)),
					/*bShowSeparator=*/false)
			];

		FPCGExInlineWidgetRegistry::AddCompactValueRow(ChildBuilder, InnerScope.ToSharedRef(), InnerStruct, NameContent, IsEnabledAttr);
	}
	else if (InnerStruct == FPCGExProperty_Struct::StaticStruct())
	{
		// Bypass the FInstancedStruct wrapper row: walk Value's inner struct as siblings of the
		// override header. Type is schema-pinned in override context so no combo is needed.
		FPCGExProperty_Struct* StructProp = reinterpret_cast<FPCGExProperty_Struct*>(StructMemory);
		const UScriptStruct* InnerInnerStruct = StructProp->Value.GetScriptStruct();
		uint8* InnerInnerMemory = StructProp->Value.GetMutableMemory();
		if (InnerInnerStruct && InnerInnerMemory)
		{
			NestedScope = MakeShared<FStructOnScope>(InnerInnerStruct, InnerInnerMemory);
			for (TFieldIterator<FProperty> It(InnerInnerStruct); It; ++It)
			{
				// CPF_Edit filter: AddExternalStructureProperty returns null on non-editable
				// fields and the dereference would crash. Arbitrary user structs may include
				// VisibleAnywhere/no-spec fields, unlike the FPCGExProperty_* set.
				if (!It->HasAnyPropertyFlags(CPF_Edit))
				{
					continue;
				}
				if (IDetailPropertyRow* RowPtr = ChildBuilder.AddExternalStructureProperty(NestedScope.ToSharedRef(), It->GetFName()))
				{
					RowPtr->IsEnabled(IsEnabledAttr);
				}
			}
		}
		else
		{
			// No struct type picked yet (or override is stale) -- show the default FInstancedStruct
			// row so the user can pick a type from the combo.
			FPCGExInlineWidgetRegistry::AddComplexValueRows(ChildBuilder, InnerScope.ToSharedRef(), InnerStruct, IsEnabledAttr);
		}
	}
	else
	{
		FPCGExInlineWidgetRegistry::AddComplexValueRows(ChildBuilder, InnerScope.ToSharedRef(), InnerStruct, IsEnabledAttr);
	}
}
