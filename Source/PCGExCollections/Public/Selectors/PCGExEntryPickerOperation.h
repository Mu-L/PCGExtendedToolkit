// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Core/PCGExAssetCollection.h"
#include "Factories/PCGExOperation.h"

namespace PCGExData
{
	class FFacade;
}

struct FPCGExContext;
class UPCGExAssetCollection;

namespace PCGExCollections
{
	class FSelectorSharedData;
}

/**
 * Abstract hot-path operation for picking an entry from a collection's category.
 *
 * One operation is bound to exactly one FCategory* target -- either Cache->Main or a
 * named sub-category. FSelectorHelper creates one op for Main and one per named
 * category at init, then routes per-point picks based on the point's Category attribute.
 *
 * Concrete subclasses implement Pick() with a tight, branch-minimal body.
 */
class PCGEXCOLLECTIONS_API FPCGExEntryPickerOperation : public FPCGExOperation
{
public:
	/** The category to pick from. Bound once at PrepareForData time; const in the hot path. */
	PCGExAssetCollection::FCategory* Target = nullptr;

	/**
	 * The collection the bound category belongs to. Bound once at PrepareForData time.
	 * Ops that need to resolve per-entry properties via FPCGExAssetCollectionEntry::GetResolvedProperty
	 * consume this during PrepareForData to pre-compute per-entry state.
	 */
	const UPCGExAssetCollection* OwningCollection = nullptr;

	/**
	 * Optional collection-derived shared data. When non-null, the op reads shared state from this
	 * cached instance instead of rebuilding it inline. Set by FSelectorHelper prior to PrepareForData
	 * when a FSelectorSharedDataCache is wired on the consumer context. Null means self-build.
	 */
	TSharedPtr<PCGExCollections::FSelectorSharedData> SharedData;

	/**
	 * Bind the operation to a data facade, a category target, and the owning collection.
	 * @return false if the target is null or otherwise unusable.
	 */
	virtual bool PrepareForData(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InDataFacade, PCGExAssetCollection::FCategory* InTarget, const UPCGExAssetCollection* InOwningCollection);

	/**
	 * Pick a raw Entries-array index from the bound target. Returns -1 if no valid pick.
	 * Called per-point in parallel scopes -- must be thread-safe and free of mutation.
	 */
	virtual int32 Pick(int32 PointIndex, int32 Seed) const = 0;
};
