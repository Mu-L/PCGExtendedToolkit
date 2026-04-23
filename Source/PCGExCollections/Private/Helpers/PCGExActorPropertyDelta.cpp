// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExActorPropertyDelta.h"

#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "ComponentReregisterContext.h"
#include "PCGExLog.h"

namespace PCGExActorDelta
{
	// Wire format magic and version.
	// The delta bytes are persisted to disk in the collection's .uasset and must survive
	// editor restarts. Older formats without this header (or with older versions) are
	// silently skipped by ApplyPropertyDelta -- the user must rebuild the collection to
	// regenerate deltas in the current format.
	//
	// Version history:
	//   v1: initial version with magic header; inner delta via FObjectWriter + structured
	//       adapter (BROKEN across sessions: FObjectWriter encoded FNames as session-local
	//       global name table indices, causing bogus names after editor restart and a crash
	//       at level save during name serialization).
	//   v2: FNames serialized as strings in both the outer wire format (component names) and
	//       inside the delta bytes (via FDeltaWriter::operator<<(FName&) override). Fully
	//       session-portable.
	static constexpr uint32 DeltaWireMagic = 0x50434745u; // 'PCGE'
	//   v3: replaced the per-object inner format. v1/v2 routed through UE's
	//       SerializeTaggedProperties via FStructuredArchiveFromArchive; writes produced
	//       large streams but reads on USplineComponent never actually applied the values
	//       to the target (bSplineHasBeenEdited never flipped). The replacement walks the
	//       object's top-level Edit properties ourselves, writes each value via
	//       FProperty::SerializeItem into a length-prefixed segment, and reads the same way.
	//   v4: added a per-segment kind byte and subobject recursion. v3 wrote Instanced UObject
	//       references (UPROPERTY(Instanced) TObjectPtr<UFoo>) as raw in-memory pointers via
	//       FObjectWriter's base << UObject*&, which doesn't persist meaningfully across
	//       invocations -- the subobject's contents were never captured. v4 detects
	//       CPF_InstancedReference properties on write and emits a recursive object-delta
	//       segment (kind=Subobject) instead of a raw pointer dump. On read, we walk into
	//       the target's existing subobject and apply the nested delta. Cycle safety via
	//       a visited set of source objects (UE subobject graphs are almost always trees,
	//       but the guard short-circuits anything pathological).
	static constexpr uint32 DeltaWireVersion = 4u;

	namespace Internal
	{
		/** Segment kind byte distinguishing a property value from a recursive subobject delta.
		 *  Forward-compatible: unknown kinds on read are skipped via the length prefix. */
		static constexpr uint8 kSegValue = 1;
		static constexpr uint8 kSegSubobject = 2;

		/**
		 * Only serialize properties that are user-editable on instances (EditAnywhere / EditInstanceOnly).
		 * This excludes engine bookkeeping (ActorGuid, tick state, net role, etc.) that always differs
		 * between instances and their CDO but doesn't represent user intent.
		 */
		static bool IsInstanceEditableProperty(const FProperty* InProperty)
		{
			return InProperty->HasAnyPropertyFlags(CPF_Edit)
				&& !InProperty->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance);
		}

		// FObjectWriter/FObjectReader are UObject-aware memory archives that handle
		// TObjectPtr, FSoftObjectPtr, etc. We subclass to filter via ShouldSkipProperty
		// so only user-editable properties are included in the delta.
		//
		// FName MUST be serialized as string, NOT as the default session-local index+number.
		// The delta bytes are persisted to the collection's .uasset and read back in a later
		// editor session. The global FName table is NOT persistent across sessions -- indices
		// assigned to names in session A refer to different names (or out-of-range entries)
		// in session B. Writing FNames as strings makes the stream session-portable.

		/** Transform-related root component properties that we NEVER want to capture in the delta.
		 *  The spawned actor's transform is determined by the PCG point, not by the source
		 *  actor's original position. Including these in the delta overwrites the spawn
		 *  transform with the source actor's (often near-origin) values on apply. */
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

		/** Edit/transform filtering should only apply at the TOP LEVEL (properties declared
		 *  on UObject-derived classes). It must NOT apply to inner struct properties --
		 *  ShouldSkipProperty is invoked recursively by tagged-property serialization when
		 *  entering structs, and most struct inner fields are plain UPROPERTY() without
		 *  CPF_Edit (e.g. FSplineCurves::Position/Rotation/Scale). Filtering them out strips
		 *  struct contents during both serialize and deserialize, so the struct tag reaches
		 *  the stream but its data is empty and the target keeps its archetype values.
		 *
		 *  Property owner distinguishes the two cases: class properties have a UClass owner,
		 *  struct inner properties have a UScriptStruct owner. Only apply the class-level
		 *  filter when the property is owned by a class. */
		static bool ShouldApplyClassLevelFilter(const FProperty* InProperty)
		{
			return InProperty && InProperty->GetOwner<UClass>() != nullptr;
		}

		class FDeltaWriter : public FObjectWriter
		{
		public:
			using FObjectWriter::FObjectWriter;

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
		};

		/** Quick check: does the object have any instance-editable property that differs from defaults? */
		static bool HasInstanceEditableDelta(UObject* Object, UObject* Defaults)
		{
			for (FProperty* Property = Object->GetClass()->PropertyLink; Property; Property = Property->PropertyLinkNext)
			{
				if (!IsInstanceEditableProperty(Property)) { continue; }
				if (!Property->Identical_InContainer(Object, Defaults)) { return true; }
			}
			return false;
		}

		/** Per-object serialization format (inner stream, wrapped by SerializeActorDelta's
		 *  outer container):
		 *
		 *    [uint32 Count]
		 *    repeated Count times:
		 *      [FString PropName]    // FName as string (session-portable)
		 *      [uint8  SegmentKind]  // kSegValue | kSegSubobject (unknown = skipped forward-compat)
		 *      [uint32 SegmentSize]
		 *      [SegmentSize bytes]
		 *
		 *  kSegValue bytes: FProperty::SerializeItem output for a normal property value.
		 *  kSegSubobject bytes: a recursive object delta (same format, applied to the target's
		 *    existing instanced subobject).
		 *
		 *  Length-prefixed segments let a missing/renamed/unknown-kind property on apply seek
		 *  past its payload without corrupting subsequent reads. */
		static void SerializeObjectDeltaImpl(
			UObject* Object,
			UObject* Defaults,
			TArray<uint8>& OutBytes,
			TSet<const UObject*>& VisitedSources);

		/** Detect UPROPERTY(Instanced) object refs -- these are per-instance-owned subobjects
		 *  whose contents must be captured via recursion, not by writing the raw pointer. */
		static bool IsInstancedSubobjectProperty(const FProperty* Property)
		{
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property);
			return ObjProp && ObjProp->HasAnyPropertyFlags(CPF_InstancedReference);
		}

		static void SerializeObjectDelta(
			UObject* Object,
			UObject* Defaults,
			TArray<uint8>& OutBytes)
		{
			TSet<const UObject*> Visited;
			SerializeObjectDeltaImpl(Object, Defaults, OutBytes, Visited);
		}

		static void SerializeObjectDeltaImpl(
			UObject* Object,
			UObject* Defaults,
			TArray<uint8>& OutBytes,
			TSet<const UObject*>& VisitedSources)
		{
			if (!Object || !Defaults) { return; }

			// Cycle guard. UE subobject graphs are trees in practice, but a pathological
			// UPROPERTY(Instanced) referring back up the chain would loop forever otherwise.
			// A source object visited once in this capture tree emits nothing on re-entry.
			bool bAlreadyVisited = false;
			VisitedSources.Add(Object, &bAlreadyVisited);
			if (bAlreadyVisited) { return; }

			if (!HasInstanceEditableDelta(Object, Defaults)) { return; }

			UClass* Class = Object->GetClass();

			struct FPendingSegment
			{
				FString PropName;
				uint8 Kind = kSegValue;
				TArray<uint8> Bytes;
			};
			TArray<FPendingSegment> Segments;

			// Use one writer only to inherit/propagate custom versions to inner writers.
			FDeltaWriter Writer(OutBytes);

			for (FProperty* Property = Class->PropertyLink; Property; Property = Property->PropertyLinkNext)
			{
				if (!IsInstanceEditableProperty(Property)) { continue; }
				if (IsTransformProperty(Property)) { continue; }

				const bool bInstanced = IsInstancedSubobjectProperty(Property);

				// Non-instanced: skip identical properties (standard delta). Instanced: always
				// recurse and let the inner call decide -- object-ref Identical compares
				// pointers, which always differ between per-instance subobjects even when
				// their contents match.
				if (!bInstanced && Property->Identical_InContainer(Object, Defaults)) { continue; }

				FPendingSegment Segment;
				Segment.PropName = Property->GetName();

				if (bInstanced)
				{
					// Instanced subobject: recurse on the owned object rather than dumping
					// the raw pointer (pointers don't persist across apply invocations).
					Segment.Kind = kSegSubobject;
					const FObjectPropertyBase* ObjProp = CastFieldChecked<FObjectPropertyBase>(Property);
					UObject* SrcSub = ObjProp->GetObjectPropertyValue_InContainer(Object);
					UObject* ArchSub = ObjProp->GetObjectPropertyValue_InContainer(Defaults);

					if (SrcSub && ArchSub && SrcSub->GetClass() == ArchSub->GetClass())
					{
						SerializeObjectDeltaImpl(SrcSub, ArchSub, Segment.Bytes, VisitedSources);
					}

					// Nothing to restore? Don't emit an empty segment -- keeps payloads tight
					// when the instanced subobject is identical to archetype in all its
					// editable fields.
					if (Segment.Bytes.IsEmpty()) { continue; }
				}
				else
				{
					Segment.Kind = kSegValue;
					FDeltaWriter ValueWriter(Segment.Bytes);
					// Propagate archive state so custom-versioned structs (FSpline uses
					// FFortniteMainBranchObjectVersion etc.) round-trip with consistent versions.
					ValueWriter.SetCustomVersions(Writer.GetCustomVersions());
					Property->SerializeItem(
						FStructuredArchiveFromArchive(ValueWriter).GetSlot(),
						Property->ContainerPtrToValuePtr<void>(Object),
						Property->ContainerPtrToValuePtr<void>(Defaults));
				}

				Segments.Add(MoveTemp(Segment));
			}

			if (Segments.IsEmpty())
			{
				// No effective differences. Reset so the caller sees an empty stream.
				OutBytes.Reset();
				return;
			}

			// Emit the segments now that we know the final count.
			uint32 Count = Segments.Num();
			Writer << Count;

			for (FPendingSegment& Segment : Segments)
			{
				Writer << Segment.PropName;
				Writer << Segment.Kind;
				uint32 SegmentSize = Segment.Bytes.Num();
				Writer << SegmentSize;
				if (SegmentSize > 0)
				{
					Writer.Serialize(Segment.Bytes.GetData(), SegmentSize);
				}
			}
		}

		/** Read per-property segments written by SerializeObjectDelta. Unknown properties,
		 *  type mismatches, and unrecognised segment kinds all seek past their payload so
		 *  subsequent reads stay aligned. */
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

				uint8 SegmentKind = 0;
				Reader << SegmentKind;

				uint32 SegmentSize = 0;
				Reader << SegmentSize;

				if (Reader.Tell() + static_cast<int64>(SegmentSize) > TotalSize)
				{
					Reader.SetError();
					break;
				}

				const int64 SegmentStart = Reader.Tell();

				FProperty* Property = Class->FindPropertyByName(FName(*PropName));
				const bool bValidProperty = Property && IsInstanceEditableProperty(Property) && !IsTransformProperty(Property);

				if (!bValidProperty)
				{
					// Property doesn't exist on target or isn't serializable; skip its payload.
					Reader.Seek(SegmentStart + SegmentSize);
					continue;
				}

				// Extract segment into a fresh buffer. Any under/over-read inside the property
				// handler is contained within this buffer and can't desync the outer stream.
				TArray<uint8> SegmentBytes;
				SegmentBytes.SetNumUninitialized(SegmentSize);
				Reader.Serialize(SegmentBytes.GetData(), SegmentSize);

				if (SegmentKind == kSegSubobject)
				{
					if (SegmentSize == 0) { continue; } // writer emitted empty (null/class mismatch)

					if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
					{
						if (UObject* TargetSub = ObjProp->GetObjectPropertyValue_InContainer(Object))
						{
							DeserializeObjectDelta(TargetSub, SegmentBytes);
						}
						// Target has no subobject to apply to: silently skip.
					}
					// Property isn't an object reference on the target class (schema changed):
					// skip. SegmentBytes already consumed above.
				}
				else if (SegmentKind == kSegValue)
				{
					FDeltaReader ValueReader(SegmentBytes);
					ValueReader.SetCustomVersions(Reader.GetCustomVersions());
					Property->SerializeItem(
						FStructuredArchiveFromArchive(ValueReader).GetSlot(),
						Property->ContainerPtrToValuePtr<void>(Object),
						Property->ContainerPtrToValuePtr<void>(Class->GetDefaultObject()));
				}
				// Unknown kind: payload was consumed above; forward-compat no-op.
			}
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
		// ApplyPropertyDelta is the only reader and is called single-threaded per actor.
		// No lock needed.
		static TArray<FEntry>& Get()
		{
			static TArray<FEntry> Instance;
			return Instance;
		}

		/** Run all registered fixups against each component on Actor. */
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
			TArray<uint8> Bytes;
		};
		TArray<FComponentDelta> ComponentDeltas;

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		for (UActorComponent* Component : Components)
		{
			UObject* Archetype = Component->GetArchetype();
			if (!Archetype || Archetype == Component) { continue; }

			// Components from CreateDefaultSubobject/Blueprint SCS have an archetype that
			// lives on the actor CDO -- these give a meaningful per-actor baseline to diff
			// against. Engine-managed components (scene root, etc.) have the raw class CDO
			// as archetype instead; skip those as they have no user-defined baseline.
			if (Archetype == Component->GetClass()->GetDefaultObject()) { continue; }

			// Class mismatch = archetype from a different version/refactor; skip safely
			if (Component->GetClass() != Archetype->GetClass()) { continue; }

			TArray<uint8> CompBytes;
			Internal::SerializeObjectDelta(Component, Archetype, CompBytes);

			if (!CompBytes.IsEmpty())
			{
				ComponentDeltas.Add({Component->GetFName(), MoveTemp(CompBytes)});
			}
		}

		// If nothing changed at all, return empty
		if (ActorBytes.IsEmpty() && ComponentDeltas.IsEmpty())
		{
			return {};
		}

		// Pack into wire format:
		//   [uint32 Magic][uint32 Version]
		//   [uint32 ActorDeltaSize][ActorDelta...]
		//   [uint32 ComponentCount]
		//   For each: [FName][uint32 CompDeltaSize][CompDelta...]
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
			// Component names serialized as strings for session-portability.
			// Default FArchive FName serialization uses the current session's global
			// name table indices, which are not stable across editor restarts.
			FString CompNameStr = CD.Name.ToString();
			Writer << CompNameStr;
			uint32 CompSize = CD.Bytes.Num();
			Writer << CompSize;
			Writer.Serialize(CD.Bytes.GetData(), CompSize);
		}

		return Result;
	}

	void ApplyPropertyDelta(AActor* Actor, const TArray<uint8>& DeltaBytes)
	{
		if (!Actor || DeltaBytes.IsEmpty()) { return; }

		// Unpack wire format written by SerializeActorDelta.
		// Bounds-check every read to handle corrupted/truncated data gracefully.
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
		if (Magic != DeltaWireMagic)
		{
			// No header / unknown format: silently ignore. This is a delta captured before
			// versioning was introduced, and applying it would corrupt the actor.
			return;
		}
		if (Version != DeltaWireVersion)
		{
			// Known format but incompatible version. Surface this so the user knows the
			// collection needs to be rebuilt to produce deltas in the current format.
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

		// Component deltas -- matched by subobject name
		if (Reader.Tell() + static_cast<int64>(sizeof(uint32)) > TotalSize) { return; }

		uint32 CompCount = 0;
		Reader << CompCount;

		for (uint32 i = 0; i < CompCount; ++i)
		{
			if (Reader.Tell() >= TotalSize) { return; }

			// Component name as string for session-portability (see writer).
			FString CompNameStr;
			Reader << CompNameStr;
			const FName CompName(*CompNameStr);

			if (Reader.Tell() + static_cast<int64>(sizeof(uint32)) > TotalSize) { return; }

			uint32 CompSize = 0;
			Reader << CompSize;

			if (CompSize == 0) { continue; }
			if (Reader.Tell() + CompSize > TotalSize) { return; }

			TArray<uint8> CompBytes;
			CompBytes.SetNumUninitialized(CompSize);
			Reader.Serialize(CompBytes.GetData(), CompSize);

			// Skip if target actor doesn't have a component with this name.
			// Writing directly to a registered component leaves the render proxy pointing at
			// stale data (observed: splines rendering with no points after apply). Wrap each
			// write in FComponentReregisterContext so the component is unregistered before
			// its properties are touched and re-registered on scope exit -- this rebuilds
			// the render proxy from the updated state.
			if (UActorComponent* Component = FindObjectFast<UActorComponent>(Actor, CompName))
			{
				FComponentReregisterContext ReregContext(Component);
				Internal::DeserializeObjectDelta(Component, CompBytes);
			}
		}

		// Repair engine-managed invariants that the per-property delta cannot express
		// (e.g. USplineComponent's Spline/SplineCurves aliasing in UE 5.7+). Runs on every
		// component so archetype-cloning inconsistencies are fixed too.
		FixupRegistry::RunAll(Actor);
	}

	uint32 HashDelta(const TArray<uint8>& DeltaBytes)
	{
		if (DeltaBytes.IsEmpty()) { return 0; }
		return FCrc::MemCrc32(DeltaBytes.GetData(), DeltaBytes.Num());
	}
}
