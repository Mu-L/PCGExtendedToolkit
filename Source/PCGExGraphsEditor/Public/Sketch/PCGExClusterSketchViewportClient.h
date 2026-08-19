// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "BaseBehaviors/BehaviorTargetInterfaces.h"
#include "InputBehaviorSet.h"

class FPCGExSketchEditController;

/**
 * Preview-scene viewport client for the Cluster Sketch editor. Hosts the ITF input behaviors DIRECTLY
 * (no UEdMode, no tools): the mode manager's always-live tools context routes input here, and every
 * gesture delegates to the shared FPCGExSketchEditController -- the client owns zero editing logic, so
 * a level-viewport host can drive the same controller from its own behavior sources.
 *
 * Input map: LMB click = select (Ctrl toggles / adds a vertex on empty space); LMB drag on a vertex =
 * move; Shift+drag from a vertex = connect (release on empty extrudes); Delete = delete selection;
 * Escape = cancel drag / clear selection. Camera navigation stays fully stock -- empty-space plain
 * clicks/drags are deliberately NOT captured.
 */
class PCGEXGRAPHSEDITOR_API FPCGExClusterSketchViewportClient final
	: public FEditorViewportClient,
	  public IInputBehaviorSource,
	  public IClickBehaviorTarget,
	  public IClickDragBehaviorTarget,
	  public IHoverBehaviorTarget
{
public:
	FPCGExClusterSketchViewportClient(FEditorModeTools* InModeTools, FPreviewScene* InPreviewScene, const TSharedPtr<FPCGExSketchEditController>& InController);
	virtual ~FPCGExClusterSketchViewportClient() override;

	//~ FEditorViewportClient
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;

	//~ IInputBehaviorSource
	virtual const UInputBehaviorSet* GetInputBehaviors() const override { return InputBehaviorSet; }

	//~ IClickBehaviorTarget
	virtual FInputRayHit IsHitByClick(const FInputDeviceRay& ClickPos) override;
	virtual void OnClicked(const FInputDeviceRay& ClickPos) override;

	//~ IClickDragBehaviorTarget
	virtual FInputRayHit CanBeginClickDragSequence(const FInputDeviceRay& PressPos) override;
	virtual void OnClickPress(const FInputDeviceRay& PressPos) override;
	virtual void OnClickDrag(const FInputDeviceRay& DragPos) override;
	virtual void OnClickRelease(const FInputDeviceRay& ReleasePos) override;
	virtual void OnTerminateDragSequence() override;

	//~ IHoverBehaviorTarget
	virtual FInputRayHit BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos) override;
	virtual void OnBeginHover(const FInputDeviceRay& DevicePos) override;
	virtual bool OnUpdateHover(const FInputDeviceRay& DevicePos) override;
	virtual void OnEndHover() override;

	//~ IModifierToggleBehaviorTarget (one override serves every inherited interface copy)
	virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;

private:
	static constexpr int CtrlModifierID = 1;
	static constexpr int ShiftModifierID = 2;

	TSharedPtr<FPCGExSketchEditController> Controller;
	FEditorModeTools* OwnerModeTools = nullptr;
	TObjectPtr<UInputBehaviorSet> InputBehaviorSet;

	bool bCtrlDown = false;
	bool bShiftDown = false;
};
