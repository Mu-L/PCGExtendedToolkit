// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExClusterDiffuse.h"

#include "PCGExVersion.h"
#include "Blenders/PCGExUnionOpsManager.h"
#include "Clusters/PCGExCluster.h"
#include "Core/PCGExBlendOpsManager.h"
#include "Core/PCGExBlendOpsSchema.h"
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
	PCGEX_PIN_FACTORIES(PCGExClusterDiffuse::SourceSeedBlendingLabel, "Blend operations that blend seed attributes (read from the seeds point cloud) onto reached vtx.", Normal, FPCGExDataTypeInfoBlendOp::AsId())

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

	// Seed Blend Ops (Layer 2) are optional -- they blend attributes from the seeds point cloud.
	PCGExFactories::GetInputFactories<UPCGExBlendOpFactory>(Context, PCGExClusterDiffuse::SourceSeedBlendingLabel, Context->SeedBlendingFactories, {FPCGExDataTypeInfoBlendOp::AsId()}, false);

	// Fill controls are optional
	PCGExFactories::GetInputFactories<UPCGExFillControlsFactoryData>(Context, PCGExFloodFill::SourceFillControlsLabel, Context->FillControlFactories, {FPCGExDataTypeInfoFillControl::AsId()}, false);

	Context->SeedsDataFacade = PCGExData::TryGetSingleFacade(Context, PCGExCommon::Labels::SourceSeedsLabel, false, true);
	if (!Context->SeedsDataFacade)
	{
		return false;
	}

	// Per-seed factor reads from the shared seeds facade -- resolve + warm it once, single-threaded,
	// so the parallel blend pass only reads. Non-scoped (full): the blend reads arbitrary seed indices.
	Context->SeedFactorValue = Settings->SeedFactor.GetValueSetting();
	if (!Context->SeedFactorValue->Init(Context->SeedsDataFacade, false))
	{
		// Invalid factor attribute -- ignore the factor rather than fail the node.
		Context->SeedFactorValue = nullptr;
	}

	if (!Context->SeedBlendingFactories.IsEmpty())
	{
		// Warm the seeds-cloud attributes (full, non-scoped reads) and resolve the seed blend configs
		// once -- single-threaded here -- so per-processor seed blending never enumerates metadata or
		// creates cold reads concurrently on the shared seeds facade.
		TArray<PCGExData::FAttributeIdentity> SeedIdentities;
		PCGExBlending::GetFilteredIdentities(Context->SeedsDataFacade->GetIn()->Metadata, SeedIdentities);
		Context->SeedsDataFacade->CreateReadables(SeedIdentities, false);

		Context->SeedBlendOpsSchema = MakeShared<PCGExBlending::FBlendOpsSchema>();
		if (!Context->SeedBlendOpsSchema->Init(Context, Context->SeedBlendingFactories, {Context->SeedsDataFacade.ToSharedRef()}))
		{
			return false;
		}
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

		// Nothing to blend without any blend operations -- the vtx pass through unchanged.
		if (Context->BlendingFactories.IsEmpty() && Context->SeedBlendingFactories.IsEmpty())
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
			// Per-diffusion falloff normalizers (0 at the seed, 1 at the diffusion's furthest reach).
			const int32 DiffMaxDepth = Diffusion->GetMaxDepth();
			const double DiffMaxDist = Diffusion->GetMaxDistance();
			const double InvMaxDepth = DiffMaxDepth > 0 ? 1.0 / static_cast<double>(DiffMaxDepth) : 0.0;
			const double InvMaxDist = DiffMaxDist > 0 ? 1.0 / DiffMaxDist : 0.0;
			for (const PCGExFloodFill::FCandidate& Candidate : Diffusion->Captured)
			{
				const double NormDepth = static_cast<double>(Candidate.Depth) * InvMaxDepth;
				const double NormDist = Candidate.PathDistance * InvMaxDist;
				VtxContributors[Candidate.Node->PointIndex].Add(FContribution{SeedVtx, SeedPoint, Candidate.Depth, NormDepth, NormDist});
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
				if (!bSelfPresent) { Contributors.Add(FContribution{VtxIndex, -1, 0, 0.0, 0.0}); }
			}
		}

		// Layer 1 (Vtx Blend Ops): source & target are the vtx facade. Layer 2 (Seed Blend Ops): source 0
		// (primary) is the shared seeds facade (thread-safe via the Boot-resolved schema); source 1
		// (background) is this processor's own vtx facade, so the participation self-entry can fade reached
		// vtx back toward their original value -- exactly as Layer 1 does. Both target the vtx facade.
		// Distances is unused -- weights are fed to Blend() directly. Either layer is optional.
		if (!Context->BlendingFactories.IsEmpty())
		{
			TArray<TSharedRef<PCGExData::FFacade>> VtxSources;
			VtxSources.Add(VtxDataFacade);

			VtxBlender = MakeShared<PCGExBlending::FUnionOpsManager>(&Context->BlendingFactories, nullptr);
			if (!VtxBlender->Init(Context, VtxDataFacade, VtxSources))
			{
				bIsProcessorValid = false;
				return;
			}
		}

		if (!Context->SeedBlendingFactories.IsEmpty())
		{
			// Background source (vtx) is resolved on the spot (per-processor, unique -> thread-safe) and
			// tolerates attributes the vtx doesn't carry: seed-only attributes simply have no original to
			// fade to and fall back toward 0, while attributes the vtx already holds fade to their original.
			TArray<TSharedRef<PCGExData::FFacade>> SeedSources;
			SeedSources.Add(Context->SeedsDataFacade.ToSharedRef());
			SeedSources.Add(VtxDataFacade);

			SeedBlender = MakeShared<PCGExBlending::FUnionOpsManager>(&Context->SeedBlendingFactories, nullptr);
			if (!SeedBlender->Init(Context, VtxDataFacade, SeedSources, Context->SeedBlendOpsSchema, /*NumTrailingBackgroundSources=*/1))
			{
				bIsProcessorValid = false;
				return;
			}
		}

		PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, BlendDiffusions)

		BlendDiffusions->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE]()
		{
			PCGEX_ASYNC_THIS
			if (This->VtxBlender) { This->VtxBlender->Cleanup(This->Context); }
			if (This->SeedBlender) { This->SeedBlender->Cleanup(This->Context); }
		};

		BlendDiffusions->OnSubLoopStartCallback = [PCGEX_ASYNC_THIS_CAPTURE, WeightMode = Settings->Weighting, WeightSpace = Settings->WeightSpace](const PCGExMT::FScope& Scope)
		{
			PCGEX_ASYNC_THIS

			TArray<double> Weights;
			TArray<int32> Order;
			TArray<PCGExData::FWeightedPoint> WeightedPoints;
			TArray<PCGEx::FOpStats> VtxTrackers;
			TArray<PCGEx::FOpStats> SeedTrackers;
			if (This->VtxBlender) { This->VtxBlender->InitTrackers(VtxTrackers); }
			if (This->SeedBlender) { This->SeedBlender->InitTrackers(SeedTrackers); }

			// Relative distance weighting reads vtx positions.
			const TConstPCGValueRange<FTransform> VtxTransforms = This->VtxDataFacade->GetIn()->GetConstTransformValueRange();
			const TSharedPtr<PCGExDetails::TSettingValue<double>>& SeedFactor = This->Context->SeedFactorValue;

			PCGEX_SCOPE_LOOP(Index)
			{
				const TArray<FContribution>& Contributors = This->VtxContributors[Index];
				const int32 Count = Contributors.Num();
				if (Count == 0) { continue; }

				Weights.Reset(Count);
				Order.Reset(Count);
				for (int32 i = 0; i < Count; i++) { Order.Add(i); }

				if (WeightSpace == EPCGExClusterDiffuseWeightSpace::Falloff)
				{
					// Absolute intensity: 1 at the seed, fading to 0 at that diffusion's furthest reach,
					// scaled by the per-seed factor. No normalization -- the multi-blend composites the
					// (un-normalized) intensities and caps once they exceed 1. Where the seeds don't fill a
					// vtx, the participation self-entry supplies the remainder (fades to original); without
					// it, the vtx fades toward 0.
					double SumSeed = 0;
					int32 SelfIndex = -1;
					for (int32 i = 0; i < Count; i++)
					{
						const FContribution& C = Contributors[i];
						if (C.SeedPointIndex < 0) { SelfIndex = i; Weights.Add(0.0); continue; }

						double Intensity;
						switch (WeightMode)
						{
						case EPCGExClusterDiffuseWeightMode::Distance:      Intensity = 1.0 - C.NormDist; break;
						case EPCGExClusterDiffuseWeightMode::Depth:         Intensity = 1.0 - C.NormDepth; break;
						case EPCGExClusterDiffuseWeightMode::CountAndDepth: Intensity = 0.5 + 0.5 * (1.0 - C.NormDepth); break;
						default:                                            Intensity = 1.0; break; // Count: flat full
						}
						Intensity = FMath::Clamp(Intensity, 0.0, 1.0);
						if (SeedFactor) { Intensity *= FMath::Max(0.0, SeedFactor->Read(C.SeedPointIndex)); }

						Weights.Add(Intensity);
						SumSeed += Intensity;
					}

					// Background fill: the vtx's own value takes whatever intensity the seeds leave.
					if (SelfIndex >= 0) { Weights[SelfIndex] = FMath::Max(0.0, 1.0 - SumSeed); }

					// No normalization, no reorder -- absolute weights are fed as-is (the participation
					// self-entry sits at Order's natural position, which is fine for accumulate-from-zero blends).
				}
				else
				{
					// Relative: seeds compete territorially -- the strongest SEED reaches full intensity, so a lone seed
					// fully claims its vtx. The participation self-entry (the vtx original value) is NOT weighted by its
					// own zero distance/depth (which pinned it to the maximum and let it swallow the seeds); instead it
					// joins as a peer to the strongest seed, and Seed Factor tilts the balance (> 1 favours seeds, < 1 the
					// original). With participation off, these steps reduce exactly to the legacy raw -> factor -> normalize.
					if (Count == 1)
					{
						// A lone seed (participation never yields a count of one) is always weight 1: a blend of one is a copy.
						Weights.Add(1.0);
					}
					else
					{
						// 1. Raw seed weights (territorial competition). The self-entry gets 0 here; step 4 promotes it to a peer.
						switch (WeightMode)
						{
						case EPCGExClusterDiffuseWeightMode::Distance:
							{
								// Inverse-distance: closer seeds weigh more; where two diffusions meet both are
								// far, so weights converge -> smooth blend.
								const FVector TargetPos = VtxTransforms[Index].GetLocation();
								for (const FContribution& C : Contributors)
								{
									if (C.SeedPointIndex < 0) { Weights.Add(0.0); continue; }
									const double DistSq = FVector::DistSquared(VtxTransforms[C.SeedVtxIndex].GetLocation(), TargetPos);
									Weights.Add(1.0 / (DistSq + 1.0));
								}
							}
							break;
						case EPCGExClusterDiffuseWeightMode::Depth:
							for (const FContribution& C : Contributors) { Weights.Add(C.SeedPointIndex < 0 ? 0.0 : 1.0 / (static_cast<double>(C.Depth) + 1.0)); }
							break;
						case EPCGExClusterDiffuseWeightMode::CountAndDepth:
							{
								double SumInvDepth = 0;
								int32 NumSeeds = 0;
								for (const FContribution& C : Contributors) { if (C.SeedPointIndex < 0) { continue; } SumInvDepth += 1.0 / (static_cast<double>(C.Depth) + 1.0); ++NumSeeds; }
								const double EqualShare = NumSeeds > 0 ? 0.5 / static_cast<double>(NumSeeds) : 0.0;
								const double DepthScale = SumInvDepth > 0 ? 0.5 / SumInvDepth : 0.0;
								for (const FContribution& C : Contributors) { Weights.Add(C.SeedPointIndex < 0 ? 0.0 : EqualShare + (1.0 / (static_cast<double>(C.Depth) + 1.0)) * DepthScale); }
							}
							break;
						default: // Count
							for (int32 i = 0; i < Count; i++) { Weights.Add(Contributors[i].SeedPointIndex < 0 ? 0.0 : 1.0); }
							break;
						}

						// 2. Normalize so the strongest SEED reaches full intensity (territorial among seeds; self-entry excluded).
						double MaxSeed = 0;
						for (int32 i = 0; i < Count; i++) { if (Contributors[i].SeedPointIndex >= 0) { MaxSeed = FMath::Max(MaxSeed, Weights[i]); } }
						if (MaxSeed > 0)
						{
							const double InvMaxSeed = 1.0 / MaxSeed;
							for (int32 i = 0; i < Count; i++) { if (Contributors[i].SeedPointIndex >= 0) { Weights[i] *= InvMaxSeed; } }
						}
					}

					// 3. Seed Factor tilts seeds against the (fixed) original, and biases seed-vs-seed for per-seed factors.
					if (SeedFactor)
					{
						for (int32 i = 0; i < Count; i++)
						{
							const int32 SeedPt = Contributors[i].SeedPointIndex;
							if (SeedPt >= 0) { Weights[i] *= FMath::Max(0.0, SeedFactor->Read(SeedPt)); }
						}
					}

					// 4. The original participates as a peer to the strongest seed (equal at Seed Factor 1).
					for (int32 i = 0; i < Count; i++) { if (Contributors[i].SeedPointIndex < 0) { Weights[i] = 1.0; } }

					// 5. Renormalize to max = 1 (preserves the seed/original ratio; orders dominant-first, as the multi-blend
					// copies the first as its base and only re-normalizes once the summed weight passes 1).
					double MaxWeight = 0;
					for (const double W : Weights) { MaxWeight = FMath::Max(MaxWeight, W); }
					if (MaxWeight > 0)
					{
						const double InvMax = 1.0 / MaxWeight;
						for (double& W : Weights) { W *= InvMax; }
					}
					Order.Sort([&Weights](const int32 A, const int32 B) { return Weights[A] > Weights[B]; });
				}

				// Layer 1: blend seed VTX values onto this vtx.
				if (This->VtxBlender)
				{
					WeightedPoints.Reset(Count);
					for (const int32 O : Order) { WeightedPoints.Emplace(Contributors[O].SeedVtxIndex, Weights[O], 0); }
					This->VtxBlender->Blend(Index, WeightedPoints, VtxTrackers);
				}

				// Layer 2: blend seeds-cloud values onto this vtx. Seed contributors read from the seeds
				// cloud (source 0); the participation self-entry (no seeds-cloud point) reads this vtx's own
				// original value from the background source (source 1), so reached vtx fade back toward their
				// original where the seeds leave intensity -- mirroring Layer 1.
				if (This->SeedBlender)
				{
					WeightedPoints.Reset(Count);
					for (const int32 O : Order)
					{
						const FContribution& C = Contributors[O];
						if (C.SeedPointIndex < 0) { WeightedPoints.Emplace(C.SeedVtxIndex, Weights[O], 1); }
						else { WeightedPoints.Emplace(C.SeedPointIndex, Weights[O], 0); }
					}
					if (!WeightedPoints.IsEmpty()) { This->SeedBlender->Blend(Index, WeightedPoints, SeedTrackers); }
				}
			}
		};

		BlendDiffusions->StartSubLoops(NumVtx, PCGEX_CORE_SETTINGS.ClusterDefaultBatchChunkSize);
	}

	void FProcessor::Cleanup()
	{
		// Base resets the growth state (diffusions + fill controls handler).
		TDiffusionGrowthProcessor<FPCGExClusterDiffuseContext, UPCGExClusterDiffuseSettings>::Cleanup();
		VtxBlender.Reset();
		SeedBlender.Reset();
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
