// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"
#include "Collections/PCGExPCGDataAssetCollection.h"
#include "Engine/World.h"
#include "AssetRegistry/AssetData.h"
#include "PCGExDataAssetFactory.h"

#include "PCGExPCGDataAssetCollectionActions.generated.h"

class UPackage;

namespace PCGExPCGDataAssetCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets);
	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExPCGDataAssetCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection = false);
};

UCLASS()
class UPCGExPCGDataAssetCollectionFactory : public UPCGExDataAssetFactoryBase
{
	GENERATED_BODY()

public:
	UPCGExPCGDataAssetCollectionFactory() { SupportedClass = UPCGExPCGDataAssetCollection::StaticClass(); }
};

UCLASS()
class UAssetDefinition_PCGExPCGDataAssetCollection : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return INVTEXT("PCGDataAsset Collection"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(100, 150, 200)); }
	virtual FText GetAssetDescription(const FAssetData& AssetData) const override { return INVTEXT("A weighted collection of PCG Data Assets."); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UPCGExPCGDataAssetCollection::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { FAssetCategoryPath(INVTEXT("PCGEx")) / INVTEXT("Collections") };
		return Categories;
	}
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
