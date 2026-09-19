// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExTargetsRangeIndex.h"

#include "PCGExCommon.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointElements.h"

namespace PCGExMatching
{
	namespace PCGExTargetsRangeIndex
	{
		FORCEINLINE TUniquePtr<PCGExOctree::FItemOctree> MakeOctree(const FBox& InBounds)
		{
			const FVector Extent = InBounds.GetExtent();
			const double MaxExtent = FMath::Max(FMath::Max3(Extent.X, Extent.Y, Extent.Z) * 1.5, 1.0);
			return MakeUnique<PCGExOctree::FItemOctree>(InBounds.GetCenter(), MaxExtent);
		}
	}

	FTargetsRangeIndex::FTargetsRangeIndex(const TSharedRef<FTargetsHandler>& InHandler)
		: Handler(InHandler)
	{
		Entries.SetNum(Handler->Num());
	}

	FBox FTargetsRangeIndex::GetSpatializedBox(const PCGExData::FConstPoint& Point, const EPCGExDistance Mode)
	{
		switch (Mode)
		{
		case EPCGExDistance::Center:
		case EPCGExDistance::None:
			{
				const FVector Location = Point.GetLocation();
				return FBox(Location, Location);
			}
		case EPCGExDistance::SphereBounds:
			{
				const FVector Location = Point.GetLocation();
				const FVector Radius = FVector(Point.GetScaledExtents().Length());
				return FBox(Location - Radius, Location + Radius);
			}
		case EPCGExDistance::BoxBounds:
			return FBox(Point.GetBoundsMin(), Point.GetBoundsMax()).TransformBy(Point.GetTransform());
		default:
			checkNoEntry();
			{
				const FVector Location = Point.GetLocation();
				return FBox(Location, Location);
			}
		}
	}

	void FTargetsRangeIndex::BuildTarget(const int32 IO, const EPCGExDistance TargetDistanceMode, TFunctionRef<double(int32)> RangeAt)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExMatching::FTargetsRangeIndex::BuildTarget);

		FEntry& Entry = Entries[IO];
		const TSharedRef<PCGExData::FFacade>& Facade = Handler->GetFacades()[IO];
		const int32 NumPoints = Facade->GetNum();

		if (NumPoints <= 0)
		{
			return;
		}

		// Two passes: the octree needs its root bounds before any item can be added.
		TArray<FBox> Boxes;
		Boxes.SetNumUninitialized(NumPoints);

		for (int32 i = 0; i < NumPoints; i++)
		{
			const FBox Box = GetSpatializedBox(Facade->GetInPoint(i), TargetDistanceMode).ExpandBy(RangeAt(i));
			Boxes[i] = Box;
			Entry.Bounds += Box;
		}

		Entry.Octree = PCGExTargetsRangeIndex::MakeOctree(Entry.Bounds);
		for (int32 i = 0; i < NumPoints; i++)
		{
			Entry.Octree->AddElement(PCGExOctree::FItem(i, FBoxSphereBounds(Boxes[i])));
		}
	}

	void FTargetsRangeIndex::BuildDataOctree()
	{
		FBox Bounds = FBox(ForceInit);
		for (const FEntry& Entry : Entries)
		{
			if (Entry.Octree)
			{
				Bounds += Entry.Bounds;
			}
		}

		if (!Bounds.IsValid)
		{
			DataOctree.Reset();
			return;
		}

		DataOctree = PCGExTargetsRangeIndex::MakeOctree(Bounds);
		for (int32 i = 0; i < Entries.Num(); i++)
		{
			if (Entries[i].Octree)
			{
				DataOctree->AddElement(PCGExOctree::FItem(i, FBoxSphereBounds(Entries[i].Bounds)));
			}
		}
	}

	void FTargetsRangeIndex::FindElementsWithBoundsTest(const FBoxCenterAndExtent& QueryBounds, FTargetsHandler::FPointIteratorWithData&& Func, const TSet<const UPCGData*>* Exclude) const
	{
		if (!DataOctree)
		{
			return;
		}

		const TArray<TSharedRef<PCGExData::FFacade>>& Facades = Handler->GetFacades();

		DataOctree->FindElementsWithBoundsTest(QueryBounds, [&](const PCGExOctree::FItem& DataItem)
		{
			const TSharedRef<PCGExData::FFacade>& Target = Facades[DataItem.Index];
			if (Exclude && Exclude->Contains(Target->GetIn()))
			{
				return;
			}

			const FEntry& Entry = Entries[DataItem.Index];
			check(Entry.Octree)

			Entry.Octree->FindElementsWithBoundsTest(QueryBounds, [&](const PCGExOctree::FItem& PointItem)
			{
				PCGExData::FConstPoint Point = Target->GetInPoint(PointItem.Index);
				Point.IO = DataItem.Index;
				Func(Point);
			});
		});
	}
}
