// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExProperties.h"
#include "PCGExProperty.h"
#include "PCGExPropertyTypes.h"

#if WITH_EDITOR
void FPCGExPropertiesModule::RegisterToEditor(const TSharedPtr<FSlateStyleSet>& InStyle)
{
	// No special editor registration needed for properties module
	IPCGExModuleInterface::RegisterToEditor(InStyle);
}
#endif

PCGEX_IMPLEMENT_MODULE(FPCGExPropertiesModule, PCGExProperties)

#pragma region FPCGExPropertySchema

// Default constructor initializes with Float type and a unique HeaderId.
// HeaderId is generated once and preserved through the lifetime of this schema entry,
// even if the user changes the property type or renames it.
FPCGExPropertySchema::FPCGExPropertySchema()
{
#if WITH_EDITOR
	HeaderId = GetTypeHash(FGuid::NewGuid());
	Property.InitializeAs<FPCGExProperty_Float>();
#endif
}

#pragma endregion

#pragma region FPCGExPropertySchemaCollection

const FPCGExPropertySchema* FPCGExPropertySchemaCollection::FindByName(FName PropertyName) const
{
	if (PropertyName.IsNone())
	{
		return nullptr;
	}

	for (const FPCGExPropertySchema& Schema : Schemas)
	{
		if (Schema.Name == PropertyName)
		{
			return &Schema;
		}
	}
	return nullptr;
}

TArray<FInstancedStruct> FPCGExPropertySchemaCollection::BuildSchema() const
{
	TArray<FInstancedStruct> Result;
	Result.Reserve(Schemas.Num());

	for (const FPCGExPropertySchema& Schema : Schemas)
	{
		if (Schema.IsValid())
		{
			Result.Add(Schema.Property);
		}
	}
	return Result;
}

bool FPCGExPropertySchemaCollection::ValidateUniqueNames(TArray<FName>& OutDuplicates) const
{
	OutDuplicates.Empty();
	TSet<FName> Seen;

	for (const FPCGExPropertySchema& Schema : Schemas)
	{
		if (Schema.Name.IsNone())
		{
			continue;
		}

		bool bAlreadyInSet = false;
		Seen.Add(Schema.Name, &bAlreadyInSet);

		if (bAlreadyInSet)
		{
			OutDuplicates.AddUnique(Schema.Name);
		}
	}

	return OutDuplicates.IsEmpty();
}

void FPCGExPropertySchemaCollection::SyncAllSchemas()
{
	for (FPCGExPropertySchema& Schema : Schemas)
	{
		Schema.SyncPropertyName();
	}
}

void FPCGExPropertySchemaCollection::SyncOverrides(FPCGExPropertyOverrides& Overrides)
{
	SyncAllSchemas();
	TArray<FInstancedStruct> Schema = BuildSchema();
	Overrides.SyncToSchema(Schema);
}

void FPCGExPropertySchemaCollection::SyncOverridesArray(TArray<FPCGExPropertyOverrides>& OverridesArray)
{
	SyncAllSchemas();
	TArray<FInstancedStruct> Schema = BuildSchema();
	for (FPCGExPropertyOverrides& Overrides : OverridesArray)
	{
		Overrides.SyncToSchema(Schema);
	}
}

void FPCGExPropertySchemaCollection::SyncFromArchetype(const FPCGExPropertySchemaCollection& Archetype)
{
#if WITH_EDITOR
	// Identity matching uses the INNER FPCGExProperty::HeaderId (the one stored inside the
	// FInstancedStruct payload), not the outer FPCGExPropertySchema::HeaderId. Both fields
	// are auto-randomized in their default constructors, but FInstancedStruct serializes
	// its payload atomically -- so the inner HeaderId propagates reliably from CDO to
	// instance, while the outer struct-level editor-only field does not. This matches the
	// pattern FPCGExPropertyOverrides::SyncToSchema already uses for the same reason.

	// Fast path: structure already mirrors the archetype. Hits on every register after the
	// first sync, keeping the per-register cost minimal.
	if (Schemas.Num() == Archetype.Schemas.Num())
	{
		bool bStructureMatches = true;
		for (int32 i = 0; i < Schemas.Num(); ++i)
		{
			const FPCGExProperty* MyProp = Schemas[i].GetProperty();
			const FPCGExProperty* ArchProp = Archetype.Schemas[i].GetProperty();
			if (!MyProp || !ArchProp ||
				Schemas[i].Property.GetScriptStruct() != Archetype.Schemas[i].Property.GetScriptStruct() ||
				MyProp->HeaderId != ArchProp->HeaderId)
			{
				bStructureMatches = false;
				break;
			}
		}

		if (bStructureMatches)
		{
			return;
		}
	}

	TArray<FPCGExPropertySchema> OldSchemas = MoveTemp(Schemas);

	TMap<int32, FPCGExPropertySchema> ExistingByInnerHeaderId;
	ExistingByInnerHeaderId.Reserve(OldSchemas.Num());
	for (FPCGExPropertySchema& Old : OldSchemas)
	{
		if (const FPCGExProperty* Prop = Old.GetProperty())
		{
			if (Prop->HeaderId != 0)
			{
				ExistingByInnerHeaderId.Add(Prop->HeaderId, MoveTemp(Old));
			}
		}
	}

	Schemas.Reset(Archetype.Schemas.Num());

	for (const FPCGExPropertySchema& ArchetypeSchema : Archetype.Schemas)
	{
		FPCGExPropertySchema& NewSchema = Schemas.AddDefaulted_GetRef();
		NewSchema.HeaderId = ArchetypeSchema.HeaderId;
		NewSchema.Name = ArchetypeSchema.Name;

		FPCGExPropertySchema* Existing = nullptr;
		if (const FPCGExProperty* ArchProp = ArchetypeSchema.GetProperty())
		{
			if (ArchProp->HeaderId != 0)
			{
				Existing = ExistingByInnerHeaderId.Find(ArchProp->HeaderId);
			}
		}

		if (Existing && Existing->Property.GetScriptStruct() == ArchetypeSchema.Property.GetScriptStruct())
		{
			// Inner HeaderId matches and type is unchanged -- preserve this collection's Value
			NewSchema.Property = MoveTemp(Existing->Property);
		}
		else
		{
			// No match or type changed -- take the archetype's entry verbatim
			NewSchema.Property = ArchetypeSchema.Property;
		}

		NewSchema.SyncPropertyName();
	}
#endif
}

#pragma endregion

#pragma region FPCGExPropertyOverrides

// ============================================================================
// SyncToSchema - The heart of the override system
// ============================================================================
//
// This method rebuilds the Overrides array to match a new schema while preserving
// user state (enabled flags, values) through schema changes. It uses HeaderId
// (editor-only) as the stable key for matching.
//
// The algorithm:
// 1. Save old overrides and index them by HeaderId
// 2. For each schema property:
//    a. Try to find an existing override with matching HeaderId
//    b. If found AND same type: keep value + bEnabled, update PropertyName
//    c. If found but type changed: use schema default value, keep bEnabled
//    d. If not found (new property): use schema default, set bEnabled=false
// 3. Result: Overrides array is parallel with Schema array
//
// At runtime (no WITH_EDITOR), the HeaderId path is skipped and all overrides
// are rebuilt from schema defaults.

void FPCGExPropertyOverrides::SyncToSchema(const TArray<FInstancedStruct>& Schema)
{
	// Save existing overrides
	TArray<FPCGExPropertyOverrideEntry> OldOverrides = MoveTemp(Overrides);

	// Build map by HeaderId (stable identity)
	TMap<int32, FPCGExPropertyOverrideEntry> ExistingById;
#if WITH_EDITOR
	for (FPCGExPropertyOverrideEntry& Entry : OldOverrides)
	{
		if (const FPCGExProperty* Prop = Entry.Value.GetPtr<FPCGExProperty>())
		{
			if (Prop->HeaderId != 0)
			{
				ExistingById.Add(Prop->HeaderId, MoveTemp(Entry));
			}
		}
	}
#endif

	// Rebuild array to match schema exactly (parallel arrays)
	Overrides.Reset(Schema.Num());

	for (const FInstancedStruct& SchemaProp : Schema)
	{
		const FPCGExProperty* SchemaData = SchemaProp.GetPtr<FPCGExProperty>();
		if (!SchemaData)
		{
			continue;
		}

		FPCGExPropertyOverrideEntry& NewEntry = Overrides.AddDefaulted_GetRef();
		FPCGExPropertyOverrideEntry* Existing = nullptr;

#if WITH_EDITOR
		// Match by HeaderId (stable across rename/reorder/type change!)
		if (SchemaData->HeaderId != 0)
		{
			Existing = ExistingById.Find(SchemaData->HeaderId);
		}
#endif

		if (Existing)
		{
			// Found existing by HeaderId - preserve state
			NewEntry.bEnabled = Existing->bEnabled;

			if (Existing->Value.GetScriptStruct() == SchemaProp.GetScriptStruct())
			{
				// Same type - preserve value, update PropertyName from schema
				NewEntry.Value = MoveTemp(Existing->Value);

				if (FPCGExProperty* Prop = NewEntry.GetPropertyMutable())
				{
					Prop->PropertyName = SchemaData->PropertyName;
					// Sync structural sub-fields from the schema (e.g. FPCGExProperty_Enum's
					// Value.Class). Keeps schema-owned parts in lockstep while leaving
					// user-overridable parts of Value untouched.
					Prop->SyncStructuralFromSchema(*SchemaData);
				}
			}
			else
			{
				// Type changed - use schema default, preserve bEnabled
				NewEntry.Value = SchemaProp;
			}
		}
		else
		{
			// New property - use schema default, disabled
			NewEntry.Value = SchemaProp;
			NewEntry.bEnabled = false;
		}
	}
}

const FInstancedStruct* FPCGExPropertyOverrides::GetOverride(FName PropertyName) const
{
	for (const FPCGExPropertyOverrideEntry& Entry : Overrides)
	{
		if (Entry.bEnabled && Entry.GetPropertyName() == PropertyName)
		{
			return &Entry.Value;
		}
	}
	return nullptr;
}

#pragma endregion
