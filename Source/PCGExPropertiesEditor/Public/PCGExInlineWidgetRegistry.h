// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class IPropertyHandle;
class SWidget;

/**
 * Factory signature for an inline widget.
 *
 * ValueHandle is the IPropertyHandle of the FInstancedStruct's inner struct's "Value" field
 * (equivalent of what AddProperty(ValueHandle) would render in the default inline path).
 * The factory is expected to return a single-row compact widget that fits into the
 * override entry's ValueContent slot (no expandable children).
 */
using FPCGExMakeInlineWidgetFn = TFunction<TSharedRef<SWidget>(const TSharedRef<IPropertyHandle>& /*ValueHandle*/)>;

/**
 * Registry of custom inline widget factories keyed by the FName of the outer
 * FPCGExProperty subclass UScriptStruct (e.g. "PCGExProperty_Vector").
 *
 * Only queried when the outer property type has the PCGExInlineValue meta tag.
 * When a factory is found, it is used in place of the default AddProperty(ValueHandle)
 * behavior; otherwise the existing default is preserved.
 *
 * External editor modules can register their own factories during StartupModule().
 */
class PCGEXPROPERTIESEDITOR_API FPCGExInlineWidgetRegistry
{
public:
	static void Register(FName StructName, FPCGExMakeInlineWidgetFn Factory);
	static void Unregister(FName StructName);
	static const FPCGExMakeInlineWidgetFn* Find(FName StructName);
	static void Clear();
};
