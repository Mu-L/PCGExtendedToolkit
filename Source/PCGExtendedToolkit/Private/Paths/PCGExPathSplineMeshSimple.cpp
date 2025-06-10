﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Paths/PCGExPathSplineMeshSimple.h"

#include "PCGExHelpers.h"


#include "Paths/PCGExPaths.h"

#define LOCTEXT_NAMESPACE "PCGExPathSplineMeshSimpleElement"
#define PCGEX_NAMESPACE BuildCustomGraph

PCGEX_INITIALIZE_ELEMENT(PathSplineMeshSimple)

UPCGExPathSplineMeshSimpleSettings::UPCGExPathSplineMeshSimpleSettings(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (SplineMeshUpVectorAttribute.GetName() == FName("@Last")) { SplineMeshUpVectorAttribute.Update(TEXT("$Rotation.Up")); }
}

bool FPCGExPathSplineMeshSimpleElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPathProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(PathSplineMeshSimple)

	if (Settings->bApplyCustomTangents)
	{
		PCGEX_VALIDATE_NAME_CONSUMABLE(Settings->ArriveTangentAttribute)
		PCGEX_VALIDATE_NAME_CONSUMABLE(Settings->LeaveTangentAttribute)
	}

	if (Settings->AssetType == EPCGExInputValueType::Attribute)
	{
		PCGEX_VALIDATE_NAME_CONSUMABLE(Settings->AssetPathAttributeName)

		TArray<FName> Names = {Settings->AssetPathAttributeName};
		Context->StaticMeshLoader = MakeShared<PCGEx::TAssetLoader<UStaticMesh>>(Context, Context->MainPoints.ToSharedRef(), Names);
	}
	else
	{
		Context->StaticMesh = PCGExHelpers::LoadBlocking_AnyThread(Settings->StaticMesh);
		if (!Context->StaticMesh)
		{
			PCGE_LOG_C(Error, GraphAndLog, Context, FTEXT("Static mesh could not be loaded."));
			return false;
		}
	}
	return true;
}

bool FPCGExPathSplineMeshSimpleElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExPathSplineMeshSimpleElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(PathSplineMeshSimple)
	PCGEX_EXECUTION_CHECK

	if (Context->StaticMesh)
	{
		PCGEX_ON_INITIAL_EXECUTION
		{
			Context->SetState(PCGEx::State_WaitingOnAsyncWork);
		}
	}
	else
	{
		PCGEX_ON_INITIAL_EXECUTION
		{
			if (!Context->StaticMeshLoader->Start(Context->GetAsyncManager(), PCGEx::State_WaitingOnAsyncWork))
			{
				PCGE_LOG(Error, GraphAndLog, FTEXT("Failed to find any asset to load."));
				return true;
			}

			return false;
		}

		if (!Context->StaticMeshLoader->Execute()) { return false; }
	}

	PCGEX_ON_STATE(PCGEx::State_WaitingOnAsyncWork)
	{
		PCGEX_ON_INVALILD_INPUTS(FTEXT("Some inputs have less than 2 points and won't be processed."))

		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExPathSplineMeshSimple::FProcessor>>(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				if (Entry->GetNum() < 2)
				{
					bHasInvalidInputs = true;
					Entry->InitializeOutput(PCGExData::EIOInit::Forward);
					return false;
				}
				return true;
			},
			[&](const TSharedPtr<PCGExPointsMT::TBatch<PCGExPathSplineMeshSimple::FProcessor>>& NewBatch)
			{
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any paths to write tangents to."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGEx::State_Done)

	Context->MainBatch->Output();

	Context->MainPoints->StageOutputs();
	Context->ExecuteOnNotifyActors(Settings->PostProcessFunctionNames);

	return Context->TryComplete();
}

namespace PCGExPathSplineMeshSimple
{
	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InAsyncManager)
	{
		// Must be set before process for filters
		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!FPointsProcessor::Process(InAsyncManager)) { return false; }

		PCGEX_INIT_IO(PointDataFacade->Source, PCGExData::EIOInit::Duplicate)

		MutationDetails = Settings->MutationDetails;
		if (!MutationDetails.Init(Context, PointDataFacade)) { return false; }

		StartOffset = Settings->GetValueSettingStartOffset();
		if (!StartOffset->Init(Context, PointDataFacade)) { return false; }

		EndOffset = Settings->GetValueSettingEndOffset();
		if (!EndOffset->Init(Context, PointDataFacade)) { return false; }

		if (Settings->SplineMeshUpMode == EPCGExSplineMeshUpMode::Attribute)
		{
			UpGetter = PointDataFacade->GetBroadcaster<FVector>(Settings->SplineMeshUpVectorAttribute, true);

			if (!UpGetter)
			{
				PCGE_LOG_C(Error, GraphAndLog, ExecutionContext, FTEXT("Mesh Up Vector attribute is missing on some inputs."));
				return false;
			}
		}

		if (Settings->AssetType == EPCGExInputValueType::Attribute)
		{
			AssetPathReader = PointDataFacade->GetBroadcaster<FSoftObjectPath>(Settings->AssetPathAttributeName, true);
			if (!AssetPathReader)
			{
				PCGE_LOG_C(Error, GraphAndLog, ExecutionContext, FTEXT("AssetPath attribute is missing on some inputs.."));
				return false;
			}
		}

		bClosedLoop = Context->ClosedLoop.IsClosedLoop(PointDataFacade->Source);
		bUseTags = Settings->TaggingDetails.IsEnabled();

		if (Settings->bApplyCustomTangents)
		{
			ArriveReader = PointDataFacade->GetReadable<FVector>(Settings->ArriveTangentAttribute);
			if (!ArriveReader)
			{
				PCGE_LOG_C(Error, GraphAndLog, ExecutionContext, FTEXT("Could not fetch tangent' Arrive attribute on some inputs."));
				return false;
			}

			LeaveReader = PointDataFacade->GetReadable<FVector>(Settings->LeaveTangentAttribute);
			if (!LeaveReader)
			{
				PCGE_LOG_C(Error, GraphAndLog, ExecutionContext, FTEXT("Could not fetch tangent' Leave attribute on some inputs."));
				return false;
			}
		}

		LastIndex = PointDataFacade->GetNum() - 1;

		PCGEx::InitArray(Segments, bClosedLoop ? LastIndex + 1 : LastIndex);
		Meshes.Init(nullptr, Segments.Num());

		switch (Settings->SplineMeshAxisConstant)
		{
		default:
		case EPCGExMinimalAxis::None:
		case EPCGExMinimalAxis::X:
			SplineMeshAxisConstant = ESplineMeshAxis::X;
			break;
		case EPCGExMinimalAxis::Y:
			C1 = 0;
			C2 = 2;
			SplineMeshAxisConstant = ESplineMeshAxis::Y;
			break;
		case EPCGExMinimalAxis::Z:
			C1 = 1;
			C2 = 0;
			SplineMeshAxisConstant = ESplineMeshAxis::Z;
			break;
		}

		StartParallelLoopForPoints();

		return true;
	}

	void FProcessor::ProcessPoints(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGEx::PathSplineMeshSimple::ProcessPoints);

		PointDataFacade->Fetch(Scope);
		FilterScope(Scope);

		auto InvalidPoint = [&]()
		{
			// Do nothing
		};

		const UPCGBasePointData* InPointData = PointDataFacade->GetIn();
		TConstPCGValueRange<FTransform> Transforms = InPointData->GetConstTransformValueRange();

		PCGEX_SCOPE_LOOP(Index)
		{
			if (Index == LastIndex && !bClosedLoop)
			{
				// Ignore last index, only used for maths reasons
				InvalidPoint();
				continue;
			}

			if (!PointFilterCache[Index])
			{
				Segments[Index] = PCGExPaths::FSplineMeshSegment();
				InvalidPoint();
				continue;
			}

			TObjectPtr<UStaticMesh>* SM = AssetPathReader ? Context->StaticMeshLoader->GetAsset(AssetPathReader->Read(Index)) : &Context->StaticMesh;

			if (!SM)
			{
				InvalidPoint();
				continue;
			}

			Meshes[Index] = *SM;
			Segments[Index] = PCGExPaths::FSplineMeshSegment();
			PCGExPaths::FSplineMeshSegment& Segment = Segments[Index];

			//

			Segment.SplineMeshAxis = SplineMeshAxisConstant;

			const int32 NextIndex = Index + 1 > LastIndex ? 0 : Index + 1;

			//

			FVector OutScale = Transforms[Index].GetScale3D();

			//

			Segment.Params.StartPos = Transforms[Index].GetLocation();
			Segment.Params.StartScale = FVector2D(OutScale[C1], OutScale[C2]);
			Segment.Params.StartRoll = Transforms[Index].GetRotation().Rotator().Roll;

			const FVector Scale = Transforms[NextIndex].GetScale3D();
			Segment.Params.EndPos = Transforms[NextIndex].GetLocation();
			Segment.Params.EndScale = FVector2D(Scale[C1], Scale[C2]);
			Segment.Params.EndRoll = Transforms[NextIndex].GetRotation().Rotator().Roll;

			Segment.Params.StartOffset = StartOffset->Read(Index);
			Segment.Params.EndOffset = EndOffset->Read(Index);

			if (Settings->bApplyCustomTangents)
			{
				Segment.Params.StartTangent = LeaveReader->Read(Index);
				Segment.Params.EndTangent = ArriveReader->Read(NextIndex);
			}
			else
			{
				Segment.Params.StartTangent = Transforms[Index].GetRotation().GetForwardVector();
				Segment.Params.EndTangent = Transforms[NextIndex].GetRotation().GetForwardVector();
			}

			if (UpGetter) { Segment.UpVector = UpGetter->Read(Index); }
			else if (Settings->SplineMeshUpMode == EPCGExSplineMeshUpMode::Constant) { Segment.UpVector = Settings->SplineMeshUpVector; }
			else { Segment.ComputeUpVectorFromTangents(); }

			MutationDetails.Mutate(Index, Segment);
		}
	}

	void FProcessor::CompleteWork()
	{
		PointDataFacade->WriteFastest(AsyncManager);
	}

	void FProcessor::Output()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UPCGExPathSplineMeshSimple::FProcessor::Output);

		// TODO : Resolve per-point target actor...? irk.
		AActor* TargetActor = Settings->TargetActor.Get() ? Settings->TargetActor.Get() : ExecutionContext->GetTargetActor(nullptr);

		if (!TargetActor)
		{
			PCGE_LOG_C(Error, GraphAndLog, ExecutionContext, FTEXT("Invalid target actor."));
			return;
		}

		Context->AddNotifyActor(TargetActor);

		const bool bIsPreviewMode = ExecutionContext->GetComponent()->IsInPreviewMode();
		const TArray<FName> DataTags = PointDataFacade->Source->Tags->FlattenToArrayOfNames();

		for (int i = 0; i < Segments.Num(); i++)
		{
			UStaticMesh* Mesh = Meshes[i];
			if (!Mesh) { continue; }

			const PCGExPaths::FSplineMeshSegment& Segment = Segments[i];

			const EObjectFlags ObjectFlags = (bIsPreviewMode ? RF_Transient : RF_NoFlags);
			USplineMeshComponent* SplineMeshComponent = NewObject<USplineMeshComponent>(
				TargetActor, MakeUniqueObjectName(
					TargetActor, USplineMeshComponent::StaticClass(),
					Context->UniqueNameGenerator->Get(TEXT("PCGSplineMeshComponent_") + Mesh->GetName())), ObjectFlags);

			Segment.ApplySettings(SplineMeshComponent); // Init Component

			if (Settings->TaggingDetails.bForwardInputDataTags) { SplineMeshComponent->ComponentTags.Append(DataTags); }
			if (!Segment.Tags.IsEmpty()) { SplineMeshComponent->ComponentTags.Append(Segment.Tags.Array()); }

			Settings->StaticMeshDescriptor.InitComponent(SplineMeshComponent);

			SplineMeshComponent->SetStaticMesh(Mesh); // Will trigger a force rebuild, so put this last

			Context->AttachManagedComponent(
				TargetActor, SplineMeshComponent,
				FAttachmentTransformRules(EAttachmentRule::KeepWorld, EAttachmentRule::KeepWorld, EAttachmentRule::KeepWorld, false));

			Context->AddNotifyActor(TargetActor);
		}
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
