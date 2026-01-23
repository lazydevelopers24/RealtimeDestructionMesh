// Copyright (c) 2026 Lazy Developers <lazydeveloper24@gmail.com>. All rights reserved.
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
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/RDMThreadManagerSubsystem.h"

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

	int32 ChunkNum = OwnerComponent->GetChunkNum();
	if (ChunkNum > 0)
	{
		ChunkGenerations.SetNumZeroed(ChunkNum);
		ChunkStates.Initialize(ChunkNum);
		ChunkHoleCount.SetNumZeroed(ChunkNum);

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
	LifeTime->bAlive.store(true);
	LifeTime->Processor.store(this);

	AngleThreshold = OwnerComponent->GetAngleThreshold();
	SubDurationHighThreshold = OwnerComponent->GetSubtractDurationLimit();
	InitInterval = OwnerComponent->GetInitInterval();
	MaxInterval = InitInterval;

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

void FRealtimeBooleanProcessor::EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal, UDynamicMeshComponent* ChunkMesh)
{
	if (!OwnerComponent.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("OwnerComponent is invalid"));
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

void FRealtimeBooleanProcessor::EnqueueIslandRemoval(
	int32 ChunkIndex,
	TSharedPtr<UE::Geometry::FDynamicMesh3> ToolMesh,
	TSharedPtr<FIslandRemovalContext> Context)
{
	if (ChunkIndex == INDEX_NONE)
	{
		return;
	}

	if (!ToolMesh.IsValid())
	{
		return;
	}

	FUnionResult WorkItem = {};
	WorkItem.ChunkIndex = ChunkIndex;
	WorkItem.SharedToolMesh = ToolMesh;
	WorkItem.OutDebrisMesh = MakeShared<FDynamicMesh3>();
	WorkItem.WorkType = EBooleanWorkType::IslandRemoval;
	WorkItem.IslandContext = Context;
	
	WorkItem.Decals = {};
	WorkItem.PendingCombinedToolMesh = {};
	WorkItem.UnionCount = 0;

	int32 SlotIndex = RouteToSlot(ChunkIndex);
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

	ChunkToSlotMap.Empty(); 
	 
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
	  
	ChunkToSlotMap.Empty(); 
}

int32 FRealtimeBooleanProcessor::RouteToSlot(int32 ChunkIndex)
{
	//FScopeLock Lock(&MapLock); 
	// Check for an existing slot mapping.
	if (int32* ExistingSlot = ChunkToSlotMap.Find(ChunkIndex))
	{
		return *ExistingSlot;
	}

	// Or choose the least busy slot.
	int32 TargetSlot = FindLeastBusySlot();
	ChunkToSlotMap.Add(ChunkIndex, TargetSlot);

	return TargetSlot;
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
		[LifeTimeToken, SlotIndex, Batch = MoveTemp(Batch), this]() mutable
		{
			ProcessSlotUnionWork(SlotIndex, MoveTemp(Batch));
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
	ThreadManager->RequestWork(
		[LifeTimeToken, SlotIndex, UnionResult = MoveTemp(UnionResult), this]() mutable
		{
			ProcessSlotSubtractWork(SlotIndex, MoveTemp(UnionResult));
		},
		OwnerComponent.Get()
	);
}

void FRealtimeBooleanProcessor::ProcessSlotUnionWork(int32 SlotIndex, FBulletHoleBatch&& Batch)
{
	TRACE_CPUPROFILER_EVENT_SCOPE("ProcessSlotUnionWork");

	// Validity check.
	if (!LifeTime.IsValid() || !LifeTime->bAlive.load())
	{
		SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);
		return;
	}

	if (!OwnerComponent.IsValid())
	{
		SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);
		return;
	}

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

	 		if (MeshUnion.Compute())
	 		{
	 			CombinedToolMesh = MoveTemp(UnionResult);
	 			UnionCount++;
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

		// Enqueue into subtract queue.
		SlotSubtractQueues[SlotIndex]->Enqueue(MoveTemp(Result)); 
	}

	SlotUnionWorkerCounts[SlotIndex]->fetch_sub(1);

	// Kick subtract (on GameThread).
	AsyncTask(ENamedThreads::GameThread, [this, SlotIndex]()
	{
		// If queue has work, kick again.
		if (!SlotUnionQueues[SlotIndex]->IsEmpty())
		{
			KickUnionWorker(SlotIndex);
		}
		// Kick subtract worker too.
		KickSubtractWorker(SlotIndex);
	});
}

void FRealtimeBooleanProcessor::ProcessSlotSubtractWork(int32 SlotIndex, FUnionResult&& UnionResult)
{
	TRACE_CPUPROFILER_EVENT_SCOPE("ProcessSlotSubtractWork");

	auto HandleFailureAndReturn = [&]()
	{
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);

		if (UnionResult.IslandContext.IsValid())
		{
			if (UnionResult.IslandContext->RemainingTaskCount.fetch_sub(1) == 1)
			{
				AsyncTask(ENamedThreads::GameThread, [Context = UnionResult.IslandContext, WeakOwner = OwnerComponent]()
				{
					TArray<UMaterialInterface*> Materials;
					if (auto ChunkMesh = WeakOwner->GetChunkMeshComponent(1))
					{
						for (int32 i = 0; i < ChunkMesh->GetNumMaterials(); i++)
						{
							Materials.Add(ChunkMesh->GetMaterial(i));
						}
					}

					Context->Owner->SpawnDebrisActor(MoveTemp(Context->AccumulatedDebrisMesh), Materials);
				});
			}
		}

		if (!SlotSubtractQueues[SlotIndex]->IsEmpty())
		{
			KickSubtractWorker(SlotIndex);
		}
	};

	// Validity check.
	if (!LifeTime.IsValid() || !LifeTime->bAlive.load())
	{
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
		return;
	}

	if (!OwnerComponent.IsValid())
	{
		SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
		return;
	}

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
	bool bOverBudget = false;
	bool bHasDebris = false;
	double StartFrameTimeMs = FPlatformTime::Seconds();
	{
		TRACE_CPUPROFILER_EVENT_SCOPE("SlotSubtract_Compute");

		// Fetch chunk mesh.
		FDynamicMesh3 WorkMesh;
		if (!OwnerComponent->GetChunkMesh(WorkMesh, ChunkIndex))
		{
			// SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
			// if (!SlotSubtractQueues[SlotIndex]->IsEmpty())
			// {
			// 	KickSubtractWorker(SlotIndex);
			// }
			//
			// if (UnionResult.IslandContext.IsValid())
			// {
			// 	UnionResult.IslandContext->RemainingTaskCount.fetch_sub(1);
			// }
			HandleFailureAndReturn();
			return;
		}

		if (WorkMesh.TriangleCount() == 0)
		{
			// SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
			// if (!SlotSubtractQueues[SlotIndex]->IsEmpty())
			// {
			// 	KickSubtractWorker(SlotIndex);
			// }
			//
			// if (UnionResult.IslandContext.IsValid())
			// {
			// 	UnionResult.IslandContext->RemainingTaskCount.fetch_sub(1);
			// }
			HandleFailureAndReturn();
			return;
		}

		if (UnionResult.WorkType == EBooleanWorkType::BulletHole)
		{
			if (UnionResult.PendingCombinedToolMesh.TriangleCount() == 0)
			{
				SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
				if (!SlotSubtractQueues[SlotIndex]->IsEmpty())
				{
					KickSubtractWorker(SlotIndex);
				}
				return;
			}

			// Run subtract.
			FGeometryScriptMeshBooleanOptions Options = OwnerComponent->GetBooleanOptions();

			double CurrentDurationMs = FPlatformTime::Seconds();

			bSuccess = ApplyMeshBooleanAsync(
				&WorkMesh,
				&UnionResult.PendingCombinedToolMesh,
				&ResultMesh,
				EGeometryScriptBooleanOperation::Subtract,
				Options);

			CurrentDurationMs = (FPlatformTime::Seconds() - CurrentDurationMs) * 1000.0;

			if (bSuccess)
			{
				AccumulateSubtractDuration(ChunkIndex, CurrentDurationMs);
				UpdateUnionSize(ChunkIndex, CurrentDurationMs);
				// Simplify.
				TrySimplify(ResultMesh, ChunkIndex, UnionResult.UnionCount);
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
			if (UnionResult.SharedToolMesh.IsValid())
			{
				FGeometryScriptMeshBooleanOptions Ops;
				Ops.bFillHoles = true;
				Ops.bSimplifyOutput = false;

				FDynamicMesh3 LocalTool = *UnionResult.SharedToolMesh;				

				// intersection
				FDynamicMesh3 Debris;
				bool bSuccessIntersection = ApplyMeshBooleanAsync(
					&WorkMesh,
					&LocalTool,
					&Debris,
					EGeometryScriptBooleanOperation::Intersection,
					Ops);

				if (bSuccessIntersection && Debris.TriangleCount() > 0)
				{
					if (UnionResult.IslandContext.IsValid())
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
					}
				}

				// subtract
				bSuccess = ApplyMeshBooleanAsync(
					&WorkMesh,
					&LocalTool,
					&ResultMesh,
					EGeometryScriptBooleanOperation::Subtract,
					Ops);
			}
		}

		double FrameDurationMs = (FPlatformTime::Seconds() - StartFrameTimeMs) * 1000.0;
		bOverBudget = (FrameDurationMs > FrameBudgetMs);
	}

	// ===== 5. Apply results (GameThread) =====
	if (bSuccess || bHasDebris)
	{
		AsyncTask(ENamedThreads::GameThread,
		          [WeakOwner = OwnerComponent,
			          LifeTimeToken = LifeTime,
			          ChunkIndex,
			          SlotIndex,
			          ResultMesh = MoveTemp(ResultMesh),
			          Context = UnionResult.IslandContext,
			          Decals = MoveTemp(UnionResult.Decals),
			          UnionCount = UnionResult.UnionCount,
			          bOverBudget,
			          bSuccess,
			          this]() mutable
		          {
			          if (!WeakOwner.IsValid())
			          {
				          return;
			          }

			          if (!LifeTimeToken.IsValid() || !LifeTimeToken->bAlive.load())
			          {
				          return;
			          }

			          FRealtimeBooleanProcessor* Proc = LifeTimeToken->Processor.load();
			          if (!Proc)
			          {
				          return;
			          }

			          double StartTime = FPlatformTime::Seconds();

			          // Apply mesh.
			          if (bSuccess)
			          {
				          WeakOwner->ApplyBooleanOperationResult(MoveTemp(ResultMesh), ChunkIndex, false);
			          }

			          double ExecutionDurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

			          Proc->UpdateSimplifyInterval(ExecutionDurationMs);

			          // Spawn debris
			          if (Context.IsValid())
			          {
				          if (Context->RemainingTaskCount.fetch_sub(1) == 1)
				          {					          
					          if (Context->Owner.IsValid() && Context->AccumulatedDebrisMesh.TriangleCount() > 0)
					          {
						          TArray<UMaterialInterface*> Materials;
						          if (auto ChunkMesh = WeakOwner->GetChunkMeshComponent(1))
						          {
							          for (int32 i = 0; i < ChunkMesh->GetNumMaterials(); i++)
							          {
								          Materials.Add(ChunkMesh->GetMaterial(i));
							          }
						          }

						          Context->Owner->SpawnDebrisActor(MoveTemp(Context->AccumulatedDebrisMesh), Materials);
					          	Context->Owner->CleanupSmallFragments();
					          }
				          }
			          }

			          // ===== Decrement counters (shutdown check) =====
			          if (LifeTime.IsValid() && LifeTime->bAlive.load() && SlotSubtractWorkerCounts.IsValidIndex(
				          SlotIndex))
			          {
				          SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
			          }

			          // Update counters.
			          Proc->ChunkGenerations[ChunkIndex]++;
			          Proc->ChunkHoleCount[ChunkIndex] += UnionCount;

			          if (!bOverBudget)
			          {
				          // If queue has work, re-kick.
				          if (!Proc->SlotSubtractQueues[SlotIndex]->IsEmpty())
				          {
					          Proc->KickSubtractWorker(SlotIndex);
				          }
				          // Next kick.
				          Proc->KickProcessIfNeededPerChunk();
			          }
			          else
			          {
				          // Skip kick if over budget
				          // retry in URealtimeDestructibleMeshComponent::TickComponent
				          UE_LOG(LogTemp, Display, TEXT("Slot %d yielding"), SlotIndex);
			          }
		          });
	}
	else
	{
		// Even on failure, re-kick if queue has work (GameThread).
		AsyncTask(ENamedThreads::GameThread, [this, SlotIndex, bOverBudget]()
		{
			if (SlotSubtractWorkerCounts.IsValidIndex(SlotIndex))
			{
				SlotSubtractWorkerCounts[SlotIndex]->fetch_sub(1);
			}

			if (!bOverBudget && !SlotSubtractQueues[SlotIndex]->IsEmpty())
			{
				KickSubtractWorker(SlotIndex);
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

	FScopeLock Lock(&MapLock);

	// Remove all chunks mapped to this slot.
	TArray<int32> ChunksToRemove;

	for (const auto& Pair : ChunkToSlotMap)
	{
		if (Pair.Value == SlotIndex)
		{
			ChunksToRemove.Add(Pair.Key);
		}
	}

	for (int32 ChunkIdx : ChunksToRemove)
	{
		ChunkToSlotMap.Remove(ChunkIdx);
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
		TRACE_CPUPROFILER_EVENT_SCOPE("GatherOps");
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
				 if (bEnableMultiWorkers)
				 {
				 	// Decide slot for this chunk.
				 	int32 TargetSlot = RouteToSlot(ChunkIndex);

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
				Processor = LifeTimeToken->Processor.load();
				if (!Processor)
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
						TRACE_CPUPROFILER_EVENT_SCOPE("ChunkBooleanAsync_Simplify");
						bool bIsSimplified = Processor->TrySimplify(WorkMesh, ChunkIndex, UnionCount);
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

	// Reset hole count and caches.
	CurrentHoleCount = 0;

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

void FRealtimeBooleanProcessor::UpdateSimplifyInterval(double CurrentSetMeshAvgCost)
{
	if (FMath::IsNearlyZero(SetMeshAvgCost))
	{
		SetMeshAvgCost = CurrentSetMeshAvgCost;
		return;
	}

	const double OldAvgCost = SetMeshAvgCost;

	// Compute exponential moving average (EMA, like linear interpolation).
	const double NewAvgCost = FMath::Lerp(SetMeshAvgCost, CurrentSetMeshAvgCost, 0.1);
	SetMeshAvgCost = NewAvgCost;

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
		UE_LOG(LogTemp, Display, TEXT("Interval decrease %d to %lld"), MaxInterval, FMath::FloorToInt(MaxInterval * 0.7));
		// Clamp lower bound to 15.
		MaxInterval = FMath::Max(15, FMath::FloorToInt(MaxInterval * 0.7));
	}
	// If cost decreased or flat, increase slowly.
	else if (ReductionRate >= StableThreshold)
	{
		UE_LOG(LogTemp, Display, TEXT("Interval increase %d to %d"), MaxInterval, MaxInterval + 1);
		MaxInterval = FMath::Min(InitInterval * 2, MaxInterval + 1);
	}
	// Watch and hold for 0-10% increase.
	// Keep else branch for logging.
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
	else if (State.Interval >= MaxInterval)
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
		ApplySimplifyToPlanarAsync(&WorkMesh, SimplifyOptions);
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

void FRealtimeBooleanProcessor::ApplySimplifyToPlanarAsync(UE::Geometry::FDynamicMesh3* TargetMesh, FGeometryScriptPlanarSimplifyOptions Options)
{
	if (!TargetMesh)
	{
		return;
	}

	if (!TargetMesh->HasAttributes())
	{
		TargetMesh->EnableAttributes();
	}
	
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
	
	FQEMSimplification Simplifier(TargetMesh);

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
	Simplifier.CollapseMode = FQEMSimplification::ESimplificationCollapseModes::MinimalExistingVertexError;
	
	Simplifier.SetExternalConstraints(MoveTemp(ExternalConstraints));
	
	Simplifier.SimplifyToMinimalPlanar(FMath::Max(0.00001, Options.AngleThreshold));

	if (Options.bAutoCompact)
	{
		TargetMesh->CompactInPlace();
	}
}