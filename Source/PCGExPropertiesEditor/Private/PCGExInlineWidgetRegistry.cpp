// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExInlineWidgetRegistry.h"

namespace PCGExInlineWidgetRegistry_Private
{
	static TMap<FName, FPCGExMakeInlineWidgetFn>& GetMap()
	{
		static TMap<FName, FPCGExMakeInlineWidgetFn> Map;
		return Map;
	}
}

void FPCGExInlineWidgetRegistry::Register(FName StructName, FPCGExMakeInlineWidgetFn Factory)
{
	if (StructName.IsNone() || !Factory) { return; }
	PCGExInlineWidgetRegistry_Private::GetMap().Add(StructName, MoveTemp(Factory));
}

void FPCGExInlineWidgetRegistry::Unregister(FName StructName)
{
	PCGExInlineWidgetRegistry_Private::GetMap().Remove(StructName);
}

const FPCGExMakeInlineWidgetFn* FPCGExInlineWidgetRegistry::Find(FName StructName)
{
	return PCGExInlineWidgetRegistry_Private::GetMap().Find(StructName);
}

void FPCGExInlineWidgetRegistry::Clear()
{
	PCGExInlineWidgetRegistry_Private::GetMap().Empty();
}
