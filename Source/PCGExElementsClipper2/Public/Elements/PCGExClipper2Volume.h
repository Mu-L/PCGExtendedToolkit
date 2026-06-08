// Copyright 2026 Timothe Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGCrc.h"
#include "Core/PCGExClipper2Processor.h"
#include "Details/PCGExInputShorthandsDetails.h"

#include "PCGExClipper2Volume.generated.h"

class AVolume;
class UPCGManagedActors;

namespace PCGExDetails
{
	template <typename T>
	class TSettingValue;
}

// Defined in the .cpp -- holds plain, game-thread-agnostic build data produced by Process(Group)
// and consumed by OutputWork() when spawning the volume actor.
struct FPCGExVolumeSpec;

/** How each convex piece's floor Z is derived from its own points. */
UENUM(BlueprintType)
enum class EPCGExVolumeBaseMode : uint8
{
	Flat    = 0 UMETA(DisplayName = "Flat", ToolTip = "All pieces share one flat floor at the footprint's lowest point."),
	Min     = 1 UMETA(DisplayName = "Min", ToolTip = "Each piece's floor sits at the lowest point Z within that piece (contains all of its points)."),
	Max     = 2 UMETA(DisplayName = "Max", ToolTip = "Each piece's floor sits at the highest point Z within that piece."),
	Average = 3 UMETA(DisplayName = "Average", ToolTip = "Each piece's floor sits at the average point Z within that piece."),
};

/**
 * Clipper2 : Volume
 *
 * Extrudes a (possibly concave) closed-path footprint into an AVolume trigger actor: triangulate, merge into
 * convex pieces (Hertel-Mehlhorn), and write each as a vertical convex prism into the brush body setup
 * (AggGeom.ConvexElems) -- no BSP, so it works in editor and cooked runtime alike.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Path", meta=(PCGExNodeLibraryDoc="paths/generate/clipper2-volume"))
class UPCGExClipper2VolumeSettings : public UPCGExClipper2ProcessorSettings
{
	GENERATED_BODY()

public:
	UPCGExClipper2VolumeSettings(const FObjectInitializer& ObjectInitializer);

	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(Clipper2Volume, "Clipper2 : Volume", "Extrude a closed path footprint into an AVolume trigger volume.");

	virtual FLinearColor GetNodeTitleColor() const override
	{
		return FLinearColor::White;
	}

	virtual EPCGSettingsType GetType() const override
	{
		return EPCGSettingsType::Spawner;
	}
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;

	virtual bool IsCacheable() const override
	{
		return false;
	}

	//~End UPCGSettings

public:
	/** Projection settings used to flatten the path into a 2D footprint and to orient the extrusion. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FPCGExGeo2DProjectionDetails ProjectionDetails;

	/** Fill rule used when triangulating the footprint. Even-Odd treats nested rings as holes (donut volumes). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExClipper2FillRule FillRule = EPCGExClipper2FillRule::EvenOdd;

	/** Volume actor class to spawn. Must derive from AVolume (e.g. ATriggerVolume, APostProcessVolume, a custom gameplay volume). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Volume", meta = (PCG_NotOverridable))
	TSubclassOf<AVolume> VolumeClass;

	/** Extrusion height applied above the footprint base plane. Constant, per-point attribute, or property (e.g. $Extent). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Volume", meta = (PCG_Overridable))
	FPCGExInputShorthandSelectorDouble Height = FPCGExInputShorthandSelectorDouble(FName("Height"), 200.0);

	/** Minimum extrusion height. Pieces thinner than this are clamped up so the prism never collapses. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Volume", meta = (PCG_Overridable, ClampMin = 0.01))
	double MinThickness = 1.0;

	/** How each convex piece's floor Z is chosen from its points, so the volume can step to follow uneven input.
	 *  Flat keeps a single shared floor at the footprint's lowest point. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Volume", meta = (PCG_NotOverridable))
	EPCGExVolumeBaseMode BaseMode = EPCGExVolumeBaseMode::Min;

	/** If enabled, greedily merge triangles into larger convex pieces (Hertel-Mehlhorn). Disable for raw per-triangle prisms. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Decomposition", meta = (PCG_NotOverridable))
	bool bMergeConvexPieces = true;

	/** Safety cap on convex pieces per volume. Volumes exceeding this are skipped with a warning (protects physics broadphase). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Decomposition", meta = (PCG_Overridable, ClampMin = 1))
	int32 MaxConvexPieces = 256;

	/** If enabled, override the spawned volume's collision profile with the one below. Otherwise the class default is kept. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Collision", meta = (PCG_NotOverridable, InlineEditConditionToggle))
	bool bOverrideCollisionProfile = false;

	/** Collision profile applied to the spawned volume's brush component when the override is enabled. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Collision", meta = (PCG_Overridable, EditCondition = "bOverrideCollisionProfile"))
	FName CollisionProfileName = FName("Trigger");

	/** Name of the soft-object-path attribute written to the Actor References output (one entry per spawned volume). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Output", meta = (PCG_NotOverridable))
	FName ActorReferenceAttributeName = FName("ActorReference");

	/** Suppress per-group warnings about degenerate footprints / failed triangulation. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Warnings and Errors", meta = (PCG_NotOverridable))
	bool bQuietWarnings = false;

	virtual FPCGExGeo2DProjectionDetails GetProjectionDetails() const override;

	virtual bool SupportOpenMainPaths() const override
	{
		return false; // Volumes require closed footprints.
	}
};

struct FPCGExClipper2VolumeContext final : FPCGExClipper2ProcessorContext
{
	friend class FPCGExClipper2VolumeElement;

	/** Per-source-path height reader (Facade->Idx indexes this array, matching AllOpData). */
	TArray<TSharedPtr<PCGExDetails::TSettingValue<double>>> HeightValues;

	/** Volume build specs produced off-thread in Process(Group), consumed on the game thread in OutputWork. */
	TArray<TSharedPtr<FPCGExVolumeSpec>> StagedVolumes;
	mutable FCriticalSection StagedVolumesLock;

	/** Managed resource that owns all spawned volumes for this generation (auto-cleanup on regen). */
	UPCGManagedActors* ManagedActors = nullptr;

	/** CRC of inputs + settings, stamped on the managed resource. */
	FPCGCrc DependenciesCrc;

	void AddStagedVolume(const TSharedPtr<FPCGExVolumeSpec>& Spec);

	/** Spawns every staged volume. MUST run on the game thread -- OutputWork marshals it there. */
	void SpawnStagedVolumes();

	virtual void Process(const TSharedPtr<PCGExClipper2::FProcessingGroup>& Group) override;
};

class FPCGExClipper2VolumeElement final : public FPCGExClipper2ProcessorElement
{
protected:
	PCGEX_CAN_ONLY_EXECUTE_ON_MAIN_THREAD(true) // OutputWork spawns actors + cooks collision on the game thread.
	PCGEX_ELEMENT_CREATE_CONTEXT(Clipper2Volume)

	virtual bool PostBoot(FPCGExContext* InContext) const override;
	virtual void OutputWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};
