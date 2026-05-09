// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExInlineWidgetRegistry.h"

namespace PCGExInlineWidgetRegistry_Private
{
	struct FKey
	{
		FName StructName;
		EPCGExInlineWidgetMode Mode;

		friend bool operator==(const FKey& A, const FKey& B) { return A.StructName == B.StructName && A.Mode == B.Mode; }
		friend uint32 GetTypeHash(const FKey& K) { return HashCombineFast(GetTypeHash(K.StructName), GetTypeHash(static_cast<uint8>(K.Mode))); }
	};

	static TMap<FKey, FPCGExMakeInlineWidgetFn>& GetMap()
	{
		static TMap<FKey, FPCGExMakeInlineWidgetFn> Map;
		return Map;
	}
}

void FPCGExInlineWidgetRegistry::Register(FName StructName, EPCGExInlineWidgetMode Mode, FPCGExMakeInlineWidgetFn Factory)
{
	if (StructName.IsNone() || !Factory) { return; }
	PCGExInlineWidgetRegistry_Private::GetMap().Add({StructName, Mode}, MoveTemp(Factory));
}

void FPCGExInlineWidgetRegistry::RegisterAllModes(FName StructName, FPCGExMakeInlineWidgetFn Factory)
{
	if (StructName.IsNone() || !Factory) { return; }
	PCGExInlineWidgetRegistry_Private::GetMap().Add({StructName, EPCGExInlineWidgetMode::Edit}, Factory);
	PCGExInlineWidgetRegistry_Private::GetMap().Add({StructName, EPCGExInlineWidgetMode::Compact}, MoveTemp(Factory));
}

void FPCGExInlineWidgetRegistry::Unregister(FName StructName, EPCGExInlineWidgetMode Mode)
{
	PCGExInlineWidgetRegistry_Private::GetMap().Remove({StructName, Mode});
}

void FPCGExInlineWidgetRegistry::UnregisterAllModes(FName StructName)
{
	PCGExInlineWidgetRegistry_Private::GetMap().Remove({StructName, EPCGExInlineWidgetMode::Edit});
	PCGExInlineWidgetRegistry_Private::GetMap().Remove({StructName, EPCGExInlineWidgetMode::Compact});
}

const FPCGExMakeInlineWidgetFn* FPCGExInlineWidgetRegistry::Find(FName StructName, EPCGExInlineWidgetMode Mode)
{
	return PCGExInlineWidgetRegistry_Private::GetMap().Find({StructName, Mode});
}

void FPCGExInlineWidgetRegistry::Clear()
{
	PCGExInlineWidgetRegistry_Private::GetMap().Empty();
}
