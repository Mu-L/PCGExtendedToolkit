// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Clusters/PCGExClusterCommon.h"
#include "Core/PCGExPathProcessor.h"
#include "Graphs/PCGExGraphDetails.h"
#include "Math/PCGExProjectionDetails.h"

#include "PCGExBuildStraightSkeleton.generated.h"

namespace PCGExGraphs
{
	class FGraphBuilder;
}

namespace PCGExData
{
	template <typename T>
	class TBuffer;
}

/**
 * Straight Skeleton generator: takes closed paths (polygons) as input and produces one edge cluster
 * per polygon -- the inward mitered-offset / wavefront topology (roofs, mid-lines, inset partitioning).
 *
 * The skeleton is computed in 2D on a projection plane, then unprojected back to 3D. The solver is the
 * robust iterative mitered-offset (Clipper2) implementation -- see FStraightSkeletonOffset.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Clusters", meta=(Keywords = "skeleton medial roof miter inset offset", PCGExNodeLibraryDoc="clusters/generate/cluster-straight-skeleton"))
class UPCGExBuildStraightSkeletonSettings : public UPCGExPathProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(BuildStraightSkeleton, "Cluster : Straight Skeleton", "Compute the straight skeleton (mitered-offset / wavefront topology) of each closed input path.");

	virtual FLinearColor GetNodeTitleColor() const override
	{
		return PCGEX_NODE_COLOR_NAME(ClusterGenerator);
	}
#endif
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

	//~Begin UPCGExPointsProcessorSettings
public:
	virtual FName GetMainOutputPin() const override
	{
		return PCGExClusters::Labels::OutputVerticesLabel;
	}

	//~End UPCGExPointsProcessorSettings

	/** Projection plane settings -- the skeleton is computed in 2D, then unprojected to 3D. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FPCGExGeo2DProjectionDetails ProjectionDetails;

	/** Include the original path boundary as Contour-classified edges in the output cluster. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	bool bIncludeContour = false;

	/** Weld skeleton nodes closer than this distance -- handles the near-coincident points degenerate
	 *  events produce. 0 welds exact duplicates only. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, ClampMin="0"))
	double MergeDistance = 1.0;

	/** Write each node's wavefront offset distance (inset depth / roof height) to a vertex attribute. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Output", meta = (PCG_Overridable, InlineEditConditionToggle))
	bool bWriteOffsetDistance = false;

	/** Name of the 'double' vertex attribute to write the offset distance to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Output", meta = (PCG_Overridable, EditCondition="bWriteOffsetDistance"))
	FName OffsetDistanceAttributeName = "OffsetDistance";

	/** Graph & Edges output properties */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, DisplayName="Cluster Output Settings"))
	FPCGExGraphBuilderDetails GraphBuilderDetails = FPCGExGraphBuilderDetails(EPCGExMinimalAxis::X);

private:
	friend class FPCGExBuildStraightSkeletonElement;
};

struct FPCGExBuildStraightSkeletonContext final : FPCGExPathProcessorContext
{
	friend class FPCGExBuildStraightSkeletonElement;

protected:
	PCGEX_ELEMENT_BATCH_POINT_DECL
};

class FPCGExBuildStraightSkeletonElement final : public FPCGExPathProcessorElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(BuildStraightSkeleton)

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};

namespace PCGExBuildStraightSkeleton
{
	class FProcessor final : public PCGExPointsMT::TProcessor<FPCGExBuildStraightSkeletonContext, UPCGExBuildStraightSkeletonSettings>
	{
		FPCGExGeo2DProjectionDetails ProjectionDetails;
		TSharedPtr<PCGExGraphs::FGraphBuilder> GraphBuilder;
		TSharedPtr<PCGExData::TBuffer<double>> OffsetWriter;

		// Per output node, the wavefront offset (== node Time). Parallel to the created vtx points.
		TArray<double> NodeTimes;

	public:
		explicit FProcessor(const TSharedRef<PCGExData::FFacade>& InPointDataFacade)
			: TProcessor(InPointDataFacade)
		{
		}

		virtual ~FProcessor() override;

		virtual bool Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager) override;
		virtual void CompleteWork() override;
		virtual void Write() override;
		virtual void Output() override;
	};
}
