// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "RealtimeBooleanProcessor.h"
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
// DEBUG only
#include "DebugConsoleVariables.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/RDMThreadManagerSubsystem.h"
#include "Remesher.h"
#include "MeshConstraintsUtil.h"
#include "Actors/DebrisActor.h"
#include "Operations/MeshClusterSimplifier.h"
#include "Debug/DebugConsoleVariables.h"

TRACE_DECLARE_INT_COUNTER(Counter_ThreadCount, TEXT("RealtimeDestruction/ThreadCount"));
TRACE_DECLARE_INT_COUNTER(Counter_UnionThreadCount, TEXT("RealtimeDestruction/UnionThreadCount"));
TRACE_DECLARE_INT_COUNTER(Counter_SubtractWorkerCount, TEXT("RealtimeDestruction/SubtractThreadCount"));
TRACE_DECLARE_INT_COUNTER(Counter_ActiveChunks, TEXT("RealtimeDestruction/ActiveChunks"));

TRACE_DECLARE_FLOAT_COUNTER(Counter_Throughput, TEXT("RealtimeDestruction/Throughput"));
TRACE_DECLARE_INT_COUNTER(Counter_BatchSize, TEXT("RealtimeDestruction/BatchSize"));
TRACE_DECLARE_FLOAT_COUNTER(Counter_WorkTime, TEXT("RealtimeDestruction/WorkTimeMs"));

using namespace UE::Geometry;

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
	OwnerComponent->SettingAsyncOption(bEnableMultiWorkers);

	InitInterval = OwnerComponent->GetInitInterval();

	int32 ChunkNum = OwnerComponent->GetChunkNum();
	if (ChunkNum > 0)
	{
		ChunkGenerations.SetNumZeroed(ChunkNum);
		ChunkStates.Initialize(ChunkNum);
		ChunkHoleCount.SetNumZeroed(ChunkNum);
		MaxInterval.Init(InitInterval, ChunkNum);
		SetMeshAvgCost.SetNumZeroed(ChunkNum);

		// Start with an initial value of 10
		MaxUnionCount.Init(10, ChunkNum);

		// Initialize chunk multi-worker state
		ChunkUnionResultsQueues.SetNum(ChunkNum);
		
		for (int32 i = 0; i < ChunkNum; ++i)
		{
			ChunkUnionResultsQueues[i] = MakeUnique<TQueue<FUnionResult, EQueueMode::Mpsc>>();
			
			// Set LastSimplifyTriCount for chunk states
			FDynamicMesh3 ChunkMesh;
			OwnerComponent->GetChunkMesh(ChunkMesh, i);
			ChunkStates.States[i].LastSimplifyTriCount = ChunkMesh.TriangleCount();
		}

		ChunkNextBatchIDs.SetNumZeroed(ChunkNum); 
	}

	LifeTime = MakeShared<FProcessorLifeTime, ESPMode::ThreadSafe>();
	LifeTime->Init(Owner->GetBooleanProcessorShared());	

	AngleThreshold = OwnerComponent->GetAngleThreshold();
	SubDurationHighThreshold = OwnerComponent->GetSubtractDurationLimit();	

	InitializeSlots();

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

	// Clear chunk queues
	for (auto& Queue : ChunkUnionResultsQueues)
	{
		if (Queue)
		{
			FUnionResult TempResult;
			while (Queue->Dequeue(TempResult)) {} 
		}
	}

	ChunkUnionResultsQueues.Empty();
	ChunkNextBatchIDs.Empty(); 

	ChunkGenerations.Empty();

	ChunkStates.Shutdown();

	ShutdownSlots();
}

void FRealtimeBooleanProcessor::EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal, UDynamicMeshComponent* ChunkMesh, int32 BatchId)
{
	if (!OwnerComponent.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("OwnerComponent is invalid"));
		if (auto Owner = OwnerComponent.Get())
		{
			Owner->NotifyBooleanSkipped(BatchId);
		}
		return;
	}

	if (!ChunkMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Chunk is null"));
		OwnerComponent->NotifyBooleanSkipped(BatchId);
		return;
	}

	FBulletHole Op = {};
	Op.ChunkIndex = Operation.Request.ChunkIndex;
	Op.BatchId = BatchId;
	Op.TargetMesh = ChunkMesh;
	FTransform ComponentToWorld = Op.TargetMesh->GetComponentTransform();

	const FVector LocalImpact = ComponentToWorld.InverseTransformPosition(Operation.Request.ToolOriginWorld);

	// Scale correction: compute axis scales in the rotated frame.
	const FVector ComponentScale = ComponentToWorld.GetScale3D();

	switch (Operation.Request.ToolShape)
	{
	case EDestructionToolShape::Cylinder:
	{
		const FVector LocalNormal = ComponentToWorld.InverseTransformVector(Operation.Request.ToolForwardVector).GetSafeNormal();
		FQuat ToolRotation = FRotationMatrix::MakeFromZ(LocalNormal).ToQuat(); // Cylinders and cones must rotate to match direction.

		// Tool mesh local axes after rotation in component local space.
		FVector ToolAxisX = ToolRotation.RotateVector(FVector::XAxisVector);
		FVector ToolAxisY = ToolRotation.RotateVector(FVector::YAxisVector);
		FVector ToolAxisZ = ToolRotation.RotateVector(FVector::ZAxisVector);

		// Compute axis stretching from ComponentScale.
		FVector ScaledAxisX = ToolAxisX * ComponentScale;
		FVector ScaledAxisY = ToolAxisY * ComponentScale;
		FVector ScaledAxisZ = ToolAxisZ * ComponentScale;

		// Adjusted scale: restore to original size.
		FVector AdjustedScale = FVector(
			1.0f / FMath::Max(KINDA_SMALL_NUMBER, ScaledAxisX.Size()),
			1.0f / FMath::Max(KINDA_SMALL_NUMBER, ScaledAxisY.Size()),
			1.0f / FMath::Max(KINDA_SMALL_NUMBER, ScaledAxisZ.Size())
		);

		Op.ToolTransform = FTransform(ToolRotation, LocalImpact, AdjustedScale);
		break;
	}
	case EDestructionToolShape::Sphere:
	{ 
		  FVector InverseScale = FVector(
		  1.0f / FMath::Max(KINDA_SMALL_NUMBER, ComponentScale.X),
		  1.0f / FMath::Max(KINDA_SMALL_NUMBER, ComponentScale.Y),
		  1.0f / FMath::Max(KINDA_SMALL_NUMBER, ComponentScale.Z)
		);

      Op.ToolTransform = FTransform(FQuat::Identity, LocalImpact, InverseScale);
		break;
	}
	default:
		break;
	}
	  
	Op.bIsPenetration = Operation.bIsPenetration;
	Op.TemporaryDecal = TemporaryDecal;
	Op.ToolMeshPtr = Operation.Request.ToolMeshPtr;

	if (FRDMCVarHelper::EnableAsyncBooleanOp())
	{
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
	else
	{
		BooleanOpSync(MoveTemp(Op));
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

void FRealtimeBooleanProcessor::EnqueueIslandRemoval(
	int32 ChunkIndex,
	TSharedPtr<UE::Geometry::FDynamicMesh3> ToolMesh,
	TSharedPtr<UE::Geometry::FDynamicMesh3> DebrisToolMesh,
	TSharedPtr<FIslandRemovalContext> Context)
{
	if (ChunkIndex == INDEX_NONE)
	{
		return;
	}

	// ToolMesh 또는 DebrisToolMesh 중 하나라도 있어야 함
	// Client extraction의 경우 ToolMesh는 nullptr이고 DebrisToolMesh만 있음
	if (!ToolMesh.IsValid() && !DebrisToolMesh.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[EnqueueIslandRemoval] Both ToolMesh and DebrisToolMesh are invalid - skipping"));
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[EnqueueIslandRemoval] ChunkIndex=%d, ToolMesh=%d, DebrisToolMesh=%d, TargetDebrisActor=%d"),
		ChunkIndex, ToolMesh.IsValid() ? 1 : 0, DebrisToolMesh.IsValid() ? 1 : 0,
		(Context.IsValid() && Context->TargetDebrisActor.IsValid()) ? 1 : 0);

	FUnionResult WorkItem = {};
	WorkItem.ChunkIndex = ChunkIndex;
	WorkItem.SharedToolMesh = ToolMesh;
	WorkItem.DebrisSharedToolMesh = DebrisToolMesh;
	WorkItem.OutDebrisMesh = MakeShared<FDynamicMesh3>();
	WorkItem.WorkType = EBooleanWorkType::IslandRemoval;
	WorkItem.IslandContext = Context;
	
	WorkItem.Decals = {};
	WorkItem.PendingCombinedToolMesh = {};
	WorkItem.UnionCount = 0;

	int32 SlotIndex = FindLeastBusySlot();
	if (SlotSubtractQueues.IsValidIndex(SlotIndex))
	{
		SlotSubtractQueues[SlotIndex]->Enqueue(MoveTemp(WorkItem));
		KickSubtractWorker(SlotIndex);
	}
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

URDMThreadManagerSubsystem* FRealtimeBooleanProcessor::GetThreadManager() const
{
	if (!OwnerComponent.IsValid())
	{
		return nullptr;
	}
	UWorld* World = OwnerComponent->GetWorld();
	return URDMThreadManagerSubsystem::Get(World);
}

void FRealtimeBooleanProcessor::InitializeSlots()
{
	if (URDMThreadManagerSubsystem* ThreadManager = GetThreadManager())
	{
		NumSlots = ThreadManager->GetSlotCount();
	}
	else
	{
		NumSlots = 1;
	}
	
	// Create union queues.
	SlotUnionQueues.SetNum(NumSlots);
	for (int32 i = 0; i < NumSlots; ++i)
	{
		SlotUnionQueues[i] = MakeUnique<TQueue<FBulletHoleBatch, EQueueMode::Mpsc>>();
	}

	// Create subtract queues.
	SlotSubtractQueues.SetNum(NumSlots);
	for (int32 i = 0; i < NumSlots; ++i)
	{
		SlotSubtractQueues[i] = MakeUnique<TQueue<FUnionResult, EQueueMode::Mpsc>>();
	} 

	// Create active flags.
	SlotUnionActiveFlags.SetNum(NumSlots);
	SlotSubtractActiveFlags.SetNum(NumSlots);
	SlotUnionWorkerCounts.SetNum(NumSlots);     
	SlotSubtractWorkerCounts.SetNum(NumSlots);
	for (int32 i = 0; i < NumSlots; ++i)
	{
		SlotUnionActiveFlags[i] = MakeUnique<std::atomic<bool>>(false);
		SlotSubtractActiveFlags[i] = MakeUnique<std::atomic<bool>>(false);

		
		SlotUnionWorkerCounts[i] = MakeUnique<std::atomic<int32>>(0);
		SlotSubtractWorkerCounts[i] = MakeUnique<std::atomic<int32>>(0);
	}	 
}

void FRealtimeBooleanProcessor::ShutdownSlots()
{
	// Clear union queues.
	  for (auto& Queue : SlotUnionQueues)
	  {
	  	if (Queue)
	  	{
	  		FBulletHoleBatch Dummy;
	  		while (Queue->Dequeue(Dummy)) {} 
	  	}
	  }
	SlotUnionQueues.Empty();

	// Clear subtract queues.
	for (auto& Queue : SlotSubtractQueues)
	{
		if (Queue)
		{
			FUnionResult Dummy;
			while (Queue->Dequeue(Dummy)) {} 
		}
	}
	SlotSubtractQueues.Empty();
   
	// Clear flags.
	SlotUnionActiveFlags.Empty();
	 
	SlotSubtractActiveFlags.Empty(); 
	 
	SlotUnionWorkerCounts.Empty();   

	SlotSubtractWorkerCounts.Empty();
}

int32 FRealtimeBooleanProcessor::FindLeastBusySlot() const
{
	int32 BestSlot = 0;
	int32 MinScore = INT32_MAX;

	for (int32 i = 0; i < NumSlots; i++)
	{
		// Score based on active worker counts.
		int32 Score = SlotUnionWorkerCounts[i]->load() + SlotSubtractWorkerCounts[i]->load() * 2;

		if (Score < MinScore)
		{
			MinScore = Score;
			BestSlot = i;
		}
	}

	return BestSlot;
}

void FRealtimeBooleanProcessor::KickUnionWorker(int32 SlotIndex)
{
	// Dequeue first (safe on GameThread only).
	FBulletHoleBatch Batch;
	if (!SlotUnionQueues[SlotIndex]->Dequeue(Batch))
	{
		return;  // Queue is empty.
	} 

	// Check per-slot worker limit.
	int32 Current = SlotUnionWorkerCounts[SlotIndex]->fetch_add(1);
	if (Current >= MaxUnionWorkerPerSlot)
	{
        SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);
		SlotUnionQueues[SlotIndex]->Enqueue(Batch);
		return;
	}
	
	// Acquire ThreadManager.
	URDMThreadManagerSubsystem* ThreadManager = GetThreadManager();
	if (!ThreadManager)
	{
        SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);
		SlotUnionQueues[SlotIndex]->Enqueue(MoveTemp(Batch));
		return;
	}

	
	// 4. Start worker (capture batch).
	UE_LOG(LogTemp, Log, TEXT("[Slot %d] Union Worker Started: %d / %d"),
		SlotIndex, SlotUnionWorkerCounts[SlotIndex]->load() , MaxUnionWorkerPerSlot);

	TSharedPtr<FProcessorLifeTime, ESPMode::ThreadSafe> LifeTimeToken = LifeTime;
	ThreadManager->RequestWork(
		[LifeTimeToken, SlotIndex, Batch = MoveTemp(Batch)]() mutable
		{
			if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
			{
				return;
			}
			if (auto Processor = LifeTimeToken->Processor.Pin())
			{
				Processor->ProcessSlotUnionWork(SlotIndex, MoveTemp(Batch));
			}			
		},
		OwnerComponent.Get()
	);	
}

void FRealtimeBooleanProcessor::KickSubtractWorker(int32 SlotIndex)
{
	// Dequeue first (safe on GameThread only).
	FUnionResult UnionResult;
	if (!SlotSubtractQueues[SlotIndex]->Dequeue(UnionResult))
	{
		return;  // Queue is empty.
	}

	// Reserve per-slot worker thread.
	int32 Current = SlotSubtractWorkerCounts[SlotIndex]->fetch_add(1);
	if (Current >= MaxSubtractWorkerPerSlot)
	{
		// At max -> rollback and re-enqueue UnionResult.
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
		SlotSubtractQueues[SlotIndex]->Enqueue(MoveTemp(UnionResult));
		return;
	}

	// Acquire ThreadManager.
	URDMThreadManagerSubsystem* ThreadManager = GetThreadManager();
	if (!ThreadManager)
	{ 
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
		SlotSubtractQueues[SlotIndex]->Enqueue(MoveTemp(UnionResult));
		return;
	}

	// Start worker (capture UnionResult).
	UE_LOG(LogTemp, Log, TEXT("[Slot %d] Subtract Worker Started: %d / %d"),
		SlotIndex, SlotSubtractWorkerCounts[SlotIndex]->load(), MaxSubtractWorkerPerSlot);

	TSharedPtr<FProcessorLifeTime, ESPMode::ThreadSafe> LifeTimeToken = LifeTime;
	if (OwnerComponent.IsValid() && !OwnerComponent->CheckAndSetChunkBusy(UnionResult.ChunkIndex))
	{
		ThreadManager->RequestWork(
		   [LifeTimeToken, SlotIndex, UnionResult = MoveTemp(UnionResult)]() mutable
		   {
		   	if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
		   	{
		   		return;
		   	}
		   	if (auto Processor = LifeTimeToken->Processor.Pin())
		   	{
		   		Processor->ProcessSlotSubtractWork(SlotIndex, MoveTemp(UnionResult));
		   	}
		   },
		   OwnerComponent.Get()
	   );
	}
	else
	{
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
		UE_LOG(LogTemp, Warning, TEXT("Re-enqueued due to ChunkBusy! ChunkIndex=%d, Context=%p"),
			UnionResult.ChunkIndex, UnionResult.IslandContext.Get());
		SlotSubtractQueues[SlotIndex]->Enqueue(MoveTemp(UnionResult));
	}
}

void FRealtimeBooleanProcessor::ProcessSlotUnionWork(int32 SlotIndex, FBulletHoleBatch&& Batch)
{
	// Batch is passed as a parameter (no dequeue).
	int32 ChunkIndex = Batch.ChunkIndex;
	if (ChunkIndex == INDEX_NONE)
	{
		SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);
		return;
	}

	// Perform union (no chunk mesh access; tool meshes only).
	FDynamicMesh3 CombinedToolMesh;
	TArray<TWeakObjectPtr<UDecalComponent>> Decals;
	int32 UnionCount = 0;

	int32 BatchCount = Batch.Num();
	TArray<FTransform> ToolTransforms = MoveTemp(Batch.ToolTransforms);
	TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
	TArray<TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(
		Batch.ToolMeshPtrs);

	bool bIsFirst = true;
	for (int32 i = 0; i < BatchCount; ++i)
	{
		if (!ToolMeshPtrs[i].IsValid())
		{
			continue;
		}

		FTransform ToolTransform = MoveTemp(ToolTransforms[i]);
		TWeakObjectPtr<UDecalComponent> TemporaryDecal = MoveTemp(TemporaryDecals[i]);

		// Skip empty meshes (avoid crash).
		FDynamicMesh3 CurrentTool = *(ToolMeshPtrs[i]);
		if (CurrentTool.TriangleCount() == 0)
		{
			UE_LOG(LogTemp, Warning,
			       TEXT(
				       "[UnionWorkerForChunk] Skipping empty ToolMesh at ChunkIndex %d, item %d"
			       ), ChunkIndex, i);
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

			bool bUnionSuccess = false;
			{
#if !UE_BUILD_SHIPPING
				TRACE_CPUPROFILER_EVENT_SCOPE("SlotWorkerUnion_Union");
#endif
				bUnionSuccess = MeshUnion.Compute();
			}

			if (bUnionSuccess)
			{
				CombinedToolMesh = MoveTemp(UnionResult);
				UnionCount++;

				UE_LOG(LogTemp, Display, TEXT("ToolMeshTri %d"), CombinedToolMesh.TriangleCount());
				
			}
			else
			{
				UE_LOG(LogTemp, Warning,
				       TEXT("[UnionWorkerForChunk] Union failed at ChunkIndex %d, item %d"),
				       ChunkIndex, i);
			}
		}
	}

	if (UnionCount > 0 && CombinedToolMesh.TriangleCount() > 0)
	{
		FUnionResult Result;
		//Result.BatchID = BatchID;
		Result.PendingCombinedToolMesh = MoveTemp(CombinedToolMesh);
		Result.Decals = MoveTemp(Decals);
		Result.UnionCount = UnionCount;
		Result.ChunkIndex = ChunkIndex;
		// 배치 완료 추적용 ID 배열 복사
		Result.CompletionBatchIds = MoveTemp(Batch.CompletionBatchIds);

		// Enqueue into subtract queue.
		SlotSubtractQueues[SlotIndex]->Enqueue(MoveTemp(Result));
	}

	SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);

	// Kick subtract (on GameThread).
	AsyncTask(ENamedThreads::GameThread, [LifeTimeToken = LifeTime, SlotIndex]()
	{
		if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
		{
			return;
		}

		if (auto Processor = LifeTimeToken->Processor.Pin())
		{
			// If queue has work, kick again.
			//if (!Processor->SlotUnionQueues[SlotIndex]->IsEmpty())
			//{
			//	Processor->KickUnionWorker(SlotIndex);
			//}

			for (int32 i = 0; i < Processor->SlotUnionQueues.Num(); i++)
			{
				if (!Processor->SlotUnionQueues[i]->IsEmpty())
				{
					Processor->KickUnionWorker(i);
				}
			}
			// Kick subtract worker too.
			//Processor->KickSubtractWorker(SlotIndex);
			 for (int32 i = 0; i < Processor->SlotSubtractQueues.Num(); i++)
              {
                  if (!Processor->SlotSubtractQueues[i]->IsEmpty())
                  {
                      Processor->KickSubtractWorker(i);
                  }
              }
		}
	});
}

void FRealtimeBooleanProcessor::ProcessSlotSubtractWork(int32 SlotIndex, FUnionResult&& UnionResult)
{
	auto HandleFailureAndReturn = [&]()
	{
		int32 ChunkIndex = UnionResult.ChunkIndex;
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);

		AsyncTask(ENamedThreads::GameThread, [LifeTimeToken = LifeTime,
			          SlotIndex = SlotIndex,
			          ChunkIndex = ChunkIndex,
			          Context = UnionResult.IslandContext,
			          CompletionBatchIds = UnionResult.CompletionBatchIds]()  // 배치 완료 추적용 캡처
		          {
			          if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
			          {
				          return;
			          }

			          TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> Processor = LifeTimeToken->Processor.Pin();
			          if (!Processor.IsValid())
			          {
				          return;
			          }

			          URealtimeDestructibleMeshComponent* Owner = Processor->OwnerComponent.Get();
			          if (!Owner)
			          {
				          return;
			          }

			          // Release chunk busy bit
			          Owner->ClearChunkBusy(ChunkIndex);

			          // 실패해도 배치 완료 추적: 모든 BatchId에 대해 완료 알림
			          for (int32 BatchId : CompletionBatchIds)
			          {
				          Owner->NotifyBooleanCompleted(BatchId);
			          }

			          if (Context.IsValid())
			          {
				          if (Context->RemainingTaskCount.fetch_sub(1) == 1)
				          {
					          if (Context->AccumulatedDebrisMesh.TriangleCount() > 0)
					          {
						          TArray<UMaterialInterface*> Materials;
						          if (auto ChunkMesh = Owner->GetChunkMeshComponent(ChunkIndex))
						          {
							          for (int32 i = 0; i < ChunkMesh->GetNumMaterials(); i++)
							          {
								          Materials.Add(ChunkMesh->GetMaterial(i));
							          }
						          }

						          // TargetDebrisActor가 있으면 기존 Actor에 메시 적용 (Client extraction)
						          if (Context->TargetDebrisActor.IsValid())
						          {
									  Owner->SpawnDebrisActor(MoveTemp(Context->AccumulatedDebrisMesh), Materials, Context->TargetDebrisActor.Get());

						          }
						          // 그 외: 새 DebrisActor 스폰 (Standalone/Listen Server)
						          else if (Context->Owner.IsValid())
						          {
					          Context->Owner->SpawnDebrisActor(MoveTemp(Context->AccumulatedDebrisMesh), Materials);
				          }
			          }

					          // IslandRemoval 완료 후 작은 파편 정리
					          if (Context->DisconnectedCellsForCleanup.Num() > 0 && Context->Owner.IsValid())
					          {
						          Context->Owner->CleanupSmallFragments(Context->DisconnectedCellsForCleanup);
					          }
				          }

				          // IslandRemoval 카운터 감소 (Boolean 배치 완료와 조율용)
				          if (Context->Owner.IsValid())
				          {
					          Context->Owner->DecrementIslandRemovalCount();
				          }
			          }

					  for (int i = 0; i < Processor->SlotSubtractQueues.Num(); ++i)
					  {
						  if (!Processor->SlotSubtractQueues[i]->IsEmpty())
						  {
							  Processor->KickSubtractWorker(i);
						  }
					  }
		          });
	};

	// UnionResult passed as parameter (no dequeue).
	int32 ChunkIndex = UnionResult.ChunkIndex;
	if (ChunkIndex == INDEX_NONE)
	{
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
		// If queue has work, re-kick.
		if (!SlotSubtractQueues[SlotIndex]->IsEmpty())
		{
			KickSubtractWorker(SlotIndex);
		}
		return;
	}

	// ===== 4. Subtract compute =====
	FDynamicMesh3 ResultMesh;
	bool bSuccess = false; 
	bool bHasDebris = false; 
	{	
		// Fetch chunk mesh.
		FDynamicMesh3 WorkMesh;
		if (!OwnerComponent->GetChunkMesh(WorkMesh, ChunkIndex))
		{
			HandleFailureAndReturn();
			return;
		}		

		if (WorkMesh.TriangleCount() == 0)
		{
			HandleFailureAndReturn();
			return;
		}

		if (UnionResult.WorkType == EBooleanWorkType::BulletHole)
		{
			if (UnionResult.PendingCombinedToolMesh.TriangleCount() == 0)
			{
				// SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
				// if (!SlotSubtractQueues[SlotIndex]->IsEmpty())
				// {
				// 	KickSubtractWorker(SlotIndex);
				// }

				HandleFailureAndReturn();
				return;
			}

			// Run subtract.
			FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();

			double CurrentSubtractDurationMs = FPlatformTime::Seconds();

			{
#if !UE_BUILD_SHIPPING
				TRACE_CPUPROFILER_EVENT_SCOPE("SlotWorkerUnion_Subtract");
#endif
				bSuccess = ApplyMeshBooleanAsync(
				   &WorkMesh,
				   &UnionResult.PendingCombinedToolMesh,
				   &ResultMesh,
				   EGeometryScriptBooleanOperation::Subtract,
				   Options);
			}

			CurrentSubtractDurationMs = (FPlatformTime::Seconds() - CurrentSubtractDurationMs) * 1000.0;

			if (bSuccess)
			{
				AccumulateSubtractDuration(ChunkIndex, CurrentSubtractDurationMs);     
				UpdateUnionSize(ChunkIndex, CurrentSubtractDurationMs);
				// Simplify.
				bool bEnableDetailMode = OwnerComponent->IsHighDetailMode();
				{
#if !UE_BUILD_SHIPPING
					TRACE_CPUPROFILER_EVENT_SCOPE("SlotWorkerUnion_Simplify");
#endif
					TrySimplify(ResultMesh, ChunkIndex, UnionResult.UnionCount, bEnableDetailMode);
				}
			}
			else
			{
				UE_LOG(LogTemp, Display, TEXT("Reset Accumulation"));
				// Reset accumulation on failure.
				FChunkState& State = ChunkStates.GetState(ChunkIndex);
				State.SubtractDurationAccum = 0;
				State.DurationAccumCount = 0;
			}
		}
		else
		{
			FGeometryScriptMeshBooleanOptions Ops;
			Ops.bFillHoles = true;
			Ops.bSimplifyOutput = false;

			// Intersection (Debris): 원본 크기 DebrisToolMesh 사용
			if (UnionResult.DebrisSharedToolMesh.IsValid() && UnionResult.IslandContext.IsValid())
			{
				FDynamicMesh3 DebrisTool = *UnionResult.DebrisSharedToolMesh;
				FDynamicMesh3 Debris;

				UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Intersection START - WorkMesh Tris=%d, DebrisTool Tris=%d"),
					WorkMesh.TriangleCount(), DebrisTool.TriangleCount());

				bool bSuccessIntersection = ApplyMeshBooleanAsync(
					&WorkMesh,
					&DebrisTool,
					&Debris,
					EGeometryScriptBooleanOperation::Intersection,
					Ops);

				UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Intersection RESULT - bSuccess=%d, Debris Tris=%d"),
					bSuccessIntersection ? 1 : 0, Debris.TriangleCount());

				if (bSuccessIntersection && Debris.TriangleCount() > 0)
				{
					FScopeLock Lock(&UnionResult.IslandContext->MeshLock);
					// Initialize attributes
					if (UnionResult.IslandContext->AccumulatedDebrisMesh.TriangleCount() == 0)
					{
						UnionResult.IslandContext->AccumulatedDebrisMesh.EnableAttributes();
						UnionResult.IslandContext->AccumulatedDebrisMesh.Attributes()->EnableMaterialID();
						if (!UnionResult.IslandContext->AccumulatedDebrisMesh.HasTriangleGroups())
						{
							UnionResult.IslandContext->AccumulatedDebrisMesh.EnableTriangleGroups();
						}
					}

					FDynamicMeshEditor Editor(&UnionResult.IslandContext->AccumulatedDebrisMesh);
					FMeshIndexMappings Mappings;
					Editor.AppendMesh(&Debris, Mappings);
					bHasDebris = true;

					UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Accumulated Debris Tris=%d"),
						UnionResult.IslandContext->AccumulatedDebrisMesh.TriangleCount());
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Intersection SKIPPED - DebrisToolMesh=%d, IslandContext=%d"),
					UnionResult.DebrisSharedToolMesh.IsValid() ? 1 : 0, UnionResult.IslandContext.IsValid() ? 1 : 0);
			}

			// Subtract (구멍): 스케일된 SharedToolMesh 사용
			if (UnionResult.SharedToolMesh.IsValid())
			{
				FDynamicMesh3 LocalTool = *UnionResult.SharedToolMesh;
				bSuccess = ApplyMeshBooleanAsync(
					&WorkMesh,
					&LocalTool,
					&ResultMesh,
					EGeometryScriptBooleanOperation::Subtract,
					Ops);
			}
		} 
	}

	if (SlotSubtractWorkerCounts.IsValidIndex(SlotIndex))
	{
	 SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
	}
	
	// ===== 5. Apply results (GameThread) =====
	if (bSuccess || bHasDebris)
	{
		AsyncTask(ENamedThreads::GameThread,
		          [LifeTimeToken = LifeTime,
			          ChunkIndex,
			          SlotIndex,
			          ResultMesh = MoveTemp(ResultMesh),
			          Context = UnionResult.IslandContext,
			          Decals = MoveTemp(UnionResult.Decals),
			          UnionCount = UnionResult.UnionCount,
			          CompletionBatchIds = MoveTemp(UnionResult.CompletionBatchIds),
			          bSuccess]() mutable
		          {
			          if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
			          {
				          return;
			          }

		          	TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> Processor = LifeTimeToken->Processor.Pin();
					  if (!Processor.IsValid())
					  {
						  return;
					  }

		          	URealtimeDestructibleMeshComponent* WeakOwner = Processor->OwnerComponent.Get();
			          if (!WeakOwner)
			          {
				          return;
			          }			         

			          double StartTime = FPlatformTime::Seconds();

			          // Apply mesh.
			          if (bSuccess)
			          {
#if !UE_BUILD_SHIPPING
				          TRACE_CPUPROFILER_EVENT_SCOPE("SlotWorkerUnion_ApplyGT");
#endif
				          WeakOwner->ApplyBooleanOperationResult(MoveTemp(ResultMesh), ChunkIndex, true);
			          }

			          // 배치 완료 추적: 모든 BatchId에 대해 완료 알림
			          for (int32 BatchId : CompletionBatchIds)
			          {
				          WeakOwner->NotifyBooleanCompleted(BatchId);
			          }

			          double ExecutionDurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

			          Processor->UpdateSimplifyInterval(ExecutionDurationMs, ChunkIndex);

			          // Spawn debris or apply to existing DebrisActor (client extraction)
			          if (Context.IsValid())
			          {
				          int32 RemainingBefore = Context->RemainingTaskCount.load();
				          UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Completion Callback - RemainingTasks=%d, AccumulatedTris=%d, TargetDebrisActor=%d"),
					          RemainingBefore, Context->AccumulatedDebrisMesh.TriangleCount(),
					          Context->TargetDebrisActor.IsValid() ? 1 : 0);

				          if (Context->RemainingTaskCount.fetch_sub(1) == 1)
				          {
					          if (Context->AccumulatedDebrisMesh.TriangleCount() > 0)
					          {
						          TArray<UMaterialInterface*> Materials;
						          if (auto ChunkMesh = WeakOwner->GetChunkMeshComponent(1))
						          {
							          for (int32 i = 0; i < ChunkMesh->GetNumMaterials(); i++)
							          {
								          Materials.Add(ChunkMesh->GetMaterial(i));
							          }
						          }

						          // TargetDebrisActor가 있으면 기존 Actor에 메시 적용 (Client extraction)
						          if (Context->TargetDebrisActor.IsValid())
						          {
							          UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Calling ApplyMeshToDebrisActor with %d triangles"), Context->AccumulatedDebrisMesh.TriangleCount());
							          WeakOwner->SpawnDebrisActor(MoveTemp(Context->AccumulatedDebrisMesh), Materials, Context->TargetDebrisActor.Get());
							          UE_LOG(LogTemp, Warning, TEXT("[BooleanProcessor] Applied mesh to existing DebrisActor"));
						          }
						          // 그 외: 새 DebrisActor 스폰 (Standalone/Listen Server)
						          else if (Context->Owner.IsValid())
						          {
							          Context->Owner->SpawnDebrisActor(MoveTemp(Context->AccumulatedDebrisMesh), Materials);
						          }
					          }

					          // IslandRemoval 완료 후 작은 파편 정리
					          if (Context->DisconnectedCellsForCleanup.Num() > 0 && Context->Owner.IsValid())
					          {
						          Context->Owner->CleanupSmallFragments(Context->DisconnectedCellsForCleanup);
					          }

					          // IslandRemoval 카운터 감소 (Boolean 배치 완료와 조율용)
					          if (Context->Owner.IsValid())
					          {
						          Context->Owner->DecrementIslandRemovalCount();
					          }
				          }
			          }

			          // ===== Decrement counters (shutdown check) =====
			          // if (Processor->SlotSubtractWorkerCounts.IsValidIndex(SlotIndex))
			          // {
				         //  Processor->SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
			          // }

			          // Update counters.
			          Processor->ChunkGenerations[ChunkIndex].fetch_add(1);
			          Processor->ChunkHoleCount[ChunkIndex] += UnionCount;

		          	WeakOwner->ClearChunkBusy(ChunkIndex);
					UE_LOG(LogTemp, Warning, TEXT("ClearChunkBusy: ChunkIndex=%d, QueueEmpty=%d"),
						ChunkIndex, Processor->SlotSubtractQueues[SlotIndex]->IsEmpty());
				    // If queue has work, re-kick. 
					for (int32 i = 0; i < Processor->SlotSubtractQueues.Num(); i++)
					{
						if (!Processor->SlotSubtractQueues[i]->IsEmpty())
						{
							Processor->KickSubtractWorker(i);
						}
					}
				    // Next kick.
					Processor->KickProcessIfNeededPerChunk();
		          });
	}
	else
	{
		// Even on failure, re-kick if queue has work (GameThread).
		AsyncTask(ENamedThreads::GameThread, [LifeTimeToken = LifeTime, SlotIndex, ChunkIndex = ChunkIndex, CompletionBatchIds = MoveTemp(UnionResult.CompletionBatchIds)]()
		{
			if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
			{
				return;
			}

			TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> Processor = LifeTimeToken->Processor.Pin();
			if (!Processor.IsValid())
			{
				return;
			}

			URealtimeDestructibleMeshComponent* WeakOwner = Processor->OwnerComponent.Get();
			if (!WeakOwner)
			{
				return;
			}
			WeakOwner->ClearChunkBusy(ChunkIndex);

			// 실패해도 배치 완료 추적: 모든 BatchId에 대해 완료 알림
			for (int32 BatchId : CompletionBatchIds)
			{
				WeakOwner->NotifyBooleanCompleted(BatchId);
			}

			// if (Processor->SlotSubtractWorkerCounts.IsValidIndex(SlotIndex))
			// {
			// 	Processor->SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
			// }

			for (int32 i = 0; i < Processor->SlotSubtractQueues.Num(); i++)
			{
				if (!Processor->SlotSubtractQueues[i]->IsEmpty())
				{
					Processor->KickSubtractWorker(i);
				}
			}
		});
	}
}

void FRealtimeBooleanProcessor::CleanupSlotMapping(int32 SlotIndex)
{
	// Ensure union queue is empty too (both must be empty to clean up).
	bool bUnionEmpty = SlotUnionQueues[SlotIndex]->IsEmpty();
	bool bSubtractEmpty = SlotSubtractQueues[SlotIndex]->IsEmpty();

	if (!bUnionEmpty || !bSubtractEmpty)
	{
		return;  // Work still remaining.
	}
}

void FRealtimeBooleanProcessor::BooleanOpSync(FBulletHole&& Op)
{
	if (!OwnerComponent.IsValid())
	{
		return;
	}

	if (Op.ChunkIndex == INDEX_NONE)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE("BooleanSync");
	
	FDynamicMesh3 Result = {};
	FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();

	FDynamicMesh3 WorkMesh = *OwnerComponent->GetMesh();

	FDynamicMesh3 ToolMesh = MoveTemp(*Op.ToolMeshPtr.Get());
	MeshTransforms::ApplyTransform(ToolMesh, Op.ToolTransform, true);

	bool bBooleanSuccess = false;
	UE_LOG(LogTemp, Display, TEXT("BooleanSync"));
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("BooleanSync_Subtact");
		bBooleanSuccess = ApplyMeshBooleanAsync(
			&WorkMesh,
			&ToolMesh,
			&Result,
			EGeometryScriptBooleanOperation::Subtract,
			Options);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE("BooleanSync_Apply");
		if (bBooleanSuccess)
		{
			OwnerComponent->EditMesh([&](FDynamicMesh3 & InternalMesh)
			{
				InternalMesh = MoveTemp(Result);
			});

			OwnerComponent->ApplyCollisionUpdate(OwnerComponent.Get());
		}
	}
}

void FRealtimeBooleanProcessor::UpdateUnionSize(int32 ChunkIndex, double DurationMs)
{
	const int32 CurrentUnionCount = MaxUnionCount[ChunkIndex];
	int32 NextCount = CurrentUnionCount;
	
	if (DurationMs > FrameBudgetMs)
	{
		// Reduce by 70%.
		NextCount = FMath::FloorToInt(CurrentUnionCount * 0.7f);

		/*
		 * Clamp to at least 1 to protect the frame even if cost spikes.
		 */
		NextCount = FMath::Max(1, NextCount);
		
		UE_LOG(LogTemp, Display, TEXT("union size reduce %d to %d"), CurrentUnionCount, NextCount);
	}
	else if (DurationMs < (FrameBudgetMs * 0.6))
	{
		/*
		 * 1. 20 seems sufficient.
		 * 2. Profiling every mesh is unrealistic.
		 */
		NextCount = FMath::Min(CurrentUnionCount + 1, 20);

		UE_LOG(LogTemp, Display, TEXT("union size increase %d to %d"), CurrentUnionCount, NextCount);
	}

	if (NextCount != CurrentUnionCount)
	{
		MaxUnionCount[ChunkIndex] = NextCount;
	}
}

void FRealtimeBooleanProcessor::KickProcessIfNeededPerChunk()
{
	/*
	 * Build TMap by priority.
	 * Use chunk address as key to gather per-chunk ops.
	 */
	TMap<UDynamicMeshComponent*, FBulletHoleBatch> HighPriorityMap;
	TMap<UDynamicMeshComponent*, FBulletHoleBatch> NormalPriorityMap;

	/*
	 * TMap does not preserve order.
	 * Use arrays to preserve order.
	 */
	TArray<UDynamicMeshComponent*> HighPriorityOrder;
	TArray<UDynamicMeshComponent*> NormalPriorityOrder;

	

	// Pre-allocate some memory.
	HighPriorityOrder.Reserve(100);
	NormalPriorityOrder.Reserve(100);

	auto GatherOps = [&](TQueue<FBulletHole, EQueueMode::Mpsc>& Queue,
	                     TMap<UDynamicMeshComponent*, FBulletHoleBatch>& OpMap,
	                     TArray<UDynamicMeshComponent*>& OrderArray, int& DebugCount)
	{
		// Temporary overflow array for ops beyond union limit (for re-enqueue).
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

			// Use Find to avoid creating unnecessary keys.
			FBulletHoleBatch* Batch = OpMap.Find(TargetMesh);
			const int32 CurrentCount = Batch ? Batch->Num() : 0;

			// If union limit exceeded, store in overflow and re-enqueue.
			if (CurrentCount >= ChunkUnionLimit)
			{
				OverflowOps.Add(MoveTemp(Op));
			}
			else
			{
				if (!Batch)
				{
					// Record order.
					OrderArray.Add(TargetMesh);

					// Create map entry.
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

	{
#if !UE_BUILD_SHIPPING
		TRACE_CPUPROFILER_EVENT_SCOPE("GatherOps");
#endif
		GatherOps(HighPriorityQueue, HighPriorityMap, HighPriorityOrder, DebugHighQueueCount);
		GatherOps(NormalPriorityQueue, NormalPriorityMap, NormalPriorityOrder, DebugNormalQueueCount);
	}

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
				UE_LOG(LogTemp, Display, TEXT("ToolMeshTri/lamda %d/ %d"), Batch->Num(), Batch->ToolMeshPtrs[0].Get()->TriangleCount());
				 if (bEnableMultiWorkers)
				 {
				 	// Decide slot for this chunk.
				 	int32 TargetSlot = FindLeastBusySlot();
				 	
				 	// Enqueue into union queue.
				  	SlotUnionQueues[TargetSlot]->Enqueue(MoveTemp(*Batch));
				 	// Wake union worker.
				 	KickUnionWorker(TargetSlot);
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
				 			* Add logic to re-queue batches for busy chunks.
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
			// Safely clear the busy bit.
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

#if !UE_BUILD_SHIPPING
			TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync");
#endif
			TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> Processor = LifeTimeToken->Processor.Pin();
			if (!Processor.IsValid())
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
			// Copy target mesh.
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
			// 배치 완료 추적용 ID 배열 (다른 데이터 move 전에 먼저 추출)
			TArray<int32> CompletionBatchIds = MoveTemp(Batch.CompletionBatchIds);
			TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = MoveTemp(Batch.TemporaryDecals);
			TArray<FTransform> Transforms = MoveTemp(Batch.ToolTransforms);
			TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = MoveTemp(Batch.ToolMeshPtrs);

			int32 UnionCount = 0;
			bool bIsFirst = true;
			bool bCombinedValid = false;
			FDynamicMesh3 CombinedToolMesh;
			{
#if !UE_BUILD_SHIPPING
				TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Union");
#endif
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
#if !UE_BUILD_SHIPPING
					TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Subtract");
#endif
					if (CombinedToolMesh.TriangleCount() > 0)
					{
						FAxisAlignedBox3d ToolBounds = CombinedToolMesh.GetBounds();
						FAxisAlignedBox3d TargetBounds = WorkMesh.GetBounds();

						// Target mesh info (commented out to avoid log spam).
						// UE_LOG(LogTemp, Warning, TEXT("[Boolean Debug] CombinedToolMesh Center: %s, Size: %s"),
						// 	*FVector(ToolBounds.Center()).ToString(),
						// 	*FVector(ToolBounds.Extents()).ToString());
						//
						// UE_LOG(LogTemp, Warning, TEXT("[Boolean Debug] WorkMesh(Target) Center: %s, Size: %s"),
						// 	*FVector(TargetBounds.Center()).ToString(),
						// 	*FVector(TargetBounds.Extents()).ToString());
					}

					bSubtractSuccess = ApplyMeshBooleanAsync(&WorkMesh, &CombinedToolMesh, &ResultMesh,
					                                         EGeometryScriptBooleanOperation::Subtract, Options);
				}
				// Re-check processor validity (may be destroyed during async).
				if (!Processor.IsValid())
				{
					SafeClearBusyBit();
					return;
				}
				++Processor->ChunkGenerations[ChunkIndex];

				CurrentSubDuration = FPlatformTime::Seconds() - CurrentSubDuration;

				if (bSubtractSuccess)
				{
					// Apply boolean result for the number of unioned bullets.
					AppliedCount = UnionCount;
					WorkMesh = MoveTemp(ResultMesh);

					Processor->AccumulateSubtractDuration(ChunkIndex, CurrentSubDuration);

					{
						// Mesh simplification.
#if !UE_BUILD_SHIPPING
						TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Simplify");
#endif
						bool bEnableDetailMode = OwnerComponent->IsHighDetailMode();
						bool bIsSimplified = Processor->TrySimplify(WorkMesh, ChunkIndex, UnionCount, bEnableDetailMode);
				}

					
					Processor->UpdateUnionSize(ChunkIndex, CurrentSubDuration * 1000.0);
				}
				else
				{ 
					// Reset accumulation on failure.
					FChunkState& State = Processor->ChunkStates.GetState(ChunkIndex);
					State.SubtractDurationAccum = 0;
					State.DurationAccumCount = 0;
				}
			}

			AsyncTask(ENamedThreads::GameThread,
				[OwnerComponent, LifeTimeToken, Gen, ChunkIndex, Result = MoveTemp(WorkMesh), AppliedCount, DecalsToRemove = MoveTemp(DecalsToRemove), CompletionBatchIds = MoveTemp(CompletionBatchIds)]() mutable
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

					TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> Processor = LifeTimeToken->Processor.Pin();
					if (!Processor.IsValid())
					{
						return;
					}

					if (OwnerComponent->GetBooleanProcessor() != Processor.Get())
					{
						return;
					}

#if !UE_BUILD_SHIPPING
					TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_ApplyGT");
#endif

					if (AppliedCount > 0)
					{
						double CurrentSetMeshAvgCost = FPlatformTime::Seconds();
						{
#if !UE_BUILD_SHIPPING
							TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_SetMesh");
#endif
							OwnerComponent->ApplyBooleanOperationResult(MoveTemp(Result), ChunkIndex, false);
						}
						CurrentSetMeshAvgCost = CurrentSetMeshAvgCost - FPlatformTime::Seconds();

						Processor->UpdateSimplifyInterval(CurrentSetMeshAvgCost, ChunkIndex);

						for (const TWeakObjectPtr<UDecalComponent>& Decal : DecalsToRemove)
						{
							if (Decal.IsValid())
							{
								//Decal->DestroyComponent();
							}
						}
					}

					// 배치 완료 추적: 모든 BatchId에 대해 완료 알림
					for (int32 BatchId : CompletionBatchIds)
					{
						OwnerComponent->NotifyBooleanCompleted(BatchId);
					}

					Processor->ChunkHoleCount[ChunkIndex] += AppliedCount;

					Processor->KickProcessIfNeededPerChunk();
				});
		});
}

void FRealtimeBooleanProcessor::CancelAllOperations()
{
	SetMeshAvgCost.Reset();

	InitInterval = 0;

	FBulletHole Temp;
	while (HighPriorityQueue.Dequeue(Temp)) {}
	while (NormalPriorityQueue.Dequeue(Temp)) {}

	DebugHighQueueCount = 0;
	DebugNormalQueueCount = 0;

	// Reset hole count and caches.
	ChunkStates.Reset();
	ChunkHoleCount.Init(OwnerComponent->GetChunkNum(), 0);
}

void FRealtimeBooleanProcessor::AccumulateSubtractDuration(int32 ChunkIndex, double CurrentSubDuration)
{
	FChunkState& State = ChunkStates.GetState(ChunkIndex);
	// Accumulate time if above threshold.
	if (CurrentSubDuration >= SubDurationHighThreshold) 
	{
		State.SubtractDurationAccum += CurrentSubDuration;
		State.DurationAccumCount++;
		UE_LOG(LogTemp, Display, TEXT("Accumulate Duration %d"), State.DurationAccumCount);
	}
	// If previously accumulated but below threshold this tick, reset.
	else if (CurrentSubDuration < SubDurationHighThreshold && State.DurationAccumCount > 0)
	{
		State.SubtractDurationAccum = 0;
		State.DurationAccumCount = 0;
		UE_LOG(LogTemp, Display, TEXT("Accumulate Reset"));
	}
}

void FRealtimeBooleanProcessor::UpdateSimplifyInterval(double CurrentSetMeshAvgCost, int32 ChunkIndex)
{
	// 범위 체크: 비동기 작업 중 청크가 제거될 수 있음
	if (ChunkIndex < 0 || ChunkIndex >= SetMeshAvgCost.Num())
	{
		return;
	}

	if (FMath::IsNearlyZero(SetMeshAvgCost[ChunkIndex]))
	{
		SetMeshAvgCost[ChunkIndex] = CurrentSetMeshAvgCost;
		return;
	}

	const double OldAvgCost = SetMeshAvgCost[ChunkIndex];

	// Compute exponential moving average (EMA, like linear interpolation).
	const double NewAvgCost = FMath::Lerp(SetMeshAvgCost[ChunkIndex], CurrentSetMeshAvgCost, 0.1);
	SetMeshAvgCost[ChunkIndex] = NewAvgCost;

	// Reduction rate: (old - new) / old.
	const double ReductionRate = (OldAvgCost - NewAvgCost) / OldAvgCost;

	/*
	 * Tuning constants.
	 */
	 // Increase threshold; above this, reduce interval.
	constexpr double PanicThreshold = 0.1;
	// Stable threshold; at/above this, increase interval.
	constexpr double StableThreshold = 0.0;

	/*
	 * AIMD (Additive Increase, Multiplicative Decrease) used in TCP congestion control.
	 * Increase slowly and decrease quickly to react to cost spikes.
	 */

	 // Reduce interval when cost increases by >=10%.
	if (-ReductionRate > PanicThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Interval decrease %d to %lld"), MaxInterval[ChunkIndex], FMath::FloorToInt(MaxInterval[ChunkIndex] * 0.7));
		// Clamp lower bound to 15.
		MaxInterval[ChunkIndex] = FMath::Max(15, FMath::FloorToInt(MaxInterval[ChunkIndex] * 0.7));
	}
	// If cost decreased or flat, increase slowly.
	else if (ReductionRate >= StableThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Interval increase %d to %d"), MaxInterval[ChunkIndex], MaxInterval[ChunkIndex] + 1);
		MaxInterval[ChunkIndex] = FMath::Min(InitInterval * 2, MaxInterval[ChunkIndex] + 1);
	}
	// Watch and hold for 0-10% increase.
	// Keep else branch for logging.
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Interval hold"));
	}
}

bool FRealtimeBooleanProcessor::TrySimplify(UE::Geometry::FDynamicMesh3& WorkMesh, int32 ChunkIndex, int32 UnionCount, bool bEnableDetail)
{
	if (!FRDMCVarHelper::EnableSimplify())
	{
		UE_LOG(LogTemp, Display, TEXT("Simplify Off"));
		return false;
	}
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
		UE_LOG(LogTemp, Display, TEXT("Simplify/TriCount"));
		bShouldSimplify = true;
	}
	// After 2 accumulations, simplify if average exceeds threshold.
	else if (State.DurationAccumCount >= 2 &&
		State.SubtractDurationAccum / State.DurationAccumCount >= SubDurationHighThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Simplify/Duration"));
		bShouldSimplify = true;
	}
	// Simplify when interval reaches max.
	else if (State.Interval >= MaxInterval[ChunkIndex])
	{
		UE_LOG(LogTemp, Display, TEXT("Simplify/Interval"));
		bShouldSimplify = true;
	}

	if (bShouldSimplify)
	{
		State.Reset();	
		FGeometryScriptPlanarSimplifyOptions SimplifyOptions;
		// SimplifyOptions.bAutoCompact = true;
		SimplifyOptions.bAutoCompact = false;
		SimplifyOptions.AngleThreshold = AngleThreshold;
		ApplySimplifyToPlanarAsync(&WorkMesh, SimplifyOptions, bEnableDetail);
		/*
		 * As of 12/26, the runtime does not access LastSimplifyTriCount on the GT.
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
	 * Validate the index before calling.
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

	// Empty mesh check to avoid AABB tree build crash.
	if (TargetMesh->TriangleCount() == 0 || ToolMesh->TriangleCount() == 0)
	{
		return false;
	}

	using namespace UE::Geometry;

	// Expand to other operations if needed.
	FMeshBoolean::EBooleanOp Op = FMeshBoolean::EBooleanOp::Difference;
	switch (Operation)
	{
	case EGeometryScriptBooleanOperation::Subtract:
		Op = FMeshBoolean::EBooleanOp::Difference;
		break;
	case EGeometryScriptBooleanOperation::Intersection:
		Op = FMeshBoolean::EBooleanOp::Intersect;
		break;
	case EGeometryScriptBooleanOperation::Union:
		Op = FMeshBoolean::EBooleanOp::Union;
		break;
	default:
		Op = FMeshBoolean::EBooleanOp::Difference;
		break;
	}

	const int32 MaxAttempts = 2;
	for (int32 Attempt = 0; Attempt < MaxAttempts; Attempt++)
	{
		FTransform CurrentToolTransform = ToolTransform;

		// On first failure, jitter position/rotation and retry.
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

		const int32 InternalMaterialID = 1;

		// Mesh operation.
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
			// Enable attributes/material IDs on OutputMesh.
			if (!OutputMesh->HasAttributes())
			{
				OutputMesh->EnableAttributes();
			}
			if (!OutputMesh->Attributes()->HasMaterialID())
			{
				OutputMesh->Attributes()->EnableMaterialID();
			}

			// Enable PolyGroup layer if missing (preserve groups from boolean).
			if (!OutputMesh->HasTriangleGroups())
			{
				OutputMesh->EnableTriangleGroups();
			}

			FDynamicMeshMaterialAttribute* MaterialIDAttr = OutputMesh->Attributes()->GetMaterialID();

			// Assign using PolyGroup IDs.
			int32 ToolGroupID = 1; // Must match the ID set on the ToolMesh.
			for (int32 TriID : OutputMesh->TriangleIndicesItr())
			{
				if (OutputMesh->GetTriangleGroup(TriID) == ToolGroupID)
				{
					MaterialIDAttr->SetValue(TriID, InternalMaterialID);
				}
			}

			/*
				 * Weld open edges.
				*/
			if (MeshBoolean.CreatedBoundaryEdges.Num() > 0)
			{
				// Select open edges.
				TSet<int32> EdgeSet(MeshBoolean.CreatedBoundaryEdges);

				FMergeCoincidentMeshEdges Welder(OutputMesh);
				// Edges to weld.
				Welder.EdgesToMerge = &EdgeSet;
				/*
				 * Merge only 1:1 corresponding edges for each edge.
				 * If edge A has candidates B and C, pairs (A,B) or (A,C) form.
				 * Skip ambiguous cases where the merge target is unclear.
				 */
				Welder.OnlyUniquePairs = true;
				// Disable welding of attributes.
				Welder.bWeldAttrsOnMergedEdges = false;

				// Tolerance for matching vertices.
				Welder.MergeVertexTolerance = 0.001;
				// Search tolerance for merge candidates.
				Welder.MergeSearchTolerance = 0.001;

				Welder.Apply();
			}

			return true;
		}
		// Clear and retry on failure.
		OutputMesh->Clear();
	}

	UE_LOG(LogTemp, Warning, TEXT("[Boolean] All attempts failed."));
	return false;
}

void FRealtimeBooleanProcessor::ApplySimplifyToPlanarAsync(UE::Geometry::FDynamicMesh3* TargetMesh, FGeometryScriptPlanarSimplifyOptions Options, bool bEnableDetail)
{
	if (!TargetMesh)
	{
		return;
	}
	using namespace UE::Geometry;

	FQEMSimplification Simplifier(TargetMesh);	

	if (bEnableDetail)
	{
		if (!TargetMesh->HasAttributes())
		{
			TargetMesh->EnableAttributes();
		}

		if (FRDMCVarHelper::GetSimplifyMode() == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("HighDetail"));
			MeshClusterSimplify::FSimplifyOptions SimplifyOptions;
			SimplifyOptions.TargetEdgeLength = 1.0;
			SimplifyOptions.PreserveEdges.PolyGroup = MeshClusterSimplify::FSimplifyOptions::EConstraintLevel::Constrained;
			SimplifyOptions.PreserveEdges.UVSeam = MeshClusterSimplify::FSimplifyOptions::EConstraintLevel::Constrained;
			SimplifyOptions.PreserveEdges.Material = MeshClusterSimplify::FSimplifyOptions::EConstraintLevel::Constrained;
			SimplifyOptions.bTransferAttributes = true;
			SimplifyOptions.bTransferGroups = true;

			FDynamicMesh3 SimplifiedMesh;
			if (MeshClusterSimplify::Simplify(*TargetMesh, SimplifiedMesh, SimplifyOptions))
			{
				*TargetMesh = MoveTemp(SimplifiedMesh);
			}
		}

		if (FRDMCVarHelper::GetSimplifyMode() == 1)
		{
			FDynamicMeshAttributeSet* Attributes = TargetMesh->Attributes();
			const bool bHasMaterialID = Attributes && Attributes->HasMaterialID();

			const FDynamicMeshMaterialAttribute* MaterialID = bHasMaterialID ? Attributes->GetMaterialID() : nullptr;
			const FDynamicMeshUVOverlay* PrimaryUV = (Attributes) ? Attributes->PrimaryUV() : nullptr;

			// Flag to protect seam/material boundaries on the surface (MaterialID = 0).
			// Disabled when no material IDs exist (surface detection not possible).
			const bool bSurfaceOnlyProtection = bHasMaterialID;
		
			const int32 SurfacematerialID = 0;

			// Lambda to check whether an edge belongs to surface (MatID = 0) triangles.
			// Assumes it is only called when bSurfaceOnlyProtection is true.
			auto EdgeTouchesSurface = [&](int32 EdgeID) -> bool
			{
				const FIndex2i EdgeTris = TargetMesh->GetEdgeT(EdgeID);
				if (EdgeTris.A >= 0 && MaterialID->GetValue(EdgeTris.A) == SurfacematerialID)
				{
					return true;
				}

				if (EdgeTris.B >= 0 && MaterialID->GetValue(EdgeTris.B) == SurfacematerialID)
				{
					return true;
				}

				return false;
			};

			/*
			 * Constraint setup: protect the following edges on surface (MatID = 0).
			 * - UV Seam Edge (Primary UV Seam)
			 * - MaterialID Boundary Edge
			 * - Boundary edges (open edges) are protected by the global Simplifier.MeshBoundaryConstraint flag
			 * - Interior (MatID = 1) allows seam collapse
			 */
			TOptional<FMeshConstraints> ExternalConstraints;
			ExternalConstraints.Emplace();
			FMeshConstraints& Constraints = ExternalConstraints.GetValue();
			const FEdgeConstraint NoCollapseEdge(EEdgeRefineFlags::NoCollapse);

			// Surface protection logic runs only when MatID exists.
			if (bSurfaceOnlyProtection)
			{
				// Iterate all edges and select protected ones.
				for (int32 EdgeID : TargetMesh->EdgeIndicesItr())
				{
					// Skip boundary edges; protected by global flag.
					if (TargetMesh->IsBoundaryEdge(EdgeID))
					{
						continue;
					}

					// Skip non-surface edges.
					if (!EdgeTouchesSurface(EdgeID))
					{
						continue;
					}

					// Protect UV seam edges.
					if (PrimaryUV && PrimaryUV->IsSeamEdge(EdgeID))
					{
						Constraints.SetOrUpdateEdgeConstraint(EdgeID, NoCollapseEdge);
						continue;
					}

					// Protect edges with different materials (material boundary).
					const FIndex2i EdgeTris = TargetMesh->GetEdgeT(EdgeID);
					if (EdgeTris.A >= 0 && EdgeTris.B >= 0)
					{
						const int32 MatA = MaterialID->GetValue(EdgeTris.A);
						const int32 MatB = MaterialID->GetValue(EdgeTris.B);
						if (MatA != MatB)
						{
							Constraints.SetOrUpdateEdgeConstraint(EdgeID, NoCollapseEdge);
						}
					}
				}
			}

			// Protect boundary edges.
			// Boundary edge: edge with only one adjacent triangle.
			Simplifier.MeshBoundaryConstraint = EEdgeRefineFlags::NoCollapse;

			// Allow global seam collapse for interior simplification; surface seams are constrained externally.
			Simplifier.bAllowSeamCollapse = true;

			/*	 
			 * Simplify merges two vertices and must choose a new vertex position.
			 * MinimalExistingVertexError picks an existing vertex as the merged position.
			 * Other modes create new positions; repeated use can drift away from the surface.
			 * This drift is called positional drift.
			 */
			Simplifier.CollapseMode = FQEMSimplification::ESimplificationCollapseModes::MinimalQuadricPositionError;

			Simplifier.SetExternalConstraints(MoveTemp(ExternalConstraints));

			Simplifier.SimplifyToMinimalPlanar(FMath::Max(0.001, Options.AngleThreshold));
		}
	}
	else
	{
		Simplifier.CollapseMode = FQEMSimplification::ESimplificationCollapseModes::AverageVertexPosition;

		Simplifier.SimplifyToMinimalPlanar(FMath::Max(0.001, Options.AngleThreshold));
	}

	if (Options.bAutoCompact)
	{
		TargetMesh->CompactInPlace();
	}
}

void FRealtimeBooleanProcessor::ApplyUniformRemesh(FDynamicMesh3* TargetMesh, double TargetEdgeLength, int32 NumPasses)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_ApplyUniformRemesh)

	if (!TargetMesh || TargetMesh->TriangleCount() == 0)
	{
		return;
	}

	FRemesher Remesher(TargetMesh);
	Remesher.SetTargetEdgeLength(TargetEdgeLength);

	// Constrain boundary edges to preserve mesh silhouette.
	TOptional<FMeshConstraints> ExternalConstraints;
	ExternalConstraints.Emplace();

	FMeshConstraintsUtil::ConstrainAllBoundariesAndSeams(
		ExternalConstraints.GetValue(),
		*TargetMesh,
		EEdgeRefineFlags::FullyConstrained,   // MeshBoundaryConstraint
		EEdgeRefineFlags::NoConstraint,        // GroupBoundaryConstraint
		EEdgeRefineFlags::NoConstraint,        // MaterialBoundaryConstraint
		true,   // bAllowSeamSplits
		true,   // bAllowSeamSmoothing
		false,  // bAllowSeamCollapse
		true    // bParallel
	);

	Remesher.SetExternalConstraints(MoveTemp(ExternalConstraints));

	for (int32 i = 0; i < NumPasses; ++i)
	{
		Remesher.BasicRemeshPass();
	}

	TargetMesh->CompactInPlace();
}