#include "RealtimeBooleanProcessor.h"

#include "AudioMixerBlueprintLibrary.h"
#include "MeshBoundaryLoops.h"
#include "MeshSimplification.h"
#include "Tasks/Task.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DynamicMesh/MeshTransforms.h"
#include "Operations/MeshBoolean.h"
#include "Operations/MinimalHoleFiller.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Components/DecalComponent.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "ProfilingDebugging/CountersTrace.h"

TRACE_DECLARE_INT_COUNTER(Counter_ThreadCount, TEXT("RealtimeDestruction/ThreadCount"));
TRACE_DECLARE_FLOAT_COUNTER(Counter_Throughput, TEXT("RealtimeDestruction/Throughput"));
TRACE_DECLARE_INT_COUNTER(Counter_BatchSize, TEXT("RealtimeDestruction/BatchSize"));
TRACE_DECLARE_FLOAT_COUNTER(Counter_WorkTime, TEXT("RealtimeDestruction/WorkTimeMs"));

using namespace UE::Geometry;

void FBooleanThreadTuner::Update(int32 BatchSize, double ElapsedTime, float CurrentDeltaTime)
{
	if (ElapsedTime <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	double CurrentThroughput = (double)BatchSize / ElapsedTime;

	/*
	 * 60 FPS 방어
	 * 현재 프레임이 60 FPS를 넘지 못하면 워커 스레드를 줄여서 스레드 경합을 줄인다.
	 */
	if (CurrentDeltaTime > TargetFrameTime * 1.1f)	// 10% 여유
	{
		CurrentThreadCount = FMath::Max(1, CurrentThreadCount - 1);
		// 스레드를 줄여야 하기 때문에 감소 방향으로 바꿈
		ExplorationDirection = -1;
		LastThroughput = CurrentThroughput;
		return;
	}

	/*
	 * Hill Climbing
	 * 효율(Throughput) 증가 시 방향 유지
	 */
	if (CurrentThroughput > LastThroughput)
	{
		CurrentThreadCount += ExplorationDirection;
	}
	/*
	 * 효율 감소 상황으로 메모리 병목 혹은 오버헤드 증가
	 */
	else
	{

		// 감소 방향으로 바꾸는 것이 아님
		// 현재 방향에서 효율이 감소 했으니 반대 방향으로 바꾸기 위해서 -1을 곱한다.
		ExplorationDirection *= -1;
		CurrentThreadCount += ExplorationDirection;
	}

	LastThroughput = CurrentThroughput;
	int32 HardwareLimit = FPlatformMisc::NumberOfWorkerThreadsToSpawn();
	CurrentThreadCount = FMath::Clamp(CurrentThreadCount, 1, HardwareLimit);
}

int32 FBooleanThreadTuner::GetRecommendedThreadCount() const
{
	return CurrentThreadCount;
}

FRealtimeBooleanProcessor::~FRealtimeBooleanProcessor()
{
	OwnerComponent.Reset();
	if (LifeTime.IsValid())
	{
		LifeTime->Clear();
	}
}

bool FRealtimeBooleanProcessor::Initialize(URealtimeDestructibleMeshComponent* Owner)
{
	if (!Owner)
	{
		return false;
	}

	OwnerComponent = Owner;
	OwnerComponent->GetDestructionSettings(MaxHoleCount, MaxOpsPerFrame, MaxBatchSize);
	OwnerComponent->GetParallelSettings(ParallelThreshold, MaxParallelThreads);

	OwnerComponent->SettingAsyncOption(bEnableParallel, bEnableMultiWorkers);

	int32 ChunkNum = OwnerComponent->GetChunkNum();
	if (ChunkNum > 0)
	{
		BooleanGenerations.SetNumZeroed(ChunkNum);

		// Chunk용 MulyiWorker 변수 초기화
		ChunkUnionResultsQueues.SetNum(ChunkNum);
		for (int32 i = 0; i < ChunkNum; ++i)
		{
			ChunkUnionResultsQueues[i] = new TQueue<FUnionResult, EQueueMode::Mpsc>();
		}

		ChunkNextBatchIDs.SetNumZeroed(ChunkNum);
		ChunkNextSubtractBatchIDs.SetNumZeroed(ChunkNum);
	}

	LifeTime = MakeShared<FProcessorLifeTime, ESPMode::ThreadSafe>();
	LifeTime->bAlive.store(true);
	LifeTime->Processor.store(this);

	/*
	 * Simplify test
	 */
	LastSimplifyTriCount = OwnerComponent->GetDynamicMesh()->GetTriangleCount();
	AngleThreshold = OwnerComponent->GetAngleThreshold();
	SubDurationHighThreshold = OwnerComponent->GetSubtractDurationLimit();
	InitInterval = OwnerComponent->GetMaxOpCount();
	MaxInterval = InitInterval;

	return true;
}

void FRealtimeBooleanProcessor::Shutdown()
{
	if (!LifeTime.IsValid())
	{
		return;
	}

	if (OwnerComponent.IsValid())
	{
		OwnerComponent.Reset();
	}

	LifeTime->Clear();
	LifeTime.Reset();
	++BooleanGeneration;
	CurrentInterval = 0;

	// Debug
	OpAccum = 0;
	DurationCount = 0;
	GrowthCount = 0;

	FBulletHole Temp;
	while (HighPriorityQueue.Dequeue(Temp)) {}
	while (NormalPriorityQueue.Dequeue(Temp)) {}

	DebugHighQueueCount = 0;
	DebugNormalQueueCount = 0;

	// 캐시 정리
	CachedMeshPtr.Reset();
	bCacheValid = false;

	// Chunk 용 Queue 정리
	for (TQueue<FUnionResult, EQueueMode::Mpsc>* Queue : ChunkUnionResultsQueues)
	{
		if (Queue)
		{
			FUnionResult TempResult;
			while (Queue->Dequeue(TempResult)) {}
			delete Queue;
		}
	}

	ChunkUnionResultsQueues.Empty();
	ChunkNextBatchIDs.Empty();
	ChunkNextSubtractBatchIDs.Empty();
}


//-------------------------------------------------------------------
// GT 복사 블로킹 최적화 - 캐시 함수들
//-------------------------------------------------------------------

void FRealtimeBooleanProcessor::SimplifyLog()
{
	UE_LOG(LogTemp, Display, TEXT("Growth %d, Op %d, Dur %d"), GrowthCount, OpAccum, DurationCount);
}

void FRealtimeBooleanProcessor::UpdateMeshCache(FDynamicMesh3&& ResultMesh)
{
	// GT에서만 호출됨
	// 워커 결과를 캐시에 저장하여 다음 작업에서 재사용
	CachedMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(ResultMesh));
	bCacheValid = true;
}

TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> FRealtimeBooleanProcessor::GetCachedMeshForWorker()
{
	// GT에서 호출 - 워커에게 전달할 캐시 포인터 반환
	// 캐시가 유효하면 캐시된 메시 사용, 아니면 컴포넌트에서 새로 생성

	if (bCacheValid && CachedMeshPtr.IsValid())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("GetCachedMeshForWorker_FromCache");
		return CachedMeshPtr;
	}

	// 캐시가 없으면 컴포넌트에서 메시를 가져와서 SharedPtr로 생성
	if (OwnerComponent.IsValid())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("GetCachedMeshForWorker_FromComponent");
		if (UDynamicMesh* TargetMesh = OwnerComponent->GetDynamicMesh())
		{
			TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> NewMeshPtr;
			TargetMesh->ProcessMesh([&](const FDynamicMesh3& Source)
				{
					NewMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(Source);
				});
			CachedMeshPtr = NewMeshPtr;
			bCacheValid = true;
			return NewMeshPtr;
		}
	}

	return nullptr;
}


void FRealtimeBooleanProcessor::EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal, UDynamicMeshComponent* ChunkMesh)
{
	if (!OwnerComponent.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Onwercomponent is invalid"));
		return;
	}

	if (IsHoleMax())
	{
		return;
	}

	/*
	 * 0105 기준 검증단계니까 CellMesh가 없으면 기존 로직 수행
	 * 나중에 CellMesh 불리언 연산이 안정화 되면 기존 로직 제거
	 */
	FBulletHole Op = {};
	Op.ChunkIndex = Operation.Request.ChunkIndex;
	Op.TargetMesh = ChunkMesh ? ChunkMesh : OwnerComponent.Get();
	FTransform ComponentToWorld = Op.TargetMesh->GetComponentTransform();

	const FVector LocalImpact = ComponentToWorld.InverseTransformPosition(Operation.Request.ImpactPoint);
	const FVector LocalNormal = ComponentToWorld.InverseTransformVector(Operation.Request.ImpactNormal).
		GetSafeNormal();
	FQuat ToolRotation = FRotationMatrix::MakeFromZ(LocalNormal).ToQuat(); // cylinder, cone일 경우 방향에 맞게 회전이 필요하다.

	// 스케일 보정: 회전된 좌표계 기준으로 각 축의 스케일 계산
	const FVector ComponentScale = ComponentToWorld.GetScale3D();

	// ToolMesh의 로컬 축이 회전 후 컴포넌트 로컬 좌표계에서 향하는 방향
	FVector ToolAxisX = ToolRotation.RotateVector(FVector::XAxisVector);
	FVector ToolAxisY = ToolRotation.RotateVector(FVector::YAxisVector);
	FVector ToolAxisZ = ToolRotation.RotateVector(FVector::ZAxisVector);

	// 각 축이 ComponentScale에 의해 늘어나는 양 계산
	FVector ScaledAxisX = ToolAxisX * ComponentScale;
	FVector ScaledAxisY = ToolAxisY * ComponentScale;
	FVector ScaledAxisZ = ToolAxisZ * ComponentScale;

	// 보정 스케일: 원래 크기로 되돌리기
	FVector AdjustedScale = FVector(
		1.0f / FMath::Max(KINDA_SMALL_NUMBER, ScaledAxisX.Size()),
		1.0f / FMath::Max(KINDA_SMALL_NUMBER, ScaledAxisY.Size()),
		1.0f / FMath::Max(KINDA_SMALL_NUMBER, ScaledAxisZ.Size())
	);

	Op.ToolTransform = FTransform(ToolRotation, LocalImpact, AdjustedScale);

	Op.bIsPenetration = Operation.bIsPenetration;
	Op.TemporaryDecal = TemporaryDecal;
	Op.ToolMeshPtr = Operation.Request.ToolMeshPtr;

	UE_LOG(LogTemp, Warning, TEXT("High Queue Size: %d"), DebugHighQueueCount);
	UE_LOG(LogTemp, Warning, TEXT("Normal Queue Size: %d"), DebugNormalQueueCount);

	if (Op.bIsPenetration)
	{
		HighPriorityQueue.Enqueue(MoveTemp(Op));
		DebugHighQueueCount++;
		UE_LOG(LogTemp, Warning, TEXT("[Enqueue] ✅ High Priority Queue Size: %d"), DebugHighQueueCount);
	}
	else
	{
		NormalPriorityQueue.Enqueue(MoveTemp(Op));
		DebugNormalQueueCount++;
		UE_LOG(LogTemp, Warning, TEXT("[Enqueue] ✅ Normal Priority Queue Size: %d"), DebugNormalQueueCount);
	}
}

void FRealtimeBooleanProcessor::EnqueueRemaining(FBulletHole&& Operation)
{
	if (Operation.bIsPenetration)
	{
		HighPriorityQueue.Enqueue(MoveTemp(Operation));
		DebugHighQueueCount++;
	}
	else
	{
		NormalPriorityQueue.Enqueue(MoveTemp(Operation));
		DebugNormalQueueCount++;
	}
}

void FRealtimeBooleanProcessor::StartUnionWorker(FBulletHoleBatch&& InBatch, int32 BatchID, int32 Gen)
{
	if (!OwnerComponent.IsValid())
	{
		ActiveUnionWorkers.fetch_sub(1);
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorker_Start");

	// 비동기 Union 작업 시작
	UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[
			OwnerComponent = OwnerComponent,
			LifeTimeToken = LifeTime,
			Batch = MoveTemp(InBatch),
			BatchID,
			Gen,
			this
		]() mutable
		{
			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor || Processor->IsStale(Gen))
			{
				ActiveUnionWorkers.fetch_sub(1);
				return;
			}

			TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorker_Union");

			//Union 수행 
			FDynamicMesh3 CombinedToolMesh;
			TArray<TWeakObjectPtr<UDecalComponent>> Decals;
			int32 UnionCount = 0;

			int32 BatchCount = Batch.Num();
			TArray<FTransform> ToolTransforms = MoveTemp(Batch.ToolTransforms);
			TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
			TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(Batch.ToolMeshPtrs);

			bool bIsFirst = true;

			for (int32 i = 0; i < BatchCount; ++i)
			{
				if (Processor->IsStale(Gen))
				{
					ActiveUnionWorkers.fetch_sub(1);
					return;
				}
				FTransform ToolTransform = MoveTemp(ToolTransforms[i]);
				TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);

				FDynamicMesh3 CurrentTool = MoveTemp(*(ToolMeshPtrs[i]));
				MeshTransforms::ApplyTransform(CurrentTool, (FTransformSRT3d)ToolTransform, true);

				if (TemporaryDecal.IsValid())
				{
					Decals.Add(TemporaryDecal);
				}

				if (bIsFirst)
				{
					CombinedToolMesh = MoveTemp(CurrentTool);
					bIsFirst = false;
					UnionCount++;
				}
				else
				{
					FDynamicMesh3 UnionResult;
					FMeshBoolean MeshUnion(
						&CombinedToolMesh, FTransform::Identity,
						&CurrentTool, FTransform::Identity,
						&UnionResult, FMeshBoolean::EBooleanOp::Union
					);

					if (MeshUnion.Compute())
					{
						CombinedToolMesh = MoveTemp(UnionResult);
						UnionCount++;
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("StartUnionWorker: Failed To Union"));
					}
				}

				if (Processor->IsHoleMax())
				{
					break;
				}

			}

			if (UnionCount > 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Union] BatchID %d completed with UnionCount %d"), BatchID, UnionCount);

				FUnionResult Result;
				Result.BatchID = BatchID;
				Result.Generation = Gen;
				Result.PendingCombinedToolMesh = MoveTemp(CombinedToolMesh);
				Result.Decals = MoveTemp(Decals);
				Result.UnionCount = UnionCount;

				Processor->UnionResultsQueue.Enqueue(MoveTemp(Result));

				UE_LOG(LogTemp, Warning, TEXT("[Union] BatchID %d enqueued, triggering Subtract"), BatchID);

				/** Subtract Worker 트리거 */
				TriggerSubtractWorker();
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Union Fail] ❌ BatchID %d had UnionCount 0"), BatchID);
			}
			ActiveUnionWorkers.fetch_sub(1);
		});
}

void FRealtimeBooleanProcessor::TriggerSubtractWorker()
{
	// 이미 Subtract 작업 중이라면 Return
	bool Expected = false;
	if (!bSubtractInProgress.compare_exchange_strong(Expected, true))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Subtract] Already in progress, skipping"));
		return;
	}

	// Subtract Worker 시작 
	UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[
			OwnerComponent = OwnerComponent,
			LifeTimeToken = LifeTime,
			this
		]() mutable {

			TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorker_Subtract");

			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor || !OwnerComponent.IsValid())
			{
				bSubtractInProgress.store(false);
				return;
			}

			TArray<FUnionResult> PendingResults;
			FUnionResult TempResult;

			// 큐에서 모든 결과 가져오기
			while (Processor->UnionResultsQueue.Dequeue(TempResult))
			{
				PendingResults.Add(MoveTemp(TempResult));
			}

			if (PendingResults.Num() == 0)
			{
				bSubtractInProgress.store(false);
				return;
			}

			// BatchID 순서대로 정렬
			//PendingResults.Sort([](const FUnionResult& A, const FUnionResult& B) {
			//	return A.BatchID < B.BatchID;
			//	});

			// 순서대로 Subtract 진행
			int32 NextBatchID = Processor->NextSubtractBatchID.load();

			// ⭐ 순서 맞는 첫 번째 결과만 처리! (한 번에 하나씩)
			bool bProcessedOne = false;
			TArray<FUnionResult> UnprocessedResults;

			for (int32 i = 0; i < PendingResults.Num(); ++i)
			{
				FUnionResult& Result = PendingResults[i];

				//// 이미 하나 처리했으면 나머지는 재큐잉
				//if (bProcessedOne)
				//{
				//	UnprocessedResults.Add(MoveTemp(PendingResults[i]));
				//	continue;
				//}  
				//if (Result.BatchID != NextBatchID)
				//{
				//	UE_LOG(LogTemp, Warning, TEXT("[Subtract] BatchID %d != NextBatchID %d, re-enqueuing"), Result.BatchID, NextBatchID);
				//	UnprocessedResults.Add(MoveTemp(Result));
				//	continue;
				//}

				if (Processor->IsStale(Result.Generation))
				{
					UE_LOG(LogTemp, Warning, TEXT("[Subtract] BatchID %d is stale, skipping"), Result.BatchID);
					NextBatchID++;
					Processor->NextSubtractBatchID.store(NextBatchID);
					continue;
				}

				// 메시 가져오기
				TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> CachedMesh =
					Processor->GetCachedMeshForWorker();

				if (!CachedMesh.IsValid())
				{
					UE_LOG(LogTemp, Warning, TEXT("[Subtract Fail] CachedMesh invalid for BatchID %d, re-enqueuing all"), Result.BatchID);

					// 현재 + 남은 결과 모두 재큐잉
					for (int32 j = i; j < PendingResults.Num(); ++j)
					{
						UnprocessedResults.Add(MoveTemp(PendingResults[j]));
					}
					break;
				}

				//FDynamicMesh3 WorkMesh = *CachedMesh;

				// Subtract 수행
				FDynamicMesh3 ResultMesh;
				FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();

				bool bSuccess = false;
				{
					TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorker_ApplyBoolean");
					bSuccess = ApplyMeshBooleanAsync(
						CachedMesh.Get(),
						&Result.PendingCombinedToolMesh,
						&ResultMesh,
						EGeometryScriptBooleanOperation::Subtract,
						Options
					);

				}
				if (bSuccess)
				{
					UE_LOG(LogTemp, Warning, TEXT("[Subtract] ✅ BatchID %d succeeded"), Result.BatchID);

					// 다음 배치 ID로 진행
					NextBatchID++;
					Processor->NextSubtractBatchID.store(NextBatchID);

					// ⭐ Game Thread에 반영 + 완료 후 다음 Subtract 트리거
					AsyncTask(ENamedThreads::GameThread,
						[OwnerComponent, Result = MoveTemp(Result),
						ResultMesh = MoveTemp(ResultMesh), Processor, NextBatchID]() mutable
						{
							if (!OwnerComponent.IsValid()) return;

							UE_LOG(LogTemp, Warning, TEXT("[Subtract][GT] Applying BatchID %d to mesh"), Result.BatchID);

							// 메시 반영
							//Processor->CachedMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(ResultMesh));
							//Processor->bCacheValid = true;
							//OwnerComponent->SetMesh(FDynamicMesh3(*Processor->CachedMeshPtr));

							Processor->CachedMeshPtr = MakeShared<FDynamicMesh3>(ResultMesh);  // 복사 1회
							Processor->bCacheValid = true;

							//// SetMesh 전에 작업을 미리 시작하자
							Processor->bSubtractInProgress.store(false);
							Processor->TriggerSubtractWorker();

							{
								TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorker_SetMesh");

								OwnerComponent->SetMesh(MoveTemp(ResultMesh));  // Move (비용 없음)
							}

							/*
							 * deprecated_realdestruction
							 * SetMesh 내부에서 호출됨
							 */
							 // 렌더링 & 충돌
							 // OwnerComponent->ApplyRenderUpdate();
							 // OwnerComponent->ApplyCollisionUpdate();

							 // HoleCount 업데이트
							Processor->CurrentHoleCount += Result.UnionCount;

							UE_LOG(LogTemp, Warning, TEXT("[Subtract][GT] CurrentHoleCount: %d"), Processor->CurrentHoleCount);

							// Decal 처리
							for (const auto& Decal : Result.Decals)
							{
								if (Decal.IsValid())
								{
									// Decal->DestroyComponent();
								}
							}

							// ⭐ 플래그 해제 후 다음 Subtract 트리거!
							//Processor->bSubtractInProgress.store(false);
							//Processor->TriggerSubtractWorker();
						});

					bProcessedOne = true;
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[Subtract Fail] ❌ BatchID %d failed"), Result.BatchID);
					NextBatchID++;
					Processor->NextSubtractBatchID.store(NextBatchID);
				}
			}

			// 처리 못한 결과들 재큐잉
			//for (FUnionResult& UnprocessedResult : UnprocessedResults)
			//{
			//	Processor->UnionResultsQueue.Enqueue(MoveTemp(UnprocessedResult));
			//}
			//
			//if (UnprocessedResults.Num() > 0)
			//{
			//	UE_LOG(LogTemp, Warning, TEXT("[Subtract Fail] Re-enqueued %d unprocessed results"), UnprocessedResults.Num());
			//}

			// ⭐ 하나도 처리 못했으면 여기서 플래그 해제
			if (!bProcessedOne)
			{
				bSubtractInProgress.store(false);

				// 큐에 아직 결과 남아있으면 다시 트리거
				if (!Processor->UnionResultsQueue.IsEmpty())
				{
					UE_LOG(LogTemp, Warning, TEXT("[Subtract] No results processed but queue not empty, re-triggering"));
					Processor->TriggerSubtractWorker();
				}
			}
			//if (!bProcessedOne)
			//{
			//	bSubtractInProgress.store(false);
			//
			//	// 큐에 아직 결과 남아있으면 다시 트리거
			//	if (!Processor->UnionResultsQueue.IsEmpty())
			//	{
			//		UE_LOG(LogTemp, Warning, TEXT("[Subtract] No results processed but queue not empty, re-triggering"));
			//		Processor->TriggerSubtractWorker();
			//	}
			//	else
			//	{
			//		UE_LOG(LogTemp, Warning, TEXT("[Subtract] No results processed, queue empty, done"));
			//	}
			//}
			//else
			//{
			//	// ⭐ 하나 처리했으면 AsyncTask에서 플래그 해제
			//	// (AsyncTask 내부에서 플래그 해제 후 TriggerSubtractWorker 호출)
			//	UE_LOG(LogTemp, Warning, TEXT("[Subtract] Processed one, waiting for GT apply"));
			//}

		});
}

void FRealtimeBooleanProcessor::StartUnionWorkerForChunk(FBulletHoleBatch&& InBatch, int32 BatchID, int32 Gen, int32 ChunkIndex)
{
	if (!OwnerComponent.IsValid() || ChunkIndex == INDEX_NONE)
	{
		ActiveUnionWorkers.fetch_sub(1);
		return;
	}

	if (ChunkIndex < 0 || ChunkIndex >= ChunkUnionResultsQueues.Num() || !ChunkUnionResultsQueues[ChunkIndex])
	{
		ActiveUnionWorkers.fetch_sub(1);
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorkerForChunk_Start");

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [
		OwnerComponent = OwnerComponent,
		LifeTimeToken = LifeTime,
		Batch = MoveTemp(InBatch),
		BatchID,
		Gen,
		ChunkIndex,
		this
	]()mutable
		{
			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor || Processor->IsStaleForChunk(Gen, ChunkIndex))
			{
				ActiveUnionWorkers.fetch_sub(1);
				return;
			}

			TRACE_CPUPROFILER_EVENT_SCOPE("UnionWorkerForChunk_Union");

			// Union 수행 (Chunk 메시 접근 없음 - ToolMesh들만 합침)
			FDynamicMesh3 CombinedToolMesh;
			TArray<TWeakObjectPtr<UDecalComponent>> Decals;
			int32 UnionCount = 0;

			int32 BatchCount = Batch.Num();
			TArray<FTransform> ToolTransforms = MoveTemp(Batch.ToolTransforms);
			TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
			TArray<TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(Batch.ToolMeshPtrs);

			bool bIsFirst = true;

			for (int32 i = 0; i < BatchCount; ++i)
			{
				if (Processor->IsStaleForChunk(Gen, ChunkIndex))
				{
					ActiveUnionWorkers.fetch_sub(1);
					return;
				}

				if (!ToolMeshPtrs[i].IsValid())
				{
					continue;
				}

				FTransform ToolTransform = MoveTemp(ToolTransforms[i]);
				TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);

				FDynamicMesh3 CurrentTool = MoveTemp(*(ToolMeshPtrs[i]));
				MeshTransforms::ApplyTransform(CurrentTool, (FTransformSRT3d)ToolTransform, true);

				if (TemporaryDecal.IsValid())
				{
					Decals.Add(TemporaryDecal);
				}

				if (bIsFirst)
				{
					CombinedToolMesh = MoveTemp(CurrentTool);
					bIsFirst = false;
					UnionCount++;
				}
				else
				{
					FDynamicMesh3 UnionResult;
					FMeshBoolean MeshUnion(
						&CombinedToolMesh, FTransform::Identity,
						&CurrentTool, FTransform::Identity,
						&UnionResult, FMeshBoolean::EBooleanOp::Union
					);

					if (MeshUnion.Compute())
					{
						CombinedToolMesh = MoveTemp(UnionResult);
						UnionCount++;
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("[UnionWorkerForChunk] Union failed at ChunkIndex %d, item %d"), ChunkIndex, i);
					}
				}

				if (Processor->IsHoleMax())
				{
					break;
				}
			}

			if (UnionCount > 0 && CombinedToolMesh.TriangleCount() > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("[UnionWorkerForChunk] ChunkIndex %d, BatchID %d - UnionCount: %d"),
					ChunkIndex, BatchID, UnionCount);

				FUnionResult Result;
				Result.BatchID = BatchID;
				Result.Generation = Gen;
				Result.PendingCombinedToolMesh = MoveTemp(CombinedToolMesh);
				Result.Decals = MoveTemp(Decals);
				Result.UnionCount = UnionCount;
				Result.ChunkIndex = ChunkIndex;

				// Chunk별 Queue에 Enqueue
				Processor->ChunkUnionResultsQueues[ChunkIndex]->Enqueue(MoveTemp(Result));

				// Subtract Worker 트리거
				Processor->TriggerSubtractWorkerForChunk(ChunkIndex);
			}

			ActiveUnionWorkers.fetch_sub(1);
		});
}

void FRealtimeBooleanProcessor::TriggerSubtractWorkerForChunk(int32 ChunkIndex)
{
	if (!OwnerComponent.IsValid() || ChunkIndex == INDEX_NONE)
	{
		return;
	}

	if (ChunkIndex < 0 || ChunkIndex >= ChunkUnionResultsQueues.Num() || !ChunkUnionResultsQueues[ChunkIndex])
	{
		return;
	}

	// GT로 전환하여 비트마스크 체크 (비트 연산은 GT에서만)
	AsyncTask(ENamedThreads::GameThread,
		[OwnerComponent = OwnerComponent, LifeTimeToken = LifeTime, ChunkIndex, this]()
		{
			if (!OwnerComponent.IsValid())
			{
				return;
			}

			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor)
			{
				return;
			}

			// 이미 해당 Chunk의 Subtract 작업 중이면 Return
			if (OwnerComponent->CheckAndSetChunkSubtractBusy(ChunkIndex))
			{
				UE_LOG(LogTemp, Log, TEXT("[SubtractWorkerForChunk] ChunkIndex %d already in progress"), ChunkIndex);
				return;
			}

			// Subtract Worker 시작 (비동기)
			UE::Tasks::Launch(
				UE_SOURCE_LOCATION,
				[OwnerComponent, LifeTimeToken, ChunkIndex, Processor]() mutable
				{
					TRACE_CPUPROFILER_EVENT_SCOPE("SubtractWorkerForChunk");

					auto SafeClearSubtractBusy = [&]()
						{
							AsyncTask(ENamedThreads::GameThread, [OwnerComponent, ChunkIndex]()
								{
									if (OwnerComponent.IsValid())
									{
										OwnerComponent->ClearChunkSubtractBusy(ChunkIndex);
									}
								});
						};

					if (!OwnerComponent.IsValid())
					{
						SafeClearSubtractBusy();
						return;
					}

					// Queue 유효성 검사
					if (ChunkIndex >= Processor->ChunkUnionResultsQueues.Num() ||
						!Processor->ChunkUnionResultsQueues[ChunkIndex])
					{
						SafeClearSubtractBusy();
						return;
					}

					TQueue<FUnionResult, EQueueMode::Mpsc>* ChunkQueue = Processor->ChunkUnionResultsQueues[ChunkIndex];

					// Queue에서 결과 가져오기
					TArray<FUnionResult> PendingResults;
					FUnionResult TempResult;
					while (ChunkQueue->Dequeue(TempResult))
					{
						PendingResults.Add(MoveTemp(TempResult));
					}

					if (PendingResults.Num() == 0)
					{
						SafeClearSubtractBusy();
						return;
					}

					bool bProcessedAny = false;

					for (FUnionResult& Result : PendingResults)
					{
						if (Processor->IsStaleForChunk(Result.Generation, ChunkIndex))
						{
							continue;
						}

						// Chunk 메시 복사
						FDynamicMesh3 WorkMesh;
						if (!OwnerComponent->GetChunkMesh(WorkMesh, ChunkIndex))
						{
							UE_LOG(LogTemp, Warning, TEXT("[SubtractWorkerForChunk] Failed to get ChunkMesh for ChunkIndex %d"), ChunkIndex);
							continue;
						}

						// Subtract 수행
						FDynamicMesh3 ResultMesh;
						FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();

						bool bSuccess = ApplyMeshBooleanAsync(
							&WorkMesh,
							&Result.PendingCombinedToolMesh,
							&ResultMesh,
							EGeometryScriptBooleanOperation::Subtract,
							Options
						);

						if (bSuccess)
						{
							UE_LOG(LogTemp, Log, TEXT("[SubtractWorkerForChunk] ChunkIndex %d, BatchID %d succeeded"),
								ChunkIndex, Result.BatchID);

							// GT에서 결과 반영
							AsyncTask(ENamedThreads::GameThread,
								[OwnerComponent, Result = MoveTemp(Result), ResultMesh = MoveTemp(ResultMesh),
								Processor, ChunkIndex]() mutable
								{
									if (!OwnerComponent.IsValid()) return;

									OwnerComponent->ApplyBooleanOperationResult(MoveTemp(ResultMesh), ChunkIndex, false);
									Processor->CurrentHoleCount += Result.UnionCount;

									for (const auto& Decal : Result.Decals)
									{
										if (Decal.IsValid())
										{
											Decal->DestroyComponent();
										}
									}
								});

							bProcessedAny = true;
						}
					}

					// GT에서 비트 해제 및 다음 트리거
					AsyncTask(ENamedThreads::GameThread,
						[OwnerComponent, Processor, ChunkIndex]()
						{
							if (!OwnerComponent.IsValid()) return;

							OwnerComponent->ClearChunkSubtractBusy(ChunkIndex);

							// Queue에 남은 것 처리
							if (ChunkIndex < Processor->ChunkUnionResultsQueues.Num() &&
								Processor->ChunkUnionResultsQueues[ChunkIndex] &&
								!Processor->ChunkUnionResultsQueues[ChunkIndex]->IsEmpty())
							{
								Processor->TriggerSubtractWorkerForChunk(ChunkIndex);
							}

							Processor->KickProcessIfNeededPerChunk();
						});
				});
		});
}

void FRealtimeBooleanProcessor::KickProcessIfNeeded()
{
	if (bWorkInFlight && !bEnableMultiWorkers)
	{
		return;
	}

	if (IsHoleMax())
	{
		FBulletHole Temp;
		while (HighPriorityQueue.Dequeue(Temp)) {}
		while (NormalPriorityQueue.Dequeue(Temp)) {}
		return;
	}

	// batch에 처리할 연산을 채움
	FBulletHoleBatch Batch;
	int32 BatchCount = DrainBatch(Batch);


	// batch 배열이 0 이하 == 처리할 연산이 없다.
	if (BatchCount <= 0)
	{
		return;
	}

	if (!bEnableMultiWorkers)
	{
		bWorkInFlight = true;
	}

	// TODO: Drain이 항상 1임 => TickComp로 옮겨서 해결
	UE_LOG(LogTemp, Warning, TEXT("[KickProcess] DrainBatch result: %d (High: %d, Normal: %d)"),
		BatchCount, DebugHighQueueCount, DebugNormalQueueCount);

	if (bEnableMultiWorkers)
	{
		// Batch 존재 && Hole 충분 할 때 Worker 배치
		// 가용한 Worker Max Count를 설정
		int32 CurrentCount = ActiveUnionWorkers.load();
		bool bWorkerReserved = false;

		while (CurrentCount < MaxWorkerCount)
		{
			if (ActiveUnionWorkers.compare_exchange_weak(CurrentCount, CurrentCount + 1))
			{
				bWorkerReserved = true;
				break;
			}
		}

		// 워커 잡는거 실패했으면 return
		if (!bWorkerReserved)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Fail] Allocated Max Worker"));
			return;
		}


		/** 순서 추적을 위해 Batch ID 할당 */
		int32 CurrentBatchID = NextBatchID.fetch_add(1);
		int32 Gen = BooleanGeneration.load();
		// TODO: BatchCount가 항상 1임

		UE_LOG(LogTemp, Warning, TEXT("[KickProcess] Starting Union Worker for BatchID %d (Count: %d)"), CurrentBatchID, BatchCount);

		/** Union Worker 시작 */
		StartUnionWorker(MoveTemp(Batch), CurrentBatchID, Gen);
	}
	else
	{
		const int32 Gen = ++BooleanGeneration;

		if (bEnableParallel/* && BatchCount >= ParallelThreshold*/)
		{
			StartBooleanWorkerParallel(MoveTemp(Batch), Gen);
		}
		else
		{
			StartBooleanWorkerAsync(MoveTemp(Batch), Gen);
		}
	}

}

void FRealtimeBooleanProcessor::KickProcessIfNeededPerChunk()
{
	if (IsHoleMax())
	{
		FBulletHole Temp;
		while (HighPriorityQueue.Dequeue(Temp)) {}
		while (NormalPriorityQueue.Dequeue(Temp)) {}
		return;
	}

	/*
	 * 우선 순위별 TMap 생성
	 * Chunk의 주소를 Key로 해서 Chunk별 연산 수집
	 */
	TMap<UDynamicMeshComponent*, FBulletHoleBatch> HighPriorityMap;
	TMap<UDynamicMeshComponent*, FBulletHoleBatch> NormalPriorityMap;

	/*
	 * Map은 순서 보장이 안된다.
	 * 순서 보장용 배열
	 */
	TArray<UDynamicMeshComponent*> HighPriorityOrder;
	TArray<UDynamicMeshComponent*> NormalPriorityOrder;

	auto GatherOps = [&](TQueue<FBulletHole, EQueueMode::Mpsc>& Queue,
		TMap<UDynamicMeshComponent*, FBulletHoleBatch>& OpMap,
		TArray<UDynamicMeshComponent*>& OrderArray, int& DebugCount)
		{
			FBulletHole Op;
			while (Queue.Dequeue(Op))
			{
				auto TargetMesh = Op.TargetMesh.Get();
				if (!TargetMesh)
				{
					continue;
				}

				/*
				 * 맵에 주소가 없다 == 처음 들어온 Chunk
				 * 처음 들어온 Chunk를 순서배열에 저장해서 순서 유지
				 */
				if (!OpMap.Contains(TargetMesh))
				{
					OrderArray.Add(TargetMesh);
				}

				FBulletHoleBatch& Batch = OpMap.FindOrAdd(TargetMesh);
				if (Batch.Num() == 0)
				{
					Batch.Reserve(MaxBatchSize);
				}

				Batch.Add(MoveTemp(Op));
				DebugCount--;
			}
		};

	GatherOps(HighPriorityQueue, HighPriorityMap, HighPriorityOrder, DebugHighQueueCount);
	GatherOps(NormalPriorityQueue, NormalPriorityMap, NormalPriorityOrder, DebugNormalQueueCount);

	auto ProcessTargetMesh = [&](TMap<UDynamicMeshComponent*, FBulletHoleBatch>& OpMap,
		TQueue<FBulletHole, EQueueMode::Mpsc>& Queue,
		TArray<UDynamicMeshComponent*>& OrderArray, int32& DebugCount)
		{
			if (OpMap.IsEmpty() || OrderArray.IsEmpty())
			{
				return;
			}

			for (auto TargetMesh : OrderArray)
			{
				const int32 ChunkIndex = OwnerComponent->GetChunkIndex(TargetMesh);

				if (ChunkIndex == INDEX_NONE)
				{
					continue;
				}

				if (!OwnerComponent->CheckAndSetChunkBusy(ChunkIndex))
				{
					if (FBulletHoleBatch* Batch = OpMap.Find(TargetMesh))
					{
						Batch->ChunkIndex = ChunkIndex;
						if (bEnableMultiWorkers)
						{
							/*
							 * 유니온 워커 로직
							 */
							 // Worker 슬롯 확보 <= 이건 왜 StartBooleanWorkerAsyncForChunk여기서는 안함?
							 // int32 CurrentWorkerCount = ActiveUnionWorkers.load();
							 // bool bWorkerReserved = false;
							 //
							 // while (CurrentWorkerCount < MaxWorkerCount)
							 // {
							 // 	if (ActiveUnionWorkers.compare_exchange_weak(CurrentWorkerCount, CurrentWorkerCount + 1))
							 // 	{
							 // 		bWorkerReserved = true;
							 // 		break;
							 // 	}
							 // }
							 //
							 // if (!bWorkerReserved)
							 // {
							 // 	// Worker 슬롯 부족 - Queue에 되돌리기
							 // 	UE_LOG(LogTemp, Warning, TEXT("[KickProcessPerChunk] Worker limit reached, re-enqueuing ChunkIndex %d"), ChunkIndex);
							 // 	EnqueueRetryOps(Queue, MoveTemp(*Batch), TargetMesh, ChunkIndex, DebugCount);
							 //
							 // 	// ChunkBusy 해제 (이미 설정되어 있으므로)
							 // 	OwnerComponent->ClearChunkBusy(ChunkIndex);
							 // 	continue;
							 // }

							 // batchID, Gen 증가
							int32 CurrentBatchID = ChunkNextBatchIDs[ChunkIndex].fetch_add(1);
							const int32 Gen = ++BooleanGenerations[ChunkIndex];

							// union worker 시작 
							StartUnionWorkerForChunk(MoveTemp(*Batch), CurrentBatchID, Gen, ChunkIndex);

							//ChunkBusy 해체
							OwnerComponent->ClearChunkBusy(ChunkIndex);

						}
						else
						{
							const int32 Gen = ++BooleanGenerations[ChunkIndex];
							StartBooleanWorkerAsyncForChunk(MoveTemp(*Batch), Gen);
						}
					}
				}
				else
				{
					/*
					 * busy상태의 청크를 queue에 되돌리는 로직 추가
					 */
					if (FBulletHoleBatch* Batch = OpMap.Find(TargetMesh))
					{
						UE_LOG(LogTemp, Warning, TEXT("Retry %d"), ChunkIndex);
						Batch->ChunkIndex = ChunkIndex;
						EnqueueRetryOps(Queue, MoveTemp(*Batch), TargetMesh, ChunkIndex, DebugCount);
					}
				}
			}
		};

	ProcessTargetMesh(HighPriorityMap, HighPriorityQueue, HighPriorityOrder, DebugHighQueueCount);
	ProcessTargetMesh(NormalPriorityMap, NormalPriorityQueue, NormalPriorityOrder, DebugNormalQueueCount);
}

int32 FRealtimeBooleanProcessor::DrainBatch(FBulletHoleBatch& InBatch)
{
	InBatch.Reserve(MaxBatchSize);

	FBulletHole Op;
	// Batch를 HighPriorityQueue로 먼저 채운다. 
	while (InBatch.Num() < MaxBatchSize && HighPriorityQueue.Dequeue(Op))
	{
		InBatch.Add(MoveTemp(Op));
		DebugHighQueueCount--;
	}

	// Batch 자리가 남으면? 비관통 작업으로 채운다
	while (InBatch.Num() < MaxBatchSize && NormalPriorityQueue.Dequeue(Op))
	{
		InBatch.Add(MoveTemp(Op));
		DebugNormalQueueCount--;
	}

	return InBatch.Num();
}

void FRealtimeBooleanProcessor::StartBooleanWorkerAsync(FBulletHoleBatch&& InBatch, int32 Gen)
{
	if (!OwnerComponent.IsValid())
	{
		bWorkInFlight = false;
		UE_LOG(LogTemp, Warning, TEXT("OwnerComponent is invalid"));
		return;
	}

	//-------------------------------------------------------------------
	// GT 복사 최적화 플래그에 따른 분기
	//-------------------------------------------------------------------
	const bool bUseOptimization = bUseCachedMeshOptimization;

	// 최적화 모드: SharedPtr로 워커에 전달
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> CachedMesh;
	// 기존 모드: GT에서 직접 복사
	FDynamicMesh3 TargetCopyGT;

	if (bUseOptimization)
	{
		// 최적화 모드: SharedPtr만 가져옴 (캐시 히트 시 복사 없음)
		CachedMesh = GetCachedMeshForWorker();
		if (!CachedMesh.IsValid())
		{
			bWorkInFlight = false;
			UE_LOG(LogTemp, Warning, TEXT("[Async][Optimized] Failed to get cached mesh"));
			return;
		}
		bCacheValid = false;
		UE_LOG(LogTemp, Log, TEXT("[Async] Using OPTIMIZED path (Worker copy)"));
	}
	else
	{
		// 기존 모드: GT에서 직접 복사 (블로킹)
		if (UDynamicMesh* TargetMesh = OwnerComponent->GetDynamicMesh())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync_CopyMesh_GT_Legacy");
			TargetMesh->ProcessMesh([&](const FDynamicMesh3& Source)
				{
					TargetCopyGT = Source;
				});
		}
		else
		{
			bWorkInFlight = false;
			return;
		}
		UE_LOG(LogTemp, Log, TEXT("[Async] Using LEGACY path (GT copy)"));
	}

	FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();
	UE::Tasks::Launch(
		UE_SOURCE_LOCATION, [OwnerComponent = OwnerComponent, LifeTimeToken = LifeTime,
		CachedMesh, TargetCopyGT = MoveTemp(TargetCopyGT), bUseOptimization,
		Batch = MoveTemp(InBatch), Options, Gen]() mutable
		{
			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor)
			{
				return;
			}

			int32 BatchCount = Batch.Num();
			if (BatchCount <= 0)
			{
				Processor->bWorkInFlight = false;
				return;
			}

			if (Processor->IsStale(Gen))
			{
				Processor->bWorkInFlight = false;
				return;
			}

			/*
			 * 현재 구조에서 오직 하나의 워커만 사용 중임 - bWorkInFlight로 KickProcessIfNeeded함수를 가드하고 있음
			 * 멀티 스레드 활용도가 매우 낮은 구조 - 스레드 사용률 증가시킬 필요 있음
			 * 현재 구조에서 충돌이 많이 발생하면 워커 하나로만 처리하기 때문에 TargetMesh에 반영이 느림 - 워커에서 불리언 연산이 끝나고 반영되기 때문에 연산 대기 필요
			 * 비동기 연산 시작 시의 TargetMesh는 유일함.
			 */
			TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync");
			using namespace UE::Geometry;

			// 플래그에 따른 메시 복사
			FDynamicMesh3 WorkMesh;
			if (bUseOptimization)
			{
				// 최적화 모드: 워커에서 복사 (GT 블로킹 없음!)
				TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync_CopyInWorker_Optimized");
				WorkMesh = *CachedMesh;
			}
			else
			{
				// 기존 모드: 이미 GT에서 복사됨
				TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync_UseGTCopy_Legacy");
				WorkMesh = MoveTemp(TargetCopyGT);
			}
			int32 AppliedCount = 0;

			// 실패/미처리 re-queuing
			TArray<FBulletHole> Remaining;
			Remaining.Reserve(Batch.Num());
			int32 DroppedCount = 0;

			TArray<TWeakObjectPtr<UDecalComponent>> DecalsToRemove;

			TArray<FTransform> ToolTransforms = MoveTemp(Batch.ToolTransforms);
			TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
			TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(Batch.ToolMeshPtrs);

			/*
			 * Toolmesh union
			 */
			bool bIsFirst = true;
			bool bCombinedValid = false;
			FDynamicMesh3 CombinedToolMesh;
			int32 UnionCount = 0;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync_Union");
				for (int32 i = 0; i < BatchCount; ++i)
				{
					if (Processor->IsStale(Gen))
					{
						Processor->bWorkInFlight = false;
						return;
					}

					if (!ToolMeshPtrs[i].IsValid())
					{
						continue;
					}

					FTransform ToolTransform = MoveTemp(ToolTransforms[i]);
					TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);

					// ToolMesh를 Local로 변환
					// ShapredPtr이니까 MoveTemp는 안씀
					FDynamicMesh3 CurrentTool = *(ToolMeshPtrs[i]);
					MeshTransforms::ApplyTransform(CurrentTool, (FTransformSRT3d)ToolTransform, true);

					if (TemporaryDecal.IsValid())
					{
						DecalsToRemove.Add(TemporaryDecal);
					}

					if (bIsFirst)
					{
						CombinedToolMesh = CurrentTool;
						bIsFirst = false;
						bCombinedValid = true;
						UnionCount++;
						//Processor->CurrentHoleCount++;
					}
					else
					{
						FDynamicMesh3 UnionResult;
						FMeshBoolean MeshUnion(&CombinedToolMesh, FTransform::Identity,
							&CurrentTool, FTransform::Identity,
							&UnionResult, FMeshBoolean::EBooleanOp::Union);
						if (MeshUnion.Compute())
						{
							CombinedToolMesh = MoveTemp(UnionResult);
							UnionCount++;
							//Processor->CurrentHoleCount++;
						}

					}

					if (Processor->IsHoleMax())
					{
						break;
					}
				}
			}

			bool bSubtractSuccess = false;
			if (bCombinedValid && CombinedToolMesh.TriangleCount() > 0)
			{
				if (Processor->IsStale(Gen))
				{
					Processor->bWorkInFlight = false;
					return;
				}

				TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync_Subtract");

				double CurrentSubDuration = FPlatformTime::Seconds();

				FDynamicMesh3 ResultMesh;
				bSubtractSuccess = ApplyMeshBooleanAsync(&WorkMesh, &CombinedToolMesh, &ResultMesh,
					EGeometryScriptBooleanOperation::Subtract, Options);

				CurrentSubDuration = FPlatformTime::Seconds() - CurrentSubDuration;

				if (bSubtractSuccess)
				{
					// 유니온된 총알 개수만큼 불리언 연산에 적용된다.
					AppliedCount = UnionCount;
					WorkMesh = MoveTemp(ResultMesh);

					Processor->AccumulateSubtractDuration(CurrentSubDuration);
				}
				else
				{
					//  실패하면 누적값 초기화
					Processor->SubtractDurationAccum = 0;
					Processor->DurationAccumCount = 0;
				}
			}

			// 메시 단순화
			if (bSubtractSuccess)
			{
				if (Processor->IsStale(Gen))
				{
					Processor->bWorkInFlight = false;
					return;
				}

				bool bIsSimplified = Processor->TrySimplify(WorkMesh, UnionCount);
			}

			AsyncTask(
				ENamedThreads::GameThread, [OwnerComponent, LifeTimeToken, Gen, Result = MoveTemp(WorkMesh), AppliedCount, Remaining = MoveTemp(Remaining), DroppedCount, DecalsToRemove = MoveTemp(DecalsToRemove), bUseOptimization]() mutable
				{
					/*
					 * 메시 변형이 반복(정점 증가)
					 * 정점 증가된 메시를 MeshComp에 반영하는 비용이 8회 발사 후 약 70배 증가함(피그잼 확인 요망)
					 * 특히 SetMesh비용은 게임 스레드에 반영하는 비용의 대부분을 차지함
					 * Notify와 Collision의 경우 처음에는 매우 적은 비용임
					 * 하지만 정점 수가 증가하면 이 비용도 엄청나게 증가함
					 */
					if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
					{
						return;
					}

					FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
					if (!Processor)
					{
						return;
					}

					if (!OwnerComponent.IsValid())
					{
						Processor->bWorkInFlight = false;
						return;
					}

					Processor->bWorkInFlight = false;
					if (OwnerComponent->GetBooleanProcessor() != Processor)
					{
						return;
					}
					UE_LOG(LogTemp, Display, TEXT("Current Hole %d"), Processor->CurrentHoleCount);

					TRACE_BOOKMARK(TEXT("ApplyGT Start"));
					TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsyncApplyGT")

						if (Processor->IsStale(Gen))
						{
							Processor->bWorkInFlight = false;
							return;
						}


					if (AppliedCount > 0)
					{
						double CurrentSetMeshAvgCost = 0.0;
						//-------------------------------------------------------------------
						// 플래그에 따른 메시 반영 방식 분기
						//-------------------------------------------------------------------
						if (bUseOptimization)
						{
							// 최적화 모드: 캐시에 저장 후 컴포넌트에 복사
							TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsyncApplyGT_CacheAndSetMesh_Optimized");

							// 결과를 캐시에 move 저장
							Processor->CachedMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(Result));
							Processor->bCacheValid = true;

							CurrentSetMeshAvgCost = FPlatformTime::Seconds();
							// 캐시에서 컴포넌트로 복사해서 전달
							OwnerComponent->SetMesh(FDynamicMesh3(*Processor->CachedMeshPtr));
							CurrentSetMeshAvgCost = FPlatformTime::Seconds() - CurrentSetMeshAvgCost;
						}
						else
						{
							CurrentSetMeshAvgCost = FPlatformTime::Seconds();
							// 기존 모드: 직접 SetMesh
							TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsyncApplyGT_SetMesh_Legacy");
							OwnerComponent->SetMesh(MoveTemp(Result));
							CurrentSetMeshAvgCost = FPlatformTime::Seconds() - CurrentSetMeshAvgCost;
						}

						Processor->UpdateSimplifyInterval(CurrentSetMeshAvgCost);

						/*
						 * deprecated_realdestruction
						 */
						 // {
							//  TRACE_BOOKMARK(TEXT("Notify Start"));
							//  TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsyncApplyGT_Notify");
							//  OwnerComponent->ApplyRenderUpdate();						
							//  TRACE_BOOKMARK(TEXT("Notify End"));
						 // }
						 // {
							//  TRACE_BOOKMARK(TEXT("Collision Start"));
							//  TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsyncApplyGT_Collision");
							//  OwnerComponent->ApplyCollisionUpdate();
							//  TRACE_BOOKMARK(TEXT("Collision End"));
						 // }
						{

							for (const TWeakObjectPtr<UDecalComponent>& Decal : DecalsToRemove)
							{
								if (Decal.IsValid())
								{
									//Decal->DestroyComponent();
								}
							}
						}

						Processor->CurrentHoleCount += AppliedCount;
					}

					Processor->SimplifyLog();

					LifeTimeToken->Processor.load()->KickProcessIfNeeded();
				});
		});
}

void FRealtimeBooleanProcessor::StartBooleanWorkerAsyncForChunk(FBulletHoleBatch&& InBatch, int32 Gen)
{
	if (InBatch.Num() == 0 || !OwnerComponent.IsValid())
	{
		return;
	}

	FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();
	UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[OwnerComponent = OwnerComponent, LifeTimeToken = LifeTime,
		Batch = MoveTemp(InBatch), Options, Gen]() mutable
		{
			// bit를 안전하게 해제
			auto SafeClearBusyBit = [&]()
				{
					if (OwnerComponent.IsValid())
					{
						int32 IndexToClear = Batch.ChunkIndex;
						AsyncTask(ENamedThreads::GameThread, [OwnerComponent, IndexToClear]()
							{
								if (OwnerComponent.IsValid())
								{
									OwnerComponent->ClearChunkBusy(IndexToClear);
								}
							});
					}
				};

			if (!OwnerComponent.IsValid())
			{
				return;
			}

			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor)
			{
				SafeClearBusyBit();
				return;
			}

			const int32 ChunkIndex = Batch.ChunkIndex;
			if (Processor->IsStaleForChunk(Gen, ChunkIndex))
			{
				SafeClearBusyBit();
				return;
			}

			const int32 BatchCount = Batch.Num();
			if (BatchCount <= 0)
			{
				SafeClearBusyBit();
				return;
			}


			// 타겟메시 복사			
			FDynamicMesh3 WorkMesh;
			if (!OwnerComponent->GetChunkMesh(WorkMesh, ChunkIndex))
			{
				SafeClearBusyBit();
				return;
			}

			using namespace UE::Geometry;

			int32 AppliedCount = 0;
			TArray<TWeakObjectPtr<UDecalComponent>> DecalsToRemove;
			DecalsToRemove.Reserve(BatchCount);
			TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
			TArray<FTransform> Transforms = MoveTemp(Batch.ToolTransforms);
			TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(Batch.ToolMeshPtrs);

			int32 UnionCount = 0;
			bool bIsFirst = true;
			bool bCombinedValid = false;
			FDynamicMesh3 CombinedToolMesh;
			for (int32 i = 0; i < BatchCount; i++)
			{
				if (Processor->IsStaleForChunk(Gen, ChunkIndex))
				{
					SafeClearBusyBit();
					return;
				}

				if (!ToolMeshPtrs[i].IsValid())
				{
					SafeClearBusyBit();
					return;
				}

				FTransform ToolTransform = MoveTemp(Transforms[i]);
				TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);

				FDynamicMesh3 CurrentTool = *(ToolMeshPtrs[i]);
				MeshTransforms::ApplyTransform(CurrentTool, (FTransformSRT3d)ToolTransform, true);

				if (TemporaryDecal.IsValid())
				{
					DecalsToRemove.Add(MoveTemp(TemporaryDecal));
				}

				if (bIsFirst)
				{
					bIsFirst = false;
					CombinedToolMesh = CurrentTool;
					bCombinedValid = true;
					UnionCount++;
				}
				else
				{
					FDynamicMesh3 UnionResult;
					FMeshBoolean MeshUnion(&CombinedToolMesh, FTransform::Identity,
						&CurrentTool, FTransform::Identity,
						&UnionResult, FMeshBoolean::EBooleanOp::Union);
					if (MeshUnion.Compute())
					{
						CombinedToolMesh = MoveTemp(UnionResult);
						UnionCount++;
					}
				}

				if (Processor->IsHoleMax())
				{
					break;
				}
			}
			UE_LOG(LogTemp, Warning, TEXT("[Union] UnionCount %d BatchCount %d"), UnionCount, BatchCount);

			FDynamicMeshAABBTree3 WorkMeshAABBTree(&WorkMesh);
			bool bSubtractSuccess = false;
			if (bCombinedValid && CombinedToolMesh.TriangleCount() > 0)
			{
				if (Processor->IsStaleForChunk(Gen, ChunkIndex))
				{
					SafeClearBusyBit();
					return;
				}

				/*
				 * 실제 교차하는 지 검사
				 */
				FAxisAlignedBox3d ToolBounds = CombinedToolMesh.GetBounds();
				if (ToolBounds.Volume() <= KINDA_SMALL_NUMBER)
				{
					SafeClearBusyBit();
					return;
				}

				bool bIsOverlap = WorkMeshAABBTree.TestIntersection(&CombinedToolMesh, ToolBounds);
				if (!bIsOverlap)
				{
					SafeClearBusyBit();
					return;
				}

				double CurrentSubDuration = FPlatformTime::Seconds();

				FDynamicMesh3 ResultMesh;
				bSubtractSuccess = ApplyMeshBooleanAsync(&WorkMesh, &CombinedToolMesh, &ResultMesh,
					EGeometryScriptBooleanOperation::Subtract, Options);

				CurrentSubDuration = FPlatformTime::Seconds() - CurrentSubDuration;

				if (bSubtractSuccess)
				{
					// 유니온된 총알 개수만큼 불리언 연산에 적용된다.
					AppliedCount = UnionCount;
					WorkMesh = MoveTemp(ResultMesh);

					Processor->AccumulateSubtractDuration(CurrentSubDuration);
				}
				else
				{
					//  실패하면 누적값 초기화
					Processor->SubtractDurationAccum = 0;
					Processor->DurationAccumCount = 0;
				}
			}

			// 메시 단순화
			if (bSubtractSuccess)
			{
				if (Processor->IsStaleForChunk(Gen, ChunkIndex))
				{
					SafeClearBusyBit();
					return;
				}

				bool bIsSimplified = Processor->TrySimplify(WorkMesh, UnionCount);
			}

			AsyncTask(ENamedThreads::GameThread,
				[OwnerComponent, LifeTimeToken, Gen, ChunkIndex, Result = MoveTemp(WorkMesh), AppliedCount, DecalsToRemove = MoveTemp(DecalsToRemove)]() mutable
				{
					if (!OwnerComponent.IsValid())
					{
						return;
					}
					OwnerComponent->ClearChunkBusy(ChunkIndex);

					if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
					{
						return;
					}

					FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
					if (!Processor)
					{
						return;
					}

					if (OwnerComponent->GetBooleanProcessor() != Processor)
					{
						return;
					}

					if (Processor->IsStaleForChunk(Gen, ChunkIndex))
					{
						return;
					}

					if (AppliedCount > 0)
					{
						double CurrentSetMeshAvgCost = FPlatformTime::Seconds();
						OwnerComponent->ApplyBooleanOperationResult(MoveTemp(Result), ChunkIndex, false);
						CurrentSetMeshAvgCost = CurrentSetMeshAvgCost - FPlatformTime::Seconds();

						Processor->UpdateSimplifyInterval(CurrentSetMeshAvgCost);

						for (const TWeakObjectPtr<UDecalComponent>& Decal : DecalsToRemove)
						{
							if (Decal.IsValid())
							{
								Decal->DestroyComponent();
							}
						}
					}
					Processor->CurrentHoleCount += AppliedCount;

					Processor->SimplifyLog();

					Processor->KickProcessIfNeededPerChunk();
				});
		});
}

void FRealtimeBooleanProcessor::StartBooleanWorkerParallel(FBulletHoleBatch&& InBatch, int32 Gen)
{
	// 유효성 검사
	if (!OwnerComponent.IsValid())
	{
		bWorkInFlight = false;
		return;
	}

	int32 RecommendedThreads = AutoTuner.GetRecommendedThreadCount();

	int32 BatchCount = InBatch.Num();
	int32 BatchBasedLimit = (BatchCount + 1) / 2;

	int32 ActualThreads = FMath::Clamp(RecommendedThreads, 1, BatchBasedLimit);

	//-------------------------------------------------------------------
	// GT 복사 최적화 플래그에 따른 분기
	//-------------------------------------------------------------------
	const bool bUseOptimization = bUseCachedMeshOptimization;

	// 최적화 모드: SharedPtr로 워커에 전달
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> CachedMesh;
	// 기존 모드: GT에서 직접 복사
	FDynamicMesh3 TargetCopyGT;

	if (bUseOptimization)
	{
		// 최적화 모드: SharedPtr만 가져옴 (캐시 히트 시 복사 없음)
		CachedMesh = GetCachedMeshForWorker();
		if (!CachedMesh.IsValid())
		{
			bWorkInFlight = false;
			UE_LOG(LogTemp, Warning, TEXT("[Parallel][Optimized] Failed to get cached mesh"));
			return;
		}
		bCacheValid = false;
		UE_LOG(LogTemp, Log, TEXT("[Parallel] Using OPTIMIZED path (Worker copy)"));
	}
	else
	{
		// 기존 모드: GT에서 직접 복사 (블로킹)
		if (UDynamicMesh* TargetMesh = OwnerComponent->GetDynamicMesh())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_CopyMesh_GT_Legacy");
			TargetMesh->ProcessMesh([&](const FDynamicMesh3& Source)
				{
					TargetCopyGT = Source;
				});
		}
		else
		{
			bWorkInFlight = false;
			return;
		}
		UE_LOG(LogTemp, Log, TEXT("[Parallel] Using LEGACY path (GT copy)"));
	}

	FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();
	const int32 LocalMaxThreads = MaxParallelThreads;

	//비동기 작업 시작
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[CachedMesh, TargetCopyGT = MoveTemp(TargetCopyGT), bUseOptimization,
		Batch = MoveTemp(InBatch), Options, ActualThreads, OwnerComponent = OwnerComponent, LifeTimeToken = LifeTime,
		Gen, BatchCount, this]() mutable
		{
			TRACE_BOOKMARK(TEXT("BooleanBatch Start (Count: %d, Threads: %d)"), BatchCount, ActualThreads);

			TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_Async");
			TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*FString::Printf(TEXT("Batch: %d"), BatchCount));
			const double WorkStartSec = FPlatformTime::Seconds();
			int32 NumThreads = ActualThreads;

			// 플래그에 따른 메시 복사
			FDynamicMesh3 TargetCopy;
			if (bUseOptimization)
			{
				// 최적화 모드: 워커에서 복사 (GT 블로킹 없음!)
				TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_CopyInWorker_Optimized");
				TargetCopy = *CachedMesh;
			}
			else
			{
				// 기존 모드: 이미 GT에서 복사됨
				TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_UseGTCopy_Legacy");
				TargetCopy = MoveTemp(TargetCopyGT);
			}

			const int32 BatchSize = Batch.Num();

			TArray<FDynamicMesh3> ThreadTools;
			ThreadTools.SetNum(NumThreads);

			TArray<TArray<TWeakObjectPtr<UDecalComponent>>> PerThreadDecals;
			PerThreadDecals.SetNum(NumThreads);

			TArray<FTransform> ToolTransforms = MoveTemp(Batch.ToolTransforms);
			TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
			TArray<TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(Batch.ToolMeshPtrs);
			/*
			 * 현재 사용하지 않음
			 */
			 // TArray<uint8> Attempts = MoveTemp(Batch.Attempts);
			 // TArray<bool> bIsPenetrations = MoveTemp(Batch.bIsPenetrations);
			ParallelFor(NumThreads, [&](int32 ForIndex)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_ParallerFor");

					const int32 ItemsPerThreads = BatchSize / NumThreads;
					const int32 StartIdx = ForIndex * ItemsPerThreads;
					const int32 EndIdx =
						(ForIndex == NumThreads - 1) ? BatchSize : StartIdx + ItemsPerThreads;
					// 마지막 부분은 꽉차있지 않을 수 있음

					FDynamicMesh3 CombinedTool;
					bool bFirst = true;
					TArray<TWeakObjectPtr<UDecalComponent>> LocalDecals;

					for (int32 i = StartIdx; i < EndIdx; ++i)
					{
						// const FBulletHole& Hole = Batch[i];
						FTransform ToolTransform = MoveTemp(ToolTransforms[i]);
						TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);
						FDynamicMesh3 SingleTool = *(ToolMeshPtrs[i]);

						// 총알을 Local Transform으로 이동시키기) 
						MeshTransforms::ApplyTransform(
							SingleTool, (UE::Geometry::FTransformSRT3d)ToolTransform, true);

						// decal도 모으자
						if (TemporaryDecal.IsValid()) LocalDecals.Add(TemporaryDecal);


						if (bFirst)
						{
							CombinedTool = MoveTemp(SingleTool);
							bFirst = false;
						}
						else
						{
							FDynamicMesh3 Result;
							FMeshBoolean UnionOp(&CombinedTool, FTransformSRT3d::Identity(), &SingleTool,
								FTransformSRT3d::Identity(), &Result,
								FMeshBoolean::EBooleanOp::Union);
							UnionOp.bSimplifyAlongNewEdges = Options.bSimplifyOutput;
							if (UnionOp.Compute())
							{
								CombinedTool = MoveTemp(Result);
							}
							else
							{
								UE_LOG(LogTemp, Warning, TEXT("[Parallel] Union OP Error"));
							}
						}
					}

					// 총알 구멍을 합들을 배열에 저장
					ThreadTools[ForIndex] = MoveTemp(CombinedTool);
					PerThreadDecals[ForIndex] = MoveTemp(LocalDecals);
				}, EParallelForFlags::None);


			//  최종 Tool 병합 
			FDynamicMesh3 BigTool = HierarchicalUnion(ThreadTools, Options);
			TArray<TWeakObjectPtr<UDecalComponent>> AllDecals;
			for (auto& List : PerThreadDecals) AllDecals.Append(List);

			FDynamicMesh3 FinalResult;
			bool bSuccess = false;

			// BigTool이 비어있지 않은 경우에만 연산
			if (BigTool.TriangleCount() > 0)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_SubtractOP");

				FMeshBoolean SubtractOp(
					&TargetCopy, FTransformSRT3d::Identity(),
					&BigTool, FTransformSRT3d::Identity(),
					&FinalResult,
					FMeshBoolean::EBooleanOp::Difference
				);

				SubtractOp.bSimplifyAlongNewEdges = Options.bSimplifyOutput;
				bSuccess = SubtractOp.Compute();
			}


			int32 ActualAppliedCount = 0;
			// 실패했거나 Tool이 없으면 원본 유지 (혹은 실패 처리)
			if (bSuccess)
			{
				ActualAppliedCount = BatchSize;
			}
			else
			{
				// 실패시 원본 유지 및 데칼 삭제 취소
				FinalResult = MoveTemp(TargetCopy);
				AllDecals.Empty();
				ActualAppliedCount = 0;
			}

			double WorkEndSec = FPlatformTime::Seconds();
			double WorkDuration = WorkEndSec - WorkStartSec;

			// GameThread에 결과 반영
			AsyncTask(ENamedThreads::GameThread,
				[OwnerComponent, LifeTimeToken, Gen, Result = MoveTemp(FinalResult), AppliedCount = ActualAppliedCount,
				DecalsToRemove = MoveTemp(AllDecals), WorkDuration, BatchCount, bUseOptimization]() mutable
				{
					// 수명 체크
					if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load()) return;

					FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
					if (!Processor || !OwnerComponent.IsValid())
					{
						if (Processor) Processor->bWorkInFlight = false;
						return;
					}

					// Processor 상태 업데이트
					Processor->bWorkInFlight = false;

					// 세대 체크
					if (Gen != Processor->BooleanGeneration)
					{
						Processor->KickProcessIfNeeded();
						return;
					}
					TRACE_COUNTER_SET(Counter_ThreadCount, Processor->AutoTuner.CurrentThreadCount);

					float WorkTimeMs = WorkDuration * 1000.0f;
					TRACE_COUNTER_SET(Counter_WorkTime, WorkTimeMs);

					float Throughput = (WorkDuration > 0.0)
						? (float)BatchCount / WorkDuration
						: 0.0f;
					TRACE_COUNTER_SET(Counter_Throughput, Throughput);

					TRACE_COUNTER_SET(Counter_BatchSize, BatchCount);

					float CurrentDeltaTime = FApp::GetDeltaTime();
					Processor->AutoTuner.Update(BatchCount, WorkDuration, CurrentDeltaTime);

					//-------------------------------------------------------------------
					// 플래그에 따른 메시 반영 방식 분기
					//-------------------------------------------------------------------
					if (bUseOptimization)
					{
						// 최적화 모드: 캐시에 저장 후 컴포넌트에 복사
						TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_CacheAndSetMesh_Optimized");

						// 결과를 캐시에 move 저장
						Processor->CachedMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(MoveTemp(Result));
						Processor->bCacheValid = true;

						// 캐시에서 컴포넌트로 복사해서 전달
						OwnerComponent->SetMesh(FDynamicMesh3(*Processor->CachedMeshPtr));
					}
					else
					{
						// 기존 모드: 직접 SetMesh
						TRACE_CPUPROFILER_EVENT_SCOPE("BooleanWorkerParallel_SetMesh_Legacy");
						OwnerComponent->SetMesh(MoveTemp(Result));
					}

					for (const auto& Decal : DecalsToRemove)
						if (Decal.IsValid())
							Decal->DestroyComponent();
					/*
					 * deprecated_realdestruction
					 * SetMesh내부에서 호출됨
					 */
					 // OwnerComponent->ApplyRenderUpdate();
					 // OwnerComponent->ApplyCollisionUpdate();

					Processor->CurrentHoleCount += AppliedCount;
					Processor->KickProcessIfNeeded();
				});
		});
}

void FRealtimeBooleanProcessor::CancelAllOperations()
{
	/*
	 * Lifetime은 살려두는 함수
	 * OwnerComponent의 ResetToSourceMesh함수 내부에서 호출
	 */
	++BooleanGeneration;

	SetMeshAvgCost = 0.0;

	CurrentInterval = 0;
	InitInterval = 0;

	SubDurationHighThreshold = 0.0;
	SubDurationLowThreshold = 0.0;
	SubtractDurationAccum = 0.0;
	DurationAccumCount = 0;

	LastSimplifyTriCount = 0;


	FBulletHole Temp;
	while (HighPriorityQueue.Dequeue(Temp)) {}
	while (NormalPriorityQueue.Dequeue(Temp)) {}

	DebugHighQueueCount = 0;
	DebugNormalQueueCount = 0;

	// Debug
	OpAccum = 0;
	DurationCount = 0;
	GrowthCount = 0;

	// 리셋 시 HoleCount와 캐시도 초기화
	CurrentHoleCount = 0;
	CachedMeshPtr.Reset();
	bCacheValid = false;
}

void FRealtimeBooleanProcessor::AccumulateSubtractDuration(double CurrentSubDuration)
{
	// 임계값을 넘으면 시간 누적
	if (CurrentSubDuration >= SubDurationHighThreshold)
	{
		SubtractDurationAccum += CurrentSubDuration;
		DurationAccumCount++;
		UE_LOG(LogTemp, Display, TEXT("Accumulate Duration %d"), DurationAccumCount);
	}
	// 한 번이라도 누적되었는 데 이번 틱에서 임계값을 넘지 않으면 초기화
	else if (CurrentSubDuration < SubDurationHighThreshold && DurationAccumCount > 0)
	{
		SubtractDurationAccum = 0;
		DurationAccumCount = 0;
		UE_LOG(LogTemp, Display, TEXT("Accumulate Reset"));
	}
}

void FRealtimeBooleanProcessor::UpdateSimplifyInterval(double CurrentSetMeshAvgCost)
{

	if (FMath::IsNearlyZero(SetMeshAvgCost))
	{
		SetMeshAvgCost = CurrentSetMeshAvgCost;
		return;
	}

	const double OldAvgCost = SetMeshAvgCost;

	// 지수 이동 평균(EMA, 선형보간이랑 같음) 계산
	const double NewAvgCost = FMath::Lerp(SetMeshAvgCost, CurrentSetMeshAvgCost, 0.1);
	SetMeshAvgCost = NewAvgCost;

	// 감소율 : (이전값 - 현재값) / 이전값 
	const double ReductionRate = (OldAvgCost - NewAvgCost) / OldAvgCost;

	/*
	 * 튜닝 상수
	 */
	 // 증가 임계값, 이 값을 넘어서면 Interval 감소
	const double PanicThreshold = 0.1;
	// 안정화 임계값, 감소율이 안정화 임계값과 같거나 크면 Interval 증가
	const double StableThreshold = 0.0;

	/*
	 * AIMD(Additive Increase, Multiplicate Decrease) - TCP 혼잡 제어에서 쓴다네요
	 * 천천히 증가하고 급격히 감소시킨다. - 비용 증가에 빠르게 대응
	 */

	 // 10% 이상 비용 증가 시 Interval 감소
	if (-ReductionRate > PanicThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Interval decrease %lld to %d"), FMath::FloorToInt(MaxInterval * 0.7), MaxInterval);
		// 하한선 15 보장
		MaxInterval = FMath::Max(15, FMath::FloorToInt(MaxInterval * 0.7));
	}
	// 비용이 감소하거나 늘어나지 않은 경우 천천히 증가
	else if (ReductionRate >= StableThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Interval increase %d to %d"), MaxInterval, MaxInterval + 1);
		MaxInterval = FMath::Min(InitInterval * 2, MaxInterval + 1);
	}
	// 0 ~ 10%의 증가는 관망
	// 로그 확인을 위해서 else문 유지 
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Interval hold"));
	}
}

bool FRealtimeBooleanProcessor::TrySimplify(UE::Geometry::FDynamicMesh3& WorkMesh, int32 UnionCount)
{
	CurrentInterval += UnionCount;

	bool bShouldSimplify = false;
	const int32 TriCount = WorkMesh.TriangleCount();

	if ((TriCount > LastSimplifyTriCount * 1.2f && LastSimplifyTriCount > 1000) || TriCount - LastSimplifyTriCount > 1000)
	{
		GrowthCount++;
		bShouldSimplify = true;
	}
	// 2회 누적되고, 누적된 값의 평균이 임계값 이상이면 simplify
	else if (DurationAccumCount == 2 && SubtractDurationAccum / DurationAccumCount >= SubDurationHighThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Duration Simplify"));
		DurationCount++;
		bShouldSimplify = true;
	}
	// 현재 인터벌이 최대 인터벌에 도달하면 단순화
	else if (CurrentInterval >= MaxInterval)
	{
		CurrentInterval = 0;
		OpAccum++;
		bShouldSimplify = true;
	}

	if (bShouldSimplify)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("ApplyMeshBooleanAsync_Simplify");
		FGeometryScriptPlanarSimplifyOptions SimplifyOptions;
		// SimplifyOptions.bAutoCompact = true;
		SimplifyOptions.bAutoCompact = false;
		SimplifyOptions.AngleThreshold = AngleThreshold;
		ApplySimplifyToPlanarAsync(&WorkMesh, SimplifyOptions);
		/*
		 * 12.26 기준 런타임에서 LastSimplifyTriCount에 GT는 접근하지 않음
		 */
		LastSimplifyTriCount = WorkMesh.TriangleCount();
	}

	return bShouldSimplify;
}

void FRealtimeBooleanProcessor::EnqueueRetryOps(TQueue<FBulletHole, EQueueMode::Mpsc>& Queue, FBulletHoleBatch&& InBatch,
	UDynamicMeshComponent* TargetMesh, int32 ChunkIndex, int32& DebugCount)
{
	int32 BatchCount = InBatch.Num();
	if (BatchCount == 0)
	{
		return;
	}

	FBulletHole Op = {};
	for (int32 i = 0; i < BatchCount; i++)
	{
		if (InBatch.Get(Op, i))
		{
			Op.ChunkIndex = ChunkIndex;
			Op.TargetMesh = TargetMesh;
			Queue.Enqueue(Op);
			DebugCount++;
		}
		Op.Reset();
	}
}

bool FRealtimeBooleanProcessor::ApplyMeshBooleanAsync(const UE::Geometry::FDynamicMesh3* TargetMesh,
	const UE::Geometry::FDynamicMesh3* ToolMesh,
	UE::Geometry::FDynamicMesh3* OutputMesh,
	const EGeometryScriptBooleanOperation Operation,
	const FGeometryScriptMeshBooleanOptions Options,
	const FTransform& TargetTransform,
	const FTransform& ToolTransform)
{
	check(TargetMesh != nullptr && ToolMesh != nullptr && OutputMesh != nullptr);

	using namespace UE::Geometry;

	// 필요하다면 다른 연산으로 확장
	FMeshBoolean::EBooleanOp Op = FMeshBoolean::EBooleanOp::Difference;
	switch (Operation)
	{
	case EGeometryScriptBooleanOperation::Subtract:
		Op = FMeshBoolean::EBooleanOp::Difference;
		break;
	default:
		Op = FMeshBoolean::EBooleanOp::Difference;
		break;
	}

	// Mesh 연산
	FMeshBoolean MeshBoolean(
		TargetMesh, (FTransformSRT3d)TargetTransform,
		ToolMesh, (FTransformSRT3d)ToolTransform,
		OutputMesh, Op);
	MeshBoolean.bPutResultInInputSpace = true;
	MeshBoolean.bSimplifyAlongNewEdges = Options.bSimplifyOutput;
	bool bSuccess = MeshBoolean.Compute();

	/*
	 * 현재 사용되지 않는 코드임
	 * bFillHoles의 true, false 여부와 관계없이 동일한 결과
	 */
	TArray<int> NewBoundaryEdges = MoveTemp(MeshBoolean.CreatedBoundaryEdges);
	if (NewBoundaryEdges.Num() > 0 && Options.bFillHoles)
	{
		FMeshBoundaryLoops OpenBoundary(OutputMesh, false);
		TSet<int> ConsiderEdges(NewBoundaryEdges);
		OpenBoundary.EdgeFilterFunc = [&ConsiderEdges](int EID)
			{
				return ConsiderEdges.Contains(EID);
			};
		OpenBoundary.Compute();

		for (FEdgeLoop& Loop : OpenBoundary.Loops)
		{
			UE::Geometry::FMinimalHoleFiller Filler(OutputMesh, Loop);
			Filler.Fill();
		}
	}

	return bSuccess;
}

void FRealtimeBooleanProcessor::ApplySimplifyToPlanarAsync(UE::Geometry::FDynamicMesh3* TargetMesh, FGeometryScriptPlanarSimplifyOptions Options)
{
	if (!TargetMesh)
	{
		return;
	}
	using namespace UE::Geometry;

	FQEMSimplification Simplifier(TargetMesh);

	Simplifier.CollapseMode = FQEMSimplification::ESimplificationCollapseModes::AverageVertexPosition;
	Simplifier.SimplifyToMinimalPlanar(FMath::Max(0.00001, Options.AngleThreshold));

	if (Options.bAutoCompact)
	{
		TargetMesh->CompactInPlace();
	}
}

UE::Geometry::FDynamicMesh3 FRealtimeBooleanProcessor::HierarchicalUnion(TArray<UE::Geometry::FDynamicMesh3>& Results, const FGeometryScriptMeshBooleanOptions& Options)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(HierarchicalUnion);

	// 빈 메시 필터링 (제안하신 내용 반영)
	Results.RemoveAll([](const FDynamicMesh3& Mesh) {
		return Mesh.TriangleCount() == 0;
		});

	if (Results.Num() == 0) return FDynamicMesh3();
	if (Results.Num() == 1) return MoveTemp(Results[0]);

	// 계층적 병합
	//TODO: 병합도 Reduction으로 빠르게 할 수 있지 않을까?
	while (Results.Num() > 1)
	{
		TArray<FDynamicMesh3> NextLevel;
		NextLevel.Reserve((Results.Num() + 1) / 2);

		// 2개씩 묶어서 Union
		for (int32 i = 0; i < Results.Num(); i += 2)
		{
			if (i + 1 < Results.Num())
			{
				// Union 연산
				FDynamicMesh3 Merged;

				FMeshBoolean MeshBoolean(
					&Results[i], FTransformSRT3d::Identity(),
					&Results[i + 1], FTransformSRT3d::Identity(),
					&Merged, FMeshBoolean::EBooleanOp::Union);

				MeshBoolean.bSimplifyAlongNewEdges = Options.bSimplifyOutput;
				bool bSuccess = MeshBoolean.Compute();

				if (bSuccess)
				{
					NextLevel.Add(MoveTemp(Merged));
				}
				else
				{
					// 실패 시 첫 번째 메시 유지
					UE_LOG(LogTemp, Warning, TEXT("Union failed, keeping first mesh"));
					NextLevel.Add(MoveTemp(Results[i]));
				}
			}
			else
			{
				// 홀수 개일 때 마지막 메시
				NextLevel.Add(MoveTemp(Results[i]));
			}
		}
		Results = MoveTemp(NextLevel);
	}

	return MoveTemp(Results[0]);
}
