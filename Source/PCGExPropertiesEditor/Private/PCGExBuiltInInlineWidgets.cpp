// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExBuiltInInlineWidgets.h"

#include "PCGExInlineWidgetRegistry.h"
#include "PCGExPropertyTypes.h"
#include "Details/PCGExEnumSelectorWidget.h"
#include "Details/PCGExInlineNumericWidgets.h"

namespace PCGExBuiltInInlineWidgets
{
	void RegisterAll()
	{
		// Numeric inline widgets render identically in Edit and Compact contexts -- no
		// "definition" controls (the value IS the definition), so register under both.
		FPCGExInlineWidgetRegistry::RegisterAllModes(FPCGExProperty_Vector::StaticStruct()->GetFName(), &PCGExInlineNumericWidgets::MakeVectorWidget);
		FPCGExInlineWidgetRegistry::RegisterAllModes(FPCGExProperty_Vector2::StaticStruct()->GetFName(), &PCGExInlineNumericWidgets::MakeVector2Widget);
		FPCGExInlineWidgetRegistry::RegisterAllModes(FPCGExProperty_Rotator::StaticStruct()->GetFName(), &PCGExInlineNumericWidgets::MakeRotatorWidget);

		// Enum: compact-mode only. The schema-edit (Edit) path falls through to the default
		// AddProperty(ValueHandle), which routes to FPCGExEnumSelectorCustomization -- that
		// renders the full editor (class picker + value picker). The compact mode used in
		// override / readonly-schema contexts hides the class picker.
		FPCGExInlineWidgetRegistry::Register(
			FPCGExProperty_Enum::StaticStruct()->GetFName(),
			EPCGExInlineWidgetMode::Compact,
			[](const TSharedRef<IPropertyHandle>& ValueHandle) -> TSharedRef<SWidget>
			{
				return PCGExEnumSelectorWidget::Make(ValueHandle, /*bAllowClassPicker=*/false);
			});
	}
}
