// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "PCGExAssetInspectionBlueprintLibrary.generated.h"

class UStaticMesh;

/**
 * Asset inspection helpers for collection tooling (staging pipelines, editor utility
 * widgets) -- the computed data reflection can't answer, e.g. mesh complexity stats.
 *
 * All functions are null-safe (0 / false / degenerate box) and LOD-clamped. Stats are
 * RenderData-based (UStaticMesh::GetNumTriangles etc.), so they work on loaded meshes in
 * any target that has render data; some engine accessors overlap BlueprintPure members on
 * UStaticMesh -- the value-add here is null-safety, clamping, and the non-exposed APIs
 * (material slot count, Nanite flag).
 */
UCLASS()
class PCGEXCOLLECTIONS_API UPCGExAssetInspectionBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Number of LODs (0 when null or no render data). */
	UFUNCTION(BlueprintPure, Category = "PCGEx|Inspection")
	static int32 GetStaticMeshLODCount(const UStaticMesh* Mesh);

	/** Triangle count for the given LOD (clamped to the valid LOD range). */
	UFUNCTION(BlueprintPure, Category = "PCGEx|Inspection")
	static int32 GetStaticMeshTriCount(const UStaticMesh* Mesh, int32 LOD = 0);

	/** Vertex count for the given LOD (clamped to the valid LOD range). */
	UFUNCTION(BlueprintPure, Category = "PCGEx|Inspection")
	static int32 GetStaticMeshVertexCount(const UStaticMesh* Mesh, int32 LOD = 0);

	/** Number of material slots on the mesh. */
	UFUNCTION(BlueprintPure, Category = "PCGEx|Inspection")
	static int32 GetStaticMeshMaterialSlotCount(const UStaticMesh* Mesh);

	/** True when the mesh carries valid Nanite data. */
	UFUNCTION(BlueprintPure, Category = "PCGEx|Inspection")
	static bool HasNaniteData(const UStaticMesh* Mesh);

	/** The mesh's local bounding box (matches the conventions of entry Staging.Bounds). */
	UFUNCTION(BlueprintPure, Category = "PCGEx|Inspection")
	static FBox GetStaticMeshBounds(const UStaticMesh* Mesh);
};
