// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// DestructionProfiler.h
// 파괴 시스템 성능 프로파일링 매크로 및 통계 수집
//
// 기능:
// - DESTRUCTION_SCOPE_TIMER: 16ms 초과 시 경고 + 통계 수집
// - DESTRUCTION_PROFILE_SCOPE: Unreal Insights 연동 + 통계 수집
// - 통계 출력/리셋/CSV 내보내기

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#if !UE_BUILD_SHIPPING
#include "ProfilingDebugging/CpuProfilerTrace.h"
#endif

//=============================================================================
// 로그 카테고리
//=============================================================================
DECLARE_LOG_CATEGORY_EXTERN(LogDestructionProfiler, Log, All);

//=============================================================================
// 전역 통계 수집기 (싱글톤)
//=============================================================================
class REALTIMEDESTRUCTION_API FDestructionProfilerStats
{
public:
	/** 싱글톤 인스턴스 */
	static FDestructionProfilerStats& Get();

	/** 스코프별 통계 */
	struct FScopeStats
	{
		int32 Count = 0;
		double TotalTimeMs = 0.0;
		double AvgTimeMs = 0.0;
		double MaxTimeMs = 0.0;
		double MinTimeMs = DBL_MAX;
		int32 OverThresholdCount = 0;  // 16ms 초과 횟수
	};

	//-------------------------------------------------------------------
	// 통계 기록
	//-------------------------------------------------------------------

	/** 스코프 시간 기록 */
	void RecordScopeTime(const FString& ScopeName, double TimeMs);

	/** Boolean 연산 시간 기록 */
	void RecordBooleanOp(double TimeMs);

	/** 콜리전 업데이트 시간 기록 */
	void RecordCollisionUpdate(double TimeMs);

	/** 네트워크 처리 시간 기록 */
	void RecordNetworkOp(double TimeMs);

	//-------------------------------------------------------------------
	// 통계 조회
	//-------------------------------------------------------------------

	/** 특정 스코프 통계 가져오기 */
	FScopeStats GetScopeStats(const FString& ScopeName) const;

	/** 모든 통계 가져오기 */
	TMap<FString, FScopeStats> GetAllStats() const;

	/** 특정 스코프가 기록되었는지 */
	bool HasStats(const FString& ScopeName) const;

	//-------------------------------------------------------------------
	// 통계 관리
	//-------------------------------------------------------------------

	/** 통계 리셋 */
	void ResetStats();

	/** CSV 파일로 내보내기 */
	bool ExportToCSV(const FString& FilePath = TEXT("")) const;

	/** 콘솔에 통계 출력 */
	void PrintStats() const;

	/** 특정 스코프 통계 출력 */
	void PrintScopeStats(const FString& ScopeName) const;

	//-------------------------------------------------------------------
	// 설정
	//-------------------------------------------------------------------

	/** 경고 임계값 설정 (기본 16ms) */
	void SetWarningThreshold(double ThresholdMs) { WarningThresholdMs = ThresholdMs; }

	/** 경고 임계값 가져오기 */
	double GetWarningThreshold() const { return WarningThresholdMs; }

private:
	FDestructionProfilerStats() = default;

	mutable FCriticalSection StatsLock;
	TMap<FString, FScopeStats> ScopeStatsMap;

	// 16ms = 1프레임 (60fps) 초과 경고 임계값
	double WarningThresholdMs = 16.0;
};

//=============================================================================
// 스코프 타이머 클래스
//=============================================================================
class REALTIMEDESTRUCTION_API FDestructionScopeTimer
{
public:
	/**
	 * 생성자
	 * @param InScopeName - 스코프 이름
	 * @param bLogWarning - 16ms 초과 시 경고 로그 출력 여부
	 */
	FDestructionScopeTimer(const TCHAR* InScopeName, bool bLogWarning = true);

	/** 소멸자 - 경과 시간 측정 및 기록 */
	~FDestructionScopeTimer();

	// 복사/이동 금지
	FDestructionScopeTimer(const FDestructionScopeTimer&) = delete;
	FDestructionScopeTimer& operator=(const FDestructionScopeTimer&) = delete;

private:
	FString ScopeName;
	double StartTime;
	bool bLogWarningOnThreshold;
};

//=============================================================================
// 매크로 정의
//=============================================================================

#if !UE_BUILD_SHIPPING

/**
 * DESTRUCTION_SCOPE_TIMER(Name)
 * - 스코프 종료 시 경과 시간 측정
 * - 16ms 초과 시 경고 로그 출력
 * - 통계 자동 수집
 *
 * 사용 예:
 * void MyFunction()
 * {
 *     DESTRUCTION_SCOPE_TIMER(MyFunction);
 *     // ... 코드 ...
 * }
 */
#define DESTRUCTION_SCOPE_TIMER(Name) \
	FDestructionScopeTimer _ScopeTimer_##Name(TEXT(#Name), true)

/**
 * DESTRUCTION_SCOPE_TIMER_NO_WARNING(Name)
 * - 스코프 종료 시 경과 시간 측정
 * - 경고 로그 없이 통계만 수집
 */
#define DESTRUCTION_SCOPE_TIMER_NO_WARNING(Name) \
	FDestructionScopeTimer _ScopeTimer_##Name(TEXT(#Name), false)

/**
 * DESTRUCTION_PROFILE_SCOPE(Name)
 * - Unreal Insights 연동 (TRACE_CPUPROFILER_EVENT_SCOPE)
 * - 타이머 통계 수집 동시 수행
 *
 * Unreal Insights에서 "Destruction_Name" 으로 표시됨
 */
#define DESTRUCTION_PROFILE_SCOPE(Name) \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_##Name); \
	FDestructionScopeTimer _ProfileTimer_##Name(TEXT(#Name), true)

/**
 * DESTRUCTION_PROFILE_SCOPE_VERBOSE(Name)
 * - Insights 연동 + 진입/종료 로깅
 */
#define DESTRUCTION_PROFILE_SCOPE_VERBOSE(Name) \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_##Name); \
	FDestructionScopeTimer _ProfileTimer_##Name(TEXT(#Name), true); \
	UE_LOG(LogDestructionProfiler, Verbose, TEXT(">>> Enter: %s"), TEXT(#Name))

//=============================================================================
// 특수 목적 매크로 (Boolean 연산, 콜리전 등)
//=============================================================================

/** Boolean 연산 프로파일링 */
#define DESTRUCTION_PROFILE_BOOLEAN() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_BooleanOp); \
	FDestructionScopeTimer _BooleanTimer(TEXT("BooleanOp"), true)

/** 콜리전 업데이트 프로파일링 */
#define DESTRUCTION_PROFILE_COLLISION() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_CollisionUpdate); \
	FDestructionScopeTimer _CollisionTimer(TEXT("CollisionUpdate"), true)

/** 네트워크 처리 프로파일링 */
#define DESTRUCTION_PROFILE_NETWORK() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_NetworkOp); \
	FDestructionScopeTimer _NetworkTimer(TEXT("NetworkOp"), false)

/** 메시 생성 프로파일링 */
#define DESTRUCTION_PROFILE_MESH_BUILD() \
	TRACE_CPUPROFILER_EVENT_SCOPE(Destruction_MeshBuild); \
	FDestructionScopeTimer _MeshBuildTimer(TEXT("MeshBuild"), true)

#else // UE_BUILD_SHIPPING

// 쉬핑 빌드에서는 모든 매크로 비활성화
#define DESTRUCTION_SCOPE_TIMER(Name)
#define DESTRUCTION_SCOPE_TIMER_NO_WARNING(Name)
#define DESTRUCTION_PROFILE_SCOPE(Name)
#define DESTRUCTION_PROFILE_SCOPE_VERBOSE(Name)
#define DESTRUCTION_PROFILE_BOOLEAN()
#define DESTRUCTION_PROFILE_COLLISION()
#define DESTRUCTION_PROFILE_NETWORK()
#define DESTRUCTION_PROFILE_MESH_BUILD()

#endif // !UE_BUILD_SHIPPING
