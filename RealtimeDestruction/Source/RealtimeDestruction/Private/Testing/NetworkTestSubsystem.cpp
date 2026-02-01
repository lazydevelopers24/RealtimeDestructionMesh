// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// NetworkTestSubsystem.cpp

#include "Testing/NetworkTestSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogNetworkTest, Log, All);

void UNetworkTestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	InitializePresetConfigs();
	UE_LOG(LogNetworkTest, Log, TEXT("NetworkTestSubsystem: Initialized"));
}

void UNetworkTestSubsystem::Deinitialize()
{
	// 정리 시 시뮬레이션 해제
	DisableSimulation();
	Super::Deinitialize();
	UE_LOG(LogNetworkTest, Log, TEXT("NetworkTestSubsystem: Deinitialized"));
}

bool UNetworkTestSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// [주석처리] 쉬핑 빌드에서는 생성하지 않음 - 복구 시 아래 주석 해제
//#if UE_BUILD_SHIPPING
//	return false;
//#else
	return true;
//#endif
}

void UNetworkTestSubsystem::InitializePresetConfigs()
{
	// 테스트 가이드 문서 기반 프리셋 값
	PresetConfigs.Add(ENetworkTestPreset::Off,    FNetworkTestPresetConfig(0,   0,  0, TEXT("Off")));
	PresetConfigs.Add(ENetworkTestPreset::Good,   FNetworkTestPresetConfig(20,  5,  0, TEXT("Good")));
	PresetConfigs.Add(ENetworkTestPreset::Normal, FNetworkTestPresetConfig(50,  15, 1, TEXT("Normal")));
	PresetConfigs.Add(ENetworkTestPreset::Bad,    FNetworkTestPresetConfig(100, 30, 3, TEXT("Bad")));
	PresetConfigs.Add(ENetworkTestPreset::Worst,  FNetworkTestPresetConfig(200, 50, 5, TEXT("Worst")));

	// 기본값은 Off
	CurrentPreset = ENetworkTestPreset::Off;
	CurrentConfig = PresetConfigs[ENetworkTestPreset::Off];
}

void UNetworkTestSubsystem::ApplyPreset(ENetworkTestPreset Preset)
{
	if (const FNetworkTestPresetConfig* Config = PresetConfigs.Find(Preset))
	{
		CurrentPreset = Preset;
		CurrentConfig = *Config;
		ApplyNetworkCVars(CurrentConfig);

		UE_LOG(LogNetworkTest, Log, TEXT("NetworkTestSubsystem: Applied preset '%s' (Lag:%dms Var:%dms Loss:%d%%)"),
			*CurrentConfig.PresetName, CurrentConfig.PktLag, CurrentConfig.PktLagVariance, CurrentConfig.PktLoss);
	}
}

bool UNetworkTestSubsystem::ApplyPresetByName(const FString& PresetName)
{
	FString LowerName = PresetName.ToLower();

	if (LowerName == TEXT("off"))
	{
		ApplyPreset(ENetworkTestPreset::Off);
		return true;
	}
	if (LowerName == TEXT("good"))
	{
		ApplyPreset(ENetworkTestPreset::Good);
		return true;
	}
	if (LowerName == TEXT("normal"))
	{
		ApplyPreset(ENetworkTestPreset::Normal);
		return true;
	}
	if (LowerName == TEXT("bad"))
	{
		ApplyPreset(ENetworkTestPreset::Bad);
		return true;
	}
	if (LowerName == TEXT("worst"))
	{
		ApplyPreset(ENetworkTestPreset::Worst);
		return true;
	}

	UE_LOG(LogNetworkTest, Warning, TEXT("NetworkTestSubsystem: Unknown preset '%s'"), *PresetName);
	return false;
}

FString UNetworkTestSubsystem::GetCurrentPresetName() const
{
	return CurrentConfig.PresetName;
}

float UNetworkTestSubsystem::GetCurrentPing() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0f;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (PC && PC->PlayerState)
	{
		return PC->PlayerState->GetPingInMilliseconds();
	}

	return 0.0f;
}

void UNetworkTestSubsystem::ApplyCustomConfig(int32 PktLag, int32 PktLagVariance, int32 PktLoss)
{
	CurrentPreset = ENetworkTestPreset::Off; // 커스텀은 Off로 표시 (프리셋 외)
	CurrentConfig = FNetworkTestPresetConfig(PktLag, PktLagVariance, PktLoss, TEXT("Custom"));
	ApplyNetworkCVars(CurrentConfig);

	UE_LOG(LogNetworkTest, Log, TEXT("NetworkTestSubsystem: Applied custom config (Lag:%dms Var:%dms Loss:%d%%)"),
		PktLag, PktLagVariance, PktLoss);
}

void UNetworkTestSubsystem::DisableSimulation()
{
	ApplyPreset(ENetworkTestPreset::Off);
}

void UNetworkTestSubsystem::ApplyNetworkCVars(const FNetworkTestPresetConfig& Config)
{
	// UE 네트워크 시뮬레이션 CVar 설정
	// Net.PktLag: 패킷 지연 (양방향 적용, 실제 RTT는 2배)
	IConsoleVariable* PktLagVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Net.PktLag"));
	if (PktLagVar)
	{
		PktLagVar->Set(Config.PktLag, ECVF_SetByCode);
	}

	// Net.PktLoss: 패킷 손실률
	IConsoleVariable* PktLossVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Net.PktLoss"));
	if (PktLossVar)
	{
		PktLossVar->Set(Config.PktLoss, ECVF_SetByCode);
	}

	// Net.PktLagVariance: 지연 변동
	IConsoleVariable* PktLagVarianceVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Net.PktLagVariance"));
	if (PktLagVarianceVar)
	{
		PktLagVarianceVar->Set(Config.PktLagVariance, ECVF_SetByCode);
	}
}

FNetworkTestPresetConfig UNetworkTestSubsystem::GetPresetConfig(ENetworkTestPreset Preset) const
{
	if (const FNetworkTestPresetConfig* Config = PresetConfigs.Find(Preset))
	{
		return *Config;
	}
	return FNetworkTestPresetConfig();
}

void UNetworkTestSubsystem::PrintAvailablePresets() const
{
	UE_LOG(LogNetworkTest, Log, TEXT(""));
	UE_LOG(LogNetworkTest, Log, TEXT("========== Network Test Presets =========="));
	UE_LOG(LogNetworkTest, Log, TEXT("  off    - No simulation (0ms)"));
	UE_LOG(LogNetworkTest, Log, TEXT("  good   - Good connection (20ms, 5ms var)"));
	UE_LOG(LogNetworkTest, Log, TEXT("  normal - Normal connection (50ms, 15ms var, 1%% loss)"));
	UE_LOG(LogNetworkTest, Log, TEXT("  bad    - Bad connection (100ms, 30ms var, 3%% loss)"));
	UE_LOG(LogNetworkTest, Log, TEXT("  worst  - Worst connection (200ms, 50ms var, 5%% loss)"));
	UE_LOG(LogNetworkTest, Log, TEXT("=========================================="));
	UE_LOG(LogNetworkTest, Log, TEXT("Usage: Destruction.NetPreset <preset>"));
}

void UNetworkTestSubsystem::PrintCurrentStatus() const
{
	UE_LOG(LogNetworkTest, Log, TEXT(""));
	UE_LOG(LogNetworkTest, Log, TEXT("========== Network Test Status =========="));
	UE_LOG(LogNetworkTest, Log, TEXT("  Current Preset: %s"), *CurrentConfig.PresetName);
	UE_LOG(LogNetworkTest, Log, TEXT("  Packet Lag: %d ms"), CurrentConfig.PktLag);
	UE_LOG(LogNetworkTest, Log, TEXT("  Lag Variance: %d ms"), CurrentConfig.PktLagVariance);
	UE_LOG(LogNetworkTest, Log, TEXT("  Packet Loss: %d%%"), CurrentConfig.PktLoss);
	UE_LOG(LogNetworkTest, Log, TEXT("  Simulation Active: %s"), CurrentConfig.IsActive() ? TEXT("Yes") : TEXT("No"));
	UE_LOG(LogNetworkTest, Log, TEXT("  Current Ping: %.0f ms"), GetCurrentPing());
	UE_LOG(LogNetworkTest, Log, TEXT("========================================="));
}
