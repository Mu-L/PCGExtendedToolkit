// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"


#include "Core/PCGExPointsProcessor.h"
#include "Data/Utils/PCGExDataFilterDetails.h"
#include "Blenders/PCGExUnionBlender.h"
#include "Details/PCGExBlendingDetails.h"
#include "Details/PCGExIntersectionDetails.h"
#include "Math/PCGExMathAxis.h"


#include "PCGExFusePoints.generated.h"

namespace PCGExGraphs
{
	class FUnionGraph;
}

UENUM()
enum class EPCGExFusedPointOutput : uint8
{
	Blend       = 0 UMETA(DisplayName = "Blend", ToolTip="Blend all points within a radius"),
	MostCentral = 1 UMETA(DisplayName = "Keep Most Central", ToolTip="Keep the existing point that's most central to the sample group"),
};

UENUM()
enum class EPCGExFusedBoundsMode : uint8
{
	None = 0 UMETA(DisplayName = "None", ToolTip="Don't modify fused point bounds."),
	AABB = 1 UMETA(DisplayName = "AABB", ToolTip="Axis-aligned bounding box of all fused points."),
	OBB  = 2 UMETA(DisplayName = "OBB", ToolTip="Oriented bounding box of all fused points. Overrides point rotation."),
};

namespace PCGExFuse
{
	struct FFusedPoint
	{
		mutable FRWLock IndicesLock;
		int32 Index = -1;
		FVector Position = FVector::ZeroVector;
		TArray<int32> Fused;
		TArray<double> Distances;
		double MaxDistance = 0;

		FFusedPoint(const int32 InIndex, const FVector& InPosition);
		~FFusedPoint() = default;

		void Add(const int32 InIndex, const double Distance);
	};
}

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Misc", meta=(PCGExNodeLibraryDoc="transform/modify/fuse-points"))
class UPCGExFusePointsSettings : public UPCGExPointsProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(FusePoints, "Fuse Points", "Fuse points based on distance.");
	virtual FLinearColor GetNodeTitleColor() const override { return PCGEX_NODE_COLOR_NAME(MiscRemove); }
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

public:
	/** Mode */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExFusedPointOutput Mode = EPCGExFusedPointOutput::Blend;

	/** Fuse Settings */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName="Point/Point Settings"))
	FPCGExPointPointIntersectionDetails PointPointIntersectionDetails = FPCGExPointPointIntersectionDetails(false);

	/** Preserve the order of input points */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings)
	bool bPreserveOrder = true;

	/** Override the bounds of each fused point to encompass all the points that were merged into it. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExFusedBoundsMode FusedBoundsMode = EPCGExFusedBoundsMode::None;

	/** Which point bounds should be used */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable, DisplayName=" └─ Bounds Source", EditCondition="FusedBoundsMode != EPCGExFusedBoundsMode::None", EditConditionHides))
	EPCGExPointBoundsSource BoundsSource = EPCGExPointBoundsSource::ScaledBounds;

	/** Axis order for OBB computation */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable, DisplayName=" ├─ Axis Order", EditCondition="FusedBoundsMode == EPCGExFusedBoundsMode::OBB", EditConditionHides))
	EPCGExAxisOrder AxisOrder = EPCGExAxisOrder::XYZ;

	/** Minimum extent for fused bounds on each axis. Prevents degenerate (flat) bounds. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName=" └─ Min Bounds Extent", EditCondition="FusedBoundsMode != EPCGExFusedBoundsMode::None", EditConditionHides, ClampMin=0))
	double MinBoundsExtent = 1;
	
	/** Defines how fused point properties and attributes are merged together. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(EditCondition="Mode == EPCGExFusedPointOutput::Blend", EditConditionHides))
	FPCGExBlendingDetails BlendingDetails = FPCGExBlendingDetails(EPCGExBlendingType::Average, EPCGExBlendingType::None);

	/** Meta filter settings. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, DisplayName="Carry Over Settings", EditCondition="Mode == EPCGExFusedPointOutput::Blend", EditConditionHides))
	FPCGExCarryOverDetails CarryOverDetails;

private:
	friend class FPCGExFusePointsElement;
};

struct FPCGExFusePointsContext final : FPCGExPointsProcessorContext
{
	friend class FPCGExFusePointsElement;
	const PCGExMath::IDistances* Distances = nullptr;
	FPCGExCarryOverDetails CarryOverDetails;

protected:
	PCGEX_ELEMENT_BATCH_POINT_DECL
};

class FPCGExFusePointsElement final : public FPCGExPointsProcessorElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(FusePoints)

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};

namespace PCGExFusePoints
{
	class FProcessor final : public PCGExPointsMT::TProcessor<FPCGExFusePointsContext, UPCGExFusePointsSettings>
	{
		TSharedPtr<PCGExGraphs::FUnionGraph> UnionGraph;
		TSharedPtr<PCGExBlending::IUnionBlender> UnionBlender;

		TSharedPtr<PCGExData::TBuffer<bool>> IsUnionWriter;
		TSharedPtr<PCGExData::TBuffer<int32>> UnionSizeWriter;

	public:
		explicit FProcessor(const TSharedRef<PCGExData::FFacade>& InPointDataFacade)
			: TProcessor(InPointDataFacade)
		{
			bForceSingleThreadedProcessPoints = true;
		}

		virtual ~FProcessor() override;

		virtual bool Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager) override;
		virtual void ProcessPoints(const PCGExMT::FScope& Scope) override;

		virtual void ProcessRange(const PCGExMT::FScope& Scope) override;
		
		virtual void CompleteWork() override;
		virtual void Write() override;
	};
}
