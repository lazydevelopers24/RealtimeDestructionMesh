// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// NetworkTestSubsystem.h
// Network Simulation Management Subsystem
//
// Features:
// - Preset-based network latency/loss simulation
// - Console command: Destruction.NetPreset [off|good|normal|bad|worst]
// - Current ping query
// - Reusable in other projects like Lyra

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NetworkTestTypes.h"
#include "NetworkTestSubsystem.generated.h"

/**
 * Network Test Subsystem
 *
 * Manages preset-based network simulation
 * - off: Disable simulation
 * - good: 20ms latency
 * - normal: 50ms latency
 * - bad: 100ms latency
 * - worst: 200ms latency + 5% packet loss
 *
 * Usage:
 * - Console: Destruction.NetPreset bad
 * - Blueprint: GetSubsystem<UNetworkTestSubsystem>()->ApplyPreset(ENetworkTestPreset::Bad)
 */
UCLASS(ClassGroup = (RealtimeDestruction))
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
	// Preset Management
	//-------------------------------------------------------------------

	/**
	 * Apply network preset
	 * @param Preset - Preset to apply
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	void ApplyPreset(ENetworkTestPreset Preset);

	/**
	 * Apply preset by name (for console commands)
	 * @param PresetName - "good", "normal", "bad", "worst", "off"
	 * @return Whether successful
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	bool ApplyPresetByName(const FString& PresetName);

	/**
	 * Get current preset
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	ENetworkTestPreset GetCurrentPreset() const { return CurrentPreset; }

	/**
	 * Get current preset name
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	FString GetCurrentPresetName() const;

	/**
	 * Get current preset configuration
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	FNetworkTestPresetConfig GetCurrentConfig() const { return CurrentConfig; }

	/**
	 * Get current ping (from PlayerState)
	 * @return Ping (ms), 0 if unavailable
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	float GetCurrentPing() const;

	/**
	 * Check if simulation is active
	 */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	bool IsSimulationActive() const { return CurrentConfig.IsActive(); }

	//-------------------------------------------------------------------
	// Custom Settings
	//-------------------------------------------------------------------

	/**
	 * Apply custom network configuration
	 * @param PktLag - Packet latency (ms)
	 * @param PktLagVariance - Latency variance (ms)
	 * @param PktLoss - Packet loss rate (%)
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	void ApplyCustomConfig(int32 PktLag, int32 PktLagVariance, int32 PktLoss);

	/**
	 * Disable network simulation
	 */
	UFUNCTION(BlueprintCallable, Category = "Destruction|NetworkTest")
	void DisableSimulation();

	//-------------------------------------------------------------------
	// Utilities
	//-------------------------------------------------------------------

	/** Print available presets (for console) */
	void PrintAvailablePresets() const;

	/** Print current status (for console) */
	void PrintCurrentStatus() const;

	/** Get configuration for a preset */
	UFUNCTION(BlueprintPure, Category = "Destruction|NetworkTest")
	FNetworkTestPresetConfig GetPresetConfig(ENetworkTestPreset Preset) const;

protected:
	/** Initialize default configurations for each preset */
	void InitializePresetConfigs();

	/** Apply UE network CVars */
	void ApplyNetworkCVars(const FNetworkTestPresetConfig& Config);

protected:
	/** Current preset */
	UPROPERTY()
	ENetworkTestPreset CurrentPreset = ENetworkTestPreset::Off;

	/** Current configuration */
	UPROPERTY()
	FNetworkTestPresetConfig CurrentConfig;

	/** Configuration map for each preset */
	TMap<ENetworkTestPreset, FNetworkTestPresetConfig> PresetConfigs;
};
