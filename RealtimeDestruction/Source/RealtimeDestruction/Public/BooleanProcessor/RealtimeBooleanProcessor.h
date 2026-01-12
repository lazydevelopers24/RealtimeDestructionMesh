// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/DestructionTypes.h"  
#include <atomic>
#include "DynamicMesh/DynamicMesh3.h"

class UDynamicMeshComponent;
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
	UE::Geometry::FDynamicMesh3 PendingCombinedToolMesh; // Union 결과 
	TArray<TWeakObjectPtr<UDecalComponent>> Decals;
	int32 UnionCount = 0;

	// Chunk용 변수
	int32 ChunkIndex = INDEX_NONE;
	TWeakObjectPtr<UDynamicMeshComponent> TargetChunkMesh = nullptr;
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

	TWeakObjectPtr<UDynamicMeshComponent> TargetMesh = nullptr;

	int32 ChunkIndex = INDEX_NONE;

	bool CanRetry() const { return Attempts <= MaxAttempts; }

	void Reset()
	{
		ToolTransform = {};
		Attempts = 0;
		bIsPenetration = false;
		TemporaryDecal = nullptr;
		ToolMeshPtr = nullptr;
		TargetMesh = nullptr;
		ChunkIndex = INDEX_NONE;
	}
};

struct FBulletHoleBatch
{
	TArray<FTransform> ToolTransforms = {};
	TArray<uint8> Attempts = {};
	TArray<bool> bIsPenetrations = {};
	TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = {};
	TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = {};

	int32 Count = 0;
	int32 ChunkIndex = INDEX_NONE;

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

	bool Get(FBulletHole& OutOp, int32 Index)
	{
		if (Index >= Count)
		{
			return false;
		}

		OutOp.ToolTransform = MoveTemp(ToolTransforms[Index]);
		OutOp.Attempts = MoveTemp(Attempts[Index]);
		OutOp.bIsPenetration = MoveTemp(bIsPenetrations[Index]);
		OutOp.TemporaryDecal = MoveTemp(TemporaryDecals[Index]);
		OutOp.ToolMeshPtr = MoveTemp(ToolMeshPtrs[Index]);

		return true;
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

struct FChunkState
{
	int32 Interval = 0;
	int32 LastSimplifyTriCount = 0;
	int32 DurationAccumCount = 0;
	float SubtractDurationAccum = 0.0f;

	void Reset()
	{
		Interval = 0;
		DurationAccumCount = 0;
		SubtractDurationAccum = 0.0f;
	}
};

struct FChunkProcessState
{
	TArray<FChunkState> States;
	
	void Initialize(int32 ChunkNum)
	{
		States.SetNumZeroed(ChunkNum);
	}

	void Reset()
	{
		for (FChunkState& State : States)
		{
			State.Reset();
		}
	}

	void Shutdown()
	{
		States.Empty();
	}

	FChunkState& GetState(int32 ChunkIndex)
	{
		check(States.IsValidIndex(ChunkIndex));
		return States[ChunkIndex];
	}
};

class FRealtimeBooleanProcessor
{
public:
	struct FProcessorLifeTime
	{
		std::atomic<bool> bAlive{ true };
		std::atomic<FRealtimeBooleanProcessor*> Processor{ nullptr };
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

	void EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal, UDynamicMeshComponent* ChunkMesh = nullptr);
	void EnqueueRemaining(FBulletHole&& Operation);

	void KickProcessIfNeededPerChunk();

	bool IsOwnerCompValid() const { return OwnerComponent.IsValid(); }

	int32 GetCurrentHoleCount() const { return CurrentHoleCount; }

	void CancelAllOperations();

	void SetWorkInFlight(bool bEnabled) { bWorkInFlight = bEnabled; }

	int32 GetChunkHoleCount(int32 ChunkIndex) const { return ChunkHoleCount[ChunkIndex]; }
	int32 GetChunkHoleCount(const UPrimitiveComponent* ChunkComponent) const;

	static bool ApplyMeshBooleanAsync(const UE::Geometry::FDynamicMesh3* TargetMesh,
		const UE::Geometry::FDynamicMesh3* ToolMesh,
		UE::Geometry::FDynamicMesh3* OutputMesh,
		const EGeometryScriptBooleanOperation Operation,
		const FGeometryScriptMeshBooleanOptions Options,
		const FTransform& TargetTransform = FTransform::Identity,
		const FTransform& ToolTransform = FTransform::Identity);

	static void ApplySimplifyToPlanarAsync(UE::Geometry::FDynamicMesh3* TargetMesh, FGeometryScriptPlanarSimplifyOptions Options);

private:
	int32 DrainBatch(FBulletHoleBatch& InBatch);

	void StartBooleanWorkerAsyncForChunk(FBulletHoleBatch&& InBatch, int32 Gen);

	void AccumulateSubtractDuration(int32 ChunkIndex, double CurrentSubDuration);

	void UpdateSimplifyInterval(double CurrentSetMeshAvgCost);

	bool TrySimplify(UE::Geometry::FDynamicMesh3& WorkMesh, int32 ChunkIndex, int32 UnionCount);

	void EnqueueRetryOps(TQueue<FBulletHole, EQueueMode::Mpsc>& Queue, FBulletHoleBatch&& InBatch,
		UDynamicMeshComponent* TargetMesh, int32 ChunkIndex, int32& DebugCount);

	int32& GetChunkInterval(int32 ChunkIndex);

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

	// 청크 변경 이력 관리
	// 불리연 연산이 완료되고 SetMesh할 때 증가
	TArray<std::atomic<int32>> ChunkGenerations;

	// Destruction Settings
	// 프로세서를 소유한 컴포넌트로부터 받아옴
	int32 MaxHoleCount = 0;
	int32 MaxOpsPerFrame = 0;
	int32 MaxBatchSize = 0;

	TArray<int32> ChunkHoleCount = {};
	int32 CurrentHoleCount = 0;

	// defaut 값 부터 테스트
	int32 AngleThreshold = 0.001;

	FChunkProcessState ChunkStates;
	
	int32 MaxInterval = 0;
	int32 InitInterval = 0;

	double SubDurationHighThreshold = 0.0;
	double SubDurationLowThreshold = 5.0;

	double SetMeshAvgCost = 0.0;

	// Batch를 나눠서 병렬로 처리하는 방법
	int32 ParallelThreshold = 12;
	int32 MaxParallelThreads = 4;
	bool bEnableParallel = true;

	FBooleanThreadTuner AutoTuner;

private:
	void StartUnionWorkerForChunk(FBulletHoleBatch&& InBatch, int32 BatchID, int32 ChunkIndex);
	void TriggerSubtractWorkerForChunk(int32 ChunkIndex);

	/** Subtract 연산 비용 측정기 */
	void UpdateSubtractAvgCost(double CostMs);

	void UpdateUnionSize(int32 ChunkIndex, double DurationMs);

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

	/** Chunk별 UnionResults Queue (Chunk마다 독립적인 파이프라인) */
	TArray<TQueue<FUnionResult, EQueueMode::Mpsc>*> ChunkUnionResultsQueues;

	/** Chunk별 BatchID 카운터 */
	TArray<std::atomic<int32>> ChunkNextBatchIDs;

	// 청크별 toolmesh의 최대 Union 개수
	TArray<uint8> MaxUnionCount;

	double FrameBudgetMs = 8.0f;
	double SubtractAvgCostMs = 2.0f;
	double SubtractCostAccum = 0.0f;
	int32 SubtractCostSampleCount = 0;

};




