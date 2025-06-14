﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPointsProcessor.h"
#include "PCGExPicker.h"
#include "PCGExPickerFactoryProvider.h"

#include "PCGExPickerConstant.generated.h"

USTRUCT(BlueprintType)
struct FPCGExPickerConstantConfig : public FPCGExPickerConfigBase
{
	GENERATED_BODY()

	FPCGExPickerConstantConfig() :
		FPCGExPickerConfigBase()
	{
	}

	/**  Use negative values to select from the end. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="!bTreatAsNormalized", EditConditionHides, DisplayAfter="bTreatAsNormalized"))
	int32 DiscreteIndex = 0;

	/**  Use negative values to select from the end. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="bTreatAsNormalized", EditConditionHides, DisplayAfter="bTreatAsNormalized"))
	double RelativeIndex = 0;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExPickerConstantFactory : public UPCGExPickerFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExPickerConstantConfig Config;

	virtual void AddPicks(int32 InNum, TSet<int32>& OutPicks) const override;

protected:
	virtual bool InitInternalData(FPCGExContext* InContext) override;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Pickers|Params", meta=(PCGExNodeLibraryDoc="filters/cherry-pick-points/picker-constant"))
class UPCGExPickerConstantSettings : public UPCGExPickerFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(
		PickerConstant, "Picker : Constant", "A Picker that has a single value.",
		FName(GetDisplayName()))

#endif
	//~End UPCGSettings

	/** Picker properties */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExPickerConstantConfig Config;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
