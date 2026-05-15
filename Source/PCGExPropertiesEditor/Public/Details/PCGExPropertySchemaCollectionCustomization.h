// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "IPropertyTypeCustomization.h"
#include "UObject/StructOnScope.h"

class IPropertyUtilities;

/**
 * Customizes FPCGExPropertySchemaCollection to:
 * - Show dynamic header with schema count
 * - Trigger refresh when schemas change (add/remove/reorder/type change)
 * - Sync PropertyName and HeaderId when array changes
 *
 * Instance mode (UPCGExPropertyCollectionComponent on a non-template actor):
 * - Schema structure is locked (no add/remove/reorder)
 * - Each entry shows name as a read-only label; only the value is editable
 * - Edit the Blueprint to change the schema definition
 */
class FPCGExPropertySchemaCollectionCustomization : public IPropertyTypeCustomization
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
	FText GetHeaderText() const;

	/** Called when Schemas array changes - syncs PropertyNames and broadcasts refresh */
	void OnSchemasArrayChanged();

	/**
	 * Render a single schema element as a flat single-row inline value editor.
	 * Returns false if the schema isn't a PCGExInlineValue type or any precondition fails,
	 * letting the caller fall back to FPCGExPropertySchemaCustomization's read-only rendering.
	 */
	bool TryRenderFlatInline(class IDetailChildrenBuilder& ChildBuilder, TSharedRef<IPropertyHandle> ElementHandle);

	TWeakPtr<IPropertyUtilities> WeakPropertyUtilities;
	TWeakPtr<IPropertyHandle> PropertyHandlePtr;
	TWeakPtr<IPropertyHandle> SchemasArrayHandlePtr;

	/** True when the outer object is a non-template UPCGExPropertyCollectionComponent instance */
	bool bIsInstanceMode = false;

	/**
	 * Struct scopes for inline flat rows in instance mode.
	 * Each FStructOnScope wraps the inner property struct memory (non-owning) and must outlive
	 * the widgets that reference it. Populated in CustomizeChildren; reset each rebuild.
	 */
	TArray<TSharedPtr<FStructOnScope>> InstanceScopes;
};
