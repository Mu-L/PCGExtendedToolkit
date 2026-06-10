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

	const UPCGExFillControlsFactoryBranchLimit* TypedFactory = Cast<UPCGExFillControlsFactoryBranchLimit>(Factory);

	Mode = TypedFactory->Config.Mode;
	bSourceIsVtx = TypedFactory->Config.Source == EPCGExFloodFillSettingSource::Vtx;

	MaxBranchesValue = TypedFactory->Config.MaxBranches.GetValueSetting();
	if (!MaxBranchesValue->Init(GetSourceFacade()))
	{
		return false;
	}

	// Prune tracks committed children per node to enforce an exact cap.
	if (Mode == EPCGExFloodFillBranchMode::Prune)
	{
		ChildCounts.Init(0, Cluster->Nodes->Num());
	}

	// Seed source pools the budget across the whole diffusion (one fork tally per seed).
	if (!bSourceIsVtx)
	{
		DiffusionForks.Init(0, Handler->GetNumDiffusions());
	}

	return true;
}

#pragma region Prune

bool FPCGExFillControlBranchLimit::ChecksCapture() const
{
	return Cast<UPCGExFillControlsFactoryBranchLimit>(Factory)->Config.Mode == EPCGExFloodFillBranchMode::Prune;
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
		const int32 Budget = MaxBranchesValue->Read(Cluster->GetNodePointIndex(ParentNode));
		return ChildCounts[ParentNode] <= Budget;
	}

	// Seed source (global): the first child is always free (it continues the lane); any
	// additional child is a fork that draws from the diffusion's shared budget.
	if (ChildCounts[ParentNode] == 0)
	{
		return true;
	}
	const int32 Budget = MaxBranchesValue->Read(GetSettingsIndex(Diffusion));
	return DiffusionForks[Diffusion->Index] < Budget;
}

bool FPCGExFillControlBranchLimit::WantsCaptureNotify() const
{
	return Cast<UPCGExFillControlsFactoryBranchLimit>(Factory)->Config.Mode == EPCGExFloodFillBranchMode::Prune;
}

void FPCGExFillControlBranchLimit::OnCaptured(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& Candidate)
{
	const int32 ParentNode = Candidate.Link.Node;
	if (ParentNode < 0)
	{
		return;
	}

	// A child beyond the first is a fork; for Seed source it consumes the shared budget.
	if (!bSourceIsVtx && ChildCounts[ParentNode] >= 1)
	{
		DiffusionForks[Diffusion->Index]++;
	}
	ChildCounts[ParentNode]++;
}

#pragma endregion

#pragma region Reroute

bool FPCGExFillControlBranchLimit::LimitsProbeFanout() const
{
	return Cast<UPCGExFillControlsFactoryBranchLimit>(Factory)->Config.Mode == EPCGExFloodFillBranchMode::Reroute;
}

int32 FPCGExFillControlBranchLimit::GetProbeFanoutLimit(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& From)
{
	if (bSourceIsVtx)
	{
		// Per-vtx: this node may spread to up to (1 + budget) children.
		return 1 + MaxBranchesValue->Read(From.Node->PointIndex);
	}

	// Seed source (global): one free child to continue the lane, plus however many forks
	// the diffusion has left in its shared pool.
	const int32 Budget = MaxBranchesValue->Read(GetSettingsIndex(Diffusion));
	return 1 + FMath::Max(0, Budget - DiffusionForks[Diffusion->Index]);
}

void FPCGExFillControlBranchLimit::OnProbeComplete(const PCGExFloodFill::FDiffusion* Diffusion, const PCGExFloodFill::FCandidate& From, const int32 NumClaimed)
{
	// Only the Seed-source (global) budget needs reconciling: every child beyond the first
	// claimed by this probe consumed one fork from the shared pool.
	if (!bSourceIsVtx)
	{
		DiffusionForks[Diffusion->Index] += FMath::Max(0, NumClaimed - 1);
	}
}

#pragma endregion

TSharedPtr<FPCGExFillControlOperation> UPCGExFillControlsFactoryBranchLimit::CreateOperation(FPCGExContext* InContext) const
{
	PCGEX_FACTORY_NEW_OPERATION(FillControlBranchLimit)
	PCGEX_FORWARD_FILLCONTROL_OPERATION
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
