// Copyright 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrity/GridCellTypes.h"

/**
 * 2-Phase 연결 검사 (Narrow Phase)
 * 얇은 연결 부분의 정확한 판정을 위해 경계 셀을 3x3x3으로 세분화
 */
class REALTIMEDESTRUCTION_API FNarrowPhaseConnectivity
{
public:
	/** 서브셀 분할 수 (3x3x3 = 27 서브셀) */
	static constexpr int32 SubDivision = 3;

	/**
	 * 두 인접 셀이 Narrow Phase에서도 연결되어 있는지 확인
	 *
	 * @param CellA - 첫 번째 셀 ID
	 * @param CellB - 두 번째 셀 ID (CellA와 인접해야 함)
	 * @param GridLayout - 격자 레이아웃
	 * @param MeshTransform - 메시 트랜스폼
	 * @param Destructions - 파괴 형상 목록
	 * @return 연결되어 있으면 true
	 */
	static bool AreNarrowConnected(
		int32 CellA,
		int32 CellB,
		const FGridCellLayout& GridLayout,
		const FTransform& MeshTransform,
		const TArray<FQuantizedDestructionInput>& Destructions);

	/**
	 * Narrow Phase를 포함한 BFS 연결 검사
	 *
	 * @param GridLayout - 격자 레이아웃
	 * @param DestroyedCells - 파괴된 셀 집합
	 * @param MeshTransform - 메시 트랜스폼
	 * @param Destructions - 파괴 형상 목록
	 * @return 분리된 셀 ID 집합
	 */
	static TSet<int32> FindDisconnectedCellsWithNarrowPhase(
		const FGridCellLayout& GridLayout,
		const TSet<int32>& DestroyedCells,
		const FTransform& MeshTransform,
		const TArray<FQuantizedDestructionInput>& Destructions);

private:
	/**
	 * 셀 내 파괴되지 않은 서브셀 목록 반환
	 */
	static void GetIntactSubCells(
		int32 CellId,
		const FGridCellLayout& GridLayout,
		const FTransform& MeshTransform,
		const TArray<FQuantizedDestructionInput>& Destructions,
		TArray<FIntVector>& OutIntactSubCells);

	/**
	 * 두 서브셀이 경계면에서 인접한지 확인
	 */
	static bool AreSubCellsAdjacent(
		const FIntVector& SubA,
		const FIntVector& SubB,
		const FIntVector& CellDirection);

	/**
	 * 경계 셀 판정 (파괴된 셀과 인접한 셀)
	 */
	static bool IsBoundaryCell(
		const FGridCellLayout& GridLayout,
		int32 CellId,
		const TSet<int32>& DestroyedCells);
};
