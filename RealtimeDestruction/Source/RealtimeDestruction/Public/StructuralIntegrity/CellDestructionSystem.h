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
 * Cell destruction evaluation system.
 * BFS-based structural integrity checks and destruction evaluation.
 */
class REALTIMEDESTRUCTION_API FCellDestructionSystem
{
public:
	//=========================================================================
	// Cell destruction evaluation
	//=========================================================================

	/**
	 * Calculate destroyed cell IDs from a destruction shape.
	 * Uses a center-point + vertex hybrid test.
	 *
	 * @param Cache - grid layout
	 * @param Shape - destruction shape (quantized)
	 * @param MeshTransform - mesh world transform
	 * @param DestroyedCells - already destroyed cells (exclude)
	 * @return Newly destroyed cell IDs
	 */
	static TArray<int32> ProcessCellDestruction(
		const FGridCellLayout& Cache,
		const FQuantizedDestructionInput& Shape,
		const FTransform& MeshTransform,
		const TSet<int32>& DestroyedCells);

	/**
	 * Calculate destroyed cell IDs from a destruction shape.
	 * Uses a center-point + vertex hybrid test.
	 *
	 * @param Cache - grid layout
	 * @param Shape - destruction shape (quantized)
	 * @param MeshTransform - mesh world transform
	 * @param InOutCellState - cell state (DestroyedCells exclusion)
	 * @return FDestructionResult with NewlyDestroyedCells populated
	 */
	static FDestructionResult ProcessCellDestruction(
		const FGridCellLayout& Cache,
		const FQuantizedDestructionInput& Shape,
		const FTransform& MeshTransform,
		FCellState& InOutCellState);

	/*
	 * <<<SubCell Level API>>>
	 * Performs subcell-level destruction.
	 * Cell destruction is handled via subcell processing instead of the center+vertex hybrid test.
	 */
	static FDestructionResult ProcessCellDestructionSubCellLevel(
		const FGridCellLayout& Cache,
		const FQuantizedDestructionInput& Shape,
		const FTransform& MeshTransform,
		FCellState& InOutCellState);
	
	/**
	 * Check whether a single cell is destroyed.
	 * Phase 1: center-point test (fast)
	 * Phase 2: majority-of-vertices test (edge cases)
	 */
	static bool IsCellDestroyed(
		const FGridCellLayout& Cache,
		int32 CellId,
		const FQuantizedDestructionInput& Shape,
		const FTransform& MeshTransform);

	
	//=========================================================================
	// Structural integrity checks (BFS)
	//=========================================================================

	/**
	 * Find cells detached from anchors (unified API).
	 *
	 * Selects the implementation based on bEnableSupercell/bEnableSubcell:
	 * - SuperCell enabled: hierarchical BFS (SuperCell + Cell/SubCell)
	 * - SubCell only: subcell-level BFS
	 * - Both disabled: cell-level BFS
	 *
	 * @param Cache - grid layout
	 * @param SupercellState - SuperCell state (used when bEnableSupercell=true)
	 * @param CellState - cell state
	 * @param bEnableSupercell - whether to use SuperCell BFS
	 * @param bEnableSubcell - whether to use subcell connectivity checks
	 * @return Set of detached cell IDs
	 */
	static TSet<int32> FindDisconnectedCells(
		const FGridCellLayout& Cache,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		bool bEnableSupercell,
		bool bEnableSubcell,
		FConnectivityContext& Context);

	/**
	 * <<<Cell Level API>>>
	 * Find cells detached from anchors.
	 * Prefer calling via FindDisconnectedCells.
	 *
	 * @param Cache - grid layout
	 * @param DestroyedCells - destroyed cell set
	 * @return Set of detached cell IDs
	 */
	static TSet<int32> FindDisconnectedCellsCellLevel(
		const FGridCellLayout& Cache,
		const TSet<int32>& DestroyedCells);

	/**
	 * <<<SubCell Level API>>>
	 * Find cells detached from anchors (subcell-level connectivity).
	 * Prefer calling via FindDisconnectedCells.
	 *
	 * Uses subcell-level BFS to test reachability to anchors.
	 * Starts from all anchors and traverses via subcell boundary connectivity.
	 *
	 * @param Cache - grid layout
	 * @param CellState - cell state (includes subcell state)
	 * @return Set of detached cell IDs
	 */
	static TSet<int32> FindDisconnectedCellsSubCellLevel(
		const FGridCellLayout& Cache,
		const FCellState& CellState);

	/**
	 * Find detached cells via hierarchical BFS.
	 *
	 * Calls FindConnectedCellsHierarchical() and returns unconnected cells.
	 * Prefer calling via FindDisconnectedCells.
	 *
	 * @param Cache - grid layout
	 * @param SupercellState - SuperCell state
	 * @param CellState - cell state
	 * @param bEnableSubcell - whether subcell mode is enabled
	 * @return Set of detached cell IDs
	 */
	static TSet<int32> FindDisconnectedCellsHierarchicalLevel(
		const FGridCellLayout& Cache,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		bool bEnableSubcell,
		FConnectivityContext& Context);

	static void FindConnectedCellsHierarchical_Optimized(
		const FGridCellLayout& Cache,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		FConnectivityContext& Context,
		bool bEnableSubcell);

	static TSet<int32> FindDisconnectedCellsFromAffected(
		const FGridCellLayout& Cache,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		const TArray<int32>& AffectedNeighborCells,
		FConnectivityContext& Context,
		bool bEnableSupercell,
		bool bEnableSubcell );

	static bool SupercellContainsAnchor(
		int32 SupercellId,
		const FGridCellLayout& Cache,
		const FSuperCellState& SupercellState,
		const FCellState& CellState);

	static bool SupercellContainsConfirmedConnected(
		int32 SupercellId,
		const FGridCellLayout& Cache,
		const FSuperCellState& SupercellState,
		const TSet<int32>& ConfirmedConnected
	);

	//=========================================================================
	// Grouping detached cells
	//=========================================================================
	/**
	 * Group detached cells into connected groups.
	 *
	 * @param Cache - grid layout
	 * @param DisconnectedCells - detached cells
	 * @param DestroyedCells - destroyed cells (exclude boundaries)
	 * @return Cell ID lists per group
	 */
	static TArray<TArray<int32>> GroupDetachedCells(
		const FGridCellLayout& Cache,
		const TSet<int32>& DisconnectedCells,
		const TSet<int32>& DestroyedCells);
	
	//=========================================================================
	// Utilities
	//=========================================================================

	/**
	 * Calculate the center of a cell group.
	 */
	static FVector CalculateGroupCenter(
		const FGridCellLayout& Cache,
		const TArray<int32>& CellIds,
		const FTransform& MeshTransform);

	/**
	 * Calculate initial debris velocity (explosion direction).
	 */
	static FVector CalculateDebrisVelocity(
		const FVector& DebrisCenter,
		const TArray<FQuantizedDestructionInput>& DestructionInputs,
		float BaseSpeed = 500.0f);

	/**
	 * Check if a cell is a boundary cell (adjacent to destroyed cells).
	 */
	static bool IsBoundaryCell(
		const FGridCellLayout& Cache,
		int32 CellId,
		const TSet<int32>& DestroyedCells);
};

/**
 * [DEPRECATED] Server destruction batching processor.
 * Collects destruction events and processes them on a 16.6ms cadence.
 *
 * Not currently used. Requires refactoring for SuperCell/SubCell unified API support
 * (add SupercellCache and bEnableSubcell to SetContext).
 */
class UE_DEPRECATED(5.0, "FDestructionBatchProcessor is not currently used. Requires refactoring for SuperCell/SubCell support.")
	REALTIMEDESTRUCTION_API FDestructionBatchProcessor
{
public:
	/** Batch interval (16.6ms = 60fps). */
	static constexpr float BatchInterval = 1.0f / 60.0f;

	FDestructionBatchProcessor();

	/**
	 * Queue a destruction request (not processed immediately).
	 */
	void QueueDestruction(const FCellDestructionShape& Shape);

	/**
	 * Tick processing (check batch interval).
	 * @return True if a batch was processed
	 */
	bool Tick(float DeltaTime);

	/**
	 * Force processing of the current queue (immediate handling).
	 */
	void FlushQueue();

	/**
	 * Get the last batch result (call after processing).
	 */
	const FBatchedDestructionEvent& GetLastBatchResult() const { return LastBatchResult; }

	/**
	 * Check if there are pending destructions.
	 */
	bool HasPendingDestructions() const { return PendingDestructions.Num() > 0; }

	/**
	 * Set processing context (must be called before batching).
	 */
	void SetContext(
		const FGridCellLayout* InCache,
		FCellState* InCellState,
		const FTransform& InMeshTransform);

private:
	/** Actual batch processing. */
	void ProcessBatch();

	/** Destruction requests accumulated over 16.6ms. */
	TArray<FQuantizedDestructionInput> PendingDestructions;

	/** Accumulated timer. */
	float AccumulatedTime;

	/** Last batch result. */
	FBatchedDestructionEvent LastBatchResult;

	/** Processing context. */
	const FGridCellLayout* LayoutPtr;
	FCellState* CellStatePtr;
	FTransform MeshTransform;

	/** Debris ID counter. */
	int32 DebrisIdCounter;
};
