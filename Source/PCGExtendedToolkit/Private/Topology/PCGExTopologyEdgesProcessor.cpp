﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Topology/PCGExTopologyEdgesProcessor.h"

#include "Topology/PCGExTopology.h"

#define LOCTEXT_NAMESPACE "PCGExEdgesToPaths"
#define PCGEX_NAMESPACE TopologyProcessor

PCGExData::EIOInit UPCGExTopologyEdgesProcessorSettings::GetMainOutputInitMode() const { return PCGExData::EIOInit::Forward; }
PCGExData::EIOInit UPCGExTopologyEdgesProcessorSettings::GetEdgeOutputInitMode() const { return PCGExData::EIOInit::Forward; }

TArray<FPCGPinProperties> UPCGExTopologyEdgesProcessorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINT(PCGExTopology::SourceHolesLabel, "Omit cells that contain any points from this dataset", Normal, {})
	if (SupportsEdgeConstraints())
	{
		PCGEX_PIN_FACTORIES(PCGExTopology::SourceEdgeConstrainsFiltersLabel, "Constrained edges filters.", Normal, {})
	}
	return PinProperties;
}

void FPCGExTopologyEdgesProcessorContext::RegisterAssetDependencies()
{
	PCGEX_SETTINGS_LOCAL(TopologyEdgesProcessor)

	FPCGExEdgesProcessorContext::RegisterAssetDependencies();

	if (Settings->Topology.Material.ToSoftObjectPath().IsValid())
	{
		AddAssetDependency(Settings->Topology.Material.ToSoftObjectPath());
	}
}

bool FPCGExTopologyEdgesProcessorElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(TopologyEdgesProcessor)

	Context->HolesFacade = PCGExData::TryGetSingleFacade(Context, PCGExTopology::SourceHolesLabel, false, false);
	if (Context->HolesFacade && Settings->ProjectionDetails.Method == EPCGExProjectionMethod::Normal)
	{
		Context->Holes = MakeShared<PCGExTopology::FHoles>(Context, Context->HolesFacade.ToSharedRef(), Settings->ProjectionDetails);
	}

	PCGExHelpers::AppendUniqueEntriesFromCommaSeparatedList(Settings->CommaSeparatedComponentTags, Context->ComponentTags);
	GetInputFactories(Context, PCGExTopology::SourceEdgeConstrainsFiltersLabel, Context->EdgeConstraintsFilterFactories, PCGExFactories::ClusterEdgeFilters, false);
	return true;
}

namespace PCGExTopology
{
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
