// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExClusterDiffuse.h"

#include "Clusters/PCGExCluster.h"
#include "Core/PCGExBlendOpsManager.h"
#include "Core/PCGExFillControlsFactoryProvider.h"
#include "Core/PCGExFloodFill.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExBlendingDetails.h"
#include "Details/PCGExSettingsDetails.h"

#define LOCTEXT_NAMESPACE "PCGExClusterDiffuse"
#define PCGEX_NAMESPACE ClusterDiffuse

PCGExData::EIOInit UPCGExClusterDiffuseSettings::GetMainOutputInitMode() const
{
	return PCGExData::EIOInit::Duplicate;
}

PCGExData::EIOInit UPCGExClusterDiffuseSettings::GetEdgeOutputInitMode() const
{
	// Edges are not modified by this node -- pass them through unchanged.
	return PCGExData::EIOInit::Forward;
}

TArray<FPCGPinProperties> UPCGExClusterDiffuseSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	PCGEX_PIN_POINT(PCGExCommon::Labels::SourceSeedsLabel, "Seed points.", Required)
	PCGEX_PIN_FACTORIES(PCGExFloodFill::SourceFillControlsLabel, "Fill controls, used to constraint & limit diffusion", Normal, FPCGExDataTypeInfoFillControl::AsId())
	PCGExBlending::DeclareBlendOpsInputs(PinProperties, EPCGPinStatus::Normal);

	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(ClusterDiffuse)
PCGEX_ELEMENT_BATCH_EDGE_IMPL_ADV(ClusterDiffuse)

bool FPCGExClusterDiffuseElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExClustersProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(ClusterDiffuse)

	PCGExFactories::GetInputFactories<UPCGExBlendOpFactory>(Context, PCGExBlending::Labels::SourceBlendingLabel, Context->BlendingFactories, {FPCGExDataTypeInfoBlendOp::AsId()}, false);

	// Fill controls are optional
	PCGExFactories::GetInputFactories<UPCGExFillControlsFactoryData>(Context, PCGExFloodFill::SourceFillControlsLabel, Context->FillControlFactories, {FPCGExDataTypeInfoFillControl::AsId()}, false);

	Context->SeedsDataFacade = PCGExData::TryGetSingleFacade(Context, PCGExCommon::Labels::SourceSeedsLabel, false, true);
	if (!Context->SeedsDataFacade)
	{
		return false;
	}

	return true;
}

bool FPCGExClusterDiffuseElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExClusterDiffuseElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(ClusterDiffuse)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartProcessingClusters(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries)
			{
				return true;
			}, [&](const TSharedPtr<PCGExClusterMT::IBatch>& NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			}))
		{
			return Context->CancelExecution(TEXT("Could not build any clusters."));
		}
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->OutputPointsAndEdges();

	return Context->TryComplete();
}

namespace PCGExClusterDiffuse
{
	FProcessor::~FProcessor()
	{
	}

	TSharedPtr<TArray<int8>> FProcessor::GetInfluencesCount() const
	{
		// WIP: claiming on for now (each vtx captured once -> safe in-place blend). Returning null
		// here is all it takes to make diffusions overlap; the union-blend output lands next.
		return InfluencesCount;
	}

	void FProcessor::CompleteWork()
	{
		if (Diffusions.IsEmpty())
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("No valid diffusions."));
			bIsProcessorValid = false;
			return;
		}

		PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, DiffuseDiffusions)

		DiffuseDiffusions->OnIterationCallback = [PCGEX_ASYNC_THIS_CAPTURE](const int32 Index, const PCGExMT::FScope& Scope)
		{
			PCGEX_ASYNC_THIS
			This->Diffuse(This->Diffusions[Index]);
		};

		DiffuseDiffusions->StartIterations(Diffusions.Num(), 1);
	}

	void FProcessor::Diffuse(const TSharedPtr<PCGExFloodFill::FDiffusion>& Diffusion)
	{
		TArray<int32> Indices;
		PCGExFloodFill::DiffuseAndBlend(*Diffusion, VtxDataFacade, BlendOpsManager, Indices);
		Diffusion->Candidates.Empty();
	}

	void FProcessor::Cleanup()
	{
		// Base resets the growth state (diffusions + fill controls handler).
		TDiffusionGrowthProcessor<FPCGExClusterDiffuseContext, UPCGExClusterDiffuseSettings>::Cleanup();
		BlendOpsManager.Reset();
	}

	FBatch::FBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FPointIO>& InVtx, TArrayView<TSharedRef<PCGExData::FPointIO>> InEdges)
		: TBatch(InContext, InVtx, InEdges)
	{
	}

	FBatch::~FBatch()
	{
	}

	void FBatch::RegisterBuffersDependencies(PCGExData::FFacadePreloader& FacadePreloader)
	{
		TBatch<FProcessor>::RegisterBuffersDependencies(FacadePreloader);

		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ClusterDiffuse)

		PCGExBlending::RegisterBuffersDependencies(Context, FacadePreloader, Context->BlendingFactories);

		for (const TObjectPtr<const UPCGExFillControlsFactoryData>& Factory : Context->FillControlFactories)
		{
			Factory->RegisterBuffersDependencies(Context, FacadePreloader);
		}
	}

	void FBatch::Process()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ClusterDiffuse)

		BlendOpsManager = MakeShared<PCGExBlending::FBlendOpsManager>(VtxDataFacade);
		if (!BlendOpsManager->Init(Context, Context->BlendingFactories))
		{
			bIsBatchValid = false;
			return;
		}

		InfluencesCount = MakeShared<TArray<int8>>();
		InfluencesCount->Init(0, VtxDataFacade->GetNum());

		FillRate = PCGExDetails::MakeSettingValue<int32>(Settings->Diffusion.FillRateInput, Settings->Diffusion.FillRateAttribute, Settings->Diffusion.FillRateConstant);
		bIsBatchValid = FillRate->Init(Settings->Diffusion.FillRateSource == EPCGExFloodFillSettingSource::Seed ? Context->SeedsDataFacade : VtxDataFacade);

		if (!bIsBatchValid)
		{
			return;
		}

		TBatch<FProcessor>::Process();
	}

	bool FBatch::PrepareSingle(const TSharedPtr<PCGExClusterMT::IProcessor>& InProcessor)
	{
		if (!TBatch<FProcessor>::PrepareSingle(InProcessor))
		{
			return false;
		}

		PCGEX_TYPED_PROCESSOR

		TypedProcessor->BlendOpsManager = BlendOpsManager;
		TypedProcessor->InfluencesCount = InfluencesCount;
		TypedProcessor->FillRate = FillRate;

		return true;
	}

	void FBatch::Write()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ClusterDiffuse)

		TBatch<FProcessor>::Write();
		BlendOpsManager->Cleanup(Context);
		VtxDataFacade->WriteFastest(TaskManager);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
