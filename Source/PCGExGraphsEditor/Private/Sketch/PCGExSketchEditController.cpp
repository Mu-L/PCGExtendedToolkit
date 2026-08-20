// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExSketchEditController.h"

#include "ScopedTransaction.h"
#include "Sketch/PCGExClusterSketch.h"
#include "Sketch/PCGExClusterSketchStyle.h"

#define LOCTEXT_NAMESPACE "PCGExSketchEditController"

namespace PCGExSketchEditController
{
	// Screen-constant pick radius: world radius grows with distance so elements keep a steady picking
	// footprint. The floor keeps close-up picking from collapsing to a point.
	constexpr double PickTan = 0.0125;
	constexpr double MinPickRadius = 4.0;

	// Vertices win over edges when both are within reach; the factor keeps a vertex pickable at the
	// junction of its own edges.
	constexpr double EdgePickFactor = 0.75;

	// Fallback placement distance along the ray when the work plane is near-parallel to it.
	constexpr double FallbackPlaceDistance = 500.0;

	// A connect drag only latches the vertex under the pointer at a TIGHTER radius than picking:
	// hover-radius latching connected vertices the user never aimed at (screen-space generous AND
	// depth-ambiguous along the ray).
	constexpr double ConnectHoverTightFactor = 0.5;
}

#pragma region FPCGExSketchAssetEditTarget

FPCGExSketchAssetEditTarget::FPCGExSketchAssetEditTarget(UPCGExClusterSketch* InSketch)
	: Sketch(InSketch)
{
}

FPCGExClusterSketchModel* FPCGExSketchAssetEditTarget::GetModel()
{
	UPCGExClusterSketch* Pinned = Sketch.Get();
	return Pinned ? &Pinned->Model : nullptr;
}

const FPCGExClusterSketchModel* FPCGExSketchAssetEditTarget::GetModel() const
{
	const UPCGExClusterSketch* Pinned = Sketch.Get();
	return Pinned ? &Pinned->Model : nullptr;
}

const UPCGExClusterSnapProvider* FPCGExSketchAssetEditTarget::GetSnapProvider() const
{
	const UPCGExClusterSketch* Pinned = Sketch.Get();
	return Pinned ? Pinned->SnapProvider.Get() : nullptr;
}

UObject* FPCGExSketchAssetEditTarget::GetTransactionObject()
{
	return Sketch.Get();
}

void FPCGExSketchAssetEditTarget::NotifyChanged()
{
	// PostEditChange (empty event) refreshes any details panel showing the asset; the viewport itself
	// is realtime and reads the model every frame.
	if (UPCGExClusterSketch* Pinned = Sketch.Get())
	{
		Pinned->PostEditChange();
	}
}

#pragma endregion

#pragma region FPCGExSketchEditController

FPCGExSketchEditController::FPCGExSketchEditController(const TSharedRef<IPCGExSketchEditTarget>& InTarget)
	: Target(InTarget)
{
}

FPCGExSketchEditController::~FPCGExSketchEditController()
{
	CancelDrag();
}

bool FPCGExSketchEditController::GetBasis(FPCGExLatticeBasis& OutBasis) const
{
	const UPCGExClusterSnapProvider* Provider = Target->GetSnapProvider();
	return Provider ? Provider->BuildBasis(OutBasis) : false;
}

FRay FPCGExSketchEditController::ToLocal(const FRay& WorldRay) const
{
	const FTransform WorldToLocal = Target->GetLocalToWorld().Inverse();
	// GetSafeNormal absorbs a scaled host transform; distances then measure in model space, which is
	// what the pick radii and snap operate in.
	return FRay(WorldToLocal.TransformPosition(WorldRay.Origin), WorldToLocal.TransformVector(WorldRay.Direction).GetSafeNormal());
}

double FPCGExSketchEditController::VertexPickFloor()
{
	const UPCGExClusterSketchStyleSettings* Style = UPCGExClusterSketchStyleSettings::Get();
	return Style->EditVertexIdle.Mesh.IsNull() ? 0.0 : Style->EditVertexIdle.Size;
}

double FPCGExSketchEditController::GhostPickFloor()
{
	const UPCGExClusterSketchStyleSettings* Style = UPCGExClusterSketchStyleSettings::Get();
	return Style->EditVertexPhantom.Mesh.IsNull() ? 0.0 : Style->EditVertexPhantom.Size;
}

double FPCGExSketchEditController::EdgePickFloor()
{
	const UPCGExClusterSketchStyleSettings* Style = UPCGExClusterSketchStyleSettings::Get();
	return Style->EditEdge.Mesh.IsNull() ? 0.0 : Style->EditEdge.Size;
}

double FPCGExSketchEditController::PickRadiusAt(const FRay& LocalRay, const FVector& LocalPos, const double InMinWorldRadius) const
{
	const double Dist = FVector::Dist(LocalRay.Origin, LocalPos);
	return FMath::Max3(PCGExSketchEditController::MinPickRadius, InMinWorldRadius, Dist * PCGExSketchEditController::PickTan);
}

FVector FPCGExSketchEditController::VertexLocation(const FPCGExClusterSketchVertex& V, const FPCGExLatticeBasis* Basis) const
{
	return FPCGExClusterSketchModel::ResolvedLocation(V, Basis);
}

FPCGExSketchHit FPCGExSketchEditController::HitTest(const FRay& WorldRay) const
{
	return HitTestLocal(ToLocal(WorldRay));
}

FPCGExSketchHit FPCGExSketchEditController::HitTestLocal(const FRay& LocalRay) const
{
	FPCGExSketchHit Hit;

	const FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model)
	{
		return Hit;
	}

	FPCGExLatticeBasis Basis;
	const FPCGExLatticeBasis* BasisPtr = GetBasis(Basis) ? &Basis : nullptr;

	// Vertices first -- nearest hit along the ray among those within pick radius.
	double BestT = TNumericLimits<double>::Max();
	for (int32 i = 0; i < Model->Vertices.Num(); ++i)
	{
		const FVector Pos = VertexLocation(Model->Vertices[i], BasisPtr);
		const double T = FMath::Max(0.0, FVector::DotProduct(Pos - LocalRay.Origin, LocalRay.Direction));
		const double Dist = FVector::Dist(LocalRay.Origin + LocalRay.Direction * T, Pos);
		if (Dist <= PickRadiusAt(LocalRay, Pos, VertexPickFloor()) && T < BestT)
		{
			BestT = T;
			Hit.Type = FPCGExSketchHit::EType::Vertex;
			Hit.Index = i;
			Hit.RayT = T;
		}
	}
	if (Hit.IsHit())
	{
		return Hit;
	}

	// Ghost crossings next: they sit ON two edges, so they must out-rank them.
	for (int32 c = 0; c < Crossings.Num(); ++c)
	{
		const FVector Pos = Crossings[c].Location;
		const double T = FMath::Max(0.0, FVector::DotProduct(Pos - LocalRay.Origin, LocalRay.Direction));
		const double Dist = FVector::Dist(LocalRay.Origin + LocalRay.Direction * T, Pos);
		if (Dist <= PickRadiusAt(LocalRay, Pos, GhostPickFloor()) && T < BestT)
		{
			BestT = T;
			Hit.Type = FPCGExSketchHit::EType::Crossing;
			Hit.Index = c;
			Hit.RayT = T;
		}
	}
	if (Hit.IsHit())
	{
		return Hit;
	}

	// Edges last, against a long ray segment.
	const FVector RayEnd = LocalRay.Origin + LocalRay.Direction * 1.0e7;
	for (int32 e = 0; e < Model->Edges.Num(); ++e)
	{
		const FPCGExClusterSketchEdge& E = Model->Edges[e];
		if (!Model->Vertices.IsValidIndex(E.A) || !Model->Vertices.IsValidIndex(E.B))
		{
			continue;
		}
		const FVector A = VertexLocation(Model->Vertices[E.A], BasisPtr);
		const FVector B = VertexLocation(Model->Vertices[E.B], BasisPtr);
		FVector OnRay, OnSegment;
		FMath::SegmentDistToSegmentSafe(LocalRay.Origin, RayEnd, A, B, OnRay, OnSegment);
		const double T = FVector::Dist(LocalRay.Origin, OnRay);
		const double EdgeReach = FMath::Max(PickRadiusAt(LocalRay, OnSegment) * PCGExSketchEditController::EdgePickFactor, EdgePickFloor());
		if (FVector::Dist(OnRay, OnSegment) <= EdgeReach && T < BestT)
		{
			BestT = T;
			Hit.Type = FPCGExSketchHit::EType::Edge;
			Hit.Index = e;
			Hit.RayT = T;
		}
	}
	return Hit;
}

void FPCGExSketchEditController::UpdateHover(const FRay& WorldRay)
{
	// Refreshed here so ghosts stay honest after edits the controller never saw (details panel, undo).
	// O(E^2) per mouse move, on models that are small by design.
	RefreshCrossings();

	// Connect-drag targeting lives in UpdateDrag, not here -- hover alone is too loose to pick a
	// link target from.
	Hover = HitTestLocal(ToLocal(WorldRay));
}

void FPCGExSketchEditController::ClearHover()
{
	Hover = FPCGExSketchHit();
}

void FPCGExSketchEditController::HandleClick(const FRay& WorldRay, const bool bAdditive, const bool bAddOnEmpty)
{
	DropInvalidIndices();

	const FPCGExSketchHit Hit = HitTest(WorldRay);

	if (!Hit.IsHit())
	{
		if (bAddOnEmpty)
		{
			AddVertexAtRay(WorldRay);
		}
		else if (!bAdditive)
		{
			ClearSelection();
			NotifyModelChanged();
		}
		return;
	}

	if (Hit.IsCrossing())
	{
		// The ghost is an OFFER: clicking it commits the cut. Nothing else materializes crossings.
		MaterializeCrossingAtRay(WorldRay);
		return;
	}

	TSet<int32>& Set = Hit.IsVertex() ? SelectedVertices : SelectedEdges;
	if (bAdditive)
	{
		if (Set.Contains(Hit.Index))
		{
			Set.Remove(Hit.Index);
		}
		else
		{
			Set.Add(Hit.Index);
		}
	}
	else
	{
		ClearSelection();
		Set.Add(Hit.Index);
	}
	NotifyModelChanged();
}

bool FPCGExSketchEditController::CanBeginDrag(const FRay& WorldRay) const
{
	return HitTest(WorldRay).IsVertex();
}

void FPCGExSketchEditController::BeginDrag(const FRay& WorldRay, const bool bConnect)
{
	CancelDrag();
	DropInvalidIndices();

	const FRay LocalRay = ToLocal(WorldRay);
	const FPCGExSketchHit Hit = HitTestLocal(LocalRay);
	if (!Hit.IsVertex())
	{
		return;
	}

	FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model)
	{
		return;
	}

	bDragHasBasis = GetBasis(DragBasis);
	const FPCGExLatticeBasis* BasisPtr = bDragHasBasis ? &DragBasis : nullptr;

	DragMode = bConnect ? EDragMode::Connect : EDragMode::Move;
	DragVertexIndex = Hit.Index;
	DragTargetVertexIndex = INDEX_NONE;
	MergeCandidateVertex = INDEX_NONE;
	DragPlaneAnchor = VertexLocation(Model->Vertices[Hit.Index], BasisPtr);
	DragPreviewLocal = DragPlaneAnchor;

	if (DragMode == EDragMode::Move)
	{
		// Screen-plane translate: the plane through the grab point facing the viewer. One transaction
		// spans the whole drag; Modify() snapshots the pre-drag state exactly once.
		DragPlaneNormal = -LocalRay.Direction;
		ActiveTransaction = MakeUnique<FScopedTransaction>(LOCTEXT("MoveVertex", "Move Sketch Vertex"));
		if (UObject* TransactionObject = Target->GetTransactionObject())
		{
			TransactionObject->Modify();
		}
		if (!SelectedVertices.Contains(DragVertexIndex))
		{
			ClearSelection();
			SelectedVertices.Add(DragVertexIndex);
		}
	}
	else
	{
		// Connect previews only; the transaction opens at release, when something actually mutates.
		FVector Unused[3];
		DragPlaneNormal = (bDragHasBasis && DragBasis.NumAxes == 2 && DragBasis.GetComplementBasis(Unused) > 0) ? Unused[0] : FVector::UpVector;
	}
}

void FPCGExSketchEditController::UpdateDrag(const FRay& WorldRay)
{
	if (DragMode == EDragMode::None)
	{
		return;
	}

	const FRay LocalRay = ToLocal(WorldRay);
	UpdateHover(WorldRay);

	// Ray onto the drag plane; near-parallel rays keep the previous preview instead of shooting off.
	const double Denominator = FVector::DotProduct(LocalRay.Direction, DragPlaneNormal);
	if (FMath::Abs(Denominator) > 1.0e-4)
	{
		const double T = FVector::DotProduct(DragPlaneAnchor - LocalRay.Origin, DragPlaneNormal) / Denominator;
		if (T > 0.0)
		{
			DragPreviewLocal = LocalRay.Origin + LocalRay.Direction * T;
		}
	}

	const FPCGExLatticeBasis* BasisPtr = bDragHasBasis ? &DragBasis : nullptr;
	if (bSnapEnabled && BasisPtr)
	{
		DragPreviewLocal = BasisPtr->CoordToWorld(BasisPtr->SnapWorldToCoord(DragPreviewLocal));
	}

	if (DragMode == EDragMode::Move)
	{
		FPCGExClusterSketchModel* Model = Target->GetModel();
		if (!Model || !Model->Vertices.IsValidIndex(DragVertexIndex))
		{
			return;
		}
		FPCGExClusterSketchVertex& V = Model->Vertices[DragVertexIndex];
		if (V.bLatticeBound && BasisPtr)
		{
			// Coords stay authoritative for bound vertices: snap regardless of the toggle, so a bound
			// vertex can never be dragged off-lattice while a basis exists. Preserving: dragging in a
			// rank-collapsed basis edits the spanned components only, the stash survives.
			V.LatticeCoord = BasisPtr->SnapWorldToCoordPreserving(DragPreviewLocal, V.LatticeCoord);
			V.Transform.SetLocation(BasisPtr->CoordToWorld(V.LatticeCoord));
			DragPreviewLocal = V.Transform.GetLocation();
		}
		else
		{
			V.Transform.SetLocation(DragPreviewLocal);
		}

		// A drag moves geometry EVERY FRAME without a completed-operation notify (which would refresh
		// details panels per mouse move). Hosts that build their visuals still have to know.
		++ModelRevision;

		// Dropping onto another vertex MERGES into it (clusters cannot hold collocated vertices).
		// Layer-aware: under a rank-collapsed basis, prefer the stack member on the dragged vertex's
		// own hidden layer.
		MergeCandidateVertex = FindNearbyVertex(LocalRay, DragPreviewLocal, DragVertexIndex, BasisPtr, V.bLatticeBound ? &V.LatticeCoord : nullptr);
	}
	else if (DragMode == EDragMode::Connect)
	{
		const FPCGExClusterSketchModel* Model = Target->GetModel();
		const FIntVector* SourceLayer = nullptr;
		if (Model && Model->Vertices.IsValidIndex(DragVertexIndex) && Model->Vertices[DragVertexIndex].bLatticeBound)
		{
			SourceLayer = &Model->Vertices[DragVertexIndex].LatticeCoord;
		}

		// Target acquisition, strictest first:
		// 1. the vertex the SNAPPED release point resolves onto (cell-exact -- an extrude can never
		//    stack a twin on an occupied node; layer-aware, so a projection stack yields the member on
		//    the SOURCE's hidden layer, not an arbitrary one);
		// 2. optionally, the vertex under the POINTER at a tightened radius (deliberate aim across the
		//    work plane, e.g. linking two distant vertices in 3D).
		DragTargetVertexIndex = FindNearbyVertex(LocalRay, DragPreviewLocal, DragVertexIndex, BasisPtr, SourceLayer);
		if (DragTargetVertexIndex == INDEX_NONE && bConnectToHover && Hover.IsVertex() && Hover.Index != DragVertexIndex)
		{
			if (Model && Model->Vertices.IsValidIndex(Hover.Index))
			{
				const FVector Pos = VertexLocation(Model->Vertices[Hover.Index], BasisPtr);
				const double T = FMath::Max(0.0, FVector::DotProduct(Pos - LocalRay.Origin, LocalRay.Direction));
				const double RayDist = FVector::Dist(LocalRay.Origin + LocalRay.Direction * T, Pos);
				if (RayDist <= PickRadiusAt(LocalRay, Pos, VertexPickFloor()) * PCGExSketchEditController::ConnectHoverTightFactor)
				{
					DragTargetVertexIndex = Hover.Index;
				}
			}
		}
	}
}

void FPCGExSketchEditController::EndDrag(const FRay& WorldRay)
{
	if (DragMode == EDragMode::None)
	{
		return;
	}

	UpdateDrag(WorldRay);

	if (DragMode == EDragMode::Move)
	{
		// Drop-on-vertex merges into it, still inside the drag's open transaction so the whole
		// move+merge is one undo step. Edges re-anchor onto the survivor; the absorbed vertex goes.
		FPCGExClusterSketchModel* Model = Target->GetModel();
		int32 FinalVertex = DragVertexIndex;
		if (MergeCandidateVertex != INDEX_NONE && Model)
		{
			const int32 Survivor = Model->MergeVertices(DragVertexIndex, MergeCandidateVertex);
			if (Survivor != INDEX_NONE)
			{
				FinalVertex = Survivor;
				ClearSelection();
				SelectedVertices.Add(Survivor);
			}
		}
		// Hand-placed now: dragging a tool-inserted vertex adopts it.
		if (Model)
		{
			Model->MarkVertexAuthored(FinalVertex);
			// Then re-enforce edge/vertex separation around the landing spot: a vertex dropped onto an
			// edge splits it, and merge-retargeted edges dissolve through the vertices they now cross
			// (collinear A-B-C never keeps A-C -- it splits and dedups into the existing chain).
			Model->EnforceSeparationAroundVertex(FinalVertex, bDragHasBasis ? &DragBasis : nullptr);
		}
		EndTransaction();
		NotifyModelChanged();
	}
	else // Connect
	{
		FPCGExClusterSketchModel* Model = Target->GetModel();
		const int32 Source = DragVertexIndex;
		const int32 ExistingTarget = DragTargetVertexIndex;
		const FVector PlacePoint = DragPreviewLocal;
		const bool bHasBasis = bDragHasBasis;
		const FPCGExLatticeBasis Basis = DragBasis;

		DragMode = EDragMode::None;
		DragVertexIndex = INDEX_NONE;
		DragTargetVertexIndex = INDEX_NONE;

		if (Model && Model->Vertices.IsValidIndex(Source))
		{
			int32 FarVertex = INDEX_NONE;
			if (ExistingTarget != INDEX_NONE && Model->Vertices.IsValidIndex(ExistingTarget))
			{
				const FScopedTransaction Transaction(LOCTEXT("ConnectVertices", "Connect Sketch Vertices"));
				if (UObject* TransactionObject = Target->GetTransactionObject())
				{
					TransactionObject->Modify();
				}
				Model->Connect(Source, ExistingTarget);
				// Deliberately wired by hand -- both ends are now authored.
				Model->MarkVertexAuthored(Source);
				Model->MarkVertexAuthored(ExistingTarget);
				// A deliberate long link across collinear vertices splits into the chain (A-C never
				// survives when B sits on it).
				Model->EnforceSeparationAroundVertex(ExistingTarget, bHasBasis ? &Basis : nullptr);
				FarVertex = ExistingTarget;
			}
			else
			{
				// The drafting gesture: release over nothing extrudes a new (snapped) vertex + edge.
				const FScopedTransaction Transaction(LOCTEXT("ExtrudeVertex", "Extrude Sketch Vertex"));
				if (UObject* TransactionObject = Target->GetTransactionObject())
				{
					TransactionObject->Modify();
				}
				if (bSnapEnabled && bHasBasis)
				{
					// Inherit the source's unspanned components: extruding in a rank-collapsed basis
					// stays on the source's hidden layer instead of dropping the new vertex onto layer 0
					// (which is what wired edges across unrelated layers).
					const FPCGExClusterSketchVertex& SourceVertex = Model->Vertices[Source];
					const FIntVector Coord = SourceVertex.bLatticeBound
						                         ? Basis.SnapWorldToCoordPreserving(PlacePoint, SourceVertex.LatticeCoord)
						                         : Basis.SnapWorldToCoord(PlacePoint);
					FarVertex = Model->AddLatticeVertex(Coord, Basis);
				}
				else
				{
					FarVertex = Model->AddVertex(FTransform(PlacePoint));
				}
				Model->Connect(Source, FarVertex);
				Model->MarkVertexAuthored(Source); // extruding FROM a tool-inserted vertex adopts it
				Model->EnforceSeparationAroundVertex(FarVertex, bHasBasis ? &Basis : nullptr);
			}

			if (FarVertex != INDEX_NONE)
			{
				// Selecting the far end chains the gesture into a walk.
				ClearSelection();
				SelectedVertices.Add(FarVertex);
			}
			NotifyModelChanged();
		}
	}

	DragMode = EDragMode::None;
	DragVertexIndex = INDEX_NONE;
	DragTargetVertexIndex = INDEX_NONE;
	MergeCandidateVertex = INDEX_NONE;
}

void FPCGExSketchEditController::CancelDrag()
{
	bool bRolledBack = false;
	if (ActiveTransaction)
	{
		// A move already mutated under the open transaction -- cancelling rolls the object back.
		ActiveTransaction->Cancel();
		ActiveTransaction.Reset();
		bRolledBack = true;
	}
	DragMode = EDragMode::None;
	DragVertexIndex = INDEX_NONE;
	DragTargetVertexIndex = INDEX_NONE;
	MergeCandidateVertex = INDEX_NONE;

	// A rollback CHANGES GEOMETRY as surely as the drag did: crossings were computed against the
	// dragged shape, and a host whose visuals are built would otherwise keep showing where the vertex
	// never ended up.
	if (bRolledBack)
	{
		NotifyModelChanged();
	}
}

void FPCGExSketchEditController::DeleteSelection()
{
	DropInvalidIndices();

	FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model || (!HasSelection()))
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("DeleteSelection", "Delete Sketch Selection"));
	if (UObject* TransactionObject = Target->GetTransactionObject())
	{
		TransactionObject->Modify();
	}

	// Edges first (their indices die with vertex removal); both descending so indices stay valid.
	TArray<int32> EdgeIndices = SelectedEdges.Array();
	EdgeIndices.Sort([](const int32 A, const int32 B) { return A > B; });
	for (const int32 e : EdgeIndices)
	{
		if (Model->Edges.IsValidIndex(e))
		{
			Model->Disconnect(Model->Edges[e].A, Model->Edges[e].B);
		}
	}

	TArray<int32> VertexIndices = SelectedVertices.Array();
	VertexIndices.Sort([](const int32 A, const int32 B) { return A > B; });
	for (const int32 v : VertexIndices)
	{
		Model->RemoveVertex(v);
	}

	// Tool residue never outlives the geometry that justified it.
	Model->RemoveOrphanSideEffectVertices();

	ClearSelection();
	ClearHover();
	NotifyModelChanged();
}

void FPCGExSketchEditController::SelectAll()
{
	const FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model)
	{
		return;
	}
	SelectedVertices.Reset();
	SelectedEdges.Reset();
	for (int32 i = 0; i < Model->Vertices.Num(); ++i)
	{
		SelectedVertices.Add(i);
	}
	for (int32 e = 0; e < Model->Edges.Num(); ++e)
	{
		SelectedEdges.Add(e);
	}
	NotifyModelChanged();
}

void FPCGExSketchEditController::ClearSelection()
{
	SelectedVertices.Reset();
	SelectedEdges.Reset();
}

bool FPCGExSketchEditController::DeleteAtRay(const FRay& WorldRay)
{
	DropInvalidIndices();

	FPCGExClusterSketchModel* Model = Target->GetModel();
	const FPCGExSketchHit Hit = HitTest(WorldRay);
	if (!Model || !Hit.IsHit())
	{
		return false;
	}

	if (Hit.IsVertex())
	{
		if (!Model->Vertices.IsValidIndex(Hit.Index))
		{
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("DeleteVertex", "Delete Sketch Vertex"));
		if (UObject* TransactionObject = Target->GetTransactionObject())
		{
			TransactionObject->Modify();
		}
		Model->RemoveVertex(Hit.Index);
		Model->RemoveOrphanSideEffectVertices();
	}
	else
	{
		if (!Model->Edges.IsValidIndex(Hit.Index))
		{
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("DeleteEdge", "Delete Sketch Edge"));
		if (UObject* TransactionObject = Target->GetTransactionObject())
		{
			TransactionObject->Modify();
		}
		const FPCGExClusterSketchEdge Edge = Model->Edges[Hit.Index];
		Model->Disconnect(Edge.A, Edge.B);
		Model->RemoveOrphanSideEffectVertices();
	}

	ClearSelection();
	ClearHover();
	NotifyModelChanged();
	return true;
}

int32 FPCGExSketchEditController::AddVertexAtRay(const FRay& WorldRay)
{
	FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model)
	{
		return INDEX_NONE;
	}

	const FRay LocalRay = ToLocal(WorldRay);

	FPCGExLatticeBasis Basis;
	const FPCGExLatticeBasis* BasisPtr = GetBasis(Basis) ? &Basis : nullptr;

	// Anchor: last selected vertex if any, else the lattice origin, else the model's first vertex.
	// A bound anchor also donates its hidden layer, so adding in a rank-collapsed basis stays on it.
	FVector Anchor = BasisPtr ? BasisPtr->Origin : FVector::ZeroVector;
	FIntVector AnchorCoord = FIntVector::ZeroValue;
	const FIntVector* AnchorLayer = nullptr;
	for (const int32 Selected : SelectedVertices)
	{
		if (Model->Vertices.IsValidIndex(Selected))
		{
			const FPCGExClusterSketchVertex& AnchorVertex = Model->Vertices[Selected];
			Anchor = VertexLocation(AnchorVertex, BasisPtr);
			if (AnchorVertex.bLatticeBound)
			{
				AnchorCoord = AnchorVertex.LatticeCoord;
				AnchorLayer = &AnchorCoord;
			}
			break;
		}
	}

	FVector PlacePoint = RayPointOnWorkPlane(LocalRay, Anchor, BasisPtr);
	if (bSnapEnabled && BasisPtr)
	{
		PlacePoint = BasisPtr->CoordToWorld(BasisPtr->SnapWorldToCoord(PlacePoint));
	}

	// An occupied spot is reused, never twinned -- clusters cannot hold collocated vertices.
	const int32 Existing = FindNearbyVertex(LocalRay, PlacePoint, INDEX_NONE, BasisPtr, AnchorLayer);
	if (Existing != INDEX_NONE)
	{
		ClearSelection();
		SelectedVertices.Add(Existing);
		NotifyModelChanged();
		return Existing;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddVertex", "Add Sketch Vertex"));
	if (UObject* TransactionObject = Target->GetTransactionObject())
	{
		TransactionObject->Modify();
	}

	int32 NewVertex;
	if (bSnapEnabled && BasisPtr)
	{
		const FIntVector Coord = AnchorLayer
			                         ? BasisPtr->SnapWorldToCoordPreserving(PlacePoint, AnchorCoord)
			                         : BasisPtr->SnapWorldToCoord(PlacePoint);
		NewVertex = Model->AddLatticeVertex(Coord, *BasisPtr);
	}
	else
	{
		NewVertex = Model->AddVertex(FTransform(PlacePoint));
	}

	// A vertex landing on an edge SPLITS it -- edges never pass through vertices.
	Model->EnforceSeparationAroundVertex(NewVertex, BasisPtr);

	ClearSelection();
	SelectedVertices.Add(NewVertex);
	NotifyModelChanged();
	return NewVertex;
}

int32 FPCGExSketchEditController::FindNearbyVertex(const FRay& LocalRay, const FVector& LocalPoint, const int32 IgnoreVertex, const FPCGExLatticeBasis* Basis, const FIntVector* LayerRef) const
{
	const FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model)
	{
		return INDEX_NONE;
	}

	// Pick radius capped below half a cell: on a dense lattice the screen-space radius could otherwise
	// reach a NEIGHBORING node and merge a drop that landed on a different cell.
	double RadiusCap = TNumericLimits<double>::Max();
	if (Basis && Basis->NumAxes > 0)
	{
		double SmallestAxis = TNumericLimits<double>::Max();
		for (int32 k = 0; k < Basis->NumAxes; ++k)
		{
			SmallestAxis = FMath::Min(SmallestAxis, Basis->AxisVecs[k].Size());
		}
		RadiusCap = SmallestAxis * 0.45;
	}

	const int32 NumAxes = Basis ? Basis->NumAxes : 3;
	auto SharesLayer = [&](const FPCGExClusterSketchVertex& V) -> bool
	{
		if (!LayerRef || !V.bLatticeBound)
		{
			return false;
		}
		if (NumAxes <= 2 && V.LatticeCoord.Z != LayerRef->Z)
		{
			return false;
		}
		if (NumAxes <= 1 && V.LatticeCoord.Y != LayerRef->Y)
		{
			return false;
		}
		return true;
	};

	int32 Best = INDEX_NONE;
	double BestDist = TNumericLimits<double>::Max();
	int32 BestSameLayer = INDEX_NONE;
	double BestSameLayerDist = TNumericLimits<double>::Max();
	for (int32 i = 0; i < Model->Vertices.Num(); ++i)
	{
		if (i == IgnoreVertex)
		{
			continue;
		}
		const FPCGExClusterSketchVertex& V = Model->Vertices[i];
		const FVector Pos = VertexLocation(V, Basis);
		const double Dist = FVector::Dist(Pos, LocalPoint);
		if (Dist > FMath::Min(PickRadiusAt(LocalRay, Pos, VertexPickFloor()), RadiusCap))
		{
			continue;
		}
		if (Dist < BestDist)
		{
			BestDist = Dist;
			Best = i;
		}
		if (SharesLayer(V) && Dist < BestSameLayerDist)
		{
			BestSameLayerDist = Dist;
			BestSameLayer = i;
		}
	}
	// The gesture source's own layer wins over a merely-nearest stack member.
	return BestSameLayer != INDEX_NONE ? BestSameLayer : Best;
}

FVector FPCGExSketchEditController::RayPointOnWorkPlane(const FRay& LocalRay, const FVector& InAnchor, const FPCGExLatticeBasis* Basis) const
{
	FVector Normal = FVector::UpVector;
	if (Basis && Basis->NumAxes == 2)
	{
		FVector Complement[3];
		if (Basis->GetComplementBasis(Complement) > 0)
		{
			Normal = Complement[0];
		}
	}

	const double Denominator = FVector::DotProduct(LocalRay.Direction, Normal);
	if (FMath::Abs(Denominator) > 1.0e-4)
	{
		const double T = FVector::DotProduct(InAnchor - LocalRay.Origin, Normal) / Denominator;
		if (T > 0.0)
		{
			return LocalRay.Origin + LocalRay.Direction * T;
		}
	}
	return LocalRay.Origin + LocalRay.Direction * PCGExSketchEditController::FallbackPlaceDistance;
}

void FPCGExSketchEditController::DropInvalidIndices()
{
	const FPCGExClusterSketchModel* Model = Target->GetModel();
	const int32 NumVtx = Model ? Model->Vertices.Num() : 0;
	const int32 NumEdges = Model ? Model->Edges.Num() : 0;
	for (auto It = SelectedVertices.CreateIterator(); It; ++It)
	{
		if (*It < 0 || *It >= NumVtx)
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = SelectedEdges.CreateIterator(); It; ++It)
	{
		if (*It < 0 || *It >= NumEdges)
		{
			It.RemoveCurrent();
		}
	}
}

void FPCGExSketchEditController::EndTransaction()
{
	ActiveTransaction.Reset();
}


void FPCGExSketchEditController::NotifyModelChanged()
{
	++ModelRevision;
	RefreshCrossings();
	Target->NotifyChanged();
}

void FPCGExSketchEditController::RefreshCrossings()
{
	const FPCGExClusterSketchModel* Model = Target->GetModel();
	if (!Model)
	{
		Crossings.Reset();
		return;
	}
	FPCGExLatticeBasis Basis;
	Model->FindEdgeCrossings(Crossings, GetBasis(Basis) ? &Basis : nullptr);
}

bool FPCGExSketchEditController::MaterializeCrossingAtRay(const FRay& WorldRay)
{
	FPCGExClusterSketchModel* Model = Target->GetModel();
	const FPCGExSketchHit Hit = HitTest(WorldRay);
	if (!Model || !Hit.IsCrossing() || !Crossings.IsValidIndex(Hit.Index))
	{
		return false;
	}

	const FPCGExClusterSketchCrossing Crossing = Crossings[Hit.Index];

	const FScopedTransaction Transaction(LOCTEXT("MaterializeCrossing", "Materialize Sketch Crossing"));
	if (UObject* TransactionObject = Target->GetTransactionObject())
	{
		TransactionObject->Modify();
	}

	FPCGExLatticeBasis Basis;
	const int32 NewVertex = Model->MaterializeCrossing(Crossing.EdgeA, Crossing.EdgeB, Crossing.Location, GetBasis(Basis) ? &Basis : nullptr);
	if (NewVertex == INDEX_NONE)
	{
		return false;
	}

	ClearSelection();
	SelectedVertices.Add(NewVertex);
	ClearHover();
	NotifyModelChanged();
	return true;
}
#pragma endregion

#undef LOCTEXT_NAMESPACE
