// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/PCGExPropertyOutputConfigCustomization.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "PCGExPropertyWriter.h"
#include "Styling/SlateColor.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"

TSharedRef<IPropertyTypeCustomization> FPCGExPropertyOutputConfigCustomization::MakeInstance()
{
	return MakeShareable(new FPCGExPropertyOutputConfigCustomization());
}

void FPCGExPropertyOutputConfigCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	TSharedPtr<IPropertyHandle> EnabledHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FPCGExPropertyOutputConfig, bEnabled));
	TSharedPtr<IPropertyHandle> PropertyNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FPCGExPropertyOutputConfig, PropertyName));
	TSharedPtr<IPropertyHandle> OutputAttributeNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FPCGExPropertyOutputConfig, OutputAttributeName));

	auto IsEnabled = [EnabledHandle]()
	{
		bool bEnabled = false;
		if (EnabledHandle) { EnabledHandle->GetValue(bEnabled); }
		return bEnabled;
	};

	HeaderRow.NameContent()
	[
		SNew(SBox)
		.MinDesiredWidth(24)
		.MaxDesiredWidth(24)
		[
			EnabledHandle->CreatePropertyValueWidget()
		]
	];

	HeaderRow.ValueContent()
	         .MinDesiredWidth(320)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().Padding(1).FillWidth(1)
		[
			SNew(SBox)
			.IsEnabled_Lambda(IsEnabled)
			[
				PropertyNameHandle->CreatePropertyValueWidget()
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("→")))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
		]
		+ SHorizontalBox::Slot().Padding(1).FillWidth(1)
		[
			SNew(SBox)
			.IsEnabled_Lambda(IsEnabled)
			[
				OutputAttributeNameHandle->CreatePropertyValueWidget()
			]
		]
	];
}

void FPCGExPropertyOutputConfigCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
}
