// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExDispatchSubgraph.h"

#include "PCGGraph.h"
#include "PCGPin.h"
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

	PCGEX_ON_INITIAL_EXECUTION
	{
		// Scaffold stage (steps 1-2): confirm the async load path resolved the driver-referenced subgraphs.
		// Dispatch (grouping -> ScheduleGraph -> DynamicDependencies wait -> gather) is wired next.
		int32 NumResolved = 0;
		for (const TPair<FSoftObjectPath, UPCGGraph*>& Pair : Context->ResolvedGraphs)
		{
			if (Pair.Value) { ++NumResolved; }
		}

		const FString Message = FString::Printf(
			TEXT("[Dispatch Subgraph] Resolved %d/%d unique subgraph(s) from driver attribute '%s'."),
			NumResolved, Context->UniqueGraphPaths.Num(), *Settings->GraphPathAttribute.ToString());
		PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::FromString(Message));

		Context->Done();
	}

	return Context->TryComplete();
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
