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
 * Distinguishes the two contexts in which inline widgets are rendered.
 *
 * Some property types (e.g. enum) need different UI affordances depending on whether the
 * user is *defining* the property (Edit) or *overriding/displaying* a previously-defined
 * one (Compact). For example, an enum schema editor needs to expose the enum class picker;
 * an enum override only needs to pick a value within the already-pinned class.
 *
 * For property types where the same widget works in both contexts (Vector, Vector2,
 * Rotator), register the same factory under both modes.
 */
enum class EPCGExInlineWidgetMode : uint8
{
	/** Schema-edit context: full editor including any "definition" controls (e.g. enum class picker). */
	Edit,
	/** Override / readonly-schema context: compact value-only editor. */
	Compact,
};

/**
 * Registry of custom inline widget factories keyed by the FName of the outer
 * FPCGExProperty subclass UScriptStruct (e.g. "PCGExProperty_Vector") and a render mode.
 *
 * When a factory is found, it is used in place of the default AddProperty(ValueHandle)
 * behavior; otherwise the existing default is preserved.
 *
 * External editor modules can register their own factories during StartupModule().
 */
class PCGEXPROPERTIESEDITOR_API FPCGExInlineWidgetRegistry
{
public:
	/** Register a factory under a single mode. */
	static void Register(FName StructName, EPCGExInlineWidgetMode Mode, FPCGExMakeInlineWidgetFn Factory);

	/** Convenience: register the same factory under both Edit and Compact modes. */
	static void RegisterAllModes(FName StructName, FPCGExMakeInlineWidgetFn Factory);

	static void Unregister(FName StructName, EPCGExInlineWidgetMode Mode);
	static void UnregisterAllModes(FName StructName);

	static const FPCGExMakeInlineWidgetFn* Find(FName StructName, EPCGExInlineWidgetMode Mode);

	static void Clear();
};
