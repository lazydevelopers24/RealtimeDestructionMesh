// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "StructuralIntegrity/StructuralIntegrityAsync.h"
#include "StructuralIntegrity/StructuralIntegritySystem.h"

//=========================================================================
// FStructuralIntegrityAsyncTask
//=========================================================================

void FStructuralIntegrityAsyncTask::DoWork()
{
	if (System && System->IsInitialized())
	{
		Result = System->DestroyCells(CellIdsToDestroy);
	}
}

//=========================================================================
// FStructuralIntegrityAsyncManager
//=========================================================================

FStructuralIntegrityAsyncManager::~FStructuralIntegrityAsyncManager()
{
	WaitForAllTasks();
}

int32 FStructuralIntegrityAsyncManager::DestroyCellsAsync(
	FStructuralIntegritySystem* System,
	const TArray<int32>& CellIds,
	FOnStructuralDestroyCompleteDelegate OnComplete)
{
	FScopeLock Lock(&TaskLock);

	FPendingTask NewTask;
	NewTask.TaskId = NextTaskId++;
	NewTask.AsyncTask = MakeUnique<FAsyncTask<FStructuralIntegrityAsyncTask>>(System, CellIds);
	NewTask.Callback = MoveTemp(OnComplete);

	// 백그라운드 스레드에서 실행 시작
	NewTask.AsyncTask->StartBackgroundTask();

	const int32 TaskId = NewTask.TaskId;
	PendingTasks.Add(MoveTemp(NewTask));

	return TaskId;
}

void FStructuralIntegrityAsyncManager::CheckPendingTasks()
{
	FScopeLock Lock(&TaskLock);

	// 완료된 Task 처리
	for (int32 i = PendingTasks.Num() - 1; i >= 0; --i)
	{
		FPendingTask& Task = PendingTasks[i];

		if (Task.AsyncTask->IsDone())
		{
			// 콜백 실행 (GameThread에서 실행됨)
			if (!Task.bCancelled && Task.Callback.IsBound())
			{
				const FStructuralIntegrityResult& Result = Task.AsyncTask->GetTask().GetResult();
				Task.Callback.Execute(Result);
			}

			// Task 제거
			PendingTasks.RemoveAtSwap(i);
		}
	}
}

void FStructuralIntegrityAsyncManager::WaitForAllTasks()
{
	// Lock 밖에서 대기해야 데드락 방지
	TArray<TUniquePtr<FAsyncTask<FStructuralIntegrityAsyncTask>>> TasksToWait;

	{
		FScopeLock Lock(&TaskLock);

		for (FPendingTask& Task : PendingTasks)
		{
			TasksToWait.Add(MoveTemp(Task.AsyncTask));
		}
		PendingTasks.Reset();
	}

	// 모든 Task 완료 대기
	for (auto& Task : TasksToWait)
	{
		if (Task)
		{
			Task->EnsureCompletion();
		}
	}
}

void FStructuralIntegrityAsyncManager::CancelTask(int32 TaskId)
{
	FScopeLock Lock(&TaskLock);

	for (FPendingTask& Task : PendingTasks)
	{
		if (Task.TaskId == TaskId)
		{
			Task.bCancelled = true;
			break;
		}
	}
}

int32 FStructuralIntegrityAsyncManager::GetPendingTaskCount() const
{
	FScopeLock Lock(&TaskLock);
	return PendingTasks.Num();
}

bool FStructuralIntegrityAsyncManager::IsAllTasksComplete() const
{
	FScopeLock Lock(&TaskLock);
	return PendingTasks.Num() == 0;
}

//=========================================================================
// StructuralIntegrityUtils
//=========================================================================

namespace StructuralIntegrityUtils
{
	bool DestroyCellsAutomatic(
		FStructuralIntegritySystem* System,
		FStructuralIntegrityAsyncManager* AsyncManager,
		const TArray<int32>& CellIds,
		FOnStructuralDestroyCompleteDelegate OnComplete,
		FStructuralIntegrityResult& OutResult)
	{
		if (!System || !System->IsInitialized())
		{
			return true; // 동기 처리 (빈 결과)
		}

		const FStructuralIntegritySettings& Settings = System->GetSettings();
		const int32 CellCount = System->GetCellCount();

		// 비동기 조건 확인
		const bool bShouldUseAsync =
			AsyncManager != nullptr &&
			Settings.bEnableAsync &&
			CellCount >= Settings.AsyncThreshold;

		if (bShouldUseAsync)
		{
			// 비동기 처리 시작
			AsyncManager->DestroyCellsAsync(System, CellIds, OnComplete);
			return false;
		}
		else
		{
			// 동기 처리
			OutResult = System->DestroyCells(CellIds);
			return true;
		}
	}
}
