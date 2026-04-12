// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"

#include "Collections/PCGExMeshCollection.h"
#include "Engine/World.h"
#include "AssetRegistry/AssetData.h"
#include "PCGExDataAssetFactory.h"

#include "PCGExMeshCollectionActions.generated.h"

class UPackage;

namespace PCGExMeshCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets);
	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExMeshCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection = false);
};

UCLASS()
class UPCGExMeshCollectionFactory : public UPCGExDataAssetFactoryBase
{
	GENERATED_BODY()

public:
	UPCGExMeshCollectionFactory() { SupportedClass = UPCGExMeshCollection::StaticClass(); }
};

UCLASS()
class UAssetDefinition_PCGExMeshCollection : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return INVTEXT("Mesh Collection"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(0, 255, 255)); }
	virtual FText GetAssetDescription(const FAssetData& AssetData) const override { return INVTEXT("A weighted collection of static meshes with optional material overrides."); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UPCGExMeshCollection::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { FAssetCategoryPath(INVTEXT("PCGEx")) / INVTEXT("Collections") };
		return Categories;
	}
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
