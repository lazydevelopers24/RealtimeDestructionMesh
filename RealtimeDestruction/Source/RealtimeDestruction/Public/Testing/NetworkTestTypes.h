// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// NetworkTestTypes.h
// Type definitions for network testing

#pragma once

#include "CoreMinimal.h"
#include "NetworkTestTypes.generated.h"

/**
 * Network Test Preset Enumeration
 *
 * Presets for simulating various network environments
 */
UENUM(BlueprintType)
enum class ENetworkTestPreset : uint8
{
	/** No simulation */
	Off		UMETA(DisplayName = "Off - No Simulation"),

	/** Good connection (20ms) - Regular users */
	Good	UMETA(DisplayName = "Good - 20ms"),

	/** Normal connection (50ms) - Majority of users */
	Normal	UMETA(DisplayName = "Normal - 50ms"),

	/** Bad connection (100ms) - WiFi */
	Bad		UMETA(DisplayName = "Bad - 100ms"),

	/** Worst environment (200ms + 5% loss) - Mobile/Overseas */
	Worst	UMETA(DisplayName = "Worst - 200ms + 5% Loss")
};

/**
 * Network Preset Configuration Struct
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FNetworkTestPresetConfig
{
	GENERATED_BODY()

	/** Packet latency (ms) - Net.PktLag */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	int32 PktLag = 0;

	/** Packet latency variance (ms) - Net.PktLagVariance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	int32 PktLagVariance = 0;

	/** Packet loss rate (%) - Net.PktLoss */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	int32 PktLoss = 0;

	/** Preset name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NetworkTest")
	FString PresetName;

	/** Default constructor */
	FNetworkTestPresetConfig()
		: PktLag(0)
		, PktLagVariance(0)
		, PktLoss(0)
		, PresetName(TEXT("Off"))
	{
	}

	/** Value initialization constructor */
	FNetworkTestPresetConfig(int32 InLag, int32 InVariance, int32 InLoss, const FString& InName)
		: PktLag(InLag)
		, PktLagVariance(InVariance)
		, PktLoss(InLoss)
		, PresetName(InName)
	{
	}

	/** Check if configuration is active (simulation running) */
	bool IsActive() const
	{
		return PktLag > 0 || PktLoss > 0;
	}

	/** Convert to string */
	FString ToString() const
	{
		return FString::Printf(TEXT("%s (Lag:%dms Var:%dms Loss:%d%%)"),
			*PresetName, PktLag, PktLagVariance, PktLoss);
	}
};
