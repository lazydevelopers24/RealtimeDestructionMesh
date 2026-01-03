// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrity/StructuralIntegrityTypes.h"
#include "CellStructure/CellStructureTypes.h"

/**
 * 구조적 무결성 코어 시스템
 *
 * 특징:
 * - 순수 C++ (UObject 아님)
 * - 스레드 안전 (읽기/쓰기 락)
 * - 결정론적 (같은 입력 -> 같은 출력)
 *
 * 사용법:
 * 1. CellStructureData로 Initialize()
 * 2. AutoDetectFloorAnchors() 또는 SetAnchor()로 Anchor 설정
 * 3. Hit 발생 시 ProcessHit() 호출
 * 4. FStructuralIntegrityResult의 DetachedGroups로 파편 처리
 */
class REALTIMEDESTRUCTION_API FStructuralIntegritySystem
{
public:
	FStructuralIntegritySystem() = default;
	~FStructuralIntegritySystem() = default;

	// 복사/이동 금지 (내부 상태 보호)
	FStructuralIntegritySystem(const FStructuralIntegritySystem&) = delete;
	FStructuralIntegritySystem& operator=(const FStructuralIntegritySystem&) = delete;

	//=========================================================================
	// 초기화
	//=========================================================================

	/**
	 * CellStructureData로부터 초기화
	 * @param CellData - 빌드된 Cell 구조
	 * @param Settings - 구조적 무결성 설정
	 */
	void Initialize(const FCellStructureData& CellData, const FStructuralIntegritySettings& Settings);

	/** 리셋 */
	void Reset();

	/** 초기화 여부 */
	bool IsInitialized() const { return bInitialized; }

	/** Cell 개수 */
	int32 GetCellCount() const;

	//=========================================================================
	// Anchor 관리
	//=========================================================================

	/**
	 * 특정 Cell을 Anchor로 설정/해제
	 * @param CellId - Cell ID
	 * @param bIsAnchor - true면 Anchor로 설정
	 */
	void SetAnchor(int32 CellId, bool bIsAnchor);

	/**
	 * 여러 Cell을 Anchor로 설정
	 * @param CellIds - Cell ID 목록
	 * @param bIsAnchor - true면 Anchor로 설정
	 */
	void SetAnchors(const TArray<int32>& CellIds, bool bIsAnchor);

	/**
	 * 바닥면 Cell 자동 감지하여 Anchor 설정
	 * Z좌표가 가장 낮은 Cell들 중 HeightThreshold 이내의 Cell을 Anchor로 설정
	 * @param HeightThreshold - 바닥 기준 높이 임계값
	 */
	void AutoDetectFloorAnchors(float HeightThreshold);

	/** Anchor 목록 조회 (스레드 안전) */
	TArray<int32> GetAnchorCellIds() const;

	/** Cell이 Anchor인지 확인 (스레드 안전) */
	bool IsAnchor(int32 CellId) const;

	/** Anchor 개수 */
	int32 GetAnchorCount() const;

	//=========================================================================
	// Hit 처리
	//=========================================================================

	/**
	 * Hit 이벤트 처리 (메인 진입점)
	 *
	 * @param HitCellId - 직접 맞은 Cell ID
	 * @param Damage - 데미지 양
	 * @param DamageRadius - 데미지 반경 (Cell 그래프 거리, 0이면 직접 타격만)
	 * @return 처리 결과 (파괴된 Cell, 분리된 그룹 등)
	 */
	FStructuralIntegrityResult ProcessHit(int32 HitCellId, float Damage, int32 DamageRadius = 0);

	/**
	 * 위치로 Cell ID 찾기
	 * @param WorldLocation - 월드 좌표
	 * @return Cell ID (없으면 INDEX_NONE)
	 */
	int32 FindCellAtLocation(const FVector& WorldLocation) const;

	/**
	 * 위치로 Hit 처리
	 * @param WorldLocation - 월드 좌표
	 * @param Damage - 데미지 양
	 * @param DamageRadius - 데미지 반경
	 * @return 처리 결과
	 */
	FStructuralIntegrityResult ProcessHitAtLocation(const FVector& WorldLocation, float Damage, int32 DamageRadius = 0);

	//=========================================================================
	// 상태 조회 (스레드 안전)
	//=========================================================================

	/** Cell 상태 조회 */
	ECellStructuralState GetCellState(int32 CellId) const;

	/** Cell 체력 조회 */
	float GetCellHealth(int32 CellId) const;

	/** Cell 체력 정규화 (0~1) */
	float GetCellHealthNormalized(int32 CellId) const;

	/** Cell이 Anchor에 연결되어 있는지 확인 */
	bool IsCellConnectedToAnchor(int32 CellId) const;

	/** 파괴된 Cell 개수 */
	int32 GetDestroyedCellCount() const;

	/** 파괴된 Cell ID 목록 (네트워크 동기화용) */
	TArray<int32> GetDestroyedCellIds() const;

	/** Cell의 월드 좌표 (Seed Voxel 기준) */
	FVector GetCellWorldPosition(int32 CellId) const;

	//=========================================================================
	// 강제 상태 설정 (네트워크 동기화용)
	//=========================================================================

	/**
	 * 늦게 접속한 클라이언트를 위한 상태 동기화
	 * @param DestroyedIds - 파괴된 Cell ID 목록
	 * @return 분리된 그룹 목록
	 */
	TArray<FDetachedCellGroup> ForceSetDestroyedCells(const TArray<int32>& DestroyedIds);

	//=========================================================================
	// 설정 접근
	//=========================================================================

	const FStructuralIntegritySettings& GetSettings() const { return Settings; }
	void SetSettings(const FStructuralIntegritySettings& NewSettings);

	//=========================================================================
	// CellData 접근 (읽기 전용)
	//=========================================================================

	const FCellStructureData* GetCellData() const { return CellDataPtr; }

private:
	//=========================================================================
	// 내부 알고리즘
	//=========================================================================

	/**
	 * 데미지 적용 (반경 내 Cell들)
	 * @param CenterCellId - 중심 Cell
	 * @param Damage - 기본 데미지
	 * @param Radius - 그래프 거리 반경
	 * @param OutNewlyDestroyed - 새로 파괴된 Cell 목록 (출력)
	 */
	void ApplyDamage(int32 CenterCellId, float Damage, int32 Radius, TArray<int32>& OutNewlyDestroyed);

	/**
	 * Cell 파괴 처리
	 * @param CellId - 파괴할 Cell
	 * @return 파괴 성공 여부
	 */
	bool DestroyCell(int32 CellId);

	/**
	 * 연결성 업데이트 후 분리된 그룹 찾기
	 * @return 분리된 그룹 목록
	 */
	TArray<FDetachedCellGroup> UpdateConnectivityAndFindDetached();

	/**
	 * Anchor로부터 연결된 모든 Cell 찾기 (BFS)
	 * Lock 내에서 호출되어야 함
	 * @return 연결된 Cell 집합
	 */
	TSet<int32> FindAllConnectedToAnchors_Internal() const;

	/**
	 * 분리된 Cell들을 연결 그룹으로 묶기
	 * @param DetachedCellIds - 분리된 Cell ID 목록
	 * @return 그룹 목록
	 */
	TArray<FDetachedCellGroup> BuildDetachedGroups(const TArray<int32>& DetachedCellIds);

	/**
	 * 그룹의 질량 중심 계산
	 * @param CellIds - Cell ID 목록
	 * @return 질량 중심 좌표
	 */
	FVector CalculateCenterOfMass(const TArray<int32>& CellIds) const;

	/**
	 * 그룹에 포함된 삼각형 ID 수집
	 * @param CellIds - Cell ID 목록
	 * @return 삼각형 ID 목록
	 */
	TArray<int32> CollectTriangleIds(const TArray<int32>& CellIds) const;

	/**
	 * BFS로 Cell 그래프 거리 계산
	 * @param StartCellId - 시작 Cell
	 * @param MaxDistance - 최대 거리
	 * @param OutCellsWithDistance - (CellId, Distance) 쌍 목록 (출력)
	 */
	void BFSFindCellsInRadius(int32 StartCellId, int32 MaxDistance,
		TArray<TPair<int32, int32>>& OutCellsWithDistance) const;

	/**
	 * Voxel 좌표를 월드 좌표로 변환
	 * @param VoxelCoord - Voxel 좌표
	 * @return 월드 좌표
	 */
	FVector VoxelToWorld(const FIntVector& VoxelCoord) const;

	//=========================================================================
	// 데이터
	//=========================================================================

	// 설정
	FStructuralIntegritySettings Settings;

	// 런타임 데이터
	FStructuralIntegrityData Data;

	// CellStructure 참조 (소유하지 않음)
	const FCellStructureData* CellDataPtr = nullptr;

	// 초기화 상태
	bool bInitialized = false;

	// 스레드 동기화
	mutable FRWLock DataLock;

	// 분리 그룹 ID 카운터
	int32 NextGroupId = 0;
};
