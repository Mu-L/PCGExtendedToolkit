// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExOctree.h"
#include "Helpers/PCGExTargetsHandler.h"

namespace PCGExMatching
{
	/**
	 * Per-target-point spatial index for range-gated sampling where the range lives on the target.
	 * Each item is the target point's spatialized bounds (per the target EPCGExDistance mode) expanded by that
	 * point's own max range, so a source point falls within a target's range only if it intersects that item.
	 * Composes over FTargetsHandler: the handler's own octrees are the engine's and cannot carry a per-point inflation.
	 */
	class PCGEXMATCHING_API FTargetsRangeIndex
	{
	protected:
		struct FEntry
		{
			TUniquePtr<PCGExOctree::FItemOctree> Octree;
			FBox Bounds = FBox(ForceInit);
		};

		TSharedRef<FTargetsHandler> Handler;
		TArray<FEntry> Entries;
		TUniquePtr<PCGExOctree::FItemOctree> DataOctree;

	public:
		explicit FTargetsRangeIndex(const TSharedRef<FTargetsHandler>& InHandler);

		/**
		 * Builds one target's octree; safe to call concurrently for distinct IOs (writes only Entries[IO]).
		 * RangeAt(PointIndex) must already be clamped >= 0 by the caller.
		 */
		void BuildTarget(const int32 IO, const EPCGExDistance TargetDistanceMode, TFunctionRef<double(int32)> RangeAt);

		/** Builds the outer per-data octree from every entry's bounds. Call once, single-threaded, after all BuildTarget calls. */
		void BuildDataOctree();

		/** Same contract as FTargetsHandler::FindElementsWithBoundsTest, over the range-inflated items. */
		void FindElementsWithBoundsTest(const FBoxCenterAndExtent& QueryBounds, FTargetsHandler::FPointIteratorWithData&& Func, const TSet<const UPCGData*>* Exclude = nullptr) const;

		/** World-space box the IDistances mode measures from; conservative for every EPCGExDistanceType. */
		static FBox GetSpatializedBox(const PCGExData::FConstPoint& Point, const EPCGExDistance Mode);
	};
}
