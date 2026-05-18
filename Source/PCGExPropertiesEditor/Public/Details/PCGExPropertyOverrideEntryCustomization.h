// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "IDetailPropertyRow.h"
#include "IPropertyTypeCustomization.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FStructOnScope;
class IPropertyUtilities;
class UPCGExPropertyCollectionComponent;


/**
 * Customizes FPCGExPropertyOverrideEntry to show PropertyName label + Value widget.
 * Uses dynamic text for PropertyName to update when schema changes.
 *
 * Lifetime-critical state (InnerScope, ValueHandlePtr, EnabledHandlePtr) is held as
 * members so every widget / lambda created in CustomizeHeader / CustomizeChildren is
 * guaranteed to outlive its backing data by being transitively owned by this
 * customization instance. The detail panel releases customizations only after its
 * Slate subtree is torn down, which makes the lifetime coupling structural rather
 * than relying on ref-count ordering between the detail tree and child widgets.
 *
 * InnerScope is intentionally non-owning (aliases FInstancedStruct::GetMutableMemory)
 * so edits flow directly into the outer struct scope the detail panel already owns.
 * The inner type is pinned by the collection schema for the lifetime of the detail
 * session, so the aliased pointer is stable.
 */
class FPCGExPropertyOverrideEntryCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		class FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> PropertyHandle,
		class IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	const struct FPCGExProperty* AccessEntryProperty() const;
	FText GetEntryNameText() const;
	FText GetEntryTypeText() const;

	TWeakPtr<IPropertyHandle> PropertyHandlePtr;
	TSharedPtr<IPropertyHandle> ValueHandlePtr;
	TSharedPtr<IPropertyHandle> EnabledHandlePtr;
	TSharedPtr<FStructOnScope> InnerScope;

	// Captured at CustomizeHeader -- inline-path external-structure rows return nothing from
	// Handle->GetOuterObjects, so the reset delegates can't re-derive the component. Null when
	// the entry isn't owned by a UPCGExPropertyCollectionComponent or is Instance-created (no
	// BP chain to walk); UE's default reset behavior applies in those cases.
	TWeakObjectPtr<UPCGExPropertyCollectionComponent> WeakLiveComponent;
	int32 CachedOverrideIndex = INDEX_NONE;

	// Asset-default resolved once at CustomizeHeader (imports tree is stable for the detail
	// session). Invalid = "no asset default exists" -- TryGetResetSource relies on the distinction.
	FInstancedStruct CachedAssetDefaultValue;

	// Defensive-only: a type-mismatch reset reallocates and would invalidate InnerScope's alias.
	// Same-type copy preserves the alias and needs no refresh, which is the common case under
	// ReconcileImportOverrides.
	TWeakPtr<IPropertyUtilities> WeakPropertyUtilities;

	FResetToDefaultOverride MakeArchetypeResetOverride() const;
};
