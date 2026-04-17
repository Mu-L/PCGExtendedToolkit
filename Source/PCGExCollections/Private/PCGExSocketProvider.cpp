// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExSocketProvider.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

APCGExSocketActor::APCGExSocketActor()
{
	PrimaryActorTick.bCanEverTick = false;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = MeshComponent;

	MeshComponent->SetStaticMesh(LoadObject<UStaticMesh>(
		nullptr, TEXT("/PCG/DebugObjects/PCG_AxisTripod.PCG_AxisTripod")));
}

void APCGExSocketActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	CachedTransform = Transform;
}
