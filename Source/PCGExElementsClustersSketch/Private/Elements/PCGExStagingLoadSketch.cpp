// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExStagingLoadSketch.h"

#include "PCGParamData.h"
#include "Clusters/PCGExClusterCommon.h"
#include "Collections/PCGExClusterSketchCollection.h"
#include "Data/PCGExClusterData.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Data/Utils/PCGExDataForward.h"
#include "Graphs/PCGExGraphBuilder.h"
#include "Graphs/PCGExGraphCommon.h"
#include "Graphs/PCGExGraphTasks.h"
#include "Helpers/PCGExAssetLoader.h"
#include "Helpers/PCGExCollectionsHelpers.h"
#include "Helpers/PCGExStreamingHelpers.h"
#include "Sketch/PCGExClusterSketch.h"
#include "Sketch/PCGExClusterSketchPrint.h"

#define LOCTEXT_NAMESPACE "PCGExStagingLoadSketch"
#define PCGEX_NAMESPACE StagingLoadSketch

namespace PCGExStagingLoadSketch
{
	PCGEX_CTX_STATE(State_PrintingRoots)

	/**
	 * Resolves every target's staged pick to a sketch path, deduped into UniqueSketchPaths, and leaves
	 * SketchIdx holding indices into THAT array -- AdvanceWork swaps them for UniqueSketches indices
	 * once the paths have loaded. Runs in Boot so RegisterAssetDependencies can register the paths.
	 */
	bool ResolveStagedSketches(FPCGExStagingLoadSketchContext* Context, const UPCGExStagingLoadSketchSettings* Settings)
	{
		Context->CollectionUnpacker = MakeShared<PCGExCollections::FPickUnpacker>();
		Context->CollectionUnpacker->UnpackPin(Context);

		if (!Context->CollectionUnpacker->HasValidMapping())
		{
			PCGE_LOG_C(Error, GraphAndLog, Context, FTEXT("Could not rebuild a valid asset mapping from the provided map."));
			return false;
		}

		const TSharedPtr<PCGExData::TBuffer<int64>> HashGetter =
			Context->TargetsDataFacade->GetReadable<int64>(Settings->GetEntryIdxAttributeName(), PCGExData::EIOSide::In, true);

		if (!HashGetter)
		{
			PCGE_LOG_C(Error, GraphAndLog, Context, FTEXT("Missing staging hash attribute. Make sure points were staged with Collection Map output."));
			return false;
		}

		TMap<FSoftObjectPath, int32> PathToIndex;
		const int32 NumTargets = Context->SketchIdx.Num();

		for (int32 i = 0; i < NumTargets; ++i)
		{
			const int64 Hash = HashGetter->Read(i);
			if (Hash == 0 || Hash == -1)
			{
				continue;
			}

			// Ignored: secondary picks only ever come from mesh micro caches.
			int16 SecondaryIndex = 0;
			const FPCGExEntryAccessResult Result = Context->CollectionUnpacker->ResolveEntry(Hash, SecondaryIndex);
			if (!Result.IsValid())
			{
				++Context->NumUnresolvedTargets;
				continue;
			}

			// Expected traffic in a mixed host: a staged mesh/actor/level pick flowing past a sketch
			// node is not an error, so it is skipped without counting as unresolved.
			if (!Result.Entry->IsType(PCGExSketch::CollectionTypeId))
			{
				continue;
			}

			const FSoftObjectPath Path = static_cast<const FPCGExClusterSketchCollectionEntry*>(Result.Entry)->Sketch.ToSoftObjectPath();
			if (Path.IsNull())
			{
				++Context->NumUnresolvedTargets;
				continue;
			}

			if (const int32* Existing = PathToIndex.Find(Path))
			{
				Context->SketchIdx[i] = *Existing;
				continue;
			}

			const int32 NewIndex = Context->UniqueSketchPaths.Add(Path);
			PathToIndex.Add(Path, NewIndex);
			Context->SketchIdx[i] = NewIndex;
		}

		if (Context->UniqueSketchPaths.IsEmpty())
		{
			PCGE_LOG_C(Error, GraphAndLog, Context, FTEXT("No Cluster Sketch entry could be resolved from the staged targets."));
			return false;
		}

		return true;
	}
}

#pragma region UPCGExStagingLoadSketchSettings

void UPCGExStagingLoadSketchSettings::InputPinPropertiesBeforeFilters(TArray<FPCGPinProperties>& PinProperties) const
{
	if (Source == EPCGExClusterSketchSource::CollectionMap)
	{
		PCGEX_PIN_PARAMS(PCGExCollections::Labels::SourceCollectionMapLabel, "Collection map information from staging nodes.", Required)
	}
	Super::InputPinPropertiesBeforeFilters(PinProperties);
}

TArray<FPCGPinProperties> UPCGExStagingLoadSketchSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExClusters::Labels::OutputEdgesLabel, "Point data representing edges.", Required)
	return PinProperties;
}

#pragma endregion

#pragma region FPCGExStagingLoadSketchContext

void FPCGExStagingLoadSketchContext::RegisterAssetDependencies()
{
	FPCGExPointsProcessorContext::RegisterAssetDependencies();

	const UPCGExStagingLoadSketchSettings* Settings = GetInputSettings<UPCGExStagingLoadSketchSettings>();
	if (!Settings)
	{
		return;
	}

	// Runs AFTER Boot, so the CollectionMap paths Boot resolved go through the normal async load
	// phase -- no blocking load, same as the constant path.
	if (Settings->Source == EPCGExClusterSketchSource::CollectionMap)
	{
		for (const FSoftObjectPath& Path : UniqueSketchPaths)
		{
			AddAssetDependency(Path);
		}
		return;
	}

	if (Settings->Sketch.Input == EPCGExInputValueType::Constant)
	{
		if (Settings->Sketch.Constant.IsValid())
		{
			AddAssetDependency(Settings->Sketch.Constant);
		}
	}
	else if (SketchLoader)
	{
		SketchLoader->AddAssetDependencies();
	}
}

#pragma endregion

#pragma region FPCGExStagingLoadSketchElement

PCGEX_INITIALIZE_ELEMENT(StagingLoadSketch)

bool FPCGExStagingLoadSketchElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(StagingLoadSketch)

	if (Context->MainPoints->Pairs.IsEmpty())
	{
		PCGEX_LOG_MISSING_INPUT(Context, FTEXT("Missing targets."))
		return false;
	}

	Context->TargetsDataFacade = MakeShared<PCGExData::FFacade>(Context->MainPoints->Pairs[0].ToSharedRef());

	PCGEX_FWD(GraphBuilderDetails)

	PCGEX_FWD(TransformDetails)
	if (!Context->TransformDetails.Init(Context, Context->TargetsDataFacade.ToSharedRef()))
	{
		return false;
	}

	PCGEX_FWD(TargetsAttributesToClusterTags)
	if (!Context->TargetsAttributesToClusterTags.Init(Context, Context->TargetsDataFacade))
	{
		return false;
	}

	Context->TargetsForwardHandler = Settings->TargetsForwarding.GetHandler(Context->TargetsDataFacade);

	Context->SketchIdx.Init(-1, Context->MainPoints->Pairs[0]->GetNum());

	if (Settings->Source == EPCGExClusterSketchSource::CollectionMap)
	{
		if (!PCGExStagingLoadSketch::ResolveStagedSketches(Context, Settings))
		{
			return false;
		}
	}
	else if (Settings->Sketch.Input == EPCGExInputValueType::Attribute)
	{
		// Attribute-driven sketches resolve through the shared asset loader, which discovers every unique
		// path now and hands them to the context's normal asset-loading phase.
		PCGEX_VALIDATE_NAME_CONSUMABLE(Settings->Sketch.Attribute)

		TArray<FName> Names = {Settings->Sketch.Attribute};
		Context->SketchLoader = MakeShared<PCGEx::TAssetLoader<UPCGExClusterSketch>>(Context, Context->MainPoints.ToSharedRef(), Names);
		if (!Context->SketchLoader->Discover())
		{
			return Context->CancelExecution(TEXT("Failed to find any Cluster Sketch to load."));
		}
	}
	else if (!Settings->Sketch.Constant.IsValid())
	{
		return Context->CancelExecution(TEXT("Invalid Cluster Sketch constant."));
	}

	Context->RootVtx = MakeShared<PCGExData::FPointIOCollection>(Context); // Pinless: roots are never staged

	Context->VtxChildCollection = MakeShared<PCGExData::FPointIOCollection>(Context);
	Context->VtxChildCollection->OutputPin = Settings->GetMainOutputPin();

	Context->EdgeChildCollection = MakeShared<PCGExData::FPointIOCollection>(Context);
	Context->EdgeChildCollection->OutputPin = PCGExClusters::Labels::OutputEdgesLabel;

	return true;
}

void FPCGExStagingLoadSketchElement::PostLoadAssetsDependencies(FPCGExContext* InContext) const
{
	FPCGExPointsProcessorElement::PostLoadAssetsDependencies(InContext);

	PCGEX_CONTEXT_AND_SETTINGS(StagingLoadSketch)

	// CollectionMap resolves its paths in AdvanceWork; a stale Sketch constant left over from Asset
	// mode must not be picked up here.
	if (Settings->Source == EPCGExClusterSketchSource::CollectionMap)
	{
		return;
	}

	if (Context->SketchLoader)
	{
		Context->SketchLoader->Finalize();
	}
	else if (Settings->Sketch.Constant.IsValid())
	{
		Context->ConstantSketch = TSoftObjectPtr<UPCGExClusterSketch>(Settings->Sketch.Constant).Get();
	}
}

bool FPCGExStagingLoadSketchElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExStagingLoadSketchElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(StagingLoadSketch)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		Context->AdvancePointsIO();

		const int32 NumTargets = Context->SketchIdx.Num();

		// --- Resolve each target's sketch, deduplicated into UniqueSketches ---
		TMap<TObjectPtr<UPCGExClusterSketch>, int32> SketchToIndex;
		int32 NumUnresolved = Context->NumUnresolvedTargets;

		auto ResolveIndex = [&](UPCGExClusterSketch* InSketch) -> int32
		{
			if (!InSketch)
			{
				return -1;
			}
			if (const int32* Existing = SketchToIndex.Find(InSketch))
			{
				return *Existing;
			}
			const int32 NewIndex = Context->UniqueSketches.Add(InSketch);
			SketchToIndex.Add(InSketch, NewIndex);
			return NewIndex;
		};

		if (Settings->Source == EPCGExClusterSketchSource::CollectionMap)
		{
			// Boot left SketchIdx indexing UniqueSketchPaths; the paths have loaded by now, so swap
			// them for UniqueSketches indices through the same dedupe every other source uses.
			TArray<int32> PathToSketch;
			PathToSketch.Reserve(Context->UniqueSketchPaths.Num());
			for (const FSoftObjectPath& Path : Context->UniqueSketchPaths)
			{
				PathToSketch.Add(ResolveIndex(TSoftObjectPtr<UPCGExClusterSketch>(Path).Get()));
			}

			for (int32 i = 0; i < NumTargets; ++i)
			{
				const int32 PathIndex = Context->SketchIdx[i];
				if (PathIndex == -1)
				{
					// Never referenced a sketch entry -- already accounted for in Boot.
					continue;
				}

				Context->SketchIdx[i] = PathToSketch[PathIndex];
				if (Context->SketchIdx[i] == -1)
				{
					++NumUnresolved;
				}
			}
		}
		else if (Context->SketchLoader)
		{
			const TSharedPtr<TArray<PCGExValueHash>> Keys = Context->SketchLoader->GetKeys(Context->CurrentIO->IOIndex);
			for (int32 i = 0; i < NumTargets; ++i)
			{
				UPCGExClusterSketch* Resolved = nullptr;
				if (Keys && Keys->IsValidIndex(i))
				{
					if (const TObjectPtr<UPCGExClusterSketch>* Found = Context->SketchLoader->GetAsset((*Keys)[i]))
					{
						Resolved = Found->Get();
					}
				}
				Context->SketchIdx[i] = ResolveIndex(Resolved);
				if (Context->SketchIdx[i] == -1)
				{
					++NumUnresolved;
				}
			}
		}
		else
		{
			const int32 ConstantIndex = ResolveIndex(Context->ConstantSketch);
			if (ConstantIndex == -1)
			{
				return Context->CancelExecution(TEXT("Cluster Sketch constant could not be loaded."));
			}
			for (int32& Index : Context->SketchIdx)
			{
				Index = ConstantIndex;
			}
		}

		if (NumUnresolved > 0 && !Settings->bQuiet)
		{
			PCGE_LOG(Warning, GraphAndLog, FText::Format(
				         FTEXT("{0} target(s) have no valid Cluster Sketch and were skipped."),
				         FText::AsNumber(NumUnresolved)));
		}

		if (Context->UniqueSketches.IsEmpty())
		{
			return Context->CancelExecution(TEXT("No Cluster Sketch could be resolved from the targets."));
		}

		// Dynamic tracking: editing a printed sketch -- or anything it references -- re-executes the
		// component. TAssetLoader does no tracking of its own, and a per-point attribute path is
		// otherwise invisible to the tracker (only a constant would ever be seen).
		TArray<FSoftObjectPath> NestedDependencies;
		for (const TObjectPtr<UPCGExClusterSketch>& Sketch : Context->UniqueSketches)
		{
			Context->EDITOR_TrackPath(FSoftObjectPath(Sketch));
			Sketch->CollectAssetDependencies(NestedDependencies);
		}

		// A sketch's own soft references (snap provider, decorators) are only knowable once the sketches
		// themselves are loaded, i.e. after the context's asset phase -- so they load here, once, before
		// any print reads them.
		if (!NestedDependencies.IsEmpty())
		{
			const TSharedPtr<TSet<FSoftObjectPath>> UniqueNested = MakeShared<TSet<FSoftObjectPath>>(NestedDependencies);
			for (const FSoftObjectPath& NestedPath : *UniqueNested)
			{
				Context->EDITOR_TrackPath(NestedPath);
			}
			PCGExHelpers::LoadBlocking_AnyThread(UniqueNested, Context);
		}

		// --- Print one shared root per unique sketch ---
		const int32 NumUnique = Context->UniqueSketches.Num();
		Context->GraphBuilders.Init(nullptr, NumUnique);
		Context->PrintContexts.Init(nullptr, NumUnique);

		const TSharedPtr<PCGExMT::FTaskManager> TaskManager = Context->GetTaskManager();
		PCGEX_ASYNC_SCHEDULING_SCOPE_BODY(TaskManager)
		{
			return Context->CancelExecution(TEXT(""));
		}

		for (int32 i = 0; i < NumUnique; ++i)
		{
			const TSharedPtr<PCGExData::FPointIO> RootIO = Context->RootVtx->Emplace_GetRef<UPCGExClusterNodesData>();
			if (!RootIO)
			{
				return Context->CancelExecution(TEXT(""));
			}

			Context->PrintContexts[i] = MakeShared<FPCGExClusterSketchPrintContext>();
			// The asset assembles its own print request (snap provider + decorators) -- consumers only
			// ever hand it an IO and receive the finished pair.
			Context->GraphBuilders[i] = Context->UniqueSketches[i]->Print(
				Context, RootIO, TaskManager, Context->PrintContexts[i],
				&Context->GraphBuilderDetails, Settings->bQuiet);
		}

		Context->SetState(PCGExStagingLoadSketch::State_PrintingRoots);
	}

	PCGEX_ON_ASYNC_STATE_READY(PCGExStagingLoadSketch::State_PrintingRoots)
	{
		Context->SetState(PCGExGraphs::States::State_WritingClusters);

		const TSharedPtr<PCGExMT::FTaskManager> TaskManager = Context->GetTaskManager();
		PCGEX_ASYNC_SCHEDULING_SCOPE_BODY(TaskManager)
		{
			return Context->CancelExecution(TEXT(""));
		}

		const int32 NumTargets = Context->SketchIdx.Num();
		for (int32 i = 0; i < NumTargets; ++i)
		{
			const int32 SketchIndex = Context->SketchIdx[i];
			if (SketchIndex == -1 || !Context->GraphBuilders.IsValidIndex(SketchIndex))
			{
				continue;
			}
			PCGEX_LAUNCH(
				PCGExGraphTask::FCopyGraphToPoint, i, Context->CurrentIO, Context->GraphBuilders[SketchIndex],
				Context->VtxChildCollection, Context->EdgeChildCollection, &Context->TransformDetails,
				&Context->TargetsAttributesToClusterTags, Context->TargetsForwardHandler)
		}
	}

	PCGEX_ON_ASYNC_STATE_READY(PCGExGraphs::States::State_WritingClusters)
	{
		Context->VtxChildCollection->StageOutputs();
		Context->EdgeChildCollection->StageOutputs();
		Context->Done();
	}

	return Context->TryComplete();
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
