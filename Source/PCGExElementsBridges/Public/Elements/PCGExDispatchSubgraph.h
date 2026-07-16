// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

#include "PCGExCoreMacros.h"
#include "Core/PCGExContext.h"
#include "Core/PCGExElement.h"
#include "Core/PCGExSettings.h"

#include "PCGExDispatchSubgraph.generated.h"

class UPCGGraph;

/**
 * Dispatch Subgraphs.
 * Resolves a PCG subgraph per driver entry (point or attribute-set row) from a soft-path attribute,
 * then executes each unique (graph + overrides) combination once as a dynamic subgraph.
 *
 * Scaffold stage (steps 1-2): resolves and async-loads the driver-referenced subgraphs. The
 * grouping / ScheduleGraph dispatch / DynamicDependencies wait / output-gather is wired next.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category = "PCGEx|Misc", meta=(Keywords = "subgraph dispatch execute graph dynamic"))
class UPCGExDispatchSubgraphSettings : public UPCGExSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(DispatchSubgraph, "Dispatch Subgraphs", "Executes a per-entry-resolved PCG subgraph, deduplicated by (graph + overrides).");

	virtual EPCGSettingsType GetType() const override
	{
		return EPCGSettingsType::ControlFlow;
	}

	virtual FLinearColor GetNodeTitleColor() const override;
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

public:
	/** Attribute on the Drivers input holding the subgraph reference as a soft object path. Empty values are ignored (no warning). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FName GraphPathAttribute = FName("AssetPath");

	/** If enabled, any driver attribute whose name exactly matches a subgraph user-parameter is applied as an override. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Overrides")
	bool bAutoMatchByName = true;

	/** Explicit source-attribute -> target-parameter remaps, for when the driver attribute name differs from the parameter name.
	 *  Takes precedence over auto-match. (Point-property sources such as $Transform are not supported yet.) */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Overrides")
	TMap<FName, FName> OverrideRemap;

	/** User-declared input pins, forwarded identically to every dispatched subgraph and matched to the subgraph's inputs by name. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Pins", meta = (TitleProperty = "{Label}"))
	TArray<FPCGPinProperties> CustomInputPins;

	/** User-declared output pins. Subgraph outputs are routed here by exact name; anything unmatched goes to the default Out pin. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Pins", meta = (TitleProperty = "{Label}"))
	TArray<FPCGPinProperties> CustomOutputPins;
};

struct FPCGExDispatchSubgraphContext final : FPCGExContext
{
	friend class FPCGExDispatchSubgraphElement;

	/** Reads the driver soft-path attribute and registers each unique subgraph as an async asset dependency. */
	virtual void RegisterAssetDependencies() override;

	/** Per driver tagged-data (aligned to the Drivers pin input order), the per-entry subgraph paths (null == ignored entry). */
	TArray<TArray<FSoftObjectPath>> DriverPaths;

	/** Unique non-null subgraph paths across all drivers -- the set registered for async loading. */
	TSet<FSoftObjectPath> UniqueGraphPaths;

	/** Loaded subgraphs by path, populated once the async load completes (null value == failed load). */
	TMap<FSoftObjectPath, UPCGGraph*> ResolvedGraphs;

	/** One scheduled dynamic subgraph execution. */
	struct FDispatch
	{
		UPCGGraph* Graph = nullptr;
		FPCGTaskId TaskId = InvalidPCGTaskId;
	};

	/** Scheduled dispatches -- one per unique subgraph (per unique graph + overrides once overrides land). */
	TArray<FDispatch> Dispatches;

	/** Set once the subgraphs are scheduled; distinguishes the schedule pass from the post-wake gather pass. */
	bool bDispatched = false;

	/** Forwarded-input and gathered-output data held alive for the context lifetime.
	 *  FPCGInputForwardingElement does not own its data, and StageOutput(None) does not GC-root it. */
	TSet<TObjectPtr<const UPCGData>> ReferencedObjects;
	void AddToReferencedObjects(const FPCGDataCollection& InCollection);

protected:
	virtual void AddExtraStructReferencedObjects(FReferenceCollector& Collector) override;
};

class FPCGExDispatchSubgraphElement final : public IPCGExElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(DispatchSubgraph)
	PCGEX_ELEMENT_MAIN_THREAD_ONLY_IN_PREPARE()

	virtual bool IsCacheable(const UPCGSettings* InSettings) const override
	{
		return false;
	}

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual void PostLoadAssetsDependencies(FPCGExContext* InContext) const override;
	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;

private:
	/** Groups driver entries by (graph + override values) and schedules one dynamic subgraph per unique group.
	 *  Returns true if at least one was scheduled (and DynamicDependencies were populated). */
	bool ScheduleDispatches(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings) const;

	/** Gathers each dispatched subgraph's output and routes it to the matching output pin (unmatched -> default Out). */
	void GatherDispatchOutputs(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings) const;

	/** Builds the Model-C input for a dispatch: the custom input pins whose labels match the graph's input pins. */
	void BuildDispatchInput(FPCGExDispatchSubgraphContext* Context, const UPCGExDispatchSubgraphSettings* Settings, const UPCGGraph* Graph, FPCGDataCollection& OutData) const;
};
