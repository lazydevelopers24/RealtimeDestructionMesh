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
#include "Components/DynamicMeshComponent.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "DestructionTypes.h"
#include "StructuralIntegrity/GridCellTypes.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "RealtimeDestructibleMeshComponent.generated.h"

class UBoxComponent;
class UProceduralMeshComponent;
class UGeometryCollection;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class FLifetimeProperty;
class FRealtimeBooleanProcessor;
class UBulletClusterComponent;
class UImpactProfileDataAsset;
class ADebrisActor;

//////////////////////////////////////////////////////////////////////////
// Destruction Types
//////////////////////////////////////////////////////////////////////////

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDestructionOpId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	int64 Value = 0;
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FRealtimeDestructionRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector ImpactPoint = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector ImpactNormal = FVector::UpVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	float Depth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	int32 RandomSeed = 0;

	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> ToolMeshPtr = {};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector DecalLocationOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FRotator DecalRotationOffset = FRotator::ZeroRotator;
	
	/** Tool Shape Parameters (for network serialization) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FDestructionToolShapeParams ShapeParams;
	  
	/** Client send time for RTT measurement (set only on client) */
	UPROPERTY()
	double ClientSendTime = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	int32 ChunkIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector ToolForwardVector = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector ToolOriginWorld = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector DecalSize = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	bool bSpawnDecal = true;

	/** Decal Material (retrieved from Projectile or ImpactProfile) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	TObjectPtr<UMaterialInterface> DecalMaterial = nullptr;
	
	UPROPERTY(EditAnywhere, Category = "RealtimeDestructibleMesh")
	FName SurfaceType = FName("Default");

	UPROPERTY()
	bool bRandomRotation = false;
	
	/** Decal config lookup ID (for network transmission) */
	UPROPERTY()
	FName DecalConfigID = FName("Default");
	
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FRealtimeDestructionOp
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FDestructionOpId OpId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FRealtimeDestructionRequest Request;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	int32 Sequence = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	bool bIsPenetration = false;
};

/**
 * Compressed destruction request data (using Unreal's built-in NetQuantize)
 *
 * Original FRealtimeDestructionRequest: ~320 bits
 * Compressed FCompactDestructionOp: ~102 bits (when serialized)
 *
 * ~70% network bandwidth reduction
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FCompactDestructionOp
{
	GENERATED_BODY()

	// Position: 1cm precision (~6 bytes when serialized)
	UPROPERTY()
	FVector_NetQuantize ImpactPoint;

	// Normal: 0.1cm precision - higher precision for direction (~6 bytes when serialized)
	UPROPERTY()
	FVector_NetQuantize10 ImpactNormal;

	// Tool mesh origin
	UPROPERTY()
	FVector_NetQuantize10 ToolOriginWorld;

	// Bullet direction (~6 bytes when serialized)
	UPROPERTY()
	FVector_NetQuantize10 ToolForwardVector;

	// Radius: 1-255 cm (1 byte)
	UPROPERTY()
	uint8 Radius = 10;

	// Sequence: allows rollover (2 bytes)
	UPROPERTY()
	uint16 Sequence = 0;

	// Tool Shape (1 byte)
	UPROPERTY()
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;

	// Shape parameters (for network serialization)
	UPROPERTY()
	FDestructionToolShapeParams ShapeParams;

	// Chunk Index (calculated by client, 1 byte)
	UPROPERTY()
	uint8 ChunkIndex = 0;

	UPROPERTY()
	FVector_NetQuantize DecalSize;

	// Decal config lookup ID
	UPROPERTY()
	FName DecalConfigID = FName("Default");

	// SurfaceType (for decal lookup)
	UPROPERTY()
	FName SurfaceType = FName("Default");

	// Compress
	static FCompactDestructionOp Compress(const FRealtimeDestructionRequest& Request, int32 Seq);

	// Decompress
	FRealtimeDestructionRequest Decompress() const;
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FRealtimeMeshSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	int32 Version = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	TArray<uint8> Payload;
};

USTRUCT()
struct FRealtimeDestructibleMeshComponentInstanceData : public FActorComponentInstanceData
{
	GENERATED_BODY()

public:
	FRealtimeDestructibleMeshComponentInstanceData() = default;
	FRealtimeDestructibleMeshComponentInstanceData(const URealtimeDestructibleMeshComponent* SourceComponent);

	virtual void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override;

	UPROPERTY()
	TObjectPtr<UStaticMesh> SavedSourceStaticMesh;

	UPROPERTY()
	bool bSavedIsInitialized = false;

	UPROPERTY()
	bool bSavedChunkMeshesValid = false;

	UPROPERTY()
	FIntVector SavedSliceCount = FIntVector::ZeroValue;

	UPROPERTY()
	bool bSavedShowGridCellDebug = false;

	// Store component names instead of pointers (to find by name during PIE duplication)
	UPROPERTY()
	TArray<FString> SavedChunkComponentNames;

	// GridCellLayout 보존 (Blueprint 재구성 시 앵커 데이터 유실 방지)
	UPROPERTY()
	FGridCellLayout SavedGridCellLayout;

	// CachedRDMScale 보존 (Blueprint 재구성 후 BeginPlay에서 불필요한 BuildGridCells 방지)
	UPROPERTY()
	FVector SavedCachedRDMScale = FVector(1.0f, 1.0f, 1.0f);
};

//////////////////////////////////////////////////////////////////////////
// Delegates
//////////////////////////////////////////////////////////////////////////

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDestructMeshInitialized);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDestructOpApplied, const FRealtimeDestructionOp&, Op);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDestructBatchCompleted, int32, AppliedCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDestructError, FName, ErrorCode, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDestructionRejected, int32, Sequence, EDestructionRejectReason, Reason);

//////////////////////////////////////////////////////////////////////////
// Class Declaration
//////////////////////////////////////////////////////////////////////////

/** Chunk data for server Cell Box Collision */
USTRUCT()
struct FCollisionChunkData
{
	GENERATED_BODY()

	/** BodySetup for this chunk */
	UPROPERTY()
	TObjectPtr<UBodySetup> BodySetup = nullptr;

	/** Component used by this chunk (prevents GC and provides access) */
	UPROPERTY()
	TObjectPtr<UPrimitiveComponent> ChunkComponent = nullptr;

	/** Cell IDs belonging to this chunk */
	TArray<int32> CellIds;

	/** Surface cell IDs of this chunk (cells that have actual collision boxes) */
	TArray<int32> SurfaceCellIds;

	/** Whether rebuild is needed */
	bool bDirty = false;
};


struct FMeshSectionData
{
	TArray<FVector> Vertices;           // Vertex positions (relative to MeshCenter)
	TArray<int32> Triangles;            // Triangle indices
	TArray<FVector> Normals;            // Vertex normals
	TArray<FVector2D> UVs;              // UV coordinates
	TMap<FVertexKey, int32> VertexRemap;     // Original VertexID → New index within section
};

/**
 * Mesh component supporting real-time destruction.
 *
 * The core component of the Realtime Destructible Mesh plugin.
 *
 * To use this component, first assign a Source Mesh,
 * then use the 'Generate Destructible Chunks' and 'Build Grid Cell' buttons in the Details panel to structure the mesh.
 *
 * Destruction can be triggered via an Actor possessing a UDestructionProjectileComponent.
 * Network synchronization is supported through a PlayerController 
 * equipped with a UDestructionNetworkComponent.
 */
UCLASS(ClassGroup = (RealtimeDestruction), meta = (BlueprintSpawnableComponent, DisplayName = "Realtime Destructible Mesh"))
class REALTIMEDESTRUCTION_API URealtimeDestructibleMeshComponent : public UDynamicMeshComponent
{
	GENERATED_BODY()

	friend struct FRealtimeDestructibleMeshComponentInstanceData;
	friend class FRealtimeDestructibleMeshComponentDetails;

public:
	URealtimeDestructibleMeshComponent();
	URealtimeDestructibleMeshComponent(FVTableHelper& Helper);
	virtual ~URealtimeDestructibleMeshComponent() override;

	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;

	/**
	 * Populates the SourceStaticMesh and DynamicMesh fields using the provided UStaticMesh.
	 * SourceStaticMesh is used as the material for creating ChunkMeshComponents.
	 * The DynamicMesh field only serves to provide a mesh preview before chunk generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh",meta = (DeprecatedFunction))
	bool InitializeFromStaticMesh(UStaticMesh* InMesh);

	UE_DEPRECATED(5.7, "Function has been deprecated. Do not Initalize this component directly form static mesh")
		UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh",
			meta = (DeprecatedFunction, DeprecationMessage = "Function has been deprecated. Do not Initalize this component directly form static mesh"))
	bool InitializeFromStaticMeshComponent(UStaticMeshComponent* InComp);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	void ResetToSourceMesh();

	// Destruction queue
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	FDestructionOpId EnqueueRequestLocal(const FRealtimeDestructionRequest& Request, bool bPenetration, UDecalComponent* TemporaryDecal = nullptr, int32 BatchId = -1);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	int32 EnqueueBatch(const TArray<FRealtimeDestructionRequest>& Requests);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	bool RequestDestruction(const FRealtimeDestructionRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	bool ExecuteDestructionInternal(const FRealtimeDestructionRequest& Request);
	
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerEnqueueOps(const TArray<FRealtimeDestructionRequest>& Requests);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastApplyOps(const TArray<FRealtimeDestructionOp>& Ops);

	/** Compressed multicast RPC (Server -> Client) */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastApplyOpsCompact(const TArray<FCompactDestructionOp>& CompactOps);

	/**
	 * Destroyed cell ID broadcast RPC (Server → Client)
	 * Sent immediately upon destruction to synchronize client CellState
	 * @param DestroyedCellIds - Newly destroyed cell IDs
	 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastDestroyedCells(const TArray<int32>& DestroyedCellIds);

	/**
	 * Detach signal RPC (Server → Client)
	 * Clients run their own BFS to calculate detached cells
	 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastDetachSignal();

	/** Destruction request rejection RPC (Server → Requesting client) */
	UFUNCTION(Client, Reliable)
	void ClientDestructionRejected(uint16 Sequence, EDestructionRejectReason Reason);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|Replication")
	void ApplyOpsDeterministic(const TArray<FRealtimeDestructionOp>& Ops);

	/**
	 * Server batching: Add request to queue
	 * Called only on server
	 */
	void EnqueueForServerBatch(const FRealtimeDestructionOp& Op);

	/**
	 * Server batching: Flush queue and multicast
	 * Called only on server
	 */
	void FlushServerBatch();

	//////////////////////////////////////////////////////////////////////////
	// Server Validation
	//////////////////////////////////////////////////////////////////////////

	/**
	 * Validate destruction request (called on server)
	 * @param Request Destruction request
	 * @param RequestingPlayer Requesting player (validation skipped if nullptr)
	 * @param OutReason Rejection reason (on failure)
	 * @return Whether validation passed
	 */
	bool ValidateDestructionRequest(const FRealtimeDestructionRequest& Request, APlayerController* RequestingPlayer, EDestructionRejectReason& OutReason);

	FConnectivityContext CellContext;

	/** Server validation: Range limit */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Validation")
	float MaxDestructionRange = 5000.0f;

	/** Server validation: Rate limit (max destructions per second) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Validation")
	float MaxDestructionsPerSecond = 10.0f;

	/** Server validation: Max requests per single RPC (kick if exceeded) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Validation", meta = (ClampMin = "1", ClampMax = "200"))
	int32 MaxRequestsPerRPC = 50;

	/** Server validation: Max allowed destruction radius (kick if exceeded) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Validation", meta = (ClampMin = "1.0"))
	float MaxAllowedRadius = 500.0f;

	/** Server validation: Enable line of sight check */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Validation")
	bool bEnableLineOfSightCheck = true;

	/** Rate limit tracking info */
	struct FRateLimitInfo
	{
		double WindowStartTime = 0.0;
		int32 RequestCount = 0;
	};

	/** Per-player rate limit tracking (server only) */
	TMap<TWeakObjectPtr<APlayerController>, FRateLimitInfo> PlayerRateLimits;

	/** Rate limit check (called on server) */
	bool CheckRateLimit(APlayerController* Player);

	//////////////////////////////////////////////////////////////////////////
	// Server Batching Settings (Server → Client Batching)
	//////////////////////////////////////////////////////////////////////////

	/**
	 * Whether to use server batching
	 * true: Collect requests from multiple clients and multicast at once (reduces header overhead)
	 * false: Individual multicast per request
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching")
	bool bUseServerBatching = true;

	/** Server batch transmission interval (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching", meta = (ClampMin = "0.008", ClampMax = "0.5"))
	float ServerBatchInterval = 0.016f;  // 16ms = 1 frame (at 60fps)

	/** Max server batch size (transmit immediately when this count is reached) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching", meta = (ClampMin = "1", ClampMax = "100"))
	int32 MaxServerBatchSize = 20;

	/**
	 * Whether to use multicast compression
	 * true: Use compressed FCompactDestructionOp (~102 bits/request)
	 * false: Use original FRealtimeDestructionOp (~320 bits/request)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching")
	bool bUseCompactMulticast = true;

	//////////////////////////////////////////////////////////////////////////
	// Late Join: Op History-based Synchronization
	//////////////////////////////////////////////////////////////////////////

	/** Get Op history (for Late Join sync, valid on server only) */
	const TArray<FCompactDestructionOp>& GetAppliedOpHistory() const { return AppliedOpHistory; }

	/** Clear Op history (called on mesh reset) */
	void ClearOpHistory() { AppliedOpHistory.Empty(); LateJoinDestroyedCells.Empty(); }

	/** Apply Late Join data (called from TickComponent when conditions are met) */
	void ApplyLateJoinData();

	UFUNCTION()
	void OnRep_LateJoinOpHistory();

	UFUNCTION()
	void OnRep_LateJoinDestroyedCells();

	// Events
	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructMeshInitialized OnInitialized;

	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructOpApplied OnOpApplied;

	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructBatchCompleted OnBatchCompleted;

	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructError OnError;

	/** Called when destruction request is rejected by server (client only) */
	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructionRejected OnDestructionRejected;

	/** Clustering variables */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Clustering")
	TObjectPtr<UBulletClusterComponent> BulletClusterComponent;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Clustering")
	bool bEnableClustering = true;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Clustering")
	float MaxMergeDistance = 10.0f;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Clustering")
	int  MinClusterCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Clustering")
	float MaxClusterRadius = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Clustering")
	float ClusterRaidusOffset = 1.0f;

	UPROPERTY()
	FBox CachedMeshBounds;

	UPROPERTY()
	FVector CachedChunkSize;

	UPROPERTY()
	FVector CachedCellSize; 
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|HoleDecal")
	FName SurfaceType = FName("Default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|StructuralIntegrity")
	bool bEnableSubcell = true;

	/**
	 * Whether to use SuperCell-based Hierarchical BFS optimization
	 * true: Use 2-Level Hierarchical BFS (performance improvement for large grids)
	 * false: Use original Cell-level BFS
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|StructuralIntegrity")
	bool bEnableSupercell = true;
	
	/** Function for preserving data */
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;

	/*
	 * Functions not exposed to editor
	 */
	FGeometryScriptMeshBooleanOptions GetBooleanOptions() const { return BooleanOptions; }
	FRealtimeBooleanProcessor* GetBooleanProcessor() const { return BooleanProcessor.Get(); }
	TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> GetBooleanProcessorShared() { return BooleanProcessor; }

	/** Recreate ToolMeshPtr from ShapeParams (used when receiving over network) */
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> CreateToolMeshPtrFromShapeParams(
		EDestructionToolShape ToolShape,
		const FDestructionToolShapeParams& ShapeParams);
	float GetAngleThreshold() const { return AngleThreshold; }
	double GetSubtractDurationLimit() const { return SubtractDurationLimit; }
	int32 GetInitInterval() const { return InitInterval; }
	bool IsHighDetailMode() const { return bEnableHighDetail;}

	void ApplyRenderUpdate();
	void ApplyCollisionUpdate(UDynamicMeshComponent* TargetComp);
	void ApplyCollisionUpdateAsync(UDynamicMeshComponent* TargetComp);

	/** Check if target chunk is penetrated during operation */
	bool IsChunkPenetrated(const FRealtimeDestructionRequest& Request) const;
	
	void SettingAsyncOption(bool& OutMultiWorker);

	bool IsInitialized() { return bIsInitialized;  }

	int32 GetChunkIndex(const UPrimitiveComponent* ChunkMesh);

	int32 GetChunkNum() const { return ChunkMeshComponents.Num(); }

	bool IsChunkValid(int32 ChunkIndex) const;

	UDynamicMeshComponent* GetChunkMeshComponent(int32 ChunkIndex) const;

	bool GetChunkMesh(FDynamicMesh3& OutMesh, int32 ChunkIndex) const;

	bool CheckAndSetChunkBusy(int32 ChunkIndex);

	void FindChunksInRadius(const FVector& WorldCenter, float Radius, TArray<int32>& OutChunkIndices, bool bAppend = false);
	
	void FindChunksAlongLine(const FVector& WorldStart, const FVector& WorldEnd, float Radius, TArray<int32>& OutChunkIndices, bool bAppend = false);

	// Bit operations are not atomic, logic modification needed when calling outside GT
	void ClearChunkBusy(int32 ChunkIndex);

	void ClearAllChunkBusyBits(); 

	void SetChunkBits(int32 ChunkIndex, int32& BitIndex, int32& BitOffset);

	// Function to immediately update visual (rendering) processing of modified mesh
	void ApplyBooleanOperationResult(FDynamicMesh3&& NewMesh, const int32 ChunkIndex, bool bDelayedCollisionUpdate, int32 BatchId = -1);

	/** Increment batch counter when Boolean operation is skipped/failed */
	void NotifyBooleanSkipped(int32 BatchId);

	/** Increment batch counter when Boolean operation completes */
	void NotifyBooleanCompleted(int32 BatchId);
	
	// Function to update collision async with target mesh idle or desired delay
	void RequestDelayedCollisionUpdate(UDynamicMeshComponent* TargetComp);		

	/*************************************************/
	void SetSourceMeshEnabled(bool bSwitch);
	
protected:
	//////////////////////////////////////////////////////////////////////////
	// Mesh Settings
	//////////////////////////////////////////////////////////////////////////

	/**
	 * Source Static Mesh to convert into Dynamic Mesh.
	 *
	 * When this property is set in the editor, the mesh is automatically
	 * converted to Dynamic Mesh via PostEditChangeProperty.
	 * At runtime, conversion occurs during OnRegister.
	 *
	 * Features:
	 * - Automatic material copying
	 * - UV, Normal, Tangent preservation
	 * - Complex-as-simple collision enabled (uses mesh triangles for collision)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Source")
	TObjectPtr<UStaticMesh> SourceStaticMesh;

	/**
	 * Cached GeometryCollection asset
	 * Used for chunk building
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ChunkMesh")
	TObjectPtr<UGeometryCollection> CachedGeometryCollection;



	//////////////////////////////////////////////////////////////////////////
	// Destruction Settings
	//////////////////////////////////////////////////////////////////////////

	/** Decoy decal for when penetration processing is delayed */

	UDecalComponent* SpawnTemporaryDecal(const FRealtimeDestructionRequest& Request);

	void RegisterDecalToCells(UDecalComponent* Decal, const FRealtimeDestructionRequest& Request);

	void ProcessDecalRemoval(const FDestructionResult& Result);

	int32 NextDecalHandle = 0;
	
	TMap<int32, FManagedDecal> ActiveDecals;

	TMap<int32, TArray<int32>> CellToDecalMap;
 
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RealtimeDestructibleMesh|Status")
	bool bIsInitialized = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean")
	FGeometryScriptMeshBooleanOptions BooleanOptions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean")
	bool bAsyncEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean")
	bool bEnableMultiWorkers = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean")
	bool bEnableHighDetail = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean", meta = (ClampMin = 0.001))
	float AngleThreshold = 0.001f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean", meta = (ClampMin = 0.0))
	double SubtractDurationLimit = 15.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|MeshBoolean", meta = (ClampMin = 0, ClampMax = 255))
	uint8 InitInterval = 50;	

	//////////////////////////////////////////////////////////////////////////
	// Chunk Mesh Parallel Processing
	//////////////////////////////////////////////////////////////////////////

	/** Chunk meshes created by slicing the source static mesh */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RealtimeDestructibleMesh|ChunkMesh")
	TArray<TObjectPtr<UDynamicMeshComponent>> ChunkMeshComponents;

	// Key set with PrimComp, FHitResult's GetComponent returns PrimitiveComp*
	TMap<TObjectPtr<UPrimitiveComponent>, int32> ChunkIndexMap;

	/** Grid index -> ChunkId (ChunkMeshComponents array index) mapping table
	 *  Fixed after slicing, calculated in BuildChunksFromGC */
	UPROPERTY()
	TArray<int32> GridToChunkMap;

	TArray<uint64> ChunkBusyBits;

	/** For Multi Worker, Subtract checking */
	TArray<uint64> ChunkSubtractBusyBits;

	/** Whether chunk meshes are valid (build complete) */
	UPROPERTY()
	bool bChunkMeshesValid = false;

	/** Grid cell cache (created in editor, no runtime changes) */
	UPROPERTY()
	FGridCellLayout GridCellLayout;

	/** Runtime cell state */
	UPROPERTY()
	FCellState CellState;

	/** SuperCell state (for BFS optimization, created after GridCellLayout build) */
	UPROPERTY()
	FSuperCellState SupercellState;

	//=========================================================================
	// Cell-based Structural Integrity System
	//=========================================================================

	/** Whether structural integrity system is enabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|StructuralIntegrity")
	bool bEnableStructuralIntegrity = true;

	/** Quantized destruction input history (for NarrowPhase) */
	UPROPERTY()
	TArray<FQuantizedDestructionInput> DestructionInputHistory;

	/** Set of chunk IDs modified in current batch */
	TSet<int32> ModifiedChunkIds;

	//=========================================================================
	// Server Cell Box Collision (Chunked BodySetup + Surface Voxel)
	// Used instead of Boolean operations to prevent server hitching
	//=========================================================================

	/** Server collision chunk data array */
	UPROPERTY()
	TArray<FCollisionChunkData> CollisionChunks;

	/** Whether to use Server Cell Box Collision (uses original mesh collision if false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerCollision")
	bool bEnableServerCellCollision = true;

	/** Target cells per chunk (division count auto-calculated based on this value) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerCollision", meta = (ClampMin = "100", ClampMax = "2000"))
	int32 TargetCellsPerCollisionChunk = 500;

	/** Actual applied collision chunk divisions (per axis, auto-calculated at runtime) */
	int32 CollisionChunkDivisions = 4;

	/** Cell ID → Collision chunk index mapping */
	TMap<int32, int32> CellToCollisionChunkMap;

	/** Whether server Cell Collision is initialized */
	bool bServerCellCollisionInitialized = false;

	/** Whether server is dedicated server (for client branching, replicated) */
	UPROPERTY(Replicated)
	bool bServerIsDedicatedServer = false;

	/** Initialize server Cell Box Collision (called from BeginPlay) */
	void BuildServerCellCollision();

	/** Rebuild collision for dirty chunks (called from TickComponent) */
	void UpdateDirtyCollisionChunks();

	/** Mark chunk as dirty */
	void MarkCollisionChunkDirty(int32 ChunkIndex);

	/** Determine if cell is exposed (neighbor destroyed or at boundary) */
	bool IsCellExposed(int32 CellId) const;

	/** Calculate collision chunk index from cell ID */
	int32 GetCollisionChunkIndexForCell(int32 CellId) const;

	/** Build collision component and BodySetup for a single chunk */
	void BuildCollisionChunkBodySetup(int32 ChunkIndex);

public:

	/** Whether cell mesh is valid */
	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|ChunkMesh")
	bool IsChunkMeshesValid() const { return bChunkMeshesValid; }

	/**
	 * Generates grid cells from SourceStaticMesh.
	 *
	 * @warning The Grid Cell system is generated based on world coordinates.
	 * If you change the world scale of this component at runtime, there will be
	 * a mismatch between grid cells and the actual mesh, causing inaccurate destruction detection.
	 * If you need to change the scale, you must call BuildGridCells() again.
	 */
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|GridCell")
	bool BuildGridCells();

	/** Whether grid cell layout is valid */
	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|GridCell")
	bool IsGridCellLayoutValid() const { return GridCellLayout.IsValid(); }

private:
	/**
	 * Extract DynamicMesh from GeometryCollection (actual implementation)
	 * @param InGC GeometryCollection to convert
	 * @return Number of meshes extracted
	 */
	int32 BuildChunksFromGC(UGeometryCollection* InGC);

	/**
	 * Build GridToChunkMap (grid index -> ChunkId mapping)
	 * Maps to grid cells based on spatial position of each fragment
	 * Called from BuildChunksFromGC
	 */
	void BuildGridToChunkMap();

	void FindChunksAlongLineInternal(const FVector& WorldStart, const FVector& WorldEnd, TArray<int32>& OutChunkIndices);

public:
	/** Get GridCellLayout (read-only) */
	const FGridCellLayout& GetGridCellLayout() const { return GridCellLayout; }

	FGridCellLayout& GetGridCellLayout() { return GridCellLayout; }

	/** Get CellState (read-only) */
	const FCellState& GetCellState() const { return CellState; }

	/**
	 * Update cell state affected by destruction request
	 * Called along with Boolean destruction processing to perform cell destruction determination
	 *
	 * @param Request - Destruction request information
	 */
	void UpdateCellStateFromDestruction(const FRealtimeDestructionRequest& Request);
	FDestructionResult DestructionLogic(const FRealtimeDestructionRequest& Request);
	void DisconnectedCellStateLogic(const TArray< FDestructionResult>& AllResults, bool bForceRun = false);

	float CalculateDebrisBoundsExtent(const TArray<int32>& CellIds) const;

	/**
	 * Function used for arbitrary destruction (currently called based on bullet count in Supercell)
	 */
	void ForceRemoveSupercell(int32 SuperCellId);
	
	UFUNCTION(NetMulticast, Reliable)  
	void MulticastForceRemoveSupercell(int32 SuperCellId);

	/**
	 * Convert GridCellId to ChunkId
	 * @param GridCellId - Grid cell ID
	 * @return Corresponding ChunkId, INDEX_NONE if not found
	 */
	int32 GridCellIdToChunkId(int32 GridCellId) const;

	/**
	 * Remove mesh of detached cells via Boolean Subtract
	 * @param DetachedCellIds - Array of detached cell IDs
	 * @param OutRemovedMeshIsland - On success, the portion cut from original mesh (OriginalMesh ∩ ToolMesh)
	 * @return Whether removal succeeded
	 */
	bool RemoveTrianglesForDetachedCells(const TArray<int32>& DetachedCellIds, ADebrisActor* TargetDebrisActor = nullptr, TArray<int32>* OutToolMeshOverlappingCellIds = nullptr);

	/**
	 * Collect grid cell IDs that overlap with the ToolMesh shape (smoothed + DebrisExpandRatio scaled).
	 * Lightweight version of RemoveTrianglesForDetachedCells — builds ToolMesh only for cell collection,
	 * without boolean operations, DebrisToolMesh, or chunk processing.
	 *
	 * @param CellIds - Input cell IDs to build ToolMesh from
	 * @param OutOverlappingCellIds - Output: cell IDs overlapping the expanded ToolMesh
	 */
	void CollectToolMeshOverlappingCells(const TArray<int32>& CellIds, TArray<int32>& OutOverlappingCellIds);

	/** Build ToolMesh from sorted voxel piece (GreedyMesh + FillHoles + HC Laplacian Smoothing). */
	FDynamicMesh3 BuildSmoothedToolMesh(TArray<FIntVector>& SortedPiece);

	/** Collect grid cell IDs that overlap with the given mesh using SAT triangle-AABB intersection. */
	void CollectCellsOverlappingMesh(const FDynamicMesh3& Mesh, TArray<int32>& OutCellIds);

	FDynamicMesh3 GenerateGreedyMeshFromVoxels(const TArray<FIntVector>& InVoxels, FVector InCellOrigin, FVector InCellSize, double InBoxExpand = 1.0f );

	/** When Supercell is destroyed beyond threshold ratio */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|StructuralIntegrity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DestroyRatioThresholdForDebris = 0.8f;

	/**
	 * Density of debris. (g/cm^3)
	 * For realistic behavior, it is recommended to set this higher than the actual material density.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debris")
	float DebrisDensity = 1.0f;

	/** Maximum mass that debris can have. (kg) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debris")
	float MaxDebrisMass = 1000;
	
	FVector CachedToolForwardVector = FVector::ForwardVector;
	 
	//TODO: Find appropriate values and remove this
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debris", meta = (ClampMin = "1", ClampMax = "8"))
	int32 DebrisSplitCount = 1;

	//TODO: Find appropriate values and remove this
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debris")
	float MinDebrisSyncSize = 5.0f;

	/**
	 * Expands the removal region by this ratio when removing floating mesh islands. (1.2 = 120% of original size)
	 * Adjusting this based on grid cell size can help remove debris cleanly without leaving residual fragments.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debris", meta = (ClampMin = "1.5", ClampMax = "3.0"))
	float DebrisExpandRatio = 1.5f;

	/**
	 * Scale ratio for the debris mesh. (0.7 = 70% of original size)
	 * Smaller values may look unnatural, but can reduce debris getting stuck in the original mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debris", meta = (ClampMin = "0", ClampMax = "1.0"))
	float DebrisScaleRatio = 0.7f;

	void SpawnDebrisActor(FDynamicMesh3&& Source, const TArray<UMaterialInterface*>& Materials, ADebrisActor* TargetActgor = nullptr);

	/** Spawn Debris for dedicated server */
	void SpawnDebrisActorForDedicatedServer(const TArray<int32>& DetachedCellIds);

	/** Check if debris extraction via Boolean Intersection is possible
	 *  Requires valid BooleanProcessor and ChunkMesh
	 */
	bool CanExtractDebrisForClient() const;
	 
	/** Generate DebrisId */
	int32 GenerateDebrisId() { return NextDebrisId++; }

	/** Register local debris (client) */
	void RegisterLocalDebris(int32 InDebrisId, UProceduralMeshComponent* Mesh);
	
	/** Register to pending queue when Actor arrives first */
	void RegisterPendingDebrisActor(int32 InDebrisId, ADebrisActor* Actor);
	
	/** Find and remove local debris (client) */
	UProceduralMeshComponent* FindAndRemoveLocalDebris(int32 InDebrisId);
	
	/** Cleanup small fragments (isolated Connected Components) */
	void CleanupSmallFragments(const TSet<int32>& InDisconnectedCells); 

	/** Cleanup small fragments (calculates detached cells internally) */
	void CleanupSmallFragments(); 

	/**
	 * Spawn detached cells as debris actors
	 * @param DetachedCellIds - Array of detached cell IDs
	 * @param InitialLocation - Center position of debris group
	 * @param InitialVelocity - Initial velocity (explosion direction)
	 */
	void SpawnDebrisFromCells(const TArray<int32>& DetachedCellIds, const FVector& InitialLocation, const FVector& InitialVelocity);

	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|ChunkMesh")
	int32 GetChunkMeshCount() const { return ChunkMeshComponents.Num(); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ChunkMesh", meta = (ClampMin = "1" , ClampMax="10"))
	FIntVector SliceCount = FIntVector(2.0f, 2.0f, 2.0f);

	/** Grid cell size (cm). Smaller values increase resolution but cost more performance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|GridCell", meta = (ClampMin = "1.0"))
	FVector GridCellSize = FVector(10.0f);
	
	/** Floor anchor detection Z height threshold (cm, relative to MeshBounds.Min.Z) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|GridCell", meta = (ClampMin = "0.0"))
	float FloorHeightThreshold = 10.0f;

	UPROPERTY()
	FVector CachedRDMScale = FVector(1.0f, 1.0f, 1.0f);

	//////////////////////////////////////////////////////////////////////////
	// Detached Cell Smoothing (Staircase Artifact Reduction)
	//////////////////////////////////////////////////////////////////////////

	/** Laplacian Smoothing iterations when removing detached cells (0 to disable) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Debris", meta = (ClampMin = "0", ClampMax = "5"))
	int32 SmoothingIterations = 4;

	/** Laplacian Smoothing strength (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Debris", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float SmoothingStrength = 0.2f;

	/** HC Laplacian correction strength (0~1, prevents shrinkage) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced|Debris", meta = (ClampMin = "0.1", ClampMax = "0.8"))
	float HCBeta = 0.5f;

	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|ChunkMesh")
	int32 GetMaterialIDFromFaceIndex(int32 FaceIndex);

	/** Apply HC Laplacian Smoothing (Vollmer et al., 1999) when removing detached cells (staircase artifact reduction)
	 * @param Mesh - ToolMesh to smooth
 */
	void ApplyHCLaplacianSmoothing(FDynamicMesh3& Mesh);
private:
	/** Create mesh sections on ProceduralMeshComponent */
	void CreateDebrisMeshSections(
		UProceduralMeshComponent* Mesh,
        const TMap<int32, FMeshSectionData>& SectionDataByMaterial,
        const TArray<UMaterialInterface*>& InMaterials);
	
	/** Create local-only Debris Actor (no sync) */
	AActor* CreateLocalOnlyDebrisActor(
		UWorld* World,
		const FVector& SpawnLocation,
		const FVector& BoxExtent,
		const TMap<int32, FMeshSectionData>& SectionDataByMaterial,
		const TArray<UMaterialInterface*>& InMaterials
	);
	
	/** Apply physics and initial velocity to Debris */
	void ApplyDebrisPhysics(
		 UBoxComponent* CollisionBox,
		 const FVector& SpawnLocation,
		 const FVector& BoxExtent
	 );

#if WITH_EDITOR
public:
	/** Creates a GeometryCollection from SourceStaticMesh and builds chunk meshes. */
	void GenerateDestructibleChunks();

	/**
	 * Destroys all ChunkMeshComponents and reverts to the state before
	 * GenerateDestructibleChunks was called.
	 */
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh", meta = (DisplayName = "Revert Chunks"))
	void RevertChunksToSourceMesh();
private:
	/**
	 * Create and slice GeometryCollection from SourceStaticMesh.
	 * @param InSourceMesh Source StaticMesh
	 * @return Created GeometryCollection, nullptr on failure
	 */
	TObjectPtr<UGeometryCollection> CreateFracturedGC(TObjectPtr<UStaticMesh> InSourceMesh);

#endif
protected:

	//////////////////////////////////////////////////////////////////////////
	// Debug Display Settings (Debug text above actor)
	//////////////////////////////////////////////////////////////////////////

	/** Whether to show debug info text above actor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowDebugText = false;

	/** GridCell debug display (grid cell system) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowGridCellDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowDestroyedCells = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowCellSpawnPosition = false;


	/** Server collision box debug visualization (for listen server) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowServerCollisionDebug = false;

	/** Show ToolMesh/Intersection wireframe debug when removing mesh islands */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bDebugMeshIslandRemoval = false;

	/** Show small non-synced debris as red boxes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bDebugDrawDebris = false;

	/** SuperCell debug display */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowSupercellDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowSubCellDebug = false;

	UPROPERTY()
	float DebugDrawDuration = 5.0f;

	/** Recently directly destroyed cell IDs (for debug highlighting) */
	TSet<int32> RecentDirectDestroyedCellIds;
	
	/** Update debug text. Controls update frequency by only performing on mesh update */
	void UpdateDebugText();

	void DrawDebugText() const;

	/** Grid cell debug visualization */
	void DrawGridCellDebug();


	/** SuperCell debug visualization */
	void DrawSupercellDebug();

	/** SubCell Dbug */
	void DrawSubCellDebug();

	/** Server collision box debug visualization */
	void DrawServerCollisionDebug();

	bool bShouldDebugUpdate = true;

	FString DebugText;	

protected:
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	void TryAutoSetupFromParentStaticMesh();

	bool bAutoSetUpDone = false;
#endif

private:
	
	FTimerHandle CollisionUpdateTimerHandle;

	/** Delayed fragment cleanup timer handle */
	FTimerHandle FragmentCleanupTimerHandle;

public:
	bool bPendingCleanup = false;

private:
	/** Last destroyed cell region (for CleanupSmallFragments) */
	TSet<FIntVector> LastOccupiedCells;
	FVector LastCellSizeVec;

	int64 NextOpId = 1;
	int32 NextSequence = 0;

	// Server batching variables
	TArray<FRealtimeDestructionOp> PendingServerBatchOps;
	TArray<FCompactDestructionOp> PendingServerBatchOpsCompact;  // For compression
	float ServerBatchTimer = 0.0f;
	int32 ServerBatchSequence = 0;  // For compression sequence

	//////////////////////////////////////////////////////////////////////////
	// Batch Completion Tracking (for determining Boolean operation completion time)
	//////////////////////////////////////////////////////////////////////////

	/** Struct for batch completion tracking */
	struct FBooleanBatchTracker
	{
		int32 TotalCount = 0;      // Total Ops enqueued
		int32 CompletedCount = 0;  // Completed Ops (includes both success and failure)

		bool IsComplete() const { return CompletedCount >= TotalCount && TotalCount > 0; }
	};

	/** Active batch tracker map (BatchId → Tracker) */
	TMap<int32, FBooleanBatchTracker> ActiveBatchTrackers;

	/** Next batch ID */
	int32 NextBatchId = 0;

	/** Function called when Boolean batch completes */
	void OnBooleanBatchCompleted(int32 BatchId);

public:
	/** Called when IslandRemoval starts (accessed from BooleanProcessor) */
	void IncrementIslandRemovalCount() { ActiveIslandRemovalCount.fetch_add(1); }

	/** Called when IslandRemoval completes (accessed from BooleanProcessor) */
	void DecrementIslandRemovalCount() { ActiveIslandRemovalCount.fetch_sub(1); }

private:
	/** Active IslandRemoval counter (number of RemoveTriangles operations in progress) */
	std::atomic<int32> ActiveIslandRemovalCount{0};

	// Standalone detached cell processing timer
	float StandaloneDetachTimer = 0.0f;
	static constexpr float StandaloneDetachInterval = 0.1f;

	//////////////////////////////////////////////////////////////////////////
	// Late Join: Op History (maintained on server only)
	//////////////////////////////////////////////////////////////////////////

	/** All applied Op history (for Late Join sync, COND_InitialOnly) */
	UPROPERTY(ReplicatedUsing=OnRep_LateJoinOpHistory)
	TArray<FCompactDestructionOp> AppliedOpHistory;

	/** Late Join: All cell IDs destroyed so far (COND_InitialOnly) */
	UPROPERTY(ReplicatedUsing=OnRep_LateJoinDestroyedCells)
	TArray<int32> LateJoinDestroyedCells;

	TArray<FDestructionResult> PendingDestructionResults;

	/** Max Op history size (memory limit) */
	static constexpr int32 MaxOpHistorySize = 10000;

	/** Late Join data received/applied flags */
	bool bLateJoinOpsReceived = false;
	bool bLateJoinCellsReceived = false;
	bool bLateJoinApplied = false;

	//////////////////////////////////////////////////////////////////////////
	// Debris Physics Synchronization
	//////////////////////////////////////////////////////////////////////////
	// TODO [Refactoring planned]: Currently Component centrally manages all Debris sync
	//   → Planning to change so each Debris Actor handles its own sync
	//   - Create custom ADebrisActor class
	//   - Utilize Unreal's built-in Replication (bReplicates, bReplicateMovement, etc.)
	//   - Separation of concerns and reduced Component responsibility
	//////////////////////////////////////////////////////////////////////////

	/** Debris ID counter (increments identically on server/client) */
	int32 NextDebrisId = 0;

	/** Active Debris Actor tracking (DebrisID → Actor) */
	TMap<int32, TWeakObjectPtr<AActor>> ActiveDebrisActors;

	/** Local Debris mesh map (for client) */
	TMap<int32, TObjectPtr<UProceduralMeshComponent>> LocalDebrisMeshMap; 

	/** Pending queue when Actor arrives before local mesh */
	UPROPERTY()
	TMap<int32, TObjectPtr<ADebrisActor>> PendingDebrisActors;
	
	/** Debris physics sync timer */
	FTimerHandle DebrisPhysicsSyncTimerHandle;

	/** Debris physics sync interval (seconds) */
	static constexpr float DebrisPhysicsSyncInterval = 0.1f;

	/** Server: Broadcast physics state of all Debris to clients */
	void BroadcastDebrisPhysicsState();

	/** Multicast RPC: Debris physics state synchronization */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastSyncDebrisPhysics(int32 DebrisId, FVector Location, FRotator Rotation, FVector LinearVelocity, FVector AngularVelocity);

	TSharedPtr<FRealtimeBooleanProcessor, ESPMode::ThreadSafe> BooleanProcessor;

	bool InitializeFromStaticMeshInternal(UStaticMesh* InMesh, bool bForce);

	UDynamicMesh* CreateToolMeshFromRequest(const FRealtimeDestructionRequest& Request);

	void CopyMaterialsFromStaticMesh(UStaticMesh* InMesh);
	void CopyMaterialsFromStaticMeshComponent(UStaticMeshComponent* InComp);
	void CopyCollisionFromStaticMeshComponent(UStaticMeshComponent* InComp);


	// UActorComponent overrides
	virtual void OnRegister() override;
	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnUnregister() override;
	virtual void BeginDestroy() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

};
