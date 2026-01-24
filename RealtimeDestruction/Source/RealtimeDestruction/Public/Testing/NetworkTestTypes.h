// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// NetworkTestTypes.h
// 네트워크 테스트용 타입 정의

#pragma once

#include "CoreMinimal.h"
#include "NetworkTestTypes.generated.h"

/**
 * 네트워크 테스트 프리셋 열거형
 *
 * 다양한 네트워크 환경을 시뮬레이션하기 위한 프리셋
 */
UENUM(BlueprintType)
enum class ENetworkTestPreset : uint8
{
	/** 시뮬레이션 없음 */
	Off		UMETA(DisplayName = "Off - No Simulation"),

	/** 좋은 연결 (20ms) - 일반 사용자 */
	Good	UMETA(DisplayName = "Good - 20ms"),

	/** 보통 연결 (50ms) - 대다수 사용자 */
	Normal	UMETA(DisplayName = "Normal - 50ms"),

	/** 나쁜 연결 (100ms) - 와이파이 */
	Bad		UMETA(DisplayName = "Bad - 100ms"),

	/** 최악 환경 (200ms + 5% 손실) - 모바일/해외 */
	Worst	UMETA(DisplayName = "Worst - 200ms + 5% Loss")
};

/**
 * 네트워크 프리셋 설정값 구조체
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FNetworkTestPresetConfig
{
	GENERATED_BODY()

	/** 패킷 지연 (ms) - Net.PktLag */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	int32 PktLag = 0;

	/** 패킷 지연 변동 (ms) - Net.PktLagVariance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	int32 PktLagVariance = 0;

	/** 패킷 손실률 (%) - Net.PktLoss */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	int32 PktLoss = 0;

	/** 프리셋 이름 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	FString PresetName;

	/** 기본 생성자 */
	FNetworkTestPresetConfig()
		: PktLag(0)
		, PktLagVariance(0)
		, PktLoss(0)
		, PresetName(TEXT("Off"))
	{
	}

	/** 값 초기화 생성자 */
	FNetworkTestPresetConfig(int32 InLag, int32 InVariance, int32 InLoss, const FString& InName)
		: PktLag(InLag)
		, PktLagVariance(InVariance)
		, PktLoss(InLoss)
		, PresetName(InName)
	{
	}

	/** 설정이 활성화되어 있는지 (시뮬레이션 중인지) */
	bool IsActive() const
	{
		return PktLag > 0 || PktLoss > 0;
	}

	/** 문자열로 변환 */
	FString ToString() const
	{
		return FString::Printf(TEXT("%s (Lag:%dms Var:%dms Loss:%d%%)"),
			*PresetName, PktLag, PktLagVariance, PktLoss);
	}
};
