// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrity/StructuralIntegrityTypes.h"

/**
 * 구조적 무결성 시스템 초기화 데이터
 *
 * CellGraph로부터 변환된 연결성 그래프 정보만 포함.
 * Cell의 기하학적 정보(위치, 삼각형)는 CellGraph가 보유.
 */
struct REALTIMEDESTRUCTION_API FStructuralIntegrityInitData
{
	// Cell별 이웃 Cell ID 목록 (그래프 연결성)
	TArray<TArray<int32>> CellNeighbors;

	// Anchor로 지정할 Cell ID 목록 (CellGraph에서 판정)
	TArray<int32> AnchorCellIds;

	int32 GetCellCount() const { return CellNeighbors.Num(); }

	bool IsValid() const
	{
		return CellNeighbors.Num() > 0;
	}
};

/**
 * 구조적 무결성 코어 시스템
 *
 * 특징:
 * - 순수 C++ (UObject 아님)
 * - 스레드 안전 (읽기/쓰기 락)
 * - 결정론적 (같은 입력 -> 같은 출력)
 *
 * 사용법:
 * 1. FStructuralIntegrityInitData로 Initialize()
 * 2. AutoDetectFloorAnchors() 또는 SetAnchor()로 Anchor 설정
 * 3. DestroyCells() 호출하여 Cell 파괴
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
	 * 초기화 데이터로부터 시스템 초기화
	 * @param InitData - Cell 연결성 그래프 및 Anchor 정보
	 * @param Settings - 구조적 무결성 설정
	 */
	void Initialize(const FStructuralIntegrityInitData& InitData, const FStructuralIntegritySettings& Settings);

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

	/** Anchor 목록 조회 (스레드 안전) */
	TArray<int32> GetAnchorCellIds() const;

	/** Cell이 Anchor인지 확인 (스레드 안전) */
	bool IsAnchor(int32 CellId) const;

	/** Anchor 개수 */
	int32 GetAnchorCount() const;

	//=========================================================================
	// Cell 파괴
	//=========================================================================

	/**
	 * 지정된 Cell들을 파괴하고 연결성 업데이트
	 * @param CellIds - 파괴할 Cell ID 목록
	 * @return 처리 결과 (파괴된 Cell, 분리된 그룹 등)
	 */
	FStructuralIntegrityResult DestroyCells(const TArray<int32>& CellIds);

	/**
	 * 단일 Cell 파괴
	 * @param CellId - 파괴할 Cell ID
	 * @return 처리 결과
	 */
	FStructuralIntegrityResult DestroyCell(int32 CellId);

	//=========================================================================
	// 상태 조회 (스레드 안전)
	//=========================================================================

	/** Cell 상태 조회 */
	ECellStructuralState GetCellState(int32 CellId) const;

	/** Cell이 Anchor에 연결되어 있는지 확인 */
	bool IsCellConnectedToAnchor(int32 CellId) const;

	/** 파괴된 Cell 개수 */
	int32 GetDestroyedCellCount() const;

	/** 파괴된 Cell ID 목록 (네트워크 동기화용) */
	TArray<int32> GetDestroyedCellIds() const;

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
	// 그래프 동기화 API (신규)
	//=========================================================================

	/**
	 * 그래프 스냅샷으로 내부 상태 동기화
	 * - 새 Key: 새 ID 할당, Intact 상태
	 * - 스냅샷에 없는 Key: Destroyed로 마킹 (ID 유지)
	 * - 이웃 리스트 재구축
	 * @param Snapshot - CellGraph로부터 생성된 스냅샷
	 */
	void SyncGraph(const FStructuralIntegrityGraphSnapshot& Snapshot);

	/**
	 * BFS로 연결성 재계산, Detached 그룹 반환
	 * @return 분리된 그룹 정보를 포함한 결과
	 */
	FStructuralIntegrityResult RefreshConnectivity();

	/**
	 * 셀들을 Destroyed로 마킹 (파편 스폰 후 호출)
	 * @param Keys - Destroyed로 마킹할 Cell Key 목록
	 */
	void MarkCellsAsDestroyed(const TArray<FCellKey>& Keys);

	//=========================================================================
	// Key 기반 조회 API
	//=========================================================================

	/** Key로 내부 ID 조회 (없으면 INDEX_NONE) */
	int32 GetCellIdForKey(const FCellKey& Key) const;

	/** 내부 ID로 Key 조회 */
	FCellKey GetKeyForCellId(int32 CellId) const;

	/** 파괴된 Cell의 Key 목록 */
	TArray<FCellKey> GetDestroyedCellKeys() const;

private:
	//=========================================================================
	// 내부 알고리즘
	//=========================================================================

	/**
	 * 단일 Cell 파괴 처리 (내부용)
	 * @param CellId - 파괴할 Cell
	 * @return 파괴 성공 여부 (이미 파괴됨 또는 유효하지 않으면 false)
	 */
	bool DestroyCellInternal(int32 CellId);

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
	 * @return 그룹 목록 (CellIds만 포함, 기하학적 정보는 CellGraph에서 조회)
	 */
	TArray<FDetachedCellGroup> BuildDetachedGroups(const TArray<int32>& DetachedCellIds);

	/**
	 * Key에 대한 내부 ID 조회/할당
	 * @param Key - Cell Key
	 * @param bCreateIfNotFound - true면 없을 때 새 ID 할당
	 * @return 내부 ID (없고 생성 안 하면 INDEX_NONE)
	 */
	int32 FindOrAllocateCellId(const FCellKey& Key, bool bCreateIfNotFound = true);

	/**
	 * 스냅샷 기반으로 이웃 리스트 재구축
	 * @param Snapshot - 그래프 스냅샷
	 */
	void RebuildNeighborLists(const FStructuralIntegrityGraphSnapshot& Snapshot);

	//=========================================================================
	// 데이터
	//=========================================================================

	// 설정
	FStructuralIntegritySettings Settings;

	// 런타임 데이터
	FStructuralIntegrityData Data;

	// Cell 연결성 (Initialize 시 복사, DisconnectCells로 동적 수정 가능)
	TArray<TArray<int32>> CellNeighbors;

	// 초기화 상태
	bool bInitialized = false;

	// 스레드 동기화
	mutable FRWLock DataLock;

	// 분리 그룹 ID 카운터
	int32 NextGroupId = 0;

	//=========================================================================
	// Key <-> ID 매핑 (신규)
	//=========================================================================

	// Key -> 내부 ID
	TMap<FCellKey, int32> KeyToId;

	// 내부 ID -> Key
	TArray<FCellKey> IdToKey;

	// 다음에 할당할 내부 ID (단조 증가, 재사용 없음)
	int32 NextInternalId = 0;
};
