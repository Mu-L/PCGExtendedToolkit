// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExAttributesToTags.h"

#include <type_traits>

#include "PCGContext.h"
#include "PCGParamData.h"
#include "Containers/PCGExManagedObjects.h"
#include "Core/PCGExPickerFactoryProvider.h"
#include "Data/PCGExDataHelpers.h"
#include "Data/PCGExDataTags.h"
#include "Data/PCGExPointElements.h"
#include "Data/PCGExSubSelection.h"
#include "Factories/PCGExFactories.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Types/PCGExTypeOpsImpl.h"

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
	// A non-empty pin input: a pointer into the caller's stable TaggedData array plus its cached row count. The source TArray must outlive these entries.
	struct FValidInput
	{
		FPCGTaggedData Tagged;
		int32 NumRows = 0;
	};

	// Filter inputs to non-null, non-empty, preserving pin order. `Inputs` must outlive `OutValid` (holds pointers into it).
	void GatherValidInputs(const TArray<FPCGTaggedData>& Inputs, TArray<FValidInput>& OutValid)
	{
		OutValid.Reserve(Inputs.Num());
		for (const FPCGTaggedData& TaggedData : Inputs)
		{
			if (!TaggedData.Data) { continue; }
			const int32 NumRows = PCGExMetaHelpers::GetElementsCount(TaggedData.Data);
			if (NumRows > 0) { OutValid.Add({TaggedData, NumRows}); }
		}
	}

	// Build the tag-detail reader for one source (shared by every resolution). Returns false if broadcasters can't init.
	bool InitDetails(const FPCGExContext* InContext, const bool bPrefixWithAttributeName, const TArray<FPCGAttributePropertyInputSelector>& Attributes, const UPCGData* SourceData, FPCGExAttributeToTagDetails& OutDetails)
	{
		OutDetails.bAddIndexTag = false;
		OutDetails.bPrefixWithAttributeName = bPrefixWithAttributeName;
		OutDetails.Attributes = Attributes;
		return OutDetails.Init(InContext, SourceData);
	}

	// Promote every picked row into the tag target (tag set or @Data metadata); the shared row->Tag loop. Tag() only reads the row index.
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

	// @Data-domain selectors carry a single value per data (not per-row), and the point-centric broadcaster
	// doesn't read them off raw data -- so handle them here via TryReadDataValue. Applied once per source,
	// mirroring FPCGExAttributeToTagDetails::Tag's formatting so @Data and element tags look identical.
	void PromoteDataDomainToTags(FPCGExContext* InContext, const UPCGData* SourceData, const TArray<FPCGAttributePropertyInputSelector>& DataAttributes, const bool bPrefixWithAttributeName, TSet<FString>& OutTags)
	{
		for (const FPCGAttributePropertyInputSelector& Selector : DataAttributes)
		{
			EPCGMetadataTypes Type = EPCGMetadataTypes::Unknown;
			if (!PCGExData::TryGetType(Selector, SourceData, Type)) { continue; }

			PCGExMetaHelpers::ExecuteWithRightType(Type, [&](auto DummyValue)
			{
				using T = decltype(DummyValue);

				T Value{};
				if (!PCGExData::Helpers::TryReadDataValue<T>(InContext, SourceData, Selector, Value, /*bQuiet=*/true)) { return; }

				const FString Prefix = PCGExMetaHelpers::GetAttributeIdentifier(Selector, SourceData).Name.ToString();
				if constexpr (std::is_same_v<T, bool>)
				{
					// Booleans tag by presence.
					if (Value) { OutTags.Add(Prefix); }
				}
				else
				{
					const FString StringValue = PCGExTypeOps::Convert<T, FString>(Value);
					if (StringValue.IsEmpty()) { return; }
					OutTags.Add(bPrefixWithAttributeName ? (Prefix + TEXT(":") + StringValue) : StringValue);
				}
			});
		}
	}

	// @Data variant that writes each promoted value as a @Data attribute on the output metadata.
	void PromoteDataDomainToMetadata(FPCGExContext* InContext, const UPCGData* SourceData, const TArray<FPCGAttributePropertyInputSelector>& DataAttributes, UPCGMetadata* OutMetadata)
	{
		for (const FPCGAttributePropertyInputSelector& Selector : DataAttributes)
		{
			EPCGMetadataTypes Type = EPCGMetadataTypes::Unknown;
			if (!PCGExData::TryGetType(Selector, SourceData, Type)) { continue; }

			PCGExMetaHelpers::ExecuteWithRightType(Type, [&](auto DummyValue)
			{
				using T = decltype(DummyValue);

				T Value{};
				if (!PCGExData::Helpers::TryReadDataValue<T>(InContext, SourceData, Selector, Value, /*bQuiet=*/true)) { return; }

				const FPCGAttributeIdentifier Identifier = PCGExMetaHelpers::GetAttributeIdentifier(Selector, SourceData);
				OutMetadata->DeleteAttribute(Identifier);
				OutMetadata->FindOrCreateAttribute<T>(Identifier, Value);
			});
		}
	}

	// Resolve the row indices to promote per selection mode (indices into the source's own row count). Picker emits all
	// picks ascending; PickerFirst/Last emit the single lowest/highest via min/max scan (no full sort). -1 = out-of-bounds sentinel, skipped.
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

	// Merge explicit selectors with the comma-separated overrides, then split by domain: @Data-domain selectors
	// read a single value via TryReadDataValue, everything else (element attrs / properties / $Index) via the broadcaster.
	TArray<FPCGAttributePropertyInputSelector> AllAttributes = Settings->Attributes;
	PCGExMetaHelpers::AppendUniqueSelectorsFromCommaSeparatedList(Settings->CommaSeparatedAttributeSelectors, AllAttributes);
	for (const FPCGAttributePropertyInputSelector& Selector : AllAttributes)
	{
		if (PCGExMetaHelpers::IsDataDomainAttribute(Selector)) { Context->DataAttributes.Add(Selector); }
		else { Context->Attributes.Add(Selector); }
	}

	// Pickers are valid for every resolution -- including Self -- so load them before the Self short-circuit below.
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
		const UPCGData* SourceData = ValidSources[i].Tagged.Data;
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

	// Gather valid main inputs (non-null, non-empty), preserving pin order. Entries point into MainInputs, which outlives the loop.
	const TArray<FPCGTaggedData> MainInputs = Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	TArray<PCGExAttributesToTags::FValidInput> ValidMains;
	PCGExAttributesToTags::GatherValidInputs(MainInputs, ValidMains);

	if (ValidMains.IsEmpty())
	{
		return Context->CancelExecution(TEXT("Could not find any points to process."));
	}

	for (int32 i = 0; i < ValidMains.Num(); i++)
	{
		const FPCGTaggedData& MainTagged = ValidMains[i].Tagged;
		const UPCGData* MainData = MainTagged.Data;

		// Resolve the source data, its reader, and its row count for this input.
		const UPCGData* SourceData = nullptr;
		const FPCGExAttributeToTagDetails* Details = nullptr;
		FPCGExAttributeToTagDetails SelfDetails;
		int32 SourceNum = 0;

		switch (Settings->Resolution)
		{
		case EPCGExAttributeToTagsResolution::Self:
			// Self reads each input from itself (per-input reader); source == main, so reuse the gathered row count.
			SourceData = MainData;
			if (!PCGExAttributesToTags::InitDetails(Context, Settings->bPrefixWithAttributeName, Context->Attributes, MainData, SelfDetails))
			{
				return false;
			}
			Details = &SelfDetails;
			SourceNum = ValidMains[i].NumRows;
			break;
		case EPCGExAttributeToTagsResolution::CollectionToCollection:
			SourceData = Context->Sources[i];
			Details = &Context->Details[i];
			SourceNum = PCGExMetaHelpers::GetElementsCount(SourceData);
			break;
		case EPCGExAttributeToTagsResolution::EntryToCollection:
			SourceData = Context->Sources[0];
			Details = &Context->Details[0];
			SourceNum = PCGExMetaHelpers::GetElementsCount(SourceData);
			break;
		}

		TArray<int32> PickedIndices;
		PCGExAttributesToTags::ResolveIndices(Settings->Selection, SourceNum, i, Context->PickerFactories, PickedIndices);

		switch (Settings->Action)
		{
		case EPCGExAttributeToTagsAction::AddTags:
			{
				// Forward the original data untouched; promoted tags merge with the input's own (round-tripped through FTags).
				PCGExData::FTags OutTags;
				OutTags.Append(MainTagged.Tags);

				TSet<FString> Promoted;
				PCGExAttributesToTags::PromoteAll(*Details, PickedIndices, Promoted);
				PCGExAttributesToTags::PromoteDataDomainToTags(Context, SourceData, Context->DataAttributes, Settings->bPrefixWithAttributeName, Promoted);
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
					PCGExAttributesToTags::PromoteDataDomainToMetadata(Context, SourceData, Context->DataAttributes, Metadata);
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
				PCGExAttributesToTags::PromoteDataDomainToMetadata(Context, SourceData, Context->DataAttributes, OutputSet->Metadata);

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
