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

// DEUBUG용 
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "HAL/CriticalSection.h"

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
		ChunkGenerations.SetNumZeroed(ChunkNum);
		ChunkStates.Initialize(ChunkNum);
		ChunkHoleCount.SetNumZeroed(ChunkNum);

		// 초기값 10으로 시작
		MaxUnionCount.Init(10, ChunkNum);

		// Chunk용 MulyiWorker 변수 초기화
		ChunkUnionResultsQueues.SetNum(ChunkNum);
		for (int32 i = 0; i < ChunkNum; ++i)
		{
			ChunkUnionResultsQueues[i] = new TQueue<FUnionResult, EQueueMode::Mpsc>();
			
			// chunkstates의 lastsimplifytricount 설정
			FDynamicMesh3 ChunkMesh;
			OwnerComponent->GetChunkMesh(ChunkMesh, i);
			ChunkStates.States[i].LastSimplifyTriCount = ChunkMesh.TriangleCount();
		}

		ChunkNextBatchIDs.SetNumZeroed(ChunkNum); 
	}

	LifeTime = MakeShared<FProcessorLifeTime, ESPMode::ThreadSafe>();
	LifeTime->bAlive.store(true);
	LifeTime->Processor.store(this);

	/*
	 * Simplify test
	 */	
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

	FBulletHole Temp;
	while (HighPriorityQueue.Dequeue(Temp)) {}
	while (NormalPriorityQueue.Dequeue(Temp)) {}

	DebugHighQueueCount = 0;
	DebugNormalQueueCount = 0;

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

	ChunkGenerations.Empty();

	ChunkStates.Shutdown();
}

void FRealtimeBooleanProcessor::EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal, UDynamicMeshComponent* ChunkMesh)
{
	if (!OwnerComponent.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Onwercomponent is invalid"));
		return;
	}

	if (!ChunkMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Chunk is null"));
		return;
	}

	FBulletHole Op = {};
	Op.ChunkIndex = Operation.Request.ChunkIndex;
	Op.TargetMesh = ChunkMesh;
	FTransform ComponentToWorld = Op.TargetMesh->GetComponentTransform();

	const FVector LocalImpact = ComponentToWorld.InverseTransformPosition(Operation.Request.ToolCenterWorld);
	const FVector LocalNormal = ComponentToWorld.InverseTransformVector(Operation.Request.ToolForwardVector).
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

void FRealtimeBooleanProcessor::StartUnionWorkerForChunk(FBulletHoleBatch&& InBatch, int32 BatchID, int32 ChunkIndex)
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
		ChunkIndex,
		this
	]()mutable
		{
			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor)
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
				if (!ToolMeshPtrs[i].IsValid())
				{
					continue;
				}
				 
				FTransform ToolTransform = MoveTemp(ToolTransforms[i]); 
				TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);
				
				// 메시가 비어있으면 스킵 (크래시 방지)
				FDynamicMesh3 CurrentTool = *(ToolMeshPtrs[i]);
				if (CurrentTool.TriangleCount() == 0)
				{
					UE_LOG(LogTemp, Warning, TEXT("[UnionWorkerForChunk] Skipping empty ToolMesh at ChunkIndex %d, item %d"), ChunkIndex, i);
					continue;
				}


				//FDynamicMesh3 CurrentTool = MoveTemp(*(ToolMeshPtrs[i]));
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
			}

			if (UnionCount > 0 && CombinedToolMesh.TriangleCount() > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("[UnionWorkerForChunk] ChunkIndex %d, BatchID %d - UnionCount: %d"),
					ChunkIndex, BatchID, UnionCount);

				FUnionResult Result;
				Result.BatchID = BatchID;
				// Result.Generation = Gen;
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
			if (OwnerComponent->CheckAndSetChunkBusy(ChunkIndex))
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
					auto SafeClearBusy = [&]()
						{
							AsyncTask(ENamedThreads::GameThread, [OwnerComponent, ChunkIndex]()
								{
									if (OwnerComponent.IsValid())
									{
										OwnerComponent->ClearChunkBusy(ChunkIndex);
									}
								});
						};

					if (!OwnerComponent.IsValid())
					{
						SafeClearBusy();
						return;
					}

					// Queue 유효성 검사
					if (ChunkIndex >= Processor->ChunkUnionResultsQueues.Num() ||
						!Processor->ChunkUnionResultsQueues[ChunkIndex])
					{
						SafeClearBusy();
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
						SafeClearBusy();
						return;
					}

					bool bProcessedAny = false;

					double BatchStartTime = FPlatformTime::Seconds();
					for (int32 ResultIndex = 0; ResultIndex < PendingResults.Num(); ++ResultIndex)
					{
						FUnionResult& Result = PendingResults[ResultIndex];
						
						if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
						{
							UE_LOG(LogTemp, Warning, TEXT("[SubtractWorkerForChunk] Chunk %d : LifeToken invalid"), ChunkIndex);
							return;
						}

						Processor = LifeTimeToken->Processor.load();
						if (!Processor)
						{
							UE_LOG(LogTemp, Warning, TEXT("[SubtractWorkerForChunk] Chunk %d : Processor invalid"), ChunkIndex);
							return;
						}

						// Chunk 메시 복사
						FDynamicMesh3 WorkMesh;
						if (!OwnerComponent->GetChunkMesh(WorkMesh, ChunkIndex))
						{
							UE_LOG(LogTemp, Warning, TEXT("[SubtractWorkerForChunk] Failed to get ChunkMesh for ChunkIndex %d"), ChunkIndex);
							continue;
						}

						// Subtract 수행 전 메시 유효성 검사
						if (WorkMesh.TriangleCount() == 0)
						{
							UE_LOG(LogTemp, Warning, TEXT("[SubtractWorkerForChunk] Chunk %d : Workmesh Triangle count is zero"), ChunkIndex);
							continue;
						}

						if (Result.PendingCombinedToolMesh.TriangleCount() == 0)
						{
							UE_LOG(LogTemp, Warning, TEXT("[SubtractWorkerForChunk] Chunk %d : Toolmesh Triangle count is zero"), ChunkIndex);
							continue;
						}

						// Subtract 수행
						FDynamicMesh3 ResultMesh;
						FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();

						double CurrentSubDuration = FPlatformTime::Seconds();

						bool bSuccess = ApplyMeshBooleanAsync(
							&WorkMesh,
							&Result.PendingCombinedToolMesh,
							&ResultMesh,
							EGeometryScriptBooleanOperation::Subtract,
							Options
						);

						CurrentSubDuration = FPlatformTime::Seconds() - CurrentSubDuration;						

						if (bSuccess)
						{
							Processor->AccumulateSubtractDuration(ChunkIndex, CurrentSubDuration);
							Processor->TrySimplify(ResultMesh, ChunkIndex, Result.UnionCount);
							
							double SubtractCost = CurrentSubDuration * 1000.0;
							Processor->UpdateSubtractAvgCost(SubtractCost);
							Processor->UpdateUnionSize(ChunkIndex, SubtractCost);

							// GT에서 결과 반영
							AsyncTask(ENamedThreads::GameThread,
								[OwnerComponent, Result = MoveTemp(Result), ResultMesh = MoveTemp(ResultMesh),
								LifeTimeToken, ChunkIndex]() mutable
								{
									if (!OwnerComponent.IsValid())
									{
										return;
									}


									// Processor 유효성 검사 추가
									if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
									{
										return;
									}
									FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
									if (!Processor)
									{
										return;
									}

									double CurrentSetMeshAvgCost = FPlatformTime::Seconds();
									
									OwnerComponent->ApplyBooleanOperationResult(MoveTemp(ResultMesh), ChunkIndex, false);
									++Processor->ChunkGenerations[ChunkIndex];

									CurrentSetMeshAvgCost = CurrentSetMeshAvgCost - FPlatformTime::Seconds();
									Processor->UpdateSimplifyInterval(CurrentSetMeshAvgCost);
									
									Processor->ChunkHoleCount[ChunkIndex] += Result.UnionCount;

									for (const auto& Decal : Result.Decals)
									{
										if (Decal.IsValid())
										{
											//Decal->DestroyComponent();
										}
									}
								});

							bProcessedAny = true;
						}
						else
						{
							/*
							 * 실패한 연산 처리를 어떻게 할 것인가
							 * 불리언 실패해서 데칼 남는 경우는 없음 
							 */
						}

						// 시간 체크
						double ElapseMs = (FPlatformTime::Seconds() - BatchStartTime) * 1000.0f;
						
						UE_LOG(LogTemp, Warning, TEXT("[Adaptive Subtract] %d Num: ElapseMs: %.2f"), PendingResults.Num(), ElapseMs);
						if (ElapseMs > Processor->FrameBudgetMs)
						{
							// 남은 결과 다시 Queue에 넣기 
							UE_LOG(LogTemp, Warning, TEXT("[Adaptive Subtract] Pass Next Frame: %d"), PendingResults.Num());
							for (int32 j = ResultIndex + 1; j < PendingResults.Num(); ++j)
							{
								ChunkQueue->Enqueue(MoveTemp(PendingResults[j]));								
							}
							break;
						}
					}

					// GT에서 비트 해제 및 다음 트리거
					AsyncTask(ENamedThreads::GameThread,
						[OwnerComponent, LifeTimeToken, ChunkIndex]()
						{
							if (!OwnerComponent.IsValid()) return;
							 
							if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
							{
								return;
							}
							FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
							if (!Processor)
							{
								return;
							}

							OwnerComponent->ClearChunkBusy(ChunkIndex);
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

void FRealtimeBooleanProcessor::UpdateSubtractAvgCost(double CostMs)
{
	SubtractCostAccum += CostMs;
	SubtractCostSampleCount++;

	if (SubtractCostSampleCount >= 10)
	{
		SubtractAvgCostMs = SubtractCostAccum / SubtractCostSampleCount;
		SubtractCostAccum = SubtractAvgCostMs;
		SubtractCostSampleCount = 1;
	}
}

void FRealtimeBooleanProcessor::UpdateUnionSize(int32 ChunkIndex, double DurationMs)
{
	const int32 CurrentUnionCount = MaxUnionCount[ChunkIndex];
	int32 NextCount = CurrentUnionCount;
	
	if (DurationMs > FrameBudgetMs)
	{
		// 70%로 감소
		NextCount = FMath::FloorToInt(CurrentUnionCount * 0.7f);

		/*
		 * 극단적으로 비용이 커져도 프레임 방어할 수 있도록 최소 1개로 결정
		 */
		NextCount = FMath::Max(1, NextCount);
	}
	else if (DurationMs < (FrameBudgetMs * 0.6))
	{
		/*
		 * 1. 20개도 충분할 것 같음
		 * 2. 모든 메시에 대해서 프로파일링하는 건 비현실적임.
		 */
		NextCount = FMath::Min(CurrentUnionCount + 1, 20);
	}

	if (NextCount != CurrentUnionCount)
	{
		MaxUnionCount[ChunkIndex] = CurrentUnionCount;
	}
}

void FRealtimeBooleanProcessor::KickProcessIfNeededPerChunk()
{
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

	

	// 임의로 메모리 할당
	HighPriorityOrder.Reserve(100);
	NormalPriorityOrder.Reserve(100);

	auto GatherOps = [&](TQueue<FBulletHole, EQueueMode::Mpsc>& Queue,
	                     TMap<UDynamicMeshComponent*, FBulletHoleBatch>& OpMap,
	                     TArray<UDynamicMeshComponent*>& OrderArray, int& DebugCount)
	{
		// Union 개수를 넘어서는 작업(Overflow)을 임시 저장할 배열(Re-enqueue용)
		TArray<FBulletHole> OverflowOps;
		OverflowOps.Reserve(50);
		
		FBulletHole Op;
		while (Queue.Dequeue(Op))
		{
			auto TargetMesh = Op.TargetMesh.Get();
			if (!TargetMesh)
			{
				continue;
			}

			const int32 ChunkIndex = OwnerComponent->GetChunkIndex(TargetMesh);
			if (ChunkIndex == INDEX_NONE)
			{
				continue;
			}
			
			const int32 ChunkUnionLimit = MaxUnionCount.IsValidIndex(ChunkIndex) ? MaxUnionCount[ChunkIndex] : 10;

			// FindOrAdd 대신 Find를 사용해서 맵에 불필요한 Key 생성 방지
			FBulletHoleBatch* Batch = OpMap.Find(TargetMesh);
			const int32 CurrentCount = Batch ? Batch->Num() : 0;

			// union 개수를 초과하는 경우 Overflow 배열에 관리 후 Re-enqueue
			if (CurrentCount >= ChunkUnionLimit)
			{
				OverflowOps.Add(MoveTemp(Op));
			}
			else
			{
				if (!Batch)
				{
					// 순서 등록
					OrderArray.Add(TargetMesh);

					// 맵 생성
					Batch = &OpMap.Add(TargetMesh);
					Batch->Reserve(ChunkUnionLimit);
				}
				Batch->Add(MoveTemp(Op));
				DebugCount--;
			}
		}

		for (FBulletHole& OverflowOp : OverflowOps)
		{
			Queue.Enqueue(MoveTemp(OverflowOp));
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

			if (FBulletHoleBatch* Batch = OpMap.Find(TargetMesh))
			{
				Batch->ChunkIndex = ChunkIndex;
				if (bEnableMultiWorkers)
				{
					// batchID, Gen 증가
					int32 CurrentBatchID = ChunkNextBatchIDs[ChunkIndex].fetch_add(1);

					// union worker 시작 
					StartUnionWorkerForChunk(MoveTemp(*Batch), CurrentBatchID, ChunkIndex);
				}
				else
				{
					if (!OwnerComponent->CheckAndSetChunkBusy(ChunkIndex))
					{
						const int32 Gen = ChunkGenerations[ChunkIndex];
						StartBooleanWorkerAsyncForChunk(MoveTemp(*Batch), Gen);
					}
					else
					{
						/*
							* busy상태의 청크를 queue에 되돌리는 로직 추가
							*/
						EnqueueRetryOps(Queue, MoveTemp(*Batch), TargetMesh, ChunkIndex, DebugCount);
					}
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

			TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync");
			FRealtimeBooleanProcessor* Processor = LifeTimeToken->Processor.load();
			if (!Processor)
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

			const int32 ChunkIndex = Batch.ChunkIndex;
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
			{
				TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Union");
				for (int32 i = 0; i < BatchCount; i++)
				{

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
				}
			}
						
			bool bSubtractSuccess = false;
			if (bCombinedValid && CombinedToolMesh.TriangleCount() > 0)
			{
				double CurrentSubDuration = FPlatformTime::Seconds();

				FDynamicMesh3 ResultMesh;
				{
					TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Subtract");
					bSubtractSuccess = ApplyMeshBooleanAsync(&WorkMesh, &CombinedToolMesh, &ResultMesh,
					                                         EGeometryScriptBooleanOperation::Subtract, Options);
				}
				++Processor->ChunkGenerations[ChunkIndex];
				
				CurrentSubDuration = FPlatformTime::Seconds() - CurrentSubDuration;

				if (bSubtractSuccess)
				{
					// 유니온된 총알 개수만큼 불리언 연산에 적용된다.
					AppliedCount = UnionCount;
					WorkMesh = MoveTemp(ResultMesh);

					Processor->AccumulateSubtractDuration(ChunkIndex, CurrentSubDuration);
				}
				else
				{ 
					//  실패하면 누적값 초기화
					FChunkState& State = Processor->ChunkStates.GetState(ChunkIndex);
					State.SubtractDurationAccum = 0;
					State.DurationAccumCount = 0;
				}
			}

			// 메시 단순화
			if (bSubtractSuccess)
			{
				if (ChunkIndex != INDEX_NONE)
				{
					TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Simplify");
					bool bIsSimplified = Processor->TrySimplify(WorkMesh, ChunkIndex, UnionCount);
				}
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

					TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_ApplyGT");
					if (AppliedCount > 0)
					{
						double CurrentSetMeshAvgCost = FPlatformTime::Seconds();
						{
							TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_SetMesh");
							OwnerComponent->ApplyBooleanOperationResult(MoveTemp(Result), ChunkIndex, false);
						}
						CurrentSetMeshAvgCost = CurrentSetMeshAvgCost - FPlatformTime::Seconds();

						Processor->UpdateSimplifyInterval(CurrentSetMeshAvgCost);

						for (const TWeakObjectPtr<UDecalComponent>& Decal : DecalsToRemove)
						{
							if (Decal.IsValid())
							{
								//Decal->DestroyComponent();
							}
						}
					}
					Processor->ChunkHoleCount[ChunkIndex] += AppliedCount;

					Processor->KickProcessIfNeededPerChunk();
				});
		});
}

void FRealtimeBooleanProcessor::CancelAllOperations()
{

	SetMeshAvgCost = 0.0;

	InitInterval = 0;

	FBulletHole Temp;
	while (HighPriorityQueue.Dequeue(Temp)) {}
	while (NormalPriorityQueue.Dequeue(Temp)) {}

	DebugHighQueueCount = 0;
	DebugNormalQueueCount = 0;

	// 리셋 시 HoleCount와 캐시도 초기화
	CurrentHoleCount = 0;

	ChunkStates.Reset();

	ChunkHoleCount.Init(OwnerComponent->GetChunkNum(), 0);
}

void FRealtimeBooleanProcessor::AccumulateSubtractDuration(int32 ChunkIndex, double CurrentSubDuration)
{
	FChunkState& State = ChunkStates.GetState(ChunkIndex);
	// 임계값을 넘으면 시간 누적
	if (CurrentSubDuration >= SubDurationHighThreshold)
	{
		State.SubtractDurationAccum += CurrentSubDuration;
		State.DurationAccumCount++;
		UE_LOG(LogTemp, Display, TEXT("Accumulate Duration %d"), State.DurationAccumCount);
	}
	// 한 번이라도 누적되었는 데 이번 틱에서 임계값을 넘지 않으면 초기화
	else if (CurrentSubDuration < SubDurationHighThreshold && State.DurationAccumCount > 0)
	{
		State.SubtractDurationAccum = 0;
		State.DurationAccumCount = 0;
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

bool FRealtimeBooleanProcessor::TrySimplify(UE::Geometry::FDynamicMesh3& WorkMesh, int32 ChunkIndex, int32 UnionCount)
{
	if (!ChunkStates.States.IsValidIndex(ChunkIndex))
	{
		return false;
	}
	
	FChunkState& State = ChunkStates.GetState(ChunkIndex);
	State.Interval += UnionCount;
	
	bool bShouldSimplify = false;
	const int32 TriCount = WorkMesh.TriangleCount();

	if ((TriCount > State.LastSimplifyTriCount * 1.2f &&
		State.LastSimplifyTriCount > 1000) ||
		TriCount - State.LastSimplifyTriCount > 1000)
	{
		bShouldSimplify = true;
	}
	// 2회 누적되고, 누적된 값의 평균이 임계값 이상이면 simplify
	else if (State.DurationAccumCount >= 2 &&
		State.SubtractDurationAccum / State.DurationAccumCount >= SubDurationHighThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Duration Simplify"));
		bShouldSimplify = true;
	}
	// 현재 인터벌이 최대 인터벌에 도달하면 단순화
	else if (State.Interval >= MaxInterval)
	{		
		bShouldSimplify = true;
	}

	if (bShouldSimplify)
	{
		State.Reset();	
		
		FGeometryScriptPlanarSimplifyOptions SimplifyOptions;
		// SimplifyOptions.bAutoCompact = true;
		SimplifyOptions.bAutoCompact = false;
		SimplifyOptions.AngleThreshold = AngleThreshold;
		ApplySimplifyToPlanarAsync(&WorkMesh, SimplifyOptions);
		/*
		 * 12.26 기준 런타임에서 LastSimplifyTriCount에 GT는 접근하지 않음
		 */
		State.LastSimplifyTriCount = WorkMesh.TriangleCount();
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

int32& FRealtimeBooleanProcessor::GetChunkInterval(int32 ChunkIndex)
{
	/*
	 * 호출 전에 인덱스 유효성 검사하세요.
	 */
	return ChunkStates.GetState(ChunkIndex).Interval;
}

int32 FRealtimeBooleanProcessor::GetChunkHoleCount(const UPrimitiveComponent* ChunkComponent) const
{
	if (!ChunkComponent)
	{
		return INDEX_NONE;
	}
	
	int32 ChunkIndex = OwnerComponent->GetChunkIndex(ChunkComponent);

	return GetChunkHoleCount(ChunkIndex);
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

	// 빈 메시 체크 - AABB 트리 빌드 시 크래시 방지
	if (TargetMesh->TriangleCount() == 0 || ToolMesh->TriangleCount() == 0)
	{ 
		return false;
	}

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

	const int32 MaxAttempts = 2;
	for (int32 Attempt = 0; Attempt < MaxAttempts; Attempt++)
	{
		FTransform CurrentToolTransform = ToolTransform;

		// 첫 boolean 실패 시 위치와 회전을 살짝 비틀어서 재시도
		if (Attempt > 0)
		{
			// 1.5mm
			const float JitterAmount = 0.015f;
			// 0.1 deg
			const float JitterAngle = 0.1f;

			FVector RandomOffset(
				FMath::FRandRange(-JitterAmount, JitterAmount),
				FMath::FRandRange(-JitterAmount, JitterAmount),
				FMath::FRandRange(-JitterAmount, JitterAmount));

			FQuat RandomRot(FVector::UpVector, FMath::DegreesToRadians(
				FMath::FRandRange(-JitterAngle, JitterAngle)));

			CurrentToolTransform.AddToTranslation(RandomOffset);
			CurrentToolTransform.SetRotation(CurrentToolTransform.GetRotation() * RandomRot);

			UE_LOG(LogTemp, Log, TEXT("[Boolean] Attempt %d: Retrying with Jitter"), Attempt);
		}

		// Mesh 연산
		FMeshBoolean MeshBoolean(
			TargetMesh, (FTransformSRT3d)TargetTransform,
			ToolMesh, (FTransformSRT3d)CurrentToolTransform,
			OutputMesh, Op);
	
		MeshBoolean.bPutResultInInputSpace = true;
		MeshBoolean.bSimplifyAlongNewEdges = Options.bSimplifyOutput;
		MeshBoolean.bWeldSharedEdges = false;

		bool bSuccess = MeshBoolean.Compute();

		if (bSuccess)
		{
			/*
			 * welding 처리
			 * 열린 메시를 닫힌 메시로 만듬
			 */
			FMergeCoincidentMeshEdges Welder(OutputMesh);
			Welder.MergeSearchTolerance = 0.001;

			Welder.Apply();

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

			return true;
		}
		// 실패한 경우 Clear 후 재시도
		OutputMesh->Clear();
	}	

	UE_LOG(LogTemp, Warning, TEXT("[Boolean] All attempts failed."));
	return false;
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