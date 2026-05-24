// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExGetCollectionData.h"

#include "PCGGraph.h"
#include "PCGParamData.h"
#include "PCGPin.h"
#include "Collections/PCGExActorCollection.h"
#include "Core/PCGExAssetCollection.h"
#include "Core/PCGExMTCommon.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGExDataHelpers.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Metadata/Accessors/PCGAttributeAccessorKeys.h"
#include "Metadata/Accessors/PCGCustomAccessor.h"
#include "Elements/Grammar/PCGSubdivisionBase.h"
#include "Helpers/PCGExCollectionPropertySetWriter.h"
#include "Helpers/PCGExMetaHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExGetCollectionData"
#define PCGEX_NAMESPACE GetCollectionData

namespace PCGExGetCollectionData
{
	const FName SourcesPin = TEXT("Sources");
	const FName OutputAttributeSetPin = TEXT("AttributeSet");
	const FName EmptyTag = TEXT("empty");

	/** Collected entry along with the collection that directly owns it and the resolved category. */
	struct FFlattenedEntry
	{
		const FPCGExAssetCollectionEntry* Entry = nullptr;
		const UPCGExAssetCollection* Host = nullptr;
		FName Category = NAME_None;
	};

	/** Invariants shared across the flatten recursion. */
	struct FProcessEntryContext
	{
		FPCGExContext* Context = nullptr;
		const FPCGExNameFiltersDetails* CategoryFilters = nullptr;
		EPCGExSubCollectionToSet SubHandling = EPCGExSubCollectionToSet::Ignore;
		EPCGExCategoryInheritance CategoryInheritance = EPCGExCategoryInheritance::None;
		bool bOmitInvalidAndEmpty = true;
		bool bNoDuplicates = true;
	};

	static void ProcessEntry(
		const FProcessEntryContext& Ctx,
		const FPCGExAssetCollectionEntry* InEntry,
		const UPCGExAssetCollection* InHost,
		TArray<FFlattenedEntry>& OutEntries,
		const FName EffectiveParentCategory,
		TSet<uint64>& GUIDS)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExGetCollectionData::ProcessEntry);

		if (Ctx.bNoDuplicates)
		{
			for (const FFlattenedEntry& Existing : OutEntries)
			{
				if (Existing.Entry == InEntry)
				{
					return;
				}
			}
		}

		auto AddNone = [&]()
		{
			if (Ctx.bOmitInvalidAndEmpty)
			{
				return;
			}
			OutEntries.Add({nullptr, nullptr, NAME_None});
		};

		if (!InEntry)
		{
			AddNone();
			return;
		}

		if (!Ctx.CategoryFilters->Test(InEntry->Category.ToString()))
		{
			return;
		}

		auto ResolveCategory = [&](const FName Authored) -> FName
		{
			switch (Ctx.CategoryInheritance)
			{
			case EPCGExCategoryInheritance::FillEmpty:
				return Authored.IsNone() ? EffectiveParentCategory : Authored;
			case EPCGExCategoryInheritance::Replace:
				return EffectiveParentCategory.IsNone() ? Authored : EffectiveParentCategory;
			default:
				return Authored;
			}
		};

		auto AddEmpty = [&](const FPCGExAssetCollectionEntry* S)
		{
			if (Ctx.bOmitInvalidAndEmpty)
			{
				return;
			}
			OutEntries.Add({S, InHost, NAME_None});
		};

		if (!InEntry->bIsSubCollection)
		{
			OutEntries.Add({InEntry, InHost, ResolveCategory(InEntry->Category)});
			return;
		}

		if (Ctx.SubHandling == EPCGExSubCollectionToSet::Ignore)
		{
			return;
		}

		if (Ctx.SubHandling == EPCGExSubCollectionToSet::Grammar)
		{
			if (InEntry->SubGrammarMode != EPCGExGrammarSubCollectionMode::Flatten)
			{
				OutEntries.Add({InEntry, InHost, ResolveCategory(InEntry->Category)});
				return;
			}
		}

		UPCGExAssetCollection* SubCollection = InEntry->Staging.LoadSync<UPCGExAssetCollection>(Ctx.Context);
		const PCGExAssetCollection::FCache* SubCache = SubCollection ? SubCollection->LoadCache() : nullptr;

		if (!SubCache)
		{
			AddEmpty(InEntry);
			return;
		}

		bool bVisited = false;
		GUIDS.Add(SubCollection->GetUniqueID(), &bVisited);
		if (bVisited)
		{
			return;
		}

		const FName NextParent = InEntry->Category.IsNone() ? EffectiveParentCategory : InEntry->Category;

		FPCGExEntryAccessResult SubResult;
		switch (Ctx.SubHandling)
		{
		default: ;
		case EPCGExSubCollectionToSet::Grammar:
		case EPCGExSubCollectionToSet::Expand:
			for (int i = 0; i < SubCache->Main->Order.Num(); i++)
			{
				SubResult = SubCollection->GetEntryAt(i);
				ProcessEntry(Ctx, SubResult.Entry, SubResult.Host, OutEntries, NextParent, GUIDS);
			}
			return;
		case EPCGExSubCollectionToSet::PickRandom:
			SubResult = SubCollection->GetEntryRandom(0);
			break;
		case EPCGExSubCollectionToSet::PickRandomWeighted:
			SubResult = SubCollection->GetEntryWeightedRandom(0);
			break;
		case EPCGExSubCollectionToSet::PickFirstItem:
			SubResult = SubCollection->GetEntryAt(0);
			break;
		case EPCGExSubCollectionToSet::PickLastItem:
			SubResult = SubCollection->GetEntryAt(SubCache->Main->Indices.Num() - 1);
			break;
		}

		ProcessEntry(Ctx, SubResult.Entry, SubResult.Host, OutEntries, NextParent, GUIDS);
	}

	static void FlattenCollection(
		const FProcessEntryContext& Ctx,
		UPCGExAssetCollection* Collection,
		TArray<FFlattenedEntry>& OutEntries)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExGetCollectionData::FlattenCollection);

		if (!Collection)
		{
			return;
		}

		const PCGExAssetCollection::FCache* MainCache = Collection->LoadCache();
		if (!MainCache)
		{
			return;
		}

		TSet<uint64> GUIDS;
		for (int i = 0; i < MainCache->Main->Order.Num(); i++)
		{
			GUIDS.Empty();
			FPCGExEntryAccessResult Result = Collection->GetEntryAt(i);
			ProcessEntry(Ctx, Result.Entry, Result.Host, OutEntries, NAME_None, GUIDS);
		}
	}

	/** Read all rows of T from a single input. One CreateConstAccessor + one GetRange call --
	 *  drops hundreds of read-locked GetValueFromItemKey hits down to a single locked bulk read,
	 *  and AllowBroadcastAndConstructible handles FString->FSoftObjectPath / narrower-int->int64
	 *  coercion for free.
	 *
	 *  We can't use TAttributeBroadcaster directly: its FAttributeProcessingInfos::Init gates
	 *  attribute discovery on Cast<UPCGSpatialData>(InData), which rejects UPCGParamData (attribute
	 *  sets) silently. We talk to PCGAttributeAccessorHelpers directly to support both Param and
	 *  Point inputs uniformly. */
	template <typename T>
	static void BulkReadRows(const UPCGData* InData, const FName AttributeName, TArray<T>& OutValues)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExGetCollectionData::BulkReadRows);

		if (!InData)
		{
			return;
		}

		FPCGAttributePropertyInputSelector Selector;
		Selector.Update(AttributeName.ToString());
		Selector = Selector.CopyAndFixLast(InData);

		TUniquePtr<const IPCGAttributeAccessor> Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(InData, Selector);
		if (!Accessor)
		{
			return;
		}

		TSharedPtr<IPCGAttributeAccessorKeys> Keys;
		if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(InData))
		{
			Keys = MakeShared<FPCGAttributeAccessorKeysPointIndices>(PointData);
		}
		else if (InData->ConstMetadata())
		{
			Keys = MakeShared<FPCGAttributeAccessorKeysEntries>(InData->ConstMetadata());
		}
		if (!Keys)
		{
			return;
		}

		const int32 NumValues = Keys->GetNum();
		if (NumValues <= 0)
		{
			return;
		}

		OutValues.SetNumUninitialized(NumValues);
		if (!Accessor->GetRange<T>(OutValues, 0, *Keys, EPCGAttributeAccessorFlags::AllowBroadcastAndConstructible))
		{
			OutValues.Reset();
		}
	}

	/** Single-value @Data-domain read for the PerInputData fanout mode. Falls back from
	 *  FSoftObjectPath to FString (authored-as-string paths) before giving up. */
	static FSoftObjectPath ReadSinglePath(FPCGExContext* InContext, const UPCGData* InData, const FName AttributeName)
	{
		if (!InData)
		{
			return FSoftObjectPath();
		}
		FSoftObjectPath Path;
		if (PCGExData::Helpers::TryReadDataValue<FSoftObjectPath>(InContext, InData, AttributeName, Path, /*bQuiet=*/true))
		{
			return Path;
		}
		FString PathStr;
		if (PCGExData::Helpers::TryReadDataValue<FString>(InContext, InData, AttributeName, PathStr, /*bQuiet=*/false))
		{
			return FSoftObjectPath(PathStr);
		}
		return FSoftObjectPath();
	}

	static int64 ReadSingleHash(FPCGExContext* InContext, const UPCGData* InData, const FName AttributeName)
	{
		if (!InData)
		{
			return 0;
		}
		int64 Hash = 0;
		PCGExData::Helpers::TryReadDataValue<int64>(InContext, InData, AttributeName, Hash, /*bQuiet=*/false);
		return Hash;
	}
}

#pragma region UPCGSettings interface

#if WITH_EDITOR
void UPCGExGetCollectionDataSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bWriteAssetClass = bWriteAssetPath;
	AssetClassAttributeName = AssetPathAttributeName;
}
#endif

TArray<FPCGPinProperties> UPCGExGetCollectionDataSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	if (SourceMode == EPCGExGetCollectionDataSourceMode::FromInputs)
	{
		PCGEX_PIN_ANY(PCGExGetCollectionData::SourcesPin, TEXT("Input attribute sets and/or point data providing collection references."), Required)
		if (SourceShape == EPCGExGetCollectionDataSourceShape::EntryIdAndMap)
		{
			PCGEX_PIN_PARAM(PCGExCollections::Labels::SourceCollectionMapLabel, TEXT("Upstream Collection Map used to resolve entry hashes back to sub-collection entries."), Required)
		}
	}
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExGetCollectionDataSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_PARAMS(PCGExGetCollectionData::OutputAttributeSetPin, TEXT("One attribute set per resolved source (see Fanout)."), Required)
	PCGEX_PIN_PARAM(PCGExCollections::Labels::OutputCollectionMapLabel, TEXT("Collection map covering every host referenced by the emitted attribute sets."), Required)
	return PinProperties;
}

FPCGElementPtr UPCGExGetCollectionDataSettings::CreateElement() const
{
	return MakeShared<FPCGExGetCollectionDataElement>();
}

#pragma endregion

bool FPCGExGetCollectionDataElement::IsCacheable(const UPCGSettings* InSettings) const
{
	const UPCGExGetCollectionDataSettings* Settings = static_cast<const UPCGExGetCollectionDataSettings*>(InSettings);
	PCGEX_GET_OPTION_STATE(Settings->CacheData, bDefaultCacheNodeOutput)
}

bool FPCGExGetCollectionDataElement::Boot(FPCGExContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExGetCollectionDataElement::Boot);

	if (!IPCGExElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_SETTINGS_C(InContext, GetCollectionData)
	FPCGExGetCollectionDataContext* Context = static_cast<FPCGExGetCollectionDataContext*>(InContext);

	// Validate attribute names up-front -- abort early on bad config.
	if (Settings->bWriteAssetPath)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->AssetPathAttributeName)
	}
	if (Settings->bWriteWeight)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->WeightAttributeName)
	}
	if (Settings->bWriteCategory)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->CategoryAttributeName)
	}
	if (Settings->bWriteExtents)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->ExtentsAttributeName)
	}
	if (Settings->bWriteBoundsMin)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->BoundsMinAttributeName)
	}
	if (Settings->bWriteBoundsMax)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->BoundsMaxAttributeName)
	}
	if (Settings->bWriteNestingDepth)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->NestingDepthAttributeName)
	}
	if (Settings->bWriteSymbol)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->SymbolAttributeName)
	}
	if (Settings->bWriteSize)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->SizeAttributeName)
	}
	if (Settings->bWriteScalable)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->ScalableAttributeName)
	}
	if (Settings->bWriteDebugColor)
	{
		PCGEX_VALIDATE_NAME_C(InContext, Settings->DebugColorAttributeName)
	}

	if (Settings->SourceMode == EPCGExGetCollectionDataSourceMode::Collection)
	{
		// Hard-ref TObjectPtr: asset is already loaded. One slot, one path.
		UPCGExAssetCollection* MainCollection = Settings->AssetCollection;
		if (!MainCollection)
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Asset collection is not set."));
			return false;
		}
		FPCGExGetCollectionDataContext::FSlot& Slot = Context->Slots.AddDefaulted_GetRef();
		Slot.Path = FSoftObjectPath(MainCollection);
		Context->ResolvedCollections.Add(Slot.Path, MainCollection);
		return true;
	}

	// FromInputs mode -- parse inputs, gather paths, register dependencies for async load.
	if (Settings->SourceAttribute.IsNone())
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("SourceAttribute is required when SourceMode is FromInputs."));
		return false;
	}

	TArray<FPCGTaggedData> Inputs = InContext->InputData.GetInputsByPin(PCGExGetCollectionData::SourcesPin);
	if (Inputs.IsEmpty())
	{
		return true;
	} // nothing to do

	// EntryIdAndMap: resolve hashes to sub-collection paths via the unpacker.
	// NOTE: FPickUnpacker::UnpackPin internally LoadBlocking_AnyThreads the map collections --
	// out of scope for this pass; tracked as a known issue (depends on the unpacker itself).
	TSharedPtr<PCGExCollections::FPickUnpacker> Unpacker;
	if (Settings->SourceShape == EPCGExGetCollectionDataSourceShape::EntryIdAndMap)
	{
		Unpacker = MakeShared<PCGExCollections::FPickUnpacker>();
		Unpacker->UnpackPin(InContext, PCGExCollections::Labels::SourceCollectionMapLabel);
		if (!Unpacker->HasValidMapping())
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("EntryIdAndMap mode requires a valid Collection Map on the Map pin."));
			return false;
		}
	}

	const bool bDataDomainOnly = Settings->Fanout == EPCGExGetCollectionDataFanout::PerInputData;
	const bool bIsSoftPath = Settings->SourceShape == EPCGExGetCollectionDataSourceShape::SoftPath;
	const FName SourceAttribute = Settings->SourceAttribute;

	// Per-input scratch: paths (SoftPath mode) or hashes (EntryIdAndMap mode).
	// We split path resolution into two passes so we can parallelize each independently.
	struct FInputParse
	{
		TArray<FSoftObjectPath> Paths;
		TArray<int64> Hashes;
	};
	TArray<FInputParse> PerInput;
	PerInput.SetNum(Inputs.Num());

	// Phase A: parallel per-input bulk read. Each iteration owns its own PerInput[i] slot.
	// Bulk GetRange replaces hundreds of read-locked GetValueFromItemKey calls with one.
	PCGExMT::ParallelOrSequential(Inputs.Num(), [&](const int32 i)
	{
		const FPCGTaggedData& TD = Inputs[i];
		FInputParse& Out = PerInput[i];

		if (bDataDomainOnly)
		{
			// One value per input (regardless of read success -> empty slot on failure).
			if (bIsSoftPath)
			{
				Out.Paths.Add(PCGExGetCollectionData::ReadSinglePath(InContext, TD.Data, SourceAttribute));
			}
			else
			{
				Out.Hashes.Add(PCGExGetCollectionData::ReadSingleHash(InContext, TD.Data, SourceAttribute));
			}
		}
		else
		{
			if (bIsSoftPath)
			{
				PCGExGetCollectionData::BulkReadRows<FSoftObjectPath>(TD.Data, SourceAttribute, Out.Paths);
			}
			else
			{
				PCGExGetCollectionData::BulkReadRows<int64>(TD.Data, SourceAttribute, Out.Hashes);
			}
		}
	}, /*Threshold=*/2, EParallelForFlags::Unbalanced);

	// Phase B (single-threaded): pre-size Slots, write SourceInputIndex backrefs and -- for
	// SoftPath -- the paths themselves. For EntryIdAndMap we leave Path empty and fill it in phase C.
	int32 TotalRows = 0;
	for (const FInputParse& P : PerInput)
	{
		TotalRows += bIsSoftPath ? P.Paths.Num() : P.Hashes.Num();
	}
	Context->Slots.SetNum(TotalRows);

	TArray<int64> FlatHashes; // only used for EntryIdAndMap
	if (!bIsSoftPath)
	{
		FlatHashes.SetNumUninitialized(TotalRows);
	}

	{
		int32 Cursor = 0;
		for (int32 i = 0; i < Inputs.Num(); i++)
		{
			const FInputParse& P = PerInput[i];
			const int32 RowCount = bIsSoftPath ? P.Paths.Num() : P.Hashes.Num();
			for (int32 r = 0; r < RowCount; r++)
			{
				FPCGExGetCollectionDataContext::FSlot& Slot = Context->Slots[Cursor];
				Slot.SourceInputIndex = i;
				if (bIsSoftPath)
				{
					Slot.Path = P.Paths[r];
				}
				else
				{
					FlatHashes[Cursor] = P.Hashes[r];
				}
				Cursor++;
			}
		}
	}

	// Phase C: parallel hash -> sub-collection path resolution (EntryIdAndMap only).
	// FPickUnpacker::UnpackHash is read-only on CollectionMap after UnpackPin, safe for concurrent reads.
	if (!bIsSoftPath)
	{
		PCGExMT::ParallelOrSequential(TotalRows, [&](const int32 i)
		{
			const int64 Hash = FlatHashes[i];
			int16 OutPrimary = 0;
			int16 OutSecondary = 0;
			UPCGExAssetCollection* HostCollection = Unpacker->UnpackHash(static_cast<uint64>(Hash), OutPrimary, OutSecondary);
			if (!HostCollection)
			{
				return;
			}
			FPCGExEntryAccessResult AccessResult = HostCollection->GetEntryRaw(OutPrimary);
			const FPCGExAssetCollectionEntry* Entry = AccessResult.Entry;
			if (!Entry || !Entry->bIsSubCollection)
			{
				return;
			} // leaf -> empty slot (per design)
			Context->Slots[i].Path = Entry->Staging.Path;
		}, /*Threshold=*/64, EParallelForFlags::None);
	}

	// Batch-load all unique valid paths synchronously. The framework's async LoadAssets path
	// (AddAssetDependency) defers AdvanceWork to the next tick, which on cold loads stacks a
	// full frame of wall-clock latency on top of the load itself. LoadBlocking_AnyThread keeps
	// everything in this frame: warm-cache resolves are instant, cold loads stall a worker
	// thread briefly -- same total cost as the load itself, no extra frame.
	{
		TSharedRef<TSet<FSoftObjectPath>> UniquePaths = MakeShared<TSet<FSoftObjectPath>>();
		UniquePaths->Reserve(Context->Slots.Num());
		for (const FPCGExGetCollectionDataContext::FSlot& Slot : Context->Slots)
		{
			if (Slot.Path.IsValid())
			{
				UniquePaths->Add(Slot.Path);
			}
		}
		if (!UniquePaths->IsEmpty())
		{
			PCGExHelpers::LoadBlocking_AnyThread(TSharedPtr<TSet<FSoftObjectPath>>(UniquePaths), InContext);
		}

		// Resolve into ResolvedCollections now that everything's in memory.
		for (const FSoftObjectPath& Path : *UniquePaths)
		{
			Context->ResolvedCollections.Add(Path, Cast<UPCGExAssetCollection>(Path.ResolveObject()));
		}
	}

	return true;
}

namespace PCGExGetCollectionData
{
	/** All the writable attribute pointers + property writer + entry list for a single unique output.
	 *  Pre-built single-threaded before the parallel write loop; iteration only touches its own slot.
	 *  In Merged fanout mode a single FUniqueOutput is shared across all unique collections (entries
	 *  from each are appended into Entries); bWantAssetPath/bWantAssetClass are pre-OR'd across the
	 *  set so the right attribute halves are declared. */
	struct FUniqueOutput
	{
		UPCGExAssetCollection* Collection = nullptr;  // primary collection (root for PropertyWriter / FixModuleInfos host context)
		UPCGParamData* OutputSet = nullptr;
		TSharedPtr<TArray<FFlattenedEntry>> Entries;
		bool bWantAssetPath = false;
		bool bWantAssetClass = false;

		// Per-output attribute pointers (set only when corresponding bWriteX is on).
		FPCGMetadataAttribute<FSoftObjectPath>* AssetPathAttr = nullptr;
		FPCGMetadataAttribute<FSoftClassPath>* AssetClassAttr = nullptr;
		FPCGMetadataAttribute<int32>* WeightAttrInt = nullptr;
		FPCGMetadataAttribute<float>* WeightAttrFloat = nullptr;
		FPCGMetadataAttribute<FName>* CategoryAttr = nullptr;
		FPCGMetadataAttribute<FVector>* ExtentsAttr = nullptr;
		FPCGMetadataAttribute<FVector>* BoundsMinAttr = nullptr;
		FPCGMetadataAttribute<FVector>* BoundsMaxAttr = nullptr;
		FPCGMetadataAttribute<int32>* NestingDepthAttr = nullptr;
		FPCGMetadataAttribute<FName>* SymbolAttr = nullptr;
		FPCGMetadataAttribute<double>* SizeAttr = nullptr;
		FPCGMetadataAttribute<bool>* ScalableAttr = nullptr;
		FPCGMetadataAttribute<FVector4>* DebugColorAttr = nullptr;
		FPCGMetadataAttribute<int64>* EntryAttr = nullptr;

		PCGExCollections::FPCGExCollectionPropertySetWriter PropertyWriter;
	};

	/** Derived settings + ambient state passed to ProcessUniqueOutput. Bundled so both the
	 *  Collection-mode fast path and the slot-based FromInputs path can share one helper
	 *  without dragging a dozen positional params. */
	struct FOutputProcessParams
	{
		const UPCGExGetCollectionDataSettings* Settings = nullptr;
		FPCGExContext* InContext = nullptr;
		PCGExCollections::FPickPacker* Packer = nullptr;
		const FProcessEntryContext* FlattenCtx = nullptr;

		bool bOutputWeight = false;
		EPCGExWeightNormalization WeightNorm = EPCGExWeightNormalization::None;
		bool bWeightAsFloat = false;
		bool bOutputCategory = false;
		bool bAnyGrammarField = false;
	};

	/** Append flattened entries from `Collection` into U.Entries. Never clobbers, so it's safe
	 *  to call multiple times on the same U (Merged fanout walks every unique collection into
	 *  one shared FUniqueOutput). */
	static void FlattenInto(FUniqueOutput& U, UPCGExAssetCollection* Collection, const FOutputProcessParams& P)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_Flatten);
		FlattenCollection(*P.FlattenCtx, Collection, *U.Entries);
	}

	/** Declares attributes on U.OutputSet, initializes the property writer, computes weight sums,
	 *  and writes every row in U.Entries. Call exactly once per output. */
	static void WriteFromEntries(FUniqueOutput& U, const FOutputProcessParams& P)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_WriteFromEntries);

		const UPCGExGetCollectionDataSettings* Settings = P.Settings;
		TArray<FFlattenedEntry>& Entries = *U.Entries;
		UPCGMetadata* Metadata = U.OutputSet->Metadata;

		// Choose which asset halves to declare. bWantAssetPath/bWantAssetClass are pre-computed
		// per-mode (single-collection: based on actor-vs-mesh type; merged: OR'd across all
		// contributing collections so heterogeneous mixes get both halves).
		const bool bOutputAssetPath = Settings->bWriteAssetPath && U.bWantAssetPath;
		const bool bOutputAssetClass = Settings->bWriteAssetPath && U.bWantAssetClass;

		// Declare attributes. CreateAttribute (vs FindOrCreate) skips the find lookup -- safe
		// because we just allocated a fresh empty Metadata.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_DeclareAttrs);
			if (bOutputAssetPath)
			{
				U.AssetPathAttr = Metadata->CreateAttribute<FSoftObjectPath>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->AssetPathAttributeName, U.OutputSet), FSoftObjectPath(), false, true);
			}
			if (bOutputAssetClass)
			{
				U.AssetClassAttr = Metadata->CreateAttribute<FSoftClassPath>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->AssetPathAttributeName, U.OutputSet), FSoftClassPath(), false, true);
			}
			if (P.bOutputWeight)
			{
				const FPCGAttributeIdentifier WeightId = PCGExMetaHelpers::GetAttributeIdentifier(Settings->WeightAttributeName, U.OutputSet);
				if (P.bWeightAsFloat)
				{
					U.WeightAttrFloat = Metadata->CreateAttribute<float>(WeightId, 0.0f, false, true);
				}
				else
				{
					U.WeightAttrInt = Metadata->CreateAttribute<int32>(WeightId, 0, false, true);
				}
			}
			if (P.bOutputCategory)
			{
				U.CategoryAttr = Metadata->CreateAttribute<FName>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->CategoryAttributeName, U.OutputSet), NAME_None, false, true);
			}
			if (Settings->bWriteExtents)
			{
				U.ExtentsAttr = Metadata->CreateAttribute<FVector>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->ExtentsAttributeName, U.OutputSet), FVector::OneVector, false, true);
			}
			if (Settings->bWriteBoundsMin)
			{
				U.BoundsMinAttr = Metadata->CreateAttribute<FVector>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->BoundsMinAttributeName, U.OutputSet), FVector::OneVector, false, true);
			}
			if (Settings->bWriteBoundsMax)
			{
				U.BoundsMaxAttr = Metadata->CreateAttribute<FVector>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->BoundsMaxAttributeName, U.OutputSet), FVector::OneVector, false, true);
			}
			if (Settings->bWriteNestingDepth)
			{
				U.NestingDepthAttr = Metadata->CreateAttribute<int32>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->NestingDepthAttributeName, U.OutputSet), -1, false, true);
			}
			if (Settings->bWriteSymbol)
			{
				U.SymbolAttr = Metadata->CreateAttribute<FName>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->SymbolAttributeName, U.OutputSet), NAME_None, false, true);
			}
			if (Settings->bWriteSize)
			{
				U.SizeAttr = Metadata->CreateAttribute<double>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->SizeAttributeName, U.OutputSet), 0.0, false, true);
			}
			if (Settings->bWriteScalable)
			{
				U.ScalableAttr = Metadata->CreateAttribute<bool>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->ScalableAttributeName, U.OutputSet), true, false, true);
			}
			if (Settings->bWriteDebugColor)
			{
				U.DebugColorAttr = Metadata->CreateAttribute<FVector4>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->DebugColorAttributeName, U.OutputSet), FVector4(1, 1, 1, 1), false, true);
			}
			U.EntryAttr = Metadata->CreateAttribute<int64>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->EntryAttributeName, U.OutputSet), 0, false, true);
		} // end DeclareAttrs scope

		// Property writer (gated -- skips an allocation per output when no properties configured).
		if (Settings->PropertyOutputSettings.HasOutputs())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_PropertyWriterInit);
			TArray<const UPCGExAssetCollection*> FallbackHosts;
			TSet<const UPCGExAssetCollection*> Seen;
			for (const FFlattenedEntry& EH : Entries)
			{
				if (!EH.Host || EH.Host == U.Collection)
				{
					continue;
				}
				bool bAlreadyIn = false;
				Seen.Add(EH.Host, &bAlreadyIn);
				if (!bAlreadyIn)
				{
					FallbackHosts.Add(EH.Host);
				}
			}
			U.PropertyWriter.Initialize(P.InContext, Settings->PropertyOutputSettings, U.Collection, FallbackHosts, Metadata);
		}

		// Weight normalization pre-pass (scoped to this output).
		double GlobalWeightSum = 0.0;
		TMap<FName, double> CategoryWeightSums;
		TMap<const UPCGExAssetCollection*, double> CollectionWeightSums;
		if (P.bWeightAsFloat)
		{
			for (const FFlattenedEntry& EH : Entries)
			{
				const FPCGExAssetCollectionEntry* E = EH.Entry;
				if (!E || E->bIsSubCollection)
				{
					continue;
				}
				const double W = E->Weight;
				switch (P.WeightNorm)
				{
				case EPCGExWeightNormalization::Global:
					GlobalWeightSum += W;
					break;
				case EPCGExWeightNormalization::PerCategory:
					CategoryWeightSums.FindOrAdd(EH.Category) += W;
					break;
				case EPCGExWeightNormalization::PerCollection:
					CollectionWeightSums.FindOrAdd(EH.Host) += W;
					break;
				default: ;
				}
			}
		}

		TMap<const FPCGExAssetCollectionEntry*, double> SizeCache;
		if (Settings->bWriteSize)
		{
			SizeCache.Reserve(Entries.Num());
		}

		TSet<FName> UniqueSymbols;
		if (Settings->bWriteSymbol)
		{
			UniqueSymbols.Reserve(Entries.Num());
		}

		// Write rows.
		TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_WriteRows);
		for (const FFlattenedEntry& EH : Entries)
		{
			const FPCGExAssetCollectionEntry* E = EH.Entry;

			FPCGSubdivisionSubmodule Module;
			bool bModuleResolved = false;
			if (E && P.bAnyGrammarField)
			{
				bModuleResolved = E->FixModuleInfos(EH.Host, Module, Settings->bWriteSize ? &SizeCache : nullptr);
			}

			if (Settings->bWriteSymbol && Settings->bSkipEmptySymbol && E && bModuleResolved && Module.Symbol.IsNone())
			{
				continue;
			}

			if (Settings->bWriteSymbol && !Settings->bAllowDuplicates && E && bModuleResolved)
			{
				bool bAlreadyInSet = false;
				UniqueSymbols.Add(Module.Symbol, &bAlreadyInSet);
				if (bAlreadyInSet)
				{
					continue;
				}
			}

			const int64 Key = Metadata->AddEntry();
			if (!E)
			{
				continue;
			}

			if (!E->bIsSubCollection)
			{
				if (U.AssetPathAttr)
				{
					U.AssetPathAttr->SetValue(Key, E->Staging.Path);
				}
				if (U.AssetClassAttr)
				{
					U.AssetClassAttr->SetValue(Key, FSoftClassPath(E->Staging.Path.ToString()));
				}
				if (U.WeightAttrInt)
				{
					U.WeightAttrInt->SetValue(Key, E->Weight);
				}
				else if (U.WeightAttrFloat)
				{
					double Denom = 0.0;
					switch (P.WeightNorm)
					{
					case EPCGExWeightNormalization::Global:
						Denom = GlobalWeightSum;
						break;
					case EPCGExWeightNormalization::PerCategory:
						Denom = CategoryWeightSums.FindRef(EH.Category);
						break;
					case EPCGExWeightNormalization::PerCollection:
						Denom = CollectionWeightSums.FindRef(EH.Host);
						break;
					default: ;
					}
					U.WeightAttrFloat->SetValue(Key, Denom > 0.0 ? static_cast<float>(static_cast<double>(E->Weight) / Denom) : 0.0f);
				}
				if (U.ExtentsAttr)
				{
					U.ExtentsAttr->SetValue(Key, E->Staging.Bounds.GetExtent());
				}
				if (U.BoundsMinAttr)
				{
					U.BoundsMinAttr->SetValue(Key, E->Staging.Bounds.Min);
				}
				if (U.BoundsMaxAttr)
				{
					U.BoundsMaxAttr->SetValue(Key, E->Staging.Bounds.Max);
				}
				if (U.NestingDepthAttr)
				{
					U.NestingDepthAttr->SetValue(Key, -1);
				}
			}

			if (U.CategoryAttr)
			{
				U.CategoryAttr->SetValue(Key, EH.Category);
			}
			if (bModuleResolved)
			{
				if (U.SymbolAttr)
				{
					U.SymbolAttr->SetValue(Key, Module.Symbol);
				}
				if (U.SizeAttr)
				{
					U.SizeAttr->SetValue(Key, Module.Size);
				}
				if (U.ScalableAttr)
				{
					U.ScalableAttr->SetValue(Key, Module.bScalable);
				}
				if (U.DebugColorAttr)
				{
					U.DebugColorAttr->SetValue(Key, Module.DebugColor);
				}
			}

			const uint64 Hash = P.Packer->GetPickIdx(EH.Host, E->Staging.InternalIndex, 0);
			U.EntryAttr->SetValue(Key, static_cast<int64>(Hash));

			U.PropertyWriter.WriteEntry(Key, E, EH.Host);
		}
	}

	/** Convenience wrapper used by the non-merged paths: flatten this output's single collection,
	 *  then write all rows. Safe to call in parallel across distinct FUniqueOutput instances. */
	static void ProcessUniqueOutput(FUniqueOutput& U, const FOutputProcessParams& P)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_ProcessUniqueOutput_Body);
		FlattenInto(U, U.Collection, P);
		WriteFromEntries(U, P);
	}
}

bool FPCGExGetCollectionDataElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExGetCollectionDataElement::AdvanceWork);

	PCGEX_SETTINGS_C(InContext, GetCollectionData)
	FPCGExGetCollectionDataContext* Context = static_cast<FPCGExGetCollectionDataContext*>(InContext);

	const bool bOutputWeight = Settings->bWriteWeight;
	const EPCGExWeightNormalization WeightNorm = bOutputWeight ? Settings->WeightNormalization : EPCGExWeightNormalization::None;
	const bool bWeightAsFloat = bOutputWeight && WeightNorm != EPCGExWeightNormalization::None;
	const bool bOutputCategory = Settings->bWriteCategory;
	const EPCGExCategoryInheritance CategoryInheritance = bOutputCategory ? Settings->CategoryInheritance : EPCGExCategoryInheritance::None;
	const bool bAnyGrammarField = Settings->bWriteSymbol || Settings->bWriteSize || Settings->bWriteScalable || Settings->bWriteDebugColor;

	FPCGExNameFiltersDetails CategoryFilters = Settings->CategoryFilters;
	CategoryFilters.Init();

	PCGExGetCollectionData::FProcessEntryContext Ctx;
	Ctx.Context = InContext;
	Ctx.CategoryFilters = &CategoryFilters;
	Ctx.SubHandling = Settings->SubCollectionHandling;
	Ctx.CategoryInheritance = CategoryInheritance;
	Ctx.bOmitInvalidAndEmpty = Settings->bOmitInvalidAndEmpty;
	Ctx.bNoDuplicates = !Settings->bAllowDuplicates;

	// Shared FPickPacker (covers both fast path and slot path).
	TSharedPtr<PCGExCollections::FPickPacker> Packer = MakeShared<PCGExCollections::FPickPacker>(InContext);

	// Bundle derived settings for ProcessUniqueOutput.
	PCGExGetCollectionData::FOutputProcessParams ProcessParams;
	ProcessParams.Settings = Settings;
	ProcessParams.InContext = InContext;
	ProcessParams.Packer = Packer.Get();
	ProcessParams.FlattenCtx = &Ctx;
	ProcessParams.bOutputWeight = bOutputWeight;
	ProcessParams.WeightNorm = WeightNorm;
	ProcessParams.bWeightAsFloat = bWeightAsFloat;
	ProcessParams.bOutputCategory = bOutputCategory;
	ProcessParams.bAnyGrammarField = bAnyGrammarField;

	auto EmitMap = [&]()
	{
		UPCGParamData* OutputMap = InContext->ManagedObjects->New<UPCGParamData>();
		Packer->PackToDataset(OutputMap);
		FPCGTaggedData& MapData = InContext->OutputData.TaggedData.Emplace_GetRef();
		MapData.Pin = PCGExCollections::Labels::OutputCollectionMapLabel;
		MapData.Data = OutputMap;
	};

	// =========================================================================================
	// Collection-mode fast path
	// =========================================================================================
	// Skips Slots / UniqueOutputs / CollectionToIndex / ResolvedCollections / ParallelOrSequential
	// dispatch entirely. Single FUniqueOutput, single ProcessUniqueOutput call. Hard-ref
	// TObjectPtr means the asset is already loaded -- no resolve, no map lookup.
	if (Settings->SourceMode == EPCGExGetCollectionDataSourceMode::Collection)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_AdvanceWork_CollectionFastPath);

		UPCGExAssetCollection* MainCollection = Settings->AssetCollection;
		InContext->OutputData.TaggedData.Reserve(InContext->OutputData.TaggedData.Num() + 2);

		PCGExGetCollectionData::FUniqueOutput U;
		U.Collection = MainCollection;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_AllocOutputSet);
			U.OutputSet = InContext->ManagedObjects->New<UPCGParamData>();
		}
		U.Entries = MakeShared<TArray<PCGExGetCollectionData::FFlattenedEntry>>();
		{
			const bool bIsActor = Cast<UPCGExActorCollection>(MainCollection) != nullptr;
			U.bWantAssetPath = !bIsActor;
			U.bWantAssetClass = bIsActor;
		}

		if (MainCollection)
		{
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_RegisterTrackingKeys);
				MainCollection->EDITOR_RegisterTrackingKeys(InContext);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_RegisterPacker);
				Packer->RegisterCollection(MainCollection);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_ProcessUniqueOutput);
				PCGExGetCollectionData::ProcessUniqueOutput(U, ProcessParams);
			}
		}

		FPCGTaggedData& OutData = InContext->OutputData.TaggedData.Emplace_GetRef();
		OutData.Pin = PCGExGetCollectionData::OutputAttributeSetPin;
		OutData.Data = U.OutputSet;
		if (!MainCollection || U.Entries->IsEmpty())
		{
			OutData.Tags.Add(PCGExGetCollectionData::EmptyTag.ToString());
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_EmitMap);
			EmitMap();
		}
		InContext->Done();
		return InContext->TryComplete();
	}

	// =========================================================================================
	// FromInputs - Merged fanout
	// =========================================================================================
	// One shared FUniqueOutput receives entries from every unique collection (append in encounter
	// order). One FPCGTaggedData emitted. AssetPath/AssetClass attributes are declared based on
	// the union of contributing collection types so heterogeneous mixes get both halves.
	if (Settings->Fanout == EPCGExGetCollectionDataFanout::Merged)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GetCollectionData_AdvanceWork_Merged);

		// Walk slots to discover unique collections (dedupe flatten work) and pre-OR the
		// asset-half flags.
		TArray<UPCGExAssetCollection*> UniqueCollections;
		TSet<UPCGExAssetCollection*> SeenCollections;
		bool bAnyActor = false;
		bool bAnyNonActor = false;
		for (const FPCGExGetCollectionDataContext::FSlot& Slot : Context->Slots)
		{
			UPCGExAssetCollection** ResolvedPtr = Context->ResolvedCollections.Find(Slot.Path);
			UPCGExAssetCollection* Collection = (ResolvedPtr && *ResolvedPtr) ? *ResolvedPtr : nullptr;
			if (!Collection) { continue; }
			bool bAlreadyIn = false;
			SeenCollections.Add(Collection, &bAlreadyIn);
			if (bAlreadyIn) { continue; }

			UniqueCollections.Add(Collection);
			Collection->EDITOR_RegisterTrackingKeys(InContext);
			if (Cast<UPCGExActorCollection>(Collection)) { bAnyActor = true; }
			else { bAnyNonActor = true; }
		}

		// Allocate the single shared output. Packer registration covers every contributing host.
		PCGExGetCollectionData::FUniqueOutput Merged;
		Merged.Collection = UniqueCollections.IsEmpty() ? nullptr : UniqueCollections[0];
		Merged.OutputSet = InContext->ManagedObjects->New<UPCGParamData>();
		Merged.Entries = MakeShared<TArray<PCGExGetCollectionData::FFlattenedEntry>>();
		Merged.bWantAssetPath = bAnyNonActor;
		Merged.bWantAssetClass = bAnyActor;

		for (UPCGExAssetCollection* Collection : UniqueCollections)
		{
			Packer->RegisterCollection(Collection);
			PCGExGetCollectionData::FlattenInto(Merged, Collection, ProcessParams);
		}

		// Single-shot declare + write.
		if (!Merged.Entries->IsEmpty())
		{
			PCGExGetCollectionData::WriteFromEntries(Merged, ProcessParams);
		}

		// Emit one FPCGTaggedData. Tag forwarding (if on) unions tags from every contributing
		// source input -- per-slot identity is lost in Merged mode by design.
		InContext->OutputData.TaggedData.Reserve(InContext->OutputData.TaggedData.Num() + 2);

		FPCGTaggedData& OutData = InContext->OutputData.TaggedData.Emplace_GetRef();
		OutData.Pin = PCGExGetCollectionData::OutputAttributeSetPin;
		OutData.Data = Merged.OutputSet;
		if (Merged.Entries->IsEmpty())
		{
			OutData.Tags.Add(PCGExGetCollectionData::EmptyTag.ToString());
		}
		if (Settings->bForwardInputTags)
		{
			TArray<FPCGTaggedData> Inputs = InContext->InputData.GetInputsByPin(PCGExGetCollectionData::SourcesPin);
			TSet<FString> TagUnion;
			for (const FPCGExGetCollectionDataContext::FSlot& Slot : Context->Slots)
			{
				if (Inputs.IsValidIndex(Slot.SourceInputIndex))
				{
					TagUnion.Append(Inputs[Slot.SourceInputIndex].Tags);
				}
			}
			OutData.Tags.Append(TagUnion);
		}

		EmitMap();
		InContext->Done();
		return InContext->TryComplete();
	}

	// =========================================================================================
	// FromInputs slot-based path (PerEntry / PerInputData)
	// =========================================================================================

	// Phase 1 (single-threaded): pre-allocate one UPCGParamData per unique collection.
	// Multiple slots pointing at the same collection share the same output Data pointer
	// (their FPCGTaggedData entries hold the per-slot tags).
	TArray<PCGExGetCollectionData::FUniqueOutput> UniqueOutputs;
	TMap<UPCGExAssetCollection*, int32> CollectionToIndex;

	for (const FPCGExGetCollectionDataContext::FSlot& Slot : Context->Slots)
	{
		UPCGExAssetCollection** ResolvedPtr = Context->ResolvedCollections.Find(Slot.Path);
		UPCGExAssetCollection* Collection = (ResolvedPtr && *ResolvedPtr) ? *ResolvedPtr : nullptr;
		if (!Collection)
		{
			continue;
		}
		if (CollectionToIndex.Contains(Collection))
		{
			continue;
		}

		const int32 Idx = UniqueOutputs.Num();
		CollectionToIndex.Add(Collection, Idx);

		PCGExGetCollectionData::FUniqueOutput& U = UniqueOutputs.AddDefaulted_GetRef();
		U.Collection = Collection;
		U.OutputSet = InContext->ManagedObjects->New<UPCGParamData>();
		U.Entries = MakeShared<TArray<PCGExGetCollectionData::FFlattenedEntry>>();
		{
			const bool bIsActor = Cast<UPCGExActorCollection>(Collection) != nullptr;
			U.bWantAssetPath = !bIsActor;
			U.bWantAssetClass = bIsActor;
		}

		Collection->EDITOR_RegisterTrackingKeys(InContext);
	}

	// Reserve TaggedData up-front (one entry per slot + one for the map) so the slot emission
	// loop doesn't pay for TArray growth reallocations.
	InContext->OutputData.TaggedData.Reserve(InContext->OutputData.TaggedData.Num() + Context->Slots.Num() + 1);

	// Phase 2 (single-threaded): packer registration.
	for (PCGExGetCollectionData::FUniqueOutput& U : UniqueOutputs)
	{
		Packer->RegisterCollection(U.Collection);
	}

	// Phase 3 (parallel over unique collections): full per-output work via the shared helper.
	// Threshold=2 (default 512 would never trigger here); Unbalanced because per-iteration cost
	// varies dramatically (small leaf-only collection vs deeply nested grammar tree).
	PCGExMT::ParallelOrSequential(UniqueOutputs.Num(), [&](const int32 i)
	{
		PCGExGetCollectionData::ProcessUniqueOutput(UniqueOutputs[i], ProcessParams);
	}, /*Threshold=*/2, EParallelForFlags::Unbalanced);

	// Phase 4 (single-threaded): emit one FPCGTaggedData per slot. Multiple slots can share the
	// same UPCGParamData pointer -- tags live on the tagged-data wrapper, not the data itself.
	// Re-fetch input list only if we'll actually forward tags (slot.SourceInputIndex is stable
	// across the lifetime of InContext, so re-querying gives the same order).
	TArray<FPCGTaggedData> InputsForTags;
	const bool bWillForwardTags = Settings->bForwardInputTags && Settings->SourceMode == EPCGExGetCollectionDataSourceMode::FromInputs;
	if (bWillForwardTags)
	{
		InputsForTags = InContext->InputData.GetInputsByPin(PCGExGetCollectionData::SourcesPin);
	}

	UPCGParamData* EmptySentinel = nullptr;
	auto GetOrCreateEmpty = [&]() -> UPCGParamData*
	{
		if (!EmptySentinel)
		{
			EmptySentinel = InContext->ManagedObjects->New<UPCGParamData>();
		}
		return EmptySentinel;
	};

	for (const FPCGExGetCollectionDataContext::FSlot& Slot : Context->Slots)
	{
		UPCGExAssetCollection** ResolvedPtr = Context->ResolvedCollections.Find(Slot.Path);
		UPCGExAssetCollection* Collection = (ResolvedPtr && *ResolvedPtr) ? *ResolvedPtr : nullptr;

		FPCGTaggedData& OutData = InContext->OutputData.TaggedData.Emplace_GetRef();
		OutData.Pin = PCGExGetCollectionData::OutputAttributeSetPin;

		if (!Collection)
		{
			OutData.Data = GetOrCreateEmpty();
			OutData.Tags.Add(PCGExGetCollectionData::EmptyTag.ToString());
		}
		else
		{
			const int32 Idx = CollectionToIndex.FindChecked(Collection);
			const PCGExGetCollectionData::FUniqueOutput& U = UniqueOutputs[Idx];
			if (U.Entries->IsEmpty())
			{
				OutData.Data = GetOrCreateEmpty();
				OutData.Tags.Add(PCGExGetCollectionData::EmptyTag.ToString());
			}
			else
			{
				OutData.Data = U.OutputSet;
			}
		}

		if (bWillForwardTags && InputsForTags.IsValidIndex(Slot.SourceInputIndex))
		{
			OutData.Tags.Append(InputsForTags[Slot.SourceInputIndex].Tags);
		}
	}

	// Phase 5: always emit the shared Map.
	EmitMap();

	InContext->Done();
	return InContext->TryComplete();
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
