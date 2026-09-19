// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExSampleStampPoints.h"

#include "Blenders/PCGExUnionBlender.h"
#include "Blenders/PCGExUnionOpsManager.h"
#include "Containers/PCGExScopedContainers.h"
#include "Core/PCGExBlendOpsManager.h"
#include "Core/PCGExBlendOpsSchema.h"
#include "Core/PCGExOpStats.h"
#include "Core/PCGExPointFilter.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataTags.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExSettingsDetails.h"
#include "Helpers/PCGExAsyncHelpers.h"
#include "Helpers/PCGExDataMatcher.h"
#include "Helpers/PCGExMatchingHelpers.h"
#include "Helpers/PCGExTargetsHandler.h"
#include "Helpers/PCGExTargetsRangeIndex.h"
#include "Sampling/PCGExSamplingHelpers.h"
#include "Sampling/PCGExSamplingUnionData.h"
#include "Sorting/PCGExPointSorter.h"
#include "Sorting/PCGExSortingDetails.h"
#include "Types/PCGExTypes.h"


#define LOCTEXT_NAMESPACE "PCGExSampleStampPointsElement"
#define PCGEX_NAMESPACE SampleStampPoints

PCGEX_SETTING_VALUE_IMPL_BOOL(UPCGExSampleStampPointsSettings, LookAtUp, FVector, LookAtUpSelection != EPCGExSampleSource::Constant, LookAtUpSource, LookAtUpConstant)

UPCGExSampleStampPointsSettings::UPCGExSampleStampPointsSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (LookAtUpSource.GetName() == FName("@Last"))
	{
		LookAtUpSource.Update(TEXT("$Transform.Up"));
	}
	if (!WeightOverDistance)
	{
		WeightOverDistance = PCGExCurves::WeightDistributionLinear;
	}
}

FName UPCGExSampleStampPointsSettings::GetMainInputPin() const
{
	return PCGExSampling::Labels::SourceSourceLabel;
}

TArray<FPCGPinProperties> UPCGExSampleStampPointsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	PCGEX_PIN_POINTS(PCGExCommon::Labels::SourceTargetsLabel, "The points that stamp their values onto sources within their range.", Required)

	PCGExMatching::Helpers::DeclareMatchingRulesInputs(DataMatching, PinProperties);
	PCGExBlending::DeclareBlendOpsInputs(PinProperties, EPCGPinStatus::Normal, BlendingInterface);
	PCGExSorting::DeclareSortingRulesInputs(PinProperties, SampleMethod == EPCGExSampleMethod::BestCandidate ? EPCGPinStatus::Required : EPCGPinStatus::Advanced);

	PCGEX_PIN_FILTERS(PCGExFilters::Labels::SourceUseValueIfFilters, "Filter which points values will be processed.", Advanced)

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExSampleStampPointsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGExMatching::Helpers::DeclareMatchingRulesOutputs(DataMatching, PinProperties);
	return PinProperties;
}

bool UPCGExSampleStampPointsSettings::IsPinUsedByNodeExecution(const UPCGPin* InPin) const
{
	if (InPin->Properties.Label == PCGExSorting::Labels::SourceSortingRules)
	{
		return SampleMethod == EPCGExSampleMethod::BestCandidate;
	}
	if (InPin->Properties.Label == PCGExBlending::Labels::SourceBlendingLabel)
	{
		return BlendingInterface == EPCGExBlendingInterface::Individual && InPin->EdgeCount() > 0;
	}
	return Super::IsPinUsedByNodeExecution(InPin);
}

PCGEX_INITIALIZE_ELEMENT(SampleStampPoints)

PCGExData::EIOInit UPCGExSampleStampPointsSettings::GetMainDataInitializationPolicy() const
{
	return PCGExData::EIOInit::Duplicate;
}

PCGEX_ELEMENT_BATCH_POINT_IMPL(SampleStampPoints)

bool FPCGExSampleStampPointsElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(SampleStampPoints)

	PCGEX_FWD(ApplySampling)
	Context->ApplySampling.Init();

	PCGEX_FOREACH_FIELD_STAMPPOINTS(PCGEX_OUTPUT_VALIDATE_NAME)

	if (Settings->BlendingInterface == EPCGExBlendingInterface::Individual)
	{
		PCGExFactories::GetInputFactories<UPCGExBlendOpFactory>(Context, PCGExBlending::Labels::SourceBlendingLabel, Context->BlendingFactories, {FPCGExDataTypeInfoBlendOp::AsId()}, false);
	}

	Context->TargetsHandler = MakeShared<PCGExMatching::FTargetsHandler>();
	Context->TargetsHandler->Init(Context, PCGExCommon::Labels::SourceTargetsLabel);

	Context->NumMaxTargets = Context->TargetsHandler->GetMaxNumTargets();
	if (!Context->NumMaxTargets)
	{
		PCGEX_LOG_MISSING_INPUT(Context, FTEXT("No targets (empty datasets)"))
		return false;
	}

	Context->TargetsHandler->SetDistances(Settings->DistanceDetails);

	if (Settings->SampleMethod == EPCGExSampleMethod::BestCandidate)
	{
		Context->Sorter = MakeShared<PCGExSorting::FSorter>(PCGExSorting::GetSortingRules(Context, PCGExSorting::Labels::SourceSortingRules));
		Context->Sorter->SortDirection = Settings->SortDirection;
	}

	if (!Context->BlendingFactories.IsEmpty())
	{
		// Resolve blend op configs once, here, single-threaded: per-processor blender init then
		// only instantiates ops (no concurrent metadata enumeration on shared target facades),
		// and the preloader warms exactly the attribute set the ops will read.
		Context->BlendOpsSchema = MakeShared<PCGExBlending::FBlendOpsSchema>();
		if (!Context->BlendOpsSchema->Init(Context, Context->BlendingFactories, Context->TargetsHandler->GetFacades()))
		{
			return false;
		}
	}

	Context->TargetsHandler->ForEachPreloader([&](PCGExData::FFacadePreloader& Preloader)
	{
		if (Settings->WeightMode != EPCGExSampleWeightMode::Distance)
		{
			Preloader.Register<double>(Context, Settings->WeightAttribute);
		}

		Settings->TargetMinRange.RegisterBufferDependencies(Context, Preloader);
		Settings->TargetMaxRange.RegisterBufferDependencies(Context, Preloader);
		Settings->TargetRangeScale.RegisterBufferDependencies(Context, Preloader);

		if (Context->BlendOpsSchema)
		{
			Context->BlendOpsSchema->RegisterBuffersDependencies(Context, Preloader);
		}
	});

	Context->WeightCurve = Settings->WeightCurveLookup.MakeLookup(
		Settings->bUseLocalCurve, Settings->LocalWeightOverDistance, Settings->WeightOverDistance,
		[](FRichCurve& CurveData)
		{
			CurveData.AddKey(0, 0);
			CurveData.AddKey(1, 1);
		});

	return true;
}

bool FPCGExSampleStampPointsElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExSampleStampPointsElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(SampleStampPoints)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		Context->SetState(PCGExCommon::States::State_FacadePreloading);

		TWeakPtr<FPCGContextHandle> WeakHandle = Context->GetWeakSelfHandle();
		Context->TargetsHandler->TargetsPreloader->OnCompleteCallback = [Settings, Context, WeakHandle]()
		{
			PCGEX_SHARED_CONTEXT_VOID(WeakHandle)

			const int32 NumTargets = Context->TargetsHandler->Num();
			Context->TargetMinRanges.SetNum(NumTargets);
			Context->TargetMaxRanges.SetNum(NumTargets);
			Context->TargetRangeScales.SetNum(NumTargets);

			const bool bError = Context->TargetsHandler->ForEachTarget([&](const TSharedRef<PCGExData::FFacade>& Target, const int32 TargetIndex, bool& bBreak)
			{
				// Prep weights
				if (Settings->WeightMode != EPCGExSampleWeightMode::Distance)
				{
					TSharedPtr<PCGExData::TBuffer<double>> Weight = Target->GetBroadcaster<double>(Settings->WeightAttribute);
					if (!Weight)
					{
						PCGEX_LOG_INVALID_SELECTOR_C(Context, Target Weight, Settings->WeightAttribute)
						bBreak = true;
						return;
					}

					Context->TargetWeights.Add(Weight);
				}

				// Prep look up getters
				if (Settings->LookAtUpSelection == EPCGExSampleSource::Target)
				{
					TSharedPtr<PCGExDetails::TSettingValue<FVector>> LookAtUpGetter = Settings->GetValueSettingLookAtUp();
					if (!LookAtUpGetter->Init(Target, false))
					{
						bBreak = true;
						return;
					}

					Context->TargetLookAtUpGetters.Add(LookAtUpGetter);
				}

				// Prep per-target ranges
				TSharedPtr<PCGExDetails::TSettingValue<double>> MinRange = Settings->TargetMinRange.GetValueSetting();
				TSharedPtr<PCGExDetails::TSettingValue<double>> MaxRange = Settings->TargetMaxRange.GetValueSetting();
				TSharedPtr<PCGExDetails::TSettingValue<double>> RangeScale = Settings->TargetRangeScale.GetValueSetting();

				if (!MinRange->Init(Target, false) || !MaxRange->Init(Target, false) || !RangeScale->Init(Target, false))
				{
					bBreak = true;
					return;
				}

				Context->TargetMinRanges[TargetIndex] = MinRange;
				Context->TargetMaxRanges[TargetIndex] = MaxRange;
				Context->TargetRangeScales[TargetIndex] = RangeScale;
			});

			if (bError)
			{
				Context->CancelExecution();
				return;
			}

			// Per-target range octrees. The range is an attribute, so this has to follow the preload; each task writes
			// only its own entry, touches only the by-value captures, and the scope blocks until every target is indexed.
			// Items inflate by the larger of min/max: the sample lambda swaps an inverted pair instead of rejecting it.
			Context->RangeIndex = MakeShared<PCGExMatching::FTargetsRangeIndex>(Context->TargetsHandler.ToSharedRef());
			{
				PCGExAsyncHelpers::FAsyncExecutionScope BuildTasks(NumTargets);
				const EPCGExDistance TargetDistanceMode = Settings->DistanceDetails.Target;

				for (int32 IO = 0; IO < NumTargets; IO++)
				{
					BuildTasks.Execute(
						[IO, TargetDistanceMode, RangeIndex = Context->RangeIndex, MinRange = Context->TargetMinRanges[IO], MaxRange = Context->TargetMaxRanges[IO], RangeScale = Context->TargetRangeScales[IO]]()
						{
							RangeIndex->BuildTarget(
								IO, TargetDistanceMode,
								[&](const int32 PointIndex)
								{
									const double Reach = FMath::Max(MinRange->Read(PointIndex), MaxRange->Read(PointIndex));
									return FMath::Max(0.0, Reach * FMath::Abs(RangeScale->Read(PointIndex)));
								});
						});
				}
			}
			Context->RangeIndex->BuildDataOctree();

			Context->TargetsHandler->SetMatchingDetails(Context, &Settings->DataMatching);

			if (Context->Sorter && !Context->Sorter->Init(Context, Context->TargetsHandler->GetFacades()))
			{
				Context->CancelExecution(TEXT("Invalid sort rules"));
				return;
			}

			if (!Context->StartBatchProcessingPoints(
				[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
				{
					return true;
				},
				[&](const TSharedPtr<PCGExPointsMT::IBatch>& NewBatch)
				{
				}))
			{
				Context->CancelExecution(TEXT("Could not find any points to sample."));
			}
		};

		Context->TargetsHandler->StartLoading(Context->GetTaskManager());
		if (Context->IsWaitingForTasks())
		{
			return false;
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	Context->MainPoints->StageOutputs();

	return Context->TryComplete();
}

namespace PCGExSampleStampPoints
{
	FProcessor::~FProcessor()
	{
	}

	void FProcessor::SamplingFailed(const int32 Index)
	{
		SamplingMask[Index] = false;

		const TConstPCGValueRange<FTransform> Transforms = PointDataFacade->GetIn()->GetConstTransformValueRange();

		const double FailDist = Settings->FailedSampleDistance;
		PCGEX_OUTPUT_VALUE(Success, Index, false)
		PCGEX_OUTPUT_VALUE(Transform, Index, Transforms[Index])
		PCGEX_OUTPUT_VALUE(LookAtTransform, Index, Transforms[Index])
		PCGEX_OUTPUT_VALUE(Distance, Index, FailDist)
		PCGEX_OUTPUT_VALUE(SignedDistance, Index, FailDist)
		PCGEX_OUTPUT_VALUE(ComponentWiseDistance, Index, FVector(FailDist))
		PCGEX_OUTPUT_VALUE(Angle, Index, 0)
		PCGEX_OUTPUT_VALUE(NumSamples, Index, 0)
		PCGEX_OUTPUT_VALUE(SampledIndex, Index, -1)
	}

	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExSampleStampPoints::Process);

		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!IProcessor::Process(InTaskManager))
		{
			return false;
		}

		if (Settings->bIgnoreSelf)
		{
			IgnoreList.Add(PointDataFacade->GetIn());
		}

		if (PCGExMatching::FScope MatchingScope(Context->InitialMainPointsNum, true);
			!Context->TargetsHandler->PopulateIgnoreList(PointDataFacade->Source, MatchingScope, IgnoreList))
		{
			(void)Context->TargetsHandler->HandleUnmatchedOutput(PointDataFacade, true);
			return false;
		}

		PCGEX_INIT_IO(PointDataFacade->Source, PCGExData::EIOInit::Duplicate)

		EPCGPointNativeProperties AllocateFor = EPCGPointNativeProperties::None;
		if (Context->ApplySampling.WantsApply())
		{
			AllocateFor |= EPCGPointNativeProperties::Transform;
		}
		PointDataFacade->GetOut()->AllocateProperties(AllocateFor);

		SamplingMask.SetNumUninitialized(PointDataFacade->GetNum());

		{
			const TSharedRef<PCGExData::FFacade>& OutputFacade = PointDataFacade;
			PCGEX_FOREACH_FIELD_STAMPPOINTS(PCGEX_OUTPUT_INIT)
		}

		if (!Context->BlendingFactories.IsEmpty())
		{
			UnionBlendOpsManager = MakeShared<PCGExBlending::FUnionOpsManager>(&Context->BlendingFactories, Context->TargetsHandler->GetDistances());
			if (!UnionBlendOpsManager->Init(Context, PointDataFacade, Context->TargetsHandler->GetFacades(), Context->BlendOpsSchema))
			{
				return false;
			}
			DataBlender = UnionBlendOpsManager;
		}
		else if (Settings->BlendingInterface == EPCGExBlendingInterface::Monolithic)
		{
			TSet<FName> MissingAttributes;
			PCGExBlending::AssembleBlendingDetails(Settings->PointPropertiesBlendingSettings, Settings->TargetAttributes, Context->TargetsHandler->GetFacades(), BlendingDetails, MissingAttributes);

			UnionBlender = MakeShared<PCGExBlending::FUnionBlender>(&BlendingDetails, nullptr, Context->TargetsHandler->GetDistances());
			UnionBlender->AddSources(Context->TargetsHandler->GetFacades());
			if (!UnionBlender->Init(Context, PointDataFacade))
			{
				return false;
			}
			DataBlender = UnionBlender;
		}

		if (!DataBlender)
		{
			TSharedPtr<PCGExBlending::FDummyUnionBlender> DummyUnionBlender = MakeShared<PCGExBlending::FDummyUnionBlender>();
			DummyUnionBlender->Init(PointDataFacade, Context->TargetsHandler->GetFacades());
			DataBlender = DummyUnionBlender;
		}

		if (Settings->bWriteLookAtTransform)
		{
			if (Settings->LookAtUpSelection != EPCGExSampleSource::Target)
			{
				LookAtUpGetter = Settings->GetValueSettingLookAtUp();
				if (!LookAtUpGetter->Init(PointDataFacade))
				{
					return false;
				}
			}
		}
		else
		{
			LookAtUpGetter = PCGExDetails::MakeSettingValue(Settings->LookAtUpConstant);
		}

		if (Settings->RangeMode == EPCGExStampRangeMode::Combined)
		{
			SourceMinGetter = Settings->SourceMinRange.GetValueSetting();
			SourceMaxGetter = Settings->SourceMaxRange.GetValueSetting();
			SourceScaleGetter = Settings->SourceRangeScale.GetValueSetting();

			if (!SourceMinGetter->Init(PointDataFacade) || !SourceMaxGetter->Init(PointDataFacade) || !SourceScaleGetter->Init(PointDataFacade))
			{
				return false;
			}
		}
		else
		{
			SourceMinGetter = PCGExDetails::MakeSettingValue<double>(0.0);
			SourceMaxGetter = PCGExDetails::MakeSettingValue<double>(0.0);
			SourceScaleGetter = PCGExDetails::MakeSettingValue<double>(1.0);
		}

		bSingleSample = Settings->SampleMethod != EPCGExSampleMethod::WithinRange;

		StartParallelLoopForPoints();

		return true;
	}

	void FProcessor::PrepareLoopScopesForPoints(const TArray<PCGExMT::FScope>& Loops)
	{
		TProcessor<FPCGExSampleStampPointsContext, UPCGExSampleStampPointsSettings>::PrepareLoopScopesForPoints(Loops);
		MaxSampledDistanceScoped = MakeShared<PCGExMT::TScopedNumericValue<double>>(Loops, 0);
	}

	void FProcessor::ProcessPoints(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGEx::SampleStampPoints::ProcessPoints);

		const bool bWeightUseAttr = Settings->WeightMode == EPCGExSampleWeightMode::Attribute;
		const bool bWeightUseAttrMult = Settings->WeightMode == EPCGExSampleWeightMode::AttributeMult;
		const bool bFullRange = Settings->WeightMethod == EPCGExRangeType::FullRange;
		const bool bSampleClosest = Settings->SampleMethod == EPCGExSampleMethod::ClosestTarget;
		const bool bSampleFarthest = Settings->SampleMethod == EPCGExSampleMethod::FarthestTarget;
		const bool bSampleBest = Settings->SampleMethod == EPCGExSampleMethod::BestCandidate;
		const EPCGExDistance SourceDistanceMode = Settings->DistanceDetails.Source;

		PointDataFacade->Fetch(Scope);
		FilterScope(Scope);

		bool bLocalAnySuccess = false;

		TArray<PCGExData::FWeightedPoint> OutWeightedPoints;
		TArray<PCGEx::FOpStats> Trackers;
		DataBlender->InitTrackers(Trackers);

		UPCGBasePointData* OutPointData = PointDataFacade->GetOut();
		TConstPCGValueRange<FTransform> InTransforms = PointDataFacade->GetIn()->GetConstTransformValueRange();

		// Weights are resolved per entry before insertion, so the union never remaps them.
		const TSharedPtr<PCGExSampling::FSampingUnionData> Union = MakeShared<PCGExSampling::FSampingUnionData>();
		Union->Reserve(Context->TargetsHandler->Num());
		Union->WeightRange = -2;

		// Samples are collected first so Effective Range can resolve against the sampled distances.
		struct FSampleEntry
		{
			PCGExData::FElement Target;
			double Dist;
			double Min;
			double Max;
		};

		TArray<FSampleEntry, TInlineAllocator<8>> Entries;

		const bool bProcessFilteredOutAsFails = Settings->bProcessFilteredOutAsFails;
		const double DefaultDet = bSampleClosest ? TNumericLimits<double>::Max() : TNumericLimits<double>::Min();

		PCGEX_SCOPE_LOOP(Index)
		{
			if (!PointFilterCache[Index])
			{
				if (bProcessFilteredOutAsFails)
				{
					SamplingFailed(Index);
				}
				continue;
			}

			Union->Reset();
			Entries.Reset();

			// Source-side contribution to the effective range; 0 unless RangeMode is Combined.
			const double SourceScale = FMath::Abs(SourceScaleGetter->Read(Index));
			const double SourceMin = FMath::Max(0.0, SourceMinGetter->Read(Index) * SourceScale);
			const double SourceMax = FMath::Max(0.0, SourceMaxGetter->Read(Index) * SourceScale);

			const PCGExData::FConstPoint Point = PointDataFacade->GetInPoint(Index);
			const FVector Origin = InTransforms[Index].GetLocation();

			PCGExData::FElement SinglePick(-1, -1);
			double Det = DefaultDet;

			auto SampleTarget = [&](const PCGExData::FConstPoint& Target)
			{
				const double Dist = FMath::Sqrt(Context->TargetsHandler->GetDistSquared(Point, Target));

				const double TargetScale = FMath::Abs(Context->TargetRangeScales[Target.IO]->Read(Target.Index));
				double Min = FMath::Max(0.0, Context->TargetMinRanges[Target.IO]->Read(Target.Index) * TargetScale) + SourceMin;
				double Max = FMath::Max(0.0, Context->TargetMaxRanges[Target.IO]->Read(Target.Index) * TargetScale) + SourceMax;

				if (Min > Max)
				{
					std::swap(Min, Max);
				}

				if (Dist < Min || Dist > Max)
				{
					return;
				}

				const FSampleEntry Entry{static_cast<PCGExData::FElement>(Target), Dist, Min, Max};

				if (!bSingleSample)
				{
					Entries.Add(Entry);
					return;
				}

				bool bReplaceWithCurrent = Entries.IsEmpty();

				if (bSampleBest)
				{
					if (SinglePick.Index != -1)
					{
						bReplaceWithCurrent = Context->Sorter->Sort(Entry.Target, SinglePick);
					}
				}
				else if ((bSampleClosest && Det > Dist) || (bSampleFarthest && Det < Dist))
				{
					bReplaceWithCurrent = true;
				}

				if (bReplaceWithCurrent)
				{
					SinglePick = Entry.Target;
					Det = Dist;
					Entries.Reset();
					Entries.Add(Entry);
				}
			};

			// Larger of the two, since an inverted pair is swapped rather than rejected.
			const FBox QueryBox = PCGExMatching::FTargetsRangeIndex::GetSpatializedBox(Point, SourceDistanceMode).ExpandBy(FMath::Max(SourceMin, SourceMax));
			Context->RangeIndex->FindElementsWithBoundsTest(FBoxCenterAndExtent(QueryBox), SampleTarget, &IgnoreList);

			if (Entries.IsEmpty())
			{
				SamplingFailed(Index);
				continue;
			}

			// Resolve weights: Full Range uses each pair's own range, Effective Range the sampled span.
			double SampledMin = TNumericLimits<double>::Max();
			double SampledMax = 0;
			if (!bFullRange)
			{
				for (const FSampleEntry& Entry : Entries)
				{
					SampledMin = FMath::Min(SampledMin, Entry.Dist);
					SampledMax = FMath::Max(SampledMax, Entry.Dist);
				}
			}

			double WeightedDistance = 0;
			for (const FSampleEntry& Entry : Entries)
			{
				const double Min = bFullRange ? Entry.Min : SampledMin;
				const double Max = bFullRange ? Entry.Max : SampledMax;
				const double Width = Max - Min;
				const double T = Width > 0 ? FMath::Clamp((Entry.Dist - Min) / Width, 0.0, 1.0) : 0.0;

				double W = 1.0 - T;
				if (bWeightUseAttr)
				{
					W = Context->TargetWeights[Entry.Target.IO]->Read(Entry.Target.Index);
				}
				else if (bWeightUseAttrMult)
				{
					W *= Context->TargetWeights[Entry.Target.IO]->Read(Entry.Target.Index);
				}

				Union->AddWeighted_Unsafe(Entry.Target, W);
				WeightedDistance += Entry.Dist;
			}
			WeightedDistance /= Entries.Num();

			DataBlender->ComputeWeights(Index, Union, OutWeightedPoints);

			FTransform WeightedTransform = FTransform::Identity;
			WeightedTransform.SetScale3D(FVector::ZeroVector);

			FVector WeightedUp = SafeUpVector;
			if (Settings->LookAtUpSelection == EPCGExSampleSource::Source)
			{
				WeightedUp = LookAtUpGetter->Read(Index);
			}

			FVector WeightedSignAxis = FVector::ZeroVector;
			FVector WeightedAngleAxis = FVector::ZeroVector;

			// Post-process weighted points and compute local data
			PCGEx::FOpStats SampleTracker{};
			for (PCGExData::FWeightedPoint& P : OutWeightedPoints)
			{
				const double W = Context->WeightCurve->Eval(P.Weight);

				// Don't remap blending if we use external blend ops; they have their own curve
				if (Settings->BlendingInterface == EPCGExBlendingInterface::Monolithic)
				{
					P.Weight = W;
				}

				SampleTracker.Count++;
				SampleTracker.TotalWeight += W;

				const FTransform& TargetTransform = Context->TargetsHandler->GetPoint(P).GetTransform();
				const FQuat TargetRotation = TargetTransform.GetRotation();

				WeightedTransform = PCGExTypeOps::FTypeOps<FTransform>::WeightedAdd(WeightedTransform, TargetTransform, W);

				if (Settings->LookAtUpSelection == EPCGExSampleSource::Target)
				{
					WeightedUp = PCGExTypeOps::FTypeOps<FVector>::WeightedAdd(WeightedUp, Context->TargetLookAtUpGetters[P.IO]->Read(P.Index), W);
				}

				WeightedSignAxis += PCGExMath::GetDirection(TargetRotation, Settings->SignAxis) * W;
				WeightedAngleAxis += PCGExMath::GetDirection(TargetRotation, Settings->AngleAxis) * W;
			}

			// Blend using updated weighted points
			DataBlender->Blend(Index, OutWeightedPoints, Trackers);

			if (SampleTracker.TotalWeight != 0) // Dodge NaN
			{
				WeightedUp = PCGExTypeOps::FTypeOps<FVector>::NormalizeWeight(WeightedUp, SampleTracker.TotalWeight);
				WeightedTransform = PCGExTypeOps::FTypeOps<FTransform>::NormalizeWeight(WeightedTransform, SampleTracker.TotalWeight);
			}
			else
			{
				WeightedTransform = InTransforms[Index];
			}

			WeightedUp.Normalize();

			const FVector CWDistance = Origin - WeightedTransform.GetLocation();
			FVector LookAt = CWDistance.GetSafeNormal();

			FTransform LookAtTransform = PCGExMath::MakeLookAtTransform(LookAt, WeightedUp, Settings->LookAtAxisAlign);
			if (Context->ApplySampling.WantsApply())
			{
				PCGExData::FMutablePoint MutablePoint(OutPointData, Index);
				Context->ApplySampling.Apply(MutablePoint, WeightedTransform, LookAtTransform);
			}

			SamplingMask[Index] = true;
			PCGEX_OUTPUT_VALUE(Success, Index, true)
			PCGEX_OUTPUT_VALUE(Transform, Index, WeightedTransform)
			PCGEX_OUTPUT_VALUE(LookAtTransform, Index, LookAtTransform)
			PCGEX_OUTPUT_VALUE(Distance, Index, Settings->bOutputNormalizedDistance ? WeightedDistance : WeightedDistance * Settings->DistanceScale)
			PCGEX_OUTPUT_VALUE(SignedDistance, Index, FMath::Sign(WeightedSignAxis.Dot(LookAt)) * WeightedDistance * Settings->SignedDistanceScale)
			PCGEX_OUTPUT_VALUE(ComponentWiseDistance, Index, Settings->bAbsoluteComponentWiseDistance ? PCGExTypes::Abs(CWDistance) : CWDistance)
			PCGEX_OUTPUT_VALUE(Angle, Index, PCGExSampling::Helpers::GetAngle(Settings->AngleRange, WeightedAngleAxis, LookAt))
			PCGEX_OUTPUT_VALUE(NumSamples, Index, SampleTracker.Count)
			PCGEX_OUTPUT_VALUE(SampledIndex, Index, SinglePick.Index)

			MaxSampledDistanceScoped->Set(Scope, FMath::Max(MaxSampledDistanceScoped->Get(Scope), WeightedDistance));
			bLocalAnySuccess = true;
		}

		if (bLocalAnySuccess)
		{
			FPlatformAtomics::InterlockedExchange(&bAnySuccess, 1);
		}
	}

	void FProcessor::OnPointsProcessingComplete()
	{
		if (Settings->bOutputNormalizedDistance && DistanceWriter)
		{
			MaxSampledDistance = MaxSampledDistanceScoped->Max();

			// Failed samples keep their sentinel; an all-zero pass has nothing to normalize.
			if (MaxSampledDistance > 0)
			{
				const int32 NumPoints = PointDataFacade->GetNum();
				const double InvMaxDist = 1.0 / MaxSampledDistance;
				const double Scale = Settings->DistanceScale;

				if (Settings->bOutputOneMinusDistance)
				{
					for (int i = 0; i < NumPoints; i++)
					{
						if (!SamplingMask[i])
						{
							continue;
						}
						const double D = DistanceWriter->GetValue(i);
						DistanceWriter->SetValue(i, (1.0 - D * InvMaxDist) * Scale);
					}
				}
				else
				{
					for (int i = 0; i < NumPoints; i++)
					{
						if (!SamplingMask[i])
						{
							continue;
						}
						const double D = DistanceWriter->GetValue(i);
						DistanceWriter->SetValue(i, D * InvMaxDist * Scale);
					}
				}
			}
		}

		if (UnionBlendOpsManager)
		{
			UnionBlendOpsManager->Cleanup(Context);
		}
		PointDataFacade->WriteFastest(TaskManager);

		if (Settings->bTagIfHasSuccesses && bAnySuccess)
		{
			PointDataFacade->Source->Tags->AddRaw(Settings->HasSuccessesTag);
		}
		if (Settings->bTagIfHasNoSuccesses && !bAnySuccess)
		{
			PointDataFacade->Source->Tags->AddRaw(Settings->HasNoSuccessesTag);
		}
	}

	void FProcessor::CompleteWork()
	{
		if (Settings->bPruneFailedSamples)
		{
			(void)PointDataFacade->Source->Gather(SamplingMask);
		}
	}

	void FProcessor::Cleanup()
	{
		TProcessor<FPCGExSampleStampPointsContext, UPCGExSampleStampPointsSettings>::Cleanup();
		UnionBlendOpsManager.Reset();
	}
}


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
