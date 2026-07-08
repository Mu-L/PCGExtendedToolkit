// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

// Build Asset Collection - Builds a transient asset collection from an input attribute set and outputs its
// soft path so a Staging : Distribute node can consume it via its SourceCollection (Constant) override pin.

#include "Elements/PCGExBuildAssetCollection.h"

#include "PCGComponent.h"
#include "PCGExLog.h"
#include "PCGParamData.h"
#include "PCGPin.h"

#include "Containers/PCGExManagedObjects.h"
#include "Core/PCGExAssetCollection.h"
#include "Core/PCGExCollectionHelpers.h"
#include "Helpers/PCGExManagedResourceHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#include "PCGExCollectionsCommon.h"

#define LOCTEXT_NAMESPACE "PCGExBuildAssetCollectionElement"
#define PCGEX_NAMESPACE BuildAssetCollection

#pragma region UPCGExManagedAssetCollection

bool UPCGExManagedAssetCollection::Release(bool bHardRelease, TSet<TSoftObjectPtr<AActor>>& OutActorsToDelete)
{
	// Honor the soft/hard release contract so the collection survives a regeneration and can be reused.
	// PCG's start-of-generation cleanup calls Release(bHardRelease=false); a reusable resource must KEEP its
	// state and return false there (the base marks it unused so the node can MarkAsReused it). Only a hard
	// release -- the end-of-gen sweep of an un-reused resource via ReleaseIfUnused, or teardown -- drops the
	// collection. No actors to delete either way.
	if (bHardRelease)
	{
		Collection = nullptr;
		Config.Reset();
	}
	return Super::Release(bHardRelease, OutActorsToDelete);
}

#pragma endregion

#pragma region UPCGExBuildAssetCollectionSettings

TArray<FPCGPinProperties> UPCGExBuildAssetCollectionSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_PARAM(PCGExCollections::Labels::SourceAssetCollection, "Attribute set describing the collection entries (asset path, optional weight/category).", Required)
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExBuildAssetCollectionSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_PARAM(PCGPinConstants::DefaultOutputLabel, "Attribute set carrying the built collection's soft path under OutputAttributeName. Wire into Staging : Distribute's SourceCollection (Constant) override pin.", Normal)
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(BuildAssetCollection)

#pragma endregion

#pragma region FPCGExBuildAssetCollectionElement

namespace PCGExBuildAssetCollection
{
	// Config-axis identity: the roaming details fully determine how the input rows map to entries. Combined
	// with the input attribute set's own data CRC (content axis), this keys managed-resource reuse across
	// regenerations. GetPathNameSafe gives a stable identity for the collection class.
	FString BuildConfigId(const FPCGExRoamingAssetCollectionDetails& Details)
	{
		return FString::Printf(
			TEXT("%s|%s|%s|%s"),
			*GetPathNameSafe(Details.AssetCollectionType.Get()),
			*Details.AssetPathSourceAttribute.ToString(),
			*Details.WeightSourceAttribute.ToString(),
			*Details.CategorySourceAttribute.ToString());
	}
}

bool FPCGExBuildAssetCollectionElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExBuildAssetCollectionElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(BuildAssetCollection)
	PCGEX_EXECUTION_CHECK

	// Guaranteed by CanExecuteOnlyOnMainThread; NewObject, managed-resource registration and the blocking
	// staging load below all require the game thread.
	check(IsInGameThread());

	// One output entry carrying the resolved collection path (empty path when the build can't happen).
	auto OutputPath = [&](const FSoftObjectPath& InPath)
	{
		UPCGParamData* OutData = Context->ManagedObjects->New<UPCGParamData>();
		check(OutData && OutData->Metadata);

		FPCGMetadataAttribute<FSoftObjectPath>* PathAttr = OutData->Metadata->CreateAttribute<FSoftObjectPath>(
			Settings->OutputAttributeName, FSoftObjectPath(), /*bAllowsInterpolation=*/false, /*bOverrideParent=*/false);

		const int64 EntryKey = OutData->Metadata->AddEntry();
		if (PathAttr) { PathAttr->SetValue(EntryKey, InPath); }

		FPCGTaggedData& Tagged = Context->OutputData.TaggedData.Emplace_GetRef();
		Tagged.Pin = PCGPinConstants::DefaultOutputLabel;
		Tagged.Data = OutData;
	};

	auto CompleteWith = [&](const FSoftObjectPath& InPath) -> bool
	{
		OutputPath(InPath);
		Context->Done();
		return Context->TryComplete();
	};

	// Validate the collection type up front -- without it there's nothing to build.
	if (!Settings->AttributeSetDetails.Validate(Context))
	{
		return CompleteWith(FSoftObjectPath());
	}

	// Pure consumer: first param on the attribute-set input pin.
	const UPCGParamData* InParam = nullptr;
	for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(PCGExCollections::Labels::SourceAssetCollection))
	{
		InParam = Cast<UPCGParamData>(Tagged.Data);
		if (InParam) { break; }
	}

	if (!InParam || !InParam->ConstMetadata())
	{
		PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("Build Asset Collection needs one input attribute set."));
		return CompleteWith(FSoftObjectPath());
	}

	// No component = nowhere to anchor the collection's lifetime, so the soft path would dangle.
	UPCGComponent* Component = Context->GetMutableComponent();
	if (!Component)
	{
		PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("Build Asset Collection needs an owning PCG component to manage collection lifetime; no collection was built."));
		return CompleteWith(FSoftObjectPath());
	}

	// Reuse identity: the roaming config string (exact, collision-proof on the config axis) folded with the
	// input attribute set's full data CRC (content axis). A 32-bit collision at worst reuses a stale
	// collection until the next real input change -- never a crash.
	const FString ConfigId = PCGExBuildAssetCollection::BuildConfigId(Settings->AttributeSetDetails);
	uint32 Hash = GetTypeHash(ConfigId);
	if (const FPCGCrc DataCrc = InParam->GetOrComputeCrc(/*bFullDataCrc=*/true); DataCrc.IsValid())
	{
		Hash = HashCombine(Hash, DataCrc.GetValue());
	}
	const FPCGCrc Crc(Hash);

	// Reuse an identical collection already materialized on this component (dedup + no rebuild churn).
	if (const UPCGExManagedAssetCollection* Existing = PCGExManagedHelpers::TryReuseManagedResource<UPCGExManagedAssetCollection>(
		Component, Crc,
		[&ConfigId](const UPCGExManagedAssetCollection* Candidate) { return Candidate->Collection && Candidate->Config == ConfigId; }))
	{
		return CompleteWith(FSoftObjectPath(Existing->Collection));
	}

	// Fresh build. Outer the resource to the component (persists across regenerations); outer the collection
	// to the resource so its lifetime is unambiguous and its object path is unique + resolvable across the
	// node boundary.
	UPCGExManagedAssetCollection* Managed = NewObject<UPCGExManagedAssetCollection>(Component);
	Managed->SetCrc(Crc);
	Managed->Config = ConfigId;

	UPCGExAssetCollection* Collection = NewObject<UPCGExAssetCollection>(
		Managed, Settings->AttributeSetDetails.AssetCollectionType.Get(), NAME_None, RF_Transient);

	// bBuildStaging=true bakes staging (bounds/paths) now -- RebuildStagingData self-loads each entry's asset
	// on this (game) thread -- so downstream Distribute consumes the resolved collection exactly like a saved
	// asset (its Asset/Constant path never rebuilds staging).
	if (!Collection || !PCGExCollectionHelpers::BuildFromAttributeSet(Collection, Context, InParam, Settings->AttributeSetDetails, /*bBuildStaging=*/true))
	{
		PCGE_LOG_C(Error, GraphAndLog, Context, FTEXT("Failed to build collection from the input attribute set."));
		return CompleteWith(FSoftObjectPath());
	}

	// Warm the lookup cache so the collection is fully ready for downstream consumers.
	Collection->LoadCache();

	Managed->Collection = Collection;
	Component->AddToManagedResources(Managed);

	return CompleteWith(FSoftObjectPath(Collection));
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
