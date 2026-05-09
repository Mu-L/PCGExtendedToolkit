// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExDefaultLevelDataExporter.h"

#include "PCGDataAsset.h"
#include "PCGParamData.h"
#include "Data/PCGPointArrayData.h"
#include "Helpers/PCGExPointArrayDataHelpers.h"
#include "Helpers/PCGExCollectionsHelpers.h"

#include "Collections/PCGExMeshCollection.h"
#include "Collections/PCGExActorCollection.h"
#include "Collections/PCGExLevelCollection.h"
#include "Collections/PCGExPCGDataAssetCollection.h"

#include "Helpers/PCGExActorMeshClassificator.h"

#include "UObject/Package.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"

#include "LevelInstance/LevelInstanceActor.h"

#include "Helpers/PCGExActorPropertyDelta.h"
#include "Helpers/PCGExMetaHelpersMacros.h"
#include "Data/PCGExDataValue.h"
#include "PCGExLog.h"

#include "PCGExCollectionsSettingsCache.h"

UPCGExDefaultLevelDataExporter::UPCGExDefaultLevelDataExporter(const FObjectInitializer& ObjectInitializer)
{
	const auto& Settings = PCGEX_COLLECTIONS_SETTINGS;

	UClass* FilterClass = Settings.DefaultContentFilterClass
		                      ? Settings.DefaultContentFilterClass.Get()
		                      : UPCGExDefaultActorContentFilter::StaticClass();

	UClass* EvalClass = Settings.DefaultBoundsEvaluatorClass
		                    ? Settings.DefaultBoundsEvaluatorClass.Get()
		                    : UPCGExDefaultBoundsEvaluator::StaticClass();

	UClass* ClassificatorClass = Settings.DefaultMeshClassificatorClass
		                             ? Settings.DefaultMeshClassificatorClass.Get()
		                             : UPCGExDefaultActorMeshClassificator::StaticClass();

	ContentFilter = Cast<UPCGExActorContentFilter>(
		ObjectInitializer.CreateDefaultSubobject(this, TEXT("ContentFilter"),
		                                         UPCGExActorContentFilter::StaticClass(), FilterClass, false, false));

	MeshClassificator = Cast<UPCGExActorMeshClassificator>(
		ObjectInitializer.CreateDefaultSubobject(this, TEXT("MeshClassificator"),
		                                         UPCGExActorMeshClassificator::StaticClass(), ClassificatorClass, false, false));

	BoundsEvaluator = Cast<UPCGExBoundsEvaluator>(
		ObjectInitializer.CreateDefaultSubobject(this, TEXT("BoundsEvaluator"),
		                                         UPCGExBoundsEvaluator::StaticClass(), EvalClass, false, false));
}

EPCGExActorExportType UPCGExDefaultLevelDataExporter::ClassifyActor(AActor* Actor, UStaticMeshComponent*& OutMeshComponent) const
{
	// Level instances precede the mesh check: an ALevelInstance can incidentally have
	// a UStaticMeshComponent (gizmo/visualizer) but the meaningful payload is the
	// referenced UWorld asset.
	if (const ALevelInstance* LI = Cast<ALevelInstance>(Actor))
	{
		if (!LI->GetWorldAsset().IsNull())
		{
			return EPCGExActorExportType::Level;
		}
		// Empty world ref -- nothing to embed; fall through to actor-style export so
		// transform/tags survive the round-trip.
	}

	if (MeshClassificator && MeshClassificator->ShouldClassifyAsMesh(Actor))
	{
		OutMeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();
		if (OutMeshComponent && OutMeshComponent->GetStaticMesh())
		{
			return EPCGExActorExportType::Mesh;
		}
	}

	return EPCGExActorExportType::Actor;
}

void UPCGExDefaultLevelDataExporter::OnExportComplete(UPCGDataAsset* OutAsset)
{
	// Default: no-op. Override for custom post-export logic.
}

namespace PCGExDefaultLevelDataExporterInternal
{
	struct FClassifiedActor
	{
		AActor* Actor = nullptr;
		EPCGExActorExportType Type = EPCGExActorExportType::Skip;
		UStaticMeshComponent* MeshComponent = nullptr;
		uint32 DeltaHash = 0;
	};

	/** Helper to allocate point data with transforms+bounds, init metadata, and return ranges */
	static UPCGBasePointData* CreatePointData(
		UObject* Outer, int32 NumPoints,
		TPCGValueRange<FTransform>& OutTransforms,
		TPCGValueRange<FVector>& OutBoundsMin,
		TPCGValueRange<FVector>& OutBoundsMax)
	{
		UPCGBasePointData* PointData = NewObject<UPCGPointArrayData>(Outer);
		PCGExPointArrayDataHelpers::SetNumPointsAllocated(
			PointData, NumPoints,
			EPCGPointNativeProperties::Transform | EPCGPointNativeProperties::BoundsMin | EPCGPointNativeProperties::BoundsMax);

		OutTransforms = PointData->GetTransformValueRange();
		OutBoundsMin = PointData->GetBoundsMinValueRange();
		OutBoundsMax = PointData->GetBoundsMaxValueRange();

		return PointData;
	}

	static void InitMetadata(UPCGBasePointData* PointData, int32 NumPoints)
	{
		UPCGMetadata* Meta = PointData->MutableMetadata();
		TPCGValueRange<int64> MetaEntries = PointData->GetMetadataEntryValueRange();

		TArray<TTuple<int64, int64>> DelayedEntries;
		DelayedEntries.SetNum(NumPoints);

		for (int32 i = 0; i < NumPoints; i++)
		{
			MetaEntries[i] = Meta->AddEntryPlaceholder();
			DelayedEntries[i] = MakeTuple(MetaEntries[i], static_cast<int64>(-1));
		}

		Meta->AddDelayedEntries(DelayedEntries);
	}

	static void WorldBoundsToLocal(const FBox& WorldBounds, const FTransform& ActorTransform, FVector& OutBoundsMin, FVector& OutBoundsMax)
	{
		if (WorldBounds.IsValid)
		{
			const FTransform InvTransform = ActorTransform.Inverse();
			const FVector LocalMin = InvTransform.TransformPosition(WorldBounds.Min);
			const FVector LocalMax = InvTransform.TransformPosition(WorldBounds.Max);

			// Re-min/max after transform (rotation can swap axes)
			OutBoundsMin = LocalMin.ComponentMin(LocalMax);
			OutBoundsMax = LocalMin.ComponentMax(LocalMax);
		}
		else
		{
			OutBoundsMin = FVector::ZeroVector;
			OutBoundsMax = FVector::ZeroVector;
		}
	}

	static void WriteActorTransformAndBounds(
		AActor* Actor, int32 Index,
		const UPCGExBoundsEvaluator* Evaluator,
		TPCGValueRange<FTransform>& Transforms,
		TPCGValueRange<FVector>& BoundsMin,
		TPCGValueRange<FVector>& BoundsMax)
	{
		const FTransform ActorTransform = Actor->GetActorTransform();
		Transforms[Index] = ActorTransform;

		const FBox WorldBounds = Evaluator ? Evaluator->EvaluateActorBounds(Actor, nullptr, -1) : FBox(ForceInit);
		WorldBoundsToLocal(WorldBounds, ActorTransform, BoundsMin[Index], BoundsMax[Index]);
	}

	struct FMeshPoint
	{
		FTransform Transform;
		FVector BoundsMin = FVector::ZeroVector;
		FVector BoundsMax = FVector::ZeroVector;
		FSoftObjectPath MeshPath;
		const UStaticMeshComponent* SourceComponent = nullptr;
		AActor* SourceActor = nullptr;
		int32 MaterialVariantIndex = -1;
	};

	struct FMeshInfo
	{
		int32 EntryIndex = -1;
		const UStaticMeshComponent* FirstComponent = nullptr;
		int32 TotalCount = 0;
		TArray<TArray<FSoftObjectPath>> UniqueVariantMaterials;
		TMap<uint32, int32> VariantHashToIndex;
	};

	struct FActorClassInfo
	{
		int32 EntryIndex = -1;
		TSet<FName> IntersectedTags;
		bool bFirstActor = true;
		int32 Count = 0;
		TArray<uint8> SerializedDelta;
	};

	struct FActorInstanceKey
	{
		FSoftClassPath ClassPath;
		uint32 DeltaHash = 0;

		bool operator==(const FActorInstanceKey& Other) const
		{
			return ClassPath == Other.ClassPath && DeltaHash == Other.DeltaHash;
		}

		friend uint32 GetTypeHash(const FActorInstanceKey& Key)
		{
			return HashCombine(GetTypeHash(Key.ClassPath), Key.DeltaHash);
		}
	};

	struct FLevelInfo
	{
		int32 EntryIndex = -1;
		int32 Count = 0;
	};

	static uint32 HashMaterials(const UStaticMeshComponent* Comp)
	{
		uint32 H = 0;
		for (int32 i = 0; i < Comp->GetNumOverrideMaterials(); i++)
		{
			if (UMaterialInterface* M = Comp->GetMaterial(i))
			{
				H = HashCombine(H, GetTypeHash(FSoftObjectPath(M)));
			}
		}
		return H;
	}

	static int32 TrackMaterialVariant(const UStaticMeshComponent* Comp, FMeshInfo& Info)
	{
		if (!Comp) { return -1; }

		const uint32 MatHash = HashMaterials(Comp);
		if (MatHash == 0) { return -1; }

		if (const int32* Existing = Info.VariantHashToIndex.Find(MatHash))
		{
			return *Existing;
		}

		const int32 VariantIdx = Info.UniqueVariantMaterials.Num();
		Info.VariantHashToIndex.Add(MatHash, VariantIdx);

		TArray<FSoftObjectPath>& Mats = Info.UniqueVariantMaterials.AddDefaulted_GetRef();
		for (int32 i = 0; i < Comp->GetNumOverrideMaterials(); i++)
		{
			if (UMaterialInterface* M = Comp->GetMaterial(i))
			{
				Mats.Add(FSoftObjectPath(M));
			}
			else
			{
				Mats.Add(FSoftObjectPath());
			}
		}

		return VariantIdx;
	}

	// Registry that tracks which attribute name maps to which PCG metadata type.
	// First registration wins; subsequent conflicts are warned and discarded.
	struct FValueTagRegistry
	{
		TMap<FName, EPCGMetadataTypes> TypeMap;

		bool Register(const FName& Name, EPCGMetadataTypes NewType, const FString& SourceActorName)
		{
			if (const EPCGMetadataTypes* Existing = TypeMap.Find(Name))
			{
				if (*Existing != NewType)
				{
					UE_LOG(LogPCGEx, Warning,
					       TEXT("Value tag type conflict: '%s' on actor '%s'. Attribute was already registered with a different type; this actor's value will be discarded."),
					       *Name.ToString(), *SourceActorName);
					return false;
				}
				return true;
			}
			TypeMap.Add(Name, NewType);
			return true;
		}
	};

	// Per-actor parsed tag result.
	// PlainTags: tags with no colon (or unrecognized format) → become bool=true attributes.
	// ValueTags: Name:Value tags → become typed attributes.
	// Under ParseAndKeep, the name-part of each ValueTag is also written to the instance tag string
	// (handled at the call site by iterating both arrays).
	struct FParsedActorTags
	{
		TArray<FName> PlainTags;
		TArray<TPair<FName, TSharedPtr<PCGExData::IDataValue>>> ValueTags;
	};

	static FParsedActorTags ParseActorTags(const AActor* Actor, FValueTagRegistry& Registry)
	{
		FParsedActorTags Result;
		const FString ActorName = Actor->GetActorNameOrLabel();

		for (const FName& Tag : Actor->Tags)
		{
			FString Key;
			const TSharedPtr<PCGExData::IDataValue> DataValue = PCGExData::TryGetValueFromTag(Tag.ToString(), Key);

			if (DataValue.IsValid())
			{
				const FName AttrName(Key);
				if (Registry.Register(AttrName, DataValue->GetTypeId(), ActorName))
				{
					Result.ValueTags.Add(TPair<FName, TSharedPtr<PCGExData::IDataValue>>(AttrName, DataValue));
				}
				// Note: name-parts of value tags are intentionally NOT added to PlainTags.
				// They appear in the instance tag string only via the ValueTags array (ParseAndKeep).
			}
			else
			{
				// Plain tag → will become a bool=true attribute
				if (Registry.Register(Tag, EPCGMetadataTypes::Boolean, ActorName))
				{
					Result.PlainTags.Add(Tag);
				}
			}
		}
		return Result;
	}

	// Creates one typed PCG metadata attribute per registry entry; returns a map of base ptrs for fast per-point writes.
	static TMap<FName, FPCGMetadataAttributeBase*> CreateValueTagAttributes(UPCGMetadata* Meta, const FValueTagRegistry& Registry)
	{
		TMap<FName, FPCGMetadataAttributeBase*> AttrMap;
		AttrMap.Reserve(Registry.TypeMap.Num());

		for (const TPair<FName, EPCGMetadataTypes>& Elem : Registry.TypeMap)
		{
			const FName& Name = Elem.Key;
			FPCGMetadataAttributeBase* Attr = nullptr;
#define PCGEX_CREATE_VALUE_TAG_ATTR(_TYPE, _NAME) Attr = Meta->CreateAttribute<_TYPE>(Name, _TYPE{}, false, true);
			PCGEX_EXECUTEWITHRIGHTTYPE(Elem.Value, PCGEX_CREATE_VALUE_TAG_ATTR)
#undef PCGEX_CREATE_VALUE_TAG_ATTR
			if (Attr) { AttrMap.Add(Name, Attr); }
		}
		return AttrMap;
	}

	// Writes value-tag attributes for a single point.
	// PlainTags → bool=true; ValueTags → typed value via base-ptr cast.
	static void SetValueTagAttributes(
		const TMap<FName, FPCGMetadataAttributeBase*>& AttrMap,
		int64 Entry,
		const FParsedActorTags& Parsed)
	{
		for (const FName& Tag : Parsed.PlainTags)
		{
			if (FPCGMetadataAttributeBase* const* BasePtr = AttrMap.Find(Tag))
			{
				static_cast<FPCGMetadataAttribute<bool>*>(*BasePtr)->SetValue(Entry, true);
			}
		}

		for (const TPair<FName, TSharedPtr<PCGExData::IDataValue>>& VT : Parsed.ValueTags)
		{
			FPCGMetadataAttributeBase* const* BasePtr = AttrMap.Find(VT.Key);
			if (!BasePtr) { continue; }

			FPCGMetadataAttributeBase* Base = *BasePtr;
			const TSharedPtr<PCGExData::IDataValue>& Val = VT.Value;

#define PCGEX_SET_VALUE_TAG_ATTR(_TYPE, _NAME) static_cast<FPCGMetadataAttribute<_TYPE>*>(Base)->SetValue(Entry, Val->GetValue<_TYPE>());
			PCGEX_EXECUTEWITHRIGHTTYPE(Val->GetTypeId(), PCGEX_SET_VALUE_TAG_ATTR)
#undef PCGEX_SET_VALUE_TAG_ATTR
		}
	}

	// Creates value-tag attributes from Registry, then writes one row per parsed actor.
	// Parsed and Entries are parallel arrays; Entries[i] receives the attributes from Parsed[i].
	static void EmitValueTagAttributes(
		UPCGMetadata* Meta,
		const FValueTagRegistry& Registry,
		TConstArrayView<FParsedActorTags> Parsed,
		const TPCGValueRange<int64>& MetaEntries)
	{
		if (Parsed.IsEmpty() || Registry.TypeMap.IsEmpty()) { return; }
		const TMap<FName, FPCGMetadataAttributeBase*> AttrMap = CreateValueTagAttributes(Meta, Registry);
		if (AttrMap.IsEmpty()) { return; }
		for (int32 i = 0; i < Parsed.Num(); ++i)
		{
			SetValueTagAttributes(AttrMap, MetaEntries[i], Parsed[i]);
		}
	}
}

bool UPCGExDefaultLevelDataExporter::ExportLevelData_Implementation(UWorld* World, UPCGDataAsset* OutAsset)
{
	if (!World || !OutAsset) { return false; }

	ULevel* PersistentLevel = World->PersistentLevel;
	if (!PersistentLevel) { return false; }

	// Move any previous inner subobjects to the transient package so they get GC'd
	// instead of being saved as orphan exports in the collection's .uasset. Otherwise,
	// each rebuild accumulates dead sub-collections/point-data that can confuse save-time
	// pointer traversal (observed as INT_MAX-pointer crashes during level save).
	{
		TArray<UObject*> OldInners;
		GetObjectsWithOuter(OutAsset, OldInners, false);
		for (UObject* Inner : OldInners)
		{
			Inner->Rename(nullptr, GetTransientPackage(),
			              REN_DontCreateRedirectors | REN_NonTransactional);
		}
	}

	using namespace PCGExDefaultLevelDataExporterInternal;

	// Phase 1: Collect and classify qualifying actors
	TArray<FClassifiedActor> ClassifiedActors;
	for (AActor* Actor : PersistentLevel->Actors)
	{
		if (!UPCGExActorContentFilter::StaticPassesFilter(ContentFilter, Actor)) { continue; }

		FClassifiedActor Classified;
		Classified.Actor = Actor;
		Classified.Type = ClassifyActor(Actor, Classified.MeshComponent);

		if (Classified.Type != EPCGExActorExportType::Skip)
		{
			ClassifiedActors.Add(Classified);
		}
	}

	if (ClassifiedActors.IsEmpty()) { return false; }

	// Separate by type
	TArray<FClassifiedActor> MeshActors;
	TArray<FClassifiedActor> ActorActors;
	TArray<FClassifiedActor> LevelActors;

	for (const FClassifiedActor& CA : ClassifiedActors)
	{
		if (CA.Type == EPCGExActorExportType::Mesh) { MeshActors.Add(CA); }
		else if (CA.Type == EPCGExActorExportType::Actor) { ActorActors.Add(CA); }
		else if (CA.Type == EPCGExActorExportType::Level) { LevelActors.Add(CA); }
	}

	// Group level instances by their referenced UWorld asset path.
	TMap<FSoftObjectPath, FLevelInfo> LevelInfoMap;
	for (const FClassifiedActor& CA : LevelActors)
	{
		const ALevelInstance* LI = Cast<ALevelInstance>(CA.Actor);
		if (!LI) { continue; }
		const FSoftObjectPath LevelPath = LI->GetWorldAsset().ToSoftObjectPath();
		if (LevelPath.IsNull()) { continue; }
		LevelInfoMap.FindOrAdd(LevelPath).Count++;
	}

	// Parse actor value tags before the intersection loop so PlainTags/ValueTags are available.
	// ActorParsedTags is parallel to ActorActors (same index); empty when NoParsing.
	FValueTagRegistry ActorValueRegistry;
	TArray<FParsedActorTags> ActorParsedTags;

	if (ValueTagMode != EPCGExValueTagMode::NoParsing && !ActorActors.IsEmpty())
	{
		ActorParsedTags.Reserve(ActorActors.Num());
		for (const FClassifiedActor& CA : ActorActors)
		{
			ActorParsedTags.Add(ParseActorTags(CA.Actor, ActorValueRegistry));
		}
	}

	// Compute actor tag intersections and property deltas for collection building
	TMap<FActorInstanceKey, FActorClassInfo> ActorClassInfoMap;
	for (int32 i = 0; i < ActorActors.Num(); i++)
	{
		FClassifiedActor& CA = ActorActors[i];

		TArray<uint8> DeltaBytes;
		uint32 DeltaHash = 0;
		if (bCapturePropertyDeltas && bGenerateCollections)
		{
			DeltaBytes = PCGExActorDelta::SerializeActorDelta(CA.Actor);
			DeltaHash = PCGExActorDelta::HashDelta(DeltaBytes);
		}
		CA.DeltaHash = DeltaHash;

		FActorInstanceKey Key;
		Key.ClassPath = FSoftClassPath(CA.Actor->GetClass());
		Key.DeltaHash = DeltaHash;

		FActorClassInfo& Info = ActorClassInfoMap.FindOrAdd(Key);
		Info.Count++;

		// Build the effective tag set for intersection based on the parsing mode.
		// Raw Actor->Tags are still used for property deltas (kept as-is).
		auto BuildEffectiveTags = [&]() -> TSet<FName>
		{
			TSet<FName> Tags;
			if (ActorParsedTags.IsValidIndex(i))
			{
				const FParsedActorTags& Parsed = ActorParsedTags[i];
				for (const FName& Tag : Parsed.PlainTags) { Tags.Add(Tag); }
				if (ValueTagMode == EPCGExValueTagMode::ParseAndKeep)
				{
					for (const TPair<FName, TSharedPtr<PCGExData::IDataValue>>& VT : Parsed.ValueTags) { Tags.Add(VT.Key); }
				}
			}
			else
			{
				for (const FName& Tag : CA.Actor->Tags) { Tags.Add(Tag); }
			}
			return Tags;
		};

		if (Info.bFirstActor)
		{
			Info.bFirstActor = false;
			if (!DeltaBytes.IsEmpty()) { Info.SerializedDelta = MoveTemp(DeltaBytes); }
			Info.IntersectedTags = BuildEffectiveTags();
		}
		else
		{
			Info.IntersectedTags = Info.IntersectedTags.Intersect(BuildEffectiveTags());
		}
	}

	// Phase 2: Create typed point data

	// --- Unified Meshes (SM actors + ISM instances) ---
	TArray<FMeshPoint> AllMeshPoints;
	TMap<FSoftObjectPath, FMeshInfo> MeshInfoMap;

	// Collect from SM actors
	for (const FClassifiedActor& CA : MeshActors)
	{
		if (!CA.MeshComponent) { continue; }
		UStaticMesh* Mesh = CA.MeshComponent->GetStaticMesh();
		if (!Mesh) { continue; }

		const FSoftObjectPath MeshPath(Mesh);
		FMeshInfo& Info = MeshInfoMap.FindOrAdd(MeshPath);
		Info.TotalCount++;
		if (!Info.FirstComponent) { Info.FirstComponent = CA.MeshComponent; }

		FMeshPoint& Point = AllMeshPoints.AddDefaulted_GetRef();
		Point.MeshPath = MeshPath;
		Point.SourceComponent = CA.MeshComponent;
		Point.SourceActor = CA.Actor;

		const FTransform ActorTransform = CA.Actor->GetActorTransform();
		Point.Transform = ActorTransform;

		const FBox WorldBounds = BoundsEvaluator ? BoundsEvaluator->EvaluateActorBounds(CA.Actor, nullptr, -1) : FBox(ForceInit);
		WorldBoundsToLocal(WorldBounds, ActorTransform, Point.BoundsMin, Point.BoundsMax);

		if (bCaptureMaterialOverrides)
		{
			Point.MaterialVariantIndex = TrackMaterialVariant(CA.MeshComponent, Info);
		}
	}

	// Collect from ISM instances on all classified actors
	for (const FClassifiedActor& CA : ClassifiedActors)
	{
		TArray<UInstancedStaticMeshComponent*> ISMComponents;
		CA.Actor->GetComponents<UInstancedStaticMeshComponent>(ISMComponents);

		for (const UInstancedStaticMeshComponent* ISM : ISMComponents)
		{
			if (!ISM || ISM->GetInstanceCount() == 0) { continue; }

			UStaticMesh* Mesh = ISM->GetStaticMesh();
			if (!Mesh) { continue; }

			const FSoftObjectPath MeshPath(Mesh);
			const int32 InstanceCount = ISM->GetInstanceCount();

			FMeshInfo& Info = MeshInfoMap.FindOrAdd(MeshPath);
			Info.TotalCount += InstanceCount;
			if (!Info.FirstComponent) { Info.FirstComponent = ISM; }

			const FBox MeshBounds = Mesh->GetBoundingBox();

			int32 VariantIdx = -1;
			if (bCaptureMaterialOverrides)
			{
				VariantIdx = TrackMaterialVariant(ISM, Info);
			}

			for (int32 Idx = 0; Idx < InstanceCount; Idx++)
			{
				FMeshPoint& Point = AllMeshPoints.AddDefaulted_GetRef();
				Point.MeshPath = MeshPath;
				Point.SourceComponent = ISM;
				Point.SourceActor = CA.Actor;
				Point.MaterialVariantIndex = VariantIdx;

				ISM->GetInstanceTransform(Idx, Point.Transform, true);
				Point.BoundsMin = MeshBounds.Min;
				Point.BoundsMax = MeshBounds.Max;
			}
		}
	}

	// Create mesh point data
	UPCGBasePointData* MeshPointData = nullptr;
	if (!AllMeshPoints.IsEmpty())
	{
		// Parse value tags for each unique source actor (ISM actors may share the same actor).
		FValueTagRegistry MeshValueRegistry;
		TMap<AActor*, int32> MeshActorParsedTagIdx;
		TArray<FParsedActorTags> MeshActorParsedTagsList;

		if (ValueTagMode != EPCGExValueTagMode::NoParsing)
		{
			for (const FMeshPoint& Point : AllMeshPoints)
			{
				if (Point.SourceActor && !MeshActorParsedTagIdx.Contains(Point.SourceActor))
				{
					const int32 Idx = MeshActorParsedTagsList.Num();
					MeshActorParsedTagIdx.Add(Point.SourceActor, Idx);
					MeshActorParsedTagsList.Add(ParseActorTags(Point.SourceActor, MeshValueRegistry));
				}
			}
		}

		TPCGValueRange<FTransform> Transforms;
		TPCGValueRange<FVector> BMin, BMax;
		MeshPointData = CreatePointData(OutAsset, AllMeshPoints.Num(), Transforms, BMin, BMax);

		for (int32 i = 0; i < AllMeshPoints.Num(); i++)
		{
			const FMeshPoint& Point = AllMeshPoints[i];
			Transforms[i] = Point.Transform;
			BMin[i] = Point.BoundsMin;
			BMax[i] = Point.BoundsMax;
		}

		InitMetadata(MeshPointData, AllMeshPoints.Num());

		UPCGMetadata* Meta = MeshPointData->MutableMetadata();
		TPCGValueRange<int64> MetaEntries = MeshPointData->GetMetadataEntryValueRange();

		TMap<FName, FPCGMetadataAttributeBase*> MeshValueTagAttrMap;
		if (!MeshActorParsedTagsList.IsEmpty())
		{
			MeshValueTagAttrMap = CreateValueTagAttributes(Meta, MeshValueRegistry);
		}

		FPCGMetadataAttribute<FString>* ActorNameAttr = Meta->CreateAttribute<FString>(TEXT("ActorName"), FString(), false, true);

		if (!bGenerateCollections)
		{
			FPCGMetadataAttribute<FSoftObjectPath>* MeshAttr = Meta->CreateAttribute<FSoftObjectPath>(TEXT("Mesh"), FSoftObjectPath(), false, true);

			for (int32 i = 0; i < AllMeshPoints.Num(); i++)
			{
				const int64 Entry = MetaEntries[i];
				if (ActorNameAttr) { ActorNameAttr->SetValue(Entry, AllMeshPoints[i].SourceActor->GetActorNameOrLabel()); }
				if (MeshAttr) { MeshAttr->SetValue(Entry, AllMeshPoints[i].MeshPath); }
			}
		}
		else
		{
			for (int32 i = 0; i < AllMeshPoints.Num(); i++)
			{
				if (ActorNameAttr) { ActorNameAttr->SetValue(MetaEntries[i], AllMeshPoints[i].SourceActor->GetActorNameOrLabel()); }
			}
		}

		if (!MeshValueTagAttrMap.IsEmpty())
		{
			for (int32 i = 0; i < AllMeshPoints.Num(); i++)
			{
				if (const int32* PIdx = MeshActorParsedTagIdx.Find(AllMeshPoints[i].SourceActor))
				{
					SetValueTagAttributes(MeshValueTagAttrMap, MetaEntries[i], MeshActorParsedTagsList[*PIdx]);
				}
			}
		}

		FPCGTaggedData& TaggedData = OutAsset->Data.TaggedData.Emplace_GetRef();
		TaggedData.Data = MeshPointData;
		TaggedData.Pin = FName(TEXT("Meshes"));
	}

	// --- Actors ---
	UPCGBasePointData* ActorPointData = nullptr;
	if (!ActorActors.IsEmpty())
	{
		TPCGValueRange<FTransform> Transforms;
		TPCGValueRange<FVector> BMin, BMax;
		ActorPointData = CreatePointData(OutAsset, ActorActors.Num(), Transforms, BMin, BMax);

		for (int32 i = 0; i < ActorActors.Num(); i++)
		{
			WriteActorTransformAndBounds(ActorActors[i].Actor, i, BoundsEvaluator, Transforms, BMin, BMax);
		}

		InitMetadata(ActorPointData, ActorActors.Num());

		UPCGMetadata* Meta = ActorPointData->MutableMetadata();
		TPCGValueRange<int64> MetaEntries = ActorPointData->GetMetadataEntryValueRange();

		FPCGMetadataAttribute<FString>* ActorNameAttr = Meta->CreateAttribute<FString>(TEXT("ActorName"), FString(), false, true);

		if (!bGenerateCollections)
		{
			FPCGMetadataAttribute<FSoftClassPath>* ActorClassAttr = Meta->CreateAttribute<FSoftClassPath>(TEXT("ActorClass"), FSoftClassPath(), false, true);

			for (int32 i = 0; i < ActorActors.Num(); i++)
			{
				const int64 Entry = MetaEntries[i];
				if (ActorNameAttr) { ActorNameAttr->SetValue(Entry, ActorActors[i].Actor->GetActorNameOrLabel()); }
				if (ActorClassAttr) { ActorClassAttr->SetValue(Entry, FSoftClassPath(ActorActors[i].Actor->GetClass())); }
			}
		}
		else
		{
			for (int32 i = 0; i < ActorActors.Num(); i++)
			{
				if (ActorNameAttr) { ActorNameAttr->SetValue(MetaEntries[i], ActorActors[i].Actor->GetActorNameOrLabel()); }
			}
		}

		EmitValueTagAttributes(Meta, ActorValueRegistry, ActorParsedTags, MetaEntries);

		// Write per-actor tag names as a joined string. Only meaningful in NoParsing and ParseAndKeep
		// modes; in Parse mode all tags are already written as typed attributes above.
		if (bWriteInstanceTags && InstanceTagsAttributeName != NAME_None && ValueTagMode != EPCGExValueTagMode::Parse)
		{
			if (FPCGMetadataAttribute<FString>* InstanceTagsAttr = Meta->CreateAttribute<FString>(InstanceTagsAttributeName, FString(), false, true))
			{
				for (int32 i = 0; i < ActorActors.Num(); i++)
				{
					FString TagsStr;

					if (ValueTagMode == EPCGExValueTagMode::NoParsing)
					{
						for (const FName& Tag : ActorActors[i].Actor->Tags)
						{
							if (!TagsStr.IsEmpty()) { TagsStr += TEXT(","); }
							TagsStr += Tag.ToString();
						}
					}
					else if (ActorParsedTags.IsValidIndex(i)) // ParseAndKeep: plain tag names + value-tag name-parts
					{
						const FParsedActorTags& Parsed = ActorParsedTags[i];
						for (const FName& Tag : Parsed.PlainTags)
						{
							if (!TagsStr.IsEmpty()) { TagsStr += TEXT(","); }
							TagsStr += Tag.ToString();
						}
						for (const TPair<FName, TSharedPtr<PCGExData::IDataValue>>& VT : Parsed.ValueTags)
						{
							if (!TagsStr.IsEmpty()) { TagsStr += TEXT(","); }
							TagsStr += VT.Key.ToString();
						}
					}

					if (!TagsStr.IsEmpty())
					{
						InstanceTagsAttr->SetValue(MetaEntries[i], TagsStr);
					}
				}
			}
		}

		FPCGTaggedData& TaggedData = OutAsset->Data.TaggedData.Emplace_GetRef();
		TaggedData.Data = ActorPointData;
		TaggedData.Pin = FName(TEXT("Actors"));
	}

	// --- Levels (nested level instances) ---
	UPCGBasePointData* LevelPointData = nullptr;
	if (!LevelActors.IsEmpty())
	{
		FValueTagRegistry LevelValueRegistry;
		TArray<FParsedActorTags> LevelParsedTags;

		if (ValueTagMode != EPCGExValueTagMode::NoParsing)
		{
			LevelParsedTags.Reserve(LevelActors.Num());
			for (const FClassifiedActor& CA : LevelActors)
			{
				LevelParsedTags.Add(ParseActorTags(CA.Actor, LevelValueRegistry));
			}
		}

		TPCGValueRange<FTransform> Transforms;
		TPCGValueRange<FVector> BMin, BMax;
		LevelPointData = CreatePointData(OutAsset, LevelActors.Num(), Transforms, BMin, BMax);

		for (int32 i = 0; i < LevelActors.Num(); i++)
		{
			WriteActorTransformAndBounds(LevelActors[i].Actor, i, BoundsEvaluator, Transforms, BMin, BMax);
		}

		InitMetadata(LevelPointData, LevelActors.Num());

		UPCGMetadata* Meta = LevelPointData->MutableMetadata();
		TPCGValueRange<int64> MetaEntries = LevelPointData->GetMetadataEntryValueRange();

		FPCGMetadataAttribute<FString>* ActorNameAttr = Meta->CreateAttribute<FString>(TEXT("ActorName"), FString(), false, true);

		if (!bGenerateCollections)
		{
			FPCGMetadataAttribute<FSoftObjectPath>* LevelAssetAttr = Meta->CreateAttribute<FSoftObjectPath>(TEXT("LevelAsset"), FSoftObjectPath(), false, true);

			for (int32 i = 0; i < LevelActors.Num(); i++)
			{
				const int64 Entry = MetaEntries[i];
				if (ActorNameAttr) { ActorNameAttr->SetValue(Entry, LevelActors[i].Actor->GetActorNameOrLabel()); }
				if (LevelAssetAttr)
				{
					if (const ALevelInstance* LI = Cast<ALevelInstance>(LevelActors[i].Actor))
					{
						LevelAssetAttr->SetValue(Entry, LI->GetWorldAsset().ToSoftObjectPath());
					}
				}
			}
		}
		else
		{
			for (int32 i = 0; i < LevelActors.Num(); i++)
			{
				if (ActorNameAttr) { ActorNameAttr->SetValue(MetaEntries[i], LevelActors[i].Actor->GetActorNameOrLabel()); }
			}
		}

		EmitValueTagAttributes(Meta, LevelValueRegistry, LevelParsedTags, MetaEntries);

		FPCGTaggedData& TaggedData = OutAsset->Data.TaggedData.Emplace_GetRef();
		TaggedData.Data = LevelPointData;
		TaggedData.Pin = FName(TEXT("Levels"));
	}

	// Phase 2.5: Notify subclasses
	OnExportComplete(OutAsset);

	// Phase 3: Embedded collection generation (when bGenerateCollections)
	if (bGenerateCollections)
	{
		// Build embedded mesh collection
		UPCGExMeshCollection* EmbeddedMeshCollection = nullptr;

		if (!MeshInfoMap.IsEmpty())
		{
			EmbeddedMeshCollection = NewObject<UPCGExMeshCollection>(OutAsset);
			EmbeddedMeshCollection->InitNumEntries(MeshInfoMap.Num());

			int32 MeshIdx = 0;
			for (auto& Elem : MeshInfoMap)
			{
				Elem.Value.EntryIndex = MeshIdx;

				FPCGExMeshCollectionEntry& MeshEntry = EmbeddedMeshCollection->Entries[MeshIdx];
				MeshEntry.StaticMesh = TSoftObjectPtr<UStaticMesh>(Elem.Key);
				MeshEntry.Weight = Elem.Value.TotalCount;

				// Populate ISM/SM descriptors from first source component
				if (Elem.Value.FirstComponent)
				{
					MeshEntry.ISMDescriptor.InitFrom(Elem.Value.FirstComponent, false);
					MeshEntry.SMDescriptor.InitFrom(Elem.Value.FirstComponent, false);
				}

				// Material variants
				if (bCaptureMaterialOverrides && Elem.Value.UniqueVariantMaterials.Num() > 1)
				{
					MeshEntry.MaterialVariants = EPCGExMaterialVariantsMode::Multi;

					for (const TArray<FSoftObjectPath>& VariantMats : Elem.Value.UniqueVariantMaterials)
					{
						FPCGExMaterialOverrideCollection& Variant = MeshEntry.MaterialOverrideVariantsList.AddDefaulted_GetRef();
						Variant.Weight = 1;

						for (int32 SlotIdx = 0; SlotIdx < VariantMats.Num(); SlotIdx++)
						{
							FPCGExMaterialOverrideEntry& MatEntry = Variant.Overrides.AddDefaulted_GetRef();
							MatEntry.SlotIndex = SlotIdx;
							MatEntry.Material = TSoftObjectPtr<UMaterialInterface>(VariantMats[SlotIdx]);
						}
					}
				}

				MeshIdx++;
			}

			EmbeddedMeshCollection->RebuildStagingData(true);
		}

		// Build embedded actor collection
		UPCGExActorCollection* EmbeddedActorCollection = nullptr;

		if (!ActorClassInfoMap.IsEmpty())
		{
			EmbeddedActorCollection = NewObject<UPCGExActorCollection>(OutAsset);
			EmbeddedActorCollection->InitNumEntries(ActorClassInfoMap.Num());

			int32 ActorIdx = 0;
			for (auto& Elem : ActorClassInfoMap)
			{
				Elem.Value.EntryIndex = ActorIdx;

				FPCGExActorCollectionEntry& ActorEntry = EmbeddedActorCollection->Entries[ActorIdx];
				ActorEntry.Actor = TSoftClassPtr<AActor>(Elem.Key.ClassPath);
				ActorEntry.Weight = Elem.Value.Count;
				ActorEntry.Tags = Elem.Value.IntersectedTags;

				if (!Elem.Value.SerializedDelta.IsEmpty())
				{
					ActorEntry.SerializedPropertyDelta = Elem.Value.SerializedDelta;
				}

				ActorIdx++;
			}

			EmbeddedActorCollection->RebuildStagingData(true);
		}

		// Build embedded level collection (one entry per unique referenced UWorld asset).
		// Level instances have no per-instance overrides beyond transform, so the entry is
		// just the level reference + instance count -- no delta capture, no tag intersection.
		UPCGExLevelCollection* EmbeddedLevelCollection = nullptr;

		if (!LevelInfoMap.IsEmpty())
		{
			EmbeddedLevelCollection = NewObject<UPCGExLevelCollection>(OutAsset);
			EmbeddedLevelCollection->InitNumEntries(LevelInfoMap.Num());

			int32 LevelIdx = 0;
			for (auto& Elem : LevelInfoMap)
			{
				Elem.Value.EntryIndex = LevelIdx;

				FPCGExLevelCollectionEntry& LevelEntry = EmbeddedLevelCollection->Entries[LevelIdx];
				LevelEntry.Level = TSoftObjectPtr<UWorld>(Elem.Key);
				LevelEntry.Weight = Elem.Value.Count;

				LevelIdx++;
			}

			EmbeddedLevelCollection->RebuildStagingData(true);
		}

		// Encode hashes on points
		PCGExCollections::FPickPacker Packer;
		if (EmbeddedMeshCollection) { Packer.RegisterCollection(EmbeddedMeshCollection); }
		if (EmbeddedActorCollection) { Packer.RegisterCollection(EmbeddedActorCollection); }
		if (EmbeddedLevelCollection) { Packer.RegisterCollection(EmbeddedLevelCollection); }

		// Encode mesh hashes
		if (MeshPointData && EmbeddedMeshCollection)
		{
			UPCGMetadata* Meta = MeshPointData->MutableMetadata();
			TPCGValueRange<int64> MetaEntries = MeshPointData->GetMetadataEntryValueRange();

			FPCGMetadataAttribute<int64>* EntryHashAttr = Meta->CreateAttribute<int64>(
				PCGExCollections::Labels::Tag_EntryIdx, static_cast<int64>(0), false, true);

			if (EntryHashAttr)
			{
				for (int32 i = 0; i < AllMeshPoints.Num(); i++)
				{
					const FMeshPoint& Point = AllMeshPoints[i];
					const FMeshInfo* Info = MeshInfoMap.Find(Point.MeshPath);
					if (!Info) { continue; }

					const int16 SecIdx = (bCaptureMaterialOverrides && Point.MaterialVariantIndex > 0)
						                     ? static_cast<int16>(Point.MaterialVariantIndex)
						                     : static_cast<int16>(-1);

					const uint64 Hash = Packer.GetPickIdx(EmbeddedMeshCollection, static_cast<int16>(Info->EntryIndex), SecIdx);
					EntryHashAttr->SetValue(MetaEntries[i], static_cast<int64>(Hash));
				}
			}
		}

		// Encode actor hashes
		if (ActorPointData && EmbeddedActorCollection)
		{
			UPCGMetadata* Meta = ActorPointData->MutableMetadata();
			TPCGValueRange<int64> MetaEntries = ActorPointData->GetMetadataEntryValueRange();

			FPCGMetadataAttribute<int64>* EntryHashAttr = Meta->CreateAttribute<int64>(
				PCGExCollections::Labels::Tag_EntryIdx, static_cast<int64>(0), false, true);

			if (EntryHashAttr)
			{
				for (int32 i = 0; i < ActorActors.Num(); i++)
				{
					FActorInstanceKey Key;
					Key.ClassPath = FSoftClassPath(ActorActors[i].Actor->GetClass());
					Key.DeltaHash = ActorActors[i].DeltaHash;
					const FActorClassInfo* Info = ActorClassInfoMap.Find(Key);
					if (!Info) { continue; }

					const uint64 Hash = Packer.GetPickIdx(EmbeddedActorCollection, static_cast<int16>(Info->EntryIndex), -1);
					EntryHashAttr->SetValue(MetaEntries[i], static_cast<int64>(Hash));
				}
			}
		}

		// Encode level hashes
		if (LevelPointData && EmbeddedLevelCollection)
		{
			UPCGMetadata* Meta = LevelPointData->MutableMetadata();
			TPCGValueRange<int64> MetaEntries = LevelPointData->GetMetadataEntryValueRange();

			FPCGMetadataAttribute<int64>* EntryHashAttr = Meta->CreateAttribute<int64>(
				PCGExCollections::Labels::Tag_EntryIdx, static_cast<int64>(0), false, true);

			if (EntryHashAttr)
			{
				for (int32 i = 0; i < LevelActors.Num(); i++)
				{
					const ALevelInstance* LI = Cast<ALevelInstance>(LevelActors[i].Actor);
					if (!LI) { continue; }
					const FSoftObjectPath LevelPath = LI->GetWorldAsset().ToSoftObjectPath();
					const FLevelInfo* Info = LevelInfoMap.Find(LevelPath);
					if (!Info) { continue; }

					const uint64 Hash = Packer.GetPickIdx(EmbeddedLevelCollection, static_cast<int16>(Info->EntryIndex), -1);
					EntryHashAttr->SetValue(MetaEntries[i], static_cast<int64>(Hash));
				}
			}
		}

		// Embed collection map
		UPCGParamData* MapData = NewObject<UPCGParamData>(OutAsset);
		Packer.PackToDataset(MapData);

		FPCGTaggedData& MapTaggedData = OutAsset->Data.TaggedData.Emplace_GetRef();
		MapTaggedData.Data = MapData;
		MapTaggedData.Pin = FName(TEXT("CollectionMap"));
	}

	return OutAsset->Data.TaggedData.Num() > 0;
}
