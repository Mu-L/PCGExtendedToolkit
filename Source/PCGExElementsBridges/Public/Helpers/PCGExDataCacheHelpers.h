// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGPin.h"

#include "Data/PCGExDataTags.h" // TagSeparator

#include "PCGExDataCacheHelpers.generated.h"

class AActor;
class UPCGSettings;
struct FPCGExContext;

UENUM()
enum class EPCGExDataCacheTarget : uint8
{
	ExecutingActor = 0 UMETA(DisplayName = "Executing Actor", Tooltip = "The actor that owns the executing PCG component. With partitioning, this is the partition actor of the current grid cell."),
	OriginalActor  = 1 UMETA(DisplayName = "Original Actor", Tooltip = "The actor that owns the original (non-partitioned) PCG component. Same as Executing Actor when not partitioned."),
	WorldActor     = 2 UMETA(DisplayName = "PCG World Actor", Tooltip = "The level's PCG World Actor, shared by every component in the world."),
};

namespace PCGExDataCache
{
	const FName TargetActorPinLabel = TEXT("Target Actor");
	const FName StatusPinLabel = TEXT("Status");

	const FName FoundAttributeName = TEXT("Found");
	const FName DataCountAttributeName = TEXT("DataCount");
	const FName CacheIDAttributeName = TEXT("CacheID");

	/** 'CacheID:<id>' -- a Key:Value tag every PCGEx tag reader parses. */
	inline FString MakeCacheIDTag(const FName InId)
	{
		return CacheIDAttributeName.ToString() + PCGExData::TagSeparator + InId.ToString();
	}

	/**
	 * Game thread only (resolves soft paths, may spawn the PCG World Actor). When the Target Actor pin carries
	 * data, every unique actor referenced by InActorReferenceAttribute is a target (a component reference resolves
	 * to its owner); otherwise the single actor named by InTarget. bCreateWorldActor: spawn the world actor if
	 * missing (writers) or only find it (readers).
	 */
	PCGEXELEMENTSBRIDGES_API void ResolveTargetActors(
		FPCGExContext* InContext,
		const EPCGExDataCacheTarget InTarget,
		const FName InActorReferenceAttribute,
		const bool bCreateWorldActor,
		TArray<AActor*>& OutActors);

	/** User-declared pins minus None labels, reserved labels and duplicates. Set and Get must agree on this. */
	PCGEXELEMENTSBRIDGES_API TArray<FPCGPinProperties> SanitizePins(const TArray<FPCGPinProperties>& InPins, const TArrayView<const FName> InReservedLabels);

	/** True when the settings live on a node whose Target Actor pin has an edge. */
	PCGEXELEMENTSBRIDGES_API bool IsTargetPinConnected(const UPCGSettings* InSettings);
}
