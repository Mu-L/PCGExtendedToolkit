// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "EditorMode/Constraints/PCGExConstraintVis_Preset.h"

#include "SceneManagement.h"
#include "Growth/Constraints/PCGExConstraintPreset.h"

#pragma region FPresetVisualizer

void FPresetVisualizer::DrawIndicator(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color) const
{
	// Small "P" marker — diamond shape near connector
	const FVector Center = ConnectorWorld.GetTranslation() + ConnectorWorld.GetRotation().GetUpVector() * 5.0f;
	const FVector Right = ConnectorWorld.GetRotation().GetRightVector() * 3.0f;
	const FVector Up = ConnectorWorld.GetRotation().GetUpVector() * 3.0f;

	// Diamond shape
	PDI->DrawLine(Center + Up, Center + Right, Color, SDPG_World, 1.0f);
	PDI->DrawLine(Center + Right, Center - Up, Color, SDPG_World, 1.0f);
	PDI->DrawLine(Center - Up, Center - Right, Color, SDPG_World, 1.0f);
	PDI->DrawLine(Center - Right, Center + Up, Color, SDPG_World, 1.0f);
}

void FPresetVisualizer::DrawZone(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color) const
{
	// Dashed box outline
	const FVector Center = ConnectorWorld.GetTranslation();
	const FQuat Rot = ConnectorWorld.GetRotation();
	const FVector Right = Rot.GetRightVector() * 8.0f;
	const FVector Up = Rot.GetUpVector() * 8.0f;
	const FVector Fwd = Rot.GetForwardVector() * 2.0f;

	// Offset slightly forward from connector
	const FVector Base = Center + Fwd;

	const FLinearColor DashColor = Color * 0.7f;
	constexpr int32 DashSegments = 4;

	// Draw top and bottom edges as dashes
	auto DrawDashedLine = [&](const FVector& Start, const FVector& End)
	{
		for (int32 i = 0; i < DashSegments; ++i)
		{
			const float T0 = static_cast<float>(i * 2) / static_cast<float>(DashSegments * 2);
			const float T1 = static_cast<float>(i * 2 + 1) / static_cast<float>(DashSegments * 2);
			PDI->DrawLine(FMath::Lerp(Start, End, T0), FMath::Lerp(Start, End, T1), DashColor, SDPG_World, 0.5f);
		}
	};

	const FVector TL = Base - Right + Up;
	const FVector TR = Base + Right + Up;
	const FVector BR = Base + Right - Up;
	const FVector BL = Base - Right - Up;

	DrawDashedLine(TL, TR);
	DrawDashedLine(TR, BR);
	DrawDashedLine(BR, BL);
	DrawDashedLine(BL, TL);

	// Center dot as "P" marker
	PDI->DrawPoint(Base, Color, 6.0f, SDPG_World);
}

void FPresetVisualizer::DrawDetail(
	FPrimitiveDrawInterface* PDI,
	const FTransform& ConnectorWorld,
	const FPCGExConnectorConstraint& Constraint,
	const FLinearColor& Color,
	bool bIsActiveConstraint) const
{
	DrawZone(PDI, ConnectorWorld, Constraint, Color);

	// Corner dots at detail level
	const FVector Center = ConnectorWorld.GetTranslation();
	const FQuat Rot = ConnectorWorld.GetRotation();
	const FVector Right = Rot.GetRightVector() * 8.0f;
	const FVector Up = Rot.GetUpVector() * 8.0f;
	const FVector Fwd = Rot.GetForwardVector() * 2.0f;
	const FVector Base = Center + Fwd;
	const FLinearColor HandleColor = bIsActiveConstraint ? Color : Color * 0.8f;

	PDI->DrawPoint(Base - Right + Up, HandleColor, 4.0f, SDPG_World);
	PDI->DrawPoint(Base + Right + Up, HandleColor, 4.0f, SDPG_World);
	PDI->DrawPoint(Base + Right - Up, HandleColor, 4.0f, SDPG_World);
	PDI->DrawPoint(Base - Right - Up, HandleColor, 4.0f, SDPG_World);
}

#pragma endregion
