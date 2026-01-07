// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrityTypes.generated.h"

/**
 * Cell의 구조적 상태
 */
UENUM(BlueprintType)
enum class ECellStructuralState : uint8
{
	Intact,      // 온전함 - Anchor에 연결된 상태
	Destroyed,   // 파괴됨 - Cell이 파괴되어 그래프에서 제거됨
	Detached     // 분리됨 - Anchor와의 연결이 끊어져 떨어질 예정
};

/**
 * 안정 Cell 식별자
 *
 * CellGraph의 노드 인덱스는 그래프 재구축 시 변경될 수 있으므로,
 * (ChunkId, CellId) 쌍을 안정적인 외부 식별자로 사용.
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

	// 결정론적 정렬용 (ChunkId 우선, 같으면 CellId로)
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
 * 분리된 조각 그룹
 *
 * Anchor와의 연결이 끊어진 Cell들의 집합.
 * IntegritySystem은 CellIds만 채우고, 기하학적 정보(CenterOfMass, TriangleIds)는
 * 상위 레벨에서 CellGraph를 통해 채운다.
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedCellGroup
{
	GENERATED_BODY()

	// 이 그룹의 고유 ID
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	int32 GroupId = INDEX_NONE;

	// 포함된 Cell ID 목록 (IntegritySystem이 채움, 레거시 호환용)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<int32> CellIds;

	// 포함된 Cell Key 목록 (IntegritySystem이 채움, 신규 API)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<FCellKey> CellKeys;

	// 그룹의 질량 중심 (상위 레벨에서 CellGraph를 통해 채움)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	FVector CenterOfMass = FVector::ZeroVector;

	// 그룹의 대략적인 질량 (Cell 개수 기반)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	float ApproximateMass = 0.0f;

	// 포함된 삼각형 ID 목록 (상위 레벨에서 CellGraph를 통해 채움)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<int32> TriangleIds;
};

/**
 * 구조적 무결성 설정
 *
 * Anchor 감지는 CellGraph에서 담당하므로 여기서는 성능/동작 관련 설정만 포함.
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FStructuralIntegritySettings
{
	GENERATED_BODY()

	// 연결성 체크를 비동기로 실행할 Cell 임계값
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance",
		meta = (ClampMin = "100"))
	int32 AsyncThreshold = 1000;

	// 비동기 처리 활성화
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance")
	bool bEnableAsync = true;

	// 병렬 처리 활성화 (ParallelFor)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance")
	bool bEnableParallel = true;

	// 병렬 처리 임계값
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Performance",
		meta = (EditCondition = "bEnableParallel", ClampMin = "100"))
	int32 ParallelThreshold = 500;

	// 붕괴 딜레이 (0이면 즉시)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Collapse",
		meta = (ClampMin = "0.0"))
	float CollapseDelay = 0.0f;
};

/**
 * 구조적 무결성 런타임 데이터 (Non-USTRUCT, 순수 C++)
 *
 * 네트워크 동기화 불필요 - CellStructure가 결정론적이므로
 * 동일 Seed + 동일 Hit 순서 = 동일 결과
 */
struct REALTIMEDESTRUCTION_API FStructuralIntegrityData
{
	// Anchor Cell ID 집합
	TSet<int32> AnchorCellIds;

	// 각 Cell의 상태
	TArray<ECellStructuralState> CellStates;

	// 파괴된 Cell ID 집합 (빠른 조회용)
	TSet<int32> DestroyedCellIds;

	// Anchor에 연결된 Cell 집합 (캐시)
	TSet<int32> ConnectedToAnchorCache;

	// 캐시 유효 여부
	bool bCacheValid = false;

	/**
	 * 초기화
	 * @param CellCount - 총 Cell 개수
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
 * Hit 이벤트 (네트워크 동기화용, 압축)
 * 기존 FCompactDestructionOp과 유사하게 최소 데이터만 전송
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FStructuralHitEvent
{
	GENERATED_BODY()

	// 파괴할 Cell ID 목록
	UPROPERTY()
	TArray<int32> DestroyedCellIds;

	// 시퀀스 번호 (결정론적 순서 보장)
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
 * 구조적 무결성 변경 결과
 * ProcessHit의 결과를 담는 구조체
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FStructuralIntegrityResult
{
	GENERATED_BODY()

	// 새로 파괴된 Cell ID 목록
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	TArray<int32> NewlyDestroyedCellIds;

	// 분리된 그룹 목록
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	TArray<FDetachedCellGroup> DetachedGroups;

	// 전체 붕괴 여부 (모든 Anchor 파괴)
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	bool bStructureCollapsed = false;

	// 총 파괴된 Cell 수
	UPROPERTY(BlueprintReadOnly, Category = "StructuralIntegrityResult")
	int32 TotalDestroyedCount = 0;

	bool HasChanges() const
	{
		return NewlyDestroyedCellIds.Num() > 0 || DetachedGroups.Num() > 0;
	}
};

/**
 * 그래프 스냅샷 - 노드별 이웃 목록 (UPROPERTY 지원용 래퍼)
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
 * 그래프 스냅샷
 *
 * CellGraph의 현재 상태를 IntegritySystem에 전달하기 위한 구조체.
 * 결정론성을 위해 모든 배열은 정렬된 상태로 유지.
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FStructuralIntegrityGraphSnapshot
{
	GENERATED_BODY()

	// 정렬된 노드 Key 목록 (ChunkId, CellId 오름차순)
	UPROPERTY()
	TArray<FCellKey> NodeKeys;

	// 각 노드의 이웃 Key 목록 (NodeKeys와 동일 인덱스)
	UPROPERTY()
	TArray<FStructuralIntegrityNeighborList> NeighborKeys;

	// Anchor 노드 Key 목록
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
