// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExBuiltInInlineWidgets.h"

#include "PCGExInlineWidgetRegistry.h"
#include "PCGExPropertyTypes.h"
#include "Details/PCGExInlineNumericWidgets.h"

namespace PCGExBuiltInInlineWidgets
{
	void RegisterAll()
	{
		FPCGExInlineWidgetRegistry::Register(FPCGExProperty_Vector::StaticStruct()->GetFName(), &PCGExInlineNumericWidgets::MakeVectorWidget);
		FPCGExInlineWidgetRegistry::Register(FPCGExProperty_Vector2::StaticStruct()->GetFName(), &PCGExInlineNumericWidgets::MakeVector2Widget);
		FPCGExInlineWidgetRegistry::Register(FPCGExProperty_Rotator::StaticStruct()->GetFName(), &PCGExInlineNumericWidgets::MakeRotatorWidget);
	}
}
