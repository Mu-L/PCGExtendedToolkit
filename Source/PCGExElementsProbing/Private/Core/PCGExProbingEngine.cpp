// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Core/PCGExProbingEngine.h"

#include "PCGExH.h"
#include "Containers/PCGExScopedContainers.h"
#include "Core/PCGExMTCommon.h"
#include "Core/PCGExProbeFactoryProvider.h"
#include "Core/PCGExProbeOperation.h"
#include "Core/PCGExProbingCandidates.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Helpers/PCGExArrayHelpers.h"

namespace PCGExProbing
{
	FProbingEngine::FProbingEngine(const TSharedRef<PCGExData::FFacade>& InDataFacade)
		: DataFacade(InDataFacade)
	{
		WorkingTransformsPtr = &WorkingTransforms;
		WorkingPositionsPtr = &WorkingPositions;
	}

	FProbingEngine::~FProbingEngine()
	{
	}

	void FProbingEngine::SetCoincidence(const bool bInPreventCoincidence, const FVector& InTolerance)
	{
		bPreventCoincidence = bInPreventCoincidence;
		CWCoincidenceTolerance = PCGEx::SafeTolerance(InTolerance);
	}

	void FProbingEngine::SetProjection(const FPCGExGeo2DProjectionDetails& InDetails)
	{
		ProjectionDetails = InDetails;
		bUseProjection = true;
	}

	void FProbingEngine::SetExternalWorkingData(const TArray<FTransform>* InTransforms, const TArray<FVector>* InPositions)
	{
		check(InTransforms && InPositions)
		WorkingTransformsPtr = InTransforms;
		WorkingPositionsPtr = InPositions;
		bExternalWorkingData = true;
	}

	bool FProbingEngine::Init(FPCGExContext* InContext, const TArray<TObjectPtr<const UPCGExProbeFactoryData>>& InFactories)
	{
		NumPoints = DataFacade->GetNum();

		if (bUseProjection)
		{
			ProjectionDetails.Init(DataFacade);
		}

		CanGenerate.SetNumUninitialized(NumPoints);
		AcceptConnections.SetNumUninitialized(NumPoints);

		if (!bExternalWorkingData)
		{
			PCGExArrayHelpers::InitArray(WorkingTransforms, NumPoints);
			PCGExArrayHelpers::InitArray(WorkingPositions, NumPoints);
		}

		AllOperations.Reserve(InFactories.Num());

		for (const UPCGExProbeFactoryData* Factory : InFactories)
		{
			TSharedPtr<FPCGExProbeOperation> NewOperation = Factory->CreateOperation(InContext);
			NewOperation->BindContext(InContext);
			NewOperation->PrimaryDataFacade = DataFacade;

			NewOperation->WorkingTransforms = WorkingTransformsPtr;
			NewOperation->WorkingPositions = WorkingPositionsPtr;
			NewOperation->CanGenerate = &CanGenerate;
			NewOperation->AcceptConnections = &AcceptConnections;

			if (!NewOperation->Prepare(InContext))
			{
				continue;
			}

			AllOperations.Add(NewOperation);

			if (NewOperation->WantsOctree())
			{
				bWantsOctree = true;
			}

			if (NewOperation->IsGlobalProbe())
			{
				GlobalOperations.Add(NewOperation.Get());
				continue;
			}

			if (NewOperation->IsDirectProbe())
			{
				DirectOperations.Add(NewOperation.Get());
				continue;
			}

			if (!NewOperation->SearchRadius->IsConstant())
			{
				bUseVariableRadius = true;
			}
			SharedSearchRadius = FMath::Max(SharedSearchRadius, NewOperation->BaseConfig->SearchRadiusConstant);

			if (NewOperation->RequiresChainProcessing())
			{
				ChainedOperations.Add(NewOperation.Get());
			}
			else
			{
				SharedOperations.Add(NewOperation.Get());
			}

			RadiusSources.Add(NewOperation.Get());
		}

		NumRadiusSources = RadiusSources.Num();
		NumChainedOps = ChainedOperations.Num();
		NumSharedOps = SharedOperations.Num();
		NumDirectOps = DirectOperations.Num();
		NumGlobalOps = GlobalOperations.Num();

		if (!RadiusSources.IsEmpty())
		{
			bWantsOctree = true;
		}

		bOnlyGlobalOps = RadiusSources.IsEmpty() && DirectOperations.IsEmpty();

		if (bOnlyGlobalOps && GlobalOperations.IsEmpty())
		{
			return false;
		}

		return true;
	}

	void FProbingEngine::PrepareWorkingData()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExProbing::FProbingEngine::PrepareWorkingData);

		const UPCGBasePointData* PointData = DataFacade->Source->GetInOut();

		if (!bExternalWorkingData)
		{
			const TConstPCGValueRange<FTransform> OriginalTransforms = PointData->GetConstTransformValueRange();

			PCGExMT::ParallelOrSequential(
				NumPoints,
				[&](const int32 i)
				{
					if (bUseProjection)
					{
						WorkingTransforms[i] = ProjectionDetails.ProjectFlat(OriginalTransforms[i]);
						WorkingPositions[i] = WorkingTransforms[i].GetLocation();
					}
					else
					{
						WorkingTransforms[i] = OriginalTransforms[i];
						WorkingPositions[i] = OriginalTransforms[i].GetLocation();
					}
				});
		}

		if (bWantsOctree)
		{
			const FBox B = PointData->GetBounds();
			Octree = MakeUnique<PCGExOctree::FItemOctree>(bUseProjection ? ProjectionDetails.ProjectFlat(B.GetCenter()) : B.GetCenter(), B.GetExtent().Length());

			constexpr double PPRefRadius = 0.05;
			const FVector PPRefExtents = FVector(PPRefRadius);
			const TArray<FVector>& Positions = *WorkingPositionsPtr;

			for (int i = 0; i < NumPoints; i++)
			{
				if (!AcceptConnections[i])
				{
					continue;
				}
				Octree->AddElement(PCGExOctree::FItem(i, FBoxSphereBounds(Positions[i], PPRefExtents, PPRefRadius)));
			}

			for (const TSharedPtr<FPCGExProbeOperation>& Operation : AllOperations)
			{
				Operation->Octree = Octree.Get();
			}
		}
	}

	int32 FProbingEngine::GetNumIterations() const
	{
		return GeneratorIndices ? GeneratorIndices->Num() : NumPoints;
	}

	void FProbingEngine::PrepareScopes(const TArray<PCGExMT::FScope>& Loops)
	{
		ScopedEdges = MakeShared<PCGExMT::TScopedSet<uint64>>(Loops, 10);
	}

	void FProbingEngine::ProcessScope(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExProbing::FProbingEngine::ProcessScope);

		TSet<uint64>* LocalUniqueEdges = ScopedEdges->Get(Scope).Get();
		TUniquePtr<TSet<uint64>> LocalCoincidence;
		if (bPreventCoincidence)
		{
			LocalCoincidence = MakeUnique<TSet<uint64>>();
		}

#define PCGEX_SCOPED_CONTAINERS(_NAME)\
		TArray<TSharedPtr<PCGExMT::FScopedContainer>> _NAME##OpsContainers;\
		_NAME##OpsContainers.Reserve(Num##_NAME##Ops);\
		for (int i = 0; i < Num##_NAME##Ops; i++){ _NAME##OpsContainers.Add(_NAME##Operations[i]->GetScopedContainer(Scope)); }

		PCGEX_SCOPED_CONTAINERS(Chained)
		PCGEX_SCOPED_CONTAINERS(Shared)
		PCGEX_SCOPED_CONTAINERS(Direct)

#undef PCGEX_SCOPED_CONTAINERS

		TArray<FCandidate> Candidates;
		TArray<FBestCandidate> BestCandidates;

		const TArray<FVector>& Positions = *WorkingPositionsPtr;
		const TArray<int32>* GroupIds = RequiresEdgePostFilter() ? PointGroupIds : nullptr;
		const bool bWantsSameGroup = EdgeRelation == EEdgeRelation::SameGroup;

		FVector Origin = FVector::ZeroVector;
		int32 CurrentIndex = 0;
		int32 CurrentGroupId = -1;

		auto ProcessPoint = [&](const PCGExOctree::FItem& InPositionRef)
		{
			const int32 OtherPointIndex = InPositionRef.Index;
			if (OtherPointIndex == CurrentIndex)
			{
				return;
			}
			if (GroupIds && (((*GroupIds)[OtherPointIndex] == CurrentGroupId) != bWantsSameGroup))
			{
				return;
			}

			const FVector Position = Positions[OtherPointIndex];
			const FVector Dir = (Origin - Position).GetSafeNormal();
			const int32 EmplaceIndex = Candidates.Emplace(OtherPointIndex, Dir, FVector::DistSquared(Position, Origin), bPreventCoincidence ? PCGEx::SH3(Dir, CWCoincidenceTolerance) : 0);

			if (NumChainedOps > 0)
			{
				for (int i = 0; i < NumChainedOps; i++)
				{
					ChainedOperations[i]->ProcessCandidateChained(i, EmplaceIndex, Candidates[EmplaceIndex], BestCandidates[i], ChainedOpsContainers[i].Get());
				}
			}
		};

		for (int It = Scope.Start; It < Scope.End; It++)
		{
			const int32 Index = GeneratorIndices ? (*GeneratorIndices)[It] : It;

			if (!CanGenerate[Index])
			{
				continue;
			} // Not a generator

#define PCGEX_SCOPED_CONTAINERS_RESET(_NAME) for (const TSharedPtr<PCGExMT::FScopedContainer>& Container : _NAME##OpsContainers){ if(Container){ Container->Reset(); } }

			PCGEX_SCOPED_CONTAINERS_RESET(Chained)
			PCGEX_SCOPED_CONTAINERS_RESET(Shared)
			PCGEX_SCOPED_CONTAINERS_RESET(Direct)

#undef PCGEX_SCOPED_CONTAINERS_RESET

			CurrentIndex = Index;
			if (GroupIds)
			{
				CurrentGroupId = (*GroupIds)[Index];
			}
			Candidates.Reset();

			if (LocalCoincidence)
			{
				LocalCoincidence->Reset();
			}

			if (NumChainedOps > 0)
			{
				BestCandidates.SetNum(NumChainedOps);
				for (int i = 0; i < NumChainedOps; i++)
				{
					ChainedOperations[i]->PrepareBestCandidate(Index, BestCandidates[i], ChainedOpsContainers[i].Get());
				}
			}

			if (!RadiusSources.IsEmpty())
			{
				double MaxRadius = 0;
				if (!bUseVariableRadius)
				{
					MaxRadius = SharedSearchRadius;
				}
				else
				{
					for (int i = 0; i < NumRadiusSources; i++)
					{
						MaxRadius = FMath::Max(MaxRadius, RadiusSources[i]->GetSearchRadius(Index));
					}
				}

				Origin = Positions[Index];

				// Find candidates within radius
				Octree->FindElementsWithBoundsTest(FBoxCenterAndExtent(Origin, FVector(MaxRadius)), ProcessPoint);
				Candidates.Sort([&](const FCandidate& A, const FCandidate& B)
				{
					return A.Distance < B.Distance;
				});

				for (int i = 0; i < NumChainedOps; i++)
				{
					ChainedOperations[i]->ProcessBestCandidate(Index, BestCandidates[i], Candidates, LocalCoincidence.Get(), CWCoincidenceTolerance, LocalUniqueEdges, ChainedOpsContainers[i].Get());
				}

				for (int i = 0; i < NumSharedOps; i++)
				{
					SharedOperations[i]->ProcessCandidates(Index, Candidates, LocalCoincidence.Get(), CWCoincidenceTolerance, LocalUniqueEdges, SharedOpsContainers[i].Get());
				}
			}

			for (int i = 0; i < NumDirectOps; i++)
			{
				DirectOperations[i]->ProcessNode(Index, LocalCoincidence.Get(), CWCoincidenceTolerance, LocalUniqueEdges, DirectOpsContainers[i].Get());
			}
		}
	}

	void FProbingEngine::CollapseScopedEdges()
	{
		{
			FWriteScopeLock WriteScopeLock(UniqueEdgesLock);
			if (RequiresEdgePostFilter())
			{
				ScopedEdges->ForEach([&](TSet<uint64>& Set) { AppendEdgesFiltered_Unsafe(Set); });
			}
			else
			{
				ScopedEdges->Collapse(UniqueEdges);
			}
		}

		ScopedEdges.Reset();
	}

	void FProbingEngine::AppendEdges(const TSet<uint64>& InUniqueEdges)
	{
		FWriteScopeLock WriteScopeLock(UniqueEdgesLock);

		if (RequiresEdgePostFilter())
		{
			AppendEdgesFiltered_Unsafe(InUniqueEdges);
			return;
		}

		UniqueEdges.Reserve(UniqueEdges.Num() + InUniqueEdges.Num());
		UniqueEdges.Append(InUniqueEdges);
	}

	void FProbingEngine::AppendEdgesFiltered_Unsafe(const TSet<uint64>& InEdges)
	{
		// Direct & global probes bypass candidate gathering; enforce the relation on their raw output.
		const TArray<int32>& Groups = *PointGroupIds;
		const bool bWantsSameGroup = EdgeRelation == EEdgeRelation::SameGroup;

		UniqueEdges.Reserve(UniqueEdges.Num() + InEdges.Num());
		for (const uint64 E : InEdges)
		{
			uint32 A;
			uint32 B;
			PCGEx::H64(E, A, B);
			if ((Groups[A] == Groups[B]) != bWantsSameGroup)
			{
				continue;
			}
			UniqueEdges.Add(E);
		}
	}
}
