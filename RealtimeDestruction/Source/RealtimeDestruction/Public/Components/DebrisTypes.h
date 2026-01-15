// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrity/StructuralIntegrityTypes.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "ProceduralMeshComponent.h"
#include "DebrisTypes.generated.h"

//////////////////////////////////////////////////////////////////////////
// Debris Enums
//////////////////////////////////////////////////////////////////////////

/**
 * 파편 타입 분류
 */
UENUM(BlueprintType)
enum class EDebrisType : uint8
{
	Cosmetic,    // 로컬 전용, 짧은 수명, 게임플레이 영향 없음
	Gameplay     // 서버 권한, 복제됨, 물리 상호작용 가능
};

/**
 * 파편 크기 티어
 */
UENUM(BlueprintType)
enum class EDebrisTier : uint8
{
	Tiny,      // < 100 cm³ - 파티클로 대체
	Small,     // 100-500 cm³ - Sphere collision
	Medium,    // 500-2000 cm³ - Box collision
	Large,     // 2000-10000 cm³ - Convex Hull
	Massive    // > 10000 cm³ - Complex collision
};

//////////////////////////////////////////////////////////////////////////
// Debris Structs
//////////////////////////////////////////////////////////////////////////

/**
 * 파편 티어별 설정
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDebrisTierConfig
{
	GENERATED_BODY()

	// 이 티어의 볼륨 상한 (cm³)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier")
	float VolumeThreshold = 0.0f;

	// 파편 수명 (초, 0이면 영구)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier")
	float Lifespan = 3.0f;

	// 최대 파편 개수
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier", meta = (ClampMin = "1"))
	int32 MaxCount = 50;

	// Cosmetic 여부 (false면 Gameplay - 네트워크 복제)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier")
	bool bIsCosmetic = true;

	FDebrisTierConfig() = default;

	FDebrisTierConfig(float InVolumeThreshold, float InLifespan, int32 InMaxCount, bool bInIsCosmetic)
		: VolumeThreshold(InVolumeThreshold)
		, Lifespan(InLifespan)
		, MaxCount(InMaxCount)
		, bIsCosmetic(bInIsCosmetic)
	{
	}
};

/**
 * 파편 스폰 설정
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDebrisSpawnSettings
{
	GENERATED_BODY()

	// 파편 스폰 활성화
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris")
	bool bEnableDebrisSpawn = true;

	// Gameplay 파편 볼륨 임계값 (cm³) - 이 값 초과 시 Gameplay 타입
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris")
	float GameplayVolumeThreshold = 2000.0f;

	// Cosmetic 파편 기본 수명 (초)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Cosmetic")
	float CosmeticLifespan = 3.0f;

	// Cosmetic 파편 최대 개수
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Cosmetic", meta = (ClampMin = "1"))
	int32 MaxCosmeticDebris = 50;

	// Gameplay 파편 기본 수명 (초, 0이면 영구)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Gameplay")
	float GameplayLifespan = 0.0f;

	// Gameplay 파편 최대 개수
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Gameplay", meta = (ClampMin = "1"))
	int32 MaxGameplayDebris = 20;

	// 파편 초기 임펄스 수평 강도
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Physics")
	float InitialImpulseHorizontal = 100.0f;

	// 파편 초기 임펄스 수직 강도
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Physics")
	float InitialImpulseVertical = 150.0f;

	// 볼륨으로 Debris 타입 결정
	EDebrisType GetDebrisType(float Volume) const
	{
		return (Volume <= GameplayVolumeThreshold) ? EDebrisType::Cosmetic : EDebrisType::Gameplay;
	}

	// 타입별 수명 반환
	float GetLifespanForType(EDebrisType Type) const
	{
		return (Type == EDebrisType::Cosmetic) ? CosmeticLifespan : GameplayLifespan;
	}

	// 타입별 최대 개수 반환
	int32 GetMaxCountForType(EDebrisType Type) const
	{
		return (Type == EDebrisType::Cosmetic) ? MaxCosmeticDebris : MaxGameplayDebris;
	}
};

/**
 * 압축된 파편 동기화 Op (네트워크 전송용)
 * 기존 FCompactDestructionOp 패턴 따름
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FCompactDebrisOp
{
	GENERATED_BODY()

	// Cell Key 압축 배열: (ChunkId << 16) | CellId
	UPROPERTY()
	TArray<int32> PackedCellKeys;

	// 그룹 ID
	UPROPERTY()
	int32 GroupId = INDEX_NONE;

	// 질량 중심 (1cm 정밀도)
	UPROPERTY()
	FVector_NetQuantize CenterOfMass;

	// 대략적 볼륨 (cm³, 압축)
	UPROPERTY()
	float ApproximateVolume = 0.0f;

	// 시퀀스 번호
	UPROPERTY()
	uint16 Sequence = 0;

	FCompactDebrisOp() = default;

	// Cell Key 압축
	static void PackCellKeys(const TArray<FCellKey>& Keys, TArray<int32>& OutPacked)
	{
		OutPacked.Reset();
		OutPacked.Reserve(Keys.Num());
		for (const FCellKey& Key : Keys)
		{
			OutPacked.Add((Key.ChunkId << 16) | (Key.CellId & 0xFFFF));
		}
	}

	// Cell Key 압축 해제
	static void UnpackCellKeys(const TArray<int32>& Packed, TArray<FCellKey>& OutKeys)
	{
		OutKeys.Reset();
		OutKeys.Reserve(Packed.Num());
		for (int32 P : Packed)
		{
			OutKeys.Add(FCellKey(P >> 16, P & 0xFFFF));
		}
	}

	// FDetachedCellGroup으로부터 생성
	static FCompactDebrisOp FromDetachedGroup(const FDetachedCellGroup& Group, uint16 InSequence)
	{
		FCompactDebrisOp Op;
		PackCellKeys(Group.CellKeys, Op.PackedCellKeys);
		Op.GroupId = Group.GroupId;
		Op.CenterOfMass = Group.CenterOfMass;
		Op.ApproximateVolume = Group.ApproximateMass;
		Op.Sequence = InSequence;
		return Op;
	}

	// FDetachedCellGroup으로 복원
	FDetachedCellGroup ToDetachedGroup() const
	{
		FDetachedCellGroup Group;
		Group.GroupId = GroupId;
		UnpackCellKeys(PackedCellKeys, Group.CellKeys);
		Group.CenterOfMass = CenterOfMass;
		Group.ApproximateMass = ApproximateVolume;
		return Group;
	}
};

//////////////////////////////////////////////////////////////////////////
// PMC Debris Tracking (Phase 1.5)
//////////////////////////////////////////////////////////////////////////

/**
 * Gameplay 파편 추적 데이터 (PMC → DMC 전환용)
 *
 * UDynamicMeshComponent는 TriMesh(Complex) 충돌만 지원하여 동적 물리 시뮬레이션 불가.
 * 따라서 물리 시뮬레이션 중에는 UProceduralMeshComponent(PMC)를 사용하고,
 * 안정화 후 UDynamicMeshComponent(DMC)로 전환하여 2차 파괴를 지원함.
 */
struct REALTIMEDESTRUCTION_API FGameplayDebrisTracker
{
	/** PMC 컴포넌트 (물리 시뮬레이션용) */
	TWeakObjectPtr<UProceduralMeshComponent> PMC;

	/** 원본 메시 데이터 (DMC 전환용 보존) */
	TSharedPtr<UE::Geometry::FDynamicMesh3> OriginalMesh;

	/** 안정 상태 지속 시간 (초) */
	float StableTime = 0.0f;

	/** 파편 타입 */
	EDebrisType DebrisType = EDebrisType::Gameplay;

	/** 안정화 판정 임계 속도 (cm/s) */
	static constexpr float StableVelocityThreshold = 5.0f;

	/** 안정화 필요 지속 시간 (초) */
	static constexpr float StableTimeRequired = 0.5f;

	FGameplayDebrisTracker() = default;

	FGameplayDebrisTracker(UProceduralMeshComponent* InPMC, TSharedPtr<UE::Geometry::FDynamicMesh3> InMesh, EDebrisType InType = EDebrisType::Gameplay)
		: PMC(InPMC)
		, OriginalMesh(InMesh)
		, StableTime(0.0f)
		, DebrisType(InType)
	{
	}

	/** PMC가 유효한지 확인 */
	bool IsValid() const { return PMC.IsValid(); }

	/** 안정화 완료 여부 */
	bool IsStabilized() const { return StableTime >= StableTimeRequired; }

	/** 안정화 타이머 리셋 */
	void ResetStableTime() { StableTime = 0.0f; }

	/** 안정화 시간 누적 */
	void AccumulateStableTime(float DeltaTime) { StableTime += DeltaTime; }
};
