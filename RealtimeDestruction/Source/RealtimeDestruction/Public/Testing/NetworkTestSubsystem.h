// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// NetworkTestSubsystem.h
// 네트워크 시뮬레이션 관리 서브시스템
//
// 기능:
// - 프리셋 기반 네트워크 지연/손실 시뮬레이션
// - 콘솔 명령어: Destruction.NetPreset [off|good|normal|bad|worst]
// - 현재 Ping 조회
// - 라일라 등 다른 프로젝트에서 재사용 가능

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NetworkTestTypes.h"
#include "NetworkTestSubsystem.generated.h"

/**
 * 네트워크 테스트 서브시스템
 *
 * 프리셋 기반 네트워크 시뮬레이션 관리
 * - off: 시뮬레이션 해제
 * - good: 20ms 지연
 * - normal: 50ms 지연
 * - bad: 100ms 지연
 * - worst: 200ms 지연 + 5% 패킷 손실
 *
 * 사용법:
 * - 콘솔: Destruction.NetPreset bad
 * - 블루프린트: GetSubsystem<UNetworkTestSubsystem>()->ApplyPreset(ENetworkTestPreset::Bad)
 */
UCLASS()
class REALTIMEDESTRUCTION_API UNetworkTestSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	//~ End USubsystem Interface

	//-------------------------------------------------------------------
	// 프리셋 관리
	//-------------------------------------------------------------------

	/**
	 * 네트워크 프리셋 적용
	 * @param Preset - 적용할 프리셋
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	void ApplyPreset(ENetworkTestPreset Preset);

	/**
	 * 프리셋 이름으로 적용 (콘솔 명령어용)
	 * @param PresetName - "good", "normal", "bad", "worst", "off"
	 * @return 성공 여부
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	bool ApplyPresetByName(const FString& PresetName);

	/**
	 * 현재 프리셋 가져오기
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	ENetworkTestPreset GetCurrentPreset() const { return CurrentPreset; }

	/**
	 * 현재 프리셋 이름 가져오기
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	FString GetCurrentPresetName() const;

	/**
	 * 현재 프리셋 설정값 가져오기
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	FNetworkTestPresetConfig GetCurrentConfig() const { return CurrentConfig; }

	/**
	 * 현재 Ping 가져오기 (PlayerState에서)
	 * @return Ping (ms), 없으면 0
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	float GetCurrentPing() const;

	/**
	 * 시뮬레이션이 활성화되어 있는지
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	bool IsSimulationActive() const { return CurrentConfig.IsActive(); }

	//-------------------------------------------------------------------
	// 커스텀 설정
	//-------------------------------------------------------------------

	/**
	 * 커스텀 네트워크 설정 적용
	 * @param PktLag - 패킷 지연 (ms)
	 * @param PktLagVariance - 지연 변동 (ms)
	 * @param PktLoss - 패킷 손실률 (%)
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	void ApplyCustomConfig(int32 PktLag, int32 PktLagVariance, int32 PktLoss);

	/**
	 * 네트워크 시뮬레이션 해제
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	void DisableSimulation();

	//-------------------------------------------------------------------
	// 유틸리티
	//-------------------------------------------------------------------

	/** 프리셋 목록 출력 (콘솔용) */
	void PrintAvailablePresets() const;

	/** 현재 상태 출력 (콘솔용) */
	void PrintCurrentStatus() const;

	/** 프리셋에 대한 설정값 가져오기 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	FNetworkTestPresetConfig GetPresetConfig(ENetworkTestPreset Preset) const;

protected:
	/** 프리셋별 기본 설정 초기화 */
	void InitializePresetConfigs();

	/** UE 네트워크 CVar 설정 */
	void ApplyNetworkCVars(const FNetworkTestPresetConfig& Config);

protected:
	/** 현재 프리셋 */
	UPROPERTY()
	ENetworkTestPreset CurrentPreset = ENetworkTestPreset::Off;

	/** 현재 설정값 */
	UPROPERTY()
	FNetworkTestPresetConfig CurrentConfig;

	/** 프리셋별 설정 맵 */
	TMap<ENetworkTestPreset, FNetworkTestPresetConfig> PresetConfigs;
};
