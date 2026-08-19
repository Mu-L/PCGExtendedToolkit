// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Lattice/PCGExLatticeBasis.h"

#include "PCGExClusterSketchModel.generated.h"

/** Value type of one annotation channel. Enumerator identifiers are a wire format -- never rename. */
UENUM(BlueprintType)
enum class EPCGExClusterDataChannelType : uint8
{
	Double  = 0 UMETA(ToolTip = "Double-precision scalar."),
	Integer = 1 UMETA(ToolTip = "64-bit integer."),
	Name    = 2 UMETA(ToolTip = "FName."),
	Vector  = 3 UMETA(ToolTip = "3D vector."),
};

/**
 * One named, typed annotation channel over a sketch domain (vertices or edges, decided by which array
 * holds it). Uninterpreted by core -- clients own their channels by naming convention. At print time a
 * channel becomes a PCG attribute of the same name on the printed Vtx or Edges output.
 * Only the array matching Type is used; element count is kept aligned with the domain by the model's
 * mutation API.
 */
USTRUCT(BlueprintType)
struct PCGEXGRAPHS_API FPCGExClusterDataChannel
{
	GENERATED_BODY()

	/** Attribute name on the printed output. Reserved cluster attributes (PCGEx/VData, PCGEx/EData) are refused. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	FName Name = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	EPCGExClusterDataChannelType Type = EPCGExClusterDataChannelType::Double;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (EditCondition = "Type == EPCGExClusterDataChannelType::Double", EditConditionHides))
	TArray<double> DoubleValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (EditCondition = "Type == EPCGExClusterDataChannelType::Integer", EditConditionHides))
	TArray<int64> IntegerValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (EditCondition = "Type == EPCGExClusterDataChannelType::Name", EditConditionHides))
	TArray<FName> NameValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (EditCondition = "Type == EPCGExClusterDataChannelType::Vector", EditConditionHides))
	TArray<FVector> VectorValues;

	/** Element count of the active array. */
	int32 Num() const;

	/** Resize the active array, default-initializing new elements. Clears the inactive arrays. */
	void SetNumDefaulted(int32 InNum);

	void RemoveAt(int32 Index);
	void InsertDefaulted(int32 Index);
};

/** One authored sketch vertex. */
USTRUCT(BlueprintType)
struct PCGEXGRAPHS_API FPCGExClusterSketchVertex
{
	GENERATED_BODY()

	/** Authoritative for FREE vertices. For lattice-bound ones the LOCATION is derived from LatticeCoord
	 *  (rotation/scale stay authored); a hand-edited location re-snaps the coord instead of dangling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	FTransform Transform = FTransform::Identity;

	/** Authoritative when bLatticeBound: the vertex IS this lattice node; world location derives from it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (EditCondition = "bLatticeBound", EditConditionHides))
	FIntVector LatticeCoord = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bLatticeBound = false;
};

/** One authored sketch edge -- a pair of vertex array indices, undirected. */
USTRUCT(BlueprintType)
struct PCGEXGRAPHS_API FPCGExClusterSketchEdge
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	int32 A = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	int32 B = -1;
};

/** Aggregate result of FPCGExClusterSketchModel::Validate -- counts, never element indices, so the
 *  caller can warn once per issue class. */
struct PCGEXGRAPHS_API FPCGExClusterSketchValidation
{
	int32 InvalidEdges = 0;      // out-of-range vertex index
	int32 SelfLoops = 0;
	int32 DuplicateEdges = 0;    // undirected duplicates beyond the first occurrence
	int32 IsolatedVertices = 0;  // dropped by cluster compile (clusters cannot represent them)
	TArray<FName> MisalignedChannels;   // channel length != domain count
	TArray<FName> InvalidChannelNames;  // None, duplicate, or reserved cluster attribute

	bool HasEdgeIssues() const { return InvalidEdges > 0 || SelfLoops > 0 || DuplicateEdges > 0; }
};

/**
 * The authored cluster-sketch model: vertices + undirected edges + annotation channels, all plain
 * serialized arrays. Edges reference vertices by array index; ALL mutations must go through the API
 * below, which keeps edge indices and channel arrays aligned through removals. Raw array edits (details
 * panel) are a supported surface -- print-time validation and the owning asset's edit hooks absorb them.
 */
USTRUCT(BlueprintType)
struct PCGEXGRAPHS_API FPCGExClusterSketchModel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TArray<FPCGExClusterSketchVertex> Vertices;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TArray<FPCGExClusterSketchEdge> Edges;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TArray<FPCGExClusterDataChannel> VertexChannels;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	TArray<FPCGExClusterDataChannel> EdgeChannels;

	int32 NumVertices() const { return Vertices.Num(); }
	int32 NumEdges() const { return Edges.Num(); }

	/** Append a free vertex. Extends every vertex channel. @return the new vertex index. */
	int32 AddVertex(const FTransform& InTransform);

	/** Append a lattice-bound vertex at Coord; location derived through the basis. Extends channels. */
	int32 AddLatticeVertex(const FIntVector& InCoord, const FPCGExLatticeBasis& InBasis);

	/** Remove a vertex, its edges, and its channel entries; remaining edge indices are remapped. */
	bool RemoveVertex(int32 Index);

	/** Add the undirected edge (A,B). Idempotent: an existing edge is returned rather than duplicated.
	 *  @return the edge index, or INDEX_NONE for an invalid pair (out of range or self-loop). */
	int32 Connect(int32 A, int32 B);

	/** Remove the undirected edge (A,B) and its channel entries. @return true if an edge was removed. */
	bool Disconnect(int32 A, int32 B);

	/** Index of the undirected edge (A,B), or INDEX_NONE. */
	int32 FindEdge(int32 A, int32 B) const;

	/** Bind/unbind a vertex to the lattice. Binding snaps the coord from the current location and
	 *  re-derives the location; unbinding keeps the current derived location as the free position. */
	bool SetLatticeBound(int32 Index, bool bBound, const FPCGExLatticeBasis& InBasis);

	/** Re-derive every bound vertex's location from its coord (basis edits rescale the sketch), or
	 *  re-snap coords from locations first when bResnapFromLocation (hand-edited transforms). */
	void SyncBoundVertices(const FPCGExLatticeBasis& InBasis, bool bResnapFromLocation);

	/** Aggregate integrity summary; cheap, never mutates. */
	void Validate(FPCGExClusterSketchValidation& OutSummary) const;
};
