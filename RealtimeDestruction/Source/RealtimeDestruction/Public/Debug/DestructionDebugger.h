// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// DestructionDebugger.h
// Destruction system debugging and visualization tool
//
// Features:
// - Destruction location visualization (DrawDebug) + network mode-based colors
// - Statistics tracking (destructions per second, processing time, etc.)
// - Network statistics (RPC calls, validation failures, RTT)
// - Per-client request tracking
// - History recording (recent destruction requests)
// - Filtering (actor/radius)
// - Frame drop detection
// - CSV export
// - Console command support
// - On-screen HUD display

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "DestructionDebugger.generated.h"

class URealtimeDestructibleMeshComponent;
class APlayerController;

/**
 * Destruction request history entry
 */
USTRUCT(BlueprintType)
struct FDestructionHistoryEntry
{
	GENERATED_BODY()

	/** Destruction timestamp */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float Timestamp = 0.0f;

	/** Destruction location */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FVector ImpactPoint = FVector::ZeroVector;

	/** Impact normal */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FVector ImpactNormal = FVector::UpVector;

	/** Destruction radius */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float Radius = 0.0f;

	/** Instigator name */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FString InstigatorName;

	/** Target actor name */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FString TargetActorName;

	/** Network mode */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FString NetMode;

	/** Processing time (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float ProcessingTimeMs = 0.0f;

	/** Whether processed on server */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	bool bFromServer = false;

	/** Client ID (valid only on server) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	int32 ClientId = -1;
};

/**
 * Basic destruction statistics
 */
USTRUCT(BlueprintType)
struct FDestructionStats
{
	GENERATED_BODY()

	/** Total destruction count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	int32 TotalDestructions = 0;

	/** Destructions per second */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float DestructionsPerSecond = 0.0f;

	/** Average processing time (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float AverageProcessingTimeMs = 0.0f;

	/** Maximum processing time (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float MaxProcessingTimeMs = 0.0f;

	/** Average destruction radius */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float AverageRadius = 0.0f;

	/** Destructions in the last second */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	int32 DestructionsLastSecond = 0;
};

/**
 * Network statistics
 */
USTRUCT(BlueprintType)
struct FDestructionNetworkStats
{
	GENERATED_BODY()

	/** Server RPC call count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ServerRPCCount = 0;

	/** Multicast RPC call count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 MulticastRPCCount = 0;

	/** Validation failure count (rejected by server) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ValidationFailures = 0;

	/** Average RTT (ms) - valid only on client */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float AverageRTT = 0.0f;

	/** Maximum RTT (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float MaxRTT = 0.0f;

	/** Minimum RTT (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float MinRTT = 999999.0f;

	/** RTT sample count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 RTTSampleCount = 0;

	//--- Data Size Statistics ---

	/** Total bytes sent */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int64 TotalBytesSent = 0;

	/** Total bytes received */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int64 TotalBytesReceived = 0;

	/** Average bytes sent per RPC */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float AvgBytesPerRPC = 0.0f;

	/** Compact RPC count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 CompactRPCCount = 0;

	/** Uncompressed RPC count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 UncompressedRPCCount = 0;

	/** Bytes saved by compression (estimated) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int64 BytesSavedByCompression = 0;
};

/**
 * Per-client request statistics (server only)
 */
USTRUCT(BlueprintType)
struct FClientDestructionStats
{
	GENERATED_BODY()

	/** Client ID */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ClientId = -1;

	/** Player name */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	FString PlayerName;

	/** Total request count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 TotalRequests = 0;

	/** Validation failure count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ValidationFailures = 0;

	/** Requests per second */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float RequestsPerSecond = 0.0f;

	/** Last request time */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float LastRequestTime = 0.0f;
};

/**
 * Performance statistics
 */
USTRUCT(BlueprintType)
struct FDestructionPerformanceStats
{
	GENERATED_BODY()

	/** Frame drop count (during destruction processing) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 FrameDropCount = 0;

	/** Maximum frame time (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MaxFrameTimeMs = 0.0f;

	/** Maximum destructions processed per frame */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 MaxDestructionsPerFrame = 0;

	/** Current frame destruction count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 CurrentFrameDestructions = 0;

	//--- FPS Impact Statistics ---

	/** Average FPS before destruction */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float AvgFPSBeforeDestruction = 0.0f;

	/** Minimum FPS during destruction */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MinFPSDuringDestruction = 999999.0f;

	/** Average FPS drop during destruction */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float AvgFPSDrop = 0.0f;

	/** Maximum FPS drop */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MaxFPSDrop = 0.0f;

	/** FPS measurement sample count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 FPSSampleCount = 0;

	//--- Boolean Operation Statistics ---

	/** Average boolean operation time (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float AvgBooleanTimeMs = 0.0f;

	/** Maximum boolean operation time (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MaxBooleanTimeMs = 0.0f;

	/** Boolean operation sample count */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 BooleanSampleCount = 0;
};

/**
 * Destruction system debugger
 *
 * Implemented as a WorldSubsystem, automatically created per world
 */
UCLASS(ClassGroup = (RealtimeDestruction))
class REALTIMEDESTRUCTION_API UDestructionDebugger : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	//~ End USubsystem Interface

	/** Tick function (for FTSTicker, updates statistics) */
	bool OnTick(float DeltaTime);

	//-------------------------------------------------------------------
	// Debugger Control
	//-------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void SetEnabled(bool bEnable);

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	bool IsEnabled() const { return bIsEnabled; }

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void SetVisualizationEnabled(bool bEnable) { bShowVisualization = bEnable; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	bool IsVisualizationEnabled() const { return bShowVisualization; }

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void SetHUDEnabled(bool bEnable) { bShowHUD = bEnable; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	bool IsHUDEnabled() const { return bShowHUD; }

	//-------------------------------------------------------------------
	// Destruction Recording
	//-------------------------------------------------------------------

	/** Record destruction request */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void RecordDestruction(
		const FVector& ImpactPoint,
		const FVector& ImpactNormal,
		float Radius,
		AActor* Instigator,
		AActor* TargetActor,
		float ProcessingTimeMs = 0.0f);

	/** Record destruction request (with network info) */
	void RecordDestructionEx(
		const FVector& ImpactPoint,
		const FVector& ImpactNormal,
		float Radius,
		AActor* Instigator,
		AActor* TargetActor,
		float ProcessingTimeMs,
		bool bFromServer,
		int32 ClientId);

	//-------------------------------------------------------------------
	// Network Statistics Recording (called from DestructionNetworkComponent)
	//-------------------------------------------------------------------

	/** Record Server RPC call */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordServerRPC();

	/** Record Multicast RPC call */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordMulticastRPC();

	/** Record validation failure */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordValidationFailure(int32 ClientId = -1);

	/** Record RTT (called from client) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordRTT(float RTTMs);

	/** Record client request (called from server) */
	void RecordClientRequest(int32 ClientId, const FString& PlayerName, bool bValidationFailed = false);

	//-------------------------------------------------------------------
	// Network Data Size Recording
	//-------------------------------------------------------------------

	/** Record sent data size (bytes) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordBytesSent(int32 Bytes, bool bIsCompact);

	/** Record received data size (bytes) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordBytesReceived(int32 Bytes);

	/** Record Multicast RPC call (with data size) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordMulticastRPCWithSize(int32 OpCount, bool bIsCompact);

	/** Record Server RPC call (with data size) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordServerRPCWithSize(bool bIsCompact);

	//-------------------------------------------------------------------
	// Performance Statistics Recording
	//-------------------------------------------------------------------

	/** Record FPS impact (FPS difference before/after destruction) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Performance")
	void RecordFPSImpact(float FPSBefore, float FPSAfter);

	/** Record boolean operation time (pure mesh operation time) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Performance")
	void RecordBooleanOperationTime(float TimeMs);

	/** Get current FPS (internal helper) */
	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Performance")
	float GetCurrentFPS() const;

	/** Store request timestamp for RTT measurement */
	void StoreRequestTimestamp(uint32 RequestId, double Timestamp);

	/** Process response for RTT measurement (calculates RTT from RequestId) */
	void ProcessResponseForRTT(uint32 RequestId);

	//-------------------------------------------------------------------
	// Statistics Query
	//-------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	FDestructionStats GetStats() const { return Stats; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Network")
	FDestructionNetworkStats GetNetworkStats() const { return NetworkStats; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Performance")
	FDestructionPerformanceStats GetPerformanceStats() const { return PerformanceStats; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	TArray<FDestructionHistoryEntry> GetHistory() const { return History; }

	/** Get per-client statistics (server only) */
	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Network")
	TArray<FClientDestructionStats> GetClientStats() const;

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void ClearHistory();

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void ResetStats();

	/** Reset all statistics (including network and performance) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void ResetAllStats();

	//-------------------------------------------------------------------
	// Filtering
	//-------------------------------------------------------------------

	/** Set actor name filter (empty string disables filter) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Filter")
	void SetActorFilter(const FString& ActorNameFilter) { FilterActorName = ActorNameFilter; }

	/** Set minimum radius filter (0 disables filter) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Filter")
	void SetMinRadiusFilter(float MinRadius) { FilterMinRadius = MinRadius; }

	/** Clear all filters */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Filter")
	void ClearFilters() { FilterActorName.Empty(); FilterMinRadius = 0.0f; }

	//-------------------------------------------------------------------
	// Visualization
	//-------------------------------------------------------------------

	/** Draw debug visualization at a specific location */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void DrawDestructionDebug(const FVector& Location, const FVector& Normal, float Radius, float Duration = 2.0f);

	/** Draw visualization with network mode-based colors */
	void DrawDestructionDebugWithNetMode(const FVector& Location, const FVector& Normal, float Radius, bool bFromServer, float Duration = 2.0f);

	//-------------------------------------------------------------------
	// CSV Export
	//-------------------------------------------------------------------

	/** Export history to CSV file */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Export")
	bool ExportHistoryToCSV(const FString& FilePath);

	/** Export statistics to CSV file */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Export")
	bool ExportStatsToCSV(const FString& FilePath);

	//-------------------------------------------------------------------
	// Console Command Functions
	//-------------------------------------------------------------------

	void PrintStats() const;
	void PrintNetworkStats() const;
	void PrintClientStats() const;
	void PrintPerformanceStats() const;
	void PrintHistory(int32 Count = 10) const;
	void PrintSessionSummary() const;

	//-------------------------------------------------------------------
	// Batching/Sequence State (for HUD display)
	//-------------------------------------------------------------------

	/** Get pending server batch op count */
	UFUNCTION(BlueprintPure, Category = "Destruction|Debug")
	int32 GetPendingBatchOpCount() const { return PendingBatchOpCount; }

	/** Get server sequence number */
	UFUNCTION(BlueprintPure, Category = "Destruction|Debug")
	int32 GetServerSequence() const { return ServerSequence; }

	/** Get local sequence number */
	UFUNCTION(BlueprintPure, Category = "Destruction|Debug")
	int32 GetLocalSequence() const { return LocalSequence; }

	/** Set pending batch op count (called from RealtimeDestructibleMeshComponent) */
	void SetPendingBatchOpCount(int32 Count) { PendingBatchOpCount = Count; }

	/** Set server sequence number */
	void SetServerSequence(int32 Seq) { ServerSequence = Seq; }

	/** Set local sequence number */
	void SetLocalSequence(int32 Seq) { LocalSequence = Seq; }

protected:
	void UpdateHUD();
	void UpdateDestructionsPerSecond(float DeltaTime);
	void UpdatePerformanceStats(float DeltaTime);
	FString GetNetModeString() const;
	bool PassesFilter(const FString& ActorName, float Radius) const;
	FColor GetColorForNetMode(bool bFromServer) const;

protected:
	//-------------------------------------------------------------------
	// Settings
	//-------------------------------------------------------------------

	UPROPERTY()
	bool bIsEnabled = true;

	UPROPERTY()
	bool bShowVisualization = true;

	UPROPERTY()
	bool bShowHUD = false;

	UPROPERTY()
	int32 MaxHistorySize = 100;

	UPROPERTY()
	float VisualizationDuration = 3.0f;

	/** Server-processed destruction color (green) */
	UPROPERTY()
	FColor ServerColor = FColor::Green;

	/** Client-requested destruction color (orange) */
	UPROPERTY()
	FColor ClientColor = FColor::Orange;

	/** Standalone destruction color (yellow) */
	UPROPERTY()
	FColor StandaloneColor = FColor::Yellow;

	/** Normal direction color */
	UPROPERTY()
	FColor NormalColor = FColor::Blue;

	/** Filter: Actor name (contains matching) */
	FString FilterActorName;

	/** Filter: Minimum radius */
	float FilterMinRadius = 0.0f;

	/** Frame drop threshold (ms) */
	float FrameDropThresholdMs = 33.33f; // Below 30 FPS

	//-------------------------------------------------------------------
	// Data
	//-------------------------------------------------------------------

	UPROPERTY()
	FDestructionStats Stats;

	UPROPERTY()
	FDestructionNetworkStats NetworkStats;

	UPROPERTY()
	FDestructionPerformanceStats PerformanceStats;

	UPROPERTY()
	TArray<FDestructionHistoryEntry> History;

	/** Per-client statistics (server only) - Key: ClientId */
	TMap<int32, FClientDestructionStats> ClientStatsMap;

	/** Recent destruction timestamps (last 1 second) */
	TArray<float> RecentDestructionTimestamps;

	/** Per-client recent request timestamps */
	TMap<int32, TArray<float>> ClientRecentRequests;

	/** Total processing time (for average calculation) */
	double TotalProcessingTime = 0.0;

	/** Total radius (for average calculation) */
	double TotalRadius = 0.0;

	/** Total RTT (for average calculation) */
	double TotalRTT = 0.0;

	/** Total FPS drop (for average calculation) */
	double TotalFPSDrop = 0.0;

	/** Total FPS before destruction (for average calculation) */
	double TotalFPSBefore = 0.0;

	/** Total boolean operation time (for average calculation) */
	double TotalBooleanTime = 0.0;

	/** Request timestamp map for RTT measurement (RequestId -> Timestamp) */
	TMap<uint32, double> PendingRTTRequests;

	/** Recent FPS samples (for average calculation) */
	TArray<float> RecentFPSSamples;

	/** Session start time */
	float SessionStartTime = 0.0f;

	/** Last frame time */
	float LastFrameTime = 0.0f;

	/** Current frame destruction count */
	int32 CurrentFrameDestructionCount = 0;

	/** FTSTicker handle */
	FTSTicker::FDelegateHandle TickHandle;

	//-------------------------------------------------------------------
	// Batching/Sequence State (for HUD display)
	//-------------------------------------------------------------------

	/** Pending server batch op count */
	int32 PendingBatchOpCount = 0;

	/** Server sequence number */
	int32 ServerSequence = 0;

	/** Local sequence number */
	int32 LocalSequence = 0;
};
