// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// DestructionProfiler.cpp

#include "Debug/DestructionProfiler.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY(LogDestructionProfiler);

//=============================================================================
// FDestructionProfilerStats
//=============================================================================

FDestructionProfilerStats& FDestructionProfilerStats::Get()
{
	static FDestructionProfilerStats Instance;
	return Instance;
}

void FDestructionProfilerStats::RecordScopeTime(const FString& ScopeName, double TimeMs)
{
	FScopeLock Lock(&StatsLock);

	FScopeStats& Stats = ScopeStatsMap.FindOrAdd(ScopeName);
	Stats.Count++;
	Stats.TotalTimeMs += TimeMs;
	Stats.AvgTimeMs = Stats.TotalTimeMs / Stats.Count;
	Stats.MaxTimeMs = FMath::Max(Stats.MaxTimeMs, TimeMs);
	Stats.MinTimeMs = FMath::Min(Stats.MinTimeMs, TimeMs);

	if (TimeMs > WarningThresholdMs)
	{
		Stats.OverThresholdCount++;
	}
}

void FDestructionProfilerStats::RecordBooleanOp(double TimeMs)
{
	RecordScopeTime(TEXT("BooleanOp"), TimeMs);
}

void FDestructionProfilerStats::RecordCollisionUpdate(double TimeMs)
{
	RecordScopeTime(TEXT("CollisionUpdate"), TimeMs);
}

void FDestructionProfilerStats::RecordNetworkOp(double TimeMs)
{
	RecordScopeTime(TEXT("NetworkOp"), TimeMs);
}

FDestructionProfilerStats::FScopeStats FDestructionProfilerStats::GetScopeStats(const FString& ScopeName) const
{
	FScopeLock Lock(&StatsLock);
	if (const FScopeStats* Stats = ScopeStatsMap.Find(ScopeName))
	{
		return *Stats;
	}
	return FScopeStats();
}

TMap<FString, FDestructionProfilerStats::FScopeStats> FDestructionProfilerStats::GetAllStats() const
{
	FScopeLock Lock(&StatsLock);
	return ScopeStatsMap;
}

bool FDestructionProfilerStats::HasStats(const FString& ScopeName) const
{
	FScopeLock Lock(&StatsLock);
	return ScopeStatsMap.Contains(ScopeName);
}

void FDestructionProfilerStats::ResetStats()
{
	FScopeLock Lock(&StatsLock);
	ScopeStatsMap.Empty();
	UE_LOG(LogDestructionProfiler, Log, TEXT("Destruction profiler stats reset"));
}

bool FDestructionProfilerStats::ExportToCSV(const FString& FilePath) const
{
	FScopeLock Lock(&StatsLock);

	if (ScopeStatsMap.Num() == 0)
	{
		UE_LOG(LogDestructionProfiler, Warning, TEXT("No stats to export"));
		return false;
	}

	// CSV 헤더
	FString CSV = TEXT("Scope,Count,TotalMs,AvgMs,MaxMs,MinMs,OverThreshold(>16ms)\n");

	// 데이터 행
	for (const auto& Pair : ScopeStatsMap)
	{
		const FScopeStats& S = Pair.Value;
		CSV += FString::Printf(TEXT("%s,%d,%.3f,%.3f,%.3f,%.3f,%d\n"),
			*Pair.Key,
			S.Count,
			S.TotalTimeMs,
			S.AvgTimeMs,
			S.MaxTimeMs,
			S.MinTimeMs < DBL_MAX ? S.MinTimeMs : 0.0,
			S.OverThresholdCount);
	}

	// 파일 경로 결정
	FString FullPath = FilePath;
	if (FullPath.IsEmpty())
	{
		FullPath = FPaths::ProjectSavedDir() / TEXT("Profiling") / TEXT("DestructionProfiler.csv");
	}

	// 디렉토리 생성
	FString Directory = FPaths::GetPath(FullPath);
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	// 파일 저장
	if (FFileHelper::SaveStringToFile(CSV, *FullPath))
	{
		UE_LOG(LogDestructionProfiler, Log, TEXT("Exported profiler stats to: %s"), *FullPath);
		return true;
	}

	UE_LOG(LogDestructionProfiler, Warning, TEXT("Failed to export profiler stats to: %s"), *FullPath);
	return false;
}

void FDestructionProfilerStats::PrintStats() const
{
	FScopeLock Lock(&StatsLock);

	UE_LOG(LogDestructionProfiler, Log, TEXT(""));
	UE_LOG(LogDestructionProfiler, Log, TEXT("===== Destruction System Stats ====="));

	if (ScopeStatsMap.Num() == 0)
	{
		UE_LOG(LogDestructionProfiler, Log, TEXT("  No stats recorded yet"));
		UE_LOG(LogDestructionProfiler, Log, TEXT("===================================="));
		return;
	}

	UE_LOG(LogDestructionProfiler, Log, TEXT("[Timing]"));

	for (const auto& Pair : ScopeStatsMap)
	{
		const FScopeStats& S = Pair.Value;

		// 16ms 초과가 있으면 경고 색상
		if (S.OverThresholdCount > 0)
		{
			UE_LOG(LogDestructionProfiler, Warning, TEXT("  %s:"), *Pair.Key);
			UE_LOG(LogDestructionProfiler, Warning, TEXT("    Count: %d, Avg: %.2f ms, Min: %.2f ms, Max: %.2f ms"),
				S.Count, S.AvgTimeMs, S.MinTimeMs < DBL_MAX ? S.MinTimeMs : 0.0, S.MaxTimeMs);
			UE_LOG(LogDestructionProfiler, Warning, TEXT("    Over 16ms: %d times (%.1f%%)"),
				S.OverThresholdCount, (float)S.OverThresholdCount / S.Count * 100.0f);
		}
		else
		{
			UE_LOG(LogDestructionProfiler, Log, TEXT("  %s:"), *Pair.Key);
			UE_LOG(LogDestructionProfiler, Log, TEXT("    Count: %d, Avg: %.2f ms, Min: %.2f ms, Max: %.2f ms"),
				S.Count, S.AvgTimeMs, S.MinTimeMs < DBL_MAX ? S.MinTimeMs : 0.0, S.MaxTimeMs);
		}
	}

	UE_LOG(LogDestructionProfiler, Log, TEXT("===================================="));
}

void FDestructionProfilerStats::PrintScopeStats(const FString& ScopeName) const
{
	FScopeLock Lock(&StatsLock);

	if (const FScopeStats* Stats = ScopeStatsMap.Find(ScopeName))
	{
		UE_LOG(LogDestructionProfiler, Log, TEXT("%s: Count=%d Avg=%.2fms Max=%.2fms >16ms=%d"),
			*ScopeName, Stats->Count, Stats->AvgTimeMs, Stats->MaxTimeMs, Stats->OverThresholdCount);
	}
	else
	{
		UE_LOG(LogDestructionProfiler, Log, TEXT("%s: No stats recorded"), *ScopeName);
	}
}

//=============================================================================
// FDestructionScopeTimer
//=============================================================================

FDestructionScopeTimer::FDestructionScopeTimer(const TCHAR* InScopeName, bool bLogWarning)
	: ScopeName(InScopeName)
	, StartTime(FPlatformTime::Seconds())
	, bLogWarningOnThreshold(bLogWarning)
{
}

FDestructionScopeTimer::~FDestructionScopeTimer()
{
	double EndTime = FPlatformTime::Seconds();
	double ElapsedMs = (EndTime - StartTime) * 1000.0;

	// 통계 기록
	FDestructionProfilerStats::Get().RecordScopeTime(ScopeName, ElapsedMs);

	// 16ms 초과 시 경고
	if (bLogWarningOnThreshold && ElapsedMs > FDestructionProfilerStats::Get().GetWarningThreshold())
	{
		UE_LOG(LogDestructionProfiler, Warning,
			TEXT("[SLOW] %s took %.2f ms (threshold: %.0fms)"),
			*ScopeName, ElapsedMs, FDestructionProfilerStats::Get().GetWarningThreshold());
	}
}
