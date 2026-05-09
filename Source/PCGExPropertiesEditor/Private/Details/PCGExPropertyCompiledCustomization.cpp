// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/PCGExPropertyCompiledCustomization.h"

#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailPropertyRow.h"
#include "PropertyHandle.h"
#include "PCGExInlineWidgetRegistry.h"
#include "PCGExProperty.h"
#include "Widgets/Layout/SBox.h"

TSharedRef<IPropertyTypeCustomization> FPCGExPropertyCompiledCustomization::MakeInstance()
{
	return MakeShareable(new FPCGExPropertyCompiledCustomization());
}

void FPCGExPropertyCompiledCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Collapse the inner group header that FInstancedStructDetails would otherwise inject
	// between the type-picker row and the Value field. The schema header already shows
	// the property name and type, so this wrapper is pure noise.
	HeaderRow.Visibility(EVisibility::Collapsed);
}

void FPCGExPropertyCompiledCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Try to get Value handle directly first
	TSharedPtr<IPropertyHandle> ValueHandle = PropertyHandle->GetChildHandle(TEXT("Value"));
	if (ValueHandle.IsValid())
	{
		// If a compact inline widget is registered for this outer property struct, use it
		// instead of the default expandable struct widget. Keeps the Compiled (shorthand)
		// view visually consistent with the override entry view for the same types.
		FName OuterStructName = NAME_None;
		if (const FStructProperty* OuterStructProp = CastField<FStructProperty>(PropertyHandle->GetProperty()))
		{
			if (OuterStructProp->Struct) { OuterStructName = OuterStructProp->Struct->GetFName(); }
		}

		// Edit-mode lookup: this customization is only invoked from the schema-edit path
		// (FInstancedStruct expansion inside FPCGExPropertySchemaCustomization's non-readonly
		// branch). Property *definition* controls (e.g. enum class picker) belong here.
		if (const FPCGExMakeInlineWidgetFn* Factory = FPCGExInlineWidgetRegistry::Find(OuterStructName, EPCGExInlineWidgetMode::Edit))
		{
			IDetailPropertyRow& Row = ChildBuilder.AddProperty(ValueHandle.ToSharedRef());
			Row.CustomWidget(/*bShowChildren=*/false)
			   .NameContent()
				[
					ValueHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(250.0f)
				.MaxDesiredWidth(3000.0f)
				[
					(*Factory)(ValueHandle.ToSharedRef())
				];
			return;
		}

		ChildBuilder.AddProperty(ValueHandle.ToSharedRef());
		return;
	}

	// Fallback: Show all children except PropertyName
	uint32 NumChildren = 0;
	PropertyHandle->GetNumChildren(NumChildren);

	for (uint32 i = 0; i < NumChildren; ++i)
	{
		TSharedPtr<IPropertyHandle> ChildHandle = PropertyHandle->GetChildHandle(i);
		if (!ChildHandle.IsValid()) { continue; }

		const FName ChildName = ChildHandle->GetProperty() ? ChildHandle->GetProperty()->GetFName() : NAME_None;

		// Skip PropertyName - it's shown in the outer schema header
		if (ChildName == TEXT("PropertyName")) { continue; }

		ChildBuilder.AddProperty(ChildHandle.ToSharedRef());
	}
}

