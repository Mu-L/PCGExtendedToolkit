﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExDetails.h"

#include "PCGComponent.h"

void FPCGExCollisionDetails::Init(const FPCGExContext* InContext)
{
	World = InContext->GetWorld();

	if (bIgnoreActors)
	{
		const TFunction<bool(const AActor*)> BoundsCheck = [](const AActor*) -> bool { return true; };
		const TFunction<bool(const AActor*)> SelfIgnoreCheck = [](const AActor*) -> bool { return true; };
		IgnoredActors = PCGActorSelector::FindActors(IgnoredActorSelector, InContext->GetComponent(), BoundsCheck, SelfIgnoreCheck);
	}

	if (bIgnoreSelf) { IgnoredActors.Add(InContext->GetComponent()->GetOwner()); }
}

void FPCGExCollisionDetails::Update(FCollisionQueryParams& InCollisionParams) const
{
	InCollisionParams.bTraceComplex = bTraceComplex;
	InCollisionParams.AddIgnoredActors(IgnoredActors);
}

bool FPCGExCollisionDetails::Linecast(const FVector& From, const FVector& To, FHitResult& HitResult) const
{
	FCollisionQueryParams CollisionParams;
	Update(CollisionParams);

	switch (CollisionType)
	{
	case EPCGExCollisionFilterType::Channel:
		return World->LineTraceSingleByChannel(HitResult, From, To, CollisionChannel, CollisionParams);
	case EPCGExCollisionFilterType::ObjectType:
		return World->LineTraceSingleByObjectType(HitResult, From, To, FCollisionObjectQueryParams(CollisionObjectType), CollisionParams);
	case EPCGExCollisionFilterType::Profile:
		return World->LineTraceSingleByProfile(HitResult, From, To, CollisionProfileName, CollisionParams);
	default:
		return false;
	}
}

namespace PCGEx
{
}
