// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExDispatchSubgraph.h"

#include "PCGCommon.h"
#include "PCGGraph.h"
#include "PCGPin.h"
#include "PCGSubgraph.h"
#include "Data/PCGUserParametersData.h"
#include "Graph/PCGStackContext.h"
#include "Helpers/PCGExBulkAttributeHelpers.h"
#include "Helpers/PCGExMetaHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExDispatchSubgraphElement"
#define PCGEX_NAMESPACE DispatchSubgraph

namespace PCGExDispatchSubgraph
{
	const FName SourceDriversLabel = FName("Drivers");
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
		if (!TaggedData.Data) { continue; }

		PCGExData::Helpers::BulkReadSoftPaths(TaggedData.Data, Settings->GraphPathAttribute, Paths);

		for (const FSoftObjectPath& Path : Paths)
		{
			if (Path.IsNull()) { continue; } // empty reference -> ignored, no warning

			bool bAlreadyRegistered = false;
			UniqueGraphPaths.Add(Path, &bAlreadyRegistered);
			if (!bAlreadyRegistered) { AddAssetDependency(Path); }
		}
	}

	FPCGExContext::RegisterAssetDependencies();
}

void FPCGExDispatchSubgraphContext::AddToReferencedObjects(const FPCGDataCollection& InCollection)
{
	for (const FPCGTaggedData& TaggedData : InCollection.TaggedData)
	{
		if (TaggedData.Data) { ReferencedObjects.Add(TaggedData.Data); }
	}
}

void FPCGExDispatchSubgraphContext::AddExtraStructReferencedObjects(FReferenceCollector& Collector)
{
	FPCGExContext::AddExtraStructReferencedObjects(Collector);
	Collector.AddReferencedObjects(ReferencedObjects);
}

bool FPCGExDispatchSubgraphElement::Boot(FPCGExContext* InContext) const
{
	if (!IPCGExElement::Boot(InContext)) { return false; }

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

	// Unique resolved graphs (deduped by pointer; grouping by overrides comes next).
	TSet<UPCGGraph*> UniqueGraphs;
	UniqueGraphs.Reserve(Context->ResolvedGraphs.Num());
	for (const TPair<FSoftObjectPath, UPCGGraph*>& Pair : Context->ResolvedGraphs)
	{
		if (Pair.Value) { UniqueGraphs.Add(Pair.Value); }
	}

	if (UniqueGraphs.IsEmpty()) { return false; }

	const FPCGStack* Stack = Context->GetStack();
	const UPCGGraph* CurrentGraph = Stack ? Stack->GetGraphForCurrentFrame() : nullptr;

	int32 LoopIndex = 0;
	for (UPCGGraph* Graph : UniqueGraphs)
	{
		const int32 DispatchIndex = LoopIndex++;

		// Recursion guard: never dispatch a graph that is, or contains, the graph currently executing.
		if ((Stack && Stack->HasObject(Graph)) || (CurrentGraph && Graph->Contains(CurrentGraph)))
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FText::FromString(TEXT("Skipped a recursive subgraph reference : ") + Graph->GetName()));
			continue;
		}

		FPCGDataCollection PreGraphData;
		BuildUserParameters(Context, Graph, PreGraphData);
		Context->AddToReferencedObjects(PreGraphData);

		FPCGDataCollection InputData;
		BuildDispatchInput(Context, Settings, Graph, InputData);
		Context->AddToReferencedObjects(InputData);

		// Distinct invocation stack per dispatch (this node + a not-a-loop index), so the executor
		// keeps each scheduled subgraph's tasks/outputs distinct.
		FPCGStack InvocationStack = Stack ? *Stack : FPCGStack();
		InvocationStack.GetStackFramesMutable().Emplace(Context->Node);
		InvocationStack.GetStackFramesMutable().Emplace(DispatchIndex);

		const FPCGTaskId TaskId = Context->ScheduleGraph(FPCGScheduleGraphParams(
			Graph,
			Context->ExecutionSource.Get(),
			MakeShared<FPCGInputForwardingElement>(PreGraphData),
			MakeShared<FPCGInputForwardingElement>(InputData),
			/*Dependencies=*/{},
			&InvocationStack,
			/*bAllowHierarchicalGeneration=*/false));

		if (TaskId == InvalidPCGTaskId) { continue; }

		FPCGExDispatchSubgraphContext::FDispatch& Dispatch = Context->Dispatches.Emplace_GetRef();
		Dispatch.Graph = Graph;
		Dispatch.TaskId = TaskId;
		Context->DynamicDependencies.Add(TaskId);
	}

	return !Context->Dispatches.IsEmpty();
}

void FPCGExDispatchSubgraphElement::GatherDispatchOutputs(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings) const
{
	TSet<FName> CustomOutputLabels;
	CustomOutputLabels.Reserve(Settings->CustomOutputPins.Num());
	for (const FPCGPinProperties& Pin : Settings->CustomOutputPins) { CustomOutputLabels.Add(Pin.Label); }

	for (const FPCGExDispatchSubgraphContext::FDispatch& Dispatch : Context->Dispatches)
	{
		if (Dispatch.TaskId == InvalidPCGTaskId) { continue; }

		FPCGDataCollection SubgraphOutput;
		if (!Context->GetOutputData(Dispatch.TaskId, SubgraphOutput)) { continue; }

		// Root the gathered data before clearing the executor's copy: StageOutput(None) records it for
		// the OnComplete flush but does not GC-root it, and ClearOutputData releases the executor's hold.
		Context->AddToReferencedObjects(SubgraphOutput);
		Context->IncreaseStagedOutputReserve(SubgraphOutput.TaggedData.Num());

		for (const FPCGTaggedData& TaggedData : SubgraphOutput.TaggedData)
		{
			if (!TaggedData.Data) { continue; }
			const FName Pin = CustomOutputLabels.Contains(TaggedData.Pin) ? TaggedData.Pin : PCGPinConstants::DefaultOutputLabel;
			Context->StageOutput(const_cast<UPCGData*>(TaggedData.Data.Get()), Pin, PCGExData::EStaging::None, TaggedData.Tags);
		}

		Context->ClearOutputData(Dispatch.TaskId);
	}
}

void FPCGExDispatchSubgraphElement::BuildUserParameters(FPCGExDispatchSubgraphContext* Context, const UPCGGraph* Graph, FPCGDataCollection& OutData) const
{
	// Mirrors FPCGSubgraphElement::PrepareSubgraphUserParameters: the internal, pinless data the
	// subgraph's User Parameter Get nodes read from. Defaults for now; per-dispatch overrides land next.
	UPCGUserParametersData* UserParamData = FPCGContext::NewObject_AnyThread<UPCGUserParametersData>(Context);
	if (const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct())
	{
		UserParamData->UserParameters = Bag->GetValue();
	}

	FPCGTaggedData& Tagged = OutData.TaggedData.Emplace_GetRef();
	Tagged.Data = UserParamData;
	Tagged.Tags.Add(PCG::Private::UserParameterTagData);
	Tagged.bPinlessData = true;
}

void FPCGExDispatchSubgraphElement::BuildDispatchInput(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings, const UPCGGraph* Graph, FPCGDataCollection& OutData) const
{
	// Model C: forward each declared custom input pin whose label matches one of the graph's input pins.
	// The Drivers pin is never forwarded.
	const UPCGNode* InputNode = Graph->GetInputNode();
	if (!InputNode) { return; }

	TSet<FName> GraphInputLabels;
	for (const FPCGPinProperties& Pin : InputNode->InputPinProperties()) { GraphInputLabels.Add(Pin.Label); }

	for (const FPCGPinProperties& CustomPin : Settings->CustomInputPins)
	{
		if (!GraphInputLabels.Contains(CustomPin.Label)) { continue; }
		OutData.TaggedData.Append(Context->InputData.GetInputsByPin(CustomPin.Label));
	}
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
