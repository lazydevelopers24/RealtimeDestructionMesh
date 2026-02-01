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
#include "StructuralIntegrity/StructuralIntegrityTypes.h"

/**
 * Structural Integrity System Initialization Data
 *
 * Contains only connectivity graph info converted from CellGraph.
 * Geometric info (position, triangles) of cells is held by CellGraph.
 */
struct REALTIMEDESTRUCTION_API FStructuralIntegrityInitData
{
	// Neighbor cell ID list per cell (graph connectivity)
	TArray<TArray<int32>> CellNeighbors;

	// Cell ID list to designate as anchors (determined by CellGraph)
	TArray<int32> AnchorCellIds;

	int32 GetCellCount() const { return CellNeighbors.Num(); }

	bool IsValid() const
	{
		return CellNeighbors.Num() > 0;
	}
};

/**
 * Structural Integrity Core System
 *
 * Features:
 * - Pure C++ (not UObject)
 * - Thread-safe (read/write locks)
 * - Deterministic (same input -> same output)
 *
 * Usage:
 * 1. Initialize() with FStructuralIntegrityInitData
 * 2. Set anchors via AutoDetectFloorAnchors() or SetAnchor()
 * 3. Call DestroyCells() to destroy cells
 * 4. Process debris via DetachedGroups in FStructuralIntegrityResult
 */
class REALTIMEDESTRUCTION_API FStructuralIntegritySystem
{
public:
	FStructuralIntegritySystem() = default;
	~FStructuralIntegritySystem() = default;

	// Non-copyable and non-movable (protect internal state)
	FStructuralIntegritySystem(const FStructuralIntegritySystem&) = delete;
	FStructuralIntegritySystem& operator=(const FStructuralIntegritySystem&) = delete;

	//=========================================================================
	// Initialization
	//=========================================================================

	/**
	 * Initialize system from initialization data
	 * @param InitData - Cell connectivity graph and anchor info
	 * @param Settings - Structural integrity settings
	 */
	void Initialize(const FStructuralIntegrityInitData& InitData, const FStructuralIntegritySettings& Settings);

	/** Reset */
	void Reset();

	/** Whether initialized */
	bool IsInitialized() const { return bInitialized; }

	/** Cell count */
	int32 GetCellCount() const;

	//=========================================================================
	// Anchor Management
	//=========================================================================

	/**
	 * Set/unset a specific cell as anchor
	 * @param CellId - Cell ID
	 * @param bIsAnchor - Set as anchor if true
	 */
	void SetAnchor(int32 CellId, bool bIsAnchor);

	/**
	 * Set multiple cells as anchors
	 * @param CellIds - Cell ID list
	 * @param bIsAnchor - Set as anchor if true
	 */
	void SetAnchors(const TArray<int32>& CellIds, bool bIsAnchor);

	/** Get anchor list (thread-safe) */
	TArray<int32> GetAnchorCellIds() const;

	/** Check if cell is anchor (thread-safe) */
	bool IsAnchor(int32 CellId) const;

	/** Anchor count */
	int32 GetAnchorCount() const;

	//=========================================================================
	// Cell Destruction
	//=========================================================================

	/**
	 * Destroy specified cells and update connectivity
	 * @param CellIds - Cell ID list to destroy
	 * @return Processing result (destroyed cells, detached groups, etc.)
	 */
	FStructuralIntegrityResult DestroyCells(const TArray<int32>& CellIds);

	/**
	 * Destroy single cell
	 * @param CellId - Cell ID to destroy
	 * @return Processing result
	 */
	FStructuralIntegrityResult DestroyCell(int32 CellId);

	//=========================================================================
	// State Query (Thread-safe)
	//=========================================================================

	/** Get cell state */
	ECellStructuralState GetCellState(int32 CellId) const;

	/** Check if cell is connected to anchor */
	bool IsCellConnectedToAnchor(int32 CellId) const;

	/** Destroyed cell count */
	int32 GetDestroyedCellCount() const;

	/** Destroyed cell ID list (for network sync) */
	TArray<int32> GetDestroyedCellIds() const;

	//=========================================================================
	// Force State Setting (For Network Sync)
	//=========================================================================

	/**
	 * State sync for late-joining clients
	 * @param DestroyedIds - Destroyed cell ID list
	 * @return Detached group list
	 */
	TArray<FDetachedCellGroup> ForceSetDestroyedCells(const TArray<int32>& DestroyedIds);

	//=========================================================================
	// Settings Access
	//=========================================================================

	const FStructuralIntegritySettings& GetSettings() const { return Settings; }
	void SetSettings(const FStructuralIntegritySettings& NewSettings);

	//=========================================================================
	// Graph Sync API (New)
	//=========================================================================

	/**
	 * Sync internal state with graph snapshot
	 * - New key: Allocate new ID, Intact state
	 * - Key not in snapshot: Mark as Destroyed (keep ID)
	 * - Rebuild neighbor lists
	 * @param Snapshot - Snapshot generated from CellGraph
	 */
	void SyncGraph(const FStructuralIntegrityGraphSnapshot& Snapshot);

	/**
	 * Recalculate connectivity via BFS, return detached groups
	 * @return Result containing detached group info
	 */
	FStructuralIntegrityResult RefreshConnectivity();

	/**
	 * Mark cells as Destroyed (call after debris spawn)
	 * @param Keys - Cell key list to mark as Destroyed
	 */
	void MarkCellsAsDestroyed(const TArray<FCellKey>& Keys);

	//=========================================================================
	// Key-based Query API
	//=========================================================================

	/** Get internal ID by key (INDEX_NONE if not found) */
	int32 GetCellIdForKey(const FCellKey& Key) const;

	/** Get key by internal ID */
	FCellKey GetKeyForCellId(int32 CellId) const;

	/** Key list of destroyed cells */
	TArray<FCellKey> GetDestroyedCellKeys() const;

private:
	//=========================================================================
	// Internal Algorithms
	//=========================================================================

	/**
	 * Single cell destruction processing (internal)
	 * @param CellId - Cell to destroy
	 * @return Whether destruction succeeded (false if already destroyed or invalid)
	 */
	bool DestroyCellInternal(int32 CellId);

	/**
	 * Find detached groups after connectivity update
	 * @return Detached group list
	 */
	TArray<FDetachedCellGroup> UpdateConnectivityAndFindDetached();

	/**
	 * Find all cells connected from anchors (BFS)
	 * Must be called within lock
	 * @return Connected cell set
	 */
	TSet<int32> FindAllConnectedToAnchors_Internal() const;

	/**
	 * Group detached cells into connected groups
	 * @param DetachedCellIds - Detached cell ID list
	 * @return Group list (contains only CellIds, geometric info from CellGraph)
	 */
	TArray<FDetachedCellGroup> BuildDetachedGroups(const TArray<int32>& DetachedCellIds);

	/**
	 * Get/allocate internal ID for key
	 * @param Key - Cell key
	 * @param bCreateIfNotFound - Allocate new ID if not found when true
	 * @return Internal ID (INDEX_NONE if not found and not creating)
	 */
	int32 FindOrAllocateCellId(const FCellKey& Key, bool bCreateIfNotFound = true);

	/**
	 * Rebuild neighbor lists based on snapshot
	 * @param Snapshot - Graph snapshot
	 */
	void RebuildNeighborLists(const FStructuralIntegrityGraphSnapshot& Snapshot);

	//=========================================================================
	// Data
	//=========================================================================

	// Settings
	FStructuralIntegritySettings Settings;

	// Runtime data
	FStructuralIntegrityData Data;

	// Cell connectivity (copied on Initialize, can be dynamically modified via DisconnectCells)
	TArray<TArray<int32>> CellNeighbors;

	// Initialization state
	bool bInitialized = false;

	// Thread synchronization
	mutable FRWLock DataLock;

	// Detached group ID counter
	int32 NextGroupId = 0;

	//=========================================================================
	// Key <-> ID Mapping (New)
	//=========================================================================

	// Key -> Internal ID
	TMap<FCellKey, int32> KeyToId;

	// Internal ID -> Key
	TArray<FCellKey> IdToKey;

	// Next internal ID to allocate (monotonically increasing, no reuse)
	int32 NextInternalId = 0;
};
