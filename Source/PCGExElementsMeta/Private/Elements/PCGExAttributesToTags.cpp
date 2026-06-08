// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExAttributesToTags.h"

#include "PCGContext.h"
#include "PCGParamData.h"
#include "Containers/PCGExManagedObjects.h"
#include "Core/PCGExPickerFactoryProvider.h"
#include "Data/PCGExDataTags.h"
#include "Data/PCGExPointElements.h"
#include "Factories/PCGExFactories.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Metadata/PCGMetadata.h"

#define LOCTEXT_NAMESPACE "PCGExAttributesToTagsElement"
#define PCGEX_NAMESPACE AttributesToTags

#if WITH_EDITOR
TArray<FText> UPCGExAttributesToTagsSettings::GetNodeTitleAliases() const
{
	return {FTEXT("PCGEx | Hoist Attributes")};
}
#endif

TArray<FPCGPinProperties> UPCGExAttributesToTagsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_ANY(PCGPinConstants::DefaultInputLabel, "The data to be processed.", Required)

	if (Resolution != EPCGExAttributeToTagsResolution::Self)
	{
		PCGEX_PIN_ANY(FName("Tags Source"), "Source collection(s) to read the tags from.", Required)
	}

	if (Selection == EPCGExCollectionEntrySelection::Picker || Selection == EPCGExCollectionEntrySelection::PickerFirst || Selection == EPCGExCollectionEntrySelection::PickerLast)
	{
		PCGEX_PIN_FACTORIES(PCGExPickers::Labels::SourcePickersLabel, "Pickers config", Required, FPCGExDataTypeInfoPicker::AsId())
	}

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExAttributesToTagsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;

	if (Action != EPCGExAttributeToTagsAction::Attribute)
	{
		PCGEX_PIN_ANY(PCGPinConstants::DefaultOutputLabel, "The processed input.", Normal)
	}
	else
	{
		PCGEX_PIN_PARAMS(FName("Tags"), "Tags value in the format `AttributeName = AttributeName:AttributeValue`", Required)
	}

	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(AttributesToTags)

namespace PCGExAttributesToTags
{
	// One valid (non-empty) input on a pin: a pointer into the caller's stable TaggedData array plus its
	// cached row count, so callers never recompute the count. The source TArray must outlive these entries.
	struct FValidInput
	{
		const FPCGTaggedData* Tagged = nullptr;
		int32 NumRows = 0;
	};

	// Filter a pin's inputs to those with non-null, non-empty data, preserving pin order. `Inputs` must
	// outlive `OutValid` (entries hold pointers into it).
	void GatherValidInputs(const TArray<FPCGTaggedData>& Inputs, TArray<FValidInput>& OutValid)
	{
		OutValid.Reserve(Inputs.Num());
		for (const FPCGTaggedData& TaggedData : Inputs)
		{
			if (!TaggedData.Data) { continue; }
			const int32 NumRows = PCGExMetaHelpers::GetElementsCount(TaggedData.Data);
			if (NumRows > 0) { OutValid.Add({&TaggedData, NumRows}); }
		}
	}

	// Build the tag-detail reader for one source. Shared by every resolution so the field setup lives in one
	// place. Returns false if the underlying broadcasters can't be initialized.
	bool InitDetails(const FPCGExContext* InContext, const bool bPrefixWithAttributeName, const TArray<FPCGAttributePropertyInputSelector>& Attributes, const UPCGData* SourceData, FPCGExAttributeToTagDetails& OutDetails)
	{
		OutDetails.bAddIndexTag = false;
		OutDetails.bPrefixWithAttributeName = bPrefixWithAttributeName;
		OutDetails.Attributes = Attributes;
		return OutDetails.Init(InContext, SourceData);
	}

	// Promote every picked row into the given tag target (a tag set, or @Data metadata). Centralizes the
	// "row index -> Tag" loop shared by all three actions; Tag() only reads the row index.
	template <typename TTarget>
	void PromoteAll(const FPCGExAttributeToTagDetails& Details, const TArray<int32>& Indices, TTarget&& Target)
	{
		PCGExData::FConstPoint TagSource;
		for (const int32 Idx : Indices)
		{
			TagSource.Index = Idx;
			Details.Tag(TagSource, Target);
		}
	}

	// Resolve the row indices to promote, honoring the selection mode. Every mode indexes into the source's
	// own row count. Picker emits all picks ascending; PickerFirst/PickerLast emit the single lowest/highest
	// valid pick via an order-independent min/max scan (no full sort). -1 is the picker out-of-bounds
	// sentinel and is skipped.
	void ResolveIndices(
		const EPCGExCollectionEntrySelection Selection,
		const int32 SourceNum,
		const int32 Seed,
		const TArray<TObjectPtr<const UPCGExPickerFactoryData>>& PickerFactories,
		TArray<int32>& OutIndices)
	{
		if (SourceNum <= 0) { return; }

		switch (Selection)
		{
		case EPCGExCollectionEntrySelection::FirstIndex:
			OutIndices.Add(0);
			break;
		case EPCGExCollectionEntrySelection::LastIndex:
			OutIndices.Add(SourceNum - 1);
			break;
		case EPCGExCollectionEntrySelection::RandomIndex:
			{
				const FRandomStream RandomSource(Seed);
				OutIndices.Add(RandomSource.RandRange(0, SourceNum - 1));
			}
			break;
		case EPCGExCollectionEntrySelection::Picker:
		case EPCGExCollectionEntrySelection::PickerFirst:
		case EPCGExCollectionEntrySelection::PickerLast:
			{
				TSet<int32> UniqueIndices;
				for (const TObjectPtr<const UPCGExPickerFactoryData>& Picker : PickerFactories)
				{
					Picker->AddPicks(SourceNum, UniqueIndices);
				}

				if (Selection == EPCGExCollectionEntrySelection::Picker)
				{
					TArray<int32> Sorted = UniqueIndices.Array();
					Sorted.Sort();
					for (const int32 Idx : Sorted) { if (Idx != -1) { OutIndices.Add(Idx); } }
				}
				else
				{
					// PickerFirst = lowest valid pick, PickerLast = highest -- single order-independent scan.
					int32 Best = INDEX_NONE;
					for (const int32 Idx : UniqueIndices)
					{
						if (Idx == -1) { continue; }
						if (Best == INDEX_NONE) { Best = Idx; }
						else if (Selection == EPCGExCollectionEntrySelection::PickerFirst) { Best = FMath::Min(Best, Idx); }
						else { Best = FMath::Max(Best, Idx); }
					}
					if (Best != INDEX_NONE) { OutIndices.Add(Best); }
				}
			}
			break;
		}
	}
}

bool FPCGExAttributesToTagsElement::Boot(FPCGExContext* InContext) const
{
	if (!IPCGExElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(AttributesToTags)

	Context->Attributes = Settings->Attributes;
	PCGExMetaHelpers::AppendUniqueSelectorsFromCommaSeparatedList(Settings->CommaSeparatedAttributeSelectors, Context->Attributes);

	// Pickers are valid for every resolution -- including Self (pick rows of the input itself) -- so load
	// them before the Self short-circuit below.
	if (Settings->Selection == EPCGExCollectionEntrySelection::Picker || Settings->Selection == EPCGExCollectionEntrySelection::PickerFirst || Settings->Selection == EPCGExCollectionEntrySelection::PickerLast)
	{
		if (!PCGExFactories::GetInputFactories(Context, PCGExPickers::Labels::SourcePickersLabel, Context->PickerFactories, {PCGExFactories::EType::IndexPicker}))
		{
			return false;
		}
	}

	if (Settings->Resolution == EPCGExAttributeToTagsResolution::Self)
	{
		return true;
	}

	// Cross-collection: resolve the "Tags Source" inputs (non-null, non-empty), preserving pin order.
	TArray<PCGExAttributesToTags::FValidInput> ValidSources;
	{
		const TArray<FPCGTaggedData> SourceInputs = Context->InputData.GetInputsByPin(FName("Tags Source"));
		PCGExAttributesToTags::GatherValidInputs(SourceInputs, ValidSources);
	}

	if (ValidSources.IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Source collections are empty."));
		return false;
	}

	int32 NumIterations = 0;

	if (Settings->Resolution == EPCGExAttributeToTagsResolution::CollectionToCollection)
	{
		TArray<PCGExAttributesToTags::FValidInput> ValidMains;
		{
			const TArray<FPCGTaggedData> MainInputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
			PCGExAttributesToTags::GatherValidInputs(MainInputs, ValidMains);
		}

		if (ValidSources.Num() != ValidMains.Num())
		{
			PCGE_LOG(Error, GraphAndLog, FTEXT("Number of input collections don't match the number of sources."));
			return false;
		}

		NumIterations = ValidSources.Num();
	}
	else
	{
		if (ValidSources.Num() != 1 && !Settings->bQuietTooManyCollectionsWarning)
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("More that one collections found in the sources, only the first one will be used."));
		}

		NumIterations = 1;
	}

	Context->Sources.Reserve(NumIterations);
	Context->Details.Reserve(NumIterations);
	for (int i = 0; i < NumIterations; i++)
	{
		const UPCGData* SourceData = ValidSources[i].Tagged->Data;
		Context->Sources.Add(SourceData);

		FPCGExAttributeToTagDetails& Details = Context->Details.Emplace_GetRef();
		if (!PCGExAttributesToTags::InitDetails(Context, Settings->bPrefixWithAttributeName, Context->Attributes, SourceData, Details))
		{
			return false;
		}
	}

	return true;
}

bool FPCGExAttributesToTagsElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExAttributesToTagsElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(AttributesToTags)
	PCGEX_EXECUTION_CHECK

	// Gather valid main inputs (non-null, non-empty), preserving pin order. Entries point into MainInputs,
	// which outlives the loop.
	const TArray<FPCGTaggedData> MainInputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	TArray<PCGExAttributesToTags::FValidInput> ValidMains;
	PCGExAttributesToTags::GatherValidInputs(MainInputs, ValidMains);

	if (ValidMains.IsEmpty())
	{
		return Context->CancelExecution(TEXT("Could not find any points to process."));
	}

	for (int32 i = 0; i < ValidMains.Num(); i++)
	{
		const FPCGTaggedData& MainTagged = *ValidMains[i].Tagged;
		const UPCGData* MainData = MainTagged.Data;

		// Resolve the source reader + its row count for this input.
		const FPCGExAttributeToTagDetails* Details = nullptr;
		FPCGExAttributeToTagDetails SelfDetails;
		int32 SourceNum = 0;

		switch (Settings->Resolution)
		{
		case EPCGExAttributeToTagsResolution::Self:
			// Self reads each input from itself, so the reader is per-input. Source == main, so reuse the
			// row count already measured during gathering.
			if (!PCGExAttributesToTags::InitDetails(Context, Settings->bPrefixWithAttributeName, Context->Attributes, MainData, SelfDetails))
			{
				return false;
			}
			Details = &SelfDetails;
			SourceNum = ValidMains[i].NumRows;
			break;
		case EPCGExAttributeToTagsResolution::CollectionToCollection:
			Details = &Context->Details[i];
			SourceNum = PCGExMetaHelpers::GetElementsCount(Context->Sources[i]);
			break;
		case EPCGExAttributeToTagsResolution::EntryToCollection:
			Details = &Context->Details[0];
			SourceNum = PCGExMetaHelpers::GetElementsCount(Context->Sources[0]);
			break;
		}

		TArray<int32> PickedIndices;
		PCGExAttributesToTags::ResolveIndices(Settings->Selection, SourceNum, i, Context->PickerFactories, PickedIndices);

		switch (Settings->Action)
		{
		case EPCGExAttributeToTagsAction::AddTags:
			{
				// Forward the original data untouched; promoted tags merge with the input's own tags
				// (round-tripped through FTags, as before).
				PCGExData::FTags OutTags;
				OutTags.Append(MainTagged.Tags);

				TSet<FString> Promoted;
				PCGExAttributesToTags::PromoteAll(*Details, PickedIndices, Promoted);
				OutTags.Append(Promoted);

				Context->StageOutput(const_cast<UPCGData*>(MainData), PCGPinConstants::DefaultOutputLabel, PCGExData::EStaging::None, OutTags.Flatten());
			}
			break;
		case EPCGExAttributeToTagsAction::Data:
			{
				// Duplicate the input and write the promoted values as @Data attributes on the copy.
				UPCGData* DupData = Context->ManagedObjects->DuplicateData<UPCGData>(MainData);
				if (!DupData)
				{
					return false;
				}

				if (UPCGMetadata* Metadata = DupData->MutableMetadata())
				{
					PCGExAttributesToTags::PromoteAll(*Details, PickedIndices, Metadata);
				}

				PCGExData::FTags OutTags;
				OutTags.Append(MainTagged.Tags);

				Context->StageOutput(DupData, PCGPinConstants::DefaultOutputLabel, PCGExData::EStaging::None, OutTags.Flatten());
			}
			break;
		case EPCGExAttributeToTagsAction::Attribute:
			{
				// Emit one attribute set per input, carrying the promoted values as @Data attributes.
				UPCGParamData* OutputSet = Context->ManagedObjects->New<UPCGParamData>();
				OutputSet->Metadata->AddEntry();

				PCGExAttributesToTags::PromoteAll(*Details, PickedIndices, OutputSet->Metadata);

				Context->StageOutput(OutputSet, FName("Tags"), PCGExData::EStaging::None);
			}
			break;
		}
	}

	Context->Done();
	return Context->TryComplete();
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
