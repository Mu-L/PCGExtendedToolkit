// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExDispatchSubgraph.h"

#include "PCGCommon.h"
#include "PCGGraph.h"
#include "PCGPin.h"
#include "PCGSubgraph.h"
#include "Data/PCGUserParametersData.h"
#include "Graph/PCGStackContext.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/Accessors/IPCGAttributeAccessor.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Helpers/PCGExBulkAttributeHelpers.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Helpers/PCGExPropertyHelpers.h"
#include "Types/PCGExTypes.h"
#include "StructUtils/PropertyBag.h"

#define LOCTEXT_NAMESPACE "PCGExDispatchSubgraphElement"
#define PCGEX_NAMESPACE DispatchSubgraph

namespace PCGExDispatchSubgraph
{
	const FName SourceDriversLabel = FName("Drivers");

	// Type-erased per-entry reader for one source attribute on one driver data. Values are read once in
	// their natural type (BulkReadRows); Hash() feeds the group key, WriteTo() coerces into a param bag.
	struct IOverrideReader
	{
		virtual ~IOverrideReader() = default;
		virtual uint32 Hash(int32 EntryIndex) const = 0;
		virtual bool WriteTo(int32 EntryIndex, void* BagMemory, const FProperty* Property) const = 0;
	};

	template <typename T>
	struct TOverrideReader final : IOverrideReader
	{
		TArray<T> Values;

		virtual uint32 Hash(const int32 EntryIndex) const override
		{
			return Values.IsValidIndex(EntryIndex) ? PCGExTypes::ComputeHash(Values[EntryIndex]) : 0;
		}

		virtual bool WriteTo(const int32 EntryIndex, void* BagMemory, const FProperty* Property) const override
		{
			if (!Values.IsValidIndex(EntryIndex))
			{
				return false;
			}

			// Primary: the engine property accessor with broadcast+construct handles targets PCGEx's own
			// TrySetFPropertyValue does not (FLinearColor, FColor, ...) plus every standard numeric/vector conversion.
			if (const TUniquePtr<IPCGAttributeAccessor> Accessor = PCGAttributeAccessorHelpers::CreatePropertyAccessor(Property))
			{
				void* Containers[1] = {BagMemory};
				FPCGAttributeAccessorKeysGenericPtrs Keys(Containers);
				if (Accessor->Set<T>(Values[EntryIndex], Keys, EPCGAttributeAccessorFlags::AllowBroadcastAndConstructible))
				{
					return true;
				}
			}

			// Fallback: targets the accessor doesn't support (e.g. object properties driven by a soft path).
			return PCGExPropertyHelpers::TrySetFPropertyValue<T>(BagMemory, const_cast<FProperty*>(Property), Values[EntryIndex]);
		}
	};

	// Reader for Struct / extended attribute types (e.g. the Struct attributes emitted by the Tuple
	// node, on points OR param). Reads and writes through the engine's generic accessor: the write is a
	// same-struct copy into the target parameter; the group hash uses PCG's per-value key.
	struct FGenericOverrideReader final : IOverrideReader
	{
		TUniquePtr<const IPCGAttributeAccessor> SrcAccessor;
		TSharedPtr<IPCGAttributeAccessorKeys> SrcKeys;
		const FPCGMetadataAttributeBase* Attr = nullptr;
		const UPCGBasePointData* PointData = nullptr; // non-null when the driver is point data
		int32 DriverIndex = 0;

		virtual uint32 Hash(const int32 EntryIndex) const override
		{
			// PCG value keys are unique per distinct value but only WITHIN one attribute instance, so we
			// fold the driver index in. Consequence: Struct/extended-valued dedup is PER DRIVER DATA --
			// identical values arriving on two separate driver inputs dispatch separately (an
			// over-dispatch, never a wrong result). [DOC: surface this in the user-facing docs.]
			const PCGMetadataEntryKey EntryKey = PointData ? PointData->GetMetadataEntry(EntryIndex) : static_cast<PCGMetadataEntryKey>(EntryIndex);
			return HashCombineFast(static_cast<uint32>(DriverIndex), static_cast<uint32>(Attr->GetValueKey(EntryKey)));
		}

		virtual bool WriteTo(const int32 EntryIndex, void* BagMemory, const FProperty* Property) const override
		{
			if (!SrcAccessor.IsValid() || !SrcKeys.IsValid()) { return false; }

			// bUseGenericAccessor=true builds an FPCGPropertyGenericAccessor, which handles arbitrary
			// structs (and other extended types) that the typed accessor / TrySetFPropertyValue can't.
			const TUniquePtr<IPCGAttributeAccessor> TargetAccessor = PCGAttributeAccessorHelpers::CreatePropertyAccessor(Property, /*bUseGenericAccessor=*/true);
			if (!TargetAccessor) { return false; }

			void* Containers[1] = {BagMemory};
			FPCGAttributeAccessorKeysGenericPtrs TargetKeys(Containers);
			return SrcAccessor->CopyTo(*SrcKeys, *TargetAccessor, TargetKeys, /*InputIndex=*/EntryIndex, /*OutputIndex=*/0, /*Count=*/1, EPCGAttributeAccessorFlags::AllowBroadcastAndConstructible);
		}
	};

	// Builds a generic reader for a Struct/extended attribute, or null if the engine cannot build a
	// const accessor for it.
	TSharedPtr<IOverrideReader> MakeGenericOverrideReader(const UPCGData* InData, const FName InSourceAttr, const FPCGMetadataAttributeBase* InAttr, const int32 InDriverIndex)
	{
		FPCGAttributePropertyInputSelector Selector;
		Selector.Update(InSourceAttr.ToString());
		Selector = Selector.CopyAndFixLast(InData);

		TUniquePtr<const IPCGAttributeAccessor> Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(InData, Selector);
		TSharedPtr<IPCGAttributeAccessorKeys> Keys = PCGExData::Helpers::GetKeys(InData);
		if (!Accessor || !Keys) { return nullptr; }

		TSharedPtr<FGenericOverrideReader> Reader = MakeShared<FGenericOverrideReader>();
		Reader->SrcAccessor = MoveTemp(Accessor);
		Reader->SrcKeys = Keys;
		Reader->Attr = InAttr;
		Reader->PointData = Cast<UPCGBasePointData>(InData);
		Reader->DriverIndex = InDriverIndex;
		return Reader;
	}

	// Builds a reader for InSourceAttr on InData, or null if the attribute is missing. Basic single-value
	// types use the fast typed path (content hash, cross-data dedup); Struct/extended types fall back to
	// the generic accessor path (per-driver-data dedup -- see FGenericOverrideReader).
	TSharedPtr<IOverrideReader> MakeOverrideReader(const UPCGData* InData, const FName InSourceAttr, const int32 InDriverIndex)
	{
		const UPCGMetadata* Metadata = InData ? InData->ConstMetadata() : nullptr;
		if (!Metadata)
		{
			return nullptr;
		}

		const FPCGMetadataAttributeBase* Attr = Metadata->GetConstAttribute(InSourceAttr);
		if (!Attr)
		{
			return nullptr;
		}

		TSharedPtr<IOverrideReader> Result;
		PCGExMetaHelpers::ExecuteWithRightType(Attr, [&](auto Dummy)
		{
			using T = decltype(Dummy);
			TSharedPtr<TOverrideReader<T>> Reader = MakeShared<TOverrideReader<T>>();
			PCGExData::Helpers::BulkReadRows<T>(InData, InSourceAttr, Reader->Values);
			Result = Reader;
		});
		if (Result)
		{
			return Result;
		}

		// Non-basic (Struct / extended) attribute -> generic accessor path.
		return MakeGenericOverrideReader(InData, InSourceAttr, Attr, InDriverIndex);
	}
}

#pragma region Settings

#if WITH_EDITOR
FLinearColor UPCGExDispatchSubgraphSettings::GetNodeTitleColor() const
{
	return PCGEX_NODE_COLOR_OPTIN_NAME(Action);
}
#endif

TArray<FPCGPinProperties> UPCGExDispatchSubgraphSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	// Points-or-param via Any (a literal Point|Param union renders gray); Boot ignores unusable rows.
	PCGEX_PIN_ANY(PCGExDispatchSubgraph::SourceDriversLabel, TEXT("Points or attribute sets carrying the subgraph reference and override sources. One dispatch per unique (graph + overrides)."), Required)
	PinProperties.Append(CustomInputPins);
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExDispatchSubgraphSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PinProperties.Append(CustomOutputPins);
	PCGEX_PIN_ANY(PCGPinConstants::DefaultOutputLabel, TEXT("Subgraph outputs that did not match a custom output pin by name."), Normal)
	return PinProperties;
}

FPCGElementPtr UPCGExDispatchSubgraphSettings::CreateElement() const
{
	return MakeShared<FPCGExDispatchSubgraphElement>();
}

#pragma endregion

#pragma region Element

void FPCGExDispatchSubgraphContext::RegisterAssetDependencies()
{
	PCGEX_SETTINGS_LOCAL(DispatchSubgraph)

	const TArray<FPCGTaggedData> Drivers = InputData.GetInputsByPin(PCGExDispatchSubgraph::SourceDriversLabel);
	DriverPaths.Reserve(Drivers.Num());

	for (const FPCGTaggedData& TaggedData : Drivers)
	{
		TArray<FSoftObjectPath>& Paths = DriverPaths.Emplace_GetRef();
		if (!TaggedData.Data)
		{
			continue;
		}

		PCGExData::Helpers::BulkReadSoftPaths(TaggedData.Data, Settings->GraphPathAttribute, Paths);

		for (const FSoftObjectPath& Path : Paths)
		{
			if (Path.IsNull())
			{
				continue;
			} // empty reference -> ignored, no warning

			bool bAlreadyRegistered = false;
			UniqueGraphPaths.Add(Path, &bAlreadyRegistered);
			if (!bAlreadyRegistered)
			{
				AddAssetDependency(Path);
			}
		}
	}

	FPCGExContext::RegisterAssetDependencies();
}

void FPCGExDispatchSubgraphContext::AddToReferencedObjects(const FPCGDataCollection& InCollection)
{
	for (const FPCGTaggedData& TaggedData : InCollection.TaggedData)
	{
		if (TaggedData.Data)
		{
			ReferencedObjects.Add(TaggedData.Data);
		}
	}
}

void FPCGExDispatchSubgraphContext::AddExtraStructReferencedObjects(FReferenceCollector& Collector)
{
	FPCGExContext::AddExtraStructReferencedObjects(Collector);
	Collector.AddReferencedObjects(ReferencedObjects);
}

bool FPCGExDispatchSubgraphElement::Boot(FPCGExContext* InContext) const
{
	if (!IPCGExElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(DispatchSubgraph)
	PCGEX_VALIDATE_NAME(Settings->GraphPathAttribute)

	return true;
}

void FPCGExDispatchSubgraphElement::PostLoadAssetsDependencies(FPCGExContext* InContext) const
{
	PCGEX_CONTEXT(DispatchSubgraph)

	Context->ResolvedGraphs.Reserve(Context->UniqueGraphPaths.Num());
	for (const FSoftObjectPath& Path : Context->UniqueGraphPaths)
	{
		UPCGGraph* Graph = TSoftObjectPtr<UPCGGraph>(Path).Get();
		Context->ResolvedGraphs.Add(Path, Graph);

		if (!Graph)
		{
			const FString Message = TEXT("Subgraph could not be loaded : ") + Path.ToString();
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::FromString(Message));
		}
	}
}

bool FPCGExDispatchSubgraphElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExDispatchSubgraphElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(DispatchSubgraph)
	PCGEX_EXECUTION_CHECK

	if (!Context->bDispatched)
	{
		Context->bDispatched = true;

		// Schedule the dynamic subgraphs. If any were scheduled, park until they complete: the PCG
		// executor clears bIsPaused and re-ticks us once every DynamicDependencies task is done.
		if (ScheduleDispatches(Context, Settings))
		{
			Context->bIsPaused = true;
			return false;
		}
	}
	else
	{
		GatherDispatchOutputs(Context, Settings);
	}

	Context->Done();
	return Context->TryComplete();
}

bool FPCGExDispatchSubgraphElement::ScheduleDispatches(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings) const
{
	if (!Context->ExecutionSource.IsValid())
	{
		PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("No execution source is available; cannot dispatch subgraphs."));
		return false;
	}

	const TArray<FPCGTaggedData> Drivers = Context->InputData.GetInputsByPin(PCGExDispatchSubgraph::SourceDriversLabel);

	const FPCGStack* Stack = Context->GetStack();
	const UPCGGraph* CurrentGraph = Stack ? Stack->GetGraphForCurrentFrame() : nullptr;

	TSet<UPCGGraph*> RecursiveGraphs; // warned + skipped once
	TSet<uint32> DispatchedKeys;      // (graph + overrides) groups already scheduled
	int32 LoopIndex = 0;

	for (int32 DriverIndex = 0; DriverIndex < Drivers.Num(); ++DriverIndex)
	{
		const UPCGData* Data = Drivers[DriverIndex].Data;
		if (!Data || !Context->DriverPaths.IsValidIndex(DriverIndex))
		{
			continue;
		}

		const TArray<FSoftObjectPath>& Paths = Context->DriverPaths[DriverIndex];

		// Driver attribute names, for auto-match-by-name.
		TArray<FName> DriverAttrNames;
		if (Settings->bAutoMatchByName && Data->ConstMetadata())
		{
			TArray<EPCGMetadataTypes> DriverAttrTypes;
			Data->ConstMetadata()->GetAttributes(DriverAttrNames, DriverAttrTypes);
		}

		// Lazily built, cached per-attribute readers on this driver data.
		TMap<FName, TSharedPtr<PCGExDispatchSubgraph::IOverrideReader>> Readers;
		auto GetReader = [&](const FName Attr) -> TSharedPtr<PCGExDispatchSubgraph::IOverrideReader>
		{
			if (const TSharedPtr<PCGExDispatchSubgraph::IOverrideReader>* Found = Readers.Find(Attr))
			{
				return *Found;
			}
			TSharedPtr<PCGExDispatchSubgraph::IOverrideReader> Reader = PCGExDispatchSubgraph::MakeOverrideReader(Data, Attr, DriverIndex);
			Readers.Add(Attr, Reader);
			return Reader;
		};

		for (int32 EntryIndex = 0; EntryIndex < Paths.Num(); ++EntryIndex)
		{
			const FSoftObjectPath& Path = Paths[EntryIndex];
			if (Path.IsNull())
			{
				continue;
			}

			UPCGGraph* Graph = Context->ResolvedGraphs.FindRef(Path);
			if (!Graph || RecursiveGraphs.Contains(Graph))
			{
				continue;
			}

			// Recursion guard (warn once per graph): never dispatch a graph that is, or contains, the graph currently executing.
			if ((Stack && Stack->HasObject(Graph)) || (CurrentGraph && Graph->Contains(CurrentGraph)))
			{
				RecursiveGraphs.Add(Graph);
				PCGE_LOG_C(Warning, GraphAndLog, Context, FText::FromString(TEXT("Skipped a recursive subgraph reference : ") + Graph->GetName()));
				continue;
			}

			const FInstancedPropertyBag* GraphBag = Graph->GetUserParametersStruct();

			// Resolve the overrides that apply to this (graph, driver-data, entry): a target param that exists
			// on the graph, fed by a source attribute that exists on the driver data. Remap wins over auto-match.
			struct FApplied
			{
				FName Param;
				FName Src;
				const FPropertyBagPropertyDesc* Desc;
			};
			TArray<FApplied> Applied;
			auto TryAddOverride = [&](const FName Param, const FName Src)
			{
				if (!GraphBag)
				{
					return;
				}
				if (Applied.ContainsByPredicate([Param](const FApplied& A)
				{
					return A.Param == Param;
				}))
				{
					return;
				}
				const FPropertyBagPropertyDesc* Desc = GraphBag->FindPropertyDescByName(Param);
				if (!Desc || !Desc->CachedProperty)
				{
					return;
				}
				if (!GetReader(Src).IsValid())
				{
					return;
				}
				Applied.Add({Param, Src, Desc});
			};

			for (const TPair<FName, FName>& Remap : Settings->OverrideRemap)
			{
				TryAddOverride(Remap.Value, Remap.Key);
			}
			if (Settings->bAutoMatchByName)
			{
				for (const FName Attr : DriverAttrNames)
				{
					TryAddOverride(Attr, Attr);
				}
			}

			Applied.Sort([](const FApplied& A, const FApplied& B)
			{
				return A.Param.LexicalLess(B.Param);
			});

			// Group key = graph identity + each applied (param name, per-entry source value).
			uint32 Key = GetTypeHash(Graph);
			for (const FApplied& A : Applied)
			{
				Key = HashCombineFast(Key, GetTypeHash(A.Param));
				Key = HashCombineFast(Key, GetReader(A.Src)->Hash(EntryIndex));
			}

			bool bAlreadyDispatched = false;
			DispatchedKeys.Add(Key, &bAlreadyDispatched);
			if (bAlreadyDispatched)
			{
				continue;
			} // identical (graph + overrides) already scheduled

			// Pre-graph user parameters: graph defaults with this entry's overrides written in.
			FPCGDataCollection PreGraphData;
			{
				UPCGUserParametersData* UserParamData = FPCGContext::NewObject_AnyThread<UPCGUserParametersData>(Context);
				if (GraphBag)
				{
					FInstancedPropertyBag Bag = *GraphBag; // copy the defaults
					void* BagMemory = Bag.GetMutableValue().GetMemory();
					for (const FApplied& A : Applied)
					{
						GetReader(A.Src)->WriteTo(EntryIndex, BagMemory, A.Desc->CachedProperty);
					}
					UserParamData->UserParameters = Bag.GetValue();
				}

				FPCGTaggedData& Tagged = PreGraphData.TaggedData.Emplace_GetRef();
				Tagged.Data = UserParamData;
				Tagged.Tags.Add(PCG::Private::UserParameterTagData);
				Tagged.bPinlessData = true;
			}
			Context->AddToReferencedObjects(PreGraphData);

			FPCGDataCollection DispatchInput;
			BuildDispatchInput(Context, Settings, Graph, DispatchInput);
			Context->AddToReferencedObjects(DispatchInput);

			// Distinct invocation stack per dispatch (this node + a not-a-loop index), so the executor
			// keeps each scheduled subgraph's tasks/outputs distinct.
			FPCGStack InvocationStack = Stack ? *Stack : FPCGStack();
			InvocationStack.GetStackFramesMutable().Emplace(Context->Node);
			InvocationStack.GetStackFramesMutable().Emplace(LoopIndex++);

			const FPCGTaskId TaskId = Context->ScheduleGraph(FPCGScheduleGraphParams(
				Graph,
				Context->ExecutionSource.Get(),
				MakeShared<FPCGInputForwardingElement>(PreGraphData),
				MakeShared<FPCGInputForwardingElement>(DispatchInput),
				/*Dependencies=*/{},
				&InvocationStack,
				/*bAllowHierarchicalGeneration=*/false));

			if (TaskId == InvalidPCGTaskId)
			{
				continue;
			}

			FPCGExDispatchSubgraphContext::FDispatch& Dispatch = Context->Dispatches.Emplace_GetRef();
			Dispatch.Graph = Graph;
			Dispatch.TaskId = TaskId;
			Context->DynamicDependencies.Add(TaskId);
		}
	}

	return !Context->Dispatches.IsEmpty();
}

void FPCGExDispatchSubgraphElement::GatherDispatchOutputs(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings) const
{
	TSet<FName> CustomOutputLabels;
	CustomOutputLabels.Reserve(Settings->CustomOutputPins.Num());
	for (const FPCGPinProperties& Pin : Settings->CustomOutputPins)
	{
		CustomOutputLabels.Add(Pin.Label);
	}

	for (const FPCGExDispatchSubgraphContext::FDispatch& Dispatch : Context->Dispatches)
	{
		if (Dispatch.TaskId == InvalidPCGTaskId)
		{
			continue;
		}

		FPCGDataCollection SubgraphOutput;
		if (!Context->GetOutputData(Dispatch.TaskId, SubgraphOutput))
		{
			continue;
		}

		// Root the gathered data before clearing the executor's copy: StageOutput(None) records it for
		// the OnComplete flush but does not GC-root it, and ClearOutputData releases the executor's hold.
		Context->AddToReferencedObjects(SubgraphOutput);
		Context->IncreaseStagedOutputReserve(SubgraphOutput.TaggedData.Num());

		for (const FPCGTaggedData& TaggedData : SubgraphOutput.TaggedData)
		{
			if (!TaggedData.Data)
			{
				continue;
			}
			const FName Pin = CustomOutputLabels.Contains(TaggedData.Pin) ? TaggedData.Pin : PCGPinConstants::DefaultOutputLabel;
			Context->StageOutput(const_cast<UPCGData*>(TaggedData.Data.Get()), Pin, PCGExData::EStaging::None, TaggedData.Tags);
		}

		Context->ClearOutputData(Dispatch.TaskId);
	}
}

void FPCGExDispatchSubgraphElement::BuildDispatchInput(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings, const UPCGGraph* Graph, FPCGDataCollection& OutData) const
{
	// Model C: forward each declared custom input pin whose label matches one of the graph's input pins.
	// The Drivers pin is never forwarded.
	const UPCGNode* InputNode = Graph->GetInputNode();
	if (!InputNode)
	{
		return;
	}

	TSet<FName> GraphInputLabels;
	for (const FPCGPinProperties& Pin : InputNode->InputPinProperties())
	{
		GraphInputLabels.Add(Pin.Label);
	}

	for (const FPCGPinProperties& CustomPin : Settings->CustomInputPins)
	{
		if (!GraphInputLabels.Contains(CustomPin.Label))
		{
			continue;
		}
		OutData.TaggedData.Append(Context->InputData.GetInputsByPin(CustomPin.Label));
	}
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
