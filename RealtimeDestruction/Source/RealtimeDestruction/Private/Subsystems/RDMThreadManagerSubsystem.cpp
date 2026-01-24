// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Subsystems/RDMThreadManagerSubsystem.h"

#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Tasks/Task.h"
#include "Async/Async.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "Settings/RDMSetting.h"
TRACE_DECLARE_INT_COUNTER(RDM_ActiveUnionWorkers, TEXT("RDMThreadManager/ActiveUnionWorkers"));
TRACE_DECLARE_INT_COUNTER(RDM_ActiveSubtractWorkers, TEXT("RDMThreadManager/ActiveSubtractWorkers"));
TRACE_DECLARE_INT_COUNTER(RDM_ActiveTotalWorkers, TEXT("RDMThreadManager/RDM_ActiveTotalWorkers"));

void URDMThreadManagerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	bIsShuttingDown.store(false);
	ActiveWorkers.store(0);
	PendingCount.store(0);
	
	// 하드웨어 기반으로 할거면 NumCores를 사용해서 휴리스틱하게 설정 
	int32 NumCores= FPlatformMisc::NumberOfCoresIncludingHyperthreads();

	// RDMSetting에서 설정한 thread만큼만 사용
	if (const URDMSetting* Settings = URDMSetting::Get())
	{
		MaxTotalWorkers = Settings->GetEffectiveThreadCount();
	}
}
 
void URDMThreadManagerSubsystem::Deinitialize()
{
	bIsShuttingDown.store(true);

	// 대기 큐 비우기
	TFunction<void()> Dummy;
	while (PendingQueue.Dequeue(Dummy)) {}
	PendingCount.store(0);

	// 활성 Worker 종료 대기 (최대 1초)
	double StartTime = FPlatformTime::Seconds();
	while (ActiveWorkers.load() > 0 && (FPlatformTime::Seconds() - StartTime) < 1.0)
	{
		FPlatformProcess::Sleep(0.01f);
	}

	Super::Deinitialize();
}

URDMThreadManagerSubsystem* URDMThreadManagerSubsystem::Get(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}

	UGameInstance* GameInstance = World->GetGameInstance();

	if (!GameInstance)
	{
		return nullptr;
	}

	return GameInstance->GetSubsystem<URDMThreadManagerSubsystem>();
}

void URDMThreadManagerSubsystem::RequestWork(TFunction<void()>&& WorkFunc, UObject* Requester)
{
	if (bIsShuttingDown.load())
	{
		return;
	}

	// Active Worker 수 체크
	int32 CurrentActive = ActiveWorkers.load();

	if (CurrentActive < MaxTotalWorkers)
	{
		// 남는 worker가 있다면 실행
		LaunchWork(MoveTemp(WorkFunc));
	}
	else
	{
		// worker가 없다면 대기 큐에 추가 
		PendingQueue.Enqueue(MoveTemp(WorkFunc));
		PendingCount.fetch_add(1); 
	}
}

void URDMThreadManagerSubsystem::LogStatus() const
{
	UE_LOG(LogTemp, Warning, TEXT("[RDMThreadManager] Active: %d / %d, Pending: %d"),
		ActiveWorkers.load(),
		MaxTotalWorkers,
		PendingCount.load());
}

void URDMThreadManagerSubsystem::LaunchWork(TFunction<void()>&& WorkFunc)
{
	ActiveWorkers.fetch_add(1); 
	
	TWeakObjectPtr<URDMThreadManagerSubsystem> WeakThis(this);

	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[WeakThis, Func = MoveTemp(WorkFunc)]() mutable
		{
			// 작업 실행
			Func();

			// 완료 처리 (GameThread에서)
			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				if (URDMThreadManagerSubsystem* Manager = WeakThis.Get())
				{
					Manager->OnWorkComplete();
				}
			});
		});
}

void URDMThreadManagerSubsystem::OnWorkComplete()
{
	ActiveWorkers.fetch_sub(1);

	if (bIsShuttingDown.load())
	{
		return;
	}

	// 대기 작업 처리
	TryDispatchPending();
}

void URDMThreadManagerSubsystem::TryDispatchPending()
{
	// worker가 있고 대기 작업이 있으면 실행
	while (ActiveWorkers.load() < MaxTotalWorkers)
	{
		TFunction<void()> WorkFunc;

		if (!PendingQueue.Dequeue(WorkFunc))
		{
			// 대기 큐 비었음
			break;
		}

		PendingCount.fetch_sub(1);
		LaunchWork(MoveTemp(WorkFunc));
	}
}
  
