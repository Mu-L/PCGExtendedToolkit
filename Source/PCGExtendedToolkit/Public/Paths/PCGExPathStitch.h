﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPathProcessor.h"
#include "PCGExPaths.h"

#include "PCGExPointsProcessor.h"
#include "PCGExSorting.h"
#include "Data/PCGExPointIOMerger.h"

#include "SubPoints/DataBlending/PCGExSubPointsBlendOperation.h"
#include "PCGExPathStitch.generated.h"

UENUM()
enum class EPCGExStitchMethod : uint8
{
	Connect = 0 UMETA(DisplayName = "Connect", ToolTip="Connect existing point with a segment (preserve all input points)"),
	Fuse    = 1 UMETA(DisplayName = "Fuse", ToolTip="Merge points that should be connected, only leaving a single one."),
};

UENUM()
enum class EPCGExStitchFuseMethod : uint8
{
	KeepStart = 0 UMETA(DisplayName = "Keep Start", ToolTip="Keep start point during the merge"),
	KeepEnd   = 1 UMETA(DisplayName = "Keep End", ToolTip="Keep end point during the merge"),
};

UENUM()
enum class EPCGExStitchFuseOperation : uint8
{
	None             = 0 UMETA(DisplayName = "None", ToolTip="Keep the chosen point as-is"),
	Average          = 1 UMETA(DisplayName = "Average", ToolTip="Average connect point position"),
	LineIntersection = 2 UMETA(DisplayName = "Line Intersection", ToolTip="Connection point position is at the line/line intersection"),
};

/**
 * 
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Path", meta=(PCGExNodeLibraryDoc="paths/stitch"))
class UPCGExPathStitchSettings : public UPCGExPathProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(PathStitch, "Path : Stitch", "Stitch paths together by their endpoints.");
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

	// TODO : Stitch only start with ends
	// TODO : Sort collections for stitching priorities
	// TODO : Prioritize least amount of candidates first
	// Work like overlap check : first process all possibilities and then resolve them until there is none left
	// - Process
	// - 

public:
	/** Choose how paths are connected. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExStitchMethod Method = EPCGExStitchMethod::Connect;

	/** Choose how paths are connected. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable, DisplayName="Method", EditCondition="Method == EPCGExStitchMethod::Fuse"))
	EPCGExStitchFuseMethod FuseMethod = EPCGExStitchFuseMethod::KeepStart;

	/** Choose how paths are connected. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable, DisplayName="Operation", EditCondition="Method == EPCGExStitchMethod::Fuse"))
	EPCGExStitchFuseOperation MergeOperation = EPCGExStitchFuseOperation::None;


	/**  */
	//UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName=" └─ Average ", EditCondition="Method == EPCGExStitchMethod::Merge", EditConditionHides))
	bool bAverageMergedPoints = false;

	/** If enabled, stitching will only happen between a path's end point and another path start point. Otherwise, it's based on spatial proximity alone. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	bool bOnlyMatchStartAndEnds = false;

	/** */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bDoRequireAlignment = false;

	/** If enabled, foreign segments must be aligned within a given angular threshold. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName="Requires Alignment", EditCondition="bDoRequireAlignment"))
	FPCGExStaticDotComparisonDetails DotComparisonDetails;

	/** */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	double Tolerance = 10;

	/** Controls the order in which data will be sorted */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExSortDirection SortDirection = EPCGExSortDirection::Ascending;

	/** Meta filter settings. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, DisplayName="Carry Over Settings"))
	FPCGExCarryOverDetails CarryOverDetails;
};

struct FPCGExPathStitchContext final : FPCGExPathProcessorContext
{
	friend class FPCGExPathStitchElement;

	TArray<FPCGTaggedData> Datas;
	FPCGExStaticDotComparisonDetails DotComparisonDetails;

	FPCGExCarryOverDetails CarryOverDetails;

protected:
	PCGEX_ELEMENT_BATCH_POINT_DECL
};

class FPCGExPathStitchElement final : public FPCGExPathProcessorElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(PathStitch)

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};

namespace PCGExPathStitch
{
	class FProcessor final : public PCGExPointsMT::TProcessor<FPCGExPathStitchContext, UPCGExPathStitchSettings>
	{
	public:
		int32 WorkIndex = -1;

		PCGExMath::FSegment StartSegment; // B---A---...
		FBox StartBounds = FBox(ForceInit);

		PCGExMath::FSegment EndSegment; // ...---A---B
		FBox EndBounds = FBox(ForceInit);

		TSharedPtr<FProcessor> StartStitch = nullptr; // Which other processor is stitched to the start
		TSharedPtr<FProcessor> EndStitch = nullptr;   // Which other processor is stitched to the end

		TSharedPtr<FPCGExPointIOMerger> Merger;

		explicit FProcessor(const TSharedRef<PCGExData::FFacade>& InPointDataFacade)
			: TProcessor(InPointDataFacade)
		{
		}

		virtual bool IsTrivial() const override { return true; }
		bool IsAvailableForStitching() const { return !StartStitch || !EndStitch; }

		bool IsStitchedTo(const TSharedPtr<FProcessor>& InOtherProcessor);
		bool SetStartStitch(const TSharedPtr<FProcessor>& InStitch);
		bool SetEndStitch(const TSharedPtr<FProcessor>& InStitch);

		virtual bool Process(const TSharedPtr<PCGExMT::FTaskManager>& InAsyncManager) override;
		virtual void CompleteWork() override;
		virtual void Write() override;
	};

	class FBatch final : public PCGExPointsMT::TBatch<FProcessor>
	{
	public:
		explicit FBatch(FPCGExContext* InContext, const TArray<TWeakPtr<PCGExData::FPointIO>>& InPointsCollection);

	protected:
		virtual void OnInitialPostProcess() override;
	};
}
