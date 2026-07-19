// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExCollectionGlobals.generated.h"

/**
 * Base for per-type collection-level global settings blocks ("type globals").
 *
 * Each collection type that exposes collection-level configuration consumed by its entries
 * (global descriptors, bounds evaluators, content filters, level exporters...) defines a
 * struct deriving from this one, mirroring those members. The struct is the interchange
 * format of UPCGExAssetCollection::GetTypeGlobals: entries query their host through it
 * instead of casting the host to a concrete collection class, so any host that can ANSWER
 * the query supports the entry -- the native typed collection, or a heterogeneous host
 * (Variant, Omni) that stores globals blocks. Hosts that cannot answer leave the entry on
 * its local-value fallbacks instead of crashing or silently misbehaving.
 *
 * Typed collections do NOT store these structs: they keep their existing UPROPERTY members
 * (serialized layout untouched, zero data migration) and assemble the struct on demand in
 * GetTypeGlobalsInternal. Only heterogeneous hosts physically store globals blocks.
 *
 * Derived structs should mirror the typed collection's member names 1:1 so conversion
 * between the two (e.g. typed -> Omni collection conversion) is a straight per-property copy.
 */
USTRUCT(BlueprintType)
struct PCGEXCOLLECTIONS_API FPCGExCollectionTypeGlobals
{
	GENERATED_BODY()

	FPCGExCollectionTypeGlobals() = default;
	virtual ~FPCGExCollectionTypeGlobals() = default;
};
