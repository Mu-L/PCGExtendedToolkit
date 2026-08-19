// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExClusterSketchViewportClient.h"

#include "EditorModeManager.h"
#include "InputRouter.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "BaseBehaviors/SingleClickOrDragBehavior.h"
#include "Sketch/PCGExSketchDrawHelper.h"
#include "Sketch/PCGExSketchEditController.h"
#include "Tools/EdModeInteractiveToolsContext.h"

namespace PCGExClusterSketchViewportClient
{
	// Depth reported for hits that have no meaningful ray depth (empty-space add capture, hover
	// everywhere): far enough that anything with a real depth wins the router's arbitration.
	constexpr double FarHitDepth = 1.0e8;
}

FPCGExClusterSketchViewportClient::FPCGExClusterSketchViewportClient(FEditorModeTools* InModeTools, FPreviewScene* InPreviewScene, const TSharedPtr<FPCGExSketchEditController>& InController)
	: FEditorViewportClient(InModeTools, InPreviewScene)
	, Controller(InController)
	, OwnerModeTools(InModeTools)
{
	// Continuous redraw: hover/drag affordances live in Draw, not in a scene component.
	SetRealtime(true);

	InputBehaviorSet = NewObject<UInputBehaviorSet>();

	USingleClickOrDragInputBehavior* ClickOrDrag = NewObject<USingleClickOrDragInputBehavior>();
	ClickOrDrag->Initialize(this, this);
	ClickOrDrag->Modifiers.RegisterModifier(CtrlModifierID, FInputDeviceState::IsCtrlKeyDown);
	ClickOrDrag->Modifiers.RegisterModifier(ShiftModifierID, FInputDeviceState::IsShiftKeyDown);
	InputBehaviorSet->Add(ClickOrDrag);

	UMouseHoverBehavior* HoverBehavior = NewObject<UMouseHoverBehavior>();
	HoverBehavior->Initialize(this);
	HoverBehavior->Modifiers.RegisterModifier(CtrlModifierID, FInputDeviceState::IsCtrlKeyDown);
	HoverBehavior->Modifiers.RegisterModifier(ShiftModifierID, FInputDeviceState::IsShiftKeyDown);
	InputBehaviorSet->Add(HoverBehavior);

	// The mode manager's tools context is constructed + activated with the manager itself, so the
	// router exists by the time any toolkit reaches viewport creation.
	OwnerModeTools->GetInteractiveToolsContext()->InputRouter->RegisterSource(this);
}

FPCGExClusterSketchViewportClient::~FPCGExClusterSketchViewportClient()
{
	// The mode manager (and its router) lives on the toolkit BASE class, so it outlives this client.
	if (OwnerModeTools)
	{
		if (UModeManagerInteractiveToolsContext* ToolsContext = OwnerModeTools->GetInteractiveToolsContext())
		{
			if (ToolsContext->InputRouter)
			{
				ToolsContext->InputRouter->DeregisterSource(this);
			}
		}
	}
}

void FPCGExClusterSketchViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	// Base first: it forwards to the mode tools / ITF render path.
	FEditorViewportClient::Draw(View, PDI);
	if (Controller)
	{
		FPCGExSketchDrawHelper::Draw(*Controller, PDI);
	}
}

bool FPCGExClusterSketchViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	if (EventArgs.Event == IE_Pressed && Controller)
	{
		if (EventArgs.Key == EKeys::Delete)
		{
			Controller->DeleteSelection();
			return true;
		}
		if (EventArgs.Key == EKeys::Escape)
		{
			if (Controller->GetDragMode() != FPCGExSketchEditController::EDragMode::None)
			{
				Controller->CancelDrag();
				return true;
			}
			if (Controller->HasSelection())
			{
				Controller->ClearSelection();
				return true;
			}
		}
	}
	return FEditorViewportClient::InputKey(EventArgs);
}

void FPCGExClusterSketchViewportClient::AddReferencedObjects(FReferenceCollector& Collector)
{
	FEditorViewportClient::AddReferencedObjects(Collector);
	Collector.AddReferencedObject(InputBehaviorSet);
}

FInputRayHit FPCGExClusterSketchViewportClient::IsHitByClick(const FInputDeviceRay& ClickPos)
{
	const FPCGExSketchHit Hit = Controller ? Controller->HitTest(ClickPos.WorldRay) : FPCGExSketchHit();
	if (Hit.IsHit())
	{
		return FInputRayHit(Hit.RayT);
	}
	// Ctrl+click on empty space adds a vertex, so that click must be captured; plain empty clicks fall
	// through to the camera (deselect lives on Escape -- capturing them would eat LMB camera drags).
	if (bCtrlDown)
	{
		return FInputRayHit(PCGExClusterSketchViewportClient::FarHitDepth);
	}
	return FInputRayHit();
}

void FPCGExClusterSketchViewportClient::OnClicked(const FInputDeviceRay& ClickPos)
{
	if (Controller)
	{
		Controller->HandleClick(ClickPos.WorldRay, /*bAdditive*/ bCtrlDown, /*bAddOnEmpty*/ bCtrlDown);
	}
}

FInputRayHit FPCGExClusterSketchViewportClient::CanBeginClickDragSequence(const FInputDeviceRay& PressPos)
{
	const FPCGExSketchHit Hit = Controller ? Controller->HitTest(PressPos.WorldRay) : FPCGExSketchHit();
	return Hit.IsVertex() ? FInputRayHit(Hit.RayT) : FInputRayHit();
}

void FPCGExClusterSketchViewportClient::OnClickPress(const FInputDeviceRay& PressPos)
{
	if (Controller)
	{
		Controller->BeginDrag(PressPos.WorldRay, /*bConnect*/ bShiftDown);
	}
}

void FPCGExClusterSketchViewportClient::OnClickDrag(const FInputDeviceRay& DragPos)
{
	if (Controller)
	{
		Controller->UpdateDrag(DragPos.WorldRay);
	}
}

void FPCGExClusterSketchViewportClient::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	if (Controller)
	{
		Controller->EndDrag(ReleasePos.WorldRay);
	}
}

void FPCGExClusterSketchViewportClient::OnTerminateDragSequence()
{
	if (Controller)
	{
		Controller->CancelDrag();
	}
}

FInputRayHit FPCGExClusterSketchViewportClient::BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos)
{
	// Hover everywhere: the controller decides what (if anything) the ray is over. No other hover
	// consumers exist in this viewport, so the blanket claim starves nothing.
	return FInputRayHit(PCGExClusterSketchViewportClient::FarHitDepth);
}

void FPCGExClusterSketchViewportClient::OnBeginHover(const FInputDeviceRay& DevicePos)
{
	if (Controller)
	{
		Controller->UpdateHover(DevicePos.WorldRay);
	}
}

bool FPCGExClusterSketchViewportClient::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	if (Controller)
	{
		Controller->UpdateHover(DevicePos.WorldRay);
	}
	return true;
}

void FPCGExClusterSketchViewportClient::OnEndHover()
{
	if (Controller)
	{
		Controller->ClearHover();
	}
}

void FPCGExClusterSketchViewportClient::OnUpdateModifierState(const int ModifierID, const bool bIsOn)
{
	if (ModifierID == CtrlModifierID)
	{
		bCtrlDown = bIsOn;
	}
	else if (ModifierID == ShiftModifierID)
	{
		bShiftDown = bIsOn;
	}
}
