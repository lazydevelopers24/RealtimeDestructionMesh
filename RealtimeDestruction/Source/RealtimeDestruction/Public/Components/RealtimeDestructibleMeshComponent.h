// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/DynamicMeshComponent.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "DestructionTypes.h"
#include "StructuralIntegrity/GridCellTypes.h"
#include "RealtimeDestructibleMeshComponent.generated.h"

class UGeometryCollection;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class FLifetimeProperty;
class FRealtimeBooleanProcessor;
class UBulletClusterComponent;
class UDecalMaterialDataAsset;

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
	
	/** Tool Shape 파라미터 (네트워크 직렬화용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FDestructionToolShapeParams ShapeParams;

	/** RTT 측정용 클라이언트 전송 시간 (클라이언트에서만 설정) */
	UPROPERTY()
	double ClientSendTime = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	int32 ChunkIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector ToolForwardVector = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector ToolCenterWorld = FVector::ZeroVector;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FVector DecalSize = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	bool bSpawnDecal = true;

	/** Decal Material (Projectile에서 조회한 결과) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	TObjectPtr<UMaterialInterface> DecalMaterial = nullptr;
	
	UPROPERTY()
	FName SurfaceType = FName("Default");

	UPROPERTY()
	bool bRandomRotation = false;
	
	/** Decal 설정 조회용 ID (네트워크 전송용) */
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
	int32 Sequence = 0; // Destruction Operation 순서. 서버에서 정하며, 0, 1, 2, 3, ... 순서로 수행해야 합니다.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	bool bIsPenetration = false;
};

/**
 * 압축된 파괴 요청 데이터 (언리얼 내장 NetQuantize 사용)
 *
 * 기존 FRealtimeDestructionRequest: ~320 bits
 * 압축 FCompactDestructionOp: ~102 bits (직렬화 시)
 *
 * 네트워크 대역폭 ~70% 절감
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FCompactDestructionOp
{
	GENERATED_BODY()

	// 위치: 1cm 정밀도 (직렬화 시 ~6 bytes)
	UPROPERTY()
	FVector_NetQuantize ImpactPoint;

	// 노말: 0.1cm 정밀도 - 방향이므로 더 정밀하게 (직렬화 시 ~6 bytes)
	UPROPERTY()
	FVector_NetQuantize10 ImpactNormal;

	// 총알 진행 방향 (직렬화 시 ~6 bytes)
	UPROPERTY()
	FVector_NetQuantize10 ToolForwardVector;

	// 반지름: 1-255 cm (1 byte)
	UPROPERTY()
	uint8 Radius = 10;

	// 시퀀스: 롤오버 허용 (2 bytes)
	UPROPERTY()
	uint16 Sequence = 0;

	// Tool Shape (1 byte)
	UPROPERTY()
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;

	// Shape 파라미터 (네트워크 직렬화용)
	UPROPERTY()
	FDestructionToolShapeParams ShapeParams;

	// Chunk Index (클라이언트가 계산, 1 byte)
	UPROPERTY()
	uint8 ChunkIndex = 0;

	UPROPERTY()
	FVector_NetQuantize DecalSize;

	// Decal 설정 조회용 ID
	UPROPERTY()
	FName DecalConfigID = FName("Default");

	// SurfaceType (데칼 조회용)
	UPROPERTY()
	FName SurfaceType = FName("Default");

	// 압축
	static FCompactDestructionOp Compress(const FRealtimeDestructionRequest& Request, int32 Seq);

	// 압축 해제
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

	// 필요한 다른 프로퍼티도 저장 가능
	UPROPERTY()
	bool bSavedIsInitialized = false;

	UPROPERTY()
	bool bSavedChunkMeshesValid = false;

	UPROPERTY()
	FIntVector SavedSliceCount = FIntVector::ZeroValue;

	UPROPERTY()
	bool bSavedShowGridCellDebug = false;

	UPROPERTY()
	// 포인터 대신 컴포넌트 이름 저장 (PIE 복제 시 이름으로 찾기 위함)
	TArray<FString> SavedChunkComponentNames;
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

/**
 * 실시간 파괴 가능 메시 컴포넌트
 *
 * [중요] Grid Cell 시스템은 월드 좌표계를 기준으로 생성됩니다.
 * 런타임 중 이 컴포넌트의 월드 스케일을 변경하면 Grid Cell과 실제 메시 간의
 * 불일치가 발생하여 파괴 판정이 정확하지 않게 됩니다.
 * 스케일 변경이 필요한 경우 BuildGridCells()를 다시 호출해야 합니다.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class REALTIMEDESTRUCTION_API URealtimeDestructibleMeshComponent : public UDynamicMeshComponent
{
	GENERATED_BODY()

	friend struct FRealtimeDestructibleMeshComponentInstanceData;
public:
	URealtimeDestructibleMeshComponent();
	URealtimeDestructibleMeshComponent(FVTableHelper& Helper);
	virtual ~URealtimeDestructibleMeshComponent() override;

	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh",meta = (DeprecatedFunction))
	bool InitializeFromStaticMesh(UStaticMesh* InMesh);

	UE_DEPRECATED(5.7, "Function has been deprecated. Do not Initalize this component directly form static mesh")
		UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh",
			meta = (DeprecatedFunction, DeprecationMessage = "Function has been deprecated. Do not Initalize this component directly form static mesh"))
	bool InitializeFromStaticMeshComponent(UStaticMeshComponent* InComp);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	void ResetToSourceMesh();

	/** DecalDataAsset 설정 (Projectile에서 호출) */
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|HoleDecal")
	void SetDecalDataAsset(UDecalMaterialDataAsset* InDecalDataAsset) { DecalDataAsset = InDecalDataAsset; }

	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|HoleDecal")
	UDecalMaterialDataAsset* GetDecalDataAsset() const { return DecalDataAsset; }

	// Destruction queue
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	FDestructionOpId EnqueueRequestLocal(const FRealtimeDestructionRequest& Request, bool bPenetration, UDecalComponent* TemporaryDecal = nullptr);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	int32 EnqueueBatch(const TArray<FRealtimeDestructionRequest>& Requests);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	bool RequestDestruction(const FRealtimeDestructionRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh")
	bool ExecuteDestructionInternal(const FRealtimeDestructionRequest& Request);
	
	// Replication
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerEnqueueOps(const TArray<FRealtimeDestructionRequest>& Requests);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastApplyOps(const TArray<FRealtimeDestructionOp>& Ops);

	/** 압축된 Multicast RPC (서버 → 클라이언트) */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastApplyOpsCompact(const TArray<FCompactDestructionOp>& CompactOps);

	/**
	 * 파괴된 셀 ID 전송 RPC (서버 → 클라이언트)
	 * 파괴 발생 시 즉시 전송하여 클라이언트 CellState 동기화
	 * @param DestroyedCellIds - 새로 파괴된 셀 ID들
	 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastDestroyedCells(const TArray<int32>& DestroyedCellIds);

	/**
	 * Detach 발생 신호 RPC (서버 → 클라이언트)
	 * 클라이언트가 자체 BFS를 실행하여 Detached 셀 계산
	 */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastDetachSignal();

	/** 파괴 요청 거부 RPC (서버 → 요청한 클라이언트) */
	UFUNCTION(Client, Reliable)
	void ClientDestructionRejected(uint16 Sequence, EDestructionRejectReason Reason);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|Replication")
	void ApplyOpsDeterministic(const TArray<FRealtimeDestructionOp>& Ops);

	/**
	 * 서버 배칭: 요청을 대기열에 추가
	 * 서버에서만 호출됨
	 */
	void EnqueueForServerBatch(const FRealtimeDestructionOp& Op);

	/**
	 * 서버 배칭: 대기열을 비우면서 Multicast
	 * 서버에서만 호출됨
	 */
	void FlushServerBatch();

	//////////////////////////////////////////////////////////////////////////
	// Server Validation (서버 검증)
	//////////////////////////////////////////////////////////////////////////

	/**
	 * 파괴 요청 검증 (서버에서 호출)
	 * @param Request 파괴 요청
	 * @param RequestingPlayer 요청한 플레이어 (nullptr이면 검증 스킵)
	 * @param OutReason 거부 사유 (실패 시)
	 * @return 검증 통과 여부
	 */
	bool ValidateDestructionRequest(const FRealtimeDestructionRequest& Request, APlayerController* RequestingPlayer, EDestructionRejectReason& OutReason);


	/** 서버 검증: 사거리 설정 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Validation")
	float MaxDestructionRange = 5000.0f;

	/** 서버 검증: 연사 제한 (초당 최대 파괴 횟수) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Validation")
	float MaxDestructionsPerSecond = 10.0f;

	/** 서버 검증: 단일 RPC 최대 요청 수 (초과 시 킥) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Validation", meta = (ClampMin = "1", ClampMax = "200"))
	int32 MaxRequestsPerRPC = 50;

	/** 서버 검증: 최대 허용 파괴 반경 (초과 시 킥) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Validation", meta = (ClampMin = "1.0"))
	float MaxAllowedRadius = 500.0f;

	/** 서버 검증: 시야 체크 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Validation")
	bool bEnableLineOfSightCheck = true;

	/** 연사 제한 추적 정보 */
	struct FRateLimitInfo
	{
		double WindowStartTime = 0.0;
		int32 RequestCount = 0;
	};

	/** 플레이어별 연사 제한 추적 (서버에서만 사용) */
	TMap<TWeakObjectPtr<APlayerController>, FRateLimitInfo> PlayerRateLimits;

	/** 연사 제한 체크 (서버에서 호출) */
	bool CheckRateLimit(APlayerController* Player);

	//////////////////////////////////////////////////////////////////////////
	// Server Batching Settings (서버 → 클라이언트 배칭)
	//////////////////////////////////////////////////////////////////////////
	/// 

	/**
	 * 서버 배칭 사용 여부
	 * true: 여러 클라이언트의 요청을 모아서 한 번에 Multicast (헤더 오버헤드 절감)
	 * false: 요청마다 개별 Multicast
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching")
	bool bUseServerBatching = true;

	/** 서버 배치 전송 간격 (초) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching", meta = (ClampMin = "0.008", ClampMax = "0.5"))
	float ServerBatchInterval = 0.016f;  // 16ms = 1프레임 (60fps 기준)

	/** 최대 서버 배치 크기 (이 개수에 도달하면 즉시 전송) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching", meta = (ClampMin = "1", ClampMax = "100"))
	int32 MaxServerBatchSize = 20;

	/**
	 * Multicast 압축 사용 여부
	 * true: 압축된 FCompactDestructionOp 사용 (~102 bits/요청)
	 * false: 기존 FRealtimeDestructionOp 사용 (~320 bits/요청)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|ServerBatching")
	bool bUseCompactMulticast = true;

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|Replication")
	bool BuildMeshSnapshot(FRealtimeMeshSnapshot& Out);

	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|Replication")
	bool ApplyMeshSnapshot(const FRealtimeMeshSnapshot& In);

	//////////////////////////////////////////////////////////////////////////
	// Late Join: Op 히스토리 기반 동기화
	//////////////////////////////////////////////////////////////////////////

	/** Op 히스토리 가져오기 (Late Join 동기화용, 서버에서만 유효) */
	const TArray<FCompactDestructionOp>& GetAppliedOpHistory() const { return AppliedOpHistory; }

	/** Op 히스토리 초기화 (메시 리셋 시 호출) */
	void ClearOpHistory() { AppliedOpHistory.Empty(); }

	// Events
	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructMeshInitialized OnInitialized;

	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructOpApplied OnOpApplied;

	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructBatchCompleted OnBatchCompleted;

	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructError OnError;

	/** 파괴 요청이 서버에서 거부되었을 때 (클라이언트에서만 호출됨) */
	UPROPERTY(BlueprintAssignable, Category = "RealtimeDestructibleMesh|Events")
	FOnDestructionRejected OnDestructionRejected;

	
	/** Clustering 변수 */
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
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	FName SurfaceType = FName("Default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Advanced")
	bool bEnableSubcell = true;
	
	/** 데이터 유지를 위한 함수 */
	virtual TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;

	/*
	 * 에디터에 노출하지 않는 함수
	 */
	void GetDestructionSettings(int32& OutMaxHoleCount, int32& OutMaxBatchSize);
	FGeometryScriptMeshBooleanOptions GetBooleanOptions() const { return BooleanOptions; }
	FRealtimeBooleanProcessor* GetBooleanProcessor() const { return BooleanProcessor.Get(); }

	/** ShapeParams로 ToolMeshPtr 재생성 (네트워크 수신 시 사용) */
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> CreateToolMeshPtrFromShapeParams(
		EDestructionToolShape ToolShape,
		const FDestructionToolShapeParams& ShapeParams);
	float GetAngleThreshold() const { return AngleThreshold; }
	double GetSubtractDurationLimit() const { return SubtractDurationLimit; }
	int32 GetInitInterval() const { return InitInterval; }
	void SetCurrentHoleCount(int32 Count) { CurrentHoleCount = Count; }

	void ApplyRenderUpdate();
	void ApplyCollisionUpdate(UDynamicMeshComponent* TargetComp);
	void ApplyCollisionUpdateAsync(UDynamicMeshComponent* TargetComp);

	bool CheckPenetration(const FRealtimeDestructionRequest& Request, float& OutPenetration);
	
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

	// 비트 연산은 원자적이지 않아서 GT 외에 호출할 때는 로직 수정 필요함
	void ClearChunkBusy(int32 ChunkIndex);

	void ClearAllChunkBusyBits(); 

	void SetChunkBits(int32 ChunkIndex, int32& BitIndex, int32& BitOffset);

	// 변형된 메시의 시각적(렌더링) 처리 즉시 업데이트하는 함수
	void ApplyBooleanOperationResult(FDynamicMesh3&& NewMesh, const int32 ChunkIndex, bool bDelayedCollisionUpdate);
	
	// 타겟메시의 idle이나 원하는 딜레이를 주고 Async로 collision 갱신하는 함수
	void RequestDelayedCollisionUpdate(UDynamicMeshComponent* TargetComp);

	/*************************************************/
	void SetSourceMeshEnabled(bool bSwitch);
protected:
	//////////////////////////////////////////////////////////////////////////
	// Mesh Settings
	//////////////////////////////////////////////////////////////////////////

	/**
	 * Dynamic Mesh로 변환할 원본 Static Mesh
	 *
	 * 에디터에서 이 속성을 설정하면 OnConstruction에서
	 * 자동으로 Dynamic Mesh로 변환됩니다.
	 *
	 * 지원 기능:
	 * - Material 자동 복사
	 * - UV, Normal, Tangent 보존
	 * - Collision 설정 복사
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Source")
	TObjectPtr<UStaticMesh> SourceStaticMesh;

	//////////////////////////////////////////////////////////////////////////
	// Destruction Settings
	//////////////////////////////////////////////////////////////////////////

	/** 관통 처리가 늦어진다면, 눈속임용 데칼*/

	UDecalComponent* SpawnTemporaryDecal(const FRealtimeDestructionRequest& Request);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|HoleDecal")
	TObjectPtr<UDecalMaterialDataAsset> DecalDataAsset = nullptr;
 
	// [deprecated]
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|HoleDecal")
	TObjectPtr<UMaterialInterface> HoleDecal = nullptr;

	// [deprecated]
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|HoleDecal")
	FVector DecalSize = FVector(10.0f, 10.0f, 10.0f);
 
	/** Decal Material (projectile에서 조회한 결과) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh")
	TObjectPtr<UMaterialInterface> DecalMaterial = nullptr;

	/**
	 * 이 메시가 받을 수 있는 최대 구멍 개수
	 *
	 * 성능 최적화를 위한 제한입니다.
	 * 구멍이 많아질수록 메시 복잡도가 증가하여
	 * Boolean 연산 시간이 길어집니다.
	 *
	 * 권장값:
	 * - 작은 오브젝트: 20-50
	 * - 큰 오브젝트: 50-100
	 * - 성능 우선: 20 이하
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options", meta = (ClampMin = 0, ClampMax = 1000))
	int32 MaxHoleCount = 1000;

	/**
	 * 현재까지 생성된 구멍 개수
	 *
	 * CreateBulletHole()이 성공할 때마다 1씩 증가합니다.
	 * MaxHoleCount에 도달하면 더 이상 구멍을 생성할 수 없습니다.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RealtimeDestructibleMesh|Status")
	int32 CurrentHoleCount = 0;

	/**
	 * Dynamic Mesh 초기화 완료 여부
	 *
	 * true: Static Mesh → Dynamic Mesh 변환 완료, 구멍 생성 가능
	 * false: 아직 초기화 안 됨, CreateBulletHole() 호출 시 실패
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RealtimeDestructibleMesh|Status")
	bool bIsInitialized = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options", meta = (ClampMin = 0, ClampMax = 500))
	float ThicknessOffset = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options")
	FGeometryScriptMeshBooleanOptions BooleanOptions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options", meta = (ClampMin = 1))
	int32 MaxBatchSize = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options")
	bool bAsyncEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options")
	bool bEnableMultiWorkers = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Replication")
	bool bDebugPenetration = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options", meta = (ClampMin = 0.001))
	float AngleThreshold = 0.001f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options", meta = (ClampMin = 0.0))
	double SubtractDurationLimit = 15.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Options", meta = (ClampMin = 0))
	int32 InitInterval = 50;

	//////////////////////////////////////////////////////////////////////////
	// Cell Mesh Parallel Processing
	//////////////////////////////////////////////////////////////////////////

	/**
	 * 에디터에서 미리 분할해둔 GeometryCollection
	 * Fracture Mode로 생성한 GC를 여기에 할당하면
	 * 런타임에 DynamicMesh로 추출하여 사용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|CellMesh")
	TObjectPtr<UGeometryCollection> FracturedGeometryCollection;

	/** Cell별 분리된 메시 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RealtimeDestructibleMesh|CellMesh")
	TArray<TObjectPtr<UDynamicMeshComponent>> ChunkMeshComponents;

	// PrimComp으로 Key값 설정, FHitResult의 GetComponent는 PrimitiveComp* 반환
	TMap<UPrimitiveComponent*, int32> ChunkIndexMap;

	/** 그리드 인덱스 -> ChunkId(CellMeshComponents 배열 인덱스) 매핑 테이블
	 *  슬라이싱 후 고정되며, BuildCellMeshesFromGeometryCollection에서 계산됨 */
	UPROPERTY()
	TArray<int32> GridToChunkMap;

	TArray<uint64> ChunkBusyBits;

	/** Multi Worker, Subtract 체크용 */
	TArray<uint64> ChunkSubtractBusyBits;

	/** Cell별 바운딩 박스 (빠른 충돌 체크용) */
	TArray<FBox> CellBounds;

	/** Cell 메시가 유효한지 (빌드 완료 여부) */
	UPROPERTY()
	bool bChunkMeshesValid = false;

	/** 격자 셀 캐시 (에디터에서 생성, 런타임 변경 없음) */
	UPROPERTY()
	FGridCellCache GridCellCache;

	/** 런타임 셀 상태 */
	UPROPERTY()
	FCellState CellState;

	//=========================================================================
	// Cell 기반 구조적 무결성 시스템
	//=========================================================================

	/** 구조적 무결성 시스템 활성화 여부 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|StructuralIntegrity")
	bool bEnableStructuralIntegrity = true;

	/** 양자화된 파괴 입력 히스토리 (NarrowPhase용) */
	UPROPERTY()
	TArray<FQuantizedDestructionInput> DestructionInputHistory;

	/** 현재 배치에서 변경된 청크 ID 집합 */
	TSet<int32> ModifiedChunkIds;

public:
	/**
	 * GeometryCollection에서 DynamicMesh 추출
	 * 에디터에서 Fracture Mode로 미리 분할해둔 GC 사용
	 * @return 추출된 메시 개수
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RealtimeDestructibleMesh|CellMesh", meta = (DisplayName = "Build Cell Meshes From GC"))
	int32 BuildChunkMeshesFromGeometryCollection();

	/** Cell 메시 유효 여부 */
	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|CellMesh")
	bool IsChunkMeshesValid() const { return bChunkMeshesValid; }

	/**
	 * 격자 셀 캐시 생성 (에디터에서 호출)
	 * SourceStaticMesh로부터 격자 셀을 생성합니다.
	 * @return 성공 여부
	 */
	UFUNCTION(BlueprintCallable, Category = "RealtimeDestructibleMesh|GridCell")
	bool BuildGridCells();

	/** 에디터 버튼: 격자 셀 빌드 */
	UFUNCTION(CallInEditor, Category = "GridCell")
	void EditorBuildGridCells();

	/** 격자 셀 캐시 유효 여부 */
	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|GridCell")
	bool IsGridCellCacheValid() const { return GridCellCache.IsValid(); }

private:
	/**
	 * GridToChunkMap 구축 (그리드 인덱스 -> ChunkId 매핑)
	 * 각 프래그먼트의 공간 위치를 기반으로 그리드 셀에 매핑
	 * BuildCellMeshesFromGeometryCollection에서 호출됨
	 */
	void BuildGridToChunkMap();

	void FindChunksAlongLineInternal(const FVector& WorldStart, const FVector& WorldEnd, TArray<int32>& OutChunkIndices);

public:
	/** GridCellCache 조회 (읽기 전용) */
	const FGridCellCache& GetGridCellCache() const { return GridCellCache; }

	/** CellState 조회 (읽기 전용) */
	const FCellState& GetCellState() const { return CellState; }

	/**
	 * 파괴 요청에 의해 영향받은 셀 상태 업데이트
	 * Boolean 파괴 처리와 함께 호출되어 Cell 파괴 판정 수행
	 *
	 * @param Request - 파괴 요청 정보
	 */
	void UpdateCellStateFromDestruction(const FRealtimeDestructionRequest& Request);

	/**
	 * GridCellId를 ChunkId로 변환
	 * @param GridCellId - 격자 셀 ID
	 * @return 해당하는 ChunkId, 없으면 INDEX_NONE
	 */
	int32 GridCellIdToChunkId(int32 GridCellId) const;

	/**
	 * 분리된 셀들의 메시를 Boolean Subtract로 제거
	 * @param DetachedCellIds - 분리된 셀 ID 배열
	 */
	void RemoveTrianglesForDetachedCells(const TArray<int32>& DetachedCellIds);

	/** 작은 파편(고립된 Connected Component) 정리 */
	void CleanupSmallFragments();

	/**
	 * 분리된 셀들을 파편 액터로 스폰
	 * @param DetachedCellIds - 분리된 셀 ID 배열
	 * @param InitialLocation - 파편 그룹 중심 위치
	 * @param InitialVelocity - 초기 속도 (폭발 방향)
	 */
	void SpawnDebrisFromCells(const TArray<int32>& DetachedCellIds, const FVector& InitialLocation, const FVector& InitialVelocity);

	//[deprecated]
	/** Cell 개수 반환 */
	//UFUNCTION(BlueprintPure, Category="RealtimeDestructibleMesh|CellMesh")
	//int32 GetChunkMeshCount() const { return CellMeshes.Num(); }
	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|CellMesh")
	int32 GetChunkMeshCount() const { return ChunkMeshComponents.Num(); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|CellMesh")
	FIntVector SliceCount;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|CellMesh")
	float SliceAngleVariation = 0.3f;

	/** 격자 셀 크기 (cm). 값이 작을수록 해상도가 높아지지만 성능 비용 증가 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|GridCell", meta = (ClampMin = "1.0"))
	FVector GridCellSize = FVector(5.0f);

	/** 바닥 Anchor 감지 Z 높이 임계값 (cm, MeshBounds.Min.Z 기준 상대값) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|GridCell", meta = (ClampMin = "0.0"))
	float FloorHeightThreshold = 10.0f;

	UFUNCTION(BlueprintPure, Category = "RealtimeDestructibleMesh|CellMesh")
	int32 GetMaterialIDFromFaceIndex(int32 FaceIndex);


#if WITH_EDITOR
	/**
	 * SourceStatic 메쉬로부터 GC를 생성, FracturedGeometryCollection에 저장합니다.
	 * 이후 GC가 DynamicMesh로 변환될 수 있도록 BuildCellMeshesFromGeometryCollection 메소드 호출까지 담당합니다.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RealtimeDestructibleMesh|CellMesh")
	void AutoFractureAndAssign();

	/** 파괴전 Mesh의 상태로 되돌리기 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RealtimeDestructibleMesh|CellMesh")
	void RevertFracture();

#endif
protected:

	//////////////////////////////////////////////////////////////////////////
	// Debug Display Settings (액터 위 디버그 텍스트 표시)
	//////////////////////////////////////////////////////////////////////////

	/** 액터 위에 디버그 정보 텍스트 표시 여부 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowDebugText = false;

	/** GridCell 디버그 표시 (격자 셀 시스템) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowGridCellDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowCellConnectivityLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowDestroyedCells = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|Debug")
	bool bShowCellSpawnPosition = false;

	/** 최근 직접 파괴된 셀 ID (디버그 강조 표시용) */
	TSet<int32> RecentDirectDestroyedCellIds;
	
	/** 디버그 텍스트 갱신. 메시 업데이트시에만 수행하는 식으로 업데이트 빈도 제어 */
	void UpdateDebugText();

	void DrawDebugText() const;

	/** 격자 셀 디버그 시각화 */
	void DrawGridCellDebug();

	bool bShouldDebugUpdate = true;

	FString DebugText;	

protected:
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FTimerHandle CollisionUpdateTimerHandle;

	/** 지연 파편 정리 타이머 핸들 */
	FTimerHandle FragmentCleanupTimerHandle;

	/** 마지막 파괴된 셀 영역 (CleanupSmallFragments용) */
	TSet<FIntVector> LastOccupiedCells;
	FVector LastCellSizeVec;

	int64 NextOpId = 1;
	int32 NextSequence = 0;

	// 서버 배칭용 변수
	TArray<FRealtimeDestructionOp> PendingServerBatchOps;
	TArray<FCompactDestructionOp> PendingServerBatchOpsCompact;  // 압축용
	float ServerBatchTimer = 0.0f;
	int32 ServerBatchSequence = 0;  // 압축용 시퀀스

	//////////////////////////////////////////////////////////////////////////
	// Late Join: Op 히스토리 (서버에서만 유지)
	//////////////////////////////////////////////////////////////////////////

	/** 적용된 모든 Op 히스토리 (Late Join 동기화용) */
	TArray<FCompactDestructionOp> AppliedOpHistory;

	/** Op 히스토리 최대 크기 (메모리 제한) */
	static constexpr int32 MaxOpHistorySize = 10000;

	TUniquePtr<FRealtimeBooleanProcessor> BooleanProcessor;

	bool InitializeFromStaticMeshInternal(UStaticMesh* InMesh, bool bForce);

	/** Cell 바운딩 박스 계산 */
	FBox CalculateCellBounds(int32 CellId) const;

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
