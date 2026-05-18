// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "IPropertyTypeCustomization.h"
#include "UObject/StructOnScope.h"

class IPropertyUtilities;
class UPCGExPropertySchemaAsset;
struct FPCGExPropertyResolved;
struct FPCGExPropertySchemaCollection;

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

	virtual ~FPCGExPropertySchemaCollectionCustomization() override;

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

	/** Called when Schemas array changes - syncs PropertyNames, reconciles imports, broadcasts refresh */
	void OnSchemasArrayChanged();

	/** Called when ImportedSchemas array changes -- imports tree shape changed, reconcile + refresh */
	void OnImportedSchemasArrayChanged();

	/** Called when a referenced UPCGExPropertySchemaAsset broadcasts OnSchemaAssetChanged */
	void OnImportedAssetChanged(UPCGExPropertySchemaAsset* ChangedAsset);

	/**
	 * Render a single schema element as a flat single-row inline value editor.
	 * Returns false if the schema isn't a PCGExInlineValue type or any precondition fails,
	 * letting the caller fall back to FPCGExPropertySchemaCustomization's read-only rendering.
	 */
	bool TryRenderFlatInline(class IDetailChildrenBuilder& ChildBuilder, TSharedRef<IPropertyHandle> ElementHandle);

	/** Emit a small uppercase gray section header row -- matches the ZoneGraph customization style. */
	void EmitSectionHeader(class IDetailChildrenBuilder& ChildBuilder, const FString& Title) const;

	/**
	 * Emit one section header per imported asset and one row per override entry. Assumes the
	 * caller has already reconciled ImportOverrides so its Overrides array is parallel with
	 * the imports-only slice of Resolved.
	 */
	void EmitImportSections(class IDetailChildrenBuilder& ChildBuilder, TSharedRef<IPropertyHandle> CollectionHandle, const TArray<FPCGExPropertyResolved>& Resolved);

	/**
	 * Mutate the underlying collection: call ReconcileImportOverrides, then ask outer object's
	 * PostEditChangeProperty pipeline to run (NotifyPostChange). Safe to call from change handlers.
	 */
	void ReconcileAndNotify();

	/** Subscribe to OnSchemaAssetChanged on every asset present in the precomputed Resolved set. */
	void SubscribeToImportedAssets(const TArray<FPCGExPropertyResolved>& Resolved);

	/** Remove all subscriptions registered through SubscribeToImportedAssets. Idempotent. */
	void UnsubscribeImportedAssets();

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

	/** Active OnSchemaAssetChanged subscriptions. Cleared on customization teardown / rebuild. */
	TArray<TPair<TWeakObjectPtr<UPCGExPropertySchemaAsset>, FDelegateHandle>> AssetDelegateHandles;

	/** Cached resolved entry count for GetHeaderText (called per Slate redraw). -1 means unset. */
	mutable int32 CachedResolvedCount = -1;
};
