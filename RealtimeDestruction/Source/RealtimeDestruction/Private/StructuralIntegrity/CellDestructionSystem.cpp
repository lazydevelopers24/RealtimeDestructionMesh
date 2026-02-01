// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "StructuralIntegrity/CellDestructionSystem.h"
#include "Containers/Queue.h"
#include "StructuralIntegrity/SubCellProcessor.h"
//=============================================================================
// FCellDestructionSystem - SubCell level API
//=============================================================================

FDestructionResult FCellDestructionSystem::ProcessCellDestructionSubCellLevel(
	const FGridCellLayout& GridLayout,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform,
	FCellState& InOutCellState)
{
	FDestructionResult Result;

	if (!GridLayout.IsValid())
	{
		return Result;
	}

	// 1. SubCell destruction via SubCellProcessor
	TArray<int32> AffectedCells;
	TMap<int32, TArray<int32>> NewlyDeadSubCells;

	FSubCellProcessor::ProcessSubCellDestruction(
		Shape,
		MeshTransform,
		GridLayout,
		InOutCellState,
		AffectedCells,
		&NewlyDeadSubCells
	);

	// 2. Collect results
	Result.AffectedCells = MoveTemp(AffectedCells);

	// NewlyDeadSubCells (TMap<int32, TArray<int32>>) -> FDestructionResult.NewlyDeadSubCells (TMap<int32, FIntArray>)
	for (auto& Pair : NewlyDeadSubCells)
	{
		FIntArray SubCellArray;
		SubCellArray.Values = MoveTemp(Pair.Value);
		Result.DeadSubCellCount += SubCellArray.Num();
		Result.NewlyDeadSubCells.Add(Pair.Key, MoveTemp(SubCellArray));
	}

	// 3. Collect fully destroyed cells (already added to DestroyedCells in SubCellProcessor)
	for (int32 CellId : Result.AffectedCells)
	{
		if (InOutCellState.DestroyedCells.Contains(CellId))
		{
			Result.NewlyDestroyedCells.Add(CellId);
		}
	}

	return Result;
}

//=============================================================================
// FCellDestructionSystem - Cell destruction evaluation (legacy cell-level API)
//=============================================================================

TArray<int32> FCellDestructionSystem::ProcessCellDestruction(
	const FGridCellLayout& GridLayout,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform,
	const TSet<int32>& DestroyedCells)
{
	TArray<int32> NewlyDestroyed;

	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); CellId++)
	{
		// Skip already destroyed or non-existent cells
		if (!GridLayout.GetCellExists(CellId) || DestroyedCells.Contains(CellId))
		{
			continue;
		}

		if (IsCellDestroyed(GridLayout, CellId, Shape, MeshTransform))
		{
			NewlyDestroyed.Add(CellId);
		}
	}

	return NewlyDestroyed;
}

FDestructionResult FCellDestructionSystem::ProcessCellDestruction(
	const FGridCellLayout& GridLayout,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform,
	FCellState& InOutCellState)
{
	FDestructionResult Result;
	Result.NewlyDestroyedCells = ProcessCellDestruction(GridLayout, Shape, MeshTransform, InOutCellState.DestroyedCells);
	InOutCellState.DestroyCells(Result.NewlyDestroyedCells);
	return Result;
}

bool FCellDestructionSystem::IsCellDestroyed(
	const FGridCellLayout& GridLayout,
	int32 CellId,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform)
{
	// Phase 1: center-point test (fast)
	const FVector WorldCenter = GridLayout.IdToWorldCenter(CellId, MeshTransform);
	if (Shape.ContainsPoint(WorldCenter))
	{
		return true;
	}

	// Phase 2: majority-of-vertices test (edge cases)
	const TArray<FVector> LocalVertices = GridLayout.GetCellVertices(CellId);
	int32 DestroyedVertices = 0;

	for (const FVector& LocalVertex : LocalVertices)
	{
		const FVector WorldVertex = MeshTransform.TransformPosition(LocalVertex);
		if (Shape.ContainsPoint(WorldVertex))
		{
			// Return immediately when majority (4) is reached
			if (++DestroyedVertices >= 4)
			{
				return true;
			}
		}
	}

	return false;
}

//=============================================================================
// FCellDestructionSystem - Structural integrity checks
//=============================================================================

TSet<int32> FCellDestructionSystem::FindDisconnectedCells(
	const FGridCellLayout& GridLayout,
	FSuperCellState& SupercellState,
	const FCellState& CellState,
	bool bEnableSupercell,
	bool bEnableSubcell,
	FConnectivityContext& Context)
{
	if (bEnableSupercell)
	{
		return FindDisconnectedCellsHierarchicalLevel(
			GridLayout,
			SupercellState,
			CellState,
			bEnableSubcell,
			Context);
	}
	if (bEnableSubcell)
	{
		return FindDisconnectedCellsSubCellLevel(
			GridLayout,
			CellState);
	}
	return FindDisconnectedCellsCellLevel(GridLayout, CellState.DestroyedCells);
}

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsCellLevel(
	const FGridCellLayout& GridLayout,
	const TSet<int32>& DestroyedCells)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindDisconnectedCellsCellLevel)
	TSet<int32> Connected;
	TQueue<int32> Queue;

	// 1. Start BFS from anchors
	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); CellId++)
	{
		if (GridLayout.GetCellExists(CellId) &&
		    GridLayout.GetCellIsAnchor(CellId) &&
		    !DestroyedCells.Contains(CellId))
		{
			Queue.Enqueue(CellId);
			Connected.Add(CellId);
		}
	}

	// 2. BFS traversal
	while (!Queue.IsEmpty())
	{
		int32 Current;
		Queue.Dequeue(Current);

		for (int32 Neighbor : GridLayout.GetCellNeighbors(Current))
		{
			if (!DestroyedCells.Contains(Neighbor) &&
			    !Connected.Contains(Neighbor))
			{
				Connected.Add(Neighbor);
				Queue.Enqueue(Neighbor);
			}
		}
	}

	// 3. Unconnected cells are detached
	TSet<int32> Disconnected;
	int32 ValidCellCount = 0;
	int32 AnchorCount = 0;
	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); CellId++)
	{
		if (GridLayout.GetCellExists(CellId))
		{
			ValidCellCount++;
			if (GridLayout.GetCellIsAnchor(CellId)) AnchorCount++;

			if (!DestroyedCells.Contains(CellId) &&
			    !Connected.Contains(CellId))
			{
				Disconnected.Add(CellId);
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("FindDisconnectedCellsCellLevel: Valid=%d, Anchor=%d, Destroyed=%d, Connected=%d, Disconnected=%d"),
		ValidCellCount, AnchorCount, DestroyedCells.Num(), Connected.Num(), Disconnected.Num());

	return Disconnected;
}

TArray<TArray<int32>> FCellDestructionSystem::GroupDetachedCells(
	const FGridCellLayout& GridLayout,
	const TSet<int32>& DisconnectedCells,
	const TSet<int32>& DestroyedCells)
{
	TArray<TArray<int32>> Groups;
	TSet<int32> Visited;

	//=========================================================================
	// Phase 1: Group disconnected cells via BFS
	//=========================================================================
	for (int32 StartCell : DisconnectedCells)
	{
		if (Visited.Contains(StartCell))
		{
			continue;
		}

		// Find a connected detached cell group via BFS
		TArray<int32> Group;
		TQueue<int32> Queue;

		Queue.Enqueue(StartCell);
		Visited.Add(StartCell);

		while (!Queue.IsEmpty())
		{
			int32 Current;
			Queue.Dequeue(Current);
			Group.Add(Current);

			for (int32 Neighbor : GridLayout.GetCellNeighbors(Current))
			{
				if (DisconnectedCells.Contains(Neighbor) &&
				    !Visited.Contains(Neighbor))
				{
					Visited.Add(Neighbor);
					Queue.Enqueue(Neighbor);
				}
			}
		}

		Groups.Add(MoveTemp(Group));
	}
	
	return Groups;
}

//=============================================================================
// FCellDestructionSystem - Utilities
//=============================================================================

FVector FCellDestructionSystem::CalculateGroupCenter(
	const FGridCellLayout& GridLayout,
	const TArray<int32>& CellIds,
	const FTransform& MeshTransform)
{
	if (CellIds.Num() == 0)
	{
		return FVector::ZeroVector;
	}

	FVector Sum = FVector::ZeroVector;
	for (int32 CellId : CellIds)
	{
		Sum += GridLayout.IdToWorldCenter(CellId, MeshTransform);
	}

	return Sum / CellIds.Num();
}

FVector FCellDestructionSystem::CalculateDebrisVelocity(
	const FVector& DebrisCenter,
	const TArray<FQuantizedDestructionInput>& DestructionInputs,
	float BaseSpeed)
{
	if (DestructionInputs.Num() == 0)
	{
		return FVector::ZeroVector;
	}

	// Find the closest destruction input
	float MinDistSq = MAX_FLT;
	FVector ClosestCenter = FVector::ZeroVector;

	for (const auto& Input : DestructionInputs)
	{
		const FVector Center = FVector(Input.CenterMM.X, Input.CenterMM.Y, Input.CenterMM.Z) * 0.1f;
		const float DistSq = FVector::DistSquared(DebrisCenter, Center);

		if (DistSq < MinDistSq)
		{
			MinDistSq = DistSq;
			ClosestCenter = Center;
		}
	}

	// Velocity in the explosion direction
	const FVector Direction = (DebrisCenter - ClosestCenter).GetSafeNormal();
	return Direction * BaseSpeed;
}

bool FCellDestructionSystem::IsBoundaryCell(
	const FGridCellLayout& GridLayout,
	int32 CellId,
	const TSet<int32>& DestroyedCells)
{
	for (int32 Neighbor : GridLayout.GetCellNeighbors(CellId))
	{
		if (DestroyedCells.Contains(Neighbor))
		{
			return true;  // Adjacent to a destroyed cell = boundary
		}
	}
	return false;
}

//=============================================================================
// FDestructionBatchProcessor
//=============================================================================

FDestructionBatchProcessor::FDestructionBatchProcessor()
	: AccumulatedTime(0.0f)
	, LayoutPtr(nullptr)
	, CellStatePtr(nullptr)
	, MeshTransform(FTransform::Identity)
	, DebrisIdCounter(0)
{
}

void FDestructionBatchProcessor::QueueDestruction(const FCellDestructionShape& Shape)
{
	// Store quantized
	PendingDestructions.Add(FQuantizedDestructionInput::FromDestructionShape(Shape));
}

bool FDestructionBatchProcessor::Tick(float DeltaTime)
{
	AccumulatedTime += DeltaTime;

	if (AccumulatedTime >= BatchInterval && PendingDestructions.Num() > 0)
	{
		AccumulatedTime = 0.0f;
		ProcessBatch();
		return true;
	}

	return false;
}

void FDestructionBatchProcessor::FlushQueue()
{
	if (PendingDestructions.Num() > 0)
	{
		ProcessBatch();
		AccumulatedTime = 0.0f;
	}
}

void FDestructionBatchProcessor::SetContext(
	const FGridCellLayout* InLayout,
	FCellState* InCellState,
	const FTransform& InMeshTransform)
{
	LayoutPtr = InLayout;
	CellStatePtr = InCellState;
	MeshTransform = InMeshTransform;
}

void FDestructionBatchProcessor::ProcessBatch()
{
	if (!LayoutPtr || !CellStatePtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("FDestructionBatchProcessor: Context not set"));
		PendingDestructions.Empty();
		return;
	}

	// Initialize results
	LastBatchResult = FBatchedDestructionEvent();
	LastBatchResult.DestructionInputs = PendingDestructions;

	//=====================================================
	// Phase 1: Evaluate cells for all destruction inputs
	//=====================================================
	TSet<int32> NewlyDestroyed;

	for (const auto& Input : PendingDestructions)
	{
		TArray<int32> Cells = FCellDestructionSystem::ProcessCellDestruction(
			*LayoutPtr, Input, MeshTransform, CellStatePtr->DestroyedCells);

		for (int32 CellId : Cells)
		{
			NewlyDestroyed.Add(CellId);
		}
	}

	if (NewlyDestroyed.Num() == 0)
	{
		PendingDestructions.Empty();
		return;
	}

	//=====================================================
	// Phase 2: Update cell state
	//=====================================================
	for (int32 CellId : NewlyDestroyed)
	{
		CellStatePtr->DestroyedCells.Add(CellId);
	}

	//=====================================================
	// Phase 3: Run BFS once (core of batching)
	//=====================================================
	TSet<int32> Disconnected = FCellDestructionSystem::FindDisconnectedCellsCellLevel(
		*LayoutPtr, CellStatePtr->DestroyedCells);

	TArray<TArray<int32>> DetachedGroups = FCellDestructionSystem::GroupDetachedCells(
		*LayoutPtr, Disconnected, CellStatePtr->DestroyedCells);

	//=====================================================
	// Phase 4: Destroy detached cells as well
	//=====================================================
	for (const auto& Group : DetachedGroups)
	{
		for (int32 CellId : Group)
		{
			CellStatePtr->DestroyedCells.Add(CellId);
		}
	}

	//=====================================================
	// Phase 5: Create events
	//=====================================================
	for (int32 CellId : NewlyDestroyed)
	{
		LastBatchResult.DestroyedCellIds.Add((int16)CellId);
	}

	// Create debris info
	for (const auto& Group : DetachedGroups)
	{
		FDetachedDebrisInfo DebrisInfo;
		DebrisInfo.DebrisId = ++DebrisIdCounter;

		for (int32 CellId : Group)
		{
			DebrisInfo.CellIds.Add((int16)CellId);
			LastBatchResult.DestroyedCellIds.Add((int16)CellId);
		}

		DebrisInfo.InitialLocation = FCellDestructionSystem::CalculateGroupCenter(
			*LayoutPtr, Group, MeshTransform);

		DebrisInfo.InitialVelocity = FCellDestructionSystem::CalculateDebrisVelocity(
			DebrisInfo.InitialLocation, PendingDestructions);

		LastBatchResult.DetachedDebris.Add(DebrisInfo);
	}

	// Clear queue
	PendingDestructions.Empty();

	UE_LOG(LogTemp, Log, TEXT("FDestructionBatchProcessor: Processed %d destroyed cells, %d debris groups"),
		LastBatchResult.DestroyedCellIds.Num(), LastBatchResult.DetachedDebris.Num());
}

//=============================================================================
// FCellDestructionSystem - Subcell-level connectivity check (2x2x2 optimization)
//=============================================================================

namespace SubCellBFSHelper
{
	/**
	 * Boundary subcell pair table (2x2x2 only).
	 * Four pairs per direction: (current cell subcell, neighbor cell subcell).
	 *
	 * SubCell layout:
	 *   Z=0: 0(0,0,0), 1(1,0,0), 2(0,1,0), 3(1,1,0)
	 *   Z=1: 4(0,0,1), 5(1,0,1), 6(0,1,1), 7(1,1,1)
	 */
	struct FBoundarySubCellPair
	{
		int32 Current;   // Boundary subcell in the current cell
		int32 Neighbor;  // Corresponding subcell in the neighbor cell
	};

	// +X direction: X=1 (1,3,5,7) -> neighbor X=0 (0,2,4,6)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_POS_X[4] = {
		{1, 0}, {3, 2}, {5, 4}, {7, 6}
	};

	// -X direction: X=0 (0,2,4,6) -> neighbor X=1 (1,3,5,7)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_NEG_X[4] = {
		{0, 1}, {2, 3}, {4, 5}, {6, 7}
	};

	// +Y direction: Y=1 (2,3,6,7) -> neighbor Y=0 (0,1,4,5)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_POS_Y[4] = {
		{2, 0}, {3, 1}, {6, 4}, {7, 5}
	};

	// -Y direction: Y=0 (0,1,4,5) -> neighbor Y=1 (2,3,6,7)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_NEG_Y[4] = {
		{0, 2}, {1, 3}, {4, 6}, {5, 7}
	};

	// +Z direction: Z=1 (4,5,6,7) -> neighbor Z=0 (0,1,2,3)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_POS_Z[4] = {
		{4, 0}, {5, 1}, {6, 2}, {7, 3}
	};

	// -Z direction: Z=0 (0,1,2,3) -> neighbor Z=1 (4,5,6,7)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_NEG_Z[4] = {
		{0, 4}, {1, 5}, {2, 6}, {3, 7}
	};

	/**
	 * Return boundary subcell pair array for a direction.
	 * @param Direction - 0:-X, 1:+X, 2:-Y, 3:+Y, 4:-Z, 5:+Z
	 */
	inline const FBoundarySubCellPair* GetBoundaryPairs(int32 Direction)
	{
		switch (Direction)
		{
		case 0: return BOUNDARY_PAIRS_NEG_X;
		case 1: return BOUNDARY_PAIRS_POS_X;
		case 2: return BOUNDARY_PAIRS_NEG_Y;
		case 3: return BOUNDARY_PAIRS_POS_Y;
		case 4: return BOUNDARY_PAIRS_NEG_Z;
		case 5: return BOUNDARY_PAIRS_POS_Z;
		default: return nullptr;
		}
	}

	/**
	 * Check if there is any connected boundary subcell pair between two cells.
	 * @param Direction - direction from CellA to CellB (0-5)
	 * @return True if any boundary pair is alive on both sides
	 */
	bool HasConnectedBoundary(
		int32 CellA,
		int32 CellB,
		int32 Direction,
		const FCellState& CellState)
	{
		const FBoundarySubCellPair* Pairs = GetBoundaryPairs(Direction);
		if (!Pairs)
		{
			return false;
		}

		for (int32 i = 0; i < 4; ++i)
		{
			if (CellState.IsSubCellAlive(CellA, Pairs[i].Current) &&
				CellState.IsSubCellAlive(CellB, Pairs[i].Neighbor))
			{
				return true;  // Connected pair found
			}
		}

		return false;  // No connection
	}

	/**
	 * Check if a cell has any alive subcell.
	 */
	bool HasAliveSubCell(int32 CellId, const FCellState& CellState)
	{
		if (CellState.DestroyedCells.Contains(CellId))
		{
			return false;
		}

		const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);
		if (!SubCellState)
		{
			return true;  // If no state, all subcells are alive
		}

		return !SubCellState->IsFullyDestroyed();
	}

	/**
	 * Check anchor reachability via cell-level BFS (2x2x2 optimization).
	 *
	 * In 2x2x2, all subcells within a cell are connected, so we traverse at the cell level
	 * and only check boundary connectivity at the subcell level.
	 *
	 * @param GridLayout - grid layout
	 * @param CellState - cell state
	 * @param StartCellId - start cell
	 * @param ConfirmedConnected - cells already confirmed as connected
	 * @param OutVisitedCells - visited cell set (output)
	 * @return Whether an anchor is reachable
	 */
	bool PerformSubCellBFS(
		const FGridCellLayout& GridLayout,
		const FCellState& CellState,
		int32 StartCellId,
		const TSet<int32>& ConfirmedConnected,
		TSet<int32>& OutVisitedCells)
	{
		OutVisitedCells.Reset();

		// Check if the start cell has any alive subcell
		if (!HasAliveSubCell(StartCellId, CellState))
		{
			return false;
		}

		// Cell-level BFS
		TQueue<int32> CellQueue;
		TSet<int32> VisitedCells;

		CellQueue.Enqueue(StartCellId);
		VisitedCells.Add(StartCellId);
		OutVisitedCells.Add(StartCellId);

		while (!CellQueue.IsEmpty())
		{
			int32 CurrCellId;
			CellQueue.Dequeue(CurrCellId);

			// Check if an anchor cell is reached
			if (GridLayout.GetCellIsAnchor(CurrCellId))
			{
				return true;
			}

			// Reached a cell already confirmed as connected
			if (ConfirmedConnected.Contains(CurrCellId))
			{
				return true;
			}

			// Explore 6-direction neighbor cells
			const FIntVector CurrCoord = GridLayout.IdToCoord(CurrCellId);

			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				const FIntVector NeighborCoord = CurrCoord + FIntVector(
					DIRECTION_OFFSETS[Dir][0],
					DIRECTION_OFFSETS[Dir][1],
					DIRECTION_OFFSETS[Dir][2]
				);

				if (!GridLayout.IsValidCoord(NeighborCoord))
				{
					continue;
				}

				const int32 NeighborCellId = GridLayout.CoordToId(NeighborCoord);

				// Skip already visited or invalid cells
				if (VisitedCells.Contains(NeighborCellId))
				{
					continue;
				}

				if (!GridLayout.GetCellExists(NeighborCellId))
				{
					continue;
				}

				if (CellState.DestroyedCells.Contains(NeighborCellId))
				{
					continue;
				}

				// Check boundary subcell connectivity
				if (HasConnectedBoundary(CurrCellId, NeighborCellId, Dir, CellState))
				{
					VisitedCells.Add(NeighborCellId);
					CellQueue.Enqueue(NeighborCellId);
					OutVisitedCells.Add(NeighborCellId);
				}
			}
		}

		// Failed to reach an anchor
		return false;
	}

	/**
	 * Subcell internal adjacency table (2x2x2 only, 6 directions).
	 * For each subcell, adjacent subcell IDs in 6 directions (-1 if none).
	 * Order: -X, +X, -Y, +Y, -Z, +Z
	 */
	inline constexpr int32 SUBCELL_ADJACENCY[8][6] = {
		// SubCell 0 (0,0,0): -X=none, +X=1, -Y=none, +Y=2, -Z=none, +Z=4
		{-1, 1, -1, 2, -1, 4},
		// SubCell 1 (1,0,0): -X=0, +X=none, -Y=none, +Y=3, -Z=none, +Z=5
		{0, -1, -1, 3, -1, 5},
		// SubCell 2 (0,1,0): -X=none, +X=3, -Y=0, +Y=none, -Z=none, +Z=6
		{-1, 3, 0, -1, -1, 6},
		// SubCell 3 (1,1,0): -X=2, +X=none, -Y=1, +Y=none, -Z=none, +Z=7
		{2, -1, 1, -1, -1, 7},
		// SubCell 4 (0,0,1): -X=none, +X=5, -Y=none, +Y=6, -Z=0, +Z=none
		{-1, 5, -1, 6, 0, -1},
		// SubCell 5 (1,0,1): -X=4, +X=none, -Y=none, +Y=7, -Z=1, +Z=none
		{4, -1, -1, 7, 1, -1},
		// SubCell 6 (0,1,1): -X=none, +X=7, -Y=4, +Y=none, -Z=2, +Z=none
		{-1, 7, 4, -1, 2, -1},
		// SubCell 7 (1,1,1): -X=6, +X=none, -Y=5, +Y=none, -Z=3, +Z=none
		{6, -1, 5, -1, 3, -1},
	};

	/**
	 * Return the opposite direction.
	 * 0(-X) <-> 1(+X), 2(-Y) <-> 3(+Y), 4(-Z) <-> 5(+Z)
	 */
	inline constexpr int32 GetOppositeDirection(int32 Direction)
	{
		return Direction ^ 1;  // 0<->1, 2<->3, 4<->5
	}

	/**
	 * Return boundary subcell IDs for a direction (4 entries).
	 * @param Direction - 0:-X, 1:+X, 2:-Y, 3:+Y, 4:-Z, 5:+Z
	 */
	inline void GetBoundarySubCellIds(int32 Direction, int32 OutIds[4])
	{
		const FBoundarySubCellPair* Pairs = GetBoundaryPairs(Direction);
		if (Pairs)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				OutIds[i] = Pairs[i].Current;
			}
		}
	}

	/**
	 * Flood subcells from a detached cell boundary into a connected cell.
	 * Starts at boundary subcells and expands until hitting dead subcells.
	 *
	 * @param CellState - cell state
	 * @param ConnectedCellId - connected cell ID
	 * @param DirectionFromDetached - direction from detached to connected (0-5)
	 * @return Flooded subcell ID list
	 */
	TArray<int32> FloodSubCellsFromBoundary(
		const FCellState& CellState,
		int32 ConnectedCellId,
		int32 DirectionFromDetached)
	{
		TArray<int32> Result;

		// Opposite of detached->connected direction = face touching the detached cell
		const int32 BoundaryDirection = GetOppositeDirection(DirectionFromDetached);

		// Get boundary subcell IDs
		int32 BoundarySubCellIds[4];
		GetBoundarySubCellIds(BoundaryDirection, BoundarySubCellIds);

		// BFS data structures
		TSet<int32> Visited;
		TQueue<int32> Queue;

		// Add boundary subcells as starting points
		for (int32 i = 0; i < 4; ++i)
		{
			const int32 SubCellId = BoundarySubCellIds[i];
			if (!Visited.Contains(SubCellId))
			{
				Visited.Add(SubCellId);
				Queue.Enqueue(SubCellId);
			}
		}

		// BFS traversal
		while (!Queue.IsEmpty())
		{
			int32 CurrentSubCellId;
			Queue.Dequeue(CurrentSubCellId);

			const bool bIsAlive = CellState.IsSubCellAlive(ConnectedCellId, CurrentSubCellId);

			// Add to result (alive or dead)
			Result.Add(CurrentSubCellId);

			// If subcell is dead, stop expanding (acts as boundary)
			if (!bIsAlive)
			{
				continue;
			}

			// If subcell is alive, expand to adjacent subcells
			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				const int32 NeighborSubCellId = SUBCELL_ADJACENCY[CurrentSubCellId][Dir];

				// Skip invalid or already visited subcells
				if (NeighborSubCellId < 0 || Visited.Contains(NeighborSubCellId))
				{
					continue;
				}

				Visited.Add(NeighborSubCellId);
				Queue.Enqueue(NeighborSubCellId);
			}
		}

		return Result;
	}

	/**
	 * Boundary cell info for a detached group.
	 */
	struct FBoundaryCellInfo
	{
		int32 BoundaryCellId = INDEX_NONE;
		TArray<TPair<int32, int32>> AdjacentConnectedCells;  // (CellId, Direction)
	};

	/**
	 * Extract boundary cells from a detached group (includes adjacent connected cells).
	 */
	TArray<FBoundaryCellInfo> GetGroupBoundaryCellsWithAdjacency(
		const FGridCellLayout& GridLayout,
		const TArray<int32>& GroupCellIds,
		const FCellState& CellState)
	{
		TArray<FBoundaryCellInfo> Result;

		// Convert group cells to a set for fast lookup
		TSet<int32> GroupCellSet;
		GroupCellSet.Reserve(GroupCellIds.Num());
		for (int32 CellId : GroupCellIds)
		{
			GroupCellSet.Add(CellId);
		}

		// Determine boundary status for each group cell
		for (int32 CellId : GroupCellIds)
		{
			FBoundaryCellInfo Info;
			Info.BoundaryCellId = CellId;

			const FIntVector CellCoord = GridLayout.IdToCoord(CellId);

			// Check 6-direction neighbors
			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				const FIntVector NeighborCoord = CellCoord + FIntVector(
					DIRECTION_OFFSETS[Dir][0],
					DIRECTION_OFFSETS[Dir][1],
					DIRECTION_OFFSETS[Dir][2]
				);

				// Skip invalid coordinates
				if (!GridLayout.IsValidCoord(NeighborCoord))
				{
					continue;
				}

				const int32 NeighborCellId = GridLayout.CoordToId(NeighborCoord);

				// Skip cells inside the group
				if (GroupCellSet.Contains(NeighborCellId))
				{
					continue;
				}

				// Skip non-existent cells
				if (!GridLayout.GetCellExists(NeighborCellId))
				{
					continue;
				}

				// Skip destroyed cells (only connected cells are considered)
				if (CellState.DestroyedCells.Contains(NeighborCellId))
				{
					continue;
				}

				// Found a connected cell -> add to adjacency list
				Info.AdjacentConnectedCells.Add(TPair<int32, int32>(NeighborCellId, Dir));
			}

			// If any adjacent connected cell exists, it's a boundary cell
			if (Info.AdjacentConnectedCells.Num() > 0)
			{
				Result.Add(MoveTemp(Info));
			}
		}

		return Result;
	}
}

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsSubCellLevel(
	const FGridCellLayout& GridLayout,
	const FCellState& CellState)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindDisconnectedCellsSubCellLevel);
	using namespace SubCellBFSHelper;

	TSet<int32> Connected;

	// 1. Start BFS from all anchor cells
	TQueue<int32> Queue;
	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); CellId++)
	{
		if (GridLayout.GetCellExists(CellId) &&
			GridLayout.GetCellIsAnchor(CellId) &&
			!CellState.DestroyedCells.Contains(CellId) &&
			HasAliveSubCell(CellId, CellState))
		{
			Queue.Enqueue(CellId);
			Connected.Add(CellId);
		}
	}

	// 2. BFS: find all cells reachable via subcell boundary connectivity
	while (!Queue.IsEmpty())
	{
		int32 CurrCellId;
		Queue.Dequeue(CurrCellId);

		const FIntVector CurrCoord = GridLayout.IdToCoord(CurrCellId);

		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const FIntVector NeighborCoord = CurrCoord + FIntVector(
				DIRECTION_OFFSETS[Dir][0],
				DIRECTION_OFFSETS[Dir][1],
				DIRECTION_OFFSETS[Dir][2]
			);

			if (!GridLayout.IsValidCoord(NeighborCoord))
			{
				continue;
			}

			const int32 NeighborCellId = GridLayout.CoordToId(NeighborCoord);

			if (Connected.Contains(NeighborCellId))
			{
				continue;
			}

			if (!GridLayout.GetCellExists(NeighborCellId))
			{
				continue;
			}

			if (CellState.DestroyedCells.Contains(NeighborCellId))
			{
				continue;
			}

			// Check subcell boundary connectivity
			if (HasConnectedBoundary(CurrCellId, NeighborCellId, Dir, CellState))
			{
				Connected.Add(NeighborCellId);
				Queue.Enqueue(NeighborCellId);
			}
		}
	}

	// 3. Cells not in Connected are disconnected
	TSet<int32> Disconnected;
	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); CellId++)
	{
		if (GridLayout.GetCellExists(CellId) &&
			!CellState.DestroyedCells.Contains(CellId) &&
			!Connected.Contains(CellId))
		{
			Disconnected.Add(CellId);
		}
	}

	return Disconnected;
}

//=============================================================================
// FCellDestructionSystem - Hierarchical BFS (SuperCell optimization)
//=============================================================================

namespace HierarchicalBFSHelper
{
	/**
	 * Compute the cell coordinate range of a SuperCell.
	 * Intended for direct coordinate iteration without TArray allocations.
	 */
	struct FSupercellCellRange
	{
		int32 StartX, StartY, StartZ;
		int32 EndX, EndY, EndZ;

		FSupercellCellRange(int32 SupercellId, const FSuperCellState& SupercellState, const FGridCellLayout& GridLayout)
		{
			const FIntVector SupercellCoord = SupercellState.SupercellIdToCoord(SupercellId);
			StartX = SupercellCoord.X * SupercellState.SupercellSize.X;
			StartY = SupercellCoord.Y * SupercellState.SupercellSize.Y;
			StartZ = SupercellCoord.Z * SupercellState.SupercellSize.Z;

			EndX = FMath::Min(StartX + SupercellState.SupercellSize.X, GridLayout.GridSize.X);
			EndY = FMath::Min(StartY + SupercellState.SupercellSize.Y, GridLayout.GridSize.Y);
			EndZ = FMath::Min(StartZ + SupercellState.SupercellSize.Z, GridLayout.GridSize.Z);
		}
	};

	/**
	 * Mark all valid cells in a SuperCell as connected.
	 * Direct coordinate iteration without TArray allocations.
	 */
	void MarkAllCellsInSupercell(
		int32 SupercellId,
		const FSuperCellState& SupercellState,
		const FGridCellLayout& GridLayout,
		const FCellState& CellState,
		TSet<int32>& ConnectedCells)
	{
		const FSupercellCellRange Range(SupercellId, SupercellState, GridLayout);

		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				for (int32 X = Range.StartX; X < Range.EndX; ++X)
				{
					const int32 CellId = GridLayout.CoordToId(X, Y, Z);
					// Exclude destroyed cells
					if (GridLayout.GetCellExists(CellId) && !CellState.DestroyedCells.Contains(CellId))
					{
						ConnectedCells.Add(CellId);
					}
				}
			}
		}
	}

	/**
	 * Helper to add a neighbor cell (includes subcell-mode branching).
	 */
	FORCEINLINE void TryAddNeighborCell(
		int32 BoundaryCellId,
		int32 NeighborCellId,
		int32 Dir,
		const FGridCellLayout& GridLayout,
		const FCellState& CellState,
		bool bEnableSubcell,
		TQueue<FCellNode>& Queue,
		TSet<int32>& ConnectedCells)
	{
		if (ConnectedCells.Contains(NeighborCellId))
		{
			return;
		}

		if (!GridLayout.GetCellExists(NeighborCellId))
		{
			return;
		}

		if (CellState.DestroyedCells.Contains(NeighborCellId))
		{
			return;
		}

		if (bEnableSubcell)
		{
			if (SubCellBFSHelper::HasConnectedBoundary(BoundaryCellId, NeighborCellId, Dir, CellState))
			{
				ConnectedCells.Add(NeighborCellId);
				Queue.Enqueue(FCellNode::MakeCell(NeighborCellId));
			}
		}
		else
		{
			ConnectedCells.Add(NeighborCellId);
			Queue.Enqueue(FCellNode::MakeCell(NeighborCellId));
		}
	}

	/**
	 * Process a SuperCell node (search adjacent nodes from an intact SuperCell).
	 *
	 * Performance optimizations:
	 * - Use only IsSupercellIntact() (bitfield O(1))
	 * - Direct coordinate iteration without TArray allocations
	 */
	void ProcessSupercellNode(
		int32 SupercellId,
		const FGridCellLayout& GridLayout,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		bool bEnableSubcell,
		TQueue<FCellNode>& Queue,
		TSet<int32>& ConnectedCells,
		TSet<int32>& VisitedSupercells)
	{
		const FSupercellCellRange Range(SupercellId, SupercellState, GridLayout);
		const FIntVector SupercellCoord = SupercellState.SupercellIdToCoord(SupercellId);

		// Search 6-direction adjacent SuperCells
		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const FIntVector NeighborSCCoord = SupercellCoord + FIntVector(
				DIRECTION_OFFSETS[Dir][0],
				DIRECTION_OFFSETS[Dir][1],
				DIRECTION_OFFSETS[Dir][2]
			);

			if (!SupercellState.IsValidSupercellCoord(NeighborSCCoord))
			{
				continue;
			}

			const int32 NeighborSupercellId = SupercellState.SupercellCoordToId(NeighborSCCoord);

			// Skip visited SuperCells
			if (VisitedSupercells.Contains(NeighborSupercellId))
			{
				continue;
			}

			// Check if adjacent SuperCell is intact (bitfield only - O(1))
			if (SupercellState.IsSupercellIntact(NeighborSupercellId))
			{
				// Intact SuperCell -> add as a SuperCell node
				VisitedSupercells.Add(NeighborSupercellId);
				Queue.Enqueue(FCellNode::MakeSupercell(NeighborSupercellId));
				MarkAllCellsInSupercell(NeighborSupercellId, SupercellState, GridLayout, CellState, ConnectedCells); 
			}
			else
			{
				// Broken SuperCell -> connect directly from boundary cells to neighbor cells
				// Direct coordinate iteration without TArray allocations

				// Traverse boundary face cells per direction and process adjacent cells
				switch (Dir)
				{
				case 0: // -X: our X=StartX face -> neighbor cell is X=StartX-1
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
						{
							const int32 BoundaryCellId = GridLayout.CoordToId(Range.StartX, Y, Z);
							const int32 NeighborCellId = GridLayout.CoordToId(Range.StartX - 1, Y, Z);
							if (GridLayout.IsValidCoord(Range.StartX - 1, Y, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 1: // +X: our X=EndX-1 face -> neighbor cell is X=EndX
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
						{
							const int32 BoundaryCellId = GridLayout.CoordToId(Range.EndX - 1, Y, Z);
							const int32 NeighborCellId = GridLayout.CoordToId(Range.EndX, Y, Z);
							if (GridLayout.IsValidCoord(Range.EndX, Y, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 2: // -Y: our Y=StartY face -> neighbor cell is Y=StartY-1
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.StartY, Z);
							const int32 NeighborCellId = GridLayout.CoordToId(X, Range.StartY - 1, Z);
							if (GridLayout.IsValidCoord(X, Range.StartY - 1, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 3: // +Y: our Y=EndY-1 face -> neighbor cell is Y=EndY
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.EndY - 1, Z);
							const int32 NeighborCellId = GridLayout.CoordToId(X, Range.EndY, Z);
							if (GridLayout.IsValidCoord(X, Range.EndY, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 4: // -Z: our Z=StartZ face -> neighbor cell is Z=StartZ-1
					for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.StartZ);
							const int32 NeighborCellId = GridLayout.CoordToId(X, Y, Range.StartZ - 1);
							if (GridLayout.IsValidCoord(X, Y, Range.StartZ - 1))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 5: // +Z: our Z=EndZ-1 face -> neighbor cell is Z=EndZ
					for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.EndZ - 1);
							const int32 NeighborCellId = GridLayout.CoordToId(X, Y, Range.EndZ);
							if (GridLayout.IsValidCoord(X, Y, Range.EndZ))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;
				}
			}
		}

		// Connect to orphan cells (from SuperCell boundary cells to external orphan cells)
		// Iterate the 6 boundary faces directly without TArray allocations

		// -X boundary face
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(Range.StartX, Y, Z);
				const int32 NeighborX = Range.StartX - 1;
				if (GridLayout.IsValidCoord(NeighborX, Y, Z))
				{
					const int32 NeighborCellId = GridLayout.CoordToId(NeighborX, Y, Z);
					if (SupercellState.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 0, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// +X boundary face
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(Range.EndX - 1, Y, Z);
				const int32 NeighborX = Range.EndX;
				if (GridLayout.IsValidCoord(NeighborX, Y, Z))
				{
					const int32 NeighborCellId = GridLayout.CoordToId(NeighborX, Y, Z);
					if (SupercellState.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 1, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// -Y boundary face
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.StartY, Z);
				const int32 NeighborY = Range.StartY - 1;
				if (GridLayout.IsValidCoord(X, NeighborY, Z))
				{
					const int32 NeighborCellId = GridLayout.CoordToId(X, NeighborY, Z);
					if (SupercellState.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 2, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// +Y boundary face
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.EndY - 1, Z);
				const int32 NeighborY = Range.EndY;
				if (GridLayout.IsValidCoord(X, NeighborY, Z))
				{
					const int32 NeighborCellId = GridLayout.CoordToId(X, NeighborY, Z);
					if (SupercellState.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 3, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// -Z boundary face
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.StartZ);
				const int32 NeighborZ = Range.StartZ - 1;
				if (GridLayout.IsValidCoord(X, Y, NeighborZ))
				{
					const int32 NeighborCellId = GridLayout.CoordToId(X, Y, NeighborZ);
					if (SupercellState.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 4, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// +Z boundary face
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.EndZ - 1);
				const int32 NeighborZ = Range.EndZ;
				if (GridLayout.IsValidCoord(X, Y, NeighborZ))
				{
					const int32 NeighborCellId = GridLayout.CoordToId(X, Y, NeighborZ);
					if (SupercellState.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 5, GridLayout, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}
	}

	/**
	 * Process a cell node (search adjacent nodes from an individual cell).
	 *
	 * Performance optimization: use only IsSupercellIntact() (bitfield O(1))
	 */
	void ProcessCellNode(
		int32 CellId,
		const FGridCellLayout& GridLayout,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		bool bEnableSubcell,
		TQueue<FCellNode>& Queue,
		TSet<int32>& ConnectedCells,
		TSet<int32>& VisitedSupercells)
	{
		
		const FIntVector CellCoord = GridLayout.IdToCoord(CellId);

		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const FIntVector NeighborCoord = CellCoord + FIntVector(
				DIRECTION_OFFSETS[Dir][0],
				DIRECTION_OFFSETS[Dir][1],
				DIRECTION_OFFSETS[Dir][2]
			);

			if (!GridLayout.IsValidCoord(NeighborCoord))
			{
				continue;
			}

			const int32 NeighborCellId = GridLayout.CoordToId(NeighborCoord);

			if (!GridLayout.GetCellExists(NeighborCellId))
			{
				continue;
			}

			if (CellState.DestroyedCells.Contains(NeighborCellId))
			{
				continue;
			}

			if (ConnectedCells.Contains(NeighborCellId))
			{
				continue;
			}

			// In subcell mode, check boundary connectivity
			bool bIsConnected = true;
			if (bEnableSubcell)
			{
				bIsConnected = SubCellBFSHelper::HasConnectedBoundary(CellId, NeighborCellId, Dir, CellState);
			}

			if (!bIsConnected)
			{
				continue;
			}

			const int32 NeighborSupercellId = SupercellState.GetSupercellForCell(NeighborCellId);

			// Check if neighbor belongs to an intact SuperCell (bitfield only - O(1))
			if (NeighborSupercellId != INDEX_NONE &&
			    !VisitedSupercells.Contains(NeighborSupercellId) &&
			    SupercellState.IsSupercellIntact(NeighborSupercellId))
			{
				// Intact SuperCell -> expand to SuperCell node
				VisitedSupercells.Add(NeighborSupercellId);
				Queue.Enqueue(FCellNode::MakeSupercell(NeighborSupercellId));
				MarkAllCellsInSupercell(NeighborSupercellId, SupercellState, GridLayout, CellState, ConnectedCells);
			}
			else
			{
				// Broken SuperCell or orphan -> add at cell level
				ConnectedCells.Add(NeighborCellId);
				Queue.Enqueue(FCellNode::MakeCell(NeighborCellId));
			}
		}
	}

	FORCEINLINE void TryAddNeighborCell_Opt(
		int32 BoundaryCellId,
		int32 NeighborCellId,
		int32 Dir,
		const FGridCellLayout& GridLayout,
		const FCellState& CellState,
		bool bEnableSubcell,
		FConnectivityContext& Context,
		TArray<FCellNode>& Stack)
	{
		if (!GridLayout.GetCellExists(NeighborCellId))
		{
			return;
		}

		if (CellState.DestroyedCells.Contains(NeighborCellId))
		{
			return;
		}
		
		if (Context.IsCellConnected(NeighborCellId))
		{
			return;
		}

		if (bEnableSubcell &&
			!SubCellBFSHelper::HasConnectedBoundary(BoundaryCellId, NeighborCellId, Dir, CellState))
		{
			return;
		}

		Context.SetCellConnected(NeighborCellId);
		Stack.Push(FCellNode::MakeCell(NeighborCellId));
	}

	void MarkAllCellsInSuperCell_Bit(
		int32 SupercellId,
		const FSuperCellState& SupercellState,
		const FGridCellLayout& GridLayout,
		const FCellState& CellState,
		FConnectivityContext& Context		
	)
	{
		const FSupercellCellRange Range(SupercellId, SupercellState, GridLayout);

		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				for (int32 X = Range.StartX; X < Range.EndX; ++X)
				{
					const int32 CellId = GridLayout.CoordToId(X, Y, Z);
					if (GridLayout.GetCellExists(CellId) && !CellState.DestroyedCells.Contains(CellId))
					{
						Context.SetCellConnected(CellId);
					}
				}
			}
		}
	}

	void ProcessSupercellNode_Opt(
		int32 SupercellId,
		const FGridCellLayout& GridLayout,
		FSuperCellState& SupercellState,
		const FCellState& CellState,
		bool bEnableSubcell,
		FConnectivityContext& Context,
		TArray<FCellNode>& Stack
		)
	{
		const FSupercellCellRange Range(SupercellId, SupercellState, GridLayout);
		const FIntVector SupercellCoord = SupercellState.SupercellIdToCoord(SupercellId);

		for (int32 Dir = 0; Dir < 6; Dir++)
		{
			const FIntVector NeighborSCCoord = SupercellCoord + FIntVector(
				DIRECTION_OFFSETS[Dir][0],
				DIRECTION_OFFSETS[Dir][1],
				DIRECTION_OFFSETS[Dir][2]);

			if (!SupercellState.IsValidSupercellCoord(NeighborSCCoord))
			{
				continue;
			}

			const int32 NeighborSupercellId = SupercellState.SupercellCoordToId(NeighborSCCoord);

			if (SupercellState.IsSupercellIntact(NeighborSupercellId))
			{
				if (Context.IsSuperCellVisited(NeighborSupercellId))
				{
					continue;
				}

				Context.SetSuperCellVisited(NeighborSupercellId);
				Stack.Push(FCellNode::MakeSupercell(NeighborSupercellId));
				MarkAllCellsInSuperCell_Bit(NeighborSupercellId, SupercellState, GridLayout, CellState, Context);
				continue;
			}

			switch (Dir)
			{
			case 0: // -X
				for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
				{
					for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
					{
						const int32 BoundaryCellId = GridLayout.CoordToId(Range.StartX, Y, Z);
						if (GridLayout.IsValidCoord(Range.StartX - 1, Y, Z))
						{
							const int32 NeighborCellId = GridLayout.CoordToId(Range.StartX - 1, Y, Z);
							TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState,
							                       bEnableSubcell, Context, Stack);
						}
					}
				}
				break;

			case 1: // +X
				for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
				{
					for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
					{
						const int32 BoundaryCellId = GridLayout.CoordToId(Range.EndX - 1, Y, Z);
						if (GridLayout.IsValidCoord(Range.EndX, Y, Z))
						{
							const int32 NeighborCellId = GridLayout.CoordToId(Range.EndX, Y, Z);
							TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState,
							                       bEnableSubcell, Context, Stack);
						}
					}
				}
				break;

			case 2: // -Y
				for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
				{
					for (int32 X = Range.StartX; X < Range.EndX; ++X)
					{
						const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.StartY, Z);
						if (GridLayout.IsValidCoord(X, Range.StartY - 1, Z))
						{
							const int32 NeighborCellId = GridLayout.CoordToId(X, Range.StartY - 1, Z);
							TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState,
							                       bEnableSubcell, Context, Stack);
						}
					}
				}
				break;

			case 3: // +Y
				for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
				{
					for (int32 X = Range.StartX; X < Range.EndX; ++X)
					{
						const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.EndY - 1, Z);
						if (GridLayout.IsValidCoord(X, Range.EndY, Z))
						{
							const int32 NeighborCellId = GridLayout.CoordToId(X, Range.EndY, Z);
							TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState,
							                       bEnableSubcell, Context, Stack);
						}
					}
				}
				break;

			case 4: // -Z
				for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
				{
					for (int32 X = Range.StartX; X < Range.EndX; ++X)
					{
						const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.StartZ);
						if (GridLayout.IsValidCoord(X, Y, Range.StartZ - 1))
						{
							const int32 NeighborCellId = GridLayout.CoordToId(X, Y, Range.StartZ - 1);
							TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState,
							                       bEnableSubcell, Context, Stack);
						}
					}
				}
				break;

			case 5: // +Z
				for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
				{
					for (int32 X = Range.StartX; X < Range.EndX; ++X)
					{
						const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.EndZ - 1);
						if (GridLayout.IsValidCoord(X, Y, Range.EndZ))
						{
							const int32 NeighborCellId = GridLayout.CoordToId(X, Y, Range.EndZ);
							TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, Dir, GridLayout, CellState,
							                       bEnableSubcell, Context, Stack);
						}
					}
				}
				break;
			}
		}

		auto TryAddOrphan = [&](int32 BoundaryCellId, int32 NeighborCellId, int32 OrphanDir)
		{
			if (SupercellState.IsCellOrphan(NeighborCellId))
			{
				TryAddNeighborCell_Opt(BoundaryCellId, NeighborCellId, OrphanDir, GridLayout, CellState,
				                       bEnableSubcell, Context, Stack);
			}
		};

		// -X boundary face
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(Range.StartX, Y, Z);
				if (GridLayout.IsValidCoord(Range.StartX - 1, Y, Z))
				{
					TryAddOrphan(BoundaryCellId, GridLayout.CoordToId(Range.StartX - 1, Y, Z), 0);
				}
			}
		}

		// +X
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(Range.EndX - 1, Y, Z);
				if (GridLayout.IsValidCoord(Range.EndX, Y, Z))
				{
					TryAddOrphan(BoundaryCellId, GridLayout.CoordToId(Range.EndX, Y, Z), 1);
				}
			}
		}

		// -Y
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.StartY, Z);
				if (GridLayout.IsValidCoord(X, Range.StartY - 1, Z))
				{
					TryAddOrphan(BoundaryCellId, GridLayout.CoordToId(X, Range.StartY - 1, Z), 2);
				}
			}
		}

		// +Y
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Range.EndY - 1, Z);
				if (GridLayout.IsValidCoord(X, Range.EndY, Z))
				{
					TryAddOrphan(BoundaryCellId, GridLayout.CoordToId(X, Range.EndY, Z), 3);
				}
			}
		}

		// -Z
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.StartZ);
				if (GridLayout.IsValidCoord(X, Y, Range.StartZ - 1))
				{
					TryAddOrphan(BoundaryCellId, GridLayout.CoordToId(X, Y, Range.StartZ - 1), 4);
				}
			}
		}

		// +Z
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridLayout.CoordToId(X, Y, Range.EndZ - 1);
				if (GridLayout.IsValidCoord(X, Y, Range.EndZ))
				{
					TryAddOrphan(BoundaryCellId, GridLayout.CoordToId(X, Y, Range.EndZ), 5);
				}
			}
		}		
	}
}   

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsHierarchicalLevel(
	const FGridCellLayout& GridLayout,
	FSuperCellState& SupercellState,
	const FCellState& CellState,
	bool bEnableSubcell,
	FConnectivityContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindDisconnectedCellsHierarchicalLevel);

	// 1. Find cells connected to anchors
	FindConnectedCellsHierarchical_Optimized(
		GridLayout, SupercellState, CellState, Context, bEnableSubcell);
	
	// if (!DEBUGStruct::BFSTest())
	// {
	// 	ConnectedCells = FindConnectedCellsHierarchical(
	// 	   GridLayout, SupercellState, CellState, bEnableSubcell);
	// }
	// else
	// {
	// 	ConnectedCells = FindConnectedCellsHierarchical_Optimized(
	// 	GridLayout, SupercellState, CellState, Context, bEnableSubcell);
	// }

	// 2. Cells not in Connected are disconnected
	TSet<int32> Disconnected;

	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); ++CellId)
	{
		if (!GridLayout.GetCellExists(CellId))
		{
			continue;
		}

		if (CellState.DestroyedCells.Contains(CellId))
		{
			continue;
		}

		if (!Context.IsCellConnected(CellId))
		{
			Disconnected.Add(CellId);
		}
	}
	
	return Disconnected;
}

void FCellDestructionSystem::FindConnectedCellsHierarchical_Optimized(
	const FGridCellLayout& Cache,
	FSuperCellState& SupercellState,
	const FCellState& CellState,
	FConnectivityContext& Context,
	bool bEnableSubcell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindConnectedCellsHierarchical_Opt);
	
	using namespace HierarchicalBFSHelper;
	int32 TotalCells = Cache.GetTotalCellCount();
	
	Context.Reset(TotalCells, SupercellState.GetTotalSupercellCount());

	TArray<FCellNode>& Stack = Context.WorkStack;

	const int32 SizeX = Cache.GridSize.X;
	const int32 SizeY = Cache.GridSize.Y;
	const int32 SizeXY = Cache.GridSize.X * Cache.GridSize.Y;

	const int32 Strides[6] = {-1, 1, -SizeX, SizeX, -SizeXY, SizeXY};

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindConnectedCellsHierarchical_Opt_Anchor);
		for (int32 CellId = 0; CellId < TotalCells; CellId++)
		{
			if (!Cache.GetCellExists(CellId))
			{
				continue;
			}
		
			if (!Cache.GetCellIsAnchor(CellId))
			{
				continue;
			}		

			if (CellState.DestroyedCells.Contains(CellId))
			{
				continue;
			}

			if (bEnableSubcell && !SubCellBFSHelper::HasAliveSubCell(CellId, CellState))
			{
				continue;
			}

			const int32 SupercellId = SupercellState.GetSupercellForCell(CellId);

			if (SupercellId != INDEX_NONE &&
				SupercellState.IsSupercellIntact(SupercellId) &&
				!Context.IsSuperCellVisited(SupercellId))
			{
				Context.SetSuperCellVisited(SupercellId);
				Stack.Push(FCellNode::MakeSupercell(SupercellId));
				MarkAllCellsInSuperCell_Bit(SupercellId, SupercellState, Cache, CellState, Context);
			}
			else
			{
				if (!Context.IsCellConnected(CellId))
				{
					Context.SetCellConnected(CellId);
					Stack.Push(FCellNode::MakeCell(CellId));
				}
			}
		}
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindConnectedCellsHierarchical_Opt_DFS);
		while (Stack.Num() > 0)
		{
			const FCellNode Current = Stack.Pop(EAllowShrinking::No);

			if (Current.bIsSupercell)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindConnectedCellsHierarchical_Opt_Intact);
				ProcessSupercellNode_Opt(
					Current.Id,
					Cache,
					SupercellState,
					CellState,
					bEnableSubcell,
					Context,
					Stack);
			}
			else
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindConnectedCellsHierarchical_Opt_Cell);
				const int32 CurrentId = Current.Id;

				const int32 Z = CurrentId / SizeXY;
				const int32 RemXY = CurrentId - Z * SizeXY;
				const int32 Y = RemXY / SizeX;
				const int32 X = RemXY - Y * SizeX;

				for (int32 Dir = 0; Dir < 6; ++Dir)
				{
					// -X boundary
					if (Dir == 0 && X == 0)
					{
						continue;
					}
					// +X boundary
					if (Dir == 1 && X == SizeX - 1)
					{
						continue;
					}				
				
					// -Y boundary
					if (Dir == 2 && Y == 0)
					{
						continue;
					}
					// +Y boundary
					if (Dir == 3 && Y >= (SizeY - 1))
					{
						continue;
					}

					// -Z boundary
					if (Dir == 4 && Z == 0)
					{
						continue;
					}
					// +Z boundary
					if (Dir == 5 && Z == Cache.GridSize.Z - 1)
					{
						continue;
					}

					const int32 NeighborId = CurrentId + Strides[Dir];

					if (!Cache.GetCellExists(NeighborId))
					{
						continue;
					}

					if (CellState.DestroyedCells.Contains(NeighborId))
					{
						continue;
					}

					if (Context.IsCellConnected(NeighborId))
					{
						continue;
					}
				
					if (bEnableSubcell && !SubCellBFSHelper::HasConnectedBoundary(CurrentId, NeighborId, Dir, CellState))
					{
						continue;
					}

					const int32 NeighborSupercellId = SupercellState.GetSupercellForCell(NeighborId);

					if (NeighborSupercellId != INDEX_NONE &&
						SupercellState.IsSupercellIntact(NeighborSupercellId) &&
						!Context.IsSuperCellVisited(NeighborSupercellId))
					{
						Context.SetSuperCellVisited(NeighborSupercellId);
						Stack.Push(FCellNode::MakeSupercell(NeighborSupercellId));
						MarkAllCellsInSuperCell_Bit(NeighborSupercellId, SupercellState, Cache, CellState, Context);
					}
					else
					{
						Context.SetCellConnected(NeighborId);
						Stack.Push(FCellNode::MakeCell(NeighborId));
					}
				}
			}
		}
	}

	// TSet<int32> ConnectedCells(Context.ConnectedCellIds);
	//
	// return ConnectedCells;
} 

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsFromAffected(
	const FGridCellLayout& Cache,
	FSuperCellState& SupercellState,
	const FCellState& CellState,
	const TArray<int32>& AffectedNeighborCells,
	FConnectivityContext& Context,
	bool bEnableSupercell,
	bool bEnableSubcell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DFSToAnchor_FindDisconnectedCellsFromAffected);
	using namespace HierarchicalBFSHelper;

	TSet<int32> DisconnectedCells;
	TSet<int32> ConfirmedConnected;

	const int32 TotalCells = Cache.GetTotalCellCount();
	const int32 SizeX = Cache.GridSize.X;
	const int32 SizeY = Cache.GridSize.Y;
	const int32 SizeZ = Cache.GridSize.Z;
	const int32 SizeXY = SizeX * SizeY;

	// CellId = X + Y * SizeX + Z * SizeX * SizeY
	// x축 이동 = 1, y축 이동 = sizeX, z축 이동 = sizeX * sizeY 
	const int32 Stride[6] = { -1, 1, -SizeX, SizeX, -SizeXY, SizeXY };

	for (int32 StartCellId : AffectedNeighborCells)
	{
		// 이미 정해진 Cell Id 
		if (ConfirmedConnected.Contains(StartCellId) || DisconnectedCells.Contains(StartCellId))
		{
			continue;
		}

		// Skip Destroyed cells
		if (CellState.DestroyedCells.Contains(StartCellId))
		{
			continue;
		}

		// Skip non-existent cells
		if (!Cache.GetCellExists(StartCellId))
		{
			continue;	
		}

		// Reset context for this search
		Context.Reset(TotalCells, SupercellState.GetTotalSupercellCount());
		 
		TArray<FCellNode>& Stack = Context.WorkStack;
		bool bFoundAnchor = false;

		if (bEnableSupercell)
		{
			const int32 SupercellId = SupercellState.GetSupercellForCell(StartCellId);

			// Supercell 존재 && 손상 X 
			if (SupercellId != INDEX_NONE && SupercellState.IsSupercellIntact(SupercellId))
			{	
				// 앵커가 있는 Supercell에 도착
				if (SupercellContainsAnchor(SupercellId, Cache, SupercellState, CellState))
				{
					bFoundAnchor = true;
				}

				// ConfirmedConnected를 포함중
				else if (SupercellContainsConfirmedConnected(SupercellId, Cache, SupercellState, ConfirmedConnected))
				{
					bFoundAnchor = true;
				}
				else
				{
					Context.SetSuperCellVisited(SupercellId);
					Stack.Push(FCellNode::MakeSupercell(SupercellId));
					MarkAllCellsInSuperCell_Bit(SupercellId, SupercellState, Cache, CellState, Context); // 이미 intact false 인데, 한번에 bit찍으면 되지 않나 ?

				}
			}
			else
			{
				// Broken SuperCell 또는 Orphan → Cell로 Push
				if (Cache.GetCellIsAnchor(StartCellId))
				{
					bFoundAnchor = true;
				}
				else if (ConfirmedConnected.Contains(StartCellId))
				{
					bFoundAnchor = true;
				}
				else
				{
					Context.SetCellConnected(StartCellId);
					Stack.Push(FCellNode::MakeCell(StartCellId));
				}
			}
		}
		// Supercell을 안쓰는 경우
		else
		{
			if (Cache.GetCellIsAnchor(StartCellId))
			{
				bFoundAnchor = true;
			}
			else if (ConfirmedConnected.Contains(StartCellId))
			{
				bFoundAnchor = true;
			}
			else
			{
				Context.SetCellConnected(StartCellId);
				Stack.Push(FCellNode::MakeCell(StartCellId));
			}
		}

		// DFS Loop
		while (!bFoundAnchor && Stack.Num() > 0)
		{
			// EAllowShrinking: 원소를 제거할 때 메모리를(capacity) 줄이지 않기
			const FCellNode Current = Stack.Pop(EAllowShrinking::No);

			if (Current.bIsSupercell)
			{
				const int32 SupercellId = Current.Id;
				const FSupercellCellRange Range(SupercellId, SupercellState, Cache);
				const FIntVector SupercellCoord = SupercellState.SupercellIdToCoord(SupercellId);

				for (int32 Dir = 0; Dir < 6 && !bFoundAnchor; ++Dir)
				{
					const FIntVector NeighborsSCCoord = SupercellCoord + FIntVector(
					DIRECTION_OFFSETS[Dir][0] ,
					DIRECTION_OFFSETS[Dir][1] ,
					DIRECTION_OFFSETS[Dir][2]
					);

					if (!SupercellState.IsValidSupercellCoord(NeighborsSCCoord))
					{
						continue;
					}

					const int32 NeighborSupercellId = SupercellState.SupercellCoordToId(NeighborsSCCoord);

					// Supercell이 손상이 안됐을 때
					if (SupercellState.IsSupercellIntact(NeighborSupercellId))
					{
						// 이미 확인했으면 Pass
						if (Context.IsSuperCellVisited(NeighborSupercellId))
						{
							continue;
						}

						// 앵커를 포함하는 Supercell인가 
						if (SupercellContainsAnchor(NeighborSupercellId,  Cache, SupercellState, CellState))
						{
							bFoundAnchor = true;
							break;
						}

						// ConfirmedConnected를 포함하는 Supercell 
						if (SupercellContainsConfirmedConnected(NeighborSupercellId, Cache, SupercellState, ConfirmedConnected))
						{
							bFoundAnchor = true;
							break;
						}

						Context.SetSuperCellVisited(NeighborSupercellId);
						Stack.Push(FCellNode::MakeSupercell(NeighborSupercellId));
						MarkAllCellsInSuperCell_Bit(NeighborSupercellId, SupercellState, Cache, CellState, Context);
					}
					else
					{
						// 이미 깨진 Supercell  
						TArray<int32> BoundaryCellIds;
						SupercellState.GetBoundaryCellsInDirection(SupercellId, Dir, Cache, BoundaryCellIds);

						for (int32 BoundaryCellId : BoundaryCellIds)
						{
							// 존재하지 않거나, 파괴된 cell을 패스
							if (!Cache.GetCellExists(BoundaryCellId) || CellState.DestroyedCells.Contains(BoundaryCellId))
							{
								continue;
							}

							const FIntVector BoundaryCoord = Cache.IdToCoord(BoundaryCellId);
							const FIntVector NeighborCoord = BoundaryCoord + FIntVector(
								DIRECTION_OFFSETS[Dir][0],
								DIRECTION_OFFSETS[Dir][1],
								DIRECTION_OFFSETS[Dir][2]
							);
						
							if (!Cache.IsValidCoord(NeighborCoord))
							{
								continue;
							}

							const int32 NeighborCellId = Cache.CoordToId(NeighborCoord);

							// 이미 방문했거나 파괴된 Cell은 스킵
							if (!Cache.GetCellExists(NeighborCellId) || CellState.DestroyedCells.Contains(NeighborCellId) || Context.IsCellConnected(NeighborCellId))
							{
								continue;
							}

							if (bEnableSubcell && !SubCellBFSHelper::HasConnectedBoundary(BoundaryCellId, NeighborCellId, Dir, CellState))
							{
								continue;
							}

							if (Cache.GetCellIsAnchor(NeighborCellId))
							{
								bFoundAnchor = true;
								break;
							}

							if (ConfirmedConnected.Contains(NeighborCellId))
							{
								bFoundAnchor = true;
								break;
							}

							Context.SetCellConnected(NeighborCellId);
							Stack.Push(FCellNode::MakeCell(NeighborCellId));
						}

					}

				}

			}
			else
			{
				// Cell 노드 처리 
				const int32 CurrentCellId = Current.Id;

				//CellId = X + Y * SizeX + Z * SizeX * SizeY
				const int32 Z = CurrentCellId / SizeXY;
				const int32 RemXY = CurrentCellId - Z * SizeXY;
				const int32 Y = RemXY / SizeX;
				const int32 X = RemXY - Y * SizeX;

				// 정방향 : -X, X, -Y, Y, -Z, Z 
				for (int32 Dir = 5 ; Dir >= 0 && !bFoundAnchor; --Dir)
				{
					// 경계 체크
					if (Dir == 0 && X == 0)
					{
						continue;
					}
					if (Dir == 1 && X == SizeX - 1)
					{
						continue;
					}
					if (Dir == 2 && Y == 0)
					{
						continue;
					}
					if (Dir == 3 && Y == SizeY - 1)
					{
						continue;
					}
					if (Dir == 4 && Z == 0)
					{
						continue;
					}
					if (Dir == 5 && Z == SizeZ - 1)
					{
						continue;
					}

					const int32 NeighborCellId = CurrentCellId + Stride[Dir];

					// 기본 체크
					if (!Cache.GetCellExists(NeighborCellId) ||
						CellState.DestroyedCells.Contains(NeighborCellId) ||
						Context.IsCellConnected(NeighborCellId))
					{
						continue;
					}

					// SubCell 경계 연결성 체크
					if (bEnableSubcell &&
						!SubCellBFSHelper::HasConnectedBoundary(CurrentCellId, NeighborCellId, Dir, CellState))
					{
						continue;
					}

					// 앵커 도달 체크
					if (Cache.GetCellIsAnchor(NeighborCellId))
					{
						bFoundAnchor = true;
						break;
					}

					// ConfirmedConnected 도달 체크
					if (ConfirmedConnected.Contains(NeighborCellId))
					{
						bFoundAnchor = true;
						break;
					}

					// 이웃이 Intact SuperCell에 속하는지 체크
					if (bEnableSupercell)
					{
						const int32 NeighborSupercellId = SupercellState.GetSupercellForCell(NeighborCellId);

						if (NeighborSupercellId != INDEX_NONE &&
							SupercellState.IsSupercellIntact(NeighborSupercellId) &&
							!Context.IsSuperCellVisited(NeighborSupercellId))
						{
							// Intact SuperCell -> 앵커/ConfirmedConnected 체크 후 SuperCell로 Push
							if (SupercellContainsAnchor(NeighborSupercellId, Cache, SupercellState, CellState))
							{
								bFoundAnchor = true;
								break;
							}

							if (SupercellContainsConfirmedConnected(NeighborSupercellId, Cache, SupercellState, ConfirmedConnected))
							{
								bFoundAnchor = true;
								break;
							}

							Context.SetSuperCellVisited(NeighborSupercellId);
							Stack.Push(FCellNode::MakeSupercell(NeighborSupercellId));
							MarkAllCellsInSuperCell_Bit(NeighborSupercellId, SupercellState, Cache, CellState, Context);
							continue;
						}
					}

					// Broken SuperCell 또는 Orphan -> Cell로 Push
					Context.SetCellConnected(NeighborCellId);
					Stack.Push(FCellNode::MakeCell(NeighborCellId));
				}
			}

		}

		if (bFoundAnchor)
		{
			for (int32 CellId : Context.ConnectedCellIds)
			{
				ConfirmedConnected.Add(CellId);
			}
		}

		else
		{
			for (int32 CellId : Context.ConnectedCellIds)
			{
				DisconnectedCells.Add(CellId);
			}
		}
	}
	 
	return DisconnectedCells;
}
bool FCellDestructionSystem::SupercellContainsAnchor(
	int32 SupercellId,
	const FGridCellLayout& Cache,
	const FSuperCellState& SupercellState,
	const FCellState& CellState)
{
	using namespace HierarchicalBFSHelper;

	const FSupercellCellRange Range(SupercellId, SupercellState, Cache);

	for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
	{
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 CellId = Cache.CoordToId(X, Y, Z);

				if (Cache.GetCellExists(CellId) &&
					!CellState.DestroyedCells.Contains(CellId) &&
					Cache.GetCellIsAnchor(CellId))
				{
					return true;
				}
			}
		}
	}

	return false;
}

bool FCellDestructionSystem::SupercellContainsConfirmedConnected(
	int32 SupercellId,
	const FGridCellLayout& Cache,
	const FSuperCellState& SupercellState,
	const TSet<int32>& ConfirmedConnected
)
{
	using namespace HierarchicalBFSHelper;

	const FSupercellCellRange Range(SupercellId, SupercellState, Cache);

	for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
	{
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 CellId = Cache.CoordToId(X, Y, Z);
				if (ConfirmedConnected.Contains(CellId))
				{
					return true;
				}
			}
		}
	}

	return false;
}