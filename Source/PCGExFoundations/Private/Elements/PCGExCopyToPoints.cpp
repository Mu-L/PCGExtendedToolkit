// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExCopyToPoints.h"

#include "PCGExVersion.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Data/Utils/PCGExDataForward.h"
#include "Fitting/PCGExFittingTasks.h"
#include "Helpers/PCGExArrayHelpers.h"
#include "Helpers/PCGExMatchingHelpers.h"
#include "Utils/PCGExPointIOMerger.h"

#define LOCTEXT_NAMESPACE "PCGExCopyToPointsElement"
#define PCGEX_NAMESPACE CopyToPoints

TArray<FPCGPinProperties> UPCGExCopyToPointsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINT(PCGExCommon::Labels::SourceTargetsLabel, "Target points to copy inputs to.", Required)
	PCGExMatching::Helpers::DeclareMatchingRulesInputs(DataMatching, PinProperties);

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExCopyToPointsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGExMatching::Helpers::DeclareMatchingRulesOutputs(DataMatching, PinProperties);
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(CopyToPoints)
PCGEX_ELEMENT_BATCH_POINT_IMPL(CopyToPoints)

#if WITH_EDITOR
void UPCGExCopyToPointsSettings::PCGExApplyDeprecationBeforeUpdatePins(UPCGNode* InOutNode, TArray<TObjectPtr<UPCGPin>>& InputPins, TArray<TObjectPtr<UPCGPin>>& OutputPins)
{
	PCGEX_IF_VERSION_LOWER(1, 76, 10)
	{
		DataMatching.RenamePins(this, InOutNode);
	}
	Super::PCGExApplyDeprecationBeforeUpdatePins(InOutNode, InputPins, OutputPins);
}

void UPCGExCopyToPointsSettings::PCGExApplyDeprecation(UPCGNode* InOutNode)
{
	PCGEX_IF_VERSION_LOWER(1, 76, 10)
	{
		DataMatching.ApplyDeprecation();
	}
	Super::PCGExApplyDeprecation(InOutNode);
}
#endif

bool FPCGExCopyToPointsElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(CopyToPoints)

	Context->TargetsDataFacade = PCGExData::TryGetSingleFacade(Context, PCGExCommon::Labels::SourceTargetsLabel, false, true);
	if (!Context->TargetsDataFacade)
	{
		return false;
	}

	PCGEX_FWD(TransformDetails)
	if (!Context->TransformDetails.Init(Context, Context->TargetsDataFacade.ToSharedRef()))
	{
		return false;
	}

	PCGEX_FWD(TargetsAttributesToCopyTags)
	if (!Context->TargetsAttributesToCopyTags.Init(Context, Context->TargetsDataFacade))
	{
		return false;
	}

	Context->DataMatcher = MakeShared<PCGExMatching::FDataMatcher>();
	Context->DataMatcher->SetDetails(&Settings->DataMatching);
	if (!Context->DataMatcher->Init(Context, {Context->TargetsDataFacade}, true))
	{
		return false;
	}


	Context->TargetsForwardHandler = Settings->TargetsForwarding.GetHandler(Context->TargetsDataFacade);

	Context->MergeCarryOver.Init();

	return true;
}

bool FPCGExCopyToPointsElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExCopyToPointsElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(CopyToPoints)
	PCGEX_EXECUTION_CHECK

	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartBatchProcessingPoints(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				return true;
			},
			[&](const TSharedPtr<PCGExPointsMT::IBatch>& NewBatch)
			{
				// Merged outputs are facade-written; per-target duplicates are complete after CompleteWork.
				NewBatch->bRequiresWriteStep = Settings->bMergeCopies;
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any points to process."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->MainPoints->StageOutputs();

	return Context->TryComplete();
}

namespace PCGExCopyToPoints
{
	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExCopyToPoints::Process);

		if (!IProcessor::Process(InTaskManager))
		{
			return false;
		}

		MatchScope = PCGExMatching::FScope(Context->InitialMainPointsNum);

		const UPCGBasePointData* Targets = Context->TargetsDataFacade->GetIn();
		const int32 NumTargets = Targets->GetNumPoints();

		if (Settings->bMergeCopies)
		{
			MatchedTargets.SetNumZeroed(NumTargets);
		}
		else
		{
			PCGExArrayHelpers::InitArray(Dupes, NumTargets);
		}

		StartParallelLoopForRange(NumTargets, 32);

		return true;
	}

	void FProcessor::ProcessRange(const PCGExMT::FScope& Scope)
	{
		int32 Copies = 0;
		FPCGExTaggedData AsCandidate = PointDataFacade->Source->GetTaggedData();
		const bool bMerge = Settings->bMergeCopies;

		PCGEX_SCOPE_LOOP(i)
		{
			if (!Context->DataMatcher->Test(Context->TargetsDataFacade->GetInPoint(i), AsCandidate, MatchScope))
			{
				if (!bMerge)
				{
					Dupes[i] = nullptr;
				}
				continue;
			}

			if (bMerge)
			{
				// Copy happens once per source in StartMerge; only record the match here
				MatchedTargets[i] = 1;
				Copies++;
				continue;
			}

			TSharedPtr<PCGExData::FPointIO> Dupe = Context->MainPoints->Emplace_GetRef(PointDataFacade->Source, PCGExData::EIOInit::Duplicate);
			if (!Dupe)
			{
				Dupes[i] = nullptr;
				continue;
			}

			Copies++;
			Context->TargetsForwardHandler->Forward(i, Dupe->GetOut()->Metadata);
			Context->TargetsAttributesToCopyTags.Tag(Context->TargetsDataFacade->GetInPoint(i), Dupe);

			Dupes[i] = Dupe;

			PCGEX_LAUNCH(PCGExFitting::Tasks::FTransformPointIO, i, Context->TargetsDataFacade->Source, Dupe, &Context->TransformDetails)
		}

		if (Copies > 0)
		{
			FPlatformAtomics::InterlockedAdd(&NumCopies, Copies);
		}
	}

	void FProcessor::CompleteWork()
	{
		if (Settings->DataMatching.bSplitUnmatched && NumCopies == 0)
		{
			(void)Context->DataMatcher->HandleUnmatchedOutput(PointDataFacade, true);
			return;
		}

		if (Settings->bMergeCopies && NumCopies > 0)
		{
			StartMerge();
		}
	}

	void FProcessor::StartMerge()
	{
		MatchedIndices.Reserve(NumCopies);
		for (int32 i = 0; i < MatchedTargets.Num(); i++)
		{
			if (MatchedTargets[i])
			{
				MatchedIndices.Add(i);
			}
		}

		const TSharedPtr<PCGExData::FPointIO> MergedIO = Context->MainPoints->Emplace_GetRef(PointDataFacade->Source, PCGExData::EIOInit::New);
		if (!MergedIO)
		{
			return;
		}

		for (const int32 TargetIndex : MatchedIndices)
		{
			Context->TargetsAttributesToCopyTags.Tag(Context->TargetsDataFacade->GetInPoint(TargetIndex), MergedIO);
		}

		const int32 NumSourcePoints = PointDataFacade->GetNum();
		if (NumSourcePoints <= 0)
		{
			// Nothing to replicate: the single empty output stands in for the empty per-target duplicates
			return;
		}

		MergedFacade = MakeShared<PCGExData::FFacade>(MergedIO.ToSharedRef());
		FitBounds = PCGExFitting::Tasks::ComputeFitBounds(PointDataFacade->GetIn(), Context->TransformDetails.bIgnoreBounds);

		Merger = MakeShared<FPCGExPointIOMerger>(MergedFacade.ToSharedRef());
		const PCGExMT::FScope ReadScope(0, NumSourcePoints);
		for (int32 k = 0; k < MatchedIndices.Num(); k++)
		{
			Merger->Append(PointDataFacade->Source, ReadScope, PCGExMT::FScope(k * NumSourcePoints, NumSourcePoints));
		}

		Merger->MergeAsync(
			TaskManager, &Context->MergeCarryOver, nullptr, false, nullptr,
			[PCGEX_ASYNC_THIS_CAPTURE]()
			{
				PCGEX_ASYNC_THIS
				This->OnMergeComplete();
			});
	}

	void FProcessor::OnMergeComplete()
	{
		// Runs inside a merger task: buffers are sized, so per-element writers and native ranges can be created once here
		const int32 NumSourcePoints = PointDataFacade->GetNum();
		MergedFacade->GetOut()->AllocateProperties(EPCGPointNativeProperties::Transform);
		const TSharedPtr<PCGExData::FDataForwardHandler> ForwardHandler = Settings->TargetsForwarding.TryGetHandler(Context->TargetsDataFacade, MergedFacade, PCGExData::EForwardDomain::ToElements);

		TArray<TSharedPtr<PCGExMT::FTask>> Tasks;
		Tasks.Reserve(MatchedIndices.Num());

		for (int32 k = 0; k < MatchedIndices.Num(); k++)
		{
			const int32 TargetIndex = MatchedIndices[k];
			const PCGExMT::FScope WriteScope(k * NumSourcePoints, NumSourcePoints);

			if (ForwardHandler)
			{
				ForwardHandler->Forward(TargetIndex, WriteScope);
			}

			Tasks.Add(MakeShared<PCGExFitting::Tasks::FTransformPointIO>(TargetIndex, Context->TargetsDataFacade->Source, MergedFacade->Source, &Context->TransformDetails, WriteScope, FitBounds));
		}

		PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, MergedTransformTasks)
		MergedTransformTasks->StartTasksBatch(Tasks);
	}

	void FProcessor::Write()
	{
		if (MergedFacade)
		{
			MergedFacade->WriteFastest(TaskManager);
		}
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
