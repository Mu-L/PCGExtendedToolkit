// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExClusterDiffuse.h"

#include "PCGExVersion.h"
#include "Blenders/PCGExUnionOpsManager.h"
#include "Clusters/PCGExCluster.h"
#include "Core/PCGExBlendOpsManager.h"
#include "Core/PCGExFillControlsFactoryProvider.h"
#include "Core/PCGExFloodFill.h"
#include "Core/PCGExOpStats.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointElements.h"
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

#if WITH_EDITOR
void UPCGExClusterDiffuseSettings::PCGExApplyDeprecation(UPCGNode* InOutNode)
{
	PCGEX_IF_VERSION_LOWER(1, 76, 3)
	{
		// FillRate migrated from the Input/Attribute/Constant triple to FPCGExInputShorthandSelectorInteger32Abs.
		Diffusion.ApplyDeprecation();
	}

	Super::PCGExApplyDeprecation(InOutNode);
}
#endif

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

	void FProcessor::CompleteWork()
	{
		if (Diffusions.IsEmpty())
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("No valid diffusions."));
			bIsProcessorValid = false;
			return;
		}

		// Nothing to blend without blend operations -- the vtx pass through unchanged.
		if (Context->BlendingFactories.IsEmpty())
		{
			return;
		}

		const int32 NumVtx = VtxDataFacade->GetNum();

		// Per-vtx "does the original value participate" toggle (constant or attribute).
		// Non-scoped (full) read: the grouping below reads participation at arbitrary vtx indices.
		const TSharedPtr<PCGExDetails::TSettingValue<bool>> ParticipatesValue = Settings->VtxParticipates.GetValueSetting();
		const bool bHasParticipates = ParticipatesValue && ParticipatesValue->Init(VtxDataFacade, false);

		// Collect, per reached vtx, every seed that touched it (with the depth it reached at). Built
		// single-threaded -- the parallel growth passes have finished. Distinct seeds have distinct vtx
		// indices, so no dedup is needed except for the self-participation case handled below.
		VtxContributors.Empty();
		VtxContributors.SetNum(NumVtx);

		for (const TSharedPtr<PCGExFloodFill::FDiffusion>& Diffusion : Diffusions)
		{
			const int32 SeedVtx = Diffusion->SeedNode->PointIndex;
			const int32 SeedPoint = Diffusion->SeedIndex;
			for (const PCGExFloodFill::FCandidate& Candidate : Diffusion->Captured)
			{
				VtxContributors[Candidate.Node->PointIndex].Add(FContribution{SeedVtx, SeedPoint, Candidate.Depth});
			}
		}

		// Optionally add each reached vtx's own value as a contributor (depth 0), unless it is already
		// present (the vtx is its own seed) -- avoid double-counting that value.
		if (bHasParticipates)
		{
			for (int32 VtxIndex = 0; VtxIndex < NumVtx; VtxIndex++)
			{
				TArray<FContribution>& Contributors = VtxContributors[VtxIndex];
				if (Contributors.IsEmpty() || !ParticipatesValue->Read(VtxIndex)) { continue; }

				bool bSelfPresent = false;
				for (const FContribution& C : Contributors) { if (C.SeedVtxIndex == VtxIndex) { bSelfPresent = true; break; } }
				if (!bSelfPresent) { Contributors.Add(FContribution{VtxIndex, -1, 0}); }
			}
		}

		// Single source & target: the vtx facade. Distances is unused -- we feed weights directly to
		// Blend() rather than computing them spatially via ComputeWeights().
		UnionBlender = MakeShared<PCGExBlending::FUnionOpsManager>(&Context->BlendingFactories, nullptr);

		TArray<TSharedRef<PCGExData::FFacade>> Sources;
		Sources.Add(VtxDataFacade);

		if (!UnionBlender->Init(Context, VtxDataFacade, Sources))
		{
			bIsProcessorValid = false;
			return;
		}

		PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, BlendDiffusions)

		BlendDiffusions->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE]()
		{
			PCGEX_ASYNC_THIS
			if (This->UnionBlender)
			{
				This->UnionBlender->Cleanup(This->Context);
			}
		};

		BlendDiffusions->OnSubLoopStartCallback = [PCGEX_ASYNC_THIS_CAPTURE, WeightMode = Settings->Weighting](const PCGExMT::FScope& Scope)
		{
			PCGEX_ASYNC_THIS

			TArray<PCGExData::FWeightedPoint> WeightedPoints;
			TArray<PCGEx::FOpStats> Trackers;
			This->UnionBlender->InitTrackers(Trackers);

			// Distance weighting reads vtx positions (source & target share the vtx facade).
			const TConstPCGValueRange<FTransform> VtxTransforms = This->VtxDataFacade->GetIn()->GetConstTransformValueRange();

			PCGEX_SCOPE_LOOP(Index)
			{
				const TArray<FContribution>& Contributors = This->VtxContributors[Index];
				const int32 Count = Contributors.Num();
				if (Count == 0) { continue; }

				WeightedPoints.Reset(Count);

				// A lone contributor is always weight 1 (mode-agnostic): a blend of one is a copy.
				if (Count == 1)
				{
					WeightedPoints.Emplace(Contributors[0].SeedVtxIndex, 1.0, 0);
					This->UnionBlender->Blend(Index, WeightedPoints, Trackers);
					continue;
				}

				switch (WeightMode)
				{
				case EPCGExClusterDiffuseWeightMode::Distance:
					{
						// Inverse-distance: w = 1 / (distSq + 1). Closer seeds weigh more; the multi-blend
						// normalizes the proportions (scale-relative for distSq >> 1). Where two diffusions
						// meet, both contributors are far so their weights converge -> smooth blend.
						const FVector TargetPos = VtxTransforms[Index].GetLocation();
						for (const FContribution& C : Contributors)
						{
							const double DistSq = FVector::DistSquared(VtxTransforms[C.SeedVtxIndex].GetLocation(), TargetPos);
							WeightedPoints.Emplace(C.SeedVtxIndex, 1.0 / (DistSq + 1.0), 0);
						}
					}
					break;

				case EPCGExClusterDiffuseWeightMode::Depth:
					{
						// Inverse-depth: w = 1 / (depth + 1). Shallower contributors weigh more; contributors
						// at similar depth (where two diffusions meet) converge to a smooth blend.
						for (const FContribution& C : Contributors)
						{
							WeightedPoints.Emplace(C.SeedVtxIndex, 1.0 / (static_cast<double>(C.Depth) + 1.0), 0);
						}
					}
					break;

				case EPCGExClusterDiffuseWeightMode::CountAndDepth:
					{
						// Half equal + half inverse-depth (each normalized within the contributor set), so
						// depth biases the result without shallow contributors dominating as in pure Depth.
						double SumInvDepth = 0;
						for (const FContribution& C : Contributors) { SumInvDepth += 1.0 / (static_cast<double>(C.Depth) + 1.0); }

						const double EqualShare = 0.5 / static_cast<double>(Count);
						const double DepthScale = SumInvDepth > 0 ? 0.5 / SumInvDepth : 0.0;
						for (const FContribution& C : Contributors)
						{
							const double InvDepth = 1.0 / (static_cast<double>(C.Depth) + 1.0);
							WeightedPoints.Emplace(C.SeedVtxIndex, EqualShare + InvDepth * DepthScale, 0);
						}
					}
					break;

				default: // Count
					for (const FContribution& C : Contributors) { WeightedPoints.Emplace(C.SeedVtxIndex, 1.0, 0); }
					break;
				}

				// Put the weights in the form the multi-blend expects: dominant contributor at weight 1,
				// and ordered first. The blender copies the first contributor as the base (ignoring its
				// weight) and only re-normalizes once the summed weight passes 1 -- so without this an
				// un-normalized low weight landing first would be applied at full strength.
				double MaxWeight = 0;
				for (const PCGExData::FWeightedPoint& P : WeightedPoints) { MaxWeight = FMath::Max(MaxWeight, P.Weight); }
				if (MaxWeight > 0)
				{
					const double InvMax = 1.0 / MaxWeight;
					for (PCGExData::FWeightedPoint& P : WeightedPoints) { P.Weight *= InvMax; }
				}
				WeightedPoints.Sort([](const PCGExData::FWeightedPoint& A, const PCGExData::FWeightedPoint& B) { return A.Weight > B.Weight; });

				This->UnionBlender->Blend(Index, WeightedPoints, Trackers);
			}
		};

		BlendDiffusions->StartSubLoops(NumVtx, PCGEX_CORE_SETTINGS.ClusterDefaultBatchChunkSize);
	}

	void FProcessor::Cleanup()
	{
		// Base resets the growth state (diffusions + fill controls handler).
		TDiffusionGrowthProcessor<FPCGExClusterDiffuseContext, UPCGExClusterDiffuseSettings>::Cleanup();
		UnionBlender.Reset();
		VtxContributors.Empty();
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

		// Vtx-participation toggle may read from a vtx attribute -- preload it.
		Settings->VtxParticipates.RegisterBufferDependencies(Context, FacadePreloader);

		for (const TObjectPtr<const UPCGExFillControlsFactoryData>& Factory : Context->FillControlFactories)
		{
			Factory->RegisterBuffersDependencies(Context, FacadePreloader);
		}
	}

	void FBatch::Process()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ClusterDiffuse)

		FillRate = Settings->Diffusion.FillRate.GetValueSetting();
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

		TypedProcessor->FillRate = FillRate;

		return true;
	}

	void FBatch::Write()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(ClusterDiffuse)

		TBatch<FProcessor>::Write();
		VtxDataFacade->WriteFastest(TaskManager);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
