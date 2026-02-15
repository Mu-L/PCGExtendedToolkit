// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "EditorMode/Constraints/PCGExConstraintVis_Branch.h"

#include "SceneManagement.h"
#include "Growth/Constraints/PCGExConstraint_Branch.h"

#pragma region FBranchVisualizer

void FBranchVisualizer::DrawIndicator(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color) const
{
	// Small Y-shape indicator
	const FVector Center = ConnectorWorld.GetTranslation() + ConnectorWorld.GetRotation().GetUpVector() * 5.0f;
	const FVector Right = ConnectorWorld.GetRotation().GetRightVector() * 2.5f;
	const FVector Up = ConnectorWorld.GetRotation().GetUpVector() * 2.5f;

	// Stem
	PDI->DrawLine(Center - Up, Center, Color, SDPG_World, 1.5f);
	// Fork
	PDI->DrawLine(Center, Center + Right + Up, Color, SDPG_World, 1.5f);
	PDI->DrawLine(Center, Center - Right + Up, Color, SDPG_World, 1.5f);
}

void FBranchVisualizer::DrawZone(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color) const
{
	// Larger Y-shape with pass/fail coloring
	const FVector Center = ConnectorWorld.GetTranslation();
	const FQuat Rot = ConnectorWorld.GetRotation();
	const FVector Right = Rot.GetRightVector();
	const FVector Up = Rot.GetUpVector();
	const FVector Fwd = Rot.GetForwardVector();

	// Fork point offset forward
	const FVector ForkBase = Center + Fwd * 2.0f;
	const FVector ForkPoint = ForkBase + Up * 6.0f;

	// Stem (from connector to fork point)
	PDI->DrawLine(ForkBase, ForkPoint, Color, SDPG_World, 1.5f);

	// Pass branch (right, green tint)
	const FLinearColor PassColor = FLinearColor::LerpUsingHSV(Color, FLinearColor::Green, 0.3f);
	PDI->DrawLine(ForkPoint, ForkPoint + Right * 8.0f + Up * 6.0f, PassColor, SDPG_World, 1.0f);
	PDI->DrawPoint(ForkPoint + Right * 8.0f + Up * 6.0f, PassColor, 5.0f, SDPG_World);

	// Fail branch (left, red tint)
	const FLinearColor FailColor = FLinearColor::LerpUsingHSV(Color, FLinearColor::Red, 0.3f);
	PDI->DrawLine(ForkPoint, ForkPoint - Right * 8.0f + Up * 6.0f, FailColor, SDPG_World, 1.0f);
	PDI->DrawPoint(ForkPoint - Right * 8.0f + Up * 6.0f, FailColor, 5.0f, SDPG_World);

	// Fork point dot
	PDI->DrawPoint(ForkPoint, Color, 6.0f, SDPG_World);
}

void FBranchVisualizer::DrawDetail(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color,
	bool bIsActiveConstraint) const
{
	// Draw zone as base
	DrawZone(PDI, ConnectorWorld, Constraint, Color);

	// Additional detail: condition indicator at fork point
	const FQuat Rot = ConnectorWorld.GetRotation();
	const FVector Fwd = Rot.GetForwardVector();
	const FVector Up = Rot.GetUpVector();
	const FVector ForkPoint = ConnectorWorld.GetTranslation() + Fwd * 2.0f + Up * 6.0f;

	// Question mark indicator (small diamond around fork)
	const FVector Right = Rot.GetRightVector();
	const FLinearColor DetailColor = bIsActiveConstraint ? Color : Color * 0.8f;
	constexpr float S = 2.0f;

	PDI->DrawLine(ForkPoint + Up * S, ForkPoint + Right * S, DetailColor, SDPG_World, 1.5f);
	PDI->DrawLine(ForkPoint + Right * S, ForkPoint - Up * S, DetailColor, SDPG_World, 1.5f);
	PDI->DrawLine(ForkPoint - Up * S, ForkPoint - Right * S, DetailColor, SDPG_World, 1.5f);
	PDI->DrawLine(ForkPoint - Right * S, ForkPoint + Up * S, DetailColor, SDPG_World, 1.5f);
}

#pragma endregion
