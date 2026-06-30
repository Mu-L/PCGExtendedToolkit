// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "Core/PCGExClustersProcessor.h"
#include "Core/PCGExFloodFill.h"
#include "Core/PCGExDiffusionGrowthProcessor.h"
#include "PCGExClusterDiffuse.generated.h"

class UPCGExBlendOpFactory;

namespace PCGExBlending
{
	class FUnionOpsManager;
	class FBlendOpsSchema;
}

UENUM()
enum class EPCGExClusterDiffuseWeightMode : uint8
{
	Count         = 0 UMETA(DisplayName = "Count", ToolTip="Every contributor weighs equally; the vtx becomes the mean of the seeds that reached it."),
	Distance      = 1 UMETA(DisplayName = "Distance", ToolTip="Closer seeds (by distance to the vtx) weigh more, farther seeds less."),
	Depth         = 2 UMETA(DisplayName = "Depth", ToolTip="Inverse-depth: seeds that reached the vtx in fewer diffusion steps weigh more; contributions blend smoothly where two diffusions meet."),
	CountAndDepth = 3 UMETA(DisplayName = "Count + Depth", ToolTip="Half equal weighting, half depth weighting -- depth biases the blend without shallow seeds dominating as strongly as pure Depth."),
};

/**
 * Metadata-only diffusion over clusters: seeds spread their attributes onto reached vtx via blend
 * operations. Sibling to Cluster : Flood Fill, but with no path output and no per-vtx scalar outputs.
 *
 * Unlike Flood Fill, diffusions do NOT claim vtx -- they overlap freely. Each reached vtx is the
 * union of every seed that touched it, reconciled by a single weighted blend pass (equal weight per
 * contributor for now; a count/distance weighting choice and a seed-sourced second layer come next).
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Clusters")
class UPCGExClusterDiffuseSettings : public UPCGExClustersProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(ClusterDiffuse, "Cluster : Diffuse", "Diffuses seed attributes onto cluster vtx via blend operations (metadata only -- no paths, overlapping diffusions).");
#endif

	virtual PCGExData::EIOInit GetMainOutputInitMode() const override;
	virtual PCGExData::EIOInit GetEdgeOutputInitMode() const override;

#if WITH_EDITOR
	virtual void PCGExApplyDeprecation(UPCGNode* InOutNode) override;
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

public:
	/** Seeds settings */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_NotOverridable))
	FPCGExFloodFillSeedPickingDetails Seeds;

	/** Defines how each vtx is diffused */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_NotOverridable))
	EPCGExFloodFillProcessing Processing = EPCGExFloodFillProcessing::Parallel;

	/** Diffusion settings */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_NotOverridable))
	FPCGExFloodFillFlowDetails Diffusion;

	/** How overlapping seed contributions are weighted when blended onto a vtx. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Blending", meta = (PCG_NotOverridable))
	EPCGExClusterDiffuseWeightMode Weighting = EPCGExClusterDiffuseWeightMode::Count;

	/** Whether the vtx's own (pre-diffusion) value participates in the blend, alongside the seeds that reached it. When off, reached vtx are fully replaced by the seed blend. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Blending", meta = (PCG_Overridable))
	FPCGExInputShorthandSelectorBoolean VtxParticipates = FPCGExInputShorthandSelectorBoolean(FName("VtxParticipates"), false, false);

	/** Whether or not to search for closest node using an octree. Depending on your dataset, enabling this may be either much faster, or much slower. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Performance, meta=(PCG_NotOverridable, AdvancedDisplay))
	bool bUseOctreeSearch = false;

private:
	friend class FPCGExClusterDiffuseElement;
};

struct FPCGExClusterDiffuseContext final : FPCGExClustersProcessorContext
{
	friend class FPCGExClusterDiffuseElement;

	TArray<TObjectPtr<const UPCGExBlendOpFactory>> BlendingFactories;
	TArray<TObjectPtr<const UPCGExBlendOpFactory>> SeedBlendingFactories;
	TArray<TObjectPtr<const UPCGExFillControlsFactoryData>> FillControlFactories;

	TSharedPtr<PCGExData::FFacade> SeedsDataFacade;
	// Pre-resolved (Boot, single-threaded) blend-op configs for the seeds-cloud source layer, so
	// per-processor blender init is thread-safe against the shared seeds facade.
	TSharedPtr<PCGExBlending::FBlendOpsSchema> SeedBlendOpsSchema;

protected:
	PCGEX_ELEMENT_BATCH_EDGE_DECL
};

class FPCGExClusterDiffuseElement final : public FPCGExClustersProcessorElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(ClusterDiffuse)

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};

namespace PCGExClusterDiffuse
{
	const FName SourceSeedBlendingLabel = FName(TEXT("Seed Blend Ops"));

	class FBatch;

	// One seed's contribution to a reached vtx (collected after growth, consumed by the blend pass).
	struct FContribution
	{
		int32 SeedVtxIndex = -1;   // seed's vtx point index -- the Layer 1 blend source
		int32 SeedPointIndex = -1; // seed's seeds-cloud point index -- reserved for the seed blend layer
		int32 Depth = 0;           // diffusion depth at which the seed reached this vtx
	};

	class FProcessor final : public PCGExFloodFill::TDiffusionGrowthProcessor<FPCGExClusterDiffuseContext, UPCGExClusterDiffuseSettings>
	{
		friend FBatch;

	protected:
		// Per reached vtx (indexed by vtx point index), the seeds that touched it. Built after growth.
		TArray<TArray<FContribution>> VtxContributors;
		// Layer 1: blends seed VTX values onto reached vtx (source & target = vtx facade).
		TSharedPtr<PCGExBlending::FUnionOpsManager> VtxBlender;
		// Layer 2: blends seeds-cloud values onto reached vtx (source = seeds facade, target = vtx facade).
		TSharedPtr<PCGExBlending::FUnionOpsManager> SeedBlender;

		// NOTE: GetInfluencesCount() is intentionally left to the base default (null) -> claiming is
		// disabled, so diffusions overlap. The union blend below reconciles multi-influence vtx.

	public:
		FProcessor(const TSharedRef<PCGExData::FFacade>& InVtxDataFacade, const TSharedRef<PCGExData::FFacade>& InEdgeDataFacade)
			: TDiffusionGrowthProcessor(InVtxDataFacade, InEdgeDataFacade)
		{
			bAllowEdgesDataFacadeScopedGet = false;
		}

		virtual ~FProcessor() override;

		virtual void CompleteWork() override;
		virtual void Cleanup() override;
	};

	class FBatch final : public PCGExClusterMT::TBatch<FProcessor>
	{
	protected:
		TSharedPtr<PCGExDetails::TSettingValue<int32>> FillRate;

	public:
		FBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FPointIO>& InVtx, TArrayView<TSharedRef<PCGExData::FPointIO>> InEdges);
		virtual ~FBatch() override;

		virtual void RegisterBuffersDependencies(PCGExData::FFacadePreloader& FacadePreloader) override;
		virtual void Process() override;
		virtual bool PrepareSingle(const TSharedPtr<PCGExClusterMT::IProcessor>& InProcessor) override;
		virtual void Write() override;
	};
}
