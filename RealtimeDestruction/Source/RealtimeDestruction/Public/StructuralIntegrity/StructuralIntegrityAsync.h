// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#pragma once

#include "CoreMinimal.h"
#include "Async/AsyncWork.h"
#include "StructuralIntegrity/StructuralIntegrityTypes.h"

class FStructuralIntegritySystem;

/**
 * 비동기 Cell 파괴 Task
 *
 * Cell 수가 많을 때 (>1000) 연결성 계산을 백그라운드에서 수행
 * FNonAbandonableTask를 사용하여 Task가 완료될 때까지 보장
 */
class REALTIMEDESTRUCTION_API FStructuralIntegrityAsyncTask : public FNonAbandonableTask
{
	friend class FAsyncTask<FStructuralIntegrityAsyncTask>;

public:
	FStructuralIntegrityAsyncTask(
		FStructuralIntegritySystem* InSystem,
		const TArray<int32>& InCellIds)
		: System(InSystem)
		, CellIdsToDestroy(InCellIds)
	{
	}

	/** Task 실행 */
	void DoWork();

	/** 결과 조회 */
	const FStructuralIntegrityResult& GetResult() const { return Result; }

	/** Task 식별자 (디버깅용) */
	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FStructuralIntegrityAsyncTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	// 입력 데이터
	FStructuralIntegritySystem* System;
	TArray<int32> CellIdsToDestroy;

	// 결과 데이터
	FStructuralIntegrityResult Result;
};

/**
 * 비동기 파괴 결과 콜백 타입
 */
DECLARE_DELEGATE_OneParam(FOnStructuralDestroyCompleteDelegate, const FStructuralIntegrityResult&);

/**
 * 비동기 작업 관리자
 *
 * 여러 비동기 Cell 파괴 처리를 관리하고 완료 시 콜백 호출
 */
class REALTIMEDESTRUCTION_API FStructuralIntegrityAsyncManager
{
public:
	FStructuralIntegrityAsyncManager() = default;
	~FStructuralIntegrityAsyncManager();

	// 복사/이동 금지
	FStructuralIntegrityAsyncManager(const FStructuralIntegrityAsyncManager&) = delete;
	FStructuralIntegrityAsyncManager& operator=(const FStructuralIntegrityAsyncManager&) = delete;

	/**
	 * 비동기 Cell 파괴 처리 시작
	 * @param System - 구조적 무결성 시스템
	 * @param CellIds - 파괴할 Cell ID 목록
	 * @param OnComplete - 완료 시 콜백
	 * @return Task ID (추적용)
	 */
	int32 DestroyCellsAsync(
		FStructuralIntegritySystem* System,
		const TArray<int32>& CellIds,
		FOnStructuralDestroyCompleteDelegate OnComplete);

	/**
	 * 대기 중인 작업 완료 체크 (Tick에서 호출)
	 * 완료된 Task의 콜백을 GameThread에서 실행
	 */
	void CheckPendingTasks();

	/**
	 * 모든 작업 완료 대기 (소멸자, EndPlay 등에서)
	 */
	void WaitForAllTasks();

	/**
	 * 특정 Task 취소 (가능한 경우)
	 * FNonAbandonableTask이므로 실제로는 완료될 때까지 대기
	 * @param TaskId - 취소할 Task ID
	 */
	void CancelTask(int32 TaskId);

	/**
	 * 대기 중인 Task 수
	 */
	int32 GetPendingTaskCount() const;

	/**
	 * 모든 Task가 완료되었는지
	 */
	bool IsAllTasksComplete() const;

private:
	struct FPendingTask
	{
		int32 TaskId;
		TUniquePtr<FAsyncTask<FStructuralIntegrityAsyncTask>> AsyncTask;
		FOnStructuralDestroyCompleteDelegate Callback;
		bool bCancelled = false;
	};

	TArray<FPendingTask> PendingTasks;
	mutable FCriticalSection TaskLock;
	int32 NextTaskId = 0;
};

/**
 * 동기/비동기 처리 유틸리티
 *
 * Cell 수에 따라 자동으로 동기/비동기 선택
 */
namespace StructuralIntegrityUtils
{
	/**
	 * Cell 수에 따라 동기/비동기 자동 선택하여 파괴 처리
	 * @param System - 구조적 무결성 시스템
	 * @param AsyncManager - 비동기 매니저 (nullptr이면 항상 동기)
	 * @param CellIds - 파괴할 Cell ID 목록
	 * @param OnComplete - 완료 시 콜백 (비동기 시에만 사용)
	 * @param OutResult - 동기 처리 시 결과 (출력)
	 * @return true면 동기 처리됨, false면 비동기 처리 시작됨
	 */
	REALTIMEDESTRUCTION_API bool DestroyCellsAutomatic(
		FStructuralIntegritySystem* System,
		FStructuralIntegrityAsyncManager* AsyncManager,
		const TArray<int32>& CellIds,
		FOnStructuralDestroyCompleteDelegate OnComplete,
		FStructuralIntegrityResult& OutResult);
}
