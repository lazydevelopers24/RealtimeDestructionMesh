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
#include "Containers/Queue.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RDMThreadManagerSubsystem.generated.h"

struct FRDMWorkerRequest
{
	TFunction<void()> WorkFunc;
	int32 Priority = 0;
	TWeakObjectPtr<UObject> Requester; // For tracking the requesting component
};

UCLASS(ClassGroup = (RealtimeDestruction))
class REALTIMEDESTRUCTION_API URDMThreadManagerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Subsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override {return true;}

	// Static accessor for safe access from Worker Thread
	static URDMThreadManagerSubsystem* Get(UWorld* World);

	// Thread Request Interface
	void RequestWork(TFunction<void()>&& WorkFunc, UObject* Requester);

	// Settings
	void SetMaxTotalWorkers(int32 Max) { MaxTotalWorkers = FMath::Max(1, Max); }
	int32 GetMaxTotalWorkers() const { return MaxTotalWorkers; }
	int32 GetActiveWorkerCount() const { return ActiveWorkers.load(); }
	int32 GetPendingCount() const { return PendingCount.load(); }

	int32 GetSlotCount () const {return (MaxTotalWorkers >= 8) ? 2 : 1; }
	// Logging
	void LogStatus() const;
private:
	// Actual execution
	void LaunchWork(TFunction<void()>&& WorkFunc);

	// Completion handling
	void OnWorkComplete();

	// Execute next task from pending queue
	void TryDispatchPending();
	
private:
	// Global thread limit
	int32 MaxTotalWorkers = 4;  // Maximum 4 workers for the entire game

	// Current active worker count
	std::atomic<int32> ActiveWorkers{ 0 };

	// Pending queue
	TQueue<TFunction<void()>, EQueueMode::Mpsc> PendingQueue;
	std::atomic<int32> PendingCount{ 0 };

	// Shutdown flag
	std::atomic<bool> bIsShuttingDown{ false };

	
	
};

