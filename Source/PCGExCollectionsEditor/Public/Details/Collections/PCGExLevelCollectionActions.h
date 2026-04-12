// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"
#include "Collections/PCGExLevelCollection.h"
#include "Engine/World.h"
#include "AssetRegistry/AssetData.h"
#include "PCGExDataAssetFactory.h"

#include "PCGExLevelCollectionActions.generated.h"

class UPackage;

namespace PCGExLevelCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets);
	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExLevelCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection = false);
};

UCLASS()
class UPCGExLevelCollectionFactory : public UPCGExDataAssetFactoryBase
{
	GENERATED_BODY()

public:
	UPCGExLevelCollectionFactory() { SupportedClass = UPCGExLevelCollection::StaticClass(); }
};

UCLASS()
class UAssetDefinition_PCGExLevelCollection : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return INVTEXT("Level Collection"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(255, 156, 0)); }
	virtual FText GetAssetDescription(const FAssetData& AssetData) const override { return INVTEXT("A weighted collection of level assets."); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UPCGExLevelCollection::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { FAssetCategoryPath(INVTEXT("PCGEx")) / INVTEXT("Collections") };
		return Categories;
	}
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
