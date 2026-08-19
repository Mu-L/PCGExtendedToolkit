// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Lattice/PCGExLatticeBasis.h"
#include "Sketch/PCGExClusterSketchModel.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FScopedTransaction;
class UPCGExClusterSketch;
class UPCGExClusterSnapProvider;

/**
 * What the sketch edit controller edits. One implementation per authoring host: the asset (standalone
 * editor, below), a component or a cage later. The controller performs EVERY mutation through the
 * model's mutation API against this seam, so hosts share selection/gesture/undo logic wholesale.
 */
class PCGEXGRAPHSEDITOR_API IPCGExSketchEditTarget
{
public:
	virtual ~IPCGExSketchEditTarget() = default;

	virtual FPCGExClusterSketchModel* GetModel() = 0;
	virtual const FPCGExClusterSketchModel* GetModel() const = 0;
	virtual const UPCGExClusterSnapProvider* GetSnapProvider() const = 0;

	/** The object Modify() is called on inside every transaction (the asset / the component). */
	virtual UObject* GetTransactionObject() = 0;

	/** Model space -> world. Identity for the asset editor; a component host returns its transform. */
	virtual FTransform GetLocalToWorld() const = 0;

	/** Fired after every completed operation so the host can refresh (viewport invalidate, details). */
	virtual void NotifyChanged() = 0;
};

/** The standalone editor's target: edits a UPCGExClusterSketch asset in place, identity transform. */
class PCGEXGRAPHSEDITOR_API FPCGExSketchAssetEditTarget final : public IPCGExSketchEditTarget
{
public:
	explicit FPCGExSketchAssetEditTarget(UPCGExClusterSketch* InSketch);

	virtual FPCGExClusterSketchModel* GetModel() override;
	virtual const FPCGExClusterSketchModel* GetModel() const override;
	virtual const UPCGExClusterSnapProvider* GetSnapProvider() const override;
	virtual UObject* GetTransactionObject() override;
	virtual FTransform GetLocalToWorld() const override { return FTransform::Identity; }
	virtual void NotifyChanged() override;

private:
	TWeakObjectPtr<UPCGExClusterSketch> Sketch;
};

/** What a ray hit in the sketch. */
struct PCGEXGRAPHSEDITOR_API FPCGExSketchHit
{
	enum class EType : uint8 { None, Vertex, Edge };

	EType Type = EType::None;
	int32 Index = INDEX_NONE;
	/** Ray parameter of the hit (world units along the ray) -- the ITF hit depth. */
	double RayT = 0.0;

	bool IsHit() const { return Type != EType::None; }
	bool IsVertex() const { return Type == EType::Vertex; }
};

/**
 * Host-agnostic sketch authoring: selection, hover, click/drag gestures, add/move/connect/disconnect/
 * delete, snapping -- everything except input plumbing and drawing. Hosts feed it WORLD rays (from ITF
 * behaviors or anything else) and render its state via FPCGExSketchDrawHelper; every mutation is one
 * scoped transaction on the target's transaction object.
 *
 * Gestures (the host maps modifiers to the two flags):
 *  - Click: select (bAdditive toggles); click on nothing clears; bAddOnEmpty + nothing = add a vertex.
 *  - Drag from a vertex: move it (snapped when a basis is active).
 *  - Connect-drag from a vertex (bConnect): release on a vertex links them; release on nothing adds a
 *    snapped vertex there AND links it (the drafting gesture); the far vertex becomes the selection.
 */
class PCGEXGRAPHSEDITOR_API FPCGExSketchEditController
{
public:
	enum class EDragMode : uint8 { None, Move, Connect };

	explicit FPCGExSketchEditController(const TSharedRef<IPCGExSketchEditTarget>& InTarget);
	~FPCGExSketchEditController();

	//~ Queries (all rays in WORLD space)
	FPCGExSketchHit HitTest(const FRay& WorldRay) const;

	//~ Hover
	void UpdateHover(const FRay& WorldRay);
	void ClearHover();

	//~ Click
	void HandleClick(const FRay& WorldRay, bool bAdditive, bool bAddOnEmpty);

	//~ Drag
	bool CanBeginDrag(const FRay& WorldRay) const;
	void BeginDrag(const FRay& WorldRay, bool bConnect);
	void UpdateDrag(const FRay& WorldRay);
	void EndDrag(const FRay& WorldRay);
	void CancelDrag();

	//~ Operations
	void DeleteSelection();
	void SelectAll();
	void ClearSelection();
	int32 AddVertexAtRay(const FRay& WorldRay);

	//~ Snapping
	bool IsSnapEnabled() const { return bSnapEnabled; }
	void SetSnapEnabled(const bool bEnabled) { bSnapEnabled = bEnabled; }
	/** Basis from the target's provider; false when there is none. Rebuilt on demand -- never cached. */
	bool GetBasis(FPCGExLatticeBasis& OutBasis) const;

	//~ Draw-state accessors (consumed by FPCGExSketchDrawHelper; indices may be stale after external
	//~ edits -- consumers must IsValidIndex-guard, the controller sanitizes on its own operations)
	const IPCGExSketchEditTarget& GetTarget() const { return Target.Get(); }
	const TSet<int32>& GetSelectedVertices() const { return SelectedVertices; }
	const TSet<int32>& GetSelectedEdges() const { return SelectedEdges; }
	const FPCGExSketchHit& GetHover() const { return Hover; }
	EDragMode GetDragMode() const { return DragMode; }
	int32 GetDragVertex() const { return DragVertexIndex; }
	int32 GetDragTargetVertex() const { return DragTargetVertexIndex; }
	/** Current drag point in MODEL space (snap already applied) -- the move ghost / connect line end. */
	const FVector& GetDragPreviewLocal() const { return DragPreviewLocal; }

	bool HasSelection() const { return !SelectedVertices.IsEmpty() || !SelectedEdges.IsEmpty(); }

private:
	//~ Internals (model space)
	FRay ToLocal(const FRay& WorldRay) const;
	FPCGExSketchHit HitTestLocal(const FRay& LocalRay) const;
	double PickRadiusAt(const FRay& LocalRay, const FVector& LocalPos) const;
	FVector VertexLocation(const FPCGExClusterSketchVertex& V, const FPCGExLatticeBasis* Basis) const;
	/** Ray point on the gesture plane: through InAnchor, lattice-plane normal for a 2-axis basis, else
	 *  Z-up; falls back to a fixed distance along the ray when near-parallel. */
	FVector RayPointOnWorkPlane(const FRay& LocalRay, const FVector& InAnchor, const FPCGExLatticeBasis* Basis) const;
	void DropInvalidIndices();
	void EndTransaction();

	TSharedRef<IPCGExSketchEditTarget> Target;

	TSet<int32> SelectedVertices;
	TSet<int32> SelectedEdges;
	FPCGExSketchHit Hover;

	EDragMode DragMode = EDragMode::None;
	int32 DragVertexIndex = INDEX_NONE;
	int32 DragTargetVertexIndex = INDEX_NONE;
	FVector DragPreviewLocal = FVector::ZeroVector;
	FVector DragPlaneAnchor = FVector::ZeroVector;
	FVector DragPlaneNormal = FVector::UpVector;
	/** Basis snapshot for the duration of one drag, so mid-drag provider edits can't tear it. */
	FPCGExLatticeBasis DragBasis;
	bool bDragHasBasis = false;

	TUniquePtr<FScopedTransaction> ActiveTransaction;

	bool bSnapEnabled = true;
};
