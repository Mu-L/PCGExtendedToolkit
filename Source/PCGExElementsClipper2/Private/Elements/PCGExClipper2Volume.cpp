// Copyright 2026 Timothe Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExClipper2Volume.h"

#include "Algo/Reverse.h"
#include "Clipper2Lib/clipper.h"
#include "Clipper2Lib/clipper.triangulation.h"

#include "Components/BrushComponent.h"
#include "Engine/Polys.h"
#include "Model.h"
#include "GameFramework/Volume.h"
#include "Engine/TriggerVolume.h"
#include "PhysicsEngine/BodySetup.h"

#include "PCGComponent.h"
#include "PCGElement.h"
#include "Helpers/PCGHelpers.h"
#include "PCGManagedResource.h"

#include "Core/PCGExMT.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Details/PCGExSettingsDetails.h"
#include "Engine/World.h"
#include "Helpers/PCGExCollectionsHelpers.h"
#include "Math/PCGExProjectionDetails.h"

#define LOCTEXT_NAMESPACE "PCGExClipper2VolumeElement"
#define PCGEX_NAMESPACE Clipper2Volume

// Plain build data produced off-thread in Process(Group), consumed on the game thread in OutputWork.
struct FPCGExVolumeSpec
{
	TArray<FKConvexElem> ConvexElems;
	TArray<FPoly> BrushPolys;
	FTransform ActorTransform = FTransform::Identity;
	int32 GroupIndex = 0;
};

// File-local helpers in a named namespace (Unity-build safe -- see project build notes).
namespace PCGExClipper2Volume
{
	// 2D footprint vertex in projection space, plus per-vertex extrusion height and base (normal) Z.
	struct FFootprintVertex
	{
		FVector2D Pos = FVector2D::ZeroVector;
		double Height = 0;
		double BaseZ = 0;
		bool bHasSource = false;
	};

	FORCEINLINE double Cross2D(const FVector2D& O, const FVector2D& A, const FVector2D& B)
	{
		return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
	}

	double SignedArea(const TArray<int32>& Loop, const TArray<FFootprintVertex>& Pool)
	{
		double Area = 0;
		const int32 N = Loop.Num();
		for (int32 i = 0; i < N; i++)
		{
			const FVector2D& A = Pool[Loop[i]].Pos;
			const FVector2D& B = Pool[Loop[(i + 1) % N]].Pos;
			Area += A.X * B.Y - B.X * A.Y;
		}
		return 0.5 * Area;
	}

	// Reorder a vertex-index loop to CCW (positive signed area) in projection X/Y.
	void EnsureCCW(TArray<int32>& Loop, const TArray<FFootprintVertex>& Pool)
	{
		if (SignedArea(Loop, Pool) < 0) { Algo::Reverse(Loop); }
	}

	// True if the loop is convex assuming CCW winding (reflex vertices -> false). Near-collinear is allowed.
	bool IsConvexCCW(const TArray<int32>& Loop, const TArray<FFootprintVertex>& Pool)
	{
		const int32 N = Loop.Num();
		if (N < 3) { return false; }
		for (int32 i = 0; i < N; i++)
		{
			const FVector2D& O = Pool[Loop[i]].Pos;
			const FVector2D& A = Pool[Loop[(i + 1) % N]].Pos;
			const FVector2D& B = Pool[Loop[(i + 2) % N]].Pos;
			if (Cross2D(O, A, B) < -UE_KINDA_SMALL_NUMBER) { return false; }
		}
		return true;
	}

	// If A and B (both CCW loops) share an edge (u->v in A, v->u in B), merge along it.
	// Returns true and fills OutMerged only when the merged polygon is convex.
	bool TryMergeConvex(const TArray<int32>& A, const TArray<int32>& B, const TArray<FFootprintVertex>& Pool, TArray<int32>& OutMerged)
	{
		const int32 NA = A.Num();
		const int32 NB = B.Num();

		for (int32 ia = 0; ia < NA; ia++)
		{
			const int32 U = A[ia];
			const int32 V = A[(ia + 1) % NA];

			for (int32 ib = 0; ib < NB; ib++)
			{
				if (B[ib] != V || B[(ib + 1) % NB] != U) { continue; }

				// Shared edge found (unique between two simple polygons). Build the merged loop:
				// walk A from V around to U, then B's interior from after U to before V.
				TArray<int32> Merged;
				Merged.Reserve(NA + NB - 2);
				for (int32 k = 0; k < NA; k++) { Merged.Add(A[(ia + 1 + k) % NA]); } // V ... U
				for (int32 k = 0; k < NB - 2; k++) { Merged.Add(B[(ib + 2 + k) % NB]); } // interior of B

				if (Merged.Num() >= 3 && IsConvexCCW(Merged, Pool))
				{
					OutMerged = MoveTemp(Merged);
					return true;
				}
				return false; // single shared edge, not convex -> cannot merge these two
			}
		}
		return false;
	}

	// Greedy Hertel-Mehlhorn-style convex merge of a triangulation into fewer convex pieces.
	void MergeIntoConvexPieces(TArray<TArray<int32>>& Pieces, const TArray<FFootprintVertex>& Pool)
	{
		bool bMerged = true;
		while (bMerged && Pieces.Num() > 1)
		{
			bMerged = false;
			for (int32 i = 0; i < Pieces.Num() && !bMerged; i++)
			{
				for (int32 j = i + 1; j < Pieces.Num() && !bMerged; j++)
				{
					TArray<int32> Result;
					if (TryMergeConvex(Pieces[i], Pieces[j], Pool, Result))
					{
						Pieces[i] = MoveTemp(Result);
						Pieces.RemoveAt(j);
						bMerged = true;
					}
				}
			}
		}
	}

	// Build the 6+ side/cap polys of a vertical prism (local space) for editor wireframe + bounds.
	void AddPrismPolys(const TArray<FVector>& Bottoms, const TArray<FVector>& Tops, TArray<FPoly>& OutPolys)
	{
		const int32 N = Bottoms.Num();
		if (N < 3) { return; }

		// Bottom cap (reversed so it faces down/outward).
		{
			FPoly Poly;
			Poly.Init();
			for (int32 i = N - 1; i >= 0; --i) { Poly.Vertices.Add(FVector3f(Bottoms[i])); }
			Poly.Base = FVector3f(Bottoms[0]);
			if (Poly.CalcNormal(true) == 0) { OutPolys.Add(Poly); }
		}
		// Top cap.
		{
			FPoly Poly;
			Poly.Init();
			for (int32 i = 0; i < N; ++i) { Poly.Vertices.Add(FVector3f(Tops[i])); }
			Poly.Base = FVector3f(Tops[0]);
			if (Poly.CalcNormal(true) == 0) { OutPolys.Add(Poly); }
		}
		// Side quads.
		for (int32 i = 0; i < N; ++i)
		{
			const int32 J = (i + 1) % N;
			FPoly Poly;
			Poly.Init();
			Poly.Vertices.Add(FVector3f(Bottoms[i]));
			Poly.Vertices.Add(FVector3f(Bottoms[J]));
			Poly.Vertices.Add(FVector3f(Tops[J]));
			Poly.Vertices.Add(FVector3f(Tops[i]));
			Poly.Base = FVector3f(Bottoms[i]);
			if (Poly.CalcNormal(true) == 0) { OutPolys.Add(Poly); }
		}
	}
}

#pragma region UPCGExClipper2VolumeSettings

UPCGExClipper2VolumeSettings::UPCGExClipper2VolumeSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	VolumeClass = ATriggerVolume::StaticClass();

	// One volume per input path by default (more intuitive than the base's Consolidate, which would
	// merge every input path into a single volume). Users can switch to Merged in the node settings.
	MainInputGroupingPolicy = EPCGExGroupingPolicy::Split;
}

FPCGExGeo2DProjectionDetails UPCGExClipper2VolumeSettings::GetProjectionDetails() const
{
	return ProjectionDetails;
}

TArray<FPCGPinProperties> UPCGExClipper2VolumeSettings::OutputPinProperties() const
{
	// This node produces actors via managed resources, not PCG data -- no output pins.
	return TArray<FPCGPinProperties>();
}

PCGEX_INITIALIZE_ELEMENT(Clipper2Volume)

#pragma endregion

#pragma region FPCGExClipper2VolumeContext

void FPCGExClipper2VolumeContext::AddStagedVolume(const TSharedPtr<FPCGExVolumeSpec>& Spec)
{
	FScopeLock Lock(&StagedVolumesLock);
	StagedVolumes.Add(Spec);
}

void FPCGExClipper2VolumeContext::SpawnStagedVolumes()
{
	const UPCGExClipper2VolumeSettings* Settings = GetInputSettings<UPCGExClipper2VolumeSettings>();

	UPCGComponent* MutableComponent = GetMutableComponent();
	if (!MutableComponent) { return; }

	UWorld* World = MutableComponent->GetWorld();
	if (!World) { return; }

	const UPCGComponent* Comp = GetComponent();
	const bool bIsPreview = Comp && Comp->IsInPreviewMode();
	const bool bTransientSpawn = PCGHelpers::IsRuntimeOrPIE() || bIsPreview;

	ULevel* TargetLevel = nullptr;
	if (const AActor* CompOwner = Comp ? Comp->GetOwner() : nullptr)
	{
		TargetLevel = CompOwner->GetLevel();
	}

	// Deterministic output ordering.
	StagedVolumes.Sort([](const TSharedPtr<FPCGExVolumeSpec>& A, const TSharedPtr<FPCGExVolumeSpec>& B)
	{
		return A->GroupIndex < B->GroupIndex;
	});

	for (const TSharedPtr<FPCGExVolumeSpec>& Spec : StagedVolumes)
	{
		if (!Spec || Spec->ConvexElems.IsEmpty()) { continue; }

		FActorSpawnParameters SpawnParams;
		if (TargetLevel) { SpawnParams.OverrideLevel = TargetLevel; }
		if (bTransientSpawn) { SpawnParams.ObjectFlags |= RF_Transient | RF_NonPIEDuplicateTransient; }

		AVolume* Volume = World->SpawnActor<AVolume>(Settings->VolumeClass, Spec->ActorTransform, SpawnParams);
		if (!Volume) { continue; }

		UBrushComponent* BrushComp = Volume->GetBrushComponent();
		if (!BrushComp)
		{
			Volume->Destroy();
			continue;
		}

		// Collision body from our convex pieces (the actual trigger geometry -- no BSP).
		UBodySetup* BodySetup = NewObject<UBodySetup>(BrushComp);
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		BodySetup->bGenerateNonMirroredCollision = true;
		BodySetup->bGenerateMirroredCollision = false;
		BodySetup->AggGeom.ConvexElems = MoveTemp(Spec->ConvexElems);
		BodySetup->CreatePhysicsMeshes();
		BrushComp->BrushBodySetup = BodySetup;

		// Brush model for editor wireframe + render/selection bounds (collision is independent of this).
		UModel* Model = NewObject<UModel>(BrushComp);
		Model->Initialize(nullptr, true);
		Model->Polys = NewObject<UPolys>(Model);
		Model->Polys->Element = MoveTemp(Spec->BrushPolys);
		Model->BuildBound();
		BrushComp->Brush = Model;

		if (Settings->bOverrideCollisionProfile)
		{
			BrushComp->SetCollisionProfileName(Settings->CollisionProfileName);
		}

		BrushComp->RecreatePhysicsState();
		BrushComp->MarkRenderStateDirty();

		// Create + register the managed resource lazily on first spawn so partial work is still tracked.
		if (!ManagedActors)
		{
			ManagedActors = NewObject<UPCGManagedActors>(MutableComponent);
			ManagedActors->SetCrc(DependenciesCrc);
#if WITH_EDITOR
			ManagedActors->SetIsPreview(bIsPreview);
#endif
			MutableComponent->AddToManagedResources(ManagedActors);
		}

		PCGExCollections::FinalizeSpawnedActor(Volume, ManagedActors, bTransientSpawn);
	}
}

void FPCGExClipper2VolumeContext::Process(const TSharedPtr<PCGExClipper2::FProcessingGroup>& Group)
{
	const UPCGExClipper2VolumeSettings* Settings = GetInputSettings<UPCGExClipper2VolumeSettings>();

	if (!Group->IsValid() || Group->SubjectPaths.empty() || Group->SubjectIndices.IsEmpty()) { return; }

	const double InvScale = 1.0 / static_cast<double>(Settings->Precision);

	// Use the first subject's projection as a consistent frame for the whole volume.
	const int32 FrameSrcIdx = Group->SubjectIndices[0];
	if (!AllOpData->Projections.IsValidIndex(FrameSrcIdx)) { return; }
	const FPCGExGeo2DProjectionDetails& FrameProjection = AllOpData->Projections[FrameSrcIdx];

	// --- Boundary-respecting triangulation (holes honored via fill rule) ---
	PCGExClipper2Lib::Paths64 CombinedPaths;
	CombinedPaths.reserve(Group->SubjectPaths.size());
	for (const auto& Path : Group->SubjectPaths) { CombinedPaths.push_back(Path); }

	PCGExClipper2Lib::Paths64 TrianglePaths;
	const PCGExClipper2Lib::TriangulateResult Result = PCGExClipper2Lib::TriangulateWithHoles(
		CombinedPaths, TrianglePaths, PCGExClipper2::ConvertFillRule(Settings->FillRule), true, Group->CreateZCallback());

	if (Result != PCGExClipper2Lib::TriangulateResult::success || TrianglePaths.empty())
	{
		if (!Settings->bQuietWarnings)
		{
			PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("A volume footprint could not be triangulated (degenerate or self-intersecting) and was skipped."));
		}
		return;
	}

	// --- Deduplicated 2D vertex pool with per-vertex height + base (normal) Z ---
	const int32 EstimatedVerts = static_cast<int32>(TrianglePaths.size()) * 3;
	TArray<PCGExClipper2Volume::FFootprintVertex> VertexPool;
	TMap<uint64, int32> VertexMap;
	VertexPool.Reserve(EstimatedVerts);
	VertexMap.Reserve(EstimatedVerts);

	auto FindOrAddVertex = [&](const PCGExClipper2Lib::Point64& Pt) -> int32
	{
		const uint64 Hash = PCGEx::H64(static_cast<uint32>(Pt.x & 0xFFFFFFFF), static_cast<uint32>(Pt.y & 0xFFFFFFFF));
		if (const int32* Found = VertexMap.Find(Hash)) { return *Found; }

		PCGExClipper2Volume::FFootprintVertex V;
		V.Pos = FVector2D(static_cast<double>(Pt.x) * InvScale, static_cast<double>(Pt.y) * InvScale);

		uint32 RawPointIdx, RawSourceIdx;
		PCGEx::H64(static_cast<uint64>(Pt.z), RawPointIdx, RawSourceIdx);

		if (RawPointIdx != PCGExClipper2::INTERSECTION_MARKER)
		{
			const int32 SrcIdx = static_cast<int32>(RawSourceIdx);
			const int32 PtIdx = static_cast<int32>(RawPointIdx);

			if (AllOpData->Facades.IsValidIndex(SrcIdx) && HeightValues.IsValidIndex(SrcIdx) && HeightValues[SrcIdx])
			{
				const int32 SrcNum = AllOpData->Facades[SrcIdx]->Source->GetNum(PCGExData::EIOSide::In);
				if (PtIdx < SrcNum)
				{
					V.Height = HeightValues[SrcIdx]->Read(PtIdx);
					if (AllOpData->ProjectedZValues.IsValidIndex(SrcIdx) && AllOpData->ProjectedZValues[SrcIdx].IsValidIndex(PtIdx))
					{
						V.BaseZ = AllOpData->ProjectedZValues[SrcIdx][PtIdx];
					}
					V.bHasSource = true;
				}
			}
		}

		const int32 Index = VertexPool.Num();
		VertexMap.Add(Hash, Index);
		VertexPool.Add(V);
		return Index;
	};

	TArray<TArray<int32>> Pieces; // each is a CCW vertex-index loop
	Pieces.Reserve(static_cast<int32>(TrianglePaths.size()));
	for (const auto& Tri : TrianglePaths)
	{
		if (Tri.size() != 3) { continue; }
		const int32 A = FindOrAddVertex(Tri[0]);
		const int32 B = FindOrAddVertex(Tri[1]);
		const int32 C = FindOrAddVertex(Tri[2]);
		if (A == B || B == C || C == A) { continue; }

		TArray<int32> Piece = {A, B, C};
		PCGExClipper2Volume::EnsureCCW(Piece, VertexPool);
		Pieces.Add(MoveTemp(Piece));
	}

	if (Pieces.IsEmpty() || VertexPool.IsEmpty()) { return; }

	if (Settings->bMergeConvexPieces)
	{
		PCGExClipper2Volume::MergeIntoConvexPieces(Pieces, VertexPool);
	}

	if (Pieces.Num() > Settings->MaxConvexPieces)
	{
		if (!Settings->bQuietWarnings)
		{
			PCGE_LOG_C(Warning, GraphAndLog, this, FText::Format(
				LOCTEXT("TooManyPieces", "A volume needs {0} convex pieces (over the {1} cap) and was skipped. Raise Max Convex Pieces or simplify the path."),
				FText::AsNumber(Pieces.Num()), FText::AsNumber(Settings->MaxConvexPieces)));
		}
		return;
	}

	// --- Global lowest projected Z (actor-origin base) + a single footprint centroid for the actor origin ---
	double MinBaseZ = 0;
	bool bAnyBase = false;
	FVector2D Centroid = FVector2D::ZeroVector;
	for (const PCGExClipper2Volume::FFootprintVertex& V : VertexPool)
	{
		Centroid += V.Pos;
		if (V.bHasSource)
		{
			MinBaseZ = bAnyBase ? FMath::Min(MinBaseZ, V.BaseZ) : V.BaseZ;
			bAnyBase = true;
		}
	}
	Centroid /= static_cast<double>(VertexPool.Num());

	// --- Build one convex prism (+ wireframe polys) per convex piece, in actor-local space ---
	TSharedPtr<FPCGExVolumeSpec> Spec = MakeShared<FPCGExVolumeSpec>();
	Spec->GroupIndex = Group->GroupIndex;
	Spec->ConvexElems.Reserve(Pieces.Num());

	for (const TArray<int32>& Piece : Pieces)
	{
		const int32 N = Piece.Num();
		if (N < 3) { continue; }

		// Extrusion height (max of the piece's point heights), and -- unless Flat -- the piece's own floor
		// (min/max/average of its points' Z). A flat-floored prism stays convex at any base Z.
		double TopHeight = 0;
		double PieceBaseZ = MinBaseZ;
		{
			double SumZ = 0;
			double MinZ = 0;
			double MaxZ = 0;
			int32 SourceCount = 0;
			for (const int32 Idx : Piece)
			{
				const PCGExClipper2Volume::FFootprintVertex& V = VertexPool[Idx];
				TopHeight = FMath::Max(TopHeight, V.Height);
				if (!V.bHasSource) { continue; }
				MinZ = SourceCount == 0 ? V.BaseZ : FMath::Min(MinZ, V.BaseZ);
				MaxZ = SourceCount == 0 ? V.BaseZ : FMath::Max(MaxZ, V.BaseZ);
				SumZ += V.BaseZ;
				++SourceCount;
			}

			if (SourceCount > 0)
			{
				switch (Settings->BaseMode)
				{
				case EPCGExVolumeBaseMode::Min: PieceBaseZ = MinZ;
					break;
				case EPCGExVolumeBaseMode::Max: PieceBaseZ = MaxZ;
					break;
				case EPCGExVolumeBaseMode::Average: PieceBaseZ = SumZ / static_cast<double>(SourceCount);
					break;
				default: break; // Flat -> keep the global base plane
				}
			}
		}
		TopHeight = FMath::Max(TopHeight, Settings->MinThickness);
		const double BaseLocalZ = PieceBaseZ - MinBaseZ; // 0 in Flat mode

		TArray<FVector> Bottoms;
		TArray<FVector> Tops;
		Bottoms.Reserve(N);
		Tops.Reserve(N);

		FKConvexElem Elem;
		Elem.VertexData.Reserve(N * 2);

		for (const int32 Idx : Piece)
		{
			const FVector2D& P = VertexPool[Idx].Pos;
			const FVector Bottom(P.X - Centroid.X, P.Y - Centroid.Y, BaseLocalZ);
			const FVector Top(P.X - Centroid.X, P.Y - Centroid.Y, BaseLocalZ + TopHeight);
			Bottoms.Add(Bottom);
			Tops.Add(Top);
			Elem.VertexData.Add(Bottom);
			Elem.VertexData.Add(Top);
		}

		Elem.UpdateElemBox();
		Spec->ConvexElems.Add(MoveTemp(Elem));

		PCGExClipper2Volume::AddPrismPolys(Bottoms, Tops, Spec->BrushPolys);
	}

	if (Spec->ConvexElems.IsEmpty()) { return; }

	// Actor frame: rotation = projection orientation, origin = footprint centroid on the base plane.
	const FVector WorldOrigin = FrameProjection.Unproject(FVector(Centroid.X, Centroid.Y, MinBaseZ));
	Spec->ActorTransform = FTransform(FrameProjection.ProjectionQuat, WorldOrigin);

	AddStagedVolume(Spec);
}

#pragma endregion

#pragma region FPCGExClipper2VolumeElement

bool FPCGExClipper2VolumeElement::PostBoot(FPCGExContext* InContext) const
{
	PCGEX_CONTEXT_AND_SETTINGS(Clipper2Volume)

	// One height reader per source path (Facade->Idx aligns with AllOpData order).
	const int32 NumFacades = Context->AllOpData->Num();
	Context->HeightValues.SetNum(NumFacades);

	for (int32 i = 0; i < NumFacades; i++)
	{
		const TSharedPtr<PCGExData::FFacade>& Facade = Context->AllOpData->Facades[i];
		TSharedPtr<PCGExDetails::TSettingValue<double>> HeightSetting = Settings->Height.GetValueSetting();
		if (!HeightSetting->Init(Facade)) { return false; }
		Context->HeightValues[i] = HeightSetting;
	}

	// CRC for the managed-resource record (no incremental reuse in this version -- resources are
	// released and respawned each generation).
	GetDependenciesCrc(FPCGGetDependenciesCrcParams(&InContext->InputData, Settings, nullptr), Context->DependenciesCrc);

	return FPCGExClipper2ProcessorElement::PostBoot(InContext);
}

void FPCGExClipper2VolumeElement::OutputWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	PCGEX_CONTEXT_AND_SETTINGS(Clipper2Volume)

	if (Context->StagedVolumes.IsEmpty()) { return; }

	// Actor spawning, physics cooking and managed-resource registration must run on the game thread.
	// OutputWork can be invoked off the game thread by the async pipeline, so marshal explicitly
	// (runs inline if already on the game thread -- no deadlock).
	PCGExMT::ExecuteOnMainThreadAndWait([Context]() { Context->SpawnStagedVolumes(); });
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
