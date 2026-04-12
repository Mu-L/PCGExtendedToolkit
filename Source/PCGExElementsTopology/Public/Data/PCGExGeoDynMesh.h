// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Data/External/PCGExMesh.h"

class UDynamicMesh;

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace PCGExMesh
{
	/**
	 * Extracts geometry from a FDynamicMesh3 into FGeoMesh arrays.
	 * Sibling to FGeoStaticMesh but operates on dynamic meshes.
	 * Does NOT own the source mesh -- it must remain valid during extraction.
	 */
	class PCGEXELEMENTSTOPOLOGY_API FGeoDynMesh : public FGeoMesh
	{
	public:
		const UE::Geometry::FDynamicMesh3* SourceMesh = nullptr;

		FVector CWTolerance = FVector(DefaultVertexMergeHashTolerance);
		bool bPreciseVertexMerge = true;

		explicit FGeoDynMesh(
			const UE::Geometry::FDynamicMesh3* InMesh,
			const FVector& InCWTolerance = FVector(DefaultVertexMergeHashTolerance),
			bool bInPreciseVertexMerge = true);

		/** Edge-only extraction (for Raw, Boundaries modes). */
		void ExtractMeshSynchronous();

		/** Full triangulation with adjacency (for Dual, Hollow modes). */
		void TriangulateMeshSynchronous();

		/**
		 * Get averaged vertex colors. For each dense vertex, averages all color overlay
		 * elements from triangles referencing that vertex.
		 * @return false if the mesh has no color overlay.
		 */
		bool GetAveragedVertexColors(TArray<FVector4f>& OutColors) const;

		/** @return number of UV layers on the source mesh, or 0 if no attributes. */
		int32 GetNumUVChannels() const;

		/**
		 * Get averaged vertex UVs for a specific channel.
		 * @return false if the channel doesn't exist.
		 */
		bool GetAveragedVertexUVs(int32 Channel, TArray<FVector2f>& OutUVs) const;

	private:
		/** Maps sparse source vertex ID → dense output vertex index. Built during extraction. */
		TMap<int32, int32> VertexIDToDenseIndex;
	};
}
