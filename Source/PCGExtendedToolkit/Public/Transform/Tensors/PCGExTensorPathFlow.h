﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPointsProcessor.h"
#include "PCGExTensor.h"
#include "PCGExTensorOperation.h"
#include "PCGExTensorSplineFactoryProvider.h"
#include "PCGExTensorSplineFlow.h"


#include "PCGExTensorPathFlow.generated.h"


USTRUCT(BlueprintType)
struct FPCGExTensorPathFlowConfig : public FPCGExTensorConfigBase
{
	GENERATED_BODY()

	FPCGExTensorPathFlowConfig() :
		FPCGExTensorConfigBase(false)
	{
	}

	/** Closed loop handling.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	FPCGExPathClosedLoopDetails ClosedLoop;

	/** Which point type to use. Shared amongst all points; if you want tight control, create a fully-fledged spline instead. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExSplinePointTypeRedux PointType = EPCGExSplinePointTypeRedux::Linear;

	/** Sample inputs.*/
	//UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExSplineSamplingIncludeMode SampleInputs = EPCGExSplineSamplingIncludeMode::All;

	/**  Base radius of the spline. Will be scaled by control points' scale length */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayAfter="TensorWeight"))
	double Radius = 100;

	/**  Which spline transform axis is to be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExAxis SplineDirection = EPCGExAxis::Forward;

	virtual void Init() override;
};

/**
 * 
 */
class FPCGExTensorPathFlow : public PCGExTensorOperation
{
public:
	FPCGExTensorPathFlowConfig Config;
	const TArray<TSharedPtr<const FPCGSplineStruct>>* Splines = nullptr;

	virtual bool Init(FPCGExContext* InContext, const UPCGExTensorFactoryData* InFactory) override;

	virtual PCGExTensor::FTensorSample Sample(int32 InSeedIndex, const FTransform& InProbe) const override;
};


UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExTensorPathFlowFactory : public UPCGExTensorSplineFlowFactory
{
	GENERATED_BODY()

public:
	FPCGExTensorPathFlowConfig Config;
	virtual TSharedPtr<PCGExTensorOperation> CreateOperation(FPCGExContext* InContext) const override;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Tensors|Params", meta=(PCGExNodeLibraryDoc="tensors/effectors/tensor-path-flow"))
class UPCGExCreateTensorPathFlowSettings : public UPCGExTensorSplineFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(TensorPathFlow, "Tensor : Path Flow", "A tensor that represent a vector/flow field along a path")

#endif
	//~End UPCGSettings

	virtual bool GetBuildFromPoints() const override { return true; }

	/** Tensor properties */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExTensorPathFlowConfig Config;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;
};
