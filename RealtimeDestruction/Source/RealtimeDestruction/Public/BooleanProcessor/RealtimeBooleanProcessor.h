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
#include <atomic>
#include "UObject/WeakObjectPtr.h"
#include "Containers/Queue.h"
#include "DynamicMesh/MeshTangents.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "HAL/CriticalSection.h"

////////////////////////////////////////
/******** forward declaration ********/
namespace UE::Geometry
{
	class FDynamicMesh3;
}
class UDynamicMeshComponent;
class UPrimitiveComponent;
struct FRealtimeDestructionOp;
struct FGeometryScriptMeshBooleanOptions;
struct FGeometryScriptPlanarSimplifyOptions;
enum class EGeometryScriptBooleanOperation : uint8;
class URealtimeDestructibleMeshComponent;
class UDecalComponent;
class URDMThreadManagerSubsystem;
////////////////////////////////////////

enum class EBooleanWorkType : uint8
{
	BulletHole,
	IslandRemoval
};

// Forward declaration
class ADebrisActor;

struct FIslandRemovalContext
{
	std::atomic<int32> RemainingTaskCount{0};
	UE::Geometry::FDynamicMesh3 AccumulatedDebrisMesh;

	FCriticalSection MeshLock;
	TWeakObjectPtr<URealtimeDestructibleMeshComponent> Owner;

	/** For client: Apply mesh to existing DebrisActor (calls SpawnDebrisActor if null) */
	TWeakObjectPtr<ADebrisActor> TargetDebrisActor;

	/** For cleanup: Disconnected cell IDs (passed to CleanupSmallFragments when all tasks complete) */
	TSet<int32> DisconnectedCellsForCleanup;
};

/** Union result payload for a chunk, including the combined tool mesh and decals. */
struct FUnionResult
{
	int32 BatchID = 0;				                     // For ordering
	UE::Geometry::FDynamicMesh3 PendingCombinedToolMesh; // Union result mesh
	TArray<TWeakObjectPtr<UDecalComponent>> Decals;
	int32 UnionCount = 0;

	// Chunk-scoped fields
	int32 ChunkIndex = INDEX_NONE;
	TWeakObjectPtr<UDynamicMeshComponent> TargetChunkMesh = nullptr;

	TSharedPtr<UE::Geometry::FDynamicMesh3> SharedToolMesh = nullptr;
	TSharedPtr<UE::Geometry::FDynamicMesh3> DebrisSharedToolMesh = nullptr;
	TSharedPtr<UE::Geometry::FDynamicMesh3> OutDebrisMesh = nullptr;
	TSharedPtr<FIslandRemovalContext> IslandContext;
	EBooleanWorkType WorkType = EBooleanWorkType::BulletHole;

	/** Batch completion tracking ID array (multiple Ops may be unioned and processed together) */
	TArray<int32> CompletionBatchIds;
};

/** A single tool impact request queued for boolean processing. */
struct FBulletHole
{
	// TODO: Consider switching to a SoA layout.
	
	FTransform ToolTransform = {};
	uint8 Attempts = 0;
	static constexpr uint8 MaxAttempts = 2;

	// true: penetration, false: non-penetration
	bool bIsPenetration = false;

	TWeakObjectPtr<UDecalComponent> TemporaryDecal = nullptr;

	TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe> ToolMeshPtr = nullptr;

	TWeakObjectPtr<UDynamicMeshComponent> TargetMesh = nullptr;

	int32 ChunkIndex = INDEX_NONE;

	/** Batch completion tracking ID (no tracking if INDEX_NONE) */
	int32 BatchId = INDEX_NONE;

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
		BatchId = INDEX_NONE;
	}
};

/** Batched bullet hole data (SoA) to run union/subtract per chunk. */
struct FBulletHoleBatch
{
	TArray<FTransform> ToolTransforms = {};
	TArray<uint8> Attempts = {};
	TArray<bool> bIsPenetrations = {};
	TArray<TWeakObjectPtr<UDecalComponent>> TemporaryDecals = {};
	TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> ToolMeshPtrs = {};
	TArray<int32> CompletionBatchIds = {};  // For batch completion tracking

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
		CompletionBatchIds.Reserve(Capacity);
	}

	void Reset()
	{
		Count = 0;
		ToolTransforms.Reset();
		Attempts.Reset();
		bIsPenetrations.Reset();
		TemporaryDecals.Reset();
		ToolMeshPtrs.Reset();
		CompletionBatchIds.Reset();
	}

	void Add(FBulletHole& Op)
	{
		ToolTransforms.Add(Op.ToolTransform);
		Attempts.Add(Op.Attempts);
		bIsPenetrations.Add(Op.bIsPenetration);
		TemporaryDecals.Add(Op.TemporaryDecal);
		ToolMeshPtrs.Add(Op.ToolMeshPtr);
		// Only add valid BatchId (not INDEX_NONE)
		if (Op.BatchId != INDEX_NONE)
		{
			CompletionBatchIds.AddUnique(Op.BatchId);
		}
		Count++;
	}

	void Add(FBulletHole&& Op)
	{
		ToolTransforms.Add(MoveTemp(Op.ToolTransform));
		Attempts.Add(MoveTemp(Op.Attempts));
		bIsPenetrations.Add(MoveTemp(Op.bIsPenetration));
		TemporaryDecals.Add(MoveTemp(Op.TemporaryDecal));
		ToolMeshPtrs.Add(MoveTemp(Op.ToolMeshPtr));
		// Only add valid BatchId (not INDEX_NONE)
		if (Op.BatchId != INDEX_NONE)
		{
			CompletionBatchIds.AddUnique(Op.BatchId);
		}
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
	 * Do not rely on ToolTransforms.Num() after moves.
	 * Move operations transfer ownership, leaving ToolTransforms.Num() as 0.
	 */
	int32 Num() const { return Count; }
};

/** Per-chunk counters used for simplify scheduling and cost accumulation. */
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

/** Container for per-chunk processing state with lifecycle helpers. */
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

/**
 * Schedules realtime boolean operations across chunks with batching and async workers.
 * Tracks per-chunk metrics to adapt union size and simplify intervals,
 * then applies results on the game thread.
 */
class FRealtimeBooleanProcessor
{
public:
	/** Shared lifetime token for async workers to detect shutdown safely. */
	struct FProcessorLifeTime
	{
		std::atomic<bool> bAlive{ true };
		TWeakPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> Processor = nullptr;

		FProcessorLifeTime() = default;
		~FProcessorLifeTime()
		{
			Clear();
		}

		void Init(const TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe>& Owner)
		{
			Processor = Owner;
			bAlive.store(true);
		}
		void Clear()
		{
			bAlive = false;
			Processor.Reset();
		}
	};

public:
	FRealtimeBooleanProcessor() = default;
	~FRealtimeBooleanProcessor();

	bool Initialize(URealtimeDestructibleMeshComponent* Owner);
	void Shutdown();

	/** Enqueues a boolean operation request. */
	void EnqueueOp(FRealtimeDestructionOp&& Operation, UDecalComponent* TemporaryDecal, UDynamicMeshComponent* ChunkMesh = nullptr, int32 BatchId = -1);
	/** Re-enqueues remaining requests (including retries). */
	void EnqueueRemaining(FBulletHole&& Operation);
	void EnqueueIslandRemoval(int32 ChunkIndex, TSharedPtr<UE::Geometry::FDynamicMesh3> ToolMesh, TSharedPtr<UE::Geometry::FDynamicMesh3> DebrisToolMesh, TSharedPtr<FIslandRemovalContext> Context);

	/**
	 * Builds per-chunk batches from queued ops and starts workers when work is available.
	 * Work is available when at least one batch is formed; in single-worker mode it starts only
	 * if the chunk is not busy, otherwise the batch is re-queued for a later tick.
	 */
	void KickProcessIfNeededPerChunk();

	/** Returns whether the owning URealtimeDestructibleMeshComponent is valid. */
	bool IsOwnerCompValid() const { return OwnerComponent.IsValid(); }

	/** Clears pending work and resets accumulated counters. */
	void CancelAllOperations();
	
	/** Returns the hole count for the specified chunk index. */
	int32 GetChunkHoleCount(int32 ChunkIndex) const { return ChunkHoleCount[ChunkIndex]; }
	/** Resolves the chunk index from the component and returns its hole count. */
	int32 GetChunkHoleCount(const UPrimitiveComponent* ChunkComponent) const;

	/** Runs a mesh boolean and writes the result into OutputMesh. */
	static bool ApplyMeshBooleanAsync(const UE::Geometry::FDynamicMesh3* TargetMesh,
		const UE::Geometry::FDynamicMesh3* ToolMesh,
		UE::Geometry::FDynamicMesh3* OutputMesh,
		const EGeometryScriptBooleanOperation Operation,
		const FGeometryScriptMeshBooleanOptions Options,
		const FTransform& TargetTransform = FTransform::Identity,
		const FTransform& ToolTransform = FTransform::Identity
		);

	/**
	 * Applies planar simplification to clean up the mesh.
	 * Removes low-importance vertices to prevent triangle count blow-up.
	 */
	static void ApplySimplifyToPlanarAsync(UE::Geometry::FDynamicMesh3* TargetMesh,
		FGeometryScriptPlanarSimplifyOptions Options,
		bool bEnableDetail);

	/**
	 * Applies uniform remeshing to reduce accumulated vertex count.
	 * Boundary edges are fully constrained to preserve mesh silhouette.
	 * @param TargetMesh The mesh to remesh in-place.
	 * @param TargetEdgeLength Desired edge length after remeshing.
	 * @param NumPasses Number of remesh iterations (more passes = better convergence).
	 */
	static void ApplyUniformRemesh(UE::Geometry::FDynamicMesh3* TargetMesh, double TargetEdgeLength, int32 NumPasses = 5);

private:	
	// ===============================================================
	// Processing Pipeline
	// ===============================================================
	void StartBooleanWorkerAsyncForChunk(FBulletHoleBatch&& InBatch, int32 Gen);	
	void EnqueueRetryOps(TQueue<FBulletHole, EQueueMode::Mpsc>& Queue, FBulletHoleBatch&& InBatch,
		UDynamicMeshComponent* TargetMesh, int32 ChunkIndex, int32& DebugCount);
	int32& GetChunkInterval(int32 ChunkIndex);	
	
	// ===============================================================
	// Simplification & Adaptive Tuning
	// ===============================================================
	void AccumulateSubtractDuration(int32 ChunkIndex, double CurrentSubDuration);
	void UpdateSimplifyInterval(double CurrentSetMeshAvgCost, int32 ChunkIndex);
	void UpdateUnionSize(int32 ChunkIndex, double DurationMs);
	bool TrySimplify(UE::Geometry::FDynamicMesh3& WorkMesh, int32 ChunkIndex, int32 UnionCount, bool bEnableDetail);
	/** Subtract cost tracker. */
	void UpdateSubtractAvgCost(double CostMs);
	
	// ===============================================================
	// Threading & Slot Workers
	// ===============================================================
	// ThreadManager access helper
	URDMThreadManagerSubsystem* GetThreadManager() const;

	void InitializeSlots();
	void ShutdownSlots();

	// Find the least busy slot.
	int32 FindLeastBusySlot() const;

	// Start workers.
	void KickUnionWorker(int32 SlotIndex);
	void KickSubtractWorker(int32 SlotIndex);

	// Worker main loop (batch passed as parameter for MPSC queue safety).
	void ProcessSlotUnionWork(int32 SlotIndex, FBulletHoleBatch&& Batch);
	void ProcessSlotSubtractWork(int32 SlotIndex, FUnionResult&& UnionResult);

	// Clean up mapping when a slot drains.
	void CleanupSlotMapping(int32 SlotIndex);

	void BooleanOpSync(FBulletHole&& Op);

private:
	// ===============================================================
	// Processing Pipeline
	// ===============================================================
	TWeakObjectPtr<URealtimeDestructibleMeshComponent> OwnerComponent = nullptr;

	TSharedPtr<FProcessorLifeTime, ESPMode::ThreadSafe> LifeTime;
	
	// Separate queues for penetration and non-penetration operations.
	TQueue<FBulletHole, EQueueMode::Mpsc> HighPriorityQueue;
	int DebugHighQueueCount;

	TQueue<FBulletHole, EQueueMode::Mpsc> NormalPriorityQueue;
	int DebugNormalQueueCount;

	FChunkProcessState ChunkStates;
	
	// Chunk generation tracking.
	// Incremented when a boolean result is applied to the mesh.
	TArray<std::atomic<int32>> ChunkGenerations;

	/** Per-chunk union result queues (independent pipelines per chunk). */
	TArray<TUniquePtr<TQueue<FUnionResult, EQueueMode::Mpsc>>> ChunkUnionResultsQueues;

	/** Per-chunk batch ID counters. */
	TArray<std::atomic<int32>> ChunkNextBatchIDs;

	// Max union count per chunk for tool meshes.
	TArray<uint8> MaxUnionCount;

	TArray<int32> ChunkHoleCount = {};

	bool bEnableMultiWorkers;
	std::atomic<int32> ActiveChunkCount{ 0 };

	TArray<TSharedPtr<UE::Geometry::FDynamicMesh3, ESPMode::ThreadSafe>> CachedChunkMeshes;

	// ===============================================================
	// Simplification & Adaptive Tuning
	// ===============================================================
	// Testing from default values.
	float AngleThreshold = 0.001;
	
	TArray<uint16> MaxInterval = {};
	uint8 InitInterval = 0;

	double SubDurationHighThreshold = 0.0;
	double SubDurationLowThreshold = 5.0;

	TArray<double> SetMeshAvgCost = {};

	double FrameBudgetMs = 16.0f;
	double SubtractAvgCostMs = 2.0f;
	double SubtractCostAccum = 0.0f;
	int32 SubtractCostSampleCount = 0;
 
	// ===============================================================
	// Threading & Slot Workers
	// ===============================================================
	// Slot count (worker management slots).
	int32 NumSlots = 1;
	
	int32 MaxUnionWorkerPerSlot = 1;
	int32 MaxSubtractWorkerPerSlot = 3;

	// Debug/statistics only.
	TArray<TUniquePtr<std::atomic<int32>>>SlotUnionWorkerCounts; 
	TArray<TUniquePtr<std::atomic<int32>>> SlotSubtractWorkerCounts;
	
	// Per-slot union queues.
	TArray<TUniquePtr<TQueue<FBulletHoleBatch, EQueueMode::Mpsc>>> SlotUnionQueues;

	// Per-slot subtract queues.
	TArray<TUniquePtr<TQueue<FUnionResult, EQueueMode::Mpsc>>> SlotSubtractQueues;
	
	FCriticalSection MapLock; 
	
	// Per-slot worker active flags.
	TArray<TUniquePtr<std::atomic<bool>>> SlotUnionActiveFlags;
	TArray<TUniquePtr<std::atomic<bool>>> SlotSubtractActiveFlags;	
};




