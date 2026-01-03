// Fill out your copyright notice in the Description page of Project Settings.

#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Materials/MaterialInstanceDynamic.h"

// GeometryCollection
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionComponent.h"

#if WITH_EDITOR
#include "GeometryCollection/GeometryCollectionConversion.h"
#include "FractureEngineFracturing.h"
#endif

//////////////////////////////////////////////////////////////////////////
// FCompactDestructionOp 구현 (언리얼 내장 NetQuantize 사용)
//////////////////////////////////////////////////////////////////////////

bool URealtimeDestructibleMeshComponent::bIsTraceEnabled = false;

FCompactDestructionOp FCompactDestructionOp::Compress(const FRealtimeDestructionRequest& Request, int32 Seq)
{
	FCompactDestructionOp Compact;

	// FVector_NetQuantize는 FVector와 호환 - 자동 변환
	Compact.ImpactPoint = Request.ImpactPoint;
	Compact.ImpactNormal = Request.ImpactNormal;

	// 반지름 압축 (1-255 cm) - ShapeParams에서 가져옴
	Compact.Radius = static_cast<uint8>(FMath::Clamp(Request.ShapeParams.Radius, 1.0f, 255.0f));

	// 시퀀스 (롤오버)
	Compact.Sequence = static_cast<uint16>(Seq & 0xFFFF);

	// ToolShape와 ShapeParams 복사
	Compact.ToolShape = Request.ToolShape;
	Compact.ShapeParams = Request.ShapeParams;

	return Compact;
}

FRealtimeDestructionRequest FCompactDestructionOp::Decompress() const
{
	FRealtimeDestructionRequest Request;

	// FVector_NetQuantize → FVector 자동 변환
	Request.ImpactPoint = ImpactPoint;
	Request.ImpactNormal = FVector(ImpactNormal).GetSafeNormal();

	// ToolShape와 ShapeParams 복원
	Request.ToolShape = ToolShape;
	Request.ShapeParams = ShapeParams;

	// Depth 설정 (Shape에 따라)
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		Request.Depth = ShapeParams.Height;
		break;
	case EDestructionToolShape::Sphere:
		Request.Depth = ShapeParams.Radius;
		break;
	default:
		Request.Depth = ShapeParams.Height;
		break;
	}

	return Request;
}

//////////////////////////////////////////////////////////////////////////

#include "Components/StaticMeshComponent.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "UDynamicMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Debug/DestructionDebugger.h"
#include "HAL/PlatformTime.h"
#include "BooleanProcessor/RealtimeBooleanProcessor.h"
#include "Components/DecalComponent.h"

URealtimeDestructibleMeshComponent::URealtimeDestructibleMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = true;  // 서버 배칭용
	SetMobility(EComponentMobility::Movable);
	SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SetCollisionProfileName(TEXT("BlockAll"));
	SetCastShadow(true);

	/*
	 * 초기값 false
	 * 마지막 불리언 연산 시 true로 변경
	 */
	BooleanOptions.bFillHoles = false;
	BooleanOptions.bSimplifyOutput = false;
}

URealtimeDestructibleMeshComponent::URealtimeDestructibleMeshComponent(FVTableHelper& Helper)
{
}

URealtimeDestructibleMeshComponent::~URealtimeDestructibleMeshComponent()
{
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->Shutdown();
		BooleanProcessor.Reset();
	}
}

UMaterialInterface* URealtimeDestructibleMeshComponent::GetMaterial(int32 ElementIndex) const
{
	if (OverrideMaterials.IsValidIndex(ElementIndex))
	{
		if (UMaterialInterface* OverrideMaterial = OverrideMaterials[ElementIndex])
		{
			return OverrideMaterial;
		}
	}

	return Super::GetMaterial(ElementIndex);
}

bool URealtimeDestructibleMeshComponent::InitializeFromStaticMesh(UStaticMesh* InMesh)
{
	SourceStaticMesh = InMesh;
	return InitializeFromStaticMeshInternal(InMesh, false);
}

bool URealtimeDestructibleMeshComponent::InitializeFromStaticMeshComponent(UStaticMeshComponent* InComp)
{
	if (!InComp || !InComp->GetStaticMesh())
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: StaticMeshComponent or StaticMesh is null"));
		return false;
	}

	SourceStaticMesh = InComp->GetStaticMesh();
	SetWorldTransform(InComp->GetComponentTransform());
	SetCastShadow(InComp->CastShadow);

	if (!InitializeFromStaticMeshInternal(SourceStaticMesh, false))
	{
		return false;
	}

	CopyMaterialsFromStaticMeshComponent(InComp);
	CopyCollisionFromStaticMeshComponent(InComp);

	InComp->SetVisibility(false);
	InComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	return true;
}

void URealtimeDestructibleMeshComponent::ResetToSourceMesh()
{
	if (!SourceStaticMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: No source mesh to reset to"));
		return;
	}

	/*
	 * SourceMesh가 리셋되면 이전의 BooleanProcessor 작업을 무효화 해야함
	 * 
	 */
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->CancelAllOperations();
		BooleanProcessor->SetWorkInFlight(false);
	}

	PendingOps.Reset();
	CurrentHoleCount = 0;
	bIsInitialized = false;
	InitializeFromStaticMeshInternal(SourceStaticMesh, true);
}

// 현재는 RequestDestruction에서만 호출됨
FDestructionOpId URealtimeDestructibleMeshComponent::EnqueueRequestLocal(const FRealtimeDestructionRequest& Request, bool bIsPenetration, UDecalComponent* TemporaryDecal)
{
	if (!BooleanProcessor.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Boolean Processor is null"));
		return FDestructionOpId();
	}
	FRealtimeDestructionOp Op;
	Op.OpId.Value = NextOpId++;
	Op.Sequence = NextSequence++;
	Op.Request = Request;
	Op.bIsPenetration = bIsPenetration;

	// Add operation to queue
	BooleanProcessor->EnqueueOp(MoveTemp(Op), TemporaryDecal);

	// Tick마다 하는 걸로 변경
	// Start Boolean Operation
	if (!bEnableMultiWorkers)
	{
		BooleanProcessor->KickProcessIfNeeded();
	}

	return Op.OpId;
}

int32 URealtimeDestructibleMeshComponent::EnqueueBatch(const TArray<FRealtimeDestructionRequest>& Requests)
{
	int32 AddedCount = 0;
	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		// 이 함수는 안쓰는 것 같으니 true로 하드코딩 
		EnqueueRequestLocal(Request, true);
		++AddedCount;
	}

	return AddedCount;
}

int32 URealtimeDestructibleMeshComponent::ProcessPendingOps(int32 MaxOpsThisFrame)
{
	const int32 MaxOps = (MaxOpsThisFrame > 0) ? MaxOpsThisFrame : MaxOpsPerFrame;
	const int32 OpsToProcess = FMath::Min(MaxOps, PendingOps.Num());
	if (OpsToProcess <= 0)
	{
		return 0;
	}

	const bool bPerHitUpdates = (CollisionUpdateMode == ERealtimeCollisionUpdateMode::PerHit);
	int32 AppliedCount = 0;

	for (int32 Index = 0; Index < OpsToProcess; ++Index)
	{
		const FRealtimeDestructionOp& Op = PendingOps[Index];
		if (ApplyDestructionRequestInternal(Op.Request))
		{
			++AppliedCount;
			OnOpApplied.Broadcast(Op);

			if (bPerHitUpdates)
			{
				if (RenderUpdateMode == ERealtimeRenderUpdateMode::Auto)
				{
					ApplyRenderUpdate();
				}
				ApplyCollisionUpdate();
			}
		}
	}

	PendingOps.RemoveAt(0, OpsToProcess);

	if (AppliedCount > 0 && !bPerHitUpdates)
	{
		if (RenderUpdateMode == ERealtimeRenderUpdateMode::Auto)
		{
			ApplyRenderUpdate();
		}
		ApplyCollisionUpdate();
	}

	if (AppliedCount > 0)
	{
		OnBatchCompleted.Broadcast(AppliedCount);
	}

	return AppliedCount;
}

bool URealtimeDestructibleMeshComponent::ApplyOpImmediate(const FRealtimeDestructionRequest& Request)
{
	if (!ApplyDestructionRequestInternal(Request))
	{
		return false;
	}

	FRealtimeDestructionOp Op;
	Op.OpId.Value = NextOpId++;
	Op.Sequence = NextSequence++;
	Op.Request = Request;

	OnOpApplied.Broadcast(Op);

	if (RenderUpdateMode == ERealtimeRenderUpdateMode::Auto)
	{
		ApplyRenderUpdate();
	}
	ApplyCollisionUpdate();
	OnBatchCompleted.Broadcast(1);

	return true;
}

// Projectile에서 호출해줌
bool URealtimeDestructibleMeshComponent::RequestDestruction(const FRealtimeDestructionRequest& Request)
{
	if (CurrentHoleCount >= MaxHoleCount)
	{
		return false;
	}
	// 관통, 비관통 여부 확인, broadphase와 같은 효과 
	float AdjustPenetration;
	bool bIsPenetration = CheckPenetration(Request, AdjustPenetration);
	 
	if (bAsyncEnabled)
	{
		UDecalComponent* TempDecal = nullptr;
		if (!bIsPenetration)
		{
			TempDecal = SpawnTemporaryDecal(Request);
		}

		// 기본 관통을 Enqeue
		EnqueueRequestLocal(Request, bIsPenetration, TempDecal);
		 
		// Offset에 따라 추가 관통처리
		if (bIsPenetration)
		{
			FRealtimeDestructionRequest PenetrationRequest = Request;
		
			/*
			 * deprecated_realdestruction
			 */
			// Cylinder 중심을 벽 중간으로 이동 (Normal 반대 방향으로 Height/2만큼)
			FVector Offset = Request.ImpactNormal * (-AdjustPenetration * 0.5f);
			PenetrationRequest.ImpactPoint = Request.ImpactPoint + Offset;
			PenetrationRequest.ToolShape = EDestructionToolShape::Cylinder;

			if (bDebugPenetration)
			{  
				// 시각화
				DrawDebugLine(GetWorld(), Request.ImpactPoint, PenetrationRequest.ImpactPoint,
					FColor::Red, false, 5.0f, 0, 3.0f);
			}

			// 구멍을 추가로 내주는거니, Decal을 필요없다. 
			EnqueueRequestLocal(PenetrationRequest, true, nullptr);
 		}
		return true;
	}

	else
	{
		return ApplyOpImmediate(Request);
	}
}

void URealtimeDestructibleMeshComponent::SetBooleanOptions(const FGeometryScriptMeshBooleanOptions& Options)
{
	BooleanOptions = Options;
}

void URealtimeDestructibleMeshComponent::SetSphereResolution(int32 StepsPhi, int32 StepsTheta)
{
	SphereStepsPhi = FMath::Max(3, StepsPhi);
	SphereStepsTheta = FMath::Max(3, StepsTheta);
}

void URealtimeDestructibleMeshComponent::SetMaxOpsPerFrame(int32 MaxOps)
{
	MaxOpsPerFrame = FMath::Max(1, MaxOps);
}

void URealtimeDestructibleMeshComponent::SetAsyncEnabled(bool bEnabled)
{
	bAsyncEnabled = bEnabled;
}

void URealtimeDestructibleMeshComponent::SetCollisionUpdateMode(ERealtimeCollisionUpdateMode Mode)
{
	CollisionUpdateMode = Mode;
}

void URealtimeDestructibleMeshComponent::SetRenderUpdateMode(ERealtimeRenderUpdateMode Mode)
{
	RenderUpdateMode = Mode;
}

void URealtimeDestructibleMeshComponent::SetMaxHoleCount(int32 MaxCount)
{
	MaxHoleCount = FMath::Max(1, MaxCount);
}

int32 URealtimeDestructibleMeshComponent::GetHoleCount() const
{
	return CurrentHoleCount;
}

int32 URealtimeDestructibleMeshComponent::GetPendingOpCount() const
{
	return PendingOps.Num();
}

void URealtimeDestructibleMeshComponent::ServerEnqueueOps_Implementation(const TArray<FRealtimeDestructionRequest>& Requests)
{
	// 서버에서 즉시 처리하고 모든 클라이언트에 Multicast
	UE_LOG(LogTemp, Warning, TEXT("ServerEnqueueOps: 서버에서 %d개 요청 수신"), Requests.Num());
	TArray<FRealtimeDestructionOp> Ops;
	Ops.Reserve(Requests.Num());

	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		FRealtimeDestructionOp Op;
		Op.OpId.Value = NextOpId++;
		Op.Sequence = NextSequence++;
		Op.Request = Request;
		Ops.Add(Op);
	}

	// 모든 클라이언트(서버 포함)에 Multicast하여 동기화
	// UE_LOG(LogTemp, Warning, TEXT("ServerEnqueueOps: MulticastApplyOps 호출"));
	MulticastApplyOps(Ops);
}

void URealtimeDestructibleMeshComponent::MulticastApplyOps_Implementation(const TArray<FRealtimeDestructionOp>& Ops)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		ApplyOpsDeterministic(Ops);
		return;
	}

	// 네트워크 역할 확인
	const ENetMode NetMode = World->GetNetMode();

	// 클라이언트에서 수신 데이터 기록 및 RTT 측정
	if (NetMode == NM_Client)
	{
		if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
		{
			// 수신 데이터 크기 기록 (비압축)
			constexpr int32 UNCOMPRESSED_OP_SIZE = 40;
			constexpr int32 RPC_OVERHEAD = 8;
			Debugger->RecordBytesReceived(Ops.Num() * UNCOMPRESSED_OP_SIZE + RPC_OVERHEAD);

			for (const FRealtimeDestructionOp& Op : Ops)
			{
				// ClientSendTime이 설정되어 있으면 RTT 계산
				if (Op.Request.ClientSendTime > 0.0)
				{
					double CurrentTime = FPlatformTime::Seconds();
					float RTTMs = static_cast<float>((CurrentTime - Op.Request.ClientSendTime) * 1000.0);
					Debugger->RecordRTT(RTTMs);
				}
			}
		}
	}

	// UE_LOG(LogTemp, Warning, TEXT("MulticastApplyOps: [%s] %d개 Op 적용"), *RoleName, Ops.Num());
	ApplyOpsDeterministic(Ops);
}

void URealtimeDestructibleMeshComponent::MulticastApplyOpsCompact_Implementation(const TArray<FCompactDestructionOp>& CompactOps)
{
	// 클라이언트에서 수신 데이터 크기 기록 (압축)
	if (UWorld* World = GetWorld())
	{
		if (World->GetNetMode() == NM_Client)
		{
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				constexpr int32 COMPACT_OP_SIZE = 15;
				constexpr int32 RPC_OVERHEAD = 8;
				Debugger->RecordBytesReceived(CompactOps.Num() * COMPACT_OP_SIZE + RPC_OVERHEAD);
			}
		}
	}

	// 압축 해제 후 적용
	TArray<FRealtimeDestructionOp> Ops;
	Ops.Reserve(CompactOps.Num());

	for (const FCompactDestructionOp& CompactOp : CompactOps)
	{
		FRealtimeDestructionOp Op;
		Op.Request = CompactOp.Decompress();
		Ops.Add(Op);
	}

	ApplyOpsDeterministic(Ops);
}

void URealtimeDestructibleMeshComponent::SetReplicationMode(ERealtimeDestructionReplicationMode Mode)
{
	ReplicationMode = Mode;
}

void URealtimeDestructibleMeshComponent::ApplyOpsDeterministic(const TArray<FRealtimeDestructionOp>& Ops)
{
	if (Ops.IsEmpty())
	{
		return;
	}

	// 서버는 이미 로컬에서 처리했으므로 Multicast 수신 시 스킵
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	for (const FRealtimeDestructionOp& Op : Ops)
	{
		// 클라이언트: ToolMeshPtr가 없으면 ShapeParams로 재생성
		FRealtimeDestructionRequest ModifiableRequest = Op.Request;
		if (!ModifiableRequest.ToolMeshPtr.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("[Client] ToolShape: %d, ShapeParams - Radius: %.2f, Height: %.2f, RadiusSteps: %d"),
				static_cast<int32>(ModifiableRequest.ToolShape),
				ModifiableRequest.ShapeParams.Radius,
				ModifiableRequest.ShapeParams.Height,
				ModifiableRequest.ShapeParams.RadiusSteps);

			ModifiableRequest.ToolMeshPtr = CreateToolMeshPtrFromShapeParams(
				ModifiableRequest.ToolShape,
				ModifiableRequest.ShapeParams
			);
		}

		// 비동기 경로로 처리 (워커 스레드 사용)
		EnqueueRequestLocal(ModifiableRequest, Op.bIsPenetration, nullptr);
	}
}

bool URealtimeDestructibleMeshComponent::BuildMeshSnapshot(FRealtimeMeshSnapshot& Out) const
{
	return false;
}

bool URealtimeDestructibleMeshComponent::ApplyMeshSnapshot(const FRealtimeMeshSnapshot& In)
{
	return false;
}

void URealtimeDestructibleMeshComponent::GetDestructionSettings(int32& OutMaxHoleCount, int32& OutMaxOpsPerFrame, int32& OutMaxBatchSize)
{
	OutMaxHoleCount = MaxHoleCount;
	OutMaxOpsPerFrame = MaxOpsPerFrame;
	OutMaxBatchSize = MaxBatchSize;
}

bool URealtimeDestructibleMeshComponent::InitializeFromStaticMeshInternal(UStaticMesh* InMesh, bool bForce)
{
	if (!InMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: SourceStaticMesh is null"));
		return false;
	}

	if (bIsInitialized && !bForce)
	{
		return true;
	}

	UDynamicMesh* DynamicMesh = GetDynamicMesh();
	if (!DynamicMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("RealtimeDestructibleMeshComponent: Failed to get DynamicMesh"));
		return false;
	}

	FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
	CopyOptions.bApplyBuildSettings = true;
	CopyOptions.bRequestTangents = true;

	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UDynamicMesh* ResultMesh = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
		InMesh,
		DynamicMesh,
		CopyOptions,
		FGeometryScriptMeshReadLOD(),
		Outcome
	);

	if (!ResultMesh || Outcome != EGeometryScriptOutcomePins::Success)
	{
		UE_LOG(LogTemp, Error, TEXT("RealtimeDestructibleMeshComponent: Failed to copy mesh from static mesh"));
		return false;
	}

	CopyMaterialsFromStaticMesh(InMesh);
	SetComplexAsSimpleCollisionEnabled(true);
	ApplyRenderUpdate();
	ApplyCollisionUpdate();

	CurrentHoleCount = 0;
	bIsInitialized = true;
	OnInitialized.Broadcast();

	return true;
}

bool URealtimeDestructibleMeshComponent::EnsureSphereTemplate()
{
	if (SphereTemplatePtr.IsValid())
	{
		return false;
	}

	UDynamicMesh* Temp = NewObject<UDynamicMesh>(this);

	// 구형 메시 생성 옵션
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	PrimitiveOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::SingleGroup;

	const int32 StepsPhi = FMath::Max(3, SphereStepsPhi);
	const int32 StepsTheta = FMath::Max(3, SphereStepsTheta);
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
		Temp,										// 대상 메시 (in-out)
		PrimitiveOptions,							// 생성 옵션
		FTransform::Identity,						// 원점에 생성 (중요!)
		1.0f,										// 구의 반지름
		StepsPhi,									// StepsPhi (위도)
		StepsTheta,									// StepsTheta (경도)
		EGeometryScriptPrimitiveOriginMode::Center	// 중심점 기준
	);

	SphereTemplatePtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	Temp->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Source)
	{
		*SphereTemplatePtr = Source;
	});

	return true;
}

UDynamicMesh* URealtimeDestructibleMeshComponent::CreateToolMeshFromRequest(const FRealtimeDestructionRequest& Request)
{
	UDynamicMesh* ToolMesh = NewObject<UDynamicMesh>();
	if (!ToolMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create ToolMesh"));
		return nullptr;
	}

	return ToolMesh;
}

TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> URealtimeDestructibleMeshComponent::CreateToolMeshPtrFromShapeParams(
	EDestructionToolShape ToolShape,
	const FDestructionToolShapeParams& ShapeParams)
{
	UDynamicMesh* TempMesh = NewObject<UDynamicMesh>(this);
	if (!TempMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateToolMeshPtrFromShapeParams: Failed to create TempMesh"));
		return nullptr;
	}

	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	PrimitiveOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::SingleGroup;

	switch (ToolShape)
	{
	case EDestructionToolShape::Sphere:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.StepsPhi,
			ShapeParams.StepsTheta,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;

	case EDestructionToolShape::Cylinder:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.Height,
			ShapeParams.RadiusSteps,
			ShapeParams.HeightSubdivisions,
			ShapeParams.bCapped,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;

	default:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.Height,
			ShapeParams.RadiusSteps,
			ShapeParams.HeightSubdivisions,
			ShapeParams.bCapped,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;
	}

	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> Result = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	TempMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Source)
	{
		*Result = Source;
	});

	return Result;
}

bool URealtimeDestructibleMeshComponent::ApplyDestructionRequestInternal(const FRealtimeDestructionRequest& Request)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: Not initialized"));
		return false;
	}

	if (CurrentHoleCount >= MaxHoleCount)
	{
		// UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: Maximum hole count reached"));
		return false;
	}

	UDynamicMesh* TargetMesh = GetDynamicMesh();
	if (!TargetMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("RealtimeDestructibleMeshComponent: TargetMesh is null"));
		return false;
	}
	 
	UDynamicMesh* ToolMesh = CreateToolMeshFromRequest(Request);


	const FVector LocalImpactPoint = GetComponentTransform().InverseTransformPosition(Request.ImpactPoint);
	const FTransform LocalToolTransform = FTransform(LocalImpactPoint);

	// Boolean 연산 시간 측정 시작
	const double BooleanStartTime = FPlatformTime::Seconds();

	UDynamicMesh* ResultMesh = UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		TargetMesh,
		FTransform::Identity,
		ToolMesh,
		LocalToolTransform,
		EGeometryScriptBooleanOperation::Subtract,
		BooleanOptions
	);

	// Boolean 연산 시간 측정 종료
	const float BooleanTimeMs = static_cast<float>((FPlatformTime::Seconds() - BooleanStartTime) * 1000.0);

	// 디버거에 Boolean 연산 시간 기록
	if (UWorld* World = GetWorld())
	{
		if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
		{
			Debugger->RecordBooleanOperationTime(BooleanTimeMs);
		}
	}

	if (!ResultMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: Boolean operation failed"));
		return false;
	}

	++CurrentHoleCount;
	return true;
}

void URealtimeDestructibleMeshComponent::CopyMaterialsFromStaticMesh(UStaticMesh* InMesh)
{
	if (!InMesh)
	{
		return;
	}

	const int32 NumMaterials = InMesh->GetStaticMaterials().Num();
	for (int32 Index = 0; Index < NumMaterials; ++Index)
	{
		if (UMaterialInterface* Material = InMesh->GetMaterial(Index))
		{
			SetMaterial(Index, Material);
		}
	}
}

// 아직 사용되는 경로 없음
void URealtimeDestructibleMeshComponent::CopyMaterialsFromStaticMeshComponent(UStaticMeshComponent* InComp)
{
	if (!InComp)
	{
		return;
	}

	const int32 NumMaterials = InComp->GetNumMaterials();
	for (int32 Index = 0; Index < NumMaterials; ++Index)
	{
		if (UMaterialInterface* Material = InComp->GetMaterial(Index))
		{
			SetMaterial(Index, Material);
		}
	}
}

void URealtimeDestructibleMeshComponent::CopyCollisionFromStaticMeshComponent(UStaticMeshComponent* InComp)
{
	if (!InComp)
	{
		return;
	}

	SetCollisionEnabled(InComp->GetCollisionEnabled());
	SetCollisionProfileName(InComp->GetCollisionProfileName());
	SetCollisionResponseToChannels(InComp->GetCollisionResponseToChannels());
	SetGenerateOverlapEvents(InComp->GetGenerateOverlapEvents());
	SetComplexAsSimpleCollisionEnabled(true);
}

FDynamicMesh3 URealtimeDestructibleMeshComponent::GetToolMesh(EDestructionToolShape ToolShape, FDestructionToolShapeParams ShapeParams)
{
	UDynamicMesh* TempMesh = NewObject<UDynamicMesh>(this);
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	PrimitiveOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::SingleGroup;

	switch (ToolShape)
	{
	case EDestructionToolShape::Sphere:
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.StepsPhi,
			ShapeParams.StepsTheta,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;
	} 

	case EDestructionToolShape::Cylinder:
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.Height,
			ShapeParams.RadiusSteps,
			ShapeParams.HeightSubdivisions,
			ShapeParams.bCapped,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;
	} 
	default:
	{ 
		if (!bSphereTemplateReady)
		{
			bSphereTemplateReady = EnsureSphereTemplate();
		}
		return *SphereTemplatePtr;
	}
	}

	FDynamicMesh3 ResultMesh;
	TempMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Source)
		{
			ResultMesh = Source;
		});

	return ResultMesh;
}

void URealtimeDestructibleMeshComponent::ApplyRenderUpdate()
{
	NotifyMeshUpdated();
	MarkRenderStateDirty();
}

void URealtimeDestructibleMeshComponent::ApplyCollisionUpdate()
{
	UpdateCollision();
	RecreatePhysicsState();
}
 
bool URealtimeDestructibleMeshComponent::CheckPenetration(const FRealtimeDestructionRequest& Request, float& OutPenetration)
{
	FVector StartPoint = Request.ImpactPoint;
	FVector ForwardDir = Request.ImpactNormal * -1.0f; // 총알 진행 방향

	// 관통 체크할 최소 두께 .
	float MaxPenetrationDepth = 150.0f;

	// 위에서 설정한 길이 만큼 벽 뒤로가서 Ray를 쏜다.
	FVector ProbeStart = StartPoint + (ForwardDir * MaxPenetrationDepth);
	FVector ProbeEnd = StartPoint;

	FHitResult BackHit;
	FCollisionQueryParams Params;

	Params.bTraceComplex = true; // Mesh의 정확한 폴리곤을 찍기 위해 켭니다.

	// 뒤에서 앞으로 쏘는 Ray 
	bool bHitBack = GetWorld()->LineTraceSingleByChannel(BackHit, ProbeStart, ProbeEnd, ECC_Visibility, Params);

	if (bDebugPenetration)
	{
		// 잘 되는 지 시각화 용  
		DrawDebugLine(GetWorld(), ProbeStart, bHitBack ? BackHit.ImpactPoint : ProbeEnd, FColor::Purple, false, 5.0f, 0, 1.0f);
	}

	if (bHitBack)
	{
		// Hit된게 본인이여한다. (다른 벽 말고) 		
		if (BackHit.GetActor() == GetOwner())
		{
			 
			// 두께 계산: (원래 맞은 앞면) <-> (지금 맞은 뒷면) 거리
			float Thickness = FVector::Dist(StartPoint, BackHit.ImpactPoint);
		 
			// 디버그 출력
			if (bDebugPenetration)
			{
				DrawDebugPoint(GetWorld(), BackHit.ImpactPoint, 10.0f, FColor::Cyan, false, 5.0f);
				FString Msg = FString::Printf(TEXT("Wall Thickness: %.2f"), Thickness);
				DrawDebugString(GetWorld(), BackHit.Location, Msg, nullptr, FColor::White, 5.0f);
			}
			
			// ThicknessOffset이  0일 때, 임의의 로직으로 계산해준다
			if (ThicknessOffset == 0)
			{
				switch (Request.ToolShape)
				{
				case EDestructionToolShape::Sphere:
					ThicknessOffset = Request.Depth * 2.0f;
					break;
			
				case EDestructionToolShape::Cylinder:
					ThicknessOffset = Request.Depth * 1.5f;
					break;
			
				default:
					ThicknessOffset = Request.Depth * 1.5f;
					break;
				}				
			}

			// 두께가 얇으면 관통 성공  
			if (Thickness <= ThicknessOffset)
			{
				OutPenetration = Thickness * 1.1f;
				return true;
			} 
		}
	}
	return false;
}
void URealtimeDestructibleMeshComponent::GetParallelSettings(int32& OutThreshold, int32& OutMaxThreads)
{
	OutThreshold = ParallelThreshold;
	OutMaxThreads = MaxParallelThreads; 
}
void URealtimeDestructibleMeshComponent::SettingAsyncOption(bool& OutParallelEnabled, bool& OutMultiWorker)
{
	OutParallelEnabled = bEnableParallel;
	OutMultiWorker = bEnableMultiWorkers;
}
void URealtimeDestructibleMeshComponent::OnRegister()
{
	Super::OnRegister();

	if (SourceStaticMesh && !bIsInitialized)
	{
		InitializeFromStaticMeshInternal(SourceStaticMesh, false);
	}
}

void URealtimeDestructibleMeshComponent::InitializeComponent()
{
	Super::InitializeComponent();
}
void URealtimeDestructibleMeshComponent::BeginPlay()
{
	Super::BeginPlay();

	// Trace 채널 활성화 (비-쉬핑 빌드에서만)
#if !UE_BUILD_SHIPPING
	if (!bIsTraceEnabled)
	{
		if (GEngine)
		{
			GEngine->Exec(GetWorld(), TEXT("Trace.Enable task"));
			GEngine->Exec(GetWorld(), TEXT("Trace.Enable contextswitch"));
			GEngine->Exec(GetWorld(), TEXT("Trace.Enable counters"));
			bIsTraceEnabled = true;
			UE_LOG(LogTemp, Log, TEXT("Trace channel task, counters enabled"));
		}
	}
#endif

	// 멀티플레이어 동기화를 위해 Owner Actor의 Replication 활성화
	if (AActor* Owner = GetOwner())
	{
		if (!Owner->GetIsReplicated())
		{
			Owner->SetReplicates(true);
			Owner->SetReplicateMovement(false);  // 움직임은 복제 안 함 (정적 오브젝트)
			Owner->bAlwaysRelevant = true;       // 모든 클라이언트에 항상 관련됨
			UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: Owner Actor의 Replication 활성화됨"));
		}
	}

	if (SourceStaticMesh && !bIsInitialized)
	{
		InitializeFromStaticMeshInternal(SourceStaticMesh, false);
	}

	if (bIsInitialized && !BooleanProcessor.IsValid())
	{
		BooleanProcessor = MakeUnique<FRealtimeBooleanProcessor>();
		if (!BooleanProcessor->Initialize(this))
		{
			UE_LOG(LogTemp, Warning, TEXT("불리언 프로세서 초기화 실패"));
		}
		else
		{
			// UPROPERTY 값을 프로세서에 동기화
			BooleanProcessor->SetCachedMeshOptimization(bUseCachedMeshOptimization);
		}
	}

	if (bIsInitialized)
	{
		if (!EnsureSphereTemplate())
		{
			UE_LOG(LogTemp, Warning, TEXT("Sphere template not ready"));
		}
	}
	 
}

void URealtimeDestructibleMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bEnableMultiWorkers)
	{
		if (BooleanProcessor.IsValid())
		{
			BooleanProcessor->SetCachedMeshOptimization(bUseCachedMeshOptimization);

			// [추가] 이번 프레임에 쌓인 요청이 있다면 여기서 처리 시작!
			// 이 함수는 내부적으로 큐가 비었거나 워커가 꽉 찼으면 즉시 리턴하므로 비용이 매우 저렴합니다.
			BooleanProcessor->KickProcessIfNeeded();
		} 
	}
	
	// 에디터에서 런타임 변경 시 프로세서에 동기화
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->SetCachedMeshOptimization(bUseCachedMeshOptimization);
	}

	// Cell 메시 디버그 와이어프레임 표시
	if (bShowCellMeshDebug)
	{
		DrawCellMeshesDebug();
	}

	// 디버그 텍스트 표시
	if (bShowDebugText && GetWorld())
	{
		// 메시 정보 가져오기
		int32 VertexCount = 0;
		int32 TriangleCount = 0;

		if (UDynamicMesh* DynMesh = GetDynamicMesh())
		{
			DynMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				VertexCount = Mesh.VertexCount();
				TriangleCount = Mesh.TriangleCount();
			});
		}

		// 대기 중인 Op 수
		int32 PendingCount = PendingOps.Num();

		// BooleanProcessor의 hole count 가져오기 (비동기 처리 시 여기서 관리됨)
		int32 HoleCount = BooleanProcessor.IsValid() ? BooleanProcessor->GetCurrentHoleCount() : CurrentHoleCount;

		// 네트워크 모드 가져오기
		FString NetModeStr = TEXT("Unknown");
		if (UWorld* World = GetWorld())
		{
			switch (World->GetNetMode())
			{
			case NM_Standalone:
				NetModeStr = TEXT("Standalone");
				break;
			case NM_DedicatedServer:
				NetModeStr = TEXT("Dedicated Server");
				break;
			case NM_ListenServer:
				NetModeStr = TEXT("Listen Server");
				break;
			case NM_Client:
				NetModeStr = TEXT("Client");
				break;
			default:
				NetModeStr = TEXT("Unknown");
				break;
			}
		}

		// 서버 배칭 상태
		FString BatchingStr = bUseServerBatching ? TEXT("ON") : TEXT("OFF");
		int32 BatchQueueSize = bUseCompactMulticast ? PendingServerBatchOpsCompact.Num() : PendingServerBatchOps.Num();

		// 디버그 텍스트 생성
		FString DebugText = FString::Printf(
			TEXT("Vertices: %d\nTriangles: %d\nHoles: %d / %d\nPending Ops: %d\nInitialized: %s\n--- Network ---\nMode: %s\nBatching: %s (Queue: %d)"),
			VertexCount,
			TriangleCount,
			HoleCount,
			MaxHoleCount,
			PendingCount,
			bIsInitialized ? TEXT("Yes") : TEXT("No"),
			*NetModeStr,
			*BatchingStr,
			BatchQueueSize
		);

		// 액터 위치 + 오프셋에 텍스트 표시
		FVector TextLocation = GetComponentLocation() + DebugTextOffset;
		DrawDebugString(GetWorld(), TextLocation, DebugText, nullptr, DebugTextColor, 0.0f, true);
	}

	// 서버 배칭 처리
	if (!bUseServerBatching)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 서버에서만 배칭 처리
	const ENetMode NetMode = World->GetNetMode();
	if (NetMode != NM_DedicatedServer && NetMode != NM_ListenServer)
	{
		return;
	}

	// 대기 중인 요청이 없으면 스킵
	const int32 PendingCount = bUseCompactMulticast ? PendingServerBatchOpsCompact.Num() : PendingServerBatchOps.Num();
	if (PendingCount == 0)
	{
		ServerBatchTimer = 0.0f;
		return;
	}

	// 타이머 업데이트
	ServerBatchTimer += DeltaTime;

	// 배치 간격 도달 시 전송
	if (ServerBatchTimer >= ServerBatchInterval)
	{
		FlushServerBatch();
		ServerBatchTimer = 0.0f;
	}
}

void URealtimeDestructibleMeshComponent::OnUnregister()
{	
	Super::OnUnregister();
}

void URealtimeDestructibleMeshComponent::BeginDestroy()
{
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->Shutdown();
		BooleanProcessor.Reset();
	}
	Super::BeginDestroy();
}

void URealtimeDestructibleMeshComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->Shutdown();
		BooleanProcessor.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

void URealtimeDestructibleMeshComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void URealtimeDestructibleMeshComponent::EnqueueForServerBatch(const FRealtimeDestructionOp& Op)
{
	if (bUseCompactMulticast)
	{
		// 압축해서 저장
		FCompactDestructionOp CompactOp = FCompactDestructionOp::Compress(Op.Request, ServerBatchSequence++);
		PendingServerBatchOpsCompact.Add(CompactOp);

		// 최대 배치 크기 도달 시 즉시 전송 -> 음 이건 관통,비관통 문제가 있을 수 있으니 수정 해야겠군요 
		if (PendingServerBatchOpsCompact.Num() >= MaxServerBatchSize)
		{
			FlushServerBatch();
		}
	}
	else
	{
		// 비압축 저장
		PendingServerBatchOps.Add(Op);

		// 최대 배치 크기 도달 시 즉시 전송
		if (PendingServerBatchOps.Num() >= MaxServerBatchSize)
		{
			FlushServerBatch();
		}
	}
}

void URealtimeDestructibleMeshComponent::FlushServerBatch()
{
	if (bUseCompactMulticast)
	{
		// 압축 모드
		if (PendingServerBatchOpsCompact.Num() == 0)
		{
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("[ServerBatching] Flushing %d ops (Compact)"), PendingServerBatchOpsCompact.Num());

		// 디버거에 Multicast RPC 기록 (압축, 데이터 크기 포함)
		if (UWorld* World = GetWorld())
		{
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				Debugger->RecordMulticastRPCWithSize(PendingServerBatchOpsCompact.Num(), true);
			}
		}

		// 압축된 데이터로 전파
		MulticastApplyOpsCompact(PendingServerBatchOpsCompact);

		// 대기열 비우기
		PendingServerBatchOpsCompact.Empty();
	}
	else
	{
		// 비압축 모드
		if (PendingServerBatchOps.Num() == 0)
		{
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("[ServerBatching] Flushing %d ops"), PendingServerBatchOps.Num());

		// 디버거에 Multicast RPC 기록 (비압축, 데이터 크기 포함)
		if (UWorld* World = GetWorld())
		{
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				Debugger->RecordMulticastRPCWithSize(PendingServerBatchOps.Num(), false);
			}
		}

		// 비압축 데이터로 전파
		MulticastApplyOps(PendingServerBatchOps);

		// 대기열 비우기
		PendingServerBatchOps.Empty();
	}
}

UDecalComponent* URealtimeDestructibleMeshComponent::SpawnTemporaryDecal(const FRealtimeDestructionRequest& Request)
{
	if (!HoleDecal)
	{
		return nullptr;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	UDecalComponent* Decal = NewObject<UDecalComponent>(Owner);
	if (!Decal)
	{	
		return nullptr;
	}

	Decal->SetDecalMaterial(HoleDecal);
	Decal->DecalSize = DecalSize;

	//데칼이 항상 보이도록 처리 
	Decal->SetFadeScreenSize(0.0f); 
	Decal->FadeStartDelay = 0.0f;
	Decal->FadeDuration = 0.0f;
	

	// decal 방향 설정
	FRotator DecalRotation = Request.ImpactNormal.Rotation();
	//DecalRotation.Pitch += 180.0f;

	Decal->SetWorldLocationAndRotation(Request.ImpactPoint, DecalRotation);

	// 액터에 등록
	Decal->RegisterComponent();
	//Decal->AttachToComponent(this, FAttachmentTransformRules::KeepWorldTransform);  

	return Decal;
}

//////////////////////////////////////////////////////////////////////////
// Cell Mesh Parallel Processing
//////////////////////////////////////////////////////////////////////////

void URealtimeDestructibleMeshComponent::DrawCellMeshesDebug()
{
	if (!bCellMeshesValid || CellMeshes.Num() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& CompTransform = GetComponentTransform();
	const float Duration = 0.0f;  // 매 프레임 새로 그림

	// Cell별로 다른 색상 생성
	TArray<FColor> CellColors;
	CellColors.SetNum(CellMeshes.Num());
	for (int32 i = 0; i < CellMeshes.Num(); ++i)
	{
		// HSV 색상환에서 균등하게 분포된 색상
		float Hue = (float)i / (float)CellMeshes.Num() * 360.0f;
		CellColors[i] = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue / 360.0f * 255.0f), 255, 255).ToFColor(true);
	}

	int32 TotalTrianglesDrawn = 0;

	for (int32 CellId = 0; CellId < CellMeshes.Num(); ++CellId)
	{
		const FColor& Color = CellColors[CellId];

		if (!CellMeshes[CellId])
		{
			continue;
		}

		const UE::Geometry::FDynamicMesh3* Mesh = CellMeshes[CellId].Get();

		// Cell 중심 위치 계산 (메쉬 바운드 기준)
		UE::Geometry::FAxisAlignedBox3d MeshBounds = Mesh->GetBounds();
		FVector MeshCenter(MeshBounds.Center().X, MeshBounds.Center().Y, MeshBounds.Center().Z);
		FVector CellCenterWorld = CompTransform.TransformPosition(MeshCenter);

		// Cell 중심에 점과 ID 표시
		DrawDebugPoint(World, CellCenterWorld, 15.0f, Color, false, Duration, SDPG_Foreground);
		DrawDebugString(World, CellCenterWorld + FVector(0, 0, 5.0f),
			FString::Printf(TEXT("%d"), CellId), nullptr, FColor::White, Duration, true, 1.2f);

		// 각 Triangle의 엣지를 그림
		for (int32 TriId : Mesh->TriangleIndicesItr())
		{
			UE::Geometry::FIndex3i Tri = Mesh->GetTriangle(TriId);

			FVector V0 = CompTransform.TransformPosition(FVector(Mesh->GetVertex(Tri.A)));
			FVector V1 = CompTransform.TransformPosition(FVector(Mesh->GetVertex(Tri.B)));
			FVector V2 = CompTransform.TransformPosition(FVector(Mesh->GetVertex(Tri.C)));

			DrawDebugLine(World, V0, V1, Color, false, Duration, SDPG_Foreground, 1.0f);
			DrawDebugLine(World, V1, V2, Color, false, Duration, SDPG_Foreground, 1.0f);
			DrawDebugLine(World, V2, V0, Color, false, Duration, SDPG_Foreground, 1.0f);

			++TotalTrianglesDrawn;
		}
	}

} 

int32 URealtimeDestructibleMeshComponent::BuildCellMeshesFromGeometryCollection()
{
	if (!FracturedGeometryCollection)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: FracturedGeometryCollection is not set."));
		return 0;
	}

	// GeometryCollection 데이터 가져오기
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GeometryCollectionPtr = FracturedGeometryCollection->GetGeometryCollection();
	if (!GeometryCollectionPtr.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: Invalid GeometryCollection data."));
		return 0;
	}

	const FGeometryCollection& GC = *GeometryCollectionPtr;

	// Transform Group에서 조각 개수 확인
	const int32 NumTransforms = GC.NumElements(FGeometryCollection::TransformGroup);
	if (NumTransforms == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: No transforms in GeometryCollection."));
		return 0;
	}

	// Geometry Group에서 실제 메시 데이터 가져오기
	const TManagedArray<FVector3f>& Vertices = GC.Vertex;
	const TManagedArray<int32>& BoneMap = GC.BoneMap;
	const TManagedArray<FIntVector>& Indices = GC.Indices;
	const TManagedArray<FVector3f>* Normals = GC.FindAttribute<FVector3f>("Normal", FGeometryCollection::VerticesGroup);

	// UV 찾기 - FindAttribute로 다양한 이름 시도
	const TManagedArray<FVector2f>* FinalUVs = nullptr;

	// 가능한 UV 속성 이름들
	const TCHAR* UVAttributeNames[] = {
		TEXT("UVs"),
		TEXT("UV"),
		TEXT("UV0"),
		TEXT("TexCoord"),
		TEXT("TexCoords")
	};

	for (const TCHAR* AttrName : UVAttributeNames)
	{
		FinalUVs = GC.FindAttribute<FVector2f>(AttrName, FGeometryCollection::VerticesGroup);
		if (FinalUVs && FinalUVs->Num() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGC: Found UV attribute '%s', %d UVs"), AttrName, FinalUVs->Num());
			break;
		}
	}

	if (!FinalUVs || FinalUVs->Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGC: No UV data found in GeometryCollection! Vertices: %d"), Vertices.Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGC: UV data found with %d elements, Vertices: %d"),
			FinalUVs->Num(), Vertices.Num());
	}

	// MaterialID 가져오기 (FacesGroup에 저장됨)
	const TManagedArray<int32>* MaterialIDs = GC.FindAttribute<int32>("MaterialID", FGeometryCollection::FacesGroup);

	//=========================================================================
	// 1패스: 버텍스를 조각별로 분류 O(M)
	//=========================================================================
	TArray<TArray<int32>> VertexIndicesByTransform;
	VertexIndicesByTransform.SetNum(NumTransforms);

	// 메모리 예약 (대략적인 균등 분배 가정)
	const int32 EstimatedVertsPerTransform = (Vertices.Num() / NumTransforms) + 10;
	for (int32 i = 0; i < NumTransforms; ++i)
	{
		VertexIndicesByTransform[i].Reserve(EstimatedVertsPerTransform);
	}

	for (int32 VertIdx = 0; VertIdx < Vertices.Num(); ++VertIdx)
	{
		int32 TransformIdx = BoneMap[VertIdx];
		if (TransformIdx >= 0 && TransformIdx < NumTransforms)
		{
			VertexIndicesByTransform[TransformIdx].Add(VertIdx);
		}
	}

	//=========================================================================
	// 1패스: 삼각형을 조각별로 분류 O(T)
	//=========================================================================
	struct FTriangleData
	{
		FIntVector Indices;
		int32 MaterialID;
	};

	TArray<TArray<FTriangleData>> TrianglesByTransform;
	TrianglesByTransform.SetNum(NumTransforms);

	const int32 EstimatedTrisPerTransform = (Indices.Num() / NumTransforms) + 10;
	for (int32 i = 0; i < NumTransforms; ++i)
	{
		TrianglesByTransform[i].Reserve(EstimatedTrisPerTransform);
	}

	for (int32 TriIdx = 0; TriIdx < Indices.Num(); ++TriIdx)
	{
		const FIntVector& Tri = Indices[TriIdx];
		// 삼각형의 첫 버텍스 기준으로 조각 결정
		int32 TransformIdx = BoneMap[Tri.X];
		if (TransformIdx >= 0 && TransformIdx < NumTransforms)
		{
			FTriangleData TriData;
			TriData.Indices = Tri;
			TriData.MaterialID = (MaterialIDs && TriIdx < MaterialIDs->Num()) ? (*MaterialIDs)[TriIdx] : 0;
			TrianglesByTransform[TransformIdx].Add(TriData);
		}
	}

	//=========================================================================
	// 각 조각별 DynamicMesh 생성 O(N)
	//=========================================================================
	CellMeshes.Reset();
	CellMeshes.SetNum(NumTransforms);

	int32 ExtractedCount = 0;

	for (int32 TransformIdx = 0; TransformIdx < NumTransforms; ++TransformIdx)
	{
		const TArray<int32>& MyVertexIndices = VertexIndicesByTransform[TransformIdx];
		const TArray<FTriangleData>& MyTriangles = TrianglesByTransform[TransformIdx];

		// 빈 조각 스킵
		if (MyVertexIndices.Num() == 0 || MyTriangles.Num() == 0)
		{
			CellMeshes[TransformIdx] = nullptr;
			continue;
		}

		// Global Index → Local Index 매핑
		TMap<int32, int32> GlobalToLocalVertex;
		GlobalToLocalVertex.Reserve(MyVertexIndices.Num());

		TArray<FVector3f> LocalVertices;
		TArray<FVector3f> LocalNormals;
		TArray<FVector2f> LocalUVs;
		LocalVertices.Reserve(MyVertexIndices.Num());
		if (Normals) LocalNormals.Reserve(MyVertexIndices.Num());
		if (FinalUVs) LocalUVs.Reserve(MyVertexIndices.Num());

		for (int32 GlobalIdx : MyVertexIndices)
		{
			int32 LocalIdx = LocalVertices.Num();
			GlobalToLocalVertex.Add(GlobalIdx, LocalIdx);

			LocalVertices.Add(Vertices[GlobalIdx]);
			if (Normals) LocalNormals.Add((*Normals)[GlobalIdx]);
			if (FinalUVs) LocalUVs.Add((*FinalUVs)[GlobalIdx]);
		}

		// 삼각형 인덱스 로컬로 변환 (MaterialID 유지)
		struct FLocalTriangleData
		{
			FIntVector Indices;
			int32 MaterialID;
		};
		TArray<FLocalTriangleData> LocalTriangles;
		LocalTriangles.Reserve(MyTriangles.Num());

		for (const FTriangleData& TriData : MyTriangles)
		{
			const FIntVector& Tri = TriData.Indices;
			if (GlobalToLocalVertex.Contains(Tri.X) &&
				GlobalToLocalVertex.Contains(Tri.Y) &&
				GlobalToLocalVertex.Contains(Tri.Z))
			{
				FLocalTriangleData LocalTriData;
				LocalTriData.Indices = FIntVector(
					GlobalToLocalVertex[Tri.X],
					GlobalToLocalVertex[Tri.Y],
					GlobalToLocalVertex[Tri.Z]
				);
				LocalTriData.MaterialID = TriData.MaterialID;
				LocalTriangles.Add(LocalTriData);
			}
		}

		if (LocalTriangles.Num() == 0)
		{
			CellMeshes[TransformIdx] = nullptr;
			continue;
		}

		// DynamicMesh3 생성
		TSharedPtr<UE::Geometry::FDynamicMesh3> NewMesh = MakeShared<UE::Geometry::FDynamicMesh3>();
		NewMesh->EnableTriangleGroups();
		NewMesh->EnableAttributes();
		NewMesh->Attributes()->EnablePrimaryColors();
		NewMesh->Attributes()->EnableMaterialID();

		UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = NewMesh->Attributes()->PrimaryUV();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = NewMesh->Attributes()->PrimaryNormals();
		UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDAttr = NewMesh->Attributes()->GetMaterialID();

		// 버텍스 추가
		TArray<int32> VertexIDs;
		VertexIDs.Reserve(LocalVertices.Num());

		for (const FVector3f& V : LocalVertices)
		{
			int32 Vid = NewMesh->AppendVertex(FVector3d(V.X, V.Y, V.Z));
			VertexIDs.Add(Vid);
		}

		// 삼각형 추가
		for (const FLocalTriangleData& TriData : LocalTriangles)
		{
			const FIntVector& Tri = TriData.Indices;
			int32 TriId = NewMesh->AppendTriangle(VertexIDs[Tri.X], VertexIDs[Tri.Y], VertexIDs[Tri.Z]);

			if (TriId >= 0)
			{
				// MaterialID 설정
				if (MaterialIDAttr)
				{
					MaterialIDAttr->SetValue(TriId, TriData.MaterialID);
				}

				// UV 설정
				if (UVOverlay && LocalUVs.Num() > 0)
				{
					int32 UV0 = UVOverlay->AppendElement(FVector2f(LocalUVs[Tri.X]));
					int32 UV1 = UVOverlay->AppendElement(FVector2f(LocalUVs[Tri.Y]));
					int32 UV2 = UVOverlay->AppendElement(FVector2f(LocalUVs[Tri.Z]));
					UVOverlay->SetTriangle(TriId, UE::Geometry::FIndex3i(UV0, UV1, UV2));
				}

				// Normal 설정
				if (NormalOverlay && LocalNormals.Num() > 0)
				{
					int32 N0 = NormalOverlay->AppendElement(FVector3f(LocalNormals[Tri.X]));
					int32 N1 = NormalOverlay->AppendElement(FVector3f(LocalNormals[Tri.Y]));
					int32 N2 = NormalOverlay->AppendElement(FVector3f(LocalNormals[Tri.Z]));
					NormalOverlay->SetTriangle(TriId, UE::Geometry::FIndex3i(N0, N1, N2));
				}
			}
		}

		CellMeshes[TransformIdx] = NewMesh;
		++ExtractedCount;
	}

	// Bounds 계산
	CellBounds.SetNum(NumTransforms);
	for (int32 i = 0; i < NumTransforms; ++i)
	{
		if (CellMeshes[i] && CellMeshes[i]->TriangleCount() > 0)
		{
			UE::Geometry::FAxisAlignedBox3d MeshBounds = CellMeshes[i]->GetBounds();
			CellBounds[i] = FBox(
				FVector(MeshBounds.Min.X, MeshBounds.Min.Y, MeshBounds.Min.Z),
				FVector(MeshBounds.Max.X, MeshBounds.Max.Y, MeshBounds.Max.Z)
			);
		}
		else
		{
			CellBounds[i] = FBox(ForceInit);
		}
	}

	// GeometryCollection에서 머티리얼 복사
	const TArray<UMaterialInterface*>& GCMaterials = FracturedGeometryCollection->Materials;
	if (GCMaterials.Num() > 0)
	{
		// OverrideMaterials 배열 크기 조정
		if (OverrideMaterials.Num() < GCMaterials.Num())
		{
			OverrideMaterials.SetNum(GCMaterials.Num());
		}

		for (int32 MatIdx = 0; MatIdx < GCMaterials.Num(); ++MatIdx)
		{
			if (GCMaterials[MatIdx])
			{
				OverrideMaterials[MatIdx] = GCMaterials[MatIdx];
			}
		}

		// 렌더 업데이트
		MarkRenderStateDirty();

		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGeometryCollection: Copied %d materials from GeometryCollection"), GCMaterials.Num());
	}

	bCellMeshesValid = ExtractedCount > 0;
	bUseCellMeshes = bCellMeshesValid;

	// Cell 메시들을 합쳐서 현재 컴포넌트의 메시로 설정
	if (bCellMeshesValid && CellMeshes.Num() > 0)
	{
		UDynamicMesh* DynMesh = GetDynamicMesh();
		if (DynMesh)
		{
			DynMesh->EditMesh([this](FDynamicMesh3& EditMesh)
			{
				// 기존 메시 클리어
				EditMesh.Clear();
				EditMesh.EnableAttributes();
				EditMesh.Attributes()->EnablePrimaryColors();
				EditMesh.Attributes()->EnableMaterialID();
				if (EditMesh.Attributes()->NumUVLayers() == 0)
				{
					EditMesh.Attributes()->SetNumUVLayers(1);
				}
				EditMesh.Attributes()->SetNumNormalLayers(1);

				// 각 Cell 메시를 합침
				for (int32 CellId = 0; CellId < CellMeshes.Num(); ++CellId)
				{
					if (!CellMeshes[CellId] || CellMeshes[CellId]->TriangleCount() == 0)
					{
						continue;
					}

					const FDynamicMesh3* SrcMesh = CellMeshes[CellId].Get();

					// Vertex ID 매핑 (원본 → 새 메시)
					TMap<int32, int32> VertexMap;

					// Vertex 복사
					for (int32 Vid : SrcMesh->VertexIndicesItr())
					{
						FVector3d Pos = SrcMesh->GetVertex(Vid);
						int32 NewVid = EditMesh.AppendVertex(Pos);
						VertexMap.Add(Vid, NewVid);
					}

					// Triangle 복사
					for (int32 Tid : SrcMesh->TriangleIndicesItr())
					{
						UE::Geometry::FIndex3i Tri = SrcMesh->GetTriangle(Tid);

						int32* NewA = VertexMap.Find(Tri.A);
						int32* NewB = VertexMap.Find(Tri.B);
						int32* NewC = VertexMap.Find(Tri.C);

						if (NewA && NewB && NewC)
						{
							int32 NewTid = EditMesh.AppendTriangle(*NewA, *NewB, *NewC);

							// MaterialID 복사
							if (NewTid >= 0 && SrcMesh->HasAttributes() && SrcMesh->Attributes()->GetMaterialID())
							{
								int32 MatId = SrcMesh->Attributes()->GetMaterialID()->GetValue(Tid);
								EditMesh.Attributes()->GetMaterialID()->SetValue(NewTid, MatId);
							}
						}
					}
				}

				UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGeometryCollection: Combined mesh has %d vertices, %d triangles"),
					EditMesh.VertexCount(), EditMesh.TriangleCount());
			});

			// 렌더 업데이트
			NotifyMeshUpdated();
			MarkRenderStateDirty();
			MarkRenderDynamicDataDirty();

#if WITH_EDITOR
			// 에디터에서 즉시 반영
			if (GIsEditor && !GIsPlayInEditorWorld)
			{
				MarkPackageDirty();
			}
#endif
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGeometryCollection: Extracted %d meshes from %d transforms"),
		ExtractedCount, NumTransforms);

	return ExtractedCount;
}

FBox URealtimeDestructibleMeshComponent::CalculateCellBounds(int32 CellId) const
{
	FBox ResultBounds(ForceInit);

	if (!CellMeshes.IsValidIndex(CellId) || !CellMeshes[CellId])
	{
		return ResultBounds;
	}

	// CellMesh의 모든 Vertex를 순회하여 Bounds 계산
	const UE::Geometry::FDynamicMesh3* Mesh = CellMeshes[CellId].Get();
	for (int32 Vid : Mesh->VertexIndicesItr())
	{
		FVector3d Pos = Mesh->GetVertex(Vid);
		ResultBounds += FVector(Pos.X, Pos.Y, Pos.Z);
	}

	return ResultBounds;
}






#if WITH_EDITOR
void URealtimeDestructibleMeshComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName = (PropertyChangedEvent.Property != nullptr)
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;

	// FracturedGeometryCollection 또는 bUseCellMeshes가 변경되면 자동 빌드
	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, FracturedGeometryCollection) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, bUseCellMeshes))
	{
		if (bUseCellMeshes && FracturedGeometryCollection)
		{
			int32 CellCount = BuildCellMeshesFromGeometryCollection();
			UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty: Auto-built %d cell meshes"), CellCount);
		}
	}
}


void URealtimeDestructibleMeshComponent::AutoFractureAndAssign()
{
	UStaticMesh* InStaticMesh = SourceStaticMesh.Get();
	if (!InStaticMesh)
	{
		return;
	} 
 
	// Transient는 디스크에 저장하지않고,메모리에만 존재하는 데이터를 담는데 유용 
	UGeometryCollection* GeometryCollection = NewObject<UGeometryCollection>();

	if (!GeometryCollection)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed To Create Geometry Collection!!"));
		return;
	}

	// Static Mesh To Geometry Collection

	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GCPtr = GeometryCollection->GetGeometryCollection();

	if (!GCPtr.IsValid())
	{
		GCPtr = MakeShared<FGeometryCollection>();
		GeometryCollection->SetGeometryCollection(GCPtr);
	}

	TArray<UMaterialInterface*> Materials;
	for (const FStaticMaterial& StaticMat : InStaticMesh->GetStaticMaterials())
	{
		Materials.Add(StaticMat.MaterialInterface);
	}

	// Static Mesh를 GC에 추가 (단일 조각으로 시작)
	FGeometryCollectionConversion::AppendStaticMesh(
		InStaticMesh,
		Materials,
		FTransform::Identity,
		GeometryCollection,
		true
	);

	GCPtr = GeometryCollection->GetGeometryCollection();
	if (!GCPtr.IsValid())
	{
		return;
	} 

	// 항상 모든 조각 선택     
	FDataflowTransformSelection TransformSelection;
	TransformSelection.InitializeFromCollection(*GCPtr, true);
	  
	// 2. Static Mesh의 BoundingBox 직접 사용
	FBox BoundingBox = InStaticMesh->GetBoundingBox();
	 
	 

	// 3. SliceCutter 실행
	// 인자가 많지만, 깔끔한 육면체 절단을 위해 노이즈 관련 값은 0으로 둡니다.
	int32 NumCreated = FFractureEngineFracturing::SliceCutter(
		*GCPtr.Get(),           // 레퍼런스로 전달 (&InOutCollection)
		TransformSelection,     // 선택 영역
		BoundingBox,            // 자를 범위
		SliceCount.X - 1,           // X축 몇번 자를 지
		SliceCount.Y - 1,           // Y축 몇번 자를 지
		SliceCount.Z - 1,           // Z축 몇번 자를 지
		0.0f,					// 0 이면 수직, 수평
		0.0f,                   // 0 이면 정간격
		0,             // 이미 각도 수직 고정이고 노이즈 진폭등 다 0으로 주면 ransdom의 의미가 없어지니 그냥 상수로 할당
		1.0f,                   // 파괴 확률 (ChanceToFracture) - 1.0f = 100%
		false,                  // 섬 분리 여부 (SplitIslands)
		0.0f,                   // Grout (틈새 벌리기) 
		0.0f,                   // Amplitude (노이즈 진폭) 
		0.0f,                   // Frequency (노이즈 빈도)
		0.0f,                   // Persistence
		0.0f,                   // Lacunarity
		0,                      // OctaveNumber
		0.0f,                   // PointSpacing
		false,                   // AddSamplesForCollision
		0.0f
	); 
	if (NumCreated <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SliceCutter FAILED! Returned %d"), NumCreated);
		return;

	}
	int32 NumTransformsAfterSlice = GCPtr->NumElements(FGeometryCollection::TransformGroup); 

	// =========================================================================
	//  데이터 무결성 갱신
	// 이 코드가 없으면 "Name not mapped" 또는 "Serialize not deterministic" 에러 발생
	// =========================================================================

	// Collection 무효화 및 재구성 (중요!)
	GeometryCollection->Materials = Materials;
	GeometryCollection->InvalidateCollection();

	GCPtr = GeometryCollection->GetGeometryCollection();
	//	바운딩 박스 재계산
	GCPtr->UpdateBoundingBox();


	// 에디터 변경 알림 (직렬화 준비)
#if WITH_EDITOR
	GeometryCollection->PostEditChange();
	GCPtr = GeometryCollection->GetGeometryCollection();
#endif
	  
	// 저장
	GeometryCollection->MarkPackageDirty();
	 
	// 컴포넌트에 자동 할당
	FracturedGeometryCollection = GeometryCollection;

	// Cell 메시 빌드
	int32 CellCount = BuildCellMeshesFromGeometryCollection();  
	return;
}

#endif
