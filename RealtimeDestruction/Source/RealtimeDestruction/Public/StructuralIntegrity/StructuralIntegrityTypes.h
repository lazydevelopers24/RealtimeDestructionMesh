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
#include "StructuralIntegrityTypes.generated.h"

/**
 * Cell structural state
 */
UENUM(BlueprintType)
enum class ECellStructuralState : uint8
{
	Intact,      // Intact - Connected to anchor
	Destroyed,   // Destroyed - Cell destroyed and removed from graph
	Detached     // Detached - Connection to anchor severed, about to fall
};

/**
 * Stable Cell Identifier
 *
 * Since CellGraph node indices can change on graph rebuild,
 * use (ChunkId, CellId) pair as stable external identifier.
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FCellKey
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CellKey")
	int32 ChunkId = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "CellKey")
	int32 CellId = INDEX_NONE;

	FCellKey() = default;
	FCellKey(int32 InChunkId, int32 InCellId) : ChunkId(InChunkId), CellId(InCellId) {}

	bool IsValid() const { return ChunkId != INDEX_NONE && CellId != INDEX_NONE; }

	bool operator==(const FCellKey& Other) const
	{
		return ChunkId == Other.ChunkId && CellId == Other.CellId;
	}

	bool operator!=(const FCellKey& Other) const
	{
		return !(*this == Other);
	}

	// For deterministic sorting (ChunkId first, then CellId if equal)
	bool operator<(const FCellKey& Other) const
	{
		if (ChunkId != Other.ChunkId)
		{
			return ChunkId < Other.ChunkId;
		}
		return CellId < Other.CellId;
	}

	friend uint32 GetTypeHash(const FCellKey& Key)
	{
		return HashCombine(GetTypeHash(Key.ChunkId), GetTypeHash(Key.CellId));
	}

	FString ToString() const
	{
		return FString::Printf(TEXT("(%d,%d)"), ChunkId, CellId);
	}
};

/**
 * Detached Cell Group
 *
 * Collection of cells whose connection to anchors has been severed.
 * IntegritySystem fills only CellIds; geometric info (CenterOfMass, TriangleIds)
 * is filled at higher level via CellGraph.
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedCellGroup
{
	GENERATED_BODY()

	// Unique ID of this group
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	int32 GroupId = INDEX_NONE;

	// Contained cell ID list (filled by IntegritySystem, for legacy compatibility)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<int32> CellIds;

	// Contained cell key list (filled by IntegritySystem, new API)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<FCellKey> CellKeys;

	// Center of mass of the group (filled at higher level via CellGraph)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	FVector CenterOfMass = FVector::ZeroVector;

	// Approximate mass of the group (based on cell count)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	float ApproximateMass = 0.0f;

	// Contained triangle ID list (filled at higher level via CellGraph)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<int32> TriangleIds;
};

/**
 * Structural Integrity Settings
 *
 * Anchor detection is handled by CellGraph, so only performance/behavior settings here.
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FStructuralIntegritySettings
{
	GENERATED_BODY()

	// Cell threshold for running connectivity check asynchronously
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance",
		meta = (ClampMin = "100"))
	int32 AsyncThreshold = 1000;

	// Enable async processing
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance")
	bool bEnableAsync = true;

	// Enable parallel processing (ParallelFor)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance")
	bool bEnableParallel = true;

	// Parallel processing threshold
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance",
		meta = (EditCondition = "bEnableParallel", ClampMin = "100"))
	int32 ParallelThreshold = 500;

	// Collapse delay (0 for immediate)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Collapse",
		meta = (ClampMin = "0.0"))
	float CollapseDelay = 0.0f;
};

/**
 * Structural Integrity Runtime Data (Non-USTRUCT, pure C++)
 *
 * No network sync needed - CellStructure is deterministic so
 * same Seed + same Hit order = same result
 */
struct REALTIMEDESTRUCTION_API FStructuralIntegrityData
{
	// Anchor cell ID set
	TSet<int32> AnchorCellIds;

	// State of each cell
	TArray<ECellStructuralState> CellStates;

	// Destroyed cell ID set (for fast lookup)
	TSet<int32> DestroyedCellIds;

	// Cells connected to anchor (cache)
	TSet<int32> ConnectedToAnchorCache;

	// Whether cache is valid
	bool bCacheValid = false;

	/**
	 * Initialize
	 * @param CellCount - Total cell count
	 */
	void Initialize(int32 CellCount)
	{
		CellStates.SetNum(CellCount);
		for (int32 i = 0; i < CellCount; ++i)
		{
			CellStates[i] = ECellStructuralState::Intact;
		}

		AnchorCellIds.Reset();
		DestroyedCellIds.Reset();
		ConnectedToAnchorCache.Reset();
		bCacheValid = false;
	}

	void Reset()
	{
		AnchorCellIds.Reset();
		CellStates.Reset();
		DestroyedCellIds.Reset();
		ConnectedToAnchorCache.Reset();
		bCacheValid = false;
	}

	int32 GetCellCount() const
	{
		return CellStates.Num();
	}

	bool IsValidCellId(int32 CellId) const
	{
		return CellId >= 0 && CellId < CellStates.Num();
	}

	void InvalidateCache()
	{
		bCacheValid = false;
	}
};

/**
 * Hit Event (for network sync, compressed)
 * Similar to existing FCompactDestructionOp, transmits only minimal data
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FStructuralHitEvent
{
	GENERATED_BODY()

	// Cell ID list to destroy
	UPROPERTY()
	TArray<int32> DestroyedCellIds;

	// Sequence number (ensures deterministic order)
	UPROPERTY()
	uint16 Sequence = 0;

	FStructuralHitEvent() = default;

	FStructuralHitEvent(const TArray<int32>& InCellIds, uint16 InSequence)
		: DestroyedCellIds(InCellIds)
		, Sequence(InSequence)
	{
	}
};

/**
 * Structural Integrity Change Result
 * Struct containing the result of ProcessHit
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FStructuralIntegrityResult
{
	GENERATED_BODY()

	// Newly destroyed cell ID list
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	TArray<int32> NewlyDestroyedCellIds;

	// Detached group list
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	TArray<FDetachedCellGroup> DetachedGroups;

	// Whether total collapse occurred (all anchors destroyed)
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	bool bStructureCollapsed = false;

	// Total destroyed cell count
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	int32 TotalDestroyedCount = 0;

	bool HasChanges() const
	{
		return NewlyDestroyedCellIds.Num() > 0 || DetachedGroups.Num() > 0;
	}
};

/**
 * Graph Snapshot - Per-node neighbor list (wrapper for UPROPERTY support)
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FStructuralIntegrityNeighborList
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCellKey> Neighbors;

	FStructuralIntegrityNeighborList() = default;
	FStructuralIntegrityNeighborList(const TArray<FCellKey>& InNeighbors) : Neighbors(InNeighbors) {}
};

/**
 * Graph Snapshot
 *
 * Struct for passing current CellGraph state to IntegritySystem.
 * All arrays are kept sorted for determinism.
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FStructuralIntegrityGraphSnapshot
{
	GENERATED_BODY()

	// Sorted node key list (ChunkId, CellId ascending)
	UPROPERTY()
	TArray<FCellKey> NodeKeys;

	// Neighbor key list per node (same index as NodeKeys)
	UPROPERTY()
	TArray<FStructuralIntegrityNeighborList> NeighborKeys;

	// Anchor node key list
	UPROPERTY()
	TArray<FCellKey> AnchorKeys;

	int32 GetNodeCount() const { return NodeKeys.Num(); }

	bool IsValid() const { return NodeKeys.Num() > 0 && NodeKeys.Num() == NeighborKeys.Num(); }

	void Reset()
	{
		NodeKeys.Reset();
		NeighborKeys.Reset();
		AnchorKeys.Reset();
	}
};
