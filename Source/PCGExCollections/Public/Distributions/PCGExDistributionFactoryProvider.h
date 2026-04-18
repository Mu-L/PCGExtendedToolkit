// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExCollectionsCommon.h"
#include "PCGExDistributionFactoryBaseConfig.h"
#include "Factories/PCGExFactoryData.h"
#include "Factories/PCGExFactoryProvider.h"

#include "PCGExDistributionFactoryProvider.generated.h"

class FPCGExEntryPickerOperation;
class FPCGExMicroEntryPickerOperation;

USTRUCT(meta=(PCG_DataTypeDisplayName="PCGEx | Distribution"))
struct FPCGExDataTypeInfoDistribution : public FPCGExFactoryDataTypeInfo
{
	GENERATED_BODY()
	PCG_DECLARE_TYPE_INFO(PCGEXCOLLECTIONS_API)
};

/**
 * Abstract factory data for collection distribution. Flows on the "Distribution" pin from
 * palette factory nodes to consuming nodes (Staging Distribute, Spline Mesh, ...).
 *
 * Concrete subclasses override CreateEntryOperation to emit the hot-path picker matching
 * their distribution mode (Index, Random, WeightedRandom, or user-authored).
 * CreateMicroOperation dispatches on BaseConfig.EntryDistribution and is typically not
 * overridden by concrete main-mode factories.
 */
UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXCOLLECTIONS_API UPCGExDistributionFactoryData : public UPCGExFactoryData
{
	GENERATED_BODY()

public:
	PCG_ASSIGN_TYPE_INFO(FPCGExDataTypeInfoDistribution)

	UPROPERTY()
	FPCGExDistributionFactoryBaseConfig BaseConfig;

	virtual PCGExFactories::EType GetFactoryType() const override { return PCGExFactories::EType::Distribution; }

	/** Create a hot-path entry picker operation. Concrete subclasses override. */
	virtual TSharedPtr<FPCGExEntryPickerOperation> CreateEntryOperation(FPCGExContext* InContext) const;

	/** Create a hot-path micro picker operation based on BaseConfig.EntryDistribution. */
	virtual TSharedPtr<FPCGExMicroEntryPickerOperation> CreateMicroOperation(FPCGExContext* InContext) const;
};

/**
 * Abstract palette node base for distribution factories. Concrete subclasses
 * (Index / Random / WeightedRandom, plus any user-authored mode) inherit this
 * and fill in CreateFactory to emit their matching UPCGExDistributionFactoryData.
 *
 * Output pin label: "Distribution" (see PCGExCollections::Labels::OutputDistributionLabel).
 */
UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Graph|Params")
class PCGEXCOLLECTIONS_API UPCGExDistributionFactoryProviderSettings : public UPCGExFactoryProviderSettings
{
	GENERATED_BODY()

protected:
	PCGEX_FACTORY_TYPE_ID(FPCGExDataTypeInfoDistribution)

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(DistributionFactory, "Distribution Definition", "Creates a distribution factory definition.")
	virtual FLinearColor GetNodeTitleColor() const override { return PCGEX_NODE_COLOR_NAME(Misc); }
#endif
	//~End UPCGSettings

	virtual FName GetMainOutputPin() const override { return PCGExCollections::Labels::OutputDistributionLabel; }
	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;
};
