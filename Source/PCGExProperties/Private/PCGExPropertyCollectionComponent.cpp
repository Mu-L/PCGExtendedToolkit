// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExPropertyCollectionComponent.h"

UPCGExPropertyCollectionComponent::UPCGExPropertyCollectionComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UPCGExPropertyCollectionComponent::OnRegister()
{
	Super::OnRegister();

	// Repair instances whose schema drifted from their Blueprint's after a CDO edit:
	// UE's per-property propagation doesn't reliably carry FInstancedStruct type info
	// through array elements, leaving existing instances with default-constructed entries.
	// Instance-created components own their schema independently (archetype is the empty
	// class CDO), so syncing would wipe the user's authored schema.
	if (IsTemplate() || CreationMethod == EComponentCreationMethod::Instance)
	{
		return;
	}

	const UPCGExPropertyCollectionComponent* Archetype = Cast<UPCGExPropertyCollectionComponent>(GetArchetype());
	if (!Archetype || Archetype == this)
	{
		return;
	}

	Properties.SyncFromArchetype(Archetype->Properties);
}
