// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExStagingSwap.h"

#include "PCGParamData.h"
#include "Core/PCGExAssetCollection.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Helpers/PCGExStreamingHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExStagingSwapElement"
#define PCGEX_NAMESPACE StagingSwap

void UPCGExStagingSwapSettings::InputPinPropertiesBeforeFilters(TArray<FPCGPinProperties>& PinProperties) const
{
	PCGEX_PIN_PARAM(PCGExCollections::Labels::SourceCollectionMapLabel, "Collection map information from, or merged from, Staging nodes.", Required)
	Super::InputPinPropertiesBeforeFilters(PinProperties);
}

TArray<FPCGPinProperties> UPCGExStagingSwapSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_PARAM(PCGExCollections::Labels::OutputCollectionMapLabel, "Updated collection map including the variant collections.", Normal)
	return PinProperties;
}

PCGExData::EIOInit UPCGExStagingSwapSettings::GetMainDataInitializationPolicy() const
{
	return WantsDataStealing() ? PCGExData::EIOInit::Forward : PCGExData::EIOInit::Duplicate;
}

PCGEX_INITIALIZE_ELEMENT(StagingSwap)

PCGEX_ELEMENT_BATCH_POINT_IMPL(StagingSwap)

bool FPCGExStagingSwapElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(StagingSwap)

	Context->CollectionPickUnpacker = MakeShared<PCGExCollections::FPickUnpacker>();
	Context->CollectionPickUnpacker->UnpackPin(InContext, PCGExCollections::Labels::SourceCollectionMapLabel);

	if (!Context->CollectionPickUnpacker->HasValidMapping())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Could not rebuild a valid asset mapping from the provided map."));
		return false;
	}

	const TMap<uint32, UPCGExAssetCollection*>& MappedCollections = Context->CollectionPickUnpacker->GetCollections();

	// The output map is a superset of the input map: every original collection plus the
	// variants that actually contribute mappings. Register originals up-front.
	Context->CollectionPickDatasetPacker = MakeShared<PCGExCollections::FPickPacker>(Context);
	for (const TPair<uint32, UPCGExAssetCollection*>& Pair : MappedCollections)
	{
		if (Pair.Value)
		{
			Context->CollectionPickDatasetPacker->RegisterCollection(Pair.Value);
		}
	}

	for (const TSoftObjectPtr<UPCGExVariantCollection>& VariantRef : Settings->VariantCollections)
	{
		if (VariantRef.IsNull())
		{
			continue;
		}

		PCGExHelpers::LoadBlocking_AnyThreadTpl(VariantRef);
		UPCGExVariantCollection* Variant = VariantRef.Get();

		if (!Variant)
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("A variant collection could not be loaded and was skipped."));
			continue;
		}

		// Track every listed variant (not just contributing ones): an edit may make a
		// currently-inert variant contribute, and that edit must retrigger generation.
		Variant->EDITOR_RegisterTrackingKeys(Context);

		const uint32 VariantGUID = Variant->GetCollectionGUID();
		bool bVariantContributes = false;

		for (const FPCGExVariantSource& Group : Variant->Sources)
		{
			if (Group.BakedPairs.IsEmpty())
			{
				continue;
			}

			// Only groups whose source is actually present in the incoming map matter here.
			if (!MappedCollections.Contains(Group.SourceGUIDAtBake))
			{
				// The source may be present under a different GUID (re-imported/duplicated since
				// the variant was baked) — that's a stale bake, not a missing source. Say so
				// instead of silently not swapping.
				for (const TPair<uint32, UPCGExAssetCollection*>& Pair : MappedCollections)
				{
					if (Pair.Value && FSoftObjectPath(Pair.Value) == Group.Source.ToSoftObjectPath())
					{
						PCGE_LOG(Warning, GraphAndLog, FTEXT("A variant source is present in the map but its GUID changed since the variant was baked — mapping skipped. Re-save the variant asset to refresh it."));
						break;
					}
				}
				continue;
			}

			if (Settings->bSkipStaleMappings && Variant->IsMappingStale(Group))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("A variant source mapping is stale (source collection changed since the variant was saved) and was skipped. Re-save the variant asset to refresh it."));
				continue;
			}

			for (const FIntPoint& Pair : Group.BakedPairs)
			{
				// Secondary pick (e.g. material variant) is reset: it indexed the SOURCE
				// entry's micro-cache and is meaningless against the replacement entry.
				Context->SwapMap.Add(
					PCGEx::H64(Group.SourceGUIDAtBake, Pair.X),
					PCGExCollections::PickHash::Pack(VariantGUID, static_cast<uint16>(Pair.Y)));
			}

			bVariantContributes = true;
		}

		if (bVariantContributes)
		{
			Context->CollectionPickDatasetPacker->RegisterCollection(Variant);
		}
	}

	if (Context->SwapMap.IsEmpty())
	{
		PCGE_LOG(Warning, GraphAndLog, FTEXT("No applicable variant mappings — points and map are forwarded unchanged."));
	}

	return true;
}

bool FPCGExStagingSwapElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExStagingSwapElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(StagingSwap)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartBatchProcessingPoints(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				return true;
			},
			[&](const TSharedPtr<PCGExPointsMT::IBatch>& NewBatch)
			{
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any points to process."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->MainPoints->StageOutputs();

	{
		UPCGParamData* OutputSet = Context->ManagedObjects->New<UPCGParamData>();
		Context->CollectionPickDatasetPacker->PackToDataset(OutputSet);

		FPCGTaggedData& OutData = Context->OutputData.TaggedData.Emplace_GetRef();
		OutData.Pin = PCGExCollections::Labels::OutputCollectionMapLabel;
		OutData.Data = OutputSet;
	}

	return Context->TryComplete();
}

namespace PCGExStagingSwap
{
	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!IProcessor::Process(InTaskManager))
		{
			return false;
		}

		PCGEX_INIT_IO(PointDataFacade->Source, Settings->GetMainDataInitializationPolicy())

		HashReader = PointDataFacade->GetReadable<int64>(PCGExCollections::Labels::Tag_EntryIdx, PCGExData::EIOSide::In, true);
		if (!HashReader)
		{
			return false;
		}

		HashWriter = PointDataFacade->GetWritable<int64>(PCGExCollections::Labels::Tag_EntryIdx, 0, true, PCGExData::EBufferInit::Inherit);
		if (!HashWriter)
		{
			return false;
		}

		StartParallelLoopForPoints(PCGExData::EIOSide::In);
		return true;
	}

	void FProcessor::ProcessPoints(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGEx::StagingSwap::ProcessPoints);

		PointDataFacade->Fetch(Scope);
		FilterScope(Scope);

		PCGEX_SCOPE_LOOP(Index)
		{
			if (!PointFilterCache[Index])
			{
				continue;
			}

			const uint64 Hash = static_cast<uint64>(HashReader->Read(Index));
			if (Hash == 0)
			{
				continue;
			}

			const uint32 CollectionGUID = PCGExCollections::PickHash::GetCollectionGUID(Hash);
			const uint32 RawEntryIndex = PCGExCollections::PickHash::GetRawEntryIndex(Hash);

			if (const uint64* NewHash = Context->SwapMap.Find(PCGEx::H64(CollectionGUID, RawEntryIndex)))
			{
				HashWriter->SetValue(Index, static_cast<int64>(*NewHash));
			}
		}
	}

	void FProcessor::OnPointsProcessingComplete()
	{
		PointDataFacade->WriteFastest(TaskManager);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
