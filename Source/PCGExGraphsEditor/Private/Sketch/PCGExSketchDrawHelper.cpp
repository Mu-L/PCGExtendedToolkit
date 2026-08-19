// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExSketchDrawHelper.h"

#include "SceneManagement.h"
#include "Sketch/PCGExSketchEditController.h"

namespace PCGExSketchDrawHelper
{
	const FLinearColor FreeVertexColor = FLinearColor(0.9f, 0.9f, 0.9f);
	const FLinearColor BoundVertexColor = FLinearColor(0.15f, 0.85f, 0.8f);
	const FLinearColor SelectedColor = FLinearColor(1.0f, 0.65f, 0.1f);
	const FLinearColor HoverColor = FLinearColor(1.0f, 1.0f, 1.0f);
	const FLinearColor EdgeColor = FLinearColor(0.55f, 0.55f, 0.6f);
	const FLinearColor PreviewColor = FLinearColor(0.3f, 1.0f, 0.4f);
	const FLinearColor MergeColor = FLinearColor(1.0f, 0.35f, 0.15f);
	const FLinearColor BasisColor = FLinearColor(0.35f, 0.5f, 0.9f, 0.6f);

	constexpr float VertexSize = 10.0f;
	constexpr float SelectedVertexSize = 14.0f;
	constexpr float HoverBonus = 4.0f;
	constexpr float EdgeThickness = 1.5f;
	constexpr float SelectedEdgeThickness = 3.0f;
}

void FPCGExSketchDrawHelper::Draw(const FPCGExSketchEditController& Controller, FPrimitiveDrawInterface* PDI)
{
	const FPCGExClusterSketchModel* Model = Controller.GetTarget().GetModel();
	if (!Model || !PDI)
	{
		return;
	}

	FPCGExLatticeBasis Basis;
	const bool bHasBasis = Controller.GetBasis(Basis);
	const FTransform LocalToWorld = Controller.GetTarget().GetLocalToWorld();

	const FPCGExSketchHit& Hover = Controller.GetHover();
	const TSet<int32>& SelectedVertices = Controller.GetSelectedVertices();
	const TSet<int32>& SelectedEdges = Controller.GetSelectedEdges();

	// Vertex locations resolved once, coord-derived for bound vertices (mirror of the print rule).
	TArray<FVector> Locations;
	Locations.SetNumUninitialized(Model->Vertices.Num());
	for (int32 i = 0; i < Model->Vertices.Num(); ++i)
	{
		const FPCGExClusterSketchVertex& V = Model->Vertices[i];
		const FVector Local = (V.bLatticeBound && bHasBasis) ? Basis.CoordToWorld(V.LatticeCoord) : V.Transform.GetLocation();
		Locations[i] = LocalToWorld.TransformPosition(Local);
	}

	// Invalid-state highlight: vertices RESOLVING to the same printed location (duplicate coords,
	// overlapping free verts, or a rank-collapsed basis stacking layers). Same key as the print
	// warning and the Merge Collocated cleanup, so all three agree.
	TSet<int32> Overlapping;
	{
		TMap<FVector, int32> FirstAtLocation;
		FirstAtLocation.Reserve(Locations.Num());
		for (int32 i = 0; i < Locations.Num(); ++i)
		{
			const FVector Key = PCGExSketch::QuantizedLocationKey(Locations[i]);
			if (const int32* First = FirstAtLocation.Find(Key))
			{
				Overlapping.Add(i);
				Overlapping.Add(*First);
			}
			else
			{
				FirstAtLocation.Add(Key, i);
			}
		}
	}

	// Basis tripod at the lattice origin -- a cheap "the snap model is live" cue.
	if (bHasBasis)
	{
		const FVector Origin = LocalToWorld.TransformPosition(Basis.Origin);
		for (int32 k = 0; k < Basis.NumAxes; ++k)
		{
			PDI->DrawLine(Origin, LocalToWorld.TransformPosition(Basis.Origin + Basis.AxisVecs[k]), PCGExSketchDrawHelper::BasisColor, SDPG_Foreground, 0.5f);
		}
	}

	// Edges under vertices.
	const double StubLength = (bHasBasis && Basis.NumAxes > 0) ? Basis.AxisVecs[0].Size() * 0.35 : 35.0;
	for (int32 e = 0; e < Model->Edges.Num(); ++e)
	{
		const FPCGExClusterSketchEdge& E = Model->Edges[e];
		if (!Locations.IsValidIndex(E.A) || !Locations.IsValidIndex(E.B))
		{
			// A DORMANT edge (endpoint index not materialized yet) would otherwise be invisible until a
			// new vertex silently activates it -- draw a warning stub from whichever endpoint exists.
			const int32 ValidEnd = Locations.IsValidIndex(E.A) ? E.A : (Locations.IsValidIndex(E.B) ? E.B : INDEX_NONE);
			if (ValidEnd != INDEX_NONE)
			{
				DrawDashedLine(PDI, Locations[ValidEnd], Locations[ValidEnd] + FVector(0, 0, StubLength), PCGExSketchDrawHelper::MergeColor, 5.0, SDPG_Foreground);
			}
			continue;
		}
		const bool bSelected = SelectedEdges.Contains(e);
		const bool bHovered = Hover.Type == FPCGExSketchHit::EType::Edge && Hover.Index == e;
		// While the host's delete modifier is held, the hovered edge reads as a delete target.
		const FLinearColor Color = bHovered
			                           ? (Controller.GetEdgeDeleteIntent() ? PCGExSketchDrawHelper::MergeColor : PCGExSketchDrawHelper::HoverColor)
			                           : (bSelected ? PCGExSketchDrawHelper::SelectedColor : PCGExSketchDrawHelper::EdgeColor);
		PDI->DrawLine(Locations[E.A], Locations[E.B], Color, SDPG_Foreground, (bSelected || bHovered) ? PCGExSketchDrawHelper::SelectedEdgeThickness : PCGExSketchDrawHelper::EdgeThickness);
	}

	for (int32 i = 0; i < Model->Vertices.Num(); ++i)
	{
		const bool bSelected = SelectedVertices.Contains(i);
		const bool bHovered = Hover.Type == FPCGExSketchHit::EType::Vertex && Hover.Index == i;
		FLinearColor Color = Model->Vertices[i].bLatticeBound ? PCGExSketchDrawHelper::BoundVertexColor : PCGExSketchDrawHelper::FreeVertexColor;
		if (bSelected)
		{
			Color = PCGExSketchDrawHelper::SelectedColor;
		}
		if (Overlapping.Contains(i))
		{
			// Invalid state outranks selection; only hover (aiming) overrides it.
			Color = PCGExSketchDrawHelper::MergeColor;
		}
		if (bHovered)
		{
			Color = PCGExSketchDrawHelper::HoverColor;
		}
		const float Size = (bSelected ? PCGExSketchDrawHelper::SelectedVertexSize : PCGExSketchDrawHelper::VertexSize) + (bHovered ? PCGExSketchDrawHelper::HoverBonus : 0.0f);
		PDI->DrawPoint(Locations[i], Color, Size, SDPG_Foreground);
	}

	// Drag affordances: move ghost or connect preview line.
	if (Controller.GetDragMode() == FPCGExSketchEditController::EDragMode::Connect && Locations.IsValidIndex(Controller.GetDragVertex()))
	{
		const FVector Start = Locations[Controller.GetDragVertex()];
		const int32 TargetVertex = Controller.GetDragTargetVertex();
		const FVector End = Locations.IsValidIndex(TargetVertex) ? Locations[TargetVertex] : LocalToWorld.TransformPosition(Controller.GetDragPreviewLocal());
		DrawDashedLine(PDI, Start, End, PCGExSketchDrawHelper::PreviewColor, 12.0, SDPG_Foreground);
		PDI->DrawPoint(End, PCGExSketchDrawHelper::PreviewColor, PCGExSketchDrawHelper::SelectedVertexSize, SDPG_Foreground);
	}
	else if (Controller.GetDragMode() == FPCGExSketchEditController::EDragMode::Move)
	{
		const int32 MergeCandidate = Controller.GetMergeCandidate();
		if (Locations.IsValidIndex(MergeCandidate))
		{
			// Release here MERGES the dragged vertex into this one -- loud, warm highlight.
			const FVector MergePos = Locations[MergeCandidate];
			PDI->DrawPoint(MergePos, PCGExSketchDrawHelper::MergeColor, PCGExSketchDrawHelper::SelectedVertexSize + 8.0f, SDPG_Foreground);
			DrawDashedLine(PDI, LocalToWorld.TransformPosition(Controller.GetDragPreviewLocal()), MergePos, PCGExSketchDrawHelper::MergeColor, 8.0, SDPG_Foreground);
		}
		else
		{
			PDI->DrawPoint(LocalToWorld.TransformPosition(Controller.GetDragPreviewLocal()), PCGExSketchDrawHelper::PreviewColor, PCGExSketchDrawHelper::VertexSize, SDPG_Foreground);
		}
	}
}
