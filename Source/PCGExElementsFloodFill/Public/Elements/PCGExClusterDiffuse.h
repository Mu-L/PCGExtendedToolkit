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
	class FBlendOpsManager;
}

/**
 * Metadata-only diffusion over clusters: seeds spread their (blended) attributes onto reached vtx.
 * Sibling to Cluster : Flood Fill but with no path output and no per-vtx scalar outputs.
 *
 * NOTE (WIP): this first cut still claims (non-overlapping), reusing the proven in-place blend. The
 * non-claiming / union-blend behavior is layered on next; the growth core already supports both.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Clusters")
class UPCGExClusterDiffuseSettings : public UPCGExClustersProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(ClusterDiffuse, "Cluster : Diffuse", "Diffuses seed attributes onto cluster vtx via blend operations (metadata only -- no paths).");
#endif

	virtual PCGExData::EIOInit GetMainOutputInitMode() const override;
	virtual PCGExData::EIOInit GetEdgeOutputInitMode() const override;

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
	TArray<TObjectPtr<const UPCGExFillControlsFactoryData>> FillControlFactories;

	TSharedPtr<PCGExData::FFacade> SeedsDataFacade;

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
	class FBatch;

	class FProcessor final : public PCGExFloodFill::TDiffusionGrowthProcessor<FPCGExClusterDiffuseContext, UPCGExClusterDiffuseSettings>
	{
		friend FBatch;

	protected:
		TSharedPtr<TArray<int8>> InfluencesCount;
		TSharedPtr<PCGExBlending::FBlendOpsManager> BlendOpsManager;

		virtual TSharedPtr<TArray<int8>> GetInfluencesCount() const override;

	public:
		FProcessor(const TSharedRef<PCGExData::FFacade>& InVtxDataFacade, const TSharedRef<PCGExData::FFacade>& InEdgeDataFacade)
			: TDiffusionGrowthProcessor(InVtxDataFacade, InEdgeDataFacade)
		{
			bAllowEdgesDataFacadeScopedGet = false;
		}

		virtual ~FProcessor() override;

		virtual void CompleteWork() override;
		void Diffuse(const TSharedPtr<PCGExFloodFill::FDiffusion>& Diffusion);

		virtual void Cleanup() override;
	};

	class FBatch final : public PCGExClusterMT::TBatch<FProcessor>
	{
	protected:
		TSharedPtr<TArray<int8>> InfluencesCount;
		TSharedPtr<PCGExBlending::FBlendOpsManager> BlendOpsManager;
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
