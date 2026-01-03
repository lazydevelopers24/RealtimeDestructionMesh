// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/DestructionTypes.h"  
#include <atomic>
#include "DynamicMesh/DynamicMesh3.h"

////////////////////////////////////////
/******** forward declaration ********/
namespace UE::Geometry
{
	class FDynamicMesh3;
}
struct FRealtimeDestructionOp;
struct FGeometryScriptMeshBooleanOptions;
struct FGeometryScriptPlanarSimplifyOptions;
enum class EGeometryScriptBooleanOperation : uint8;
class URealtimeDestructibleMeshComponent;
class UDecalComponent;
////////////////////////////////////////

/**
 * Union 결과 저장 구조체 .
 */

struct FUnionResult
{
	int32 BatchID = 0;				// 순서 추적용
	int32 Generation = 0;
	UE::Geometry::FDynamicMesh3 PendingCombinedToolMesh; // Union 결과 
	TArray<TWeakObjectPtr<UDecalComponent>> Decals;
	int32 UnionCount = 0;
};


// SOA로 바꿀 방법 고민해보기
struct FBulletHole
{
	FTransform ToolTransform = {};
	uint8 Attempts = 0;
	static constexpr uint8 MaxAttempts = 2;

	// 관통 여부 플래그
	// true: 관통, false: 비관통 
	bool bIsPenetration = false;

	TWeakObjectPtr<UDecalComponent> TemporaryDecal = nullptr;

	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> ToolMeshPtr = nullptr;

	bool CanRetry() const { return Attempts <= MaxAttempts; }
};

struct FBulletHoleBatch
{
	TArray<FTransform> ToolTransforms = {};
	TArray<uint8> Attempts = {};
	TArray<bool> bIsPenetrations = {};
	TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = {};
	TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = {};

	int32 Count = 0;

	FBulletHoleBatch() = default;
	~FBulletHoleBatch() = default;	

	void Reserve(int32 Capacity)
	{
		ToolTransforms.Reserve(Capacity);
		Attempts.Reserve(Capacity);
		bIsPenetrations.Reserve(Capacity);
		TemporaryDecals.Reserve(Capacity);
		ToolMeshPtrs.Reserve(Capacity);		
	}

	void Reset()
	{
		Count = 0;
		ToolTransforms.Reset();
		Attempts.Reset();
		bIsPenetrations.Reset();
		TemporaryDecals.Reset();
		ToolMeshPtrs.Reset();
	}

	void Add(FBulletHole& Op)
	{
		ToolTransforms.Add(Op.ToolTransform);
		Attempts.Add(Op.Attempts);
		bIsPenetrations.Add(Op.bIsPenetration);
		TemporaryDecals.Add(Op.TemporaryDecal);
		ToolMeshPtrs.Add(Op.ToolMeshPtr);
		Count++;
	}

	void Add(FBulletHole&& Op)
	{
		ToolTransforms.Add(MoveTemp(Op.ToolTransform));
		Attempts.Add(MoveTemp(Op.Attempts));
		bIsPenetrations.Add(MoveTemp(Op.bIsPenetration));
		TemporaryDecals.Add(MoveTemp(Op.TemporaryDecal));
		ToolMeshPtrs.Add(MoveTemp(Op.ToolMeshPtr));
		Count++;
	}

	/*
	 * ToolTransforms.Num을 반환하는 걸로 하면 이동연산 고려해야함
	 * ToolTransform을 이동연산으로 소유권을 넘기면 ToolTransforms.Num는 0이 됨
	 */
	int32 Num() const { return Count; }
};

struct FBooleanThreadTuner
{
	// 시작 스레드 수
	int32 CurrentThreadCount = 2;
	// 이전 단계 효율
	double LastThroughput = 0.0;
	// 탐색 방향 (1 : 증가, -1 : 감소)
	int32 ExplorationDirection = 1;

	// 60 FPS 목표
	const double TargetFrameTime = 1.0 / 60.0;

	void Update(int32 BatchSize, double ElapsedTime, float CurrentDeltaTime);
	int32 GetRecommendedThreadCount() const;
};

class FRealtimeBooleanProcessor
{
public:
	struct FProcessorLifeTime
	{
		std::atomic<bool> bAlive{true};
		std::atomic<FRealtimeBooleanProcessor*> Processor{nullptr};
		void Clear()
		{			
			bAlive = false;
			Processor = nullptr;
		}
	};
	
public:
	FRealtimeBooleanProcessor() = default;
	~FRealtimeBooleanProcessor();

	bool Initialize(URealtimeDestructibleMeshComponent* Owner);
	void Shutdown();

	void EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal);
	void EnqueueRemaining(FBulletHole&& Operation);

	// 큐가 쌓이면 불리언 연산 시작
	void KickProcessIfNeeded();

	bool IsOwnerCompValid() const { return OwnerComponent.IsValid(); }

	int32 GetCurrentHoleCount() const { return CurrentHoleCount; }

	void CancelAllOperations();

	void SetWorkInFlight(bool bEnabled) { bWorkInFlight = bEnabled; }

	bool IsStale(int32 Gen) const { return Gen != BooleanGeneration.load(); }

	bool IsHoleMax() const { return CurrentHoleCount >= MaxHoleCount; }	

	static bool ApplyMeshBooleanAsync(const UE::Geometry::FDynamicMesh3* TargetMesh,
	                                  const UE::Geometry::FDynamicMesh3* ToolMesh,
	                                  UE::Geometry::FDynamicMesh3* OutputMesh,
	                                  const EGeometryScriptBooleanOperation Operation,
	                                  const FGeometryScriptMeshBooleanOptions Options,
	                                  const FTransform& TargetTransform = FTransform::Identity,
	                                  const FTransform& ToolTransform = FTransform::Identity);
	
	static void ApplySimplifyToPlanarAsync(UE::Geometry::FDynamicMesh3* TargetMesh, FGeometryScriptPlanarSimplifyOptions Options);
	
	static UE::Geometry::FDynamicMesh3 HierarchicalUnion(TArray<UE::Geometry::FDynamicMesh3>& Results, const FGeometryScriptMeshBooleanOptions& Options);

private:
	int32 DrainBatch(FBulletHoleBatch& InBatch);
	void StartBooleanWorkerAsync(FBulletHoleBatch&& InBatch, int32 Gen);

	void StartBooleanWorkerParallel(FBulletHoleBatch&& InBatch, int32 Gen);

	void AccumulateSubtractDuration(double CurrentSubDuration);

	void UpdateSimplifyInterval(double CurrentSetMeshAvgCost);

	bool TrySimplify(UE::Geometry::FDynamicMesh3& WorkMesh, int32 UnionCount);

private:
	TWeakObjectPtr<URealtimeDestructibleMeshComponent> OwnerComponent = nullptr;

	// Queue를 관통, 비관통 전용으로 나눠서 관리
	TQueue<FBulletHole, EQueueMode::Mpsc> HighPriorityQueue;
	int DebugHighQueueCount;

	TQueue<FBulletHole, EQueueMode::Mpsc> NormalPriorityQueue;
	int DebugNormalQueueCount;

	TSharedPtr<FProcessorLifeTime, ESPMode::ThreadSafe> LifeTime;

	// 비동기 작업 실행 여부 검사
	bool bWorkInFlight = false;

	/*
	 * BooleanGeneration이 필요한 상황
	 * 1. 불리언 연산 시작(KickProcessIfNeeded호출, BooleanGeneration == 10)
	 * 2. 워커 스레드 작업 시작
	 * 3. 게임 로직 등에 의해서 라운드 재시작 or 메시 리셋 등 발생
	 * 4. 워커 스레드 작업 완료 및 게임 스레드에서 불리언 연산 결과 반영 로직 실행
	 * 위 경우에서 메시가 새로 갱신되었는데 이전의 불리언 연산값을 반영하는 결과가 발생할 수 있음
	 * 3번 단계에서 BooleanGeneration을 증가시키고 GT에서 Stale 검사를 하면 방어 가능
	 */
	std::atomic<int32>BooleanGeneration = 0;

	// Destruction Settings
	// 프로세서를 소유한 컴포넌트로부터 받아옴
	int32 MaxHoleCount = 0;
	int32 MaxOpsPerFrame = 0;
	int32 MaxBatchSize = 0;
	
	int32 CurrentHoleCount = 0;

	/*
	 * Simplify 변수
	 */
	int32 LastSimplifyTriCount = 0;
	// defaut 값 부터 테스트
	int32 AngleThreshold = 0.001;
	
	int32 CurrentInterval = 0;
	int32 MaxInterval = 0;
	int32 InitInterval = 0;

	double SubtractDurationAccum = 0.0;
	double SubDurationHighThreshold = 0.0;
	double SubDurationLowThreshold = 5.0;
	int32 DurationAccumCount = 0;

	double SetMeshAvgCost = 0.0;

		// Debug
	int32 OpAccum = 0;
	int32 DurationCount = 0;
	int32 GrowthCount = 0;
	void SimplifyLog();

	// Batch를 나눠서 병렬로 처리하는 방법
	int32 ParallelThreshold = 12;
	int32 MaxParallelThreads = 4;
	bool bEnableParallel = true;

	FBooleanThreadTuner AutoTuner;

	//-------------------------------------------------------------------
	// GT 복사 블로킹 최적화를 위한 캐시된 메시
	// - GT에서는 포인터만 전달, 실제 복사는 워커에서 수행
	// - 워커 완료 후 결과를 캐시에 저장하여 다음 작업에 재사용
	//-------------------------------------------------------------------
	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> CachedMeshPtr;

	// 캐시가 유효한지 (워커 결과가 반영되었는지)
	bool bCacheValid = false;

	// GT 복사 최적화 사용 여부 (런타임 토글 가능)
	// true: 캐시 기반 최적화 (워커에서 복사)
	// false: 기존 방식 (GT에서 복사)
	bool bUseCachedMeshOptimization = true;

	// 캐시 업데이트 (GT에서 호출)
	void UpdateMeshCache(UE::Geometry::FDynamicMesh3&& ResultMesh);

	// 캐시에서 메시 가져오기 (워커에서 복사할 소스)
	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> GetCachedMeshForWorker();

public:
	// 최적화 플래그 Getter/Setter
	void SetCachedMeshOptimization(bool bEnable) { bUseCachedMeshOptimization = bEnable; }
	bool IsCachedMeshOptimizationEnabled() const { return bUseCachedMeshOptimization; }

private:

	void StartUnionWorker(FBulletHoleBatch&& InBatch, int32 BatchID, int32 Gen);
	void TriggerSubtractWorker();

	bool bEnableMultiWorkers;
	/** 최대 Worker 수 */
	int8 MaxWorkerCount = 8;

	/** 여러개의 Worker를 사용하기 위한 테스트용 변수들 */
	TQueue<FUnionResult, EQueueMode::Mpsc> UnionResultsQueue;
	
	/** 현재 Union 작업 중인 Worker 수 */
	std::atomic<int32> ActiveUnionWorkers{ 0 };

	/** Subtract 작업 중 플래그 (Subtract는 한 곳에서만 실행 ) */
	std::atomic<bool> bSubtractInProgress{ false };

	/** 배치 ID (순서 추적용) */
	std::atomic<int32> NextBatchID{ 0 };

	/** Subtract 대기 중 배치 ID (순서 보장용) */
	std::atomic<int32> NextSubtractBatchID{ 0 }; 

};




