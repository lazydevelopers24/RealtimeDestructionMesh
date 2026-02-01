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
 * Async Cell Destruction Task
 *
 * Performs connectivity calculation in background when cell count is high (>1000)
 * Uses FNonAbandonableTask to guarantee task completion
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

	/** Execute task */
	void DoWork();

	/** Get result */
	const FStructuralIntegrityResult& GetResult() const { return Result; }

	/** Task identifier (for debugging) */
	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FStructuralIntegrityAsyncTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	// Input data
	FStructuralIntegritySystem* System;
	TArray<int32> CellIdsToDestroy;

	// Result data
	FStructuralIntegrityResult Result;
};

/**
 * Async destruction result callback type
 */
DECLARE_DELEGATE_OneParam(FOnStructuralDestroyCompleteDelegate, const FStructuralIntegrityResult&);

/**
 * Async Task Manager
 *
 * Manages multiple async cell destruction operations and invokes callbacks on completion
 */
class REALTIMEDESTRUCTION_API FStructuralIntegrityAsyncManager
{
public:
	FStructuralIntegrityAsyncManager() = default;
	~FStructuralIntegrityAsyncManager();

	// Non-copyable and non-movable
	FStructuralIntegrityAsyncManager(const FStructuralIntegrityAsyncManager&) = delete;
	FStructuralIntegrityAsyncManager& operator=(const FStructuralIntegrityAsyncManager&) = delete;

	/**
	 * Start async cell destruction processing
	 * @param System - Structural integrity system
	 * @param CellIds - Cell ID list to destroy
	 * @param OnComplete - Callback on completion
	 * @return Task ID (for tracking)
	 */
	int32 DestroyCellsAsync(
		FStructuralIntegritySystem* System,
		const TArray<int32>& CellIds,
		FOnStructuralDestroyCompleteDelegate OnComplete);

	/**
	 * Check pending task completion (called from Tick)
	 * Executes completed task callbacks on GameThread
	 */
	void CheckPendingTasks();

	/**
	 * Wait for all tasks to complete (in destructor, EndPlay, etc.)
	 */
	void WaitForAllTasks();

	/**
	 * Cancel a specific task (if possible)
	 * Since FNonAbandonableTask, actually waits until completion
	 * @param TaskId - Task ID to cancel
	 */
	void CancelTask(int32 TaskId);

	/**
	 * Number of pending tasks
	 */
	int32 GetPendingTaskCount() const;

	/**
	 * Whether all tasks are complete
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
 * Sync/Async Processing Utilities
 *
 * Automatically selects sync/async based on cell count
 */
namespace StructuralIntegrityUtils
{
	/**
	 * Process destruction with automatic sync/async selection based on cell count
	 * @param System - Structural integrity system
	 * @param AsyncManager - Async manager (always sync if nullptr)
	 * @param CellIds - Cell ID list to destroy
	 * @param OnComplete - Callback on completion (used only for async)
	 * @param OutResult - Result for sync processing (output)
	 * @return true if processed synchronously, false if async processing started
	 */
	REALTIMEDESTRUCTION_API bool DestroyCellsAutomatic(
		FStructuralIntegritySystem* System,
		FStructuralIntegrityAsyncManager* AsyncManager,
		const TArray<int32>& CellIds,
		FOnStructuralDestroyCompleteDelegate OnComplete,
		FStructuralIntegrityResult& OutResult);
}
