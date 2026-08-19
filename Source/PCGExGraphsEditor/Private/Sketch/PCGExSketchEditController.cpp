// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExSketchEditController.h"

#include "ScopedTransaction.h"
#include "Sketch/PCGExClusterSketch.h"

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

double FPCGExSketchEditController::PickRadiusAt(const FRay& LocalRay, const FVector& LocalPos) const
{
	const double Dist = FVector::Dist(LocalRay.Origin, LocalPos);
	return FMath::Max(PCGExSketchEditController::MinPickRadius, Dist * PCGExSketchEditController::PickTan);
}

FVector FPCGExSketchEditController::VertexLocation(const FPCGExClusterSketchVertex& V, const FPCGExLatticeBasis* Basis) const
{
	// Mirror of the print rule: a bound vertex's location derives from its coord whenever a basis exists.
	if (V.bLatticeBound && Basis)
	{
		return Basis->CoordToWorld(V.LatticeCoord);
	}
	return V.Transform.GetLocation();
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
		if (Dist <= PickRadiusAt(LocalRay, Pos) && T < BestT)
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

	// Edges second, against a long ray segment.
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
		if (FVector::Dist(OnRay, OnSegment) <= PickRadiusAt(LocalRay, OnSegment) * PCGExSketchEditController::EdgePickFactor && T < BestT)
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
	const FRay LocalRay = ToLocal(WorldRay);
	Hover = HitTestLocal(LocalRay);

	// During a connect drag, hover doubles as the link target (any vertex but the source).
	if (DragMode == EDragMode::Connect)
	{
		DragTargetVertexIndex = (Hover.IsVertex() && Hover.Index != DragVertexIndex) ? Hover.Index : INDEX_NONE;
	}
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
			Target->NotifyChanged();
		}
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
	Target->NotifyChanged();
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
			// vertex can never be dragged off-lattice while a basis exists.
			V.LatticeCoord = BasisPtr->SnapWorldToCoord(DragPreviewLocal);
			V.Transform.SetLocation(BasisPtr->CoordToWorld(V.LatticeCoord));
			DragPreviewLocal = V.Transform.GetLocation();
		}
		else
		{
			V.Transform.SetLocation(DragPreviewLocal);
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
		EndTransaction();
		Target->NotifyChanged();
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
					FarVertex = Model->AddLatticeVertex(Basis.SnapWorldToCoord(PlacePoint), Basis);
				}
				else
				{
					FarVertex = Model->AddVertex(FTransform(PlacePoint));
				}
				Model->Connect(Source, FarVertex);
			}

			if (FarVertex != INDEX_NONE)
			{
				// Selecting the far end chains the gesture into a walk.
				ClearSelection();
				SelectedVertices.Add(FarVertex);
			}
			Target->NotifyChanged();
		}
	}

	DragMode = EDragMode::None;
	DragVertexIndex = INDEX_NONE;
	DragTargetVertexIndex = INDEX_NONE;
}

void FPCGExSketchEditController::CancelDrag()
{
	if (ActiveTransaction)
	{
		// A move already mutated under the open transaction -- cancelling rolls the object back.
		ActiveTransaction->Cancel();
		ActiveTransaction.Reset();
	}
	DragMode = EDragMode::None;
	DragVertexIndex = INDEX_NONE;
	DragTargetVertexIndex = INDEX_NONE;
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

	ClearSelection();
	ClearHover();
	Target->NotifyChanged();
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
	Target->NotifyChanged();
}

void FPCGExSketchEditController::ClearSelection()
{
	SelectedVertices.Reset();
	SelectedEdges.Reset();
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
	FVector Anchor = BasisPtr ? BasisPtr->Origin : FVector::ZeroVector;
	for (const int32 Selected : SelectedVertices)
	{
		if (Model->Vertices.IsValidIndex(Selected))
		{
			Anchor = VertexLocation(Model->Vertices[Selected], BasisPtr);
			break;
		}
	}

	const FVector PlacePoint = RayPointOnWorkPlane(LocalRay, Anchor, BasisPtr);

	const FScopedTransaction Transaction(LOCTEXT("AddVertex", "Add Sketch Vertex"));
	if (UObject* TransactionObject = Target->GetTransactionObject())
	{
		TransactionObject->Modify();
	}

	int32 NewVertex;
	if (bSnapEnabled && BasisPtr)
	{
		NewVertex = Model->AddLatticeVertex(BasisPtr->SnapWorldToCoord(PlacePoint), *BasisPtr);
	}
	else
	{
		NewVertex = Model->AddVertex(FTransform(PlacePoint));
	}

	ClearSelection();
	SelectedVertices.Add(NewVertex);
	Target->NotifyChanged();
	return NewVertex;
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

#pragma endregion

#undef LOCTEXT_NAMESPACE
