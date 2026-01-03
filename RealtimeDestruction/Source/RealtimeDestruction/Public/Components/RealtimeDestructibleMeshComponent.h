// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/DynamicMeshComponent.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "DestructionTypes.h"
#include "RealtimeDestructibleMeshComponent.generated.h"

class UGeometryCollection;

class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class FLifetimeProperty;
class FRealtimeBooleanProcessor;

//////////////////////////////////////////////////////////////////////////
// Enum Types
//////////////////////////////////////////////////////////////////////////

// Controls when collision is rebuilt after destruction ops are applied.
UENUM(BlueprintType)
enum class ERealtimeCollisionUpdateMode : uint8
{
	Batch, // Rebuild once after a batch of ops completes.
	PerHit // Rebuild collision after each hit/op is applied.
};

// Controls when the render mesh is refreshed after destruction ops.
UENUM(BlueprintType)
enum class ERealtimeRenderUpdateMode : uint8
{
	Auto,  // Refresh automatically: per hit in PerHit mode, once per batch in Batch mode.
	Manual // Caller is responsible for triggering render update.
};

// Controls how destruction state is replicated across the network.
UENUM(BlueprintType)
enum class ERealtimeDestructionReplicationMode : uint8
{
	None,               // No replication; local-only destruction.
	ServerSequencedOps, // Server batches/assigns order and multicasts ops to replay.
	FullSnapshot        // Periodic/initial resync snapshot; normally use ServerSequencedOps.
};

//////////////////////////////////////////////////////////////////////////
// Destruction Types
//////////////////////////////////////////////////////////////////////////

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDestructionOpId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	int64 Value = 0;
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FRealtimeDestructionRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	FVector ImpactPoint = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	FVector ImpactNormal = FVector::UpVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	float Depth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	int32 RandomSeed = 0;

	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> ToolMeshPtr = {};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;
	
	/** Tool Shape 파라미터 (네트워크 직렬화용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	FDestructionToolShapeParams ShapeParams;

	/** RTT 측정용 클라이언트 전송 시간 (클라이언트에서만 설정) */
	UPROPERTY()
	double ClientSendTime = 0.0;
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FRealtimeDestructionOp
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	FDestructionOpId OpId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	FRealtimeDestructionRequest Request;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	int32 Sequence = 0; // Destruction Operation 순서. 서버에서 정하며, 0, 1, 2, 3, ... 순서로 수행해야 합니다.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
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

	// 압축
	static FCompactDestructionOp Compress(const FRealtimeDestructionRequest& Request, int32 Seq);

	// 압축 해제
	FRealtimeDestructionRequest Decompress() const;
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FRealtimeMeshSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	int32 Version = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh")
	TArray<uint8> Payload;
};

//////////////////////////////////////////////////////////////////////////
// Delegates
//////////////////////////////////////////////////////////////////////////

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDestructMeshInitialized);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDestructOpApplied, const FRealtimeDestructionOp&, Op);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDestructBatchCompleted, int32, AppliedCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDestructError, FName, ErrorCode, const FString&, ErrorMessage);

//////////////////////////////////////////////////////////////////////////
// Class Declaration
//////////////////////////////////////////////////////////////////////////

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class REALTIMEDESTRUCTION_API URealtimeDestructibleMeshComponent : public UDynamicMeshComponent
{
	GENERATED_BODY()
	
public:
	URealtimeDestructibleMeshComponent();
	URealtimeDestructibleMeshComponent(FVTableHelper& Helper);
	virtual ~URealtimeDestructibleMeshComponent() override;

	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;

	// Initialization	
	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	bool InitializeFromStaticMesh(UStaticMesh* InMesh);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	bool InitializeFromStaticMeshComponent(UStaticMeshComponent* InComp);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	void ResetToSourceMesh();

	// Destruction queue
	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	FDestructionOpId EnqueueRequestLocal(const FRealtimeDestructionRequest& Request, bool bPenetration, UDecalComponent* TemporaryDecal = nullptr);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	int32 EnqueueBatch(const TArray<FRealtimeDestructionRequest>& Requests);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	int32 ProcessPendingOps(int32 MaxOpsThisFrame);	
	
	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh")
	bool RequestDestruction(const FRealtimeDestructionRequest& Request);

	// Options
	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetBooleanOptions(const FGeometryScriptMeshBooleanOptions& Options);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetSphereResolution(int32 StepsPhi, int32 StepsTheta);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetMaxOpsPerFrame(int32 MaxOps);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetAsyncEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetCollisionUpdateMode(ERealtimeCollisionUpdateMode Mode);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetRenderUpdateMode(ERealtimeRenderUpdateMode Mode);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Options")
	void SetMaxHoleCount(int32 MaxCount);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Status")
	int32 GetHoleCount() const;

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Status")
	int32 GetPendingOpCount() const;

	// Replication
	UFUNCTION(Server, Reliable)
	void ServerEnqueueOps(const TArray<FRealtimeDestructionRequest>& Requests);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastApplyOps(const TArray<FRealtimeDestructionOp>& Ops);

	/** 압축된 Multicast RPC (서버 → 클라이언트) */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastApplyOpsCompact(const TArray<FCompactDestructionOp>& CompactOps);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Replication")
	void SetReplicationMode(ERealtimeDestructionReplicationMode Mode);

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Replication")
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
	// Server Batching Settings (서버 → 클라이언트 배칭)
	//////////////////////////////////////////////////////////////////////////

	/**
	 * 서버 배칭 사용 여부
	 * true: 여러 클라이언트의 요청을 모아서 한 번에 Multicast (헤더 오버헤드 절감)
	 * false: 요청마다 개별 Multicast
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|ServerBatching")
	bool bUseServerBatching = true;

	/** 서버 배치 전송 간격 (초) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|ServerBatching", meta=(ClampMin="0.008", ClampMax="0.5"))
	float ServerBatchInterval = 0.016f;  // 16ms = 1프레임 (60fps 기준)

	/** 최대 서버 배치 크기 (이 개수에 도달하면 즉시 전송) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|ServerBatching", meta=(ClampMin="1", ClampMax="100"))
	int32 MaxServerBatchSize = 20;

	/**
	 * Multicast 압축 사용 여부
	 * true: 압축된 FCompactDestructionOp 사용 (~102 bits/요청)
	 * false: 기존 FRealtimeDestructionOp 사용 (~320 bits/요청)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|ServerBatching")
	bool bUseCompactMulticast = true;

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Replication")
	bool BuildMeshSnapshot(FRealtimeMeshSnapshot& Out) const;

	UFUNCTION(BlueprintCallable, Category="RealtimeDestructibleMesh|Replication")
	bool ApplyMeshSnapshot(const FRealtimeMeshSnapshot& In);

	// Events
	UPROPERTY(BlueprintAssignable, Category="RealtimeDestructibleMesh|Events")
	FOnDestructMeshInitialized OnInitialized;

	UPROPERTY(BlueprintAssignable, Category="RealtimeDestructibleMesh|Events")
	FOnDestructOpApplied OnOpApplied;

	UPROPERTY(BlueprintAssignable, Category="RealtimeDestructibleMesh|Events")
	FOnDestructBatchCompleted OnBatchCompleted;

	UPROPERTY(BlueprintAssignable, Category="RealtimeDestructibleMesh|Events")
	FOnDestructError OnError;
	 
public:
	/*
	 * 에디터에 노출하지 않는 함수
	 */
	void GetDestructionSettings(int32& OutMaxHoleCount, int32& OutMaxOpsPerFrame, int32& OutMaxBatchSize);
	FGeometryScriptMeshBooleanOptions GetBooleanOptions() const { return BooleanOptions; }
	FRealtimeBooleanProcessor* GetBooleanProcessor() const { return BooleanProcessor.Get(); }
	FDynamicMesh3 GetToolMesh(EDestructionToolShape ToolShape, FDestructionToolShapeParams ToolParam);

	/** ShapeParams로 ToolMeshPtr 재생성 (네트워크 수신 시 사용) */
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> CreateToolMeshPtrFromShapeParams(
		EDestructionToolShape ToolShape,
		const FDestructionToolShapeParams& ShapeParams);
	float GetAngleThreshold() const { return AngleThreshold; }
	double GetSubtractDurationLimit() const { return SubtractDurationLimit; }
	int32 GetMaxOpCount() const { return MaxOpCount; }
	void SetCurrentHoleCount(int32 Count) { CurrentHoleCount = Count; }
	bool IsInitialized() const { return bIsInitialized; }
	
	void ApplyRenderUpdate();
	void ApplyCollisionUpdate();

	bool CheckPenetration(const FRealtimeDestructionRequest& Request, float& OutPenetration);

	void GetParallelSettings(int32& OutThreshold, int32& OutMaxThreads);
	
	void SettingAsyncOption(bool& OutParallelEnabled, bool& OutMultiWorker) ;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Source")
	TObjectPtr<UStaticMesh> SourceStaticMesh;

	//////////////////////////////////////////////////////////////////////////
	// Destruction Settings
	//////////////////////////////////////////////////////////////////////////
	
	/** 관통 처리가 늦어진다면, 눈속임용 데칼*/ 

	UDecalComponent* SpawnTemporaryDecal(const FRealtimeDestructionRequest& Request);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|HoleDecal")
	TObjectPtr<UMaterialInterface> HoleDecal = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|HoleDecal")
	FVector DecalSize = FVector(10.0f, 10.0f, 10.0f);
	 

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=1, ClampMax=1000))
	int32 MaxHoleCount = 100;

	/**
	 * 현재까지 생성된 구멍 개수
	 *
	 * CreateBulletHole()이 성공할 때마다 1씩 증가합니다.
	 * MaxHoleCount에 도달하면 더 이상 구멍을 생성할 수 없습니다.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="RealtimeDestructibleMesh|Status")
	int32 CurrentHoleCount = 0;

	/**
	 * Dynamic Mesh 초기화 완료 여부
	 *
	 * true: Static Mesh → Dynamic Mesh 변환 완료, 구멍 생성 가능
	 * false: 아직 초기화 안 됨, CreateBulletHole() 호출 시 실패
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="RealtimeDestructibleMesh|Status")
	bool bIsInitialized = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=0, ClampMax=500))
	float ThicknessOffset = 5.0f;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options")
	FGeometryScriptMeshBooleanOptions BooleanOptions;

	/*
	 * SharedPtr로 변경
	 * Projectile에서 받아 올 때도 Shared로 하면 좋을 것 같아서 바꿈
	 * Projectile의 ToolMesh는 Runtime에서 변하지 않을 듯
	 */
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> SphereTemplatePtr;
	
	FDynamicMesh3 SphereTemplate;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=3, ClampMax=64))
	int32 SphereStepsPhi = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=3, ClampMax=128))
	int32 SphereStepsTheta = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=1))
	int32 MaxOpsPerFrame = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=1))
	int32 MaxBatchSize = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=1))
	int32 ParallelThreshold = 12;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=1))
	int32 MaxParallelThreads = 12;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=1))
	bool bEnableParallel = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options")
	bool bAsyncEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options")
	bool bEnableMultiWorkers = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options")
	ERealtimeCollisionUpdateMode CollisionUpdateMode = ERealtimeCollisionUpdateMode::Batch;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options")
	ERealtimeRenderUpdateMode RenderUpdateMode = ERealtimeRenderUpdateMode::Auto;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Replication")
	ERealtimeDestructionReplicationMode ReplicationMode = ERealtimeDestructionReplicationMode::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Replication")
	bool bDebugPenetration = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=0.001))
	float AngleThreshold = 0.001f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=0.0))
	double SubtractDurationLimit = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options", meta=(ClampMin=0))
	int32 MaxOpCount = 0;

	/**
	 * GT 복사 블로킹 최적화 사용 여부
	 *
	 * true: 캐시 기반 최적화 (워커에서 복사, GT 블로킹 최소화)
	 * false: 기존 방식 (GT에서 복사)
	 *
	 * Unreal Insights에서 다음 스코프로 비교 가능:
	 * - Optimized: *_CopyInWorker_Optimized, *_CacheAndSetMesh_Optimized
	 * - Legacy: *_CopyMesh_GT_Legacy, *_SetMesh_Legacy
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Options")
	bool bUseCachedMeshOptimization = true;

	//////////////////////////////////////////////////////////////////////////
	// Cell Mesh Parallel Processing
	//////////////////////////////////////////////////////////////////////////

	/** Cell별 메시 분리 모드 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|CellMesh")
	bool bUseCellMeshes = false;

	/**
	 * 에디터에서 미리 분할해둔 GeometryCollection
	 * Fracture Mode로 생성한 GC를 여기에 할당하면
	 * 런타임에 DynamicMesh로 추출하여 사용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|CellMesh",
		meta=(EditCondition="bUseCellMeshes"))
	TObjectPtr<UGeometryCollection> FracturedGeometryCollection;

	/** Cell별 분리된 메시 */
	TArray<TSharedPtr<UE::Geometry::FDynamicMesh3>> CellMeshes;

	/** Cell별 바운딩 박스 (빠른 충돌 체크용) */
	TArray<FBox> CellBounds;

	/** Cell 메시가 유효한지 (빌드 완료 여부) */
	bool bCellMeshesValid = false;

public:
	/**
	 * GeometryCollection에서 DynamicMesh 추출
	 * 에디터에서 Fracture Mode로 미리 분할해둔 GC 사용
	 * @return 추출된 메시 개수
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category="RealtimeDestructibleMesh|CellMesh", meta=(DisplayName="Build Cell Meshes From GC"))
	int32 BuildCellMeshesFromGeometryCollection();

	/** Cell 메시 유효 여부 */
	UFUNCTION(BlueprintPure, Category="RealtimeDestructibleMesh|CellMesh")
	bool IsCellMeshesValid() const { return bCellMeshesValid; }

	/** Cell 개수 반환 */
	UFUNCTION(BlueprintPure, Category="RealtimeDestructibleMesh|CellMesh")
	int32 GetCellMeshCount() const { return CellMeshes.Num(); }

	/** Cell 메시 디버그 시각화 (각 Cell을 다른 색상으로 표시) */
	UFUNCTION(CallInEditor, BlueprintCallable, Category="RealtimeDestructibleMesh|CellMesh")
	void DrawCellMeshesDebug();
	 

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|CellMesh")
	FIntVector SliceCount;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RealtimeDestructibleMesh|CellMesh")
	float SliceAngleVariation = 0.3f;
	 
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "RealtimeDestructibleMesh|CellMesh")
	void AutoFractureAndAssign(); 

protected:



	//////////////////////////////////////////////////////////////////////////
	// Debug Display Settings (액터 위 디버그 텍스트 표시)
	//////////////////////////////////////////////////////////////////////////

	/** 액터 위에 디버그 정보 텍스트 표시 여부 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Debug")
	bool bShowDebugText = false;

	/** 디버그 텍스트 표시 위치 오프셋 (액터 위치 기준) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Debug")
	FVector DebugTextOffset = FVector(0.0f, 0.0f, 250.0f);

	/** 디버그 텍스트 색상 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Debug")
	FColor DebugTextColor = FColor::Yellow;

	/** Cell 메시 와이어프레임 디버그 표시 (PIE에서 자동으로 그려짐) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="RealtimeDestructibleMesh|Debug")
	bool bShowCellMeshDebug = false;

protected:
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	UPROPERTY()
	TArray<FRealtimeDestructionOp> PendingOps;

	int64 NextOpId = 1;
	int32 NextSequence = 0;

	bool bSphereTemplateReady = false;

	// 서버 배칭용 변수
	TArray<FRealtimeDestructionOp> PendingServerBatchOps;
	TArray<FCompactDestructionOp> PendingServerBatchOpsCompact;  // 압축용
	float ServerBatchTimer = 0.0f;
	int32 ServerBatchSequence = 0;  // 압축용 시퀀스

	TUniquePtr<FRealtimeBooleanProcessor> BooleanProcessor;

	bool InitializeFromStaticMeshInternal(UStaticMesh* InMesh, bool bForce);

	/** Cell 바운딩 박스 계산 */
	FBox CalculateCellBounds(int32 CellId) const;


	bool EnsureSphereTemplate();

	UDynamicMesh* CreateToolMeshFromRequest(const FRealtimeDestructionRequest& Request);

	///////////////////////////
	/*
	 * BooleanProcessor로 리팩토링 예정
	 * 모든 불리언 연산은 BooleanProcessor에서 처리할거임
	 */
	bool ApplyDestructionRequestInternal(const FRealtimeDestructionRequest& Request);
	///////////////////////////

	// 파괴 요청 함수
	bool ApplyOpImmediate(const FRealtimeDestructionRequest& Request);
	
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

	// Trace 활성화 상태 (비-쉬핑 빌드에서 사용)
	static bool bIsTraceEnabled;
};
