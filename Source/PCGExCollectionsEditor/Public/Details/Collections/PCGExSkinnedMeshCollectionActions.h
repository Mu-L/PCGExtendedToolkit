// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"

#include "PCGExDataAssetFactory.h"
#include "AssetRegistry/AssetData.h"
#include "Collections/PCGExSkinnedMeshCollection.h"
#include "Engine/World.h"

#include "PCGExSkinnedMeshCollectionActions.generated.h"

class UPackage;

namespace PCGExSkinnedMeshCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets);
	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExSkinnedMeshCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection = false);
};

UCLASS()
class UPCGExSkinnedMeshCollectionFactory : public UPCGExDataAssetFactoryBase
{
	GENERATED_BODY()

public:
	UPCGExSkinnedMeshCollectionFactory()
	{
		SupportedClass = UPCGExSkinnedMeshCollection::StaticClass();
	}
};

UCLASS()
class UAssetDefinition_PCGExSkinnedMeshCollection : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override
	{
		return INVTEXT("Skinned Mesh Collection");
	}

	virtual FLinearColor GetAssetColor() const override
	{
		return FLinearColor(FColor(0, 255, 255));
	}

	virtual FText GetAssetDescription(const FAssetData& AssetData) const override
	{
		return INVTEXT("A weighted collection of skinned meshes with optional material overrides.");
	}

	virtual TSoftClassPtr<UObject> GetAssetClass() const override
	{
		return UPCGExSkinnedMeshCollection::StaticClass();
	}

	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = {FAssetCategoryPath(INVTEXT("PCGEx")) / INVTEXT("Collections")};
		return Categories;
	}

	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
