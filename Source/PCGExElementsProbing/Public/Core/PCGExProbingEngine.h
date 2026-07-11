// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExOctree.h"
#include "Math/PCGExProjectionDetails.h"
#include "UObject/ObjectPtr.h"

struct FPCGExContext;
class UPCGExProbeFactoryData;
class FPCGExProbeOperation;

namespace PCGExData
{
	class FFacade;
}

namespace PCGExMT
{
	struct FScope;
	template <typename T>
	class TScopedSet;
}

namespace PCGExProbing
{
	/** Constrains which point pairs probes may connect, based on per-point group ids. */
	enum class EEdgeRelation : uint8
	{
		Any            = 0, // No constraint
		DifferentGroup = 1, // Only points with differing group ids may connect
		SameGroup      = 2, // Only points sharing a group id may connect
	};

	/**
	 * Shared probing core: owns the probe operations, working transforms/positions, search octree,
	 * coincidence handling and edge accumulation. Extracted from Connect Points so cluster-aware
	 * nodes can run the same probing over any point facade (e.g. cluster vtx).
	 *
	 * Call sequence:
	 *   1. Configure (SetCoincidence / SetProjection / optional constraints), then Init().
	 *   2. Fill the CanGenerate & AcceptConnections masks (sized by Init, uninitialized).
	 *   3. PrepareWorkingData() - builds working transforms/positions & octree, binds operations.
	 *   4. Local ops (HasLocalWork): PrepareScopes() once, ProcessScope() per scope (parallel-safe),
	 *      CollapseScopedEdges() once all scopes are done.
	 *      Global ops (HasGlobalWork): per operation, ProcessAll() into a local set -> AppendEdges().
	 *   5. Read the result from GetUniqueEdges().
	 *
	 * With a group constraint (EEdgeRelation != Any), radius-based probes exclude non-matching
	 * candidates at gathering time (so "closest"-style probes re-pick), while direct & global probe
	 * outputs are post-filtered on accumulation - those probes name explicit targets and cannot re-pick.
	 */
	class PCGEXELEMENTSPROBING_API FProbingEngine : public TSharedFromThis<FProbingEngine>
	{
	public:
		explicit FProbingEngine(const TSharedRef<PCGExData::FFacade>& InDataFacade);
		~FProbingEngine();

		//~ Configuration - set before Init()

		void SetCoincidence(const bool bInPreventCoincidence, const FVector& InTolerance);
		void SetProjection(const FPCGExGeo2DProjectionDetails& InDetails);

		/** Optional: reuse working data computed elsewhere (must match the facade size & projection). */
		void SetExternalWorkingData(const TArray<FTransform>* InTransforms, const TArray<FVector>* InPositions);

		/** Optional: iterate generators through this list instead of [0, NumPoints).
		 * ProcessScope scopes must then span [0, GeneratorIndices->Num()). */
		const TArray<int32>* GeneratorIndices = nullptr;

		/** Optional: per-point group ids consumed by EdgeRelation. */
		const TArray<int32>* PointGroupIds = nullptr;
		EEdgeRelation EdgeRelation = EEdgeRelation::Any;

		/** Creates & buckets operations. Returns false if no operation can produce edges. */
		bool Init(FPCGExContext* InContext, const TArray<TObjectPtr<const UPCGExProbeFactoryData>>& InFactories);

		//~ Participation masks - sized by Init (uninitialized), fill before PrepareWorkingData()

		TArray<int8> CanGenerate;
		TArray<int8> AcceptConnections;

		void PrepareWorkingData();

		bool HasLocalWork() const { return !bOnlyGlobalOps; }
		bool HasGlobalWork() const { return !GlobalOperations.IsEmpty(); }
		const TArray<FPCGExProbeOperation*>& GetGlobalOperations() const { return GlobalOperations; }

		/** Loop domain size for local probing (generator list size, or point count). */
		int32 GetNumIterations() const;

		void PrepareScopes(const TArray<PCGExMT::FScope>& Loops);
		void ProcessScope(const PCGExMT::FScope& Scope);
		void CollapseScopedEdges();

		/** Thread-safe accumulation of global-probe outputs (relation post-filter applies). */
		void AppendEdges(const TSet<uint64>& InUniqueEdges);

		TSet<uint64>& GetUniqueEdges() { return UniqueEdges; }
		const TArray<FVector>& GetWorkingPositions() const { return *WorkingPositionsPtr; }
		const TArray<FTransform>& GetWorkingTransforms() const { return *WorkingTransformsPtr; }

	protected:
		TSharedRef<PCGExData::FFacade> DataFacade;
		int32 NumPoints = 0;

		TArray<TSharedPtr<FPCGExProbeOperation>> AllOperations;

		TArray<FPCGExProbeOperation*> RadiusSources;
		TArray<FPCGExProbeOperation*> DirectOperations;
		TArray<FPCGExProbeOperation*> ChainedOperations;
		TArray<FPCGExProbeOperation*> SharedOperations;
		TArray<FPCGExProbeOperation*> GlobalOperations;

		int32 NumRadiusSources = 0;
		int32 NumDirectOps = 0;
		int32 NumChainedOps = 0;
		int32 NumSharedOps = 0;
		int32 NumGlobalOps = 0;

		bool bOnlyGlobalOps = false;
		bool bWantsOctree = false;

		bool bUseVariableRadius = false;
		double SharedSearchRadius = 0;

		TUniquePtr<PCGExOctree::FItemOctree> Octree;

		// Owned storage; the pointers alias external arrays when SetExternalWorkingData was used.
		TArray<FTransform> WorkingTransforms;
		TArray<FVector> WorkingPositions;
		const TArray<FTransform>* WorkingTransformsPtr = nullptr;
		const TArray<FVector>* WorkingPositionsPtr = nullptr;
		bool bExternalWorkingData = false;

		mutable FRWLock UniqueEdgesLock;
		TSharedPtr<PCGExMT::TScopedSet<uint64>> ScopedEdges;
		TSet<uint64> UniqueEdges;

		FPCGExGeo2DProjectionDetails ProjectionDetails;
		bool bUseProjection = false;

		bool bPreventCoincidence = false;
		FVector CWCoincidenceTolerance = FVector::OneVector;

		bool RequiresEdgePostFilter() const { return EdgeRelation != EEdgeRelation::Any && PointGroupIds; }
		void AppendEdgesFiltered_Unsafe(const TSet<uint64>& InEdges);
	};
}
