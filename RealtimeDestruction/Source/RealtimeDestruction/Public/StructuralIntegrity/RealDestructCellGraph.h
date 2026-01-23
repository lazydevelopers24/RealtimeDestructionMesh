// Copyright (c) 2026 Lazy Developers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#pragma once
#include "CoreMinimal.h"
#include "Components/DynamicMeshComponent.h"
#include "StructuralIntegrity/StructuralIntegritySystem.h"

namespace UE::Geometry
{
	class FDynamicMesh3;
	class FMeshConnectedComponents;
	struct FIndex3i;
}

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FMeshConnectedComponents;
using UE::Geometry::FIndex3i;

// Represents a finite rectangular area on a cutting plane (slice face).
struct FChunkDivisionPlaneRect
{
	FVector PlaneOrigin = FVector::ZeroVector;     // Reference point on the plane (a point on the plane equation)
	FVector PlaneNormal = FVector::UpVector;       // Plane normal (should be normalized)
	FVector RectCenter = FVector::ZeroVector;      // Rectangle center (must lie on the plane)
	FVector RectAxisU = FVector::ForwardVector;    // U axis on the plane (normalized, orthogonal to PlaneNormal)
	FVector RectAxisV = FVector::RightVector;      // V axis on the plane (normalized, orthogonal to PlaneNormal)
	FVector2D HalfExtents = FVector2D::ZeroVector; // Half extents (half of U/V lengths)
	int32 ChunkA = INDEX_NONE;                     // Chunk ID on one side of the plane
	int32 ChunkB = INDEX_NONE;                     // Chunk ID on the other side of the plane
};

// 2D projected data of triangles touching the plane rectangle.
struct FChunkBoundaryTriangle2D
{
	FVector2D P0 = FVector2D::ZeroVector; // Plane UV coordinate 0
	FVector2D P1 = FVector2D::ZeroVector; // Plane UV coordinate 1
	FVector2D P2 = FVector2D::ZeroVector; // Plane UV coordinate 2
	FBox2D Bounds;                        // Triangle 2D AABB
};

// Graph adjacency info: neighbor node connected to the current node.
struct FChunkCellNeighbor
{
	int32 ChunkId = INDEX_NONE;            // Neighbor chunk ID
	int32 CellId = INDEX_NONE;             // Neighbor cell ID (unique within the neighbor chunk)
	int32 DivisionPlaneIndex = INDEX_NONE; // Division plane index used for the connection (INDEX_NONE if none)
};

// Graph node: chunk/cell-level information.
struct FChunkCellNode
{
	int32 ChunkId = INDEX_NONE;               // Chunk ID (corresponds to one UDynamicMeshComponent)
	int32 CellId = INDEX_NONE;                // Cell ID (unique within the chunk, corresponds to FMeshConnectedComponent)
	TArray<FChunkCellNeighbor> Neighbors;     // Neighbor list
	bool bIsAnchor = false;                   // Anchor flag
};

// Per-chunk cell (connected component) cache.
struct FChunkCellCache
{
	int32 ChunkId = INDEX_NONE;                 // Chunk ID
	TArray<int32> CellIds;                      // Cell ID list (1:1 with array indices)
	TArray<TArray<int32>> CellTriangles;        // Triangle ID list per cell
	TArray<FBox> CellBounds;                    // AABB per cell
	bool bHasGeometry = false;                  // Whether the chunk has valid geometry
	int32 MeshRevision = 0;                     // Mesh revision (optional)
};

// Old Cell -> New Cell(s) mapping info.
struct FCellMapping
{
	int32 OldCellId = INDEX_NONE;       // Cell ID before update
	TArray<int32> NewCellIds;           // Cell IDs after update (multiple if split)
	bool bDestroyed = false;            // Fully destroyed flag
};

// Chunk update result.
struct FChunkUpdateResult
{
	int32 ChunkId = INDEX_NONE;         // Updated chunk ID
	TArray<FCellMapping> Mappings;      // Old -> New cell mapping list
	FChunkCellCache OldCache;           // Cache before update
	FChunkCellCache NewCache;           // Cache after update
};

// <Potential additions if bottlenecks appear>
// - (ChunkId, CellId) -> NodeIndex lookup cache: minimize linear searches for adjacency/updates
// - Cell matching cache: preserve Old/New CellId mapping after recompute
// - Anchor node list: reduce BFS start-point scan cost

/**
 * Structures geometry data from URealtimeDestructibleMesh-owned meshes into a graph.
 * The structured graph is used by FStructuralIntegritySystem for structural analysis.
 */
class REALTIMEDESTRUCTION_API FRealDestructCellGraph
{
public:
	//=========================================================================
	// Initialization and graph construction
	//=========================================================================

	/**
	 * Build a division plane list from grid slicing results.
	 * Bounds should be the local-space AABB.
	 */
	void BuildDivisionPlanesFromGrid(
		const FBox& Bounds, // Source static mesh's AABB
		const FIntVector& SliceCount,
		const TArray<int32>& ChunkIdByGridIndex); // Chunk id per grid cell index

	/**
	 * Compute connected components (cells) for each chunk and build the graph by
	 * determining inter-chunk cell connectivity using division planes.
	 * - BuildDivisionPlanesFromGrid() must be called first
	 * - ChunkMeshes size must cover the ChunkId range referenced by DivisionPlanes
	 *
	 * @param ChunkMeshes - mesh pointer array per chunk (index = ChunkId)
	 * @param PlaneTolerance - plane distance tolerance (cm)
	 * @param RectTolerance - rectangle expansion tolerance (cm)
	 * @param FloorHeightThreshold - floor anchor Z threshold (cm, relative to Bounds.Min.Z)
	 */
	void BuildGraph(
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance = 0.1f,
		float RectTolerance = 0.1f,
		float FloorHeightThreshold = 10.0f);

	/** Convert graph nodes to a flat adjacency list usable by IntegritySystem. */
	FStructuralIntegrityInitData BuildInitDataFromGraph() const;

	/**
	 * Create a snapshot of the current graph state (for the new SyncGraph API).
	 * - Nodes are sorted by (ChunkId, CellId)
	 * - Includes the anchor node list
	 */
	FStructuralIntegrityGraphSnapshot BuildGraphSnapshot() const;

	/** Check whether the graph is initialized. */
	bool IsGraphBuilt() const { return Nodes.Num() > 0; }

	/** Reset the graph. */
	void Reset();

	//=========================================================================
	// Runtime graph updates
	//=========================================================================

	/**
	 * Recompute cells for modified chunks and build Old->New mappings.
	 *
	 * @param ModifiedChunkIds - set of modified chunk IDs
	 * @param ChunkMeshes - mesh pointer array per chunk (index = ChunkId)
	 * @return Update results per chunk (includes Old->New cell mapping)
	 */
	TArray<FChunkUpdateResult> UpdateModifiedChunks(
		const TSet<int32>& ModifiedChunkIds,
		const TArray<FDynamicMesh3*>& ChunkMeshes);

	/**
	 * Recheck connections on division planes related to updated chunks.
	 *
	 * @param UpdateResults - results from UpdateModifiedChunks()
	 * @param ChunkMeshes - mesh pointer array per chunk
	 * @param PlaneTolerance - plane distance tolerance (cm)
	 * @param RectTolerance - rectangle expansion tolerance (cm)
	 */
	void RebuildConnectionsForChunks(
		const TArray<FChunkUpdateResult>& UpdateResults,
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance = 0.1f,
		float RectTolerance = 0.1f);

	//=========================================================================
	// Boundary triangle and connectivity checks (static utilities)
	//=========================================================================
	
	/**
	 * Check if there are boundary triangles touching the rectangle area of a division plane.
	 *
     * Finds triangles in the mesh that touch the division plane, projects them into the
	 * plane-local U/V space, and returns the projections.
	 * (Returned triangles are used to test actual contact with triangles on the opposite chunk.)
	 *
	 * Criteria:
 	 * 1. All three triangle vertices are within PlaneTolerance of the plane
	 * 2. The projected triangle intersects the rectangle area (HalfExtents) within RectTolerance
	 *
	 * Usage:
	 * - Verify connectivity between two chunks after boolean operations
	 * - If no boundary triangles exist, connectivity in that direction is considered broken
	 *
	 * @param Mesh - mesh to test (chunk's FDynamicMesh3)
	 * @param TriangleIds - triangle IDs to test (triangles in the cell)
	 * @param Plane - division plane info (plane equation + rectangle area)
	 * @param PlaneTolerance - plane distance tolerance (cm). Vertex within this distance counts as on-plane
	 * @param RectTolerance - rectangle expansion tolerance (cm). Rect boundary is expanded by this amount
	 * @param OutTriangles - [out] 2D projection data of triangles touching the plane
	 * @param OutBounds - [out] 2D AABB enclosing all OutTriangles
	 * @return True if at least one boundary triangle exists
	 */
	static bool HasBoundaryTrianglesOnPlane(
		const FDynamicMesh3& Mesh,
		const TArray<int32>& TriangleIds,
		const FChunkDivisionPlaneRect& Plane,
		float PlaneTolerance,
		float RectTolerance,
		TArray<FChunkBoundaryTriangle2D>& OutTriangles,
		FBox2D& OutBounds);

	/**
	 * Check whether two nodes (chunk/cell) are still connected by the same division plane.
	 *
	 * Finds boundary triangles touching the division plane in both meshes and tests whether
	 * their 2D projections overlap to determine connectivity.
	 *
	 * Connectivity criteria:
	 * 1. MeshA has boundary triangles touching the plane
	 * 2. MeshB has boundary triangles touching the plane
	 * 3. The 2D projection areas overlap
	 *
	 * Usage:
	 * - Recheck connectivity between two previously adjacent chunks after boolean operations
	 * - If false, notify FStructuralIntegritySystem of disconnection
	 *
	 * @param MeshA - first chunk mesh
	 * @param TriangleIdsA - triangle IDs in MeshA (cell triangles)
	 * @param MeshB - second chunk mesh
	 * @param TriangleIdsB - triangle IDs in MeshB (cell triangles)
	 * @param Plane - division plane separating the two chunks
	 * @param PlaneTolerance - plane distance tolerance (cm)
	 * @param RectTolerance - rectangle expansion tolerance (cm)
	 * @return True if the nodes are connected on this plane, false if disconnected
	 */
	static bool AreNodesConnectedByPlane(
		const FDynamicMesh3& MeshA,
		const TArray<int32>& TriangleIdsA,
		const FDynamicMesh3& MeshB,
		const TArray<int32>& TriangleIdsB,
		const FChunkDivisionPlaneRect& Plane,
		float PlaneTolerance,
		float RectTolerance);

	//=========================================================================
	// Getters
	//=========================================================================

	/** Get node count. */
	int32 GetNodeCount() const { return Nodes.Num(); }

	/** Get chunk count. */
	int32 GetChunkCount() const { return ChunkCellCaches.Num(); }

	/** Get a node (read-only). */
	const FChunkCellNode* GetNode(int32 NodeIndex) const;

	/** Find node index by (ChunkId, CellId). */
	int32 FindNodeIndex(int32 ChunkId, int32 CellId) const;

	/** Get cell cache for a chunk. */
	const FChunkCellCache* GetChunkCellCache(int32 ChunkId) const;

private:
	//=========================================================================
	// Internal helpers
	//=========================================================================

	/**
	 * Build connected components (cells) for a single chunk.
	 * @param Mesh - mesh to analyze
	 * @param ChunkId - chunk ID
	 * @param OutCache - output cache
	 */
	void BuildChunkCellCache(const FDynamicMesh3& Mesh, int32 ChunkId, FChunkCellCache& OutCache);

	/**
	 * Check inter-chunk cell connectivity by division planes and build nodes.
	 * @param ChunkMeshes - chunk mesh array
	 * @param PlaneTolerance - plane distance tolerance
	 * @param RectTolerance - rectangle expansion tolerance
	 */
	void BuildNodesAndConnections(
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance,
		float RectTolerance);

	/**
	 * Check whether a cell is anchored to the floor.
	 * @param Cache - cache for the chunk containing the cell
	 * @param CellId - cell ID
	 * @param Mesh - chunk mesh
	 * @param FloorHeightThreshold - floor height threshold
	 * @return True if the cell touches the floor
	 */
	bool IsCellOnFloor(
		const FChunkCellCache& Cache,
		int32 CellId,
		const FDynamicMesh3& Mesh,
		float FloorHeightThreshold) const;

	/**
	 * Build Old -> New cell mappings based on AABB overlap.
	 * @param OldCache - cache before update
	 * @param NewCache - cache after update
	 * @return Cell mapping array
	 */
	TArray<FCellMapping> BuildCellMappings(
		const FChunkCellCache& OldCache,
		const FChunkCellCache& NewCache);

	/**
	 * Rebuild connections for a specific division plane.
	 * Removes existing connections for that plane and rechecks them.
	 */
	void RebuildConnectionsOnPlane(
		int32 PlaneIndex,
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance,
		float RectTolerance);

	/**
	 * Remove all nodes for a chunk.
	 */
	void RemoveNodesForChunk(int32 ChunkId);

	/**
	 * Add nodes for new cells in a chunk.
	 */
	void AddNodesForChunk(int32 ChunkId, const FChunkCellCache& NewCache);

	//=========================================================================
	// Data
	//=========================================================================

	TArray<FChunkCellNode> Nodes;                   // Graph nodes (all cells)
	TArray<FChunkDivisionPlaneRect> DivisionPlanes; // Chunk division planes
	TArray<FChunkCellCache> ChunkCellCaches;        // Per-chunk cell caches
	FBox MeshBounds;                                // Whole mesh bounds (for anchor tests)
};
