﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExRelaxClusterOperation.h"
#include "PCGExForceDirectedRelax.generated.h"

/**
 * 
 */
UCLASS(MinimalAPI, meta=(DisplayName="Force Directed", PCGExNodeLibraryDoc="clusters/relax-cluster/force-directed"))
class UPCGExForceDirectedRelax : public UPCGExRelaxClusterOperation
{
	GENERATED_BODY()

public:
	virtual void CopySettingsFrom(const UPCGExInstancedFactory* Other) override
	{
		Super::CopySettingsFrom(Other);
		if (const UPCGExForceDirectedRelax* TypedOther = Cast<UPCGExForceDirectedRelax>(Other))
		{
			SpringConstant = TypedOther->SpringConstant;
			ElectrostaticConstant = TypedOther->ElectrostaticConstant;
		}
	}

	virtual void Step1(const PCGExCluster::FNode& Node) override
	{
		const FVector Position = (ReadBuffer->GetData() + Node.Index)->GetLocation();
		FVector Force = FVector::ZeroVector;

		for (const PCGExGraph::FLink& Lk : Node.Links)
		{
			const FVector OtherPosition = (ReadBuffer->GetData() + Lk.Node)->GetLocation();
			CalculateAttractiveForce(Force, Position, OtherPosition);
			CalculateRepulsiveForce(Force, Position, OtherPosition);
		}

		(*WriteBuffer)[Node.Index].SetLocation(Position + Force);
	}

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	double SpringConstant = 0.1;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	double ElectrostaticConstant = 1000;

protected:
	void CalculateAttractiveForce(FVector& Force, const FVector& A, const FVector& B) const
	{
		// Calculate the displacement vector between the nodes
		FVector Displacement = B - A;

		const double Distance = FMath::Max(Displacement.Length(), 1e-5);
		Displacement /= Distance;

		// Calculate the force magnitude using Hooke's law
		const double ForceMagnitude = SpringConstant * Distance;
		Force += Displacement * ForceMagnitude;
	}

	void CalculateRepulsiveForce(FVector& Force, const FVector& A, const FVector& B) const
	{
		// Calculate the displacement vector between the nodes
		FVector Displacement = B - A;

		const double Distance = FMath::Max(Displacement.Length(), 1e-5);
		Displacement /= Distance;

		// Calculate the force magnitude using Coulomb's law
		const double ForceMagnitude = ElectrostaticConstant / (Distance * Distance);
		Force -= Displacement * ForceMagnitude;
	}
};
