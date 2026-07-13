// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExCopyAttributes.h"

#include "PCGContext.h"
#include "Data/PCGExDataHelpers.h"
#include "Elements/PCGCopyPoints.h"
#include "Helpers/PCGHelpers.h"
#include "Helpers/PCGMetadataHelpers.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataCommon.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Metadata/Accessors/PCGAttributeAccessorKeys.h"

#define LOCTEXT_NAMESPACE "PCGExCopyAttributesElement"
#define PCGEX_NAMESPACE CopyAttributes

#pragma region UPCGExCopyAttributesSettings

UPCGExCopyAttributesSettings::UPCGExCopyAttributesSettings()
{
	if (PCGHelpers::IsNewObjectAndNotDefault(this))
	{
		InputSource.SetAttributeName(PCGMetadataAttributeConstants::LastAttributeName);
		OutputTarget.SetAttributeName(PCGMetadataAttributeConstants::SourceAttributeName);
	}
}

FString UPCGExCopyAttributesSettings::GetAdditionalTitleInformation() const
{
#if WITH_EDITOR
	if (bCopyAllAttributes)
	{
		return LOCTEXT("NodeTitleAllAttributes", "All Attributes").ToString();
	}

	if (IsPropertyOverriddenByPin(GET_MEMBER_NAME_CHECKED(UPCGExCopyAttributesSettings, InputSource)) || IsPropertyOverriddenByPin(GET_MEMBER_NAME_CHECKED(UPCGExCopyAttributesSettings, OutputTarget)))
	{
		return FString();
	}
#endif

	return FString::Printf(TEXT("%s -> %s"), *InputSource.GetDisplayText().ToString(), *OutputTarget.GetDisplayText().ToString());
}

TArray<FPCGPinProperties> UPCGExCopyAttributesSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_ANY(PCGCopyPointsConstants::TargetPointsLabel, "The data to copy attributes onto.", Required)
	PCGEX_PIN_ANY(PCGCopyPointsConstants::SourcePointsLabel, "The data to copy attributes from.", Required)
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExCopyAttributesSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_ANY(PCGPinConstants::DefaultOutputLabel, "The target data, with attributes copied from the sources.", Required)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(CopyAttributes)

#pragma endregion

#pragma region PCGExCopyAttributes

namespace PCGExCopyAttributes
{
	// True when the resolved pair reads a single-value source attribute (@Data-like domain) into a
	// multi-entry target domain -- the case the engine helper collapses to a single element entry.
	bool IsSingleToMultiPromotion(
		const UPCGData* SourceData, const UPCGData* TargetData,
		const FPCGAttributePropertyInputSelector& InputSource, const FPCGAttributePropertyOutputSelector& OutputTarget)
	{
		if (InputSource.GetSelection() != EPCGAttributePropertySelection::Attribute || !InputSource.GetExtraNames().IsEmpty())
		{
			return false;
		}

		if (OutputTarget.GetSelection() != EPCGAttributePropertySelection::Attribute || !OutputTarget.GetExtraNames().IsEmpty())
		{
			return false;
		}

		const UPCGMetadata* SourceMetadata = SourceData->ConstMetadata();
		const UPCGMetadata* TargetMetadata = TargetData->ConstMetadata();
		if (!SourceMetadata || !TargetMetadata)
		{
			return false;
		}

		const FPCGMetadataDomainID SourceDomain = SourceData->GetMetadataDomainIDFromSelector(InputSource);
		const FPCGMetadataDomainID TargetDomain = TargetData->GetMetadataDomainIDFromSelector(OutputTarget);
		if (!SourceDomain.IsValid() || !TargetDomain.IsValid())
		{
			return false;
		}

		return !SourceMetadata->MetadataDomainSupportsMultiEntries(SourceDomain) && TargetMetadata->MetadataDomainSupportsMultiEntries(TargetDomain);
	}

	bool CopySingleValueToElements(
		FPCGExContext* InContext,
		const UPCGData* SourceData, UPCGData* TargetData,
		const FPCGAttributePropertyInputSelector& InputSource, const FPCGAttributePropertyOutputSelector& OutputTarget,
		const bool bMaterialize)
	{
		const FPCGAttributeIdentifier SourceId = PCGExMetaHelpers::GetAttributeIdentifier(InputSource, SourceData);
		const FPCGMetadataAttributeBase* SourceAttribute = PCGExMetaHelpers::TryGetConstAttribute(SourceData, SourceId);
		if (!SourceAttribute)
		{
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(LOCTEXT("SourceAttributeMissing", "Source attribute '{0}' does not exist."), InputSource.GetDisplayText()));
			return false;
		}

		// Canonical single-value read slot (mirrors PCGExData::Helpers::ReadDataValue): first entry
		// when the domain carries items, default-value slot otherwise.
		const FPCGMetadataDomain* SourceDomain = SourceAttribute->GetMetadataDomain();
		const PCGMetadataEntryKey SourceKey = (SourceDomain && SourceDomain->GetItemCountForChild() > 0) ? PCGFirstEntryKey : PCGDefaultValueKey;

		UPCGMetadata* TargetMetadata = TargetData->MutableMetadata();
		FPCGMetadataDomain* TargetDomain = TargetMetadata ? TargetMetadata->GetMetadataDomainFromSelector(OutputTarget) : nullptr;
		if (!TargetDomain)
		{
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(LOCTEXT("InvalidTargetDomain", "Invalid target domain for '{0}'."), OutputTarget.GetDisplayText()));
			return false;
		}

		// Same semantics as the engine helper: an existing target attribute is deleted and recreated,
		// so the non-materialized copy leaves nothing but the default value behind.
		const FName TargetName = OutputTarget.GetAttributeName();
		if (TargetDomain->HasAttribute(TargetName))
		{
			TargetDomain->DeleteAttribute(TargetName);
		}

		FPCGMetadataAttributeDesc Desc = SourceAttribute->GetAttributeDesc();
		Desc.Name = TargetName;

		FPCGMetadataAttributeBase* TargetAttribute = TargetDomain->CreateAttribute(Desc, SourceAttribute->AllowsInterpolation(), /*bOverrideParent=*/true);
		if (!TargetAttribute)
		{
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(LOCTEXT("FailedCreateAttribute", "Failed to create attribute '{0}'."), FText::FromName(TargetName)));
			return false;
		}

		if (!PCGExData::Helpers::PropertyCopyAttribute(SourceAttribute, SourceKey, TargetAttribute, PCGDefaultValueKey))
		{
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(LOCTEXT("FailedCopyDefaultValue", "Failed to copy the value of '{0}' onto '{1}'."), InputSource.GetDisplayText(), FText::FromName(TargetName)));
			return false;
		}

		if (!bMaterialize)
		{
			return true;
		}

		const TUniquePtr<IPCGAttributeAccessorKeys> TargetKeys = PCGAttributeAccessorHelpers::CreateKeys(TargetData, OutputTarget);
		const int32 NumTargetKeys = TargetKeys ? TargetKeys->GetNum() : 0;
		if (NumTargetKeys <= 0)
		{
			// No elements to materialize onto; the default value is already set.
			return true;
		}

		TArray<PCGMetadataEntryKey*> EntryKeyPtrs;
		EntryKeyPtrs.SetNumUninitialized(NumTargetKeys);
		if (!TargetKeys->GetKeys<PCGMetadataEntryKey>(0, EntryKeyPtrs))
		{
			return true;
		}

		// Stripped-down InitializeOnSet, same shape as the engine helper: elements without a concrete
		// entry get one so a value can be attached per element.
		TArray<PCGMetadataEntryKey*> EntriesToAdd;
		EntriesToAdd.Reserve(NumTargetKeys);
		for (PCGMetadataEntryKey* EntryKey : EntryKeyPtrs)
		{
			if (*EntryKey == PCGInvalidEntryKey || *EntryKey < TargetDomain->GetItemKeyCountForParent())
			{
				EntriesToAdd.Add(EntryKey);
			}
		}

		if (!EntriesToAdd.IsEmpty())
		{
			TargetDomain->AddEntriesInPlace(EntriesToAdd);
		}

		// Store the value once through the first entry, then share its value key across all entries.
		if (!PCGExData::Helpers::PropertyCopyAttribute(SourceAttribute, SourceKey, TargetAttribute, *EntryKeyPtrs[0]))
		{
			// Default value is set; elements simply stay non-materialized.
			return true;
		}

		const PCGMetadataEntryKey FirstEntryKey = *EntryKeyPtrs[0];
		TArray<PCGMetadataValueKey> ValueKeys;
		TargetAttribute->GetValueKeys(MakeArrayView(&FirstEntryKey, 1), ValueKeys);

		if (ValueKeys.Num() == 1)
		{
			TArray<PCGMetadataValueKey> AllValueKeys;
			AllValueKeys.Init(ValueKeys[0], NumTargetKeys);
			TargetAttribute->SetValuesFromValueKeys(EntryKeyPtrs, AllValueKeys, /*bResetValueOnDefaultValueKey=*/true);
		}

		return true;
	}

	bool CopyPair(
		FPCGExContext* InContext,
		const UPCGData* SourceData, UPCGData* TargetData,
		const FPCGAttributePropertyInputSelector& InInputSource, const FPCGAttributePropertyOutputSelector& InOutputTarget,
		const bool bSameOrigin, const bool bMaterialize, const bool bRegisterConsumable)
	{
		const FPCGAttributePropertyInputSelector InputSource = InInputSource.CopyAndFixLast(SourceData);
		const FPCGAttributePropertyOutputSelector OutputTarget = InOutputTarget.CopyAndFixSource(&InputSource, SourceData);

		// Cache-and-restore workflows: the source attribute is registered as consumable so enabling
		// the cleanup toggle deletes the temp attribute from the output -- unless deletion would
		// target the very attribute this copy writes.
		if (bRegisterConsumable)
		{
			const FName QualifiedSourceName = PCGExMetaHelpers::GetDomainQualifiedName(InputSource, SourceData);
			if (!QualifiedSourceName.IsNone())
			{
				bool bCollidesWithWritten = false;
				if (OutputTarget.GetSelection() == EPCGAttributePropertySelection::Attribute)
				{
					const FPCGAttributeIdentifier DeleteId = PCGExMetaHelpers::GetAttributeIdentifier(QualifiedSourceName, TargetData);
					const FPCGAttributeIdentifier WrittenId(OutputTarget.GetAttributeName(), TargetData->GetMetadataDomainIDFromSelector(OutputTarget));
					bCollidesWithWritten = DeleteId == WrittenId;
				}

				if (!bCollidesWithWritten)
				{
					InContext->AddConsumableAttributeName(QualifiedSourceName);
				}
			}
		}

		if (IsSingleToMultiPromotion(SourceData, TargetData, InputSource, OutputTarget))
		{
			return CopySingleValueToElements(InContext, SourceData, TargetData, InputSource, OutputTarget, bMaterialize);
		}

		PCGMetadataHelpers::FPCGCopyAttributeParams Params{};
		Params.SourceData = SourceData;
		Params.TargetData = TargetData;
		Params.InputSource = InputSource;
		Params.OutputTarget = OutputTarget;
		Params.OptionalContext = InContext;
		Params.bSameOrigin = bSameOrigin;

		return PCGMetadataHelpers::CopyAttribute(Params);
	}

	bool CopyAllAttributes(
		FPCGExContext* InContext, const UPCGExCopyAttributesSettings* Settings,
		const UPCGData* SourceData, UPCGData* TargetData)
	{
		const UPCGMetadata* SourceMetadata = SourceData->ConstMetadata();
		if (!SourceMetadata)
		{
			return false;
		}

		PCGMetadataHelpers::FPCGCopyAllAttributesParams Params
		{
			.SourceData = SourceData,
			.TargetData = TargetData,
			.OptionalContext = InContext,
		};

		if (Settings->bCopyAllDomains)
		{
			Params.InitializeMappingForAllDomains();
		}
		else
		{
			Params.InitializeMappingFromDomainNames(Settings->MetadataDomainsMapping);
		}

		if (Params.DomainMapping.IsEmpty())
		{
			return false;
		}

		// Mirror of the engine's CopyAllAttributes selector expansion, except each pair routes through
		// CopyPair so the single-value -> multi-entry promotion applies here too.
		TArray<FPCGAttributeIdentifier> AttributeIDs;
		TArray<EPCGMetadataTypes> AttributeTypes;
		SourceMetadata->GetAllAttributes(AttributeIDs, AttributeTypes);

		const FPCGMetadataDomainID DefaultSourceDomain = SourceMetadata->GetConstDefaultMetadataDomain()->GetDomainID();

		bool bSuccess = false;
		for (const FPCGAttributeIdentifier& AttributeID : AttributeIDs)
		{
			const FPCGMetadataDomainID* TargetDomainId = Params.DomainMapping.Find(AttributeID.MetadataDomain);
			if (!TargetDomainId && AttributeID.MetadataDomain == DefaultSourceDomain)
			{
				TargetDomainId = Params.DomainMapping.Find(PCGMetadataDomainID::Default);
			}

			if (!TargetDomainId && AttributeID.MetadataDomain.IsDefault())
			{
				TargetDomainId = Params.DomainMapping.Find(DefaultSourceDomain);
			}

			if (!TargetDomainId)
			{
				continue;
			}

			FPCGAttributePropertyInputSelector InputSource;
			InputSource.SetAttributeName(AttributeID.Name);
			FPCGAttributePropertyOutputSelector OutputTarget;
			OutputTarget.SetAttributeName(AttributeID.Name);

			SourceData->SetDomainFromDomainID(AttributeID.MetadataDomain, InputSource);
			TargetData->SetDomainFromDomainID(*TargetDomainId, OutputTarget);

			// No consumable registration in copy-all: source names match the just-copied attributes.
			bSuccess |= CopyPair(InContext, SourceData, TargetData, InputSource, OutputTarget, /*bSameOrigin=*/false, Settings->bMaterializeDataValues, /*bRegisterConsumable=*/false);
		}

		return bSuccess;
	}
}

#pragma endregion

#pragma region FPCGExCopyAttributesElement

bool FPCGExCopyAttributesElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExCopyAttributesElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(CopyAttributes)

	TArray<FPCGTaggedData> Sources = Context->InputData.GetInputsByPin(PCGCopyPointsConstants::SourcePointsLabel);
	TArray<FPCGTaggedData> Targets = Context->InputData.GetInputsByPin(PCGCopyPointsConstants::TargetPointsLabel);

	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	const int32 NumSources = Sources.Num();
	const int32 NumTargets = Targets.Num();

	if (NumSources == 0 || NumTargets == 0)
	{
		Context->Done();
		return Context->TryComplete();
	}

	int32 NumIterations = 0;
	switch (Settings->Operation)
	{
	case EPCGCopyAttributesOperation::CopyEachSourceOnEveryTarget:
		NumIterations = NumSources * NumTargets;
		break;
	case EPCGCopyAttributesOperation::MergeSourcesAndCopyToAllTargets:
		NumIterations = NumTargets;
		break;
	case EPCGCopyAttributesOperation::CopyEachSourceToEachTargetRespectively:
	default:
		if (NumSources != NumTargets && NumSources != 1 && NumTargets != 1)
		{
			PCGE_LOG_C(Error, GraphAndLog, Context, FText::Format(LOCTEXT("MismatchNum", "Num Sources ({0}) mismatches with Num Targets ({1}). Only supports N:N, 1:N and N:1 operation."), NumSources, NumTargets));
			for (const FPCGTaggedData& Target : Targets)
			{
				FPCGTaggedData& Output = Outputs.Add_GetRef(Target);
				Output.Pin = PCGPinConstants::DefaultOutputLabel;
			}
			Context->Done();
			return Context->TryComplete();
		}
		// The engine element iterates NumTargets here, silently dropping extra sources in the N:1
		// case; Max honors the documented "produces Max(N,M) data" contract of the operation enum.
		NumIterations = FMath::Max(NumSources, NumTargets);
		break;
	}

	const bool bStealData = Context->bWantsDataStealing;

	for (int32 i = 0; i < NumIterations; ++i)
	{
		const int32 TargetIndex = i % NumTargets;
		const FPCGTaggedData& Target = Targets[TargetIndex];

		FPCGTaggedData& Output = Outputs.Add_GetRef(Target);
		Output.Pin = PCGPinConstants::DefaultOutputLabel;

		const UPCGData* TargetData = Target.Data;

		// Stealing may only mutate a given input once; each target's last iteration is the only one
		// with no later reader, so earlier occurrences fall back to duplication.
		const bool bStealThisTarget = bStealData && (i + NumTargets >= NumIterations);

		bool bSuccess = false;
		UPCGData* OutputData = nullptr;

		auto DoCopy = [&](const int32 SourceIndex)
		{
			const FPCGTaggedData& Source = Sources[SourceIndex];
			const UPCGData* SourceData = Source.Data;

			const bool bIsSameData = TargetData == SourceData;

			if (!TargetData || !SourceData)
			{
				PCGE_LOG_C(Error, GraphAndLog, Context, LOCTEXT("InvalidInputData", "Invalid input data"));
				return;
			}

			if (!SourceData->ConstMetadata())
			{
				PCGE_LOG_C(Warning, GraphAndLog, Context, LOCTEXT("MissingMetadata", "Source has no metadata"));
				return;
			}

			if (bIsSameData && Settings->bCopyAllAttributes)
			{
				PCGE_LOG_C(Verbose, LogOnly, Context, LOCTEXT("TrivialAllCopy", "Copying all attributes on itself is a trivial operation."));
				return;
			}

			if (bIsSameData && Settings->InputSource == Settings->OutputTarget)
			{
				PCGE_LOG_C(Verbose, LogOnly, Context, LOCTEXT("TrivialCopy", "Copying attribute to itself is a trivial operation."));
				return;
			}

			if (!OutputData)
			{
				OutputData = bStealThisTarget ? const_cast<UPCGData*>(TargetData) : TargetData->DuplicateData(Context);
			}

			if (Settings->bCopyAllAttributes)
			{
				bSuccess |= PCGExCopyAttributes::CopyAllAttributes(Context, Settings, SourceData, OutputData);
			}
			else
			{
				bSuccess |= PCGExCopyAttributes::CopyPair(Context, SourceData, OutputData, Settings->InputSource, Settings->OutputTarget, bIsSameData, Settings->bMaterializeDataValues, /*bRegisterConsumable=*/true);
			}
		};

		switch (Settings->Operation)
		{
		case EPCGCopyAttributesOperation::CopyEachSourceOnEveryTarget:
			DoCopy(i / NumTargets);
			break;
		case EPCGCopyAttributesOperation::MergeSourcesAndCopyToAllTargets:
			for (int32 j = 0; j < NumSources; ++j)
			{
				DoCopy(j);
			}
			break;
		case EPCGCopyAttributesOperation::CopyEachSourceToEachTargetRespectively:
		default:
			DoCopy(FMath::Min(i, NumSources - 1));
			break;
		}

		if (bSuccess && OutputData)
		{
			Output.Data = OutputData;
			Context->AddCleanableOutput(OutputData);
		}
	}

	Context->Done();
	return Context->TryComplete();
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
