// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExAssetInspectionBlueprintLibrary.h"

#include "Engine/StaticMesh.h"

namespace PCGExAssetInspectionBlueprintLibrary_Private
{
	int32 ClampLOD(const UStaticMesh* Mesh, int32 LOD)
	{
		return FMath::Clamp(LOD, 0, FMath::Max(0, Mesh->GetNumLODs() - 1));
	}
}

int32 UPCGExAssetInspectionBlueprintLibrary::GetStaticMeshLODCount(const UStaticMesh* Mesh)
{
	return Mesh ? Mesh->GetNumLODs() : 0;
}

int32 UPCGExAssetInspectionBlueprintLibrary::GetStaticMeshTriCount(const UStaticMesh* Mesh, int32 LOD)
{
	if (!Mesh || Mesh->GetNumLODs() == 0)
	{
		return 0;
	}
	return Mesh->GetNumTriangles(PCGExAssetInspectionBlueprintLibrary_Private::ClampLOD(Mesh, LOD));
}

int32 UPCGExAssetInspectionBlueprintLibrary::GetStaticMeshVertexCount(const UStaticMesh* Mesh, int32 LOD)
{
	if (!Mesh || Mesh->GetNumLODs() == 0)
	{
		return 0;
	}
	return Mesh->GetNumVertices(PCGExAssetInspectionBlueprintLibrary_Private::ClampLOD(Mesh, LOD));
}

int32 UPCGExAssetInspectionBlueprintLibrary::GetStaticMeshMaterialSlotCount(const UStaticMesh* Mesh)
{
	return Mesh ? Mesh->GetStaticMaterials().Num() : 0;
}

bool UPCGExAssetInspectionBlueprintLibrary::HasNaniteData(const UStaticMesh* Mesh)
{
	return Mesh ? Mesh->HasValidNaniteData() : false;
}

FBox UPCGExAssetInspectionBlueprintLibrary::GetStaticMeshBounds(const UStaticMesh* Mesh)
{
	return Mesh ? Mesh->GetBoundingBox() : FBox(ForceInit);
}
