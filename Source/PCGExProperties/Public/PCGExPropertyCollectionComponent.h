// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExProperty.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

#include "PCGExPropertyCollectionComponent.generated.h"

/**
 * Actor component for attaching property collections to any actor.
 * Runtime-compatible - can be used on any actor, not just editor cages.
 *
 * Valency scans for these on cages/patterns during compilation.
 * Other systems can scan for them on spawned actors at runtime.
 *
 * This is the bridge between the property system and the actor world:
 * place this component on any actor, define properties in the Details panel,
 * and any PCGEx system that scans for properties will find them.
 *
 * Custom property types defined in any module will appear in the schema
 * collection's FInstancedStruct picker automatically.
 */
UCLASS(ClassGroup = "PCGEx", meta = (BlueprintSpawnableComponent, DisplayName = "PCGEx Property Collection"))
class PCGEXPROPERTIES_API UPCGExPropertyCollectionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPCGExPropertyCollectionComponent();

	virtual void OnRegister() override;

	/**
	 * Property collection with schema definitions and default values.
	 * These compile into runtime property data during cage/pattern builds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Properties")
	FPCGExPropertySchemaCollection Properties;

	/**
	 * Get the property collection.
	 * @return Reference to the property schema collection
	 */
	const FPCGExPropertySchemaCollection& GetProperties() const
	{
		return Properties;
	}

	/**
	 * Get the property collection (mutable).
	 * @return Reference to the property schema collection
	 */
	FPCGExPropertySchemaCollection& GetPropertiesMutable()
	{
		return Properties;
	}

	/**
	 * Locate the property-collection component on an actor with copy-paste resilience.
	 *
	 * FindComponentByClass walks the runtime-registered OwnedComponents set. Level-editor
	 * copy-paste of an actor that carries an instance component sometimes produces a
	 * duplicate whose component is present in the persisted InstanceComponents array but
	 * never registered into OwnedComponents -- making FindComponentByClass return null
	 * even though the data is on the actor. Fallback walks the persisted list so callers
	 * (the actor-collection scan path, the level-exporter mesh path, and any future
	 * downstream consumer) all see the same set of components.
	 *
	 * Const-on-input mirrors AActor::FindComponentByClass: walking the component set is a
	 * read on the actor; the returned component is mutable because callers commonly need
	 * to call mutating accessors on it.
	 */
	static UPCGExPropertyCollectionComponent* FindOnActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}
		if (UPCGExPropertyCollectionComponent* Comp = Actor->FindComponentByClass<UPCGExPropertyCollectionComponent>())
		{
			return Comp;
		}
		for (UActorComponent* IC : Actor->GetInstanceComponents())
		{
			if (UPCGExPropertyCollectionComponent* Candidate = Cast<UPCGExPropertyCollectionComponent>(IC))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * Extract the authored schema from a donor actor's property-collection component, with
	 * SyncPropertyName run on every schema first so each FInstancedStruct's inner property
	 * carries up-to-date PropertyName + HeaderId identity. Returns an empty array when the
	 * actor lacks a component.
	 */
	static TArray<FInstancedStruct> ExtractSchemaFromActor(const AActor* Actor)
	{
		UPCGExPropertyCollectionComponent* Comp = FindOnActor(Actor);
		if (!Comp)
		{
			return {};
		}
		for (FPCGExPropertySchema& Schema : Comp->GetPropertiesMutable().Schemas)
		{
			Schema.SyncPropertyName();
		}
		return Comp->GetProperties().BuildSchema();
	}
};
