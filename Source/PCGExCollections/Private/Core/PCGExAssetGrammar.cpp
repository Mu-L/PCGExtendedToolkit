// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Core/PCGExAssetGrammar.h"

#include "Core/PCGExAssetCollection.h"
#include "Elements/Grammar/PCGSubdivisionBase.h"

namespace PCGExAssetGrammar
{
	/** Extract the bounds extent along the given single-axis bit. Returns 0 for an invalid mask. */
	static double AxisExtent(const FBox& InBounds, const EPCGExGrammarAxes Axis)
	{
		const FVector S = InBounds.GetSize();
		switch (Axis)
		{
		case EPCGExGrammarAxes::X: return S.X;
		case EPCGExGrammarAxes::Y: return S.Y;
		case EPCGExGrammarAxes::Z: return S.Z;
		default:                   return 0.0;
		}
	}
}

#pragma region FPCGExGrammarAxisDetails

double FPCGExGrammarAxisDetails::ApplyOp(const double InResolved) const
{
	switch (SizeOp)
	{
	case EPCGExGrammarSizeOp::Offset:   return InResolved + FixedSize;
	case EPCGExGrammarSizeOp::Multiply: return InResolved * FixedSize;
	default:                            return InResolved;
	}
}

#pragma endregion

#pragma region FPCGExAssetGrammarDetails

const FPCGExGrammarAxisDetails& FPCGExAssetGrammarDetails::GetAxisDetails(const EPCGExGrammarAxes Axis) const
{
	switch (Axis)
	{
	case EPCGExGrammarAxes::Y: return SizingY;
	case EPCGExGrammarAxes::Z: return SizingZ;
	default:                   return SizingX;
	}
}

FPCGExGrammarAxisDetails& FPCGExAssetGrammarDetails::GetAxisDetailsMutable(const EPCGExGrammarAxes Axis)
{
	return const_cast<FPCGExGrammarAxisDetails&>(const_cast<const FPCGExAssetGrammarDetails*>(this)->GetAxisDetails(Axis));
}

double FPCGExAssetGrammarDetails::GetLeafSize(const FBox& InBounds, const EPCGExGrammarAxes Axis) const
{
	if (!HasAxis(Axis)) { return 0.0; }

	const FPCGExGrammarAxisDetails& A = GetAxisDetails(Axis);

	double Resolved = 0.0;
	switch (A.Size)
	{
	case EPCGExGrammarAxisSize::Bounds:
		Resolved = A.ApplyOp(PCGExAssetGrammar::AxisExtent(InBounds, Axis));
		break;
	case EPCGExGrammarAxisSize::Fixed:
		// Fixed mode treats FixedSize as the literal size; SizeOp is ignored by contract.
		Resolved = A.FixedSize;
		break;
	default:
		// Min/Max/Average are subcollection-only; reached only on misconfigured leaves.
		return 0.0;
	}

	return FMath::Max(1.0, Resolved);
}

double FPCGExAssetGrammarDetails::GetSubCollectionSize(
	const UPCGExAssetCollection* SubCollection,
	const EPCGExGrammarAxes Axis,
	TMap<const FPCGExAssetCollectionEntry*, double>* SizeCache) const
{
	if (!HasAxis(Axis) || !SubCollection) { return 0.0; }

	const FPCGExGrammarAxisDetails& A = GetAxisDetails(Axis);

	if (A.Size == EPCGExGrammarAxisSize::Fixed)
	{
		return FMath::Max(1.0, A.FixedSize);
	}

	if (A.Size == EPCGExGrammarAxisSize::Bounds)
	{
		// Subcollections have no own bounds -- invalid combination, customization should prevent.
		return 0.0;
	}

	UPCGExAssetCollection* MutableSub = const_cast<UPCGExAssetCollection*>(SubCollection);
	const PCGExAssetCollection::FCache* Cache = MutableSub->LoadCache();
	if (!Cache || !Cache->Main) { return 0.0; }
	const int32 NumEntries = Cache->Main->Order.Num();
	if (NumEntries == 0) { return 0.0; }

	double Aggregate = 0.0;
	double Samples = 0.0;

	if (A.Size == EPCGExGrammarAxisSize::Min) { Aggregate = TNumericLimits<double>::Max(); }

	for (int32 i = 0; i < NumEntries; i++)
	{
		FPCGExEntryAccessResult Result = MutableSub->GetEntryAt(i);
		if (!Result.IsValid()) { continue; }

		const double ChildSize = Result.Entry->GetGrammarSize(Result.Host, Axis, SizeCache);
		if (ChildSize <= 0.0) { continue; } // disabled-axis on this child -- skip the sample

		switch (A.Size)
		{
		case EPCGExGrammarAxisSize::Min:
			Aggregate = FMath::Min(Aggregate, ChildSize);
			break;
		case EPCGExGrammarAxisSize::Max:
			Aggregate = FMath::Max(Aggregate, ChildSize);
			break;
		case EPCGExGrammarAxisSize::Average:
			Aggregate += ChildSize;
			break;
		default: ;
		}
		Samples += 1.0;
	}

	if (Samples == 0.0) { return 0.0; }

	if (A.Size == EPCGExGrammarAxisSize::Average) { Aggregate /= Samples; }

	return FMath::Max(1.0, A.ApplyOp(Aggregate));
}

bool FPCGExAssetGrammarDetails::FixLeaf(const FBox& InBounds, const EPCGExGrammarAxes Axis, FPCGSubdivisionSubmodule& OutSubmodule) const
{
	if (!HasAxis(Axis)) { return false; }

	const FPCGExGrammarAxisDetails& A = GetAxisDetails(Axis);
	OutSubmodule.Symbol = Symbol;
	OutSubmodule.DebugColor = DebugColor;
	OutSubmodule.Size = GetLeafSize(InBounds, Axis);
	OutSubmodule.bScalable = A.bScalable;
	return true;
}

bool FPCGExAssetGrammarDetails::FixSubCollection(
	const UPCGExAssetCollection* SubCollection,
	const EPCGExGrammarAxes Axis,
	FPCGSubdivisionSubmodule& OutSubmodule,
	TMap<const FPCGExAssetCollectionEntry*, double>* SizeCache) const
{
	if (!HasAxis(Axis)) { return false; }

	const FPCGExGrammarAxisDetails& A = GetAxisDetails(Axis);
	OutSubmodule.Symbol = Symbol;
	OutSubmodule.DebugColor = DebugColor;
	OutSubmodule.Size = GetSubCollectionSize(SubCollection, Axis, SizeCache);
	OutSubmodule.bScalable = A.bScalable;
	return true;
}

#if WITH_EDITOR

bool FPCGExAssetGrammarDetails::MigrateFromV0Internal()
{
	// Symbol == NAME_None on a v0 entry means no grammar -- preserve that as Axes=None.
	if (Symbol.IsNone())
	{
		Axes = static_cast<uint8>(EPCGExGrammarAxes::None);
		return false;
	}

	bool bDowngradedAggregation = false;
	EPCGExGrammarAxes TargetAxis = EPCGExGrammarAxes::X;
	EPCGExGrammarAxisSize TargetSize = EPCGExGrammarAxisSize::Bounds;

	switch (Size_DEPRECATED)
	{
	case EPCGExGrammarSizeReference::X:
		TargetAxis = EPCGExGrammarAxes::X;
		TargetSize = EPCGExGrammarAxisSize::Bounds;
		break;
	case EPCGExGrammarSizeReference::Y:
		TargetAxis = EPCGExGrammarAxes::Y;
		TargetSize = EPCGExGrammarAxisSize::Bounds;
		break;
	case EPCGExGrammarSizeReference::Z:
		TargetAxis = EPCGExGrammarAxes::Z;
		TargetSize = EPCGExGrammarAxisSize::Bounds;
		break;
	case EPCGExGrammarSizeReference::Fixed:
		TargetAxis = EPCGExGrammarAxes::X;
		TargetSize = EPCGExGrammarAxisSize::Fixed;
		break;
	case EPCGExGrammarSizeReference::Min:
	case EPCGExGrammarSizeReference::Max:
	case EPCGExGrammarSizeReference::Average:
		// Min/Max/Average on leaves had no real meaning under per-axis grammar -- closest
		// preservation is "use X bounds". Caller logs once per affected entry.
		TargetAxis = EPCGExGrammarAxes::X;
		TargetSize = EPCGExGrammarAxisSize::Bounds;
		bDowngradedAggregation = true;
		break;
	}

	Axes = static_cast<uint8>(TargetAxis);
	FPCGExGrammarAxisDetails& Target = GetAxisDetailsMutable(TargetAxis);
	Target.Size = TargetSize;
	Target.SizeOp = SizeOp_DEPRECATED;
	Target.FixedSize = FixedSize_DEPRECATED;
	Target.bScalable = (ScaleMode_DEPRECATED == EPCGExGrammarScaleMode::Flex);

	return bDowngradedAggregation;
}

void FPCGExAssetGrammarDetails::MigrateFromLegacyCollectionGrammar(const FPCGExCollectionGrammarDetails& Legacy)
{
	Symbol = Legacy.Symbol;
	DebugColor = Legacy.DebugColor;

	if (Symbol.IsNone())
	{
		Axes = static_cast<uint8>(EPCGExGrammarAxes::None);
		return;
	}

	// Legacy CollectionGrammar was single-axis (X-equivalent) with subcollection-aggregation mode.
	Axes = static_cast<uint8>(EPCGExGrammarAxes::X);
	FPCGExGrammarAxisDetails& Target = SizingX;

	switch (Legacy.SizeMode)
	{
	case EPCGExCollectionGrammarSize::Fixed:   Target.Size = EPCGExGrammarAxisSize::Fixed; break;
	case EPCGExCollectionGrammarSize::Min:     Target.Size = EPCGExGrammarAxisSize::Min; break;
	case EPCGExCollectionGrammarSize::Max:     Target.Size = EPCGExGrammarAxisSize::Max; break;
	case EPCGExCollectionGrammarSize::Average: Target.Size = EPCGExGrammarAxisSize::Average; break;
	}
	Target.SizeOp = EPCGExGrammarSizeOp::None;
	Target.FixedSize = Legacy.Size;
	Target.bScalable = (Legacy.ScaleMode == EPCGExGrammarScaleMode::Flex);
}

bool FPCGExAssetGrammarDetails::ValidateContext(const bool bIsSubCollection)
{
	bool bChanged = false;
	FPCGExGrammarAxisDetails* Axes3[3] = { &SizingX, &SizingY, &SizingZ };
	for (FPCGExGrammarAxisDetails* A : Axes3)
	{
		if (bIsSubCollection)
		{
			// Bounds is leaf-only; subcollections have no own bounds. Snap to Fixed.
			if (A->Size == EPCGExGrammarAxisSize::Bounds)
			{
				A->Size = EPCGExGrammarAxisSize::Fixed;
				bChanged = true;
			}
		}
		else
		{
			// Min/Max/Average aggregate child entries; only meaningful for subcollections.
			if (A->Size == EPCGExGrammarAxisSize::Min
				|| A->Size == EPCGExGrammarAxisSize::Max
				|| A->Size == EPCGExGrammarAxisSize::Average)
			{
				A->Size = EPCGExGrammarAxisSize::Fixed;
				bChanged = true;
			}
		}
	}
	return bChanged;
}

#endif

#pragma endregion
