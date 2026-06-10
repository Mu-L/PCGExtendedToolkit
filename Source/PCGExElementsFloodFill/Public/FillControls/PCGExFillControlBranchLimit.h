// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Core/PCGExFillControlOperation.h"
#include "Core/PCGExFillControlsFactoryProvider.h"
#include "Details/PCGExInputShorthandsDetails.h"
#include "Details/PCGExSettingsMacros.h"
#include "UObject/Object.h"

#include "PCGExFillControlBranchLimit.generated.h"

UENUM()
enum class EPCGExFloodFillBranchMode : uint8
{
	Prune   = 0 UMETA(DisplayName = "Prune", ToolTip="Hard-cap branching, enforced when a node is captured. Excess branches are dropped entirely -- fewer, longer, cleaner lanes, at the cost of coverage (some nodes may go uncaptured). No core overhead."),
	Reroute = 1 UMETA(DisplayName = "Reroute", ToolTip="Cap branching when a node spreads, but leave the excess neighbors available for other nodes to pick up. Preserves coverage (the region stays filled) while de-bushing. Slightly more expensive."),
};

USTRUCT(BlueprintType)
struct FPCGExFillControlConfigBranchLimit : public FPCGExFillControlConfigBase
{
	GENERATED_BODY()

	FPCGExFillControlConfigBranchLimit()
		: FPCGExFillControlConfigBase()
	{
		bSupportSteps = false; // The diffusion step is implied by Mode.
	}

	/** How the branch limit is enforced. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExFloodFillBranchMode Mode = EPCGExFloodFillBranchMode::Prune;

	/**
	 * Maximum number of times the fill may branch (i.e. children beyond the first).
	 * 0 = never branches (a single lane). The scope depends on Source:
	 * - Source = Vtx: per-vtx -- each node may branch up to this many times.
	 * - Source = Seed: GLOBAL -- the seed's whole diffusion may branch this many times total.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayName="Max Branches"))
	FPCGExInputShorthandSelectorInteger32Abs MaxBranches = FPCGExInputShorthandSelectorInteger32Abs(FName("MaxBranches"), 0);
};

/**
 * Limits how many times the fill may branch during diffusion, suppressing heavy
 * branching.
 *
 * Prune mode caps branching at capture time: excess branches are dropped, which
 * trims the fill into fewer, longer lanes but can leave nodes uncaptured. Reroute
 * mode caps branching at probe time but leaves the excess neighbors unclaimed, so
 * other nodes can still adopt them -- the region stays filled while branching is reduced.
 *
 * The budget scope follows Source: Vtx is per-node, Seed is global to the whole
 * diffusion (a shared fork pool for the seed's lifetime). Either way the limit is
 * per-diffusion: a node skipped by one seed remains fully reachable by every other
 * seed (the shared influence claim only happens on an actual capture, which a
 * skipped node never reaches).
 */
class FPCGExFillControlBranchLimit : public FPCGExFillControlOperation
{
	friend class UPCGExFillControlsFactoryBranchLimit;

public:
	virtual bool PrepareForDiffusions(FPCGExContext* InContext, const TSharedPtr<PCGExFloodFill::FFillControlsHandler>& InHandler) override;

	// Capture-time enforcement (everything except Vtx + Reroute): Vtx tracks per-node child
	// counts; Seed draws from one shared per-diffusion fork budget, spent best-first.
	virtual bool ChecksCapture() const override;
	virtual bool IsValidCapture(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& Candidate) override;
	virtual bool WantsCaptureNotify() const override;
	virtual void OnCaptured(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& Candidate) override;

	// Probe-time enforcement (Vtx + Reroute only): cap a node's fan-out, keeping the best
	// children by score and leaving the rest unvisited (adoptable by other nodes).
	virtual bool LimitsProbeFanout() const override;
	virtual int32 GetProbeFanoutLimit(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& From) override;

	virtual bool ChecksProbe() const override
	{
		return false;
	}

	virtual bool ChecksCandidate() const override
	{
		return false;
	}

protected:
	// Vtx + Reroute is the only combination that limits fan-out at probe time; every other
	// combination enforces its budget at capture time.
	FORCEINLINE bool UsesProbeFanout() const { return Mode == EPCGExFloodFillBranchMode::Reroute && bSourceIsVtx; }

	// Reads the per-parent branch budget for a settings index, clamped to a non-negative,
	// overflow-safe range (so 1 + budget can neither overflow nor hit the MAX_int32 'unlimited'
	// sentinel). Returns the cached value when the input is constant / data-domain.
	int32 ReadBudget(int32 Index);

	TSharedPtr<PCGExDetails::TSettingValue<int32>> MaxBranchesValue;
	bool bConstantBudget = false;
	int32 ConstantBudget = 0;

	// Captured children per node index (Prune + Vtx source). A node is a parent in exactly
	// one diffusion (single capturer), so a single shared array is safe: each element has
	// exactly one writer, on that diffusion's own thread.
	TArray<int32> ChildCounts;

	// Whether a node already has a child (Prune + Seed source) -- only the 'has any child'
	// bit is needed there; the global fork tally lives in DiffusionForks.
	TBitArray<> ParentHasChild;

	// Forks spent per diffusion (Seed source -- the global, shared budget). Indexed by
	// diffusion index; each slot has a single writer (its diffusion's thread).
	TArray<int32> DiffusionForks;

	EPCGExFloodFillBranchMode Mode = EPCGExFloodFillBranchMode::Prune;
	bool bSourceIsVtx = false;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExFillControlsFactoryBranchLimit : public UPCGExFillControlsFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExFillControlConfigBranchLimit Config;

	virtual TSharedPtr<FPCGExFillControlOperation> CreateOperation(FPCGExContext* InContext) const override;

	virtual void RegisterBuffersDependencies(FPCGExContext* InContext, PCGExData::FFacadePreloader& FacadePreloader) const override;
};

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Graph|Params", meta=(PCGExNodeLibraryDoc="pathfinding/cluster-flood-fill/fc-branch-limit"))
class UPCGExFillControlsBranchLimitProviderSettings : public UPCGExFillControlsFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(FillControlsBranchLimit, "Fill Control : Branch Limit", "Limit how many times the fill may branch, suppressing heavy branching.", FName(GetDisplayName()))
#endif
	//~End UPCGSettings

	/** Control Config.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExFillControlConfigBranchLimit Config;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif
};
