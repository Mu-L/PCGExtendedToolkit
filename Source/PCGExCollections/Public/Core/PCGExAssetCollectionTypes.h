// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"

#include "PCGExAssetCollectionTypes.generated.h"

class UPCGExAssetCollection;
struct FPCGExAssetCollectionEntry;
#if WITH_EDITOR
struct FAssetData;
#endif

/**
 * Runtime type registry for collection types. Allows the system to discover, query,
 * and check inheritance between collection types without compile-time coupling.
 *
 * Built-in types (registered via PCGEX_REGISTER_COLLECTION_TYPE):
 *   Base → Mesh, Actor, PCGDataAsset
 *
 * Registering a custom type:
 *   1. Define a FTypeId constant (just an FName):
 *        inline const FTypeId MyType = FName(TEXT("MyType"));
 *   2. In your collection .cpp, use the macro:
 *        PCGEX_REGISTER_COLLECTION_TYPE(MyType, UMyCollection, FMyEntry, "My Collection", Base)
 *      This registers at static init via pending registration (safe before module load).
 *   3. Your collection's GetTypeId() should return your FTypeId.
 *
 * Querying:
 *   FTypeRegistry::Get().Find(TypeIds::Mesh)       -- get FTypeInfo by ID
 *   FTypeRegistry::Get().FindByClass(UClass*)       -- reverse lookup from UClass
 *   FTypeRegistry::Get().IsA(Mesh, Base)            -- inheritance check
 *   Entry->IsType(TypeIds::Mesh)                    -- check entry type
 */

namespace PCGExAssetCollection
{
	// Type identifier - FName for debuggability and Unreal integration
	using FTypeId = FName;

	// "Native" type IDs
	namespace TypeIds
	{
		inline const FTypeId None = NAME_None;
		inline const FTypeId Base = FName(TEXT("Base"));
		inline const FTypeId Mesh = FName(TEXT("Mesh"));
		inline const FTypeId SkinnedMesh = FName(TEXT("SkinnedMesh"));
		inline const FTypeId Actor = FName(TEXT("Actor"));
		inline const FTypeId PCGDataAsset = FName(TEXT("PCGDataAsset"));
		inline const FTypeId Level = FName(TEXT("Level"));
		inline const FTypeId Variant = FName(TEXT("Variant"));
		inline const FTypeId Omni = FName(TEXT("Omni"));
	}

	/**
	 * Information about a registered collection type
	 */
	struct PCGEXCOLLECTIONS_API FTypeInfo
	{
		FTypeId Id = NAME_None;
		TWeakObjectPtr<UClass> CollectionClass = nullptr;

		// Null for heterogeneous collection types (Omni) that have no single entry struct --
		// resolve per entry via Entry->GetTypeId() -> Find(TypeId)->EntryStruct instead.
		UScriptStruct* EntryStruct = nullptr;

		FText DisplayName;
		FTypeId ParentType = NAME_None; // For inheritance checking

#if WITH_EDITOR
		/**
		 * Editor-only: return true when the given content-browser asset can seed an entry of
		 * this type. Consumed by heterogeneous collections (Omni) to route dropped assets to
		 * an entry type. Register via FTypeRegistry::AddPendingCustomization so ordering vs
		 * the type's own registration never matters. Null = type doesn't participate.
		 */
		TFunction<bool(const FAssetData&)> DetectSourceAsset;

		/**
		 * Editor-only, optional: build a fully-initialized entry payload from a detected
		 * source asset (payload struct must derive FPCGExAssetCollectionEntry). Only needed
		 * when the generic path (InitializeAs(EntryStruct) + SetAssetPath(asset path)) is
		 * insufficient -- e.g. Actor entries resolve a Blueprint's GeneratedClass first.
		 * Return false to reject the asset after all.
		 */
		TFunction<bool(const FAssetData&, FInstancedStruct&)> MakeEntryFromSourceAsset;

		/** Lower values are offered the asset first when several detectors could claim it. */
		int32 SourceDetectPriority = 100;
#endif

		bool IsValid() const
		{
			return Id != NAME_None && CollectionClass.IsValid();
		}
	};

	/**
	 * Singleton registry for collection types
	 */
	class PCGEXCOLLECTIONS_API FTypeRegistry
	{
	public:
		static FTypeRegistry& Get();

		/**
		 * Register a new collection type
		 * @return The registered type ID, or NAME_None if registration failed
		 */
		FTypeId Register(const FTypeInfo& Info);

		/** Find type info by ID */
		const FTypeInfo* Find(FTypeId Id) const;

		/** Find type info by collection class */
		const FTypeInfo* FindByClass(const UClass* Class) const;

		/** Find type info by entry struct */
		const FTypeInfo* FindByEntryStruct(const UScriptStruct* Struct) const;

		/**
		 * Apply a mutator to a registered entry (e.g. to attach editor-only source-asset
		 * detection). Mutator must NOT re-enter the registry -- FRWLock is non-recursive.
		 * Logs a warning if Id isn't registered; prefer AddPendingCustomization when the
		 * registration order is not guaranteed.
		 */
		void Customize(FTypeId Id, TFunctionRef<void(FTypeInfo&)> Mutator);

		/** Check if a type is or derives from another type */
		bool IsA(FTypeId Type, FTypeId BaseType) const;

		/** Get all registered type IDs */
		void GetAllTypeIds(TArray<FTypeId>& OutIds) const;

		/** Iterate over all registered types */
		template <typename Func>
		void ForEach(Func&& Callback) const
		{
			FReadScopeLock Lock(RegistryLock);
			for (const auto& Pair : Types)
			{
				Callback(Pair.Value);
			}
		}

		static void AddPendingRegistration(TFunction<void()>&& Func);

		/**
		 * Queue a customization applied AFTER every pending registration has run (or
		 * immediately if registration already happened) -- static-init ordering between the
		 * type's registration TU and the customizing TU never matters. Use this to attach
		 * DetectSourceAsset / MakeEntryFromSourceAsset to types registered elsewhere.
		 */
		static void AddPendingCustomization(FTypeId Id, TFunction<void(FTypeInfo&)>&& Mutator);

		static void ProcessPendingRegistrations();

	private:
		FTypeRegistry() = default;

		static TArray<TFunction<void()>>& GetPendingRegistrations();
		static TArray<TPair<FTypeId, TFunction<void(FTypeInfo&)>>>& GetPendingCustomizations();
		static bool& IsProcessed();

		mutable FRWLock RegistryLock;
		TMap<FTypeId, FTypeInfo> Types;
		TMap<TWeakObjectPtr<UClass>, FTypeId> ClassToType;
		TMap<UScriptStruct*, FTypeId> StructToType;
	};

	/**
	 * Helper macro for registering collection types at static init
	 * Place in the cpp file of your collection class
	 */
#define PCGEX_REGISTER_COLLECTION_TYPE(_TypeId, _CollectionClass, _EntryStruct, _DisplayName, _ParentType) \
namespace { \
struct FAutoRegister##_CollectionClass { \
FAutoRegister##_CollectionClass() { \
PCGExAssetCollection::FTypeRegistry::AddPendingRegistration([]() { \
PCGExAssetCollection::FTypeInfo Info; \
Info.Id = PCGExAssetCollection::TypeIds::_TypeId; \
Info.CollectionClass = _CollectionClass::StaticClass(); \
Info.EntryStruct = _EntryStruct::StaticStruct(); \
Info.DisplayName = NSLOCTEXT("PCGEx", #_TypeId "Collection", _DisplayName); \
Info.ParentType = PCGExAssetCollection::TypeIds::_ParentType; \
PCGExAssetCollection::FTypeRegistry::Get().Register(Info); \
}); \
} \
} GAutoRegister##_CollectionClass; \
}
}

/**
 * Type set - efficient storage for multiple type IDs
 * Replaces the bit-flag enum approach
 */
USTRUCT(BlueprintType)
struct PCGEXCOLLECTIONS_API FPCGExCollectionTypeSet
{
	GENERATED_BODY()

	FPCGExCollectionTypeSet() = default;

	explicit FPCGExCollectionTypeSet(PCGExAssetCollection::FTypeId SingleType);

	FPCGExCollectionTypeSet(std::initializer_list<PCGExAssetCollection::FTypeId> InTypes);

	void Add(PCGExAssetCollection::FTypeId Type)
	{
		Types.Add(Type);
	}

	void Remove(PCGExAssetCollection::FTypeId Type)
	{
		Types.Remove(Type);
	}

	bool Contains(PCGExAssetCollection::FTypeId Type) const
	{
		return Types.Contains(Type);
	}

	bool IsEmpty() const
	{
		return Types.IsEmpty();
	}

	int32 Num() const
	{
		return Types.Num();
	}

	// Check if this set contains a type or any of its parent types
	bool ContainsOrDerives(PCGExAssetCollection::FTypeId Type) const;

	FPCGExCollectionTypeSet operator|(const FPCGExCollectionTypeSet& Other) const;
	FPCGExCollectionTypeSet operator&(const FPCGExCollectionTypeSet& Other) const;

private:
	UPROPERTY()
	TSet<FName> Types;
};
