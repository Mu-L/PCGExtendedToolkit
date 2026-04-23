// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExActorPropertyDelta.h"

#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
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
	static constexpr uint32 DeltaWireVersion = 2u;

	namespace Internal
	{
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

		class FDeltaWriter : public FObjectWriter
		{
		public:
			using FObjectWriter::FObjectWriter;

			virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
			{
				if (!IsInstanceEditableProperty(InProperty)) { return true; }
				if (IsTransformProperty(InProperty)) { return true; }
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
				if (!IsInstanceEditableProperty(InProperty)) { return true; }
				if (IsTransformProperty(InProperty)) { return true; }
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

		/** Serialize only the properties that differ from Defaults into OutBytes.
		 *  Skips entirely if nothing differs -- avoids the ~13-byte terminator overhead
		 *  that SerializeTaggedProperties writes even when no properties are emitted. */
		static void SerializeObjectDelta(
			UObject* Object,
			UObject* Defaults,
			TArray<uint8>& OutBytes)
		{
			if (!HasInstanceEditableDelta(Object, Defaults)) { return; }

			UClass* Class = Object->GetClass();
			FDeltaWriter Writer(OutBytes);
			FStructuredArchiveFromArchive Adapter(Writer);
			Class->SerializeTaggedProperties(
				Adapter.GetSlot(),
				reinterpret_cast<uint8*>(Object),
				Class,
				reinterpret_cast<uint8*>(Defaults),
				Object);
		}

		/** Deserialize delta bytes onto Object; properties not present in the delta are untouched.
		 *  Uses CDO as the diff baseline so tagged properties resolve correctly. */
		static void DeserializeObjectDelta(
			UObject* Object,
			const TArray<uint8>& InBytes)
		{
			UClass* Class = Object->GetClass();
			FDeltaReader Reader(InBytes);
			FStructuredArchiveFromArchive Adapter(Reader);
			Class->SerializeTaggedProperties(
				Adapter.GetSlot(),
				reinterpret_cast<uint8*>(Object),
				Class,
				reinterpret_cast<uint8*>(Class->GetDefaultObject()),
				Object);
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

		// Validate magic+version. Older collections saved delta bytes in an incompatible
		// format (FStructuredArchiveFromArchive framing) -- reading those with the current
		// binary path would misalign properties and corrupt FName values, which crashes
		// the editor on later save. Silently skip unknown-format deltas; the user must
		// rebuild the collection to regenerate deltas in the current format.
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

			// Skip if target actor doesn't have a component with this name
			if (UActorComponent* Component = FindObjectFast<UActorComponent>(Actor, CompName))
			{
				Internal::DeserializeObjectDelta(Component, CompBytes);
			}
		}

		// Repair engine-managed invariants that the tagged-property delta cannot express.
		// Runs on every component so archetype-cloning inconsistencies are fixed too.
		FixupRegistry::RunAll(Actor);
	}

	uint32 HashDelta(const TArray<uint8>& DeltaBytes)
	{
		if (DeltaBytes.IsEmpty()) { return 0; }
		return FCrc::MemCrc32(DeltaBytes.GetData(), DeltaBytes.Num());
	}
}
