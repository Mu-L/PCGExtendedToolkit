// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExActorPropertyDelta.h"

#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "ComponentReregisterContext.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
#include "PCGExLog.h"

namespace PCGExActorDelta
{
	// Wire format magic and version.
	// The delta bytes are persisted to disk in the collection's .uasset and must survive
	// editor restarts. Older formats without this header (or with older versions) are
	// silently skipped by ApplyPropertyDelta -- the user must rebuild the collection to
	// regenerate deltas in the current format.
	//
	// Version history (only v1 and v2 shipped to production; later designs went through
	// several iterations before settling on the current one):
	//   v1: magic header; inner delta via FObjectWriter + FStructuredArchiveFromArchive.
	//       BROKEN across sessions -- FNames encoded as session-local name-table indices,
	//       causing bogus names after editor restart and a crash at level save.
	//   v2: FNames serialized as strings in both the outer container and the inner delta.
	//       Session-portable, but tagged-property roundtrip through the structured-archive
	//       adapter silently dropped values on some components (bSplineHasBeenEdited never
	//       flipped on USplineComponent apply).
	//   v5 (current): per-property format with a trailing subobject section at every
	//       object level. Key properties of this design:
	//       - Outer-ownership detection (Inst->GetOuter() == Object) for subobjects
	//         instead of CPF_InstancedReference. UHT only sets that flag for the
	//         Instanced keyword / DefaultToInstanced; plugins using CreateDefaultSubobject
	//         or NewObject(this, ...) without the keyword still get per-instance lifetime
	//         but no flag. Runtime outer-ownership is the ground truth.
	//       - Relaxed filter for subobject contents (non-Edit UPROPERTY fields captured)
	//         since subobject state is typically internal, not user-facing.
	//       - Runtime component recreation: class path is stored alongside each component
	//         delta so apply can NewObject + AddInstanceComponent + SetupAttachment if
	//         the spawned target doesn't have a subobject by that name.
	//       - Dynamic components (archetype == class CDO) are captured/applied instead
	//         of being skipped.
	//       - operator<<(UObject*&) override on the archive intercepts Instanced refs
	//         nested in struct/TArray/TMap (gated to struct-inner positions only, so it
	//         never interferes with top-level property handling).
	//       - Cycle guard via TSet<const UObject*> threaded through the recursion.
	//
	// v3 and v4 were intermediate designs that never shipped; readers don't need to
	// handle them. v5 itself went through several internal iterations before landing on
	// the current shape.
	static constexpr uint32 DeltaWireMagic = 0x50434745u; // 'PCGE'
	static constexpr uint32 DeltaWireVersion = 5u;

	namespace Internal
	{
		/** Only user-editable instance properties at the TOP level of an actor/component.
		 *  Excludes engine bookkeeping (ActorGuid, tick state, net role, ...) that always
		 *  differs from the CDO but doesn't represent user intent. */
		static bool IsInstanceEditableProperty(const FProperty* InProperty)
		{
			return InProperty->HasAnyPropertyFlags(CPF_Edit)
				&& !InProperty->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance);
		}

		/** Root-component transform props are driven by the PCG point, never by the source
		 *  actor. Including them in the delta would overwrite the spawn transform with the
		 *  source actor's (often near-origin) values. */
		static bool IsTransformProperty(const FProperty* InProperty)
		{
			const FName PropName = InProperty->GetFName();
			static const FName NAME_RelativeLocation(TEXT("RelativeLocation"));
			static const FName NAME_RelativeRotation(TEXT("RelativeRotation"));
			static const FName NAME_RelativeScale3D(TEXT("RelativeScale3D"));
			return PropName == NAME_RelativeLocation
				|| PropName == NAME_RelativeRotation
				|| PropName == NAME_RelativeScale3D;
		}

		/** The Edit/transform filter only applies at the TOP LEVEL. It must NOT apply to
		 *  inner struct properties -- ShouldSkipProperty is invoked recursively by tagged
		 *  serialization when entering structs, and most struct-inner fields are plain
		 *  UPROPERTY() without CPF_Edit (e.g. FSplineCurves::Position). Filtering them out
		 *  strips struct contents during both serialize and deserialize. Properties owned
		 *  by a UClass are top-level; owned by a UScriptStruct (or nested container type)
		 *  are inner, and must pass through unfiltered. */
		static bool ShouldApplyClassLevelFilter(const FProperty* InProperty)
		{
			return InProperty && InProperty->GetOwner<UClass>() != nullptr;
		}

		/**
		 * Top-level property filter, branching on whether we're serializing an actor /
		 * component (bIsSubobject=false) or a subobject (true).
		 *
		 *  - Top level: user-editable, non-transform properties only. Engine bookkeeping and
		 *    transform fields would drown the delta in noise or fight the spawn transform.
		 *  - Subobject: any non-transient, non-deprecated UPROPERTY field. Subobject state is
		 *    typically internal (no details-panel exposure), so requiring CPF_Edit would drop
		 *    exactly the state we came to capture -- UVoxelSplineMetadata::GuidToValues and
		 *    similar plain-UPROPERTY TMap/TArray backing stores.
		 */
		static bool ShouldIncludeProperty(const FProperty* Property, bool bIsSubobject)
		{
			if (bIsSubobject)
			{
				return !Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated);
			}
			if (!IsInstanceEditableProperty(Property)) { return false; }
			if (IsTransformProperty(Property)) { return false; }
			return true;
		}

		// Forward declaration so the archive's operator<<(UObject*&) can call back for
		// nested-Instanced recursion.
		static void SerializeObjectDelta(
			UObject* Object, UObject* Defaults, TArray<uint8>& OutBytes,
			TSet<const UObject*>& VisitedSources, bool bIsSubobject);

		static void DeserializeObjectDelta(UObject* Object, const TArray<uint8>& InBytes);

		/**
		 * Memory archive subclasses layered over FObjectWriter/FObjectReader.
		 *
		 * ShouldSkipProperty override:
		 *   Edit-only + transform filter at the TOP LEVEL only (property owner is a UClass).
		 *   Struct/array/map inner properties pass through, so struct contents round-trip.
		 *
		 * FName override:
		 *   Serialize as string. The default FArchive FName path uses session-local name-
		 *   table indices, which aren't stable across editor restarts. Writing as string
		 *   makes the stream session-portable.
		 *
		 * operator<<(UObject*&) override (writer/reader):
		 *   Catches Instanced refs NESTED inside struct/TArray/TMap. The top-level
		 *   property loop already handles outer-owned children via the trailing subobject
		 *   section; but once FProperty::SerializeItem recurses into a struct/container,
		 *   any inner FObjectProperty writes the raw pointer via the base archive, and the
		 *   nested subobject's contents are lost. This override intercepts that position
		 *   for Instanced refs and inlines a recursive object-delta instead.
		 *
		 *   Gating:
		 *     - CPF_InstancedReference must be set on the inner property (explicit author
		 *       intent; avoids us mis-handling shared asset references).
		 *     - The property must not be owned by a UClass (i.e. we're inside a struct
		 *       or container element). Top-level refs are routed through the trailing
		 *       subobject section instead.
		 *
		 *   Payload (Instanced-nested position only):
		 *     [uint8  Present]         0 = null, 1 = subobject present
		 *     If Present:
		 *       [FString ClassPath]
		 *       [uint32  SubSize]
		 *       [SubSize bytes]        recursive object delta vs. the subobject's class CDO
		 *
		 *   Non-Instanced / non-nested refs fall through to the base raw-pointer write/read.
		 */
		class FDeltaWriter : public FObjectWriter
		{
		public:
			using FObjectWriter::FObjectWriter;

			/** Cycle guard shared across the capture tree. Nested operator<<(UObject*&)
			 *  recursion propagates this into the SerializeObjectDelta call so the same
			 *  source object never serializes twice. */
			TSet<const UObject*>* VisitedSources = nullptr;

			virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
			{
				if (ShouldApplyClassLevelFilter(InProperty))
				{
					if (!IsInstanceEditableProperty(InProperty)) { return true; }
					if (IsTransformProperty(InProperty)) { return true; }
				}
				return FObjectWriter::ShouldSkipProperty(InProperty);
			}

			virtual FArchive& operator<<(FName& Name) override
			{
				FString AsString = Name.ToString();
				*this << AsString;
				return *this;
			}

			virtual FArchive& operator<<(UObject*& Res) override;
		};

		class FDeltaReader : public FObjectReader
		{
		public:
			using FObjectReader::FObjectReader;

			virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
			{
				if (ShouldApplyClassLevelFilter(InProperty))
				{
					if (!IsInstanceEditableProperty(InProperty)) { return true; }
					if (IsTransformProperty(InProperty)) { return true; }
				}
				return FObjectReader::ShouldSkipProperty(InProperty);
			}

			virtual FArchive& operator<<(FName& Name) override
			{
				FString AsString;
				*this << AsString;
				Name = FName(*AsString);
				return *this;
			}

			virtual FArchive& operator<<(UObject*& Res) override;
		};

		/** A direct FObjectProperty field on Object whose value is outer-owned by Object --
		 *  i.e. a subobject the SerializeObjectDelta code should recurse into rather than
		 *  writing as a pointer. */
		struct FSubobjectBinding
		{
			FString PropName;
			UObject* Instance;
			UObject* Defaults;
		};

		/**
		 * Collect outer-owned subobjects reachable from Object via direct FObjectProperty
		 * fields.
		 *
		 * Outer-ownership (Inst->GetOuter() == Object) is the robust discriminator: both
		 * CreateDefaultSubobject and NewObject(this, ...) outer the child to the owner,
		 * while external references (asset pointers, other actors) have a different outer.
		 * We intentionally do NOT rely on CPF_InstancedReference -- UHT only sets it for
		 * UPROPERTY(Instanced) / DefaultToInstanced, which many plugins don't bother with
		 * even when their subobjects are genuinely per-instance-owned.
		 *
		 * We require Defaults to hold a parallel subobject of the same class so the
		 * recursive diff has a stable baseline. One-sided null and class mismatches are
		 * silently skipped -- there'd be no safe way to apply the delta.
		 *
		 * UActorComponent instances are explicitly skipped at the actor level: they're
		 * outered to the actor (both CDSO and SCS-created), so without this guard
		 * SerializeActorDelta would double-serialize each component -- once via its outer
		 * component loop, once by recursing into it as a subobject here.
		 */
		static void CollectSubobjects(UObject* Object, UObject* Defaults, TArray<FSubobjectBinding>& Out)
		{
			UClass* Class = Object->GetClass();
			for (FProperty* Property = Class->PropertyLink; Property; Property = Property->PropertyLinkNext)
			{
				FObjectProperty* ObjProp = CastField<FObjectProperty>(Property);
				if (!ObjProp) { continue; }
				if (ObjProp->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)) { continue; }

				UObject* Inst = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Object));
				UObject* Def = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Defaults));
				if (!Inst || !Def) { continue; }
				if (Inst->GetOuter() != Object) { continue; }
				if (Inst->GetClass() != Def->GetClass()) { continue; }

				// Components are handled by SerializeActorDelta's component loop. Recursing
				// into them here at the actor level would double-serialize their state.
				if (Inst->IsA<UActorComponent>()) { continue; }

				Out.Add({ ObjProp->GetName(), Inst, Def });
			}
		}

		/** Per-object inner format, wrapped by SerializeActorDelta's outer container:
		 *
		 *    [uint32 PropCount]
		 *    repeated PropCount times:
		 *      [FString PropName]      // FName as string (session-portable)
		 *      [uint32 ValueSize]
		 *      [ValueSize bytes]       // FProperty::SerializeItem output (may contain
		 *                              //   nested Instanced payloads produced by our
		 *                              //   operator<<(UObject*&) override; see FDeltaWriter)
		 *    [uint32 SubCount]
		 *    repeated SubCount times:
		 *      [FString PropName]      // name of the FObjectProperty holding the subobject
		 *      [uint32 SubSize]
		 *      [SubSize bytes]         // recursive SerializeObjectDelta output
		 *
		 *  Each segment is length-prefixed so unknown/renamed props or subobjects on apply
		 *  seek past their payload without corrupting subsequent reads. */
		static void SerializeObjectDelta(
			UObject* Object,
			UObject* Defaults,
			TArray<uint8>& OutBytes,
			TSet<const UObject*>& VisitedSources,
			bool bIsSubobject = false)
		{
			if (!Object || !Defaults) { return; }

			// Cycle guard. In practice outer-ownership graphs are trees (UE disallows cycles
			// in the outer chain), but operator<< recursion through non-outer-owned
			// Instanced refs could theoretically loop. Cheap safety.
			bool bAlreadyVisited = false;
			VisitedSources.Add(Object, &bAlreadyVisited);
			if (bAlreadyVisited) { return; }

			UClass* Class = Object->GetClass();

			TArray<FProperty*> PropsToWrite;
			for (FProperty* Property = Class->PropertyLink; Property; Property = Property->PropertyLinkNext)
			{
				if (!ShouldIncludeProperty(Property, bIsSubobject)) { continue; }

				// Outer-owned subobject refs at the top level are routed to the trailing
				// subobject section below. Serializing them at the property layer writes
				// the source's subobject path as a pointer, which either misresolves on
				// apply or cross-links to a foreign subobject -- and we'd also apply the
				// subobject delta, doubling state.
				if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
				{
					UObject* Inst = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Object));
					if (Inst && Inst->GetOuter() == Object) { continue; }
				}

				if (Property->Identical_InContainer(Object, Defaults)) { continue; }
				PropsToWrite.Add(Property);
			}

			TArray<FSubobjectBinding> Subobjects;
			CollectSubobjects(Object, Defaults, Subobjects);

			struct FSubDelta { FString PropName; TArray<uint8> Bytes; };
			TArray<FSubDelta> SubDeltas;
			for (const FSubobjectBinding& Sub : Subobjects)
			{
				TArray<uint8> SubBytes;
				SerializeObjectDelta(Sub.Instance, Sub.Defaults, SubBytes, VisitedSources, /*bIsSubobject=*/true);
				if (!SubBytes.IsEmpty())
				{
					SubDeltas.Add({ Sub.PropName, MoveTemp(SubBytes) });
				}
			}

			if (PropsToWrite.IsEmpty() && SubDeltas.IsEmpty()) { return; }

			FDeltaWriter Writer(OutBytes);
			Writer.VisitedSources = &VisitedSources;

			uint32 Count = PropsToWrite.Num();
			Writer << Count;

			for (FProperty* Property : PropsToWrite)
			{
				FString PropName = Property->GetName();
				Writer << PropName;

				// Write the value into its own buffer first so we can length-prefix it.
				TArray<uint8> ValueBytes;
				{
					FDeltaWriter ValueWriter(ValueBytes);
					// Inherit the outer writer's archive state so custom-versioned structs
					// (FSpline uses FFortniteMainBranchObjectVersion etc.) round-trip with
					// consistent versions, and share the visited set so nested
					// operator<<(UObject*&) recursion can be cycle-safe.
					ValueWriter.SetCustomVersions(Writer.GetCustomVersions());
					ValueWriter.VisitedSources = &VisitedSources;
					Property->SerializeItem(
						FStructuredArchiveFromArchive(ValueWriter).GetSlot(),
						Property->ContainerPtrToValuePtr<void>(Object),
						Property->ContainerPtrToValuePtr<void>(Defaults));
				}

				uint32 ValueSize = ValueBytes.Num();
				Writer << ValueSize;
				if (ValueSize > 0)
				{
					Writer.Serialize(ValueBytes.GetData(), ValueSize);
				}
			}

			uint32 SubCount = SubDeltas.Num();
			Writer << SubCount;
			for (FSubDelta& SD : SubDeltas)
			{
				Writer << SD.PropName;
				uint32 SubSize = SD.Bytes.Num();
				Writer << SubSize;
				if (SubSize > 0)
				{
					Writer.Serialize(SD.Bytes.GetData(), SubSize);
				}
			}
		}

		/** Public wrapper: allocate a fresh visited set and defer to the recursive impl. */
		static void SerializeObjectDelta(UObject* Object, UObject* Defaults, TArray<uint8>& OutBytes, bool bIsSubobject = false)
		{
			TSet<const UObject*> Visited;
			SerializeObjectDelta(Object, Defaults, OutBytes, Visited, bIsSubobject);
		}

		/** Read per-property segments, then descend into the subobject section. Missing /
		 *  renamed properties and mismatched subobjects seek past their payload instead of
		 *  corrupting the outer stream. */
		static void DeserializeObjectDelta(
			UObject* Object,
			const TArray<uint8>& InBytes)
		{
			if (!Object || InBytes.IsEmpty()) { return; }

			UClass* Class = Object->GetClass();
			FDeltaReader Reader(InBytes);
			const int64 TotalSize = InBytes.Num();

			uint32 Count = 0;
			Reader << Count;

			for (uint32 i = 0; i < Count; ++i)
			{
				if (Reader.IsError() || Reader.Tell() >= TotalSize) { break; }

				FString PropName;
				Reader << PropName;

				uint32 ValueSize = 0;
				Reader << ValueSize;

				if (Reader.Tell() + static_cast<int64>(ValueSize) > TotalSize)
				{
					Reader.SetError();
					break;
				}

				const int64 ValueStart = Reader.Tell();

				FProperty* Property = Class->FindPropertyByName(FName(*PropName));
				if (!Property
					|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)
					|| IsTransformProperty(Property))
				{
					Reader.Seek(ValueStart + ValueSize);
					continue;
				}

				// Extract the value segment into its own buffer so any over/under-read inside
				// the property handler stays contained and can't desync the outer stream.
				TArray<uint8> ValueBytes;
				ValueBytes.SetNumUninitialized(ValueSize);
				Reader.Serialize(ValueBytes.GetData(), ValueSize);

				FDeltaReader ValueReader(ValueBytes);
				ValueReader.SetCustomVersions(Reader.GetCustomVersions());
				Property->SerializeItem(
					FStructuredArchiveFromArchive(ValueReader).GetSlot(),
					Property->ContainerPtrToValuePtr<void>(Object),
					Property->ContainerPtrToValuePtr<void>(Class->GetDefaultObject()));
			}

			// Trailing subobject section. Truncated or absent is treated as empty -- the
			// outer wire-version gate has already rejected incompatible formats, so here a
			// missing subobject count means the writer produced zero subobjects.
			if (Reader.IsError() || Reader.Tell() + static_cast<int64>(sizeof(uint32)) > TotalSize) { return; }

			uint32 SubCount = 0;
			Reader << SubCount;

			for (uint32 i = 0; i < SubCount; ++i)
			{
				if (Reader.IsError() || Reader.Tell() >= TotalSize) { break; }

				FString PropName;
				Reader << PropName;

				uint32 SubSize = 0;
				Reader << SubSize;

				if (Reader.Tell() + static_cast<int64>(SubSize) > TotalSize)
				{
					Reader.SetError();
					break;
				}

				TArray<uint8> SubBytes;
				SubBytes.SetNumUninitialized(SubSize);
				Reader.Serialize(SubBytes.GetData(), SubSize);

				FProperty* Property = Class->FindPropertyByName(FName(*PropName));
				FObjectProperty* ObjProp = CastField<FObjectProperty>(Property);
				if (!ObjProp
					|| ObjProp->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated))
				{
					continue;
				}

				UObject* Subobj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Object));
				if (!Subobj) { continue; }
				// Symmetric outer-ownership guard: refuse to write into a foreign object.
				// The target's own archetype-spawned subobject is what the source delta was
				// captured against; anything else could pollute external state.
				if (Subobj->GetOuter() != Object) { continue; }

				DeserializeObjectDelta(Subobj, SubBytes);
			}
		}

		// ---- Out-of-line archive operator<< definitions ----

		FArchive& FDeltaWriter::operator<<(UObject*& Res)
		{
			const FProperty* CurProp = GetSerializedProperty();
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(CurProp);

			// Only intercept Instanced refs NESTED inside a struct/array/map (owner is not
			// a UClass). Top-level class-owned refs are routed through the subobject section
			// in SerializeObjectDelta; non-Instanced refs are raw pointers (fine for asset
			// references that resolve by path via the base archive).
			const bool bInstancedNested = ObjProp
				&& ObjProp->HasAnyPropertyFlags(CPF_InstancedReference)
				&& ObjProp->GetOwner<UClass>() == nullptr;

			if (!bInstancedNested)
			{
				return FObjectWriter::operator<<(Res);
			}

			uint8 Present = (Res != nullptr) ? 1u : 0u;
			*this << Present;
			if (!Present) { return *this; }

			FString ClassPath = Res->GetClass()->GetPathName();
			*this << ClassPath;

			TArray<uint8> SubBytes;
			{
				FDeltaWriter SubWriter(SubBytes);
				SubWriter.SetCustomVersions(GetCustomVersions());
				SubWriter.VisitedSources = VisitedSources;

				// Diff against the subobject's class CDO -- at this depth we don't have a
				// per-instance archetype handle. Target and source share the class, so
				// applying the delta onto target (which archetype-cloning already seeded
				// with the archetype baseline) reproduces source's state for fields source
				// customized past the CDO, and leaves archetype-level customizations intact
				// where source didn't touch them.
				UObject* CDO = Res->GetClass()->GetDefaultObject();
				if (VisitedSources)
				{
					SerializeObjectDelta(Res, CDO, SubBytes, *VisitedSources, /*bIsSubobject=*/true);
				}
				else
				{
					TSet<const UObject*> LocalVisited;
					SerializeObjectDelta(Res, CDO, SubBytes, LocalVisited, /*bIsSubobject=*/true);
				}
			}

			uint32 SubSize = SubBytes.Num();
			*this << SubSize;
			if (SubSize > 0) { Serialize(SubBytes.GetData(), SubSize); }
			return *this;
		}

		FArchive& FDeltaReader::operator<<(UObject*& Res)
		{
			const FProperty* CurProp = GetSerializedProperty();
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(CurProp);
			const bool bInstancedNested = ObjProp
				&& ObjProp->HasAnyPropertyFlags(CPF_InstancedReference)
				&& ObjProp->GetOwner<UClass>() == nullptr;

			if (!bInstancedNested)
			{
				return FObjectReader::operator<<(Res);
			}

			uint8 Present = 0;
			*this << Present;
			if (!Present) { return *this; }

			FString ClassPath;
			*this << ClassPath;

			uint32 SubSize = 0;
			*this << SubSize;

			TArray<uint8> SubBytes;
			if (SubSize > 0)
			{
				SubBytes.SetNumUninitialized(SubSize);
				Serialize(SubBytes.GetData(), SubSize);
			}

			// Apply ONTO the target's existing subobject (seeded by archetype-cloning at
			// NewObject time). Do NOT overwrite Res with the stale source-time pointer from
			// the stream -- that pointer is meaningless in the target's context.
			if (Res && SubSize > 0)
			{
				DeserializeObjectDelta(Res, SubBytes);
			}
			return *this;
		}
	}

	namespace FixupRegistry
	{
		struct FEntry
		{
			TWeakObjectPtr<UClass> ComponentClass;
			FPostApplyFixup Fixup;
		};

		// Registrations happen once at module startup, before any gameplay/PCG execution.
		// ApplyPropertyDelta is the only reader and runs single-threaded per actor.
		static TArray<FEntry>& Get()
		{
			static TArray<FEntry> Instance;
			return Instance;
		}

		static void RunAll(AActor* Actor)
		{
			TArray<FEntry>& Registry = Get();
			if (Registry.IsEmpty() || !Actor) { return; }

			TInlineComponentArray<UActorComponent*> Components;
			Actor->GetComponents(Components);

			for (UActorComponent* Component : Components)
			{
				if (!Component) { continue; }

				UClass* CompClass = Component->GetClass();
				UObject* Archetype = Component->GetArchetype();

				for (const FEntry& Entry : Registry)
				{
					UClass* TargetClass = Entry.ComponentClass.Get();
					if (!TargetClass || !CompClass->IsChildOf(TargetClass)) { continue; }
					Entry.Fixup(Component, Archetype);
				}
			}
		}
	}

	void RegisterPostApplyFixup(UClass* ComponentClass, FPostApplyFixup Fixup)
	{
		if (!ComponentClass || !Fixup) { return; }
		FixupRegistry::Get().Add({ComponentClass, MoveTemp(Fixup)});
	}

	void UnregisterPostApplyFixupsForClass(UClass* ComponentClass)
	{
		if (!ComponentClass) { return; }
		TArray<FixupRegistry::FEntry>& Registry = FixupRegistry::Get();
		Registry.RemoveAllSwap([ComponentClass](const FixupRegistry::FEntry& Entry)
		{
			return Entry.ComponentClass.Get() == ComponentClass;
		});
	}

	TArray<uint8> SerializeActorDelta(AActor* Actor)
	{
		if (!Actor) { return {}; }

		// Actor-level: diff instance against its CDO
		UClass* ActorClass = Actor->GetClass();
		UObject* ActorCDO = ActorClass->GetDefaultObject();

		TArray<uint8> ActorBytes;
		Internal::SerializeObjectDelta(Actor, ActorCDO, ActorBytes);

		// Collect component deltas
		struct FComponentDelta
		{
			FName Name;
			FString ClassPath;
			TArray<uint8> Bytes;
		};
		TArray<FComponentDelta> ComponentDeltas;

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		for (UActorComponent* Component : Components)
		{
			UObject* Archetype = Component->GetArchetype();
			if (!Archetype || Archetype == Component) { continue; }

			// Class mismatch = archetype from a different version/refactor; skip safely
			if (Component->GetClass() != Archetype->GetClass()) { continue; }

			// Components from CreateDefaultSubobject or Blueprint SCS have a per-actor
			// archetype on the actor CDO. Runtime-NewObject'd components (e.g. a
			// UVoxelSplineComponent attached to AVoxelStampActor at runtime) instead have
			// the raw class CDO as archetype -- no per-actor baseline. Earlier versions
			// skipped those; v6 diffs against the class CDO anyway so dynamic state
			// (spline points, metadata) survives, and records the class path so the apply
			// side can recreate the component if the spawned target doesn't include it.
			TArray<uint8> CompBytes;
			Internal::SerializeObjectDelta(Component, Archetype, CompBytes);

			if (!CompBytes.IsEmpty())
			{
				FComponentDelta CD;
				CD.Name = Component->GetFName();
				CD.ClassPath = FSoftClassPath(Component->GetClass()).ToString();
				CD.Bytes = MoveTemp(CompBytes);
				ComponentDeltas.Add(MoveTemp(CD));
			}
		}

		if (ActorBytes.IsEmpty() && ComponentDeltas.IsEmpty()) { return {}; }

		// Outer wire format:
		//   [uint32 Magic][uint32 Version]
		//   [uint32 ActorDeltaSize][ActorDelta...]
		//   [uint32 ComponentCount]
		//   For each: [FString Name][FString ClassPath][uint32 CompDeltaSize][CompDelta...]
		TArray<uint8> Result;
		FMemoryWriter Writer(Result);

		uint32 Magic = DeltaWireMagic;
		uint32 Version = DeltaWireVersion;
		Writer << Magic;
		Writer << Version;

		uint32 ActorSize = ActorBytes.Num();
		Writer << ActorSize;
		if (ActorSize > 0)
		{
			Writer.Serialize(ActorBytes.GetData(), ActorSize);
		}

		uint32 CompCount = ComponentDeltas.Num();
		Writer << CompCount;

		for (FComponentDelta& CD : ComponentDeltas)
		{
			// Names and class paths serialized as strings for session-portability. The
			// default FArchive FName path uses session-local name-table indices.
			FString CompNameStr = CD.Name.ToString();
			Writer << CompNameStr;
			Writer << CD.ClassPath;
			uint32 CompSize = CD.Bytes.Num();
			Writer << CompSize;
			Writer.Serialize(CD.Bytes.GetData(), CompSize);
		}

		return Result;
	}

	void ApplyPropertyDelta(AActor* Actor, const TArray<uint8>& DeltaBytes)
	{
		if (!Actor || DeltaBytes.IsEmpty()) { return; }

		FMemoryReader Reader(DeltaBytes);
		const int64 TotalSize = DeltaBytes.Num();

		// Validate magic+version. Old-format deltas would misalign under the current reader
		// and corrupt the target (FName reads returning bogus indices, crashing the editor
		// on later save). Silently skip unknown-format deltas; the user must rebuild the
		// collection to regenerate deltas in the current format.
		if (TotalSize < static_cast<int64>(sizeof(uint32) * 2)) { return; }

		uint32 Magic = 0;
		uint32 Version = 0;
		Reader << Magic;
		Reader << Version;
		if (Magic != DeltaWireMagic) { return; }
		if (Version != DeltaWireVersion)
		{
			UE_LOG(LogPCGEx, Warning,
				TEXT("[PCGExActorDelta] Skipping delta with incompatible wire version %u (expected %u). Rebuild the source collection to regenerate deltas."),
				Version, DeltaWireVersion);
			return;
		}

		// Actor-level delta
		uint32 ActorSize = 0;
		Reader << ActorSize;
		if (ActorSize > 0)
		{
			if (Reader.Tell() + ActorSize > TotalSize) { return; }

			TArray<uint8> ActorBytes;
			ActorBytes.SetNumUninitialized(ActorSize);
			Reader.Serialize(ActorBytes.GetData(), ActorSize);
			Internal::DeserializeObjectDelta(Actor, ActorBytes);
		}

		if (Reader.Tell() + static_cast<int64>(sizeof(uint32)) > TotalSize) { return; }

		uint32 CompCount = 0;
		Reader << CompCount;

		for (uint32 i = 0; i < CompCount; ++i)
		{
			if (Reader.Tell() >= TotalSize) { return; }

			FString CompNameStr;
			Reader << CompNameStr;
			const FName CompName(*CompNameStr);

			FString CompClassPathStr;
			Reader << CompClassPathStr;

			if (Reader.Tell() + static_cast<int64>(sizeof(uint32)) > TotalSize) { return; }

			uint32 CompSize = 0;
			Reader << CompSize;

			if (CompSize == 0) { continue; }
			if (Reader.Tell() + CompSize > TotalSize) { return; }

			TArray<uint8> CompBytes;
			CompBytes.SetNumUninitialized(CompSize);
			Reader.Serialize(CompBytes.GetData(), CompSize);

			UActorComponent* Component = FindObjectFast<UActorComponent>(Actor, CompName);

			// Dynamic-component path: if the source actor had a runtime-NewObject'd
			// component that the spawned target doesn't include, recreate it from the
			// captured class path so the delta bytes have somewhere to land. Inherit
			// transient flags from the actor so preview / runtime spawns don't
			// accidentally persist a subobject.
			if (!Component && !CompClassPathStr.IsEmpty())
			{
				const FSoftClassPath CompClassPath(CompClassPathStr);
				if (UClass* CompClass = CompClassPath.ResolveClass())
				{
					if (CompClass->IsChildOf(UActorComponent::StaticClass()))
					{
						EObjectFlags ObjectFlags = RF_Transactional;
						ObjectFlags |= Actor->GetFlags() & (RF_Transient | RF_NonPIEDuplicateTransient);

						Component = NewObject<UActorComponent>(Actor, CompClass, CompName, ObjectFlags);
						if (Component)
						{
							Actor->AddInstanceComponent(Component);
							// Scene components need an attach parent before registration.
							// Non-scene components skip this step.
							if (USceneComponent* SceneComp = Cast<USceneComponent>(Component))
							{
								if (USceneComponent* Root = Actor->GetRootComponent())
								{
									SceneComp->SetupAttachment(Root);
								}
							}
							// First-registration happens via the FComponentReregisterContext
							// scope below -- its destructor calls RegisterComponent.
						}
					}
				}
			}

			// Writing directly to a registered component leaves the render proxy pointing
			// at stale data (observed: splines rendering with no points after apply).
			// FComponentReregisterContext unregisters on construct, re-registers on scope
			// exit -- rebuilds the proxy from updated state. For newly-created components,
			// the construct-time unregister is a no-op and the destruct-time register
			// performs the component's first registration.
			if (Component)
			{
				FComponentReregisterContext ReregContext(Component);
				Internal::DeserializeObjectDelta(Component, CompBytes);
			}
		}

		// Repair engine-managed invariants that the per-property delta cannot express
		// (e.g. USplineComponent's Spline/SplineCurves aliasing in UE 5.7+).
		FixupRegistry::RunAll(Actor);
	}

	uint32 HashDelta(const TArray<uint8>& DeltaBytes)
	{
		if (DeltaBytes.IsEmpty()) { return 0; }
		return FCrc::MemCrc32(DeltaBytes.GetData(), DeltaBytes.Num());
	}
}
