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
#include "StructuralIntegrity/GridCellTypes.h"

/**
 * SubCell Processor
 * Utility class that marks subcells as dead based on collision with tool mesh
 * and prepares data for structural integrity checks
 */
class REALTIMEDESTRUCTION_API FSubCellProcessor
{
public:
	FSubCellProcessor() = default;

	/**
	 * Mark subcells overlapping the destruction shape as dead
	 *
	 * @param QuantizedShape - Quantized destruction shape (network deterministic)
	 * @param MeshTransform - Mesh world transform
	 * @param GridLayout - Grid cell layout (read-only)
	 * @param InOutCellState - Cell state (subcell state updated)
	 * @param OutAffectedCells - Affected cell ID list (output)
	 * @param OutNewlyDeadSubCells - Newly dead subcell info (output, optional)
	 * @return Whether processing succeeded
	 */
	static bool ProcessSubCellDestruction(
		const FQuantizedDestructionInput& QuantizedShape,
		const FTransform& MeshTransform,
		const FGridCellLayout& GridLayout,
		FCellState& InOutCellState,
		TArray<int32>& OutAffectedCells,
		TMap<int32, TArray<int32>>* OutNewlyDeadSubCells = nullptr);
	
	/**
	 * Return the number of alive subcells in a specific cell
	 *
	 * @param CellId - Cell ID
	 * @param CellState - Cell state
	 * @return Number of alive subcells
	 */
	static int32 CountLiveSubCells(int32 CellId, const FCellState& CellState);

	/**
	 * Check if a specific cell is fully destroyed
	 * (all subcells are dead)
	 *
	 * @param CellId - Cell ID
	 * @param CellState - Cell state
	 * @return Whether fully destroyed
	 */
	static bool IsCellFullyDestroyed(int32 CellId, const FCellState& CellState);

	/**
	 * Return list of subcell IDs on the boundary face for a given direction
	 *
	 * @param Direction - Direction (0-5: -X, +X, -Y, +Y, -Z, +Z)
	 * @return Subcell ID array on the boundary face for that direction
	 */
	static TArray<int32> GetBoundarySubCellIds(int32 Direction);

	/**
	 * Return bitmask of alive subcells on the boundary face for a given direction
	 *
	 * @param CellId - Cell ID
	 * @param Direction - Direction (0-5)
	 * @param CellState - Cell state
	 * @return Boundary subcell bitmask (max 4 bits used with 2x2 subdivision)
	 */
	static uint32 GetBoundaryLiveSubCellMask(int32 CellId, int32 Direction, const FCellState& CellState);

private:
	/**
	 * Compute AABB of a quantized shape
	 */
	static FBox ComputeShapeAABB(const FQuantizedDestructionInput& Shape);
};
