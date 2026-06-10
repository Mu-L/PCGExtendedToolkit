// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "FillControls/PCGExFillControlBranchLimit.h"


#include "Clusters/PCGExCluster.h"
#include "Containers/PCGExManagedObjects.h"
#include "Core/PCGExFillControlsFactoryProvider.h"
#include "Data/Utils/PCGExDataPreloader.h"
#include "Details/PCGExSettingsDetails.h"

bool FPCGExFillControlBranchLimit::PrepareForDiffusions(FPCGExContext* InContext, const TSharedPtr<PCGExFloodFill::FFillControlsHandler>& InHandler)
{
	if (!FPCGExFillControlOperation::PrepareForDiffusions(InContext, InHandler))
	{
		return false;
	}

	// Mode & bSourceIsVtx are cached on the operation in CreateOperation (so the capability
	// predicates, which run during the handler's BuildFrom before this, share one source).
	const UPCGExFillControlsFactoryBranchLimit* TypedFactory = Cast<UPCGExFillControlsFactoryBranchLimit>(Factory);

	MaxBranchesValue = TypedFactory->Config.MaxBranches.GetValueSetting();
	if (!MaxBranchesValue->Init(GetSourceFacade()))
	{
		return false;
	}

	// Hoist a constant / data-domain budget so the hot path skips the per-call virtual Read.
	if (MaxBranchesValue->IsConstant())
	{
		bConstantBudget = true;
		ConstantBudget = MaxBranchesValue->Read(0);
	}

	// Branch state by enforcement strategy:
	//  - Vtx + Reroute limits fan-out at probe time and needs no persistent state.
	//  - Everything else enforces at capture: Vtx tracks per-node child counts, while Seed
	//    shares one global fork budget per diffusion (spent best-first as the heap pops).
	if (bSourceIsVtx)
	{
		if (Mode == EPCGExFloodFillBranchMode::Prune)
		{
			ChildCounts.Init(0, Cluster->Nodes->Num());
		}
	}
	else
	{
		ParentHasChild.Init(false, Cluster->Nodes->Num());
		DiffusionForks.Init(0, Handler->GetNumDiffusions());
	}

	return true;
}

int32 FPCGExFillControlBranchLimit::ReadBudget(const int32 Index)
{
	const int32 Raw = bConstantBudget ? ConstantBudget : MaxBranchesValue->Read(Index);
	// Clamp to a non-negative, overflow-safe range: a budget so large it is effectively
	// unlimited still keeps (1 + budget) below the MAX_int32 sentinel, so the reroute
	// fan-out math can neither overflow nor accidentally read as 'no limit'.
	return FMath::Clamp(Raw, 0, MAX_int32 - 2);
}

#pragma region Capture-time enforcement

bool FPCGExFillControlBranchLimit::ChecksCapture() const
{
	return !UsesProbeFanout();
}

bool FPCGExFillControlBranchLimit::IsValidCapture(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& Candidate)
{
	const int32 ParentNode = Candidate.Link.Node;
	if (ParentNode < 0)
	{
		// Seed/root has no parent to budget against.
		return true;
	}

	if (bSourceIsVtx)
	{
		// Per-vtx: a node may gain up to (1 + budget) children, i.e. branch 'budget' times.
		return ChildCounts[ParentNode] <= ReadBudget(Cluster->GetNodePointIndex(ParentNode));
	}

	// Seed source (global): the first child is always free (it continues the lane); any
	// additional child is a fork that draws from the diffusion's shared budget.
	if (!ParentHasChild[ParentNode])
	{
		return true;
	}
	return DiffusionForks[Diffusion->Index] < ReadBudget(GetSettingsIndex(Diffusion));
}

bool FPCGExFillControlBranchLimit::WantsCaptureNotify() const
{
	return !UsesProbeFanout();
}

void FPCGExFillControlBranchLimit::OnCaptured(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& Candidate)
{
	const int32 ParentNode = Candidate.Link.Node;
	if (ParentNode < 0)
	{
		return;
	}

	if (bSourceIsVtx)
	{
		ChildCounts[ParentNode]++;
		return;
	}

	// Seed source: the first child is free; every child beyond it is a fork that consumes
	// the diffusion's shared budget.
	if (ParentHasChild[ParentNode]) { DiffusionForks[Diffusion->Index]++; }
	else { ParentHasChild[ParentNode] = true; }
}

#pragma endregion

#pragma region Probe-time enforcement

bool FPCGExFillControlBranchLimit::LimitsProbeFanout() const
{
	return UsesProbeFanout();
}

int32 FPCGExFillControlBranchLimit::GetProbeFanoutLimit(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& From)
{
	// Only Vtx + Reroute registers for probe fan-out limiting. A node may spread to up to
	// (1 + budget) children; the probe keeps the best ones by score (see FDiffusion::Probe).
	return 1 + ReadBudget(From.Node->PointIndex);
}

#pragma endregion

TSharedPtr<FPCGExFillControlOperation> UPCGExFillControlsFactoryBranchLimit::CreateOperation(FPCGExContext* InContext) const
{
	PCGEX_FACTORY_NEW_OPERATION(FillControlBranchLimit)
	PCGEX_FORWARD_FILLCONTROL_OPERATION
	// Cache mode & source on the operation so the capability predicates (run during the
	// handler's BuildFrom, before PrepareForDiffusions) have a single source of truth.
	NewOperation->Mode = Config.Mode;
	NewOperation->bSourceIsVtx = Config.Source == EPCGExFloodFillSettingSource::Vtx;
	return NewOperation;
}

void UPCGExFillControlsFactoryBranchLimit::RegisterBuffersDependencies(FPCGExContext* InContext, PCGExData::FFacadePreloader& FacadePreloader) const
{
	Super::RegisterBuffersDependencies(InContext, FacadePreloader);

	// The preloader targets the cluster's vtx facade, so only Vtx-source attributes preload here.
	if (Config.Source == EPCGExFloodFillSettingSource::Vtx)
	{
		Config.MaxBranches.RegisterBufferDependencies(InContext, FacadePreloader);
	}
}

UPCGExFactoryData* UPCGExFillControlsBranchLimitProviderSettings::CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const
{
	UPCGExFillControlsFactoryBranchLimit* NewFactory = InContext->ManagedObjects->New<UPCGExFillControlsFactoryBranchLimit>();
	PCGEX_FORWARD_FILLCONTROL_FACTORY
	return Super::CreateFactory(InContext, NewFactory);
}

#if WITH_EDITOR
FString UPCGExFillControlsBranchLimitProviderSettings::GetDisplayName() const
{
	FString DName = GetDefaultNodeTitle().ToString().Replace(TEXT("PCGEx | Fill Control"), TEXT("FC"));
	DName += Config.Mode == EPCGExFloodFillBranchMode::Reroute ? TEXT(" - Reroute @ ") : TEXT(" - Prune @ ");
	DName += Config.MaxBranches.Input == EPCGExInputValueType::Constant
		? FString::FromInt(Config.MaxBranches.Constant)
		: TEXT("attr");
	return DName;
}
#endif
