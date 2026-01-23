#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrity/GridCellTypes.h"

/**
 * SubCell 처리기
 * Tool mesh와의 충돌을 기반으로 subcell을 dead 처리하고
 * 구조적 무결성 검사를 위한 데이터를 준비하는 유틸 클래스
 */
class REALTIMEDESTRUCTION_API FSubCellProcessor
{
public:
	FSubCellProcessor() = default;

	/**
	 * 파괴 형태와 겹치는 subcell들을 dead 처리
	 *
	 * @param QuantizedShape - 양자화된 파괴 형태 (네트워크 결정론적)
	 * @param MeshTransform - 메시의 월드 트랜스폼
	 * @param GridLayout - 격자 셀 레이아웃 (읽기 전용)
	 * @param InOutCellState - 셀 상태 (subcell 상태 업데이트)
	 * @param OutAffectedCells - 영향받은 cell ID 목록 (출력)
	 * @param OutNewlyDeadSubCells - 새로 dead된 subcell 정보 (출력, 선택적)
	 * @return 처리 성공 여부
	 */
	static bool ProcessSubCellDestruction(
		const FQuantizedDestructionInput& QuantizedShape,
		const FTransform& MeshTransform,
		const FGridCellLayout& GridLayout,
		FCellState& InOutCellState,
		TArray<int32>& OutAffectedCells,
		TMap<int32, TArray<int32>>* OutNewlyDeadSubCells = nullptr);
	
	/**
	 * 특정 cell의 살아있는 subcell 개수 반환
	 *
	 * @param CellId - 셀 ID
	 * @param CellState - 셀 상태
	 * @return 살아있는 subcell 개수
	 */
	static int32 CountLiveSubCells(int32 CellId, const FCellState& CellState);

	/**
	 * 특정 cell이 완전히 파괴되었는지 확인
	 * (모든 subcell이 dead)
	 *
	 * @param CellId - 셀 ID
	 * @param CellState - 셀 상태
	 * @return 완전 파괴 여부
	 */
	static bool IsCellFullyDestroyed(int32 CellId, const FCellState& CellState);

	/**
	 * 특정 방향 경계면의 subcell ID 목록 반환
	 *
	 * @param Direction - 방향 (0-5: -X, +X, -Y, +Y, -Z, +Z)
	 * @return 해당 방향 경계면의 subcell ID 배열
	 */
	static TArray<int32> GetBoundarySubCellIds(int32 Direction);

	/**
	 * 특정 방향 경계면의 살아있는 subcell 비트마스크 반환
	 *
	 * @param CellId - 셀 ID
	 * @param Direction - 방향 (0-5)
	 * @param CellState - 셀 상태
	 * @return 경계면 subcell 비트마스크 (최대 25비트 사용)
	 */
	static uint32 GetBoundaryLiveSubCellMask(int32 CellId, int32 Direction, const FCellState& CellState);

private:
	/**
	 * 양자화된 Shape의 AABB 계산
	 */
	static FBox ComputeShapeAABB(const FQuantizedDestructionInput& Shape);
};
