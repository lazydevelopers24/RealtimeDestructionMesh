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
	TWeakObjectPtr<UObject> Requester; // 요청한 컴포넌트 추적용
};

UCLASS()
class REALTIMEDESTRUCTION_API URDMThreadManagerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Subsystem 인터페이스 
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override {return true;}

	// 정적 접근자 Worker Thread에서 안전하게 접근하기 위함
	static URDMThreadManagerSubsystem* Get(UWorld* World);

	// Thread 요청 인터페이스 
	void RequestWork(TFunction<void()>&& WorkFunc, UObject* Requester);

	// 설정 
	void SetMaxTotalWorkers(int32 Max) { MaxTotalWorkers = FMath::Max(1, Max); }
	int32 GetMaxTotalWorkers() const { return MaxTotalWorkers; }
	int32 GetActiveWorkerCount() const { return ActiveWorkers.load(); }
	int32 GetPendingCount() const { return PendingCount.load(); }

	int32 GetSlotCount () const {return (MaxTotalWorkers >= 8) ? 2 : 1; }
	// 로그
	void LogStatus() const;
private:
	// 실제 실행
	void LaunchWork(TFunction<void()>&& WorkFunc);

	// 완료 처리
	void OnWorkComplete();

	// 대기 큐에서 다음 작업 실행
	void TryDispatchPending();
	
private:
	// 전역 Thread 제한
	int32 MaxTotalWorkers = 4;  // 전체 게임에서 최대 4개

	// 현재 활성 Worker 수
	std::atomic<int32> ActiveWorkers{ 0 };

	// 대기 큐
	TQueue<TFunction<void()>, EQueueMode::Mpsc> PendingQueue;
	std::atomic<int32> PendingCount{ 0 };

	// 종료 플래그
	std::atomic<bool> bIsShuttingDown{ false };

	
	
};

