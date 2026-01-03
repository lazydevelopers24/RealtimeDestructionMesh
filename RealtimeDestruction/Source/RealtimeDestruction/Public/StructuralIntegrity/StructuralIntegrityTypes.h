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
	Intact,      // 온전함
	Damaged,     // 손상됨 (연결 유지)
	Destroyed,   // 파괴됨 (연결 끊김)
	Detached     // 분리됨 (Anchor 없음, 떨어질 예정)
};

/**
 * 분리된 조각 그룹
 * Anchor와의 연결이 끊어진 Cell들의 집합
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedCellGroup
{
	GENERATED_BODY()

	// 이 그룹의 고유 ID
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	int32 GroupId = INDEX_NONE;

	// 포함된 Cell ID 목록
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<int32> CellIds;

	// 그룹의 질량 중심 (파편 스폰 위치)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	FVector CenterOfMass = FVector::ZeroVector;

	// 그룹의 대략적인 질량 (Cell 개수 기반)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	float ApproximateMass = 0.0f;

	// 포함된 삼각형 ID 목록 (메쉬 추출용)
	UPROPERTY(BlueprintReadOnly, Category = "DetachedCellGroup")
	TArray<int32> TriangleIds;
};

/**
 * 구조적 무결성 설정
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FStructuralIntegritySettings
{
	GENERATED_BODY()

	// Anchor 자동 감지: 바닥면 Cell들을 Anchor로
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Anchor")
	bool bAutoDetectFloorAnchors = true;

	// Anchor 감지 높이 임계값
	// 1.0 이하: VoxelSize의 배수 (0.5 = VoxelSize의 절반, 1.0 = VoxelSize 1개)
	// 1.0 초과: 절대값 (단위: cm)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Anchor",
		meta = (EditCondition = "bAutoDetectFloorAnchors", ClampMin = "0.0"))
	float FloorHeightThreshold = 0.5f;

	// Cell 기본 체력
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Damage",
		meta = (ClampMin = "0.1"))
	float DefaultCellHealth = 100.0f;

	// 데미지 감쇠 (거리에 따른)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StructuralIntegrity|Damage",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DamageFalloff = 0.5f;

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

	// 각 Cell의 체력 (0.0 ~ MaxHealth)
	TArray<float> CellHealth;

	// 각 Cell의 최대 체력
	float MaxHealth = 100.0f;

	// 파괴된 Cell ID 집합 (빠른 조회용)
	TSet<int32> DestroyedCellIds;

	// Anchor에 연결된 Cell 집합 (캐시)
	TSet<int32> ConnectedToAnchorCache;

	// 캐시 유효 여부
	bool bCacheValid = false;

	/**
	 * 초기화
	 * @param CellCount - 총 Cell 개수
	 * @param InMaxHealth - Cell 최대 체력
	 */
	void Initialize(int32 CellCount, float InMaxHealth = 100.0f)
	{
		MaxHealth = InMaxHealth;
		CellStates.SetNumZeroed(CellCount);
		CellHealth.SetNum(CellCount);

		for (int32 i = 0; i < CellCount; ++i)
		{
			CellStates[i] = ECellStructuralState::Intact;
			CellHealth[i] = MaxHealth;
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
		CellHealth.Reset();
		DestroyedCellIds.Reset();
		ConnectedToAnchorCache.Reset();
		bCacheValid = false;
		MaxHealth = 100.0f;
	}

	int32 GetCellCount() const
	{
		return CellStates.Num();
	}

	bool IsValidCellId(int32 CellId) const
	{
		return CellId >= 0 && CellId < CellStates.Num();
	}

	float GetHealthNormalized(int32 CellId) const
	{
		if (!IsValidCellId(CellId) || MaxHealth <= 0.0f)
		{
			return 0.0f;
		}
		return FMath::Clamp(CellHealth[CellId] / MaxHealth, 0.0f, 1.0f);
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

	// 압축된 Hit 위치
	UPROPERTY()
	FVector_NetQuantize HitLocation;

	// 데미지 양
	UPROPERTY()
	float Damage = 0.0f;

	// 데미지 반경 (Cell 단위)
	UPROPERTY()
	uint8 DamageRadius = 0;

	// 시퀀스 번호 (결정론적 순서 보장)
	UPROPERTY()
	uint16 Sequence = 0;

	FStructuralHitEvent() = default;

	FStructuralHitEvent(const FVector& InLocation, float InDamage, uint8 InRadius, uint16 InSequence)
		: HitLocation(InLocation)
		, Damage(InDamage)
		, DamageRadius(InRadius)
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
