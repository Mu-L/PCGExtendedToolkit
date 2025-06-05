﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Misc/PCGExUberFilter.h"

#include "Data/PCGExData.h"
#include "Data/PCGExPointFilter.h"


#define LOCTEXT_NAMESPACE "PCGExUberFilter"
#define PCGEX_NAMESPACE UberFilter

TArray<FPCGPinProperties> UPCGExUberFilterSettings::OutputPinProperties() const
{
	if (Mode == EPCGExUberFilterMode::Write) { return Super::OutputPinProperties(); }

	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_POINTS(PCGExPointFilter::OutputInsideFiltersLabel, "Points that passed the filters.", Required, {})
	PCGEX_PIN_POINTS(PCGExPointFilter::OutputOutsideFiltersLabel, "Points that didn't pass the filters.", Required, {})
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(UberFilter)

FName UPCGExUberFilterSettings::GetMainOutputPin() const
{
	// Ensure proper forward when node is disabled
	return Mode == EPCGExUberFilterMode::Partition ? PCGExPointFilter::OutputInsideFiltersLabel : Super::GetMainOutputPin();
}

bool FPCGExUberFilterElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(UberFilter)

	if (Settings->Mode == EPCGExUberFilterMode::Write)
	{
		PCGEX_VALIDATE_NAME(Settings->ResultAttributeName)
		return true;
	}

	Context->Inside = MakeShared<PCGExData::FPointIOCollection>(Context);
	Context->Outside = MakeShared<PCGExData::FPointIOCollection>(Context);

	Context->Inside->OutputPin = PCGExPointFilter::OutputInsideFiltersLabel;
	Context->Outside->OutputPin = PCGExPointFilter::OutputOutsideFiltersLabel;

	if (Settings->bSwap)
	{
		Context->Inside->OutputPin = PCGExPointFilter::OutputOutsideFiltersLabel;
		Context->Outside->OutputPin = PCGExPointFilter::OutputInsideFiltersLabel;
	}

	return true;
}

bool FPCGExUberFilterElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExUberFilterElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(UberFilter)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		Context->NumPairs = Context->MainPoints->Pairs.Num();

		if (Settings->Mode == EPCGExUberFilterMode::Partition)
		{
			Context->Inside->Pairs.Init(nullptr, Context->NumPairs);
			Context->Outside->Pairs.Init(nullptr, Context->NumPairs);
		}

		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExUberFilter::FProcessor>>(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry) { return true; },
			[&](const TSharedPtr<PCGExPointsMT::TBatch<PCGExUberFilter::FProcessor>>& NewBatch)
			{
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any points to filter."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGEx::State_Done)

	if (Settings->Mode == EPCGExUberFilterMode::Write)
	{
		Context->MainPoints->StageOutputs();
	}
	else
	{
		Context->Inside->PruneNullEntries(true);
		Context->Outside->PruneNullEntries(true);

		Context->Inside->StageOutputs();
		Context->Outside->StageOutputs();
	}

	return Context->TryComplete();
}

namespace PCGExUberFilter
{
	FProcessor::~FProcessor()
	{
	}

	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExUberFilter::Process);

		// Must be set before process for filters
		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!FPointsProcessor::Process(InAsyncManager)) { return false; }

		PCGEX_INIT_IO(PointDataFacade->Source, Settings->Mode == EPCGExUberFilterMode::Write ? PCGExData::EIOInit::Duplicate : PCGExData::EIOInit::NoInit)

		if (Settings->Mode == EPCGExUberFilterMode::Write)
		{
			Results = PointDataFacade->GetWritable<bool>(Settings->ResultAttributeName, false, true, PCGExData::EBufferInit::New);
		}
		else
		{
			PCGEx::InitArray(PointFilterCache, PointDataFacade->GetNum());
		}

		StartParallelLoopForPoints(PCGExData::EIOSide::In);

		return true;
	}

	void FProcessor::PrepareLoopScopesForPoints(const TArray<PCGExMT::FScope>& Loops)
	{
		if (!Results)
		{
			const int32 MaxRange = PCGExMT::FScope::GetMaxRange(Loops);

			IndicesInside = MakeShared<PCGExMT::TScopedArray<int32>>(Loops);
			IndicesInside->Reserve(MaxRange);

			IndicesOutside = MakeShared<PCGExMT::TScopedArray<int32>>(Loops);
			IndicesOutside->Reserve(MaxRange);
		}
	}

	void FProcessor::ProcessPoints(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGEx::UberFilter::ProcessPoints);

		PointDataFacade->Fetch(Scope);
		FilterScope(Scope);

		if (!Results)
		{
			TArray<int32>& IndicesInsideRef = IndicesInside->Get_Ref(Scope);
			TArray<int32>& IndicesOutsideRef = IndicesOutside->Get_Ref(Scope);

			PCGEX_SCOPE_LOOP(Index)
			{
				const int8 bPass = PointFilterCache[Index];

				if (bPass) { IndicesInsideRef.Add(Index); }
				else { IndicesOutsideRef.Add(Index); }

				if (bPass) { FPlatformAtomics::InterlockedAdd(&NumInside, 1); }
				else { FPlatformAtomics::InterlockedAdd(&NumOutside, 1); }
			}
		}
		else
		{
			PCGEX_SCOPE_LOOP(Index)
			{
				const int8 bPass = PointFilterCache[Index];

				if (bPass) { FPlatformAtomics::InterlockedAdd(&NumInside, 1); }
				else { FPlatformAtomics::InterlockedAdd(&NumOutside, 1); }

				Results->SetValue(Index, bPass ? !Settings->bSwap : Settings->bSwap);
			}
		}
	}

	TSharedPtr<PCGExData::FPointIO> FProcessor::CreateIO(const TSharedRef<PCGExData::FPointIOCollection>& InCollection, const PCGExData::EIOInit InitMode) const
	{
		TSharedPtr<PCGExData::FPointIO> NewPointIO = PCGExData::NewPointIO(PointDataFacade->Source, InCollection->OutputPin);

		if (!NewPointIO->InitializeOutput(InitMode)) { return nullptr; }

		InCollection->Pairs[BatchIndex] = NewPointIO;
		return NewPointIO;
	}

	void FProcessor::CompleteWork()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExUberFilterProcessor::CompleteWork);

		if (Settings->Mode == EPCGExUberFilterMode::Write)
		{
			const bool bHasAnyPass = Settings->bSwap ? NumOutside != 0 : NumInside != 0;
			const bool bAllPass = Settings->bSwap ? NumOutside == PointDataFacade->GetNum() : NumInside == PointDataFacade->GetNum();
			if (bHasAnyPass && Settings->bTagIfAnyPointPassed) { PointDataFacade->Source->Tags->AddRaw(Settings->HasAnyPointPassedTag); }
			if (bAllPass && Settings->bTagIfAllPointsPassed) { PointDataFacade->Source->Tags->AddRaw(Settings->AllPointsPassedTag); }
			if (!bHasAnyPass && Settings->bTagIfNoPointPassed) { PointDataFacade->Source->Tags->AddRaw(Settings->NoPointPassedTag); }


			PointDataFacade->Write(AsyncManager);
			return;
		}

		if (NumInside == 0 || NumOutside == 0)
		{
			if (NumInside == 0)
			{
				Outside = CreateIO(Context->Outside.ToSharedRef(), PCGExData::EIOInit::Forward);
				if (!Outside) { return; }
				if (Settings->bTagIfNoPointPassed) { Outside->Tags->AddRaw(Settings->NoPointPassedTag); }
			}
			else
			{
				Inside = CreateIO(Context->Inside.ToSharedRef(), PCGExData::EIOInit::Forward);
				if (!Inside) { return; }
				if (Settings->bTagIfAnyPointPassed) { Inside->Tags->AddRaw(Settings->HasAnyPointPassedTag); }
				if (Settings->bTagIfAllPointsPassed) { Inside->Tags->AddRaw(Settings->AllPointsPassedTag); }
			}
			return;
		}

		const int32 NumPoints = PointDataFacade->GetNum();
		TArray<int32> Indices;
		PCGEx::InitArray(Indices, NumPoints);

		TArray<int32> ReadIndices;
		IndicesInside->Collapse(ReadIndices);

		Inside = CreateIO(Context->Inside.ToSharedRef(), PCGExData::EIOInit::New);
		if (!Inside) { return; }

		PCGEx::SetNumPointsAllocated(Inside->GetOut(), ReadIndices.Num());
		Inside->InheritProperties(ReadIndices, EPCGPointNativeProperties::All);

		if (Settings->bTagIfAnyPointPassed) { Inside->Tags->AddRaw(Settings->HasAnyPointPassedTag); }

		ReadIndices.Reset();
		IndicesOutside->Collapse(ReadIndices);
		Outside = CreateIO(Context->Outside.ToSharedRef(), PCGExData::EIOInit::New);

		if (!Outside) { return; }

		PCGEx::SetNumPointsAllocated(Outside->GetOut(), ReadIndices.Num());
		Outside->InheritProperties(ReadIndices, EPCGPointNativeProperties::All);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
