// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// DestructionProfiler.h
// Destruction system performance profiling macros and statistics collection
//
// Features:
// - DESTRUCTION_SCOPE_TIMER: Warning on >16ms + statistics collection
// - DESTRUCTION_PROFILE_SCOPE: Unreal Insights integration + statistics collection
// - Statistics output/reset/CSV export

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#if !UE_BUILD_SHIPPING
#include "ProfilingDebugging/CpuProfilerTrace.h"
#endif

//=============================================================================
// Log Category
//=============================================================================
DECLARE_LOG_CATEGORY_EXTERN(LogDestructionProfiler, Log, All);

//=============================================================================
// Global Statistics Collector (Singleton)
//=============================================================================
class REALTIMEDESTRUCTION_API FDestructionProfilerStats
{
public:
	/** Singleton instance */
	static FDestructionProfilerStats& Get();

	/** Per-scope statistics */
	struct FScopeStats
	{
		int32 Count = 0;
		double TotalTimeMs = 0.0;
		double AvgTimeMs = 0.0;
		double MaxTimeMs = 0.0;
		double MinTimeMs = DBL_MAX;
		int32 OverThresholdCount = 0;  // Count of >16ms occurrences
	};

	//-------------------------------------------------------------------
	// Statistics Recording
	//-------------------------------------------------------------------

	/** Record scope time */
	void RecordScopeTime(const FString& ScopeName, double TimeMs);

	/** Record boolean operation time */
	void RecordBooleanOp(double TimeMs);

	/** Record collision update time */
	void RecordCollisionUpdate(double TimeMs);

	/** Record network operation time */
	void RecordNetworkOp(double TimeMs);

	//-------------------------------------------------------------------
	// Statistics Query
	//-------------------------------------------------------------------

	/** Get statistics for a specific scope */
	FScopeStats GetScopeStats(const FString& ScopeName) const;

	/** Get all statistics */
	TMap<FString, FScopeStats> GetAllStats() const;

	/** Check if a specific scope has been recorded */
	bool HasStats(const FString& ScopeName) const;

	//-------------------------------------------------------------------
	// Statistics Management
	//-------------------------------------------------------------------

	/** Reset statistics */
	void ResetStats();

	/** Export to CSV file */
	bool ExportToCSV(const FString& FilePath = TEXT("")) const;

	/** Print statistics to console */
	void PrintStats() const;

	/** Print statistics for a specific scope */
	void PrintScopeStats(const FString& ScopeName) const;

	//-------------------------------------------------------------------
	// Settings
	//-------------------------------------------------------------------

	/** Set warning threshold (default 16ms) */
	void SetWarningThreshold(double ThresholdMs) { WarningThresholdMs = ThresholdMs; }

	/** Get warning threshold */
	double GetWarningThreshold() const { return WarningThresholdMs; }

private:
	FDestructionProfilerStats() = default;

	mutable FCriticalSection StatsLock;
	TMap<FString, FScopeStats> ScopeStatsMap;

	// Warning threshold for exceeding 1 frame (60fps) = 16ms
	double WarningThresholdMs = 16.0;
};

//=============================================================================
// Scope Timer Class
//=============================================================================
class REALTIMEDESTRUCTION_API FDestructionScopeTimer
{
public:
	/**
	 * Constructor
	 * @param InScopeName - Scope name
	 * @param bLogWarning - Whether to log warning when exceeding 16ms
	 */
	FDestructionScopeTimer(const TCHAR* InScopeName, bool bLogWarning = true);

	/** Destructor - Measures elapsed time and records statistics */
	~FDestructionScopeTimer();

	// Non-copyable and non-movable
	FDestructionScopeTimer(const FDestructionScopeTimer&) = delete;
	FDestructionScopeTimer& operator=(const FDestructionScopeTimer&) = delete;

private:
	FString ScopeName;
	double StartTime;
	bool bLogWarningOnThreshold;
};

//=============================================================================
// Macro Definitions
//=============================================================================

#if !UE_BUILD_SHIPPING

/**
 * DESTRUCTION_SCOPE_TIMER(Name)
 * - Measures elapsed time at scope exit
 * - Logs warning when exceeding 16ms
 * - Automatic statistics collection
 *
 * Usage:
 * void MyFunction()
 * {
 *     DESTRUCTION_SCOPE_TIMER(MyFunction);
 *     // ... code ...
 * }
 */
#define DESTRUCTION_SCOPE_TIMER(Name) \
	FDestructionScopeTimer _ScopeTimer_##Name(TEXT(#Name), true)

/**
 * DESTRUCTION_SCOPE_TIMER_NO_WARNING(Name)
 * - Measures elapsed time at scope exit
 * - Collects statistics only without warning log
 */
#define DESTRUCTION_SCOPE_TIMER_NO_WARNING(Name) \
	FDestructionScopeTimer _ScopeTimer_##Name(TEXT(#Name), false)

/**
 * DESTRUCTION_PROFILE_SCOPE(Name)
 * - Unreal Insights integration (TRACE_CPUPROFILER_EVENT_SCOPE)
 * - Simultaneous timer statistics collection
 *
 * Displayed as "Destruction_Name" in Unreal Insights
 */
#define DESTRUCTION_PROFILE_SCOPE(Name) \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_##Name); \
	FDestructionScopeTimer _ProfileTimer_##Name(TEXT(#Name), true)

/**
 * DESTRUCTION_PROFILE_SCOPE_VERBOSE(Name)
 * - Insights integration + entry/exit logging
 */
#define DESTRUCTION_PROFILE_SCOPE_VERBOSE(Name) \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_##Name); \
	FDestructionScopeTimer _ProfileTimer_##Name(TEXT(#Name), true); \
	UE_LOG(LogDestructionProfiler, Verbose, TEXT(">>> Enter: %s"), TEXT(#Name))

//=============================================================================
// Special Purpose Macros (Boolean operations, Collision, etc.)
//=============================================================================

/** Boolean operation profiling */
#define DESTRUCTION_PROFILE_BOOLEAN() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_BooleanOp); \
	FDestructionScopeTimer _BooleanTimer(TEXT("BooleanOp"), true)

/** Collision update profiling */
#define DESTRUCTION_PROFILE_COLLISION() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_CollisionUpdate); \
	FDestructionScopeTimer _CollisionTimer(TEXT("CollisionUpdate"), true)

/** Network operation profiling */
#define DESTRUCTION_PROFILE_NETWORK() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_NetworkOp); \
	FDestructionScopeTimer _NetworkTimer(TEXT("NetworkOp"), false)

/** Mesh build profiling */
#define DESTRUCTION_PROFILE_MESH_BUILD() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_MeshBuild); \
	FDestructionScopeTimer _MeshBuildTimer(TEXT("MeshBuild"), true)

#else // UE_BUILD_SHIPPING

// All macros disabled in shipping builds
#define DESTRUCTION_SCOPE_TIMER(Name)
#define DESTRUCTION_SCOPE_TIMER_NO_WARNING(Name)
#define DESTRUCTION_PROFILE_SCOPE(Name)
#define DESTRUCTION_PROFILE_SCOPE_VERBOSE(Name)
#define DESTRUCTION_PROFILE_BOOLEAN()
#define DESTRUCTION_PROFILE_COLLISION()
#define DESTRUCTION_PROFILE_NETWORK()
#define DESTRUCTION_PROFILE_MESH_BUILD()

#endif // !UE_BUILD_SHIPPING
