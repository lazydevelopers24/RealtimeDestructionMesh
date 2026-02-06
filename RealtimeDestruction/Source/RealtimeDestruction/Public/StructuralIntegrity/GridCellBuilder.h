// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "StructuralIntegrity/GridCellTypes.h"
#include "VectorTypes.h"  // UE::Geometry::FVector3d

class UBodySetup;
struct FKConvexElem;
struct FKBoxElem;
struct FKSphereElem;
struct FKSphylElem;

namespace UE { namespace Geometry { class FDynamicMesh3; } }

/**
 * Grid cell builder (editor utility).
 * Builds a grid cell layout from a StaticMesh or DynamicMesh.
 */
class REALTIMEDESTRUCTION_API FGridCellBuilder
{
public:
	/**
	 * Build a grid cell layout from a static mesh.
	 *
	 * @param SourceMesh - source mesh
	 * @param MeshScale - mesh scale (component scale)
	 * @param CellSize - cell size (cm, world space)
	 * @param AnchorHeightThreshold - anchor height threshold (cm)
	 * @param OutLayout - output layout
	 * @return Whether the build succeeded
	 */
	static bool BuildFromStaticMesh(
		const UStaticMesh* SourceMesh,
		const FVector& MeshScale,
		const FVector& CellSize,
		float AnchorHeightThreshold,
		FGridCellLayout& OutLayout,
		TMap<int32, FSubCell>* OutSubCellStates = nullptr
		);

	/**
	 * Build a grid cell layout from a dynamic mesh.
	 *
	 * @param Mesh - source dynamic mesh
	 * @param CellSize - cell size (cm)
	 * @param AnchorHeightThreshold - anchor height threshold (cm)
	 * @param OutLayout - output layout
	 * @return Whether the build succeeded
	 */
	static bool BuildFromDynamicMesh(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const FVector& CellSize,
		float AnchorHeightThreshold,
		FGridCellLayout& OutLayout);

	static bool TriangleIntersectsAABB(
		const FVector& V0, const FVector& V1, const FVector& V2,
		const FVector& BoxMin, const FVector& BoxMax
	);

	/**
	* Mark subcells as alive if they intersect with the given triangle.
	*
	* @param V0, V1, V2 - Triangle vertices (local space)
	* @param CellMin - Cell minimum corner (local space)
	* @param CellSize - Cell size (local space)
	* @param OutSubCellState - SubCell state to update (bits set to 1 for alive)
	*/

	static void MarkIntersectingSubCellsAlive(
		const FVector& V0, const FVector& V1, const FVector& V2,
		const FVector& CellMin, const FVector& CellSIze,
		FSubCell& OutSubCellState
	);

	static void SetAnchorsByFinitePlane(
		const FTransform& PlaneTransform,
		const FTransform& MeshTransform,
		FGridCellLayout& OutLayout,
		bool bIsEraser);

	static void SetAnchorsByFiniteBox(
		const FTransform& BoxTransform,
		const FVector& BoxExtent,
		const FTransform& MeshTransform,
		FGridCellLayout& OutLayout,
		bool bIsEraser);

	static void SetAnchorsByFiniteSphere(
		const FTransform& SphereTransform,
		float SphereRadius,
		const FTransform& MeshTransform,
		FGridCellLayout& OutLayout,
		bool bIsEraser);

	static void ClearAllAnchors(FGridCellLayout& OutLayout);

private:
	/**
	 * Calculate grid dimensions from a bounding box.
	 */
	static void CalculateGridDimensions(
		const FBox& Bounds,
		const FVector& CellSize,
		FGridCellLayout& OutLayout);

	/**
	 * Assign triangles to cells.
	 */
	static void AssignTrianglesToCells(
		const UE::Geometry::FDynamicMesh3& Mesh,
		FGridCellLayout& OutLayout);

	/**
	 * Calculate adjacency (6 directions).
	 */
	static void CalculateNeighbors(FGridCellLayout& OutLayout);

	/**
	 * Determine anchor cells.
	 */
	static void DetermineAnchors(
		FGridCellLayout& OutLayout,
		float HeightThreshold);
	
	/**
	 * Voxelize mesh (fill interior cells) - DynamicMesh version.
	 */
	static void VoxelizeMesh(
		const UE::Geometry::FDynamicMesh3& Mesh,
		FGridCellLayout& OutLayout);

	/**
	 * Voxelize using all collision types (Convex, Box, Sphere, Capsule).
	 */
	static void VoxelizeWithCollision(
		const UBodySetup* BodySetup,
		FGridCellLayout& OutLayout);

	/**
	 * Voxelize using convex collision (compatibility).
	 */
	static void VoxelizeWithConvex(
		const UBodySetup* BodySetup,
		FGridCellLayout& OutLayout);

	/** Voxelize using StaticMesh render triangles. */
	static void VoxelizeWithTriangles(
		const UStaticMesh* SourceMesh,
		FGridCellLayout& OutLayout,
		TMap<int32, FSubCell>* OutSubCellStates);

	/** Voxelize a single triangle (with optional SubCell support). */
	static void VoxelizeTriangle(
		const FVector& V0,
		const FVector& V1,
		const FVector& V2,
		FGridCellLayout& OutLayout,
		TMap<int32, FSubCell>* OutSubCellStates);

	/** Voxelize from vertex/index arrays (for cached data). */
	static void VoxelizeFromArrays(
		const TArray<FVector>& Vertices,
		const TArray<uint32>& Indices,
		FGridCellLayout& OutLayout,
		TMap<int32, FSubCell>* OutSubCellStates);


	static void FillInsideVoxels(FGridCellLayout& OutLayout);

	/**
	 * Check whether a point is inside a convex hull.
	 */
	static bool IsPointInsideConvex(
		const FKConvexElem& ConvexElem,
		const FVector& Point);

	/**
	 * Check whether a point is inside a box.
	 */
	static bool IsPointInsideBox(
		const FKBoxElem& BoxElem,
		const FVector& Point);

	/**
	 * Check whether a point is inside a sphere.
	 */
	static bool IsPointInsideSphere(
		const FKSphereElem& SphereElem,
		const FVector& Point);

	/**
	 * Check whether a point is inside a capsule.
	 */
	static bool IsPointInsideCapsule(
		const FKSphylElem& CapsuleElem,
		const FVector& Point);

};
