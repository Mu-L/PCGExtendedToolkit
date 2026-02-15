// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "EditorMode/Constraints/PCGExConstraintVis_ContextCondition.h"

#include "SceneManagement.h"
#include "Growth/Constraints/PCGExConstraint_ContextCondition.h"

#pragma region FContextConditionVisualizer

void FContextConditionVisualizer::DrawIndicator(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color) const
{
	// Small filter icon — funnel shape
	const FVector Center = ConnectorWorld.GetTranslation() + ConnectorWorld.GetRotation().GetUpVector() * 5.0f;
	const FVector Right = ConnectorWorld.GetRotation().GetRightVector();
	const FVector Up = ConnectorWorld.GetRotation().GetUpVector();

	// Funnel: wide top, narrow bottom
	PDI->DrawLine(Center - Right * 3.0f + Up * 2.0f, Center + Right * 3.0f + Up * 2.0f, Color, SDPG_World, 1.0f);
	PDI->DrawLine(Center - Right * 3.0f + Up * 2.0f, Center - Up * 2.0f, Color, SDPG_World, 1.0f);
	PDI->DrawLine(Center + Right * 3.0f + Up * 2.0f, Center - Up * 2.0f, Color, SDPG_World, 1.0f);
}

void FContextConditionVisualizer::DrawZone(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color) const
{
	const auto& Condition = static_cast<const FPCGExConstraint_ContextCondition&>(Constraint);

	const FVector Center = ConnectorWorld.GetTranslation();
	const FQuat Rot = ConnectorWorld.GetRotation();
	const FVector Right = Rot.GetRightVector();
	const FVector Up = Rot.GetUpVector();
	const FVector Fwd = Rot.GetForwardVector();

	// Draw a horizontal line representing the threshold
	const FVector ThresholdBase = Center + Fwd * 2.0f;
	PDI->DrawLine(ThresholdBase - Right * 10.0f, ThresholdBase + Right * 10.0f, Color * 0.6f, SDPG_World, 0.5f);

	// Draw comparison direction arrow
	FVector ArrowDir = Up; // Points up for "greater" comparisons
	switch (Condition.Comparison)
	{
	case EPCGExComparison::StrictlySmaller:
	case EPCGExComparison::EqualOrSmaller:
		ArrowDir = -Up;
		break;
	default:
		break;
	}

	const FVector ArrowStart = ThresholdBase;
	const FVector ArrowEnd = ThresholdBase + ArrowDir * 8.0f;
	PDI->DrawLine(ArrowStart, ArrowEnd, Color, SDPG_World, 1.5f);

	// Arrowhead
	PDI->DrawLine(ArrowEnd, ArrowEnd - ArrowDir * 2.0f + Right * 2.0f, Color, SDPG_World, 1.0f);
	PDI->DrawLine(ArrowEnd, ArrowEnd - ArrowDir * 2.0f - Right * 2.0f, Color, SDPG_World, 1.0f);

	// Threshold dot
	PDI->DrawPoint(ThresholdBase, Color, 5.0f, SDPG_World);
}

void FContextConditionVisualizer::DrawDetail(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color,
	bool bIsActiveConstraint) const
{
	DrawZone(PDI, ConnectorWorld, Constraint, Color);

	// Additional detail: threshold value marker
	const FVector Center = ConnectorWorld.GetTranslation();
	const FQuat Rot = ConnectorWorld.GetRotation();
	const FVector Fwd = Rot.GetForwardVector();
	const FVector Right = Rot.GetRightVector();

	const FLinearColor DetailColor = bIsActiveConstraint ? Color : Color * 0.8f;
	const FVector ThresholdBase = Center + Fwd * 2.0f;

	// Tick marks along the threshold line
	const FVector Up = Rot.GetUpVector();
	for (int32 i = -2; i <= 2; ++i)
	{
		const FVector TickPos = ThresholdBase + Right * (static_cast<float>(i) * 5.0f);
		PDI->DrawLine(TickPos - Up * 1.5f, TickPos + Up * 1.5f, DetailColor, SDPG_World, 0.5f);
	}
}

#pragma endregion
