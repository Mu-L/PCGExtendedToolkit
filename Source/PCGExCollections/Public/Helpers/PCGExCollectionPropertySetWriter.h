// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "PCGExPropertyWriter.h"

class UPCGExAssetCollection;
struct FPCGExAssetCollectionEntry;
struct FPCGExContext;
class FPCGMetadataAttributeBase;
class UPCGMetadata;

namespace PCGExCollections
{
	/**
	 * Scan a set of collections for a property prototype by name. First match wins; returns
	 * nullptr if no collection declares the property. Used when building writers that will
	 * later be fed values from heterogeneous hosts.
	 */
	PCGEXCOLLECTIONS_API const FInstancedStruct* FindPrototypeProperty(
		FName PropertyName,
		TConstArrayView<const UPCGExAssetCollection*> Collections);

	/**
	 * Resolve the effective source property for a given entry: per-entry override first,
	 * then the host collection's default. Returns nullptr if neither is present/valid.
	 */
	PCGEXCOLLECTIONS_API const FInstancedStruct* ResolveEntrySourceProperty(
		const FPCGExAssetCollectionEntry* Entry,
		const UPCGExAssetCollection* Host,
		FName PropertyName);

	/**
	 * Writes FPCGExPropertyOutputSettings-driven custom properties to an attribute set
	 * (UPCGMetadata) for collection-backed generation nodes.
	 *
	 * Lifecycle (single-threaded init, per-entry writes):
	 *   Writer.Initialize(Context, OutputSettings, RootCollection, FallbackHosts, Metadata);
	 *   for (... entries ...)
	 *   {
	 *       const int64 Key = Metadata->AddEntry();
	 *       // Write your node-specific attributes here
	 *       Writer.WriteEntry(Key, Entry, Host);
	 *   }
	 *
	 * Prototype resolution: scans RootCollection->CollectionProperties first, then each
	 * FallbackHost's CollectionProperties if not found. Missing properties log a warning
	 * and are skipped. Attributes are created on the provided Metadata.
	 *
	 * Per-entry source resolution: Entry->PropertyOverrides takes precedence, then
	 * Host->CollectionProperties default. Type mismatches between source and writer are
	 * skipped silently (leaves default value on the attribute).
	 */
	class PCGEXCOLLECTIONS_API FPCGExCollectionPropertySetWriter
	{
	public:
		FPCGExCollectionPropertySetWriter() = default;

		/**
		 * Scan output configs, find prototypes, clone writer instances and create attributes.
		 * Safe to call with no outputs configured - HasOutputs() will simply return false.
		 *
		 * @param InContext         Context for warning logs.
		 * @param OutputSettings    The user-facing output config.
		 * @param RootCollection    Primary collection to search for property prototypes.
		 * @param FallbackHosts     Additional hosts to scan when a property is missing from RootCollection.
		 * @param Metadata          Metadata owning the attribute set being written.
		 * @return true if at least one writer was successfully initialized.
		 */
		bool Initialize(
			FPCGExContext* InContext,
			const FPCGExPropertyOutputSettings& OutputSettings,
			const UPCGExAssetCollection* RootCollection,
			TConstArrayView<const UPCGExAssetCollection*> FallbackHosts,
			UPCGMetadata* Metadata);

		/**
		 * Write all configured property values for the given metadata key.
		 * Resolves per-entry override then falls back to Host default.
		 */
		void WriteEntry(int64 Key, const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Host);

		bool HasOutputs() const { return Writers.Num() > 0; }

	protected:
		struct FWriter
		{
			FName PropertyName;
			FInstancedStruct WriterInstance;
			FPCGMetadataAttributeBase* Attribute = nullptr;
		};

		TArray<FWriter> Writers;
	};
}
