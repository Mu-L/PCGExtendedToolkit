// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"
#include "Collections/PCGExActorCollection.h"
#include "Engine/World.h"
#include "AssetRegistry/AssetData.h"
#include "PCGExDataAssetFactory.h"

#include "PCGExActorCollectionActions.generated.h"

class UPackage;

namespace PCGExActorCollectionActions
{
	void CreateCollectionFrom(const TArray<FAssetData>& SelectedAssets);
	void UpdateCollectionsFrom(
		const TArray<TObjectPtr<UPCGExActorCollection>>& SelectedCollections,
		const TArray<FAssetData>& SelectedAssets,
		bool bIsNewCollection = false);
};

UCLASS()
class UPCGExActorCollectionFactory : public UPCGExDataAssetFactoryBase
{
	GENERATED_BODY()

public:
	UPCGExActorCollectionFactory() { SupportedClass = UPCGExActorCollection::StaticClass(); }
};

UCLASS()
class UAssetDefinition_PCGExActorCollection : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override { return INVTEXT("Actor Collection"); }
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(67, 142, 245)); }
	virtual FText GetAssetDescription(const FAssetData& AssetData) const override { return INVTEXT("A weighted collection of actor classes for spawning."); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UPCGExActorCollection::StaticClass(); }
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override
	{
		static const auto Categories = { FAssetCategoryPath(INVTEXT("PCGEx")) / INVTEXT("Collections") };
		return Categories;
	}
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
