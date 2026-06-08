// Copyright 2026 Timothe Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExClipper2Volume.h"

#include "Clipper2Lib/clipper.h"
#include "Core/PCGExClipper2Decomposition.h"

#include "Model.h"
#include "Components/BrushComponent.h"
#include "Engine/Polys.h"
#include "Engine/TriggerVolume.h"
#include "GameFramework/Volume.h"
#include "PhysicsEngine/BodySetup.h"

#include "PCGComponent.h"
#include "PCGElement.h"
#include "PCGManagedResource.h"
#include "PCGParamData.h"
#include "Helpers/PCGHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#include "Core/PCGExMT.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Data/Utils/PCGExDataForward.h"
#include "Details/PCGExSettingsDetails.h"
#include "Engine/World.h"
#include "Helpers/PCGExCollectionsHelpers.h"
#include "Helpers/PCGExMetaHelpers.h"
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
	int32 SourceFacadeIndex = INDEX_NONE; // AllOpData index of the group's representative path (for @Data forwarding).
};

// File-local helpers in a named namespace (Unity-build safe). Shared triangulation/decomposition lives in
// PCGExClipper2Decomposition; only the volume-specific prism tessellation is here.
namespace PCGExClipper2Volume
{
	// Build the side/cap polys of a vertical prism (local space) for editor wireframe + bounds.
	void AddPrismPolys(const TArray<FVector>& Bottoms, const TArray<FVector>& Tops, TArray<FPoly>& OutPolys)
	{
		const int32 N = Bottoms.Num();
		if (N < 3)
		{
			return;
		}

		// Bottom cap (reversed winding so it faces down/outward).
		{
			FPoly Poly;
			Poly.Init();
			for (int32 i = N - 1; i >= 0; --i)
			{
				Poly.Vertices.Add(FVector3f(Bottoms[i]));
			}
			Poly.Base = FVector3f(Bottoms[0]);
			if (Poly.CalcNormal(true) == 0)
			{
				OutPolys.Add(Poly);
			}
		}
		{
			FPoly Poly;
			Poly.Init();
			for (int32 i = 0; i < N; ++i)
			{
				Poly.Vertices.Add(FVector3f(Tops[i]));
			}
			Poly.Base = FVector3f(Tops[0]);
			if (Poly.CalcNormal(true) == 0)
			{
				OutPolys.Add(Poly);
			}
		}
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
			if (Poly.CalcNormal(true) == 0)
			{
				OutPolys.Add(Poly);
			}
		}
	}
}

#pragma region UPCGExClipper2VolumeSettings

UPCGExClipper2VolumeSettings::UPCGExClipper2VolumeSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	VolumeClass = ATriggerVolume::StaticClass();

	// Default to Auto grouping: an outer footprint + its nested rings form one volume (rings become holes),
	// unrelated footprints stay separate. Dropdown exposed so users can pick Separate or Merged.
	bExposeGroupingPolicy = true;
	MainInputGroupingPolicy = EPCGExGroupingPolicy::Auto;

	// Hide inherited path-output-only parameters (blending, carry-over, open-path, simplify).
	bExposePathOutputProperties = false;
}

FPCGExGeo2DProjectionDetails UPCGExClipper2VolumeSettings::GetProjectionDetails() const
{
	return ProjectionDetails;
}

TArray<FPCGPinProperties> UPCGExClipper2VolumeSettings::OutputPinProperties() const
{
	// One soft-object-path reference per spawned volume actor, so downstream graphs can address them.
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_PARAM(FName("Actor References"), TEXT("Attribute set with one soft-object-path entry per spawned volume actor."), Normal)
	return PinProperties;
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

	// Build the actor-reference set up front and emit it unconditionally (even on early-outs / zero volumes) so
	// the "Actor References" pin is always present.
	const FName AttrName = Settings->ActorReferenceAttributeName.IsNone() ? FName("ActorReference") : Settings->ActorReferenceAttributeName;
	UPCGParamData* RefData = NewObject<UPCGParamData>();
	UPCGMetadata* RefMetadata = RefData->Metadata;
	FPCGMetadataAttribute<FSoftObjectPath>* RefAttribute = RefMetadata->FindOrCreateAttribute<FSoftObjectPath>(
		PCGExMetaHelpers::GetAttributeIdentifier(AttrName, RefData), FSoftObjectPath(), false, true);

	auto EmitReferences = [&]()
	{
		FPCGTaggedData& OutRef = OutputData.TaggedData.Emplace_GetRef();
		OutRef.Pin = FName("Actor References");
		OutRef.Data = RefData;
	};

	UPCGComponent* MutableComponent = GetMutableComponent();
	if (!MutableComponent)
	{
		EmitReferences();
		return;
	}

	UWorld* World = MutableComponent->GetWorld();
	if (!World)
	{
		EmitReferences();
		return;
	}

	const UPCGComponent* Comp = GetComponent();
	const bool bIsPreview = Comp && Comp->IsInPreviewMode();
	const bool bTransientSpawn = PCGHelpers::IsRuntimeOrPIE() || bIsPreview;

	ULevel* TargetLevel = nullptr;
	if (const AActor* CompOwner = Comp ? Comp->GetOwner() : nullptr)
	{
		TargetLevel = CompOwner->GetLevel();
	}

	StagedVolumes.Sort([](const TSharedPtr<FPCGExVolumeSpec>& A, const TSharedPtr<FPCGExVolumeSpec>& B)
	{
		return A->GroupIndex < B->GroupIndex;
	});

	// Forward every @Data-domain attribute from the source path onto its actor's row. Handler is built once
	// per source and cached (rebuilding per row would re-scan the source's identities needlessly).
	const FPCGExForwardDetails ForwardDetails(true);
	TMap<int32, TSharedPtr<PCGExData::FDataForwardHandler>> HandlersBySource;

	for (const TSharedPtr<FPCGExVolumeSpec>& Spec : StagedVolumes)
	{
		if (!Spec || Spec->ConvexElems.IsEmpty())
		{
			continue;
		}

		FActorSpawnParameters SpawnParams;
		if (TargetLevel)
		{
			SpawnParams.OverrideLevel = TargetLevel;
		}
		if (bTransientSpawn)
		{
			SpawnParams.ObjectFlags |= RF_Transient | RF_NonPIEDuplicateTransient;
		}

		AVolume* Volume = World->SpawnActor<AVolume>(Settings->VolumeClass, Spec->ActorTransform, SpawnParams);
		if (!Volume)
		{
			continue;
		}

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

		// One row per spawned volume: the actor reference + the source's @Data attributes.
		const int64 Key = RefMetadata->AddEntry();
		if (RefAttribute)
		{
			RefAttribute->SetValue(Key, FSoftObjectPath(Volume));
		}

		if (AllOpData && AllOpData->Facades.IsValidIndex(Spec->SourceFacadeIndex))
		{
			const int32 SrcIdx = Spec->SourceFacadeIndex;
			if (!HandlersBySource.Contains(SrcIdx))
			{
				TSharedPtr<PCGExData::FDataForwardHandler> NewHandler = ForwardDetails.TryGetHandler(AllOpData->Facades[SrcIdx], false);
				if (NewHandler)
				{
					NewHandler->ValidateIdentities([](const PCGExData::FAttributeIdentity& Identity)
					{
						return Identity.InDataDomain();
					});
				}
				HandlersBySource.Add(SrcIdx, NewHandler); // cache the result (incl. null) so each source resolves once
			}
			if (const TSharedPtr<PCGExData::FDataForwardHandler>& Handler = HandlersBySource.FindChecked(SrcIdx))
			{
				Handler->Forward(0, RefMetadata, Key);
			}
		}
	}

	EmitReferences();
}

void FPCGExClipper2VolumeContext::Process(const TSharedPtr<PCGExClipper2::FProcessingGroup>& Group)
{
	const UPCGExClipper2VolumeSettings* Settings = GetInputSettings<UPCGExClipper2VolumeSettings>();

	// Triangulate + dedup vertex pool + Hertel-Mehlhorn merge (shared with Clipper2 : Decompose).
	const PCGExClipper2Decomposition::FDecomposeParams Params = PCGExClipper2Decomposition::MakeParams(Settings);

	PCGExClipper2Decomposition::FDecomposeResult Decomposition;
	if (!PCGExClipper2Decomposition::TryDecomposeGroup(Group, AllOpData, Params, Decomposition))
	{
		if (!Settings->bQuietWarnings)
		{
			const FText WarningText = PCGExClipper2Decomposition::DescribeDecomposeFailure(
				Decomposition, LOCTEXT("VolumeSubject", "volume"), Settings->MaxConvexPieces);
			if (!WarningText.IsEmpty())
			{
				PCGE_LOG_C(Warning, GraphAndLog, this, WarningText);
			}
		}
		return;
	}

	// First subject's projection is the consistent frame for the whole volume.
	const int32 FrameSrcIdx = Group->SubjectIndices[0];
	const FPCGExGeo2DProjectionDetails& FrameProjection = AllOpData->Projections[FrameSrcIdx];

	const TArray<PCGExClipper2Decomposition::FFootprintVertex>& VertexPool = Decomposition.VertexPool;
	const TArray<TArray<int32>>& Pieces = Decomposition.Pieces;

	// Per-vertex extrusion height (volume-specific, not in the shared pool): read at each vertex's mapped
	// source point; non-source vertices contribute 0.
	TArray<double> Heights;
	Heights.SetNumUninitialized(VertexPool.Num());
	for (int32 i = 0; i < VertexPool.Num(); i++)
	{
		const PCGExClipper2Decomposition::FFootprintVertex& V = VertexPool[i];
		Heights[i] = (V.bHasSource && HeightValues.IsValidIndex(V.SourceIdx) && HeightValues[V.SourceIdx])
			? HeightValues[V.SourceIdx]->Read(V.SourcePointIdx)
			: 0.0;
	}

	// --- Global lowest projected Z (actor base) + footprint centroid (actor origin) ---
	double MinBaseZ = 0;
	bool bAnyBase = false;
	FVector2D Centroid = FVector2D::ZeroVector;
	for (const PCGExClipper2Decomposition::FFootprintVertex& V : VertexPool)
	{
		Centroid += V.Pos;
		if (V.bHasSource)
		{
			MinBaseZ = bAnyBase ? FMath::Min(MinBaseZ, V.ProjectedZ) : V.ProjectedZ;
			bAnyBase = true;
		}
	}
	Centroid /= VertexPool.Num();

	// --- Build one convex prism (+ wireframe polys) per convex piece, in actor-local space ---
	TSharedPtr<FPCGExVolumeSpec> Spec = MakeShared<FPCGExVolumeSpec>();
	Spec->GroupIndex = Group->GroupIndex;
	Spec->SourceFacadeIndex = FrameSrcIdx;
	Spec->ConvexElems.Reserve(Pieces.Num());

	for (const TArray<int32>& Piece : Pieces)
	{
		const int32 N = Piece.Num();
		if (N < 3)
		{
			continue;
		}

		// Extrusion height = max of the piece's point heights; floor = piece's own min/max/avg Z (unless Flat).
		// A flat-floored prism stays convex at any base Z.
		double TopHeight = 0;
		double PieceBaseZ = MinBaseZ;
		{
			double SumZ = 0;
			double MinZ = 0;
			double MaxZ = 0;
			int32 SourceCount = 0;
			for (const int32 Idx : Piece)
			{
				const PCGExClipper2Decomposition::FFootprintVertex& V = VertexPool[Idx];
				TopHeight = FMath::Max(TopHeight, Heights[Idx]);
				if (!V.bHasSource)
				{
					continue;
				}
				MinZ = SourceCount == 0 ? V.ProjectedZ : FMath::Min(MinZ, V.ProjectedZ);
				MaxZ = SourceCount == 0 ? V.ProjectedZ : FMath::Max(MaxZ, V.ProjectedZ);
				SumZ += V.ProjectedZ;
				++SourceCount;
			}

			if (SourceCount > 0)
			{
				switch (Settings->BaseMode)
				{
				case EPCGExVolumeBaseMode::Min:
					PieceBaseZ = MinZ;
					break;
				case EPCGExVolumeBaseMode::Max:
					PieceBaseZ = MaxZ;
					break;
				case EPCGExVolumeBaseMode::Average:
					PieceBaseZ = SumZ / static_cast<double>(SourceCount);
					break;
				default:
					break; // Flat -> keep global base plane
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

	if (Spec->ConvexElems.IsEmpty())
	{
		return;
	}

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
		if (!HeightSetting->Init(Facade))
		{
			return false;
		}
		Context->HeightValues[i] = HeightSetting;
	}

	// CRC for the managed-resource record (no incremental reuse -- resources respawn each generation).
	GetDependenciesCrc(FPCGGetDependenciesCrcParams(&InContext->InputData, Settings, nullptr), Context->DependenciesCrc);

	return FPCGExClipper2ProcessorElement::PostBoot(InContext);
}

void FPCGExClipper2VolumeElement::OutputWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	PCGEX_CONTEXT_AND_SETTINGS(Clipper2Volume)

	// Actor spawning, physics cooking and managed-resource registration must run on the game thread (inline if
	// already there -- no deadlock). Always marshal, even with zero volumes, so SpawnStagedVolumes still emits
	// the (empty) Actor References set.
	PCGExMT::ExecuteOnMainThreadAndWait([Context]()
	{
		Context->SpawnStagedVolumes();
	});
}

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
