// Copyright 2025. All Rights Reserved.

#include "StructuralIntegrity/NarrowPhaseConnectivity.h"

//=============================================================================
// Public Methods
//=============================================================================

bool FNarrowPhaseConnectivity::AreNarrowConnected(
	int32 CellA,
	int32 CellB,
	const FGridCellLayout& GridLayout,
	const FTransform& MeshTransform,
	const TArray<FQuantizedDestructionInput>& Destructions)
{
	// CellA의 서브셀 중 파괴되지 않은 것 찾기
	TArray<FIntVector> IntactSubCellsA;
	GetIntactSubCells(CellA, GridLayout, MeshTransform, Destructions, IntactSubCellsA);

	if (IntactSubCellsA.Num() == 0)
	{
		return false;
	}

	// CellB의 서브셀 중 파괴되지 않은 것 찾기
	TArray<FIntVector> IntactSubCellsB;
	GetIntactSubCells(CellB, GridLayout, MeshTransform, Destructions, IntactSubCellsB);

	if (IntactSubCellsB.Num() == 0)
	{
		return false;
	}

	// 두 셀 사이 경계면에서 인접한 서브셀 쌍이 있는지 확인
	const FIntVector CoordA = GridLayout.IdToCoord(CellA);
	const FIntVector CoordB = GridLayout.IdToCoord(CellB);
	const FIntVector Direction = CoordB - CoordA;  // 인접 방향

	for (const FIntVector& SubA : IntactSubCellsA)
	{
		for (const FIntVector& SubB : IntactSubCellsB)
		{
			// 경계면에서 인접한지 확인
			if (AreSubCellsAdjacent(SubA, SubB, Direction))
			{
				return true;  // 연결됨!
			}
		}
	}

	return false;  // 끊어짐
}

TSet<int32> FNarrowPhaseConnectivity::FindDisconnectedCellsWithNarrowPhase(
	const FGridCellLayout& GridLayout,
	const TSet<int32>& DestroyedCells,
	const FTransform& MeshTransform,
	const TArray<FQuantizedDestructionInput>& Destructions)
{
	TSet<int32> Connected;
	TQueue<int32> Queue;

	// 1. 앵커에서 BFS 시작
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

	// 2. BFS with Narrow Phase
	while (!Queue.IsEmpty())
	{
		int32 Current;
		Queue.Dequeue(Current);

		for (int32 Neighbor : GridLayout.GetCellNeighbors(Current))
		{
			if (DestroyedCells.Contains(Neighbor) || Connected.Contains(Neighbor))
			{
				continue;
			}

			// 경계 셀인지 확인 (파괴 영역과 인접)
			const bool bIsBoundaryCell = IsBoundaryCell(GridLayout, Neighbor, DestroyedCells);

			bool bIsConnected = true;
			if (bIsBoundaryCell)
			{
				// Narrow Phase 검사
				bIsConnected = AreNarrowConnected(
					Current, Neighbor, GridLayout, MeshTransform, Destructions);
			}

			if (bIsConnected)
			{
				Connected.Add(Neighbor);
				Queue.Enqueue(Neighbor);
			}
		}
	}

	// 3. 연결되지 않은 셀 반환
	TSet<int32> Disconnected;
	for (int32 CellId = 0; CellId < GridLayout.GetTotalCellCount(); CellId++)
	{
		if (GridLayout.GetCellExists(CellId) &&
		    !DestroyedCells.Contains(CellId) &&
		    !Connected.Contains(CellId))
		{
			Disconnected.Add(CellId);
		}
	}

	return Disconnected;
}

//=============================================================================
// Private Methods
//=============================================================================

void FNarrowPhaseConnectivity::GetIntactSubCells(
	int32 CellId,
	const FGridCellLayout& GridLayout,
	const FTransform& MeshTransform,
	const TArray<FQuantizedDestructionInput>& Destructions,
	TArray<FIntVector>& OutIntactSubCells)
{
	const FVector CellMin = GridLayout.IdToLocalMin(CellId);
	const FVector SubCellSize = GridLayout.CellSize / SubDivision;

	for (int32 X = 0; X < SubDivision; X++)
	{
		for (int32 Y = 0; Y < SubDivision; Y++)
		{
			for (int32 Z = 0; Z < SubDivision; Z++)
			{
				// 서브셀 중심점 (로컬)
				const FVector LocalSubCellCenter = CellMin + FVector(
					(X + 0.5f) * SubCellSize.X,
					(Y + 0.5f) * SubCellSize.Y,
					(Z + 0.5f) * SubCellSize.Z
				);

				// 월드 좌표로 변환
				const FVector WorldSubCellCenter = MeshTransform.TransformPosition(LocalSubCellCenter);

				// 어떤 파괴 형상에도 포함되지 않으면 intact
				bool bIntact = true;
				for (const auto& Input : Destructions)
				{
					if (Input.ContainsPoint(WorldSubCellCenter))
					{
						bIntact = false;
						break;
					}
				}

				if (bIntact)
				{
					OutIntactSubCells.Add(FIntVector(X, Y, Z));
				}
			}
		}
	}
}

bool FNarrowPhaseConnectivity::AreSubCellsAdjacent(
	const FIntVector& SubA,
	const FIntVector& SubB,
	const FIntVector& CellDirection)
{
	// CellDirection에 따라 경계면 확인
	// 예: CellDirection = (1,0,0)이면 X+ 방향 경계

	if (CellDirection.X == 1)
	{
		// A의 X=2 면과 B의 X=0 면이 인접해야 함
		return SubA.X == SubDivision - 1 && SubB.X == 0 &&
		       SubA.Y == SubB.Y && SubA.Z == SubB.Z;
	}
	else if (CellDirection.X == -1)
	{
		return SubA.X == 0 && SubB.X == SubDivision - 1 &&
		       SubA.Y == SubB.Y && SubA.Z == SubB.Z;
	}
	else if (CellDirection.Y == 1)
	{
		return SubA.Y == SubDivision - 1 && SubB.Y == 0 &&
		       SubA.X == SubB.X && SubA.Z == SubB.Z;
	}
	else if (CellDirection.Y == -1)
	{
		return SubA.Y == 0 && SubB.Y == SubDivision - 1 &&
		       SubA.X == SubB.X && SubA.Z == SubB.Z;
	}
	else if (CellDirection.Z == 1)
	{
		return SubA.Z == SubDivision - 1 && SubB.Z == 0 &&
		       SubA.X == SubB.X && SubA.Y == SubB.Y;
	}
	else if (CellDirection.Z == -1)
	{
		return SubA.Z == 0 && SubB.Z == SubDivision - 1 &&
		       SubA.X == SubB.X && SubA.Y == SubB.Y;
	}

	return false;
}

bool FNarrowPhaseConnectivity::IsBoundaryCell(
	const FGridCellLayout& GridLayout,
	int32 CellId,
	const TSet<int32>& DestroyedCells)
{
	for (int32 Neighbor : GridLayout.GetCellNeighbors(CellId))
	{
		if (DestroyedCells.Contains(Neighbor))
		{
			return true;  // 파괴된 셀과 인접 = 경계
		}
	}
	return false;
}
