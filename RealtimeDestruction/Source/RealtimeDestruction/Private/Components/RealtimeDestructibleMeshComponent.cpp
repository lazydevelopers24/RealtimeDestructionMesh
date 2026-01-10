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
//Fracturing
#include "FractureSettings.h"
#include "FractureEngineFracturing.h"
#include "Editor.h"
#endif

// Packaging
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "DrawDebugHelpers.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"

#include <ThirdParty/skia/skia-simplify.h>

#include "BulletClusterComponent.h"
#include "Algo/Unique.h"

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

	// ChunkIndex 저장 (클라이언트가 계산한 값)
	Compact.ChunkIndex = (Request.ChunkIndex >= 0 && Request.ChunkIndex < 256)
		? static_cast<uint8>(Request.ChunkIndex)
		: 0;

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

	// ChunkIndex 복원 (클라이언트가 계산한 값)
	Request.ChunkIndex = static_cast<int32>(ChunkIndex);

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

	// 기존 단일 메시 로직
	if (CellMeshComponents.Num() == 0)
	{
		// Add operation to queue
		BooleanProcessor->EnqueueOp(MoveTemp(Op), TemporaryDecal);

		// Tick마다 하는 걸로 변경
		// Start Boolean Operation
		if (!bEnableMultiWorkers)
		{
			BooleanProcessor->KickProcessIfNeeded();
		}
	}
	else
	{
		/*
		* 기존 구조는 BooleanProcessor에 캐싱된 OwnerComponent에서 FDynamicMesh3를 가져와서 연산하는 방식이었음
		* 파괴 요청 시 CellMesh 넘겨줘야함
		*/
		if (Op.Request.ChunkIndex != INDEX_NONE)
		{
			BooleanProcessor->EnqueueOp(MoveTemp(Op), TemporaryDecal, CellMeshComponents[Op.Request.ChunkIndex].Get());
		}

		if (!bEnableMultiWorkers)
		{
			BooleanProcessor->KickProcessIfNeededPerChunk();
		}
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
	ModifiedChunkIds.Add(Op.Request.ChunkIndex);

	ApplyRenderUpdate();
	ApplyCollisionUpdate(this);
	OnBatchCompleted.Broadcast(1);
	UpdateCellGraphForModifiedChunks();

	return true;
}

// Projectile에서 호출해줌
bool URealtimeDestructibleMeshComponent::RequestDestruction(const FRealtimeDestructionRequest& Request)
{ 
	if (bEnableClustering && BulletClusterComponent)
	{
		BulletClusterComponent->RegisterRequest(Request); 
	}
	return ExecuteDestructionInternal(Request);
}

bool URealtimeDestructibleMeshComponent::ExecuteDestructionInternal(const FRealtimeDestructionRequest& Request)
{
	if (MaxHoleCount > 0 && CurrentHoleCount >= MaxHoleCount)
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

// [deprecated]
void URealtimeDestructibleMeshComponent::RegisterForClustering(const FRealtimeDestructionRequest& Request)
{
	//TODO: 언제 return할 지 고민 중
	if (!bEnableClustering || !BulletClusterComponent)
	{
		ExecuteDestructionInternal(Request);
		return;
	}

	BulletClusterComponent->RegisterRequest(Request);

}

void URealtimeDestructibleMeshComponent::SetBooleanOptions(const FGeometryScriptMeshBooleanOptions& Options)
{
	BooleanOptions = Options;
}

void URealtimeDestructibleMeshComponent::SetMaxOpsPerFrame(int32 MaxOps)
{
	MaxOpsPerFrame = FMath::Max(1, MaxOps);
}

void URealtimeDestructibleMeshComponent::SetAsyncEnabled(bool bEnabled)
{
	bAsyncEnabled = bEnabled;
}

void URealtimeDestructibleMeshComponent::SetMaxHoleCount(int32 MaxCount)
{
	MaxHoleCount = FMath::Max(0, MaxCount);
}

int32 URealtimeDestructibleMeshComponent::GetHoleCount() const
{
	return CurrentHoleCount;
}

bool URealtimeDestructibleMeshComponent::ServerEnqueueOps_Validate(const TArray<FRealtimeDestructionRequest>& Requests)
{
	// 1단계: 명백한 치트 검사 → 실패 시 클라이언트 킥

	// 비정상적으로 많은 요청 (DoS 시도)
	if (Requests.Num() > MaxRequestsPerRPC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerValidate] 비정상적 요청 수: %d (최대: %d) → 킥"),
			Requests.Num(), MaxRequestsPerRPC);
		return false;
	}

	// 각 요청의 기본 유효성 검사
	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		// 비정상적으로 큰 파괴 반경
		if (Request.ShapeParams.Radius > MaxAllowedRadius)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerValidate] 비정상 반경: %.1f (최대: %.1f) → 킥"),
				Request.ShapeParams.Radius, MaxAllowedRadius);
			return false;
		}

		// 유효하지 않은 ChunkIndex
		if (Request.ChunkIndex != INDEX_NONE &&
			CellMeshComponents.Num() > 0 &&
			Request.ChunkIndex >= CellMeshComponents.Num())
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerValidate] 유효하지 않은 ChunkIndex: %d (최대: %d) → 킥"),
				Request.ChunkIndex, CellMeshComponents.Num() - 1);
			return false;
		}
	}

	return true;
}

bool URealtimeDestructibleMeshComponent::CheckRateLimit(APlayerController* Player)
{
	if (!Player)
	{
		return true;  // 플레이어 없으면 검증 스킵
	}

	const double CurrentTime = FPlatformTime::Seconds();
	FRateLimitInfo& Info = PlayerRateLimits.FindOrAdd(Player);

	// 1초 윈도우 리셋
	if (CurrentTime - Info.WindowStartTime > 1.0)
	{
		Info.WindowStartTime = CurrentTime;
		Info.RequestCount = 0;
	}

	Info.RequestCount++;

	// 연사 제한 초과
	if (Info.RequestCount > static_cast<int32>(MaxDestructionsPerSecond))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RateLimit] 플레이어 %s: 초당 %d회 (최대: %.0f)"),
			*Player->GetName(), Info.RequestCount, MaxDestructionsPerSecond);
		return false;
	}

	return true;
}

void URealtimeDestructibleMeshComponent::ServerEnqueueOps_Implementation(const TArray<FRealtimeDestructionRequest>& Requests)
{
	UE_LOG(LogTemp, Display, TEXT("ServerEnqueueOps: 서버에서 %d개 요청 수신"), Requests.Num());

	TArray<FRealtimeDestructionOp> Ops;
	Ops.Reserve(Requests.Num());

	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		// 세부 검증 (킥은 안 하고 요청만 거부)
		EDestructionRejectReason Reason;
		if (!ValidateDestructionRequest(Request, nullptr, Reason))
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerEnqueueOps] 요청 거부 - 사유: %d"), static_cast<int32>(Reason));
			continue;
		}

		FRealtimeDestructionOp Op;
		Op.OpId.Value = NextOpId++;
		Op.Sequence = NextSequence++;
		Op.Request = Request;
		Ops.Add(Op);
	}

	// 유효한 Op가 있으면 Multicast
	if (Ops.Num() > 0)
	{
		MulticastApplyOps(Ops);
	}
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

bool URealtimeDestructibleMeshComponent::BuildMeshSnapshot(FRealtimeMeshSnapshot& Out)
{
	Out.Version = 1;
	Out.Payload.Empty();

	// Cell 메시 모드
	if (bUseCellMeshes && CellMeshComponents.Num() > 0)
	{
		FMemoryWriter Ar(Out.Payload);

		// Cell 개수 저장
		int32 CellCount = CellMeshComponents.Num();
		Ar << CellCount;

		// 각 Cell 메시 직렬화
		for (const auto& CellComp : CellMeshComponents)
		{
			if (CellComp && CellComp->GetDynamicMesh())
			{
				FDynamicMesh3 MeshCopy;
				CellComp->GetDynamicMesh()->ProcessMesh([&MeshCopy](const FDynamicMesh3& ReadMesh)
				{
					MeshCopy = ReadMesh;
				});
				MeshCopy.Serialize(Ar);
			}
		}

		// 현재 구멍 수 저장
		int32 HoleCount = CurrentHoleCount;
		Ar << HoleCount;

		UE_LOG(LogTemp, Display, TEXT("[BuildMeshSnapshot] Cell 모드: %d cells, %d bytes"),
			CellCount, Out.Payload.Num());
		return true;
	}

	// 단일 메시 모드
	UDynamicMesh* DynMesh = GetDynamicMesh();
	if (!DynMesh)
	{
		return false;
	}

	FMemoryWriter Ar(Out.Payload);

	// Cell 개수 0 = 단일 메시 모드
	int32 CellCount = 0;
	Ar << CellCount;

	// 메시 직렬화
	FDynamicMesh3 MeshCopy;
	DynMesh->ProcessMesh([&MeshCopy](const FDynamicMesh3& ReadMesh)
	{
		MeshCopy = ReadMesh;
	});
	MeshCopy.Serialize(Ar);

	// 현재 구멍 수 저장
	int32 HoleCount = CurrentHoleCount;
	Ar << HoleCount;

	UE_LOG(LogTemp, Display, TEXT("[BuildMeshSnapshot] 단일 메시 모드: %d bytes"), Out.Payload.Num());
	return true;
}

bool URealtimeDestructibleMeshComponent::ApplyMeshSnapshot(const FRealtimeMeshSnapshot& In)
{
	if (In.Payload.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ApplyMeshSnapshot] 빈 스냅샷"));
		return false;
	}

	FMemoryReader Ar(In.Payload);

	// Cell 개수 읽기
	int32 CellCount = 0;
	Ar << CellCount;

	// Cell 메시 모드
	if (CellCount > 0)
	{
		if (CellMeshComponents.Num() != CellCount)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ApplyMeshSnapshot] Cell 개수 불일치: 예상 %d, 실제 %d"),
				CellCount, CellMeshComponents.Num());
			return false;
		}

		// 각 Cell 메시 역직렬화
		for (int32 i = 0; i < CellCount; ++i)
		{
			FDynamicMesh3 LoadedMesh;
			LoadedMesh.Serialize(Ar);

			if (CellMeshComponents[i] && CellMeshComponents[i]->GetDynamicMesh())
			{
				CellMeshComponents[i]->SetMesh(MoveTemp(LoadedMesh));
			}
		}

		// 구멍 수 복원
		int32 HoleCount = 0;
		Ar << HoleCount;
		CurrentHoleCount = HoleCount;

		UE_LOG(LogTemp, Display, TEXT("[ApplyMeshSnapshot] Cell 모드 적용: %d cells, HoleCount: %d"),
			CellCount, CurrentHoleCount);
		return true;
	}

	// 단일 메시 모드
	UDynamicMesh* DynMesh = GetDynamicMesh();
	if (!DynMesh)
	{
		return false;
	}

	FDynamicMesh3 LoadedMesh;
	LoadedMesh.Serialize(Ar);

	// 메시 적용
	SetMesh(MoveTemp(LoadedMesh));

	// 구멍 수 복원
	int32 HoleCount = 0;
	Ar << HoleCount;
	CurrentHoleCount = HoleCount;

	UE_LOG(LogTemp, Display, TEXT("[ApplyMeshSnapshot] 단일 메시 모드 적용, HoleCount: %d"), CurrentHoleCount);
	return true;
}

void URealtimeDestructibleMeshComponent::GetDestructionSettings(int32& OutMaxHoleCount, int32& OutMaxOpsPerFrame, int32& OutMaxBatchSize)
{
	OutMaxHoleCount = MaxHoleCount;
	OutMaxOpsPerFrame = MaxOpsPerFrame;
	OutMaxBatchSize = MaxBatchSize;
}


bool URealtimeDestructibleMeshComponent::InitializeFromStaticMeshInternal(UStaticMesh* InMesh, bool bForce)
{
	// 1. 유효성 검사
	if (!InMesh)
	{
		// 메쉬가 None이 되면 화면에서도 지워줍니다.
		if (UDynamicMesh* Mesh = GetDynamicMesh())
		{
			Mesh->Reset();
		}
		ApplyRenderUpdate();
		return false;
	}
	UE_LOG(LogTemp, Warning, TEXT("New Static Mesh Name: %s"), *InMesh->GetName());

	// 2. 이미 초기화되었고 강제(bForce)가 아니면 스킵
	if (bIsInitialized && !bForce)
	{
		return true;
	}

	UDynamicMesh* DynamicMesh = GetDynamicMesh();
	if (!DynamicMesh)
	{
		return false;
	}
	DynamicMesh->EditMesh([](FDynamicMesh3& Mesh) {
		Mesh.Clear();
		});

	// =========================================================
	// [핵심 1] 에디터에게 "나 수정된다!"라고 알림 (Undo/Redo 및 뷰포트 갱신 트리거)
	// =========================================================
#if WITH_EDITOR
	Modify();
	DynamicMesh->Modify();
#endif

	// =========================================================
	// [핵심 2] 기존 데이터 완전 초기화 (찌꺼기 제거)
	// =========================================================
	DynamicMesh->Reset();


	// 3. Static Mesh 복사 설정
	FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
	CopyOptions.bApplyBuildSettings = true;
	CopyOptions.bRequestTangents = true;

	// 만약 Static Mesh의 'Allow CPU Access'가 꺼져있어도 에디터에서는 동작하도록 설정
	CopyOptions.bIgnoreRemoveDegenerates = false;

	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	// 4. 복사 실행
	UDynamicMesh* ResultMesh = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
		InMesh,
		DynamicMesh,
		CopyOptions,
		FGeometryScriptMeshReadLOD(),
		Outcome
	);

	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to copy mesh"));
		return false;
	}

	// 5. 머티리얼 및 콜리전 복사
	CopyMaterialsFromStaticMesh(InMesh);
	SetComplexAsSimpleCollisionEnabled(true);

	// =========================================================
	// [핵심 3] 강제 렌더링 상태 재생성 (기존 ApplyRenderUpdate보다 강력하게)
	// =========================================================
	//NotifyMeshUpdated();        // 내부 데이터 버전 올림
	//MarkRenderStateDirty();     // 렌더 상태 더러움 표시
	//RecreatePhysicsState();     // 물리 상태 재생성 (Bounds 갱신을 위해 필수)
	//RecreateRenderState_Concurrent(); // 렌더 프록시 재생성
	SetMesh(MoveTemp(ResultMesh->GetMeshRef()));

	// 상태값 갱신
	CurrentHoleCount = 0;
	bIsInitialized = true; // Construction Script에서 중복 실행 방지
	OnInitialized.Broadcast();

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

	if (MaxHoleCount > 0 && CurrentHoleCount >= MaxHoleCount)
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

void URealtimeDestructibleMeshComponent::ApplyRenderUpdate()
{
	NotifyMeshUpdated();
	MarkRenderStateDirty();
	RecreateRenderState_Concurrent();

}

void URealtimeDestructibleMeshComponent::ApplyCollisionUpdate(UDynamicMeshComponent* TargetComp)
{
	TargetComp->UpdateCollision();

	/*
	 * deprecated_realdestruction
	 * UpdateCollision 내부에서 RecreatePhysicsState함수 호출
	 */
	 // RecreatePhysicsState();
}

void URealtimeDestructibleMeshComponent::ApplyCollisionUpdateAsync(UDynamicMeshComponent* TargetComp)
{
	UE_LOG(LogTemp, Display, TEXT("Call Collision Update %f"), FPlatformTime::Seconds());
	TargetComp->UpdateCollision(true);
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

int32 URealtimeDestructibleMeshComponent::GetChunkIndex(const UPrimitiveComponent* ChunkMesh)
{
	if (int32* Index = ChunkIndexMap.Find(ChunkMesh))
	{
		return *Index;
	}

	return INDEX_NONE;
}

bool URealtimeDestructibleMeshComponent::IsChunkValid(int32 ChunkIndex) const
{
	return GetChunkMeshComponent(ChunkIndex) != nullptr;
}

UDynamicMeshComponent* URealtimeDestructibleMeshComponent::GetChunkMeshComponent(int32 ChunkIndex) const
{
	if (ChunkIndex == INDEX_NONE)
	{
		return nullptr;
	}

	return CellMeshComponents[ChunkIndex].Get();
}

bool URealtimeDestructibleMeshComponent::GetChunkMesh(FDynamicMesh3& OutMesh, int32 ChunkIndex) const
{
	if (auto MeshComp = GetChunkMeshComponent(ChunkIndex))
	{
		MeshComp->ProcessMesh([&](const FDynamicMesh3& Source)
			{
				OutMesh = Source;
			});

		return true;
	}

	return false;
}

bool URealtimeDestructibleMeshComponent::CheckAndSetChunkBusy(int32 ChunkIndex)
{
	// 비트 배열의 인덱스
	// 0 ~ 63(0), 64 ~ 127(1) ...
	const int32 BitIndex = ChunkIndex / 64;
	if (!ChunkBusyBits.IsValidIndex(BitIndex))
	{
		// 유효하지 않은 CellIndex의 경우 로그를 출력하고 true를 반환해서 작업하지 못하게 한다.
		UE_LOG(LogTemp, Warning, TEXT("Invalid Cell Index: %d"), ChunkIndex);
		return true;
	}

	// 해당 비트 마스크에서 비트 위치
	const int32 BitOffset = ChunkIndex % 64;
	const uint64 BitMask = 1ULL << BitOffset;

	const bool bIsBusy = (ChunkBusyBits[BitIndex] & BitMask) != 0;
	if (!bIsBusy)
	{
		ChunkBusyBits[BitIndex] |= BitMask;
	}

	return bIsBusy;
}

void URealtimeDestructibleMeshComponent::FindChunksInRadius(const FVector& WorldCenter, float Radius, TArray<int32>& OutChunkIndices, bool bAppend)
{
	if (!bAppend)
	{
		OutChunkIndices.Reset();
	}

	if (GridToChunkMap.Num() == 0 || SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		return;
	}

	// 월드 좌표를 로컬 좌표로 변환
	FVector LocalCenter = GetComponentTransform().InverseTransformPosition(WorldCenter);

	// 스케일도 고려해서 Radius 변환 (비균일 스케일이면 근사치)
	FVector LocalRadius = GetComponentTransform().InverseTransformVector(FVector(Radius));
	float LocalRadiusScalar = LocalRadius.GetAbsMax();  // 또는 평균값

	const FVector& CellSize = CachedCellSize;
	const FBox& MeshBounds = CachedMeshBounds;

	//  로컬 좌표로 계산
	FVector MinPos = LocalCenter - FVector(LocalRadiusScalar);
	FVector MaxPos = LocalCenter + FVector(LocalRadiusScalar);

	int32 MinGridX = FMath::Clamp(static_cast<int32>((MinPos.X - MeshBounds.Min.X) / CellSize.X), 0, SliceCount.X - 1);
	int32 MaxGridX = FMath::Clamp(static_cast<int32>((MaxPos.X - MeshBounds.Min.X) / CellSize.X), 0, SliceCount.X - 1);
	int32 MinGridY = FMath::Clamp(static_cast<int32>((MinPos.Y - MeshBounds.Min.Y) / CellSize.Y), 0, SliceCount.Y - 1);
	int32 MaxGridY = FMath::Clamp(static_cast<int32>((MaxPos.Y - MeshBounds.Min.Y) / CellSize.Y), 0, SliceCount.Y - 1);
	int32 MinGridZ = FMath::Clamp(static_cast<int32>((MinPos.Z - MeshBounds.Min.Z) / CellSize.Z), 0, SliceCount.Z - 1);
	int32 MaxGridZ = FMath::Clamp(static_cast<int32>((MaxPos.Z - MeshBounds.Min.Z) / CellSize.Z), 0, SliceCount.Z - 1);


	/*
	 * 1. GridToChunk를 만들 때 하나의 그리드에 하나의 청크가 매핑되도록 생성됨
	 * 2. 3중 for문을 돌면서 GridIndex가 다름
	 * 위 2가지 이유로 청크인덱스가 중복될 수 없으므로 TSet을 주석처리해서 배열의 메모리 재활용 하도록 변경 
	 */
	// 해당 범위의 그리드 셀만 순회
	// TSet<int32> UniqueChunks;
	for (int32 Z = MinGridZ; Z <= MaxGridZ; ++Z)
	{
		for (int32 Y = MinGridY; Y <= MaxGridY; ++Y)
		{
			for (int32 X = MinGridX; X <= MaxGridX; ++X)
			{
				int32 GridIndex = X + Y * SliceCount.X + Z * SliceCount.X * SliceCount.Y;

				if (GridIndex >= 0 && GridIndex < GridToChunkMap.Num())
				{
					int32 ChunkId = GridToChunkMap[GridIndex];
					if (ChunkId != INDEX_NONE)
					{
						OutChunkIndices.Add(ChunkId);
						// UniqueChunks.Add(ChunkId);
					}
				}
			}
		}
	}

	// OutChunkIndices = UniqueChunks.Array();
}

void URealtimeDestructibleMeshComponent::FindChunksAlongLine(const FVector& WorldStart, const FVector& WorldEnd, float Radius, TArray<int32>& OutChunkIndices, bool bAppend)
{
	if (!bAppend)
	{
		OutChunkIndices.Reset();
	}
	
	FVector Forward = WorldEnd - WorldStart;	
	if (Forward.IsNearlyZero())
	{
		return;
	}

	Forward.Normalize();
	FVector Right, Up;
	Forward.FindBestAxisVectors(Right, Up);

	const float OffsetRadius = Radius * 0.9f;

	// 실린더의 중심과 4방향의 offset
	FVector Offsets[] = {
		FVector::ZeroVector,
		Right * OffsetRadius,
		-Right * OffsetRadius,
		Up * OffsetRadius,
		-Up * OffsetRadius
	};

	// 5방향 DDA 시작
	for (const FVector& Offset : Offsets)
	{
		FVector RayStart = WorldStart + Offset;
		FVector RayEnd = WorldEnd + Offset;

		FindChunksAlongLineInternal(RayStart, RayEnd, OutChunkIndices);
	}

	if (OutChunkIndices.Num() > 1)
	{
		OutChunkIndices.Sort();
		int32 UniqueCount = Algo::Unique(OutChunkIndices);
		OutChunkIndices.SetNum(UniqueCount);
	}
}

void URealtimeDestructibleMeshComponent::ClearChunkBusy(int32 ChunkIndex)
{
	const int32 BitIndex = ChunkIndex / 64;
	if (!ChunkBusyBits.IsValidIndex(BitIndex))
	{
		// 유효하지 않은 CellIndex의 경우 로그를 출력하고 true를 반환해서 작업하지 못하게 한다.
		UE_LOG(LogTemp, Warning, TEXT("Invalid Cell Index: %d"), ChunkIndex);
		return;
	}

	// 해당 비트 마스크에서 비트 위치
	const int32 BitOffset = ChunkIndex % 64;

	// 해당 위치의 비트를 1로 설정한 뒤 반전
	// 반전된 결과를 AND 연산
	// 원하는 위치의 비트는 0이 되고 나머지 비트는 기존의 값을 유지
	ChunkBusyBits[BitIndex] &= ~(1ULL << BitOffset);
}

void URealtimeDestructibleMeshComponent::ClearAllChunkBusyBits()
{
	for (auto& BitMask : ChunkBusyBits)
	{
		BitMask = 0ULL;
	}
} 
void URealtimeDestructibleMeshComponent::SetChunkBits(int32 ChunkIndex, int32& BitIndex, int32& BitOffset)
{
	// 64bit 를 사용 중이니, bit indexing 
	BitIndex = ChunkIndex / 64;

	if (!ChunkSubtractBusyBits.IsValidIndex(BitIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ClearChunkSubtractBusy: Invalid ChunkIndex: %d"), ChunkIndex);
		return;
	}
	BitOffset = ChunkIndex % 64;
}

void URealtimeDestructibleMeshComponent::ApplyBooleanOperationResult(FDynamicMesh3&& NewMesh, const int32 ChunkIndex, bool bDelayedCollisionUpdate)
{
	if (ChunkIndex == INDEX_NONE)
	{
		return;
	}

	UDynamicMeshComponent* TargetComp = GetChunkMeshComponent(ChunkIndex);
	if (!TargetComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("TargetComp is invalid"));
		return;
	}

	TargetComp->EditMesh([&](FDynamicMesh3& InternalMesh)
		{
			InternalMesh = MoveTemp(NewMesh);
		});

	// CellGraph 갱신을 위해 수정된 청크 추적
	ModifiedChunkIds.Add(ChunkIndex);
	// 디버그 텍스트 갱신 플래그는 기본적으로 구조적 무결성 갱신 후 업데이트되지만, 청크 없는 경우 여기에서 대신 갱신 
	if (CellMeshComponents.Num() == 0)
	{
		bShouldDebugUpdate = true;
	}

	if (bDelayedCollisionUpdate)
	{
		RequestDelayedCollisionUpdate(TargetComp);
	}
	else
	{
		ApplyCollisionUpdate(TargetComp);
	}
}

void URealtimeDestructibleMeshComponent::RequestDelayedCollisionUpdate(UDynamicMeshComponent* TargetComp)
{
	if (!TargetComp)
	{
		return;
	}
	// InRate 이내에 호출되는 경우 타이머 리셋
	if (UWorld* World = GetWorld())
	{
		FTimerDelegate CollsionTimerDelegate;
		CollsionTimerDelegate.BindUObject(this, &URealtimeDestructibleMeshComponent::ApplyCollisionUpdateAsync, TargetComp);
		UE_LOG(LogTemp, Display, TEXT("Set Collision Timer %f"), FPlatformTime::Seconds());
		World->GetTimerManager().SetTimer(
			CollisionUpdateTimerHandle,
			CollsionTimerDelegate,			
			0.05f,
			false);
	}
}

void URealtimeDestructibleMeshComponent::UpdateDebugText()
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

	const int32 ChunkCount = CellMeshComponents.Num();
	const int32 NodeCount = CellGraph.GetNodeCount();

	int32 CellCount = 0;
	for (int32 ChunkId = 0; ChunkId < CellGraph.GetChunkCount(); ++ChunkId)
	{
		if (const FChunkCellCache* Cache = CellGraph.GetChunkCellCache(ChunkId))
		{
			CellCount += Cache->CellIds.Num();
		}
	}

	int32 EdgeCount = 0;
	for (int32 NodeIdx = 0; NodeIdx < NodeCount; ++NodeIdx)
	{
		if (const FChunkCellNode* Node = CellGraph.GetNode(NodeIdx))
		{
			EdgeCount += Node->Neighbors.Num();
		}
	}
	EdgeCount /= 2;

	// 디버그 텍스트 생성
	DebugText = FString::Printf(
		TEXT("Vertices: %d\nTriangles: %d\nHoles: %d / %d\nInitialized: %s\nNetwork Mode: %s\n<Cell Structures>\nChunks: %d | Cells: %d | Nodes: %d | Edges: %d"),
		VertexCount,
		TriangleCount,
		HoleCount,
		MaxHoleCount,
		bIsInitialized ? TEXT("Yes") : TEXT("No"),
		*NetModeStr,
		ChunkCount,
		CellCount,
		NodeCount,
		EdgeCount
	);

	bShouldDebugUpdate = false;
}

void URealtimeDestructibleMeshComponent::DrawDebugText() const
{
#if !UE_BUILD_SHIPPING
	UWorld* DebugWorld = GetWorld();
	if (!DebugWorld)
	{
		return;
	}

	float BoundsHeight = Bounds.BoxExtent.Z * 2.0f;
	if (CellMeshComponents.Num() > 0)
	{
		if (SliceCount.Z > 0 && CachedCellSize.Z > 0.0f)
		{
			BoundsHeight = CachedCellSize.Z * SliceCount.Z;
		}
	}

	const float WorldScaleZ = GetComponentTransform().GetScale3D().Z;
	const FVector TextLocation = GetComponentLocation() + FVector(0, 0, BoundsHeight * WorldScaleZ);
	DrawDebugString(DebugWorld, TextLocation, DebugText, nullptr, FColor::Cyan, 0.0f, true, 1.5f);
#endif
}

void URealtimeDestructibleMeshComponent::DrawCellGraphDebug()
{
	if (!CellGraph.IsGraphBuilt())
	{
		// 매 프레임 경고는 스팸이 되므로 주석 처리
		// UE_LOG(LogTemp, Warning, TEXT("DrawCellGraphDebug: CellGraph not built"));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 디버그 로그는 첫 프레임만 출력 (스팸 방지)
	static bool bFirstDraw = true;
	if (bFirstDraw)
	{
		UE_LOG(LogTemp, Log, TEXT("DrawCellGraphDebug: Drawing %d nodes"), CellGraph.GetNodeCount());
		bFirstDraw = false;
	}

	const FTransform& ComponentTransform = GetComponentTransform();
	const int32 NodeCount = CellGraph.GetNodeCount();

	// 노드 중심 위치 캐시 (연결선 그리기용)
	TArray<FVector> NodeCenters;
	NodeCenters.SetNum(NodeCount);

	// 1. 각 노드(Cell)의 바운드 중심에 점 그리기
	for (int32 NodeIdx = 0; NodeIdx < NodeCount; ++NodeIdx)
	{
		const FChunkCellNode* Node = CellGraph.GetNode(NodeIdx);
		if (!Node)
		{
			continue;
		}

		const FChunkCellCache* Cache = CellGraph.GetChunkCellCache(Node->ChunkId);
		if (!Cache || Node->CellId >= Cache->CellBounds.Num())
		{
			continue;
		}

		const FBox& NodeBounds = Cache->CellBounds[Node->CellId];
		if (!NodeBounds.IsValid)
		{
			continue;
		}

		// 로컬 바운드 중심을 월드 좌표로 변환
		const FVector LocalCenter = NodeBounds.GetCenter();
		const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);
		NodeCenters[NodeIdx] = WorldCenter;

		// IntegritySystem에서 Cell 상태 확인
		const FCellKey CellKey(Node->ChunkId, Node->CellId);
		const int32 IntegrityId = IntegritySystem.GetCellIdForKey(CellKey);
		const ECellStructuralState CellState = IntegritySystem.GetCellState(IntegrityId);

		// 상태에 따른 색상: Anchor=녹색, Intact=시안, Destroyed=회색(스킵)
		if (CellState == ECellStructuralState::Destroyed)
		{
			continue; // Destroyed된 셀은 그리지 않음
		}

		const FColor NodeColor = Node->bIsAnchor ? FColor::Green : FColor::Cyan;

		// 노드 위치에 점 그리기 (매 프레임 그리기)
		DrawDebugPoint(World, WorldCenter, 10.0f, NodeColor, false, 0.0f, SDPG_Foreground);
	}

	// 2. 연결된 노드 간에 선 그리기 (중복 방지를 위해 NodeA < NodeB인 경우만)
	TSet<TPair<int32, int32>> DrawnEdges;

	for (int32 NodeIdx = 0; NodeIdx < NodeCount; ++NodeIdx)
	{
		const FChunkCellNode* Node = CellGraph.GetNode(NodeIdx);
		if (!Node)
		{
			continue;
		}

		// 현재 노드가 Destroyed면 스킵
		const FCellKey NodeKey(Node->ChunkId, Node->CellId);
		const int32 NodeIntegrityId = IntegritySystem.GetCellIdForKey(NodeKey);
		if (IntegritySystem.GetCellState(NodeIntegrityId) == ECellStructuralState::Destroyed)
		{
			continue;
		}

		for (const FChunkCellNeighbor& Neighbor : Node->Neighbors)
		{
			// 이웃 노드가 Destroyed면 스킵
			const FCellKey NeighborKey(Neighbor.ChunkId, Neighbor.CellId);
			const int32 NeighborIntegrityId = IntegritySystem.GetCellIdForKey(NeighborKey);
			if (IntegritySystem.GetCellState(NeighborIntegrityId) == ECellStructuralState::Destroyed)
			{
				continue;
			}

			const int32 NeighborNodeIdx = CellGraph.FindNodeIndex(Neighbor.ChunkId, Neighbor.CellId);
			if (NeighborNodeIdx == INDEX_NONE)
			{
				continue;
			}

			// 중복 방지: 작은 인덱스 -> 큰 인덱스 방향만 그리기
			const int32 MinIdx = FMath::Min(NodeIdx, NeighborNodeIdx);
			const int32 MaxIdx = FMath::Max(NodeIdx, NeighborNodeIdx);
			TPair<int32, int32> EdgeKey(MinIdx, MaxIdx);

			if (DrawnEdges.Contains(EdgeKey))
			{
				continue;
			}
			DrawnEdges.Add(EdgeKey);

			// 연결선 그리기 (노란색, 매 프레임 그리기)
			if (NodeCenters.IsValidIndex(NodeIdx) && NodeCenters.IsValidIndex(NeighborNodeIdx))
			{
				DrawDebugLine(World, NodeCenters[NodeIdx], NodeCenters[NeighborNodeIdx],
					FColor::Yellow, false, 0.0f, SDPG_Foreground, 2.0f);
			}
		}
	}
}

void URealtimeDestructibleMeshComponent::SetSourceMeshEnabled(bool bEnabled)
{
	SetVisibility(bEnabled, false);
	if (bEnabled)
	{
		SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); 
		
	}
	else
	{
		SetCollisionEnabled(ECollisionEnabled::NoCollision); 
	}
	SetComponentTickEnabled(bEnabled);
	
	// 물리 상태 강제 갱신
	RecreatePhysicsState();
}
void URealtimeDestructibleMeshComponent::OnRegister()
{
	Super::OnRegister();

	if (bUseCellMeshes && CellMeshComponents.Num() > 0)
	{
		return;  // Cell 모드에서 이미 셀이 있으면 스킵
	}

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

	UE_LOG(LogTemp, Display, TEXT("CellMesh Num %d"), CellMeshComponents.Num());

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

	

	for (int32 i = 0; i < CellMeshComponents.Num(); i++)
	{
		ChunkIndexMap.Add(CellMeshComponents[i].Get(), i);
	}

	int32 NumBits = (CellMeshComponents.Num() + 63) / 64;
	ChunkBusyBits.Init(0ULL, NumBits);
	ChunkSubtractBusyBits.Init(0ULL, NumBits);

	// 런타임 시작 시 CellGraph가 구축되지 않았으면 구축
	if (CellMeshComponents.Num() > 1 && !CellGraph.IsGraphBuilt())
	{
		bCellMeshesValid = true;

		if (GridToChunkMap.Num() == 0)
		{
			BuildGridToChunkMap();
		}
		BuildCellGraph();
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

	// 기존 저장 데이터 호환: CellMeshComponents가 있으면 bCellMeshesValid 자동 설정
	if (!bCellMeshesValid && CellMeshComponents.Num() > 1)
	{
		int32 ValidCount = 0;
		for (const auto& Cell : CellMeshComponents)
		{
			if (Cell && Cell->IsValidLowLevel()) ValidCount++;
		}
		if (ValidCount > 0)
		{
			bCellMeshesValid = true;
			bUseCellMeshes = true;
			UE_LOG(LogTemp, Log, TEXT("BeginPlay: Auto-detected %d valid CellMeshComponents, setting bCellMeshesValid=true"), ValidCount);
		}
	}

	// 런타임 시작 시 CellGraph가 구축되지 않았으면 구축
	UE_LOG(LogTemp, Warning, TEXT("BeginPlay: bCellMeshesValid=%d, CellGraph.IsGraphBuilt=%d, CellMeshComponents.Num=%d"),
		bCellMeshesValid, CellGraph.IsGraphBuilt(), CellMeshComponents.Num());

	/** Culstering 관련 초기화 */
	if (bEnableClustering)
	{
		// Cluster Comp가 없을 경우 생성
		if (!BulletClusterComponent)
		{
			BulletClusterComponent = NewObject<UBulletClusterComponent>(GetOwner()); 
			BulletClusterComponent->RegisterComponent();
		}

		if (BulletClusterComponent)
		{
			BulletClusterComponent->Init(MaxMergeDistance , MaxClusterRadius, MinClusterCount, ClusterRaidusOffset);
			BulletClusterComponent->SetOwnerMesh(this);
		}
	}
}

void URealtimeDestructibleMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

#if !UE_BUILD_SHIPPING
	if (bShowDebugText)
	{
		if (bShouldDebugUpdate)
		{
			UpdateDebugText();
		}
		DrawDebugText();
	}
#endif

	if (bEnableMultiWorkers)
	{
		if (BooleanProcessor.IsValid())
		{
			BooleanProcessor->SetCachedMeshOptimization(bUseCachedMeshOptimization);
			
			// 매틱 Subtract Queue를 비워준다.
			//BooleanProcessor->KickProcessIfNeeded();
			BooleanProcessor->KickProcessIfNeededPerChunk();
		}
	}
	else
	{
		if (GetChunkNum() > 0)
		{
			BooleanProcessor->KickProcessIfNeededPerChunk();
		}
	}

	// 에디터에서 런타임 변경 시 프로세서에 동기화
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->SetCachedMeshOptimization(bUseCachedMeshOptimization);
	}

	// 수정된 청크가 있으면 CellGraph 갱신 (BooleanProcessor 비동기 작업 완료 후)
	if (ModifiedChunkIds.Num() > 0)
	{
		UpdateCellGraphForModifiedChunks();
	}
	
	// CellGraph 노드 및 연결 디버그 표시
	if (bShowCellGraphDebug)
	{
		DrawCellGraphDebug();
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

		// Late Join: Op 히스토리에 기록 (서버에서만)
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			for (const FCompactDestructionOp& CompactOp : PendingServerBatchOpsCompact)
			{
				if (AppliedOpHistory.Num() < MaxOpHistorySize)
				{
					AppliedOpHistory.Add(CompactOp);
				}
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

		// Late Join: Op 히스토리에 기록 (서버에서만)
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			for (const FRealtimeDestructionOp& Op : PendingServerBatchOps)
			{
				if (AppliedOpHistory.Num() < MaxOpHistorySize)
				{
					AppliedOpHistory.Add(FCompactDestructionOp::Compress(Op.Request, Op.Sequence));
				}
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

int32 URealtimeDestructibleMeshComponent::BuildCellMeshesFromGeometryCollection()
{
	if (!FracturedGeometryCollection)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: FracturedGeometryCollection is not set."));
		return 0;
	}

	// 기존에 있던 Old DynamicMeshComponent 정리
	for (UDynamicMeshComponent* OldComp : CellMeshComponents)
	{
		if (OldComp)
		{
			OldComp->DestroyComponent();
		}
	}
	CellMeshComponents.Empty();

	// GeometryCollection 데이터 가져오기
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GeometryCollectionPtr = FracturedGeometryCollection->GetGeometryCollection();

	if (!GeometryCollectionPtr.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: Invalid GeometryCollection data."));
		return 0;
	}

	const FGeometryCollection& GC = *GeometryCollectionPtr;

	// 만들어진 조각이 없다면 return;
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

	// 디버깅: GeometryCollection의 모든 속성 이름 출력
	UE_LOG(LogTemp, Log, TEXT("=== GeometryCollection Attributes ==="));
	for (const FName& GroupName : GC.GroupNames())
	{
		UE_LOG(LogTemp, Log, TEXT("Group: %s"), *GroupName.ToString());
		for (const FName& AttrName : GC.AttributeNames(GroupName))
		{
			UE_LOG(LogTemp, Log, TEXT("  - %s"), *AttrName.ToString());
		}
	}
	UE_LOG(LogTemp, Log, TEXT("====================================="));

	// UV 찾기 - UVLayer0 속성 사용
	const TManagedArray<FVector2f>* UVsArray = GC.FindAttribute<FVector2f>("UVLayer0", FGeometryCollection::VerticesGroup);

	if (UVsArray && UVsArray->Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGC: Found UVLayer0 with %d elements"), UVsArray->Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGC: Found UVLayer0 with %d elements"), UVsArray->Num());
	}

	// MaterialID 가져오기 (FacesGroup에 저장됨)
	const TManagedArray<int32>* MaterialIDs = GC.FindAttribute<int32>("MaterialID", FGeometryCollection::FacesGroup);

	//=========================================================================
	// 1패스: 버텍스를 조각별로 분류 O(M)
	//=========================================================================
	TArray<TArray<int32>> VertexIndicesByTransform;
	VertexIndicesByTransform.SetNum(NumTransforms);


	struct FTriangleData
	{
		FIntVector Indices;
		int32 MaterialID;
	};

	TArray<TArray<FTriangleData>> TrianglesByTransform;
	TrianglesByTransform.SetNum(NumTransforms);

	// Vertex 분류
	for (int32 VertexIdx = 0; VertexIdx < Vertices.Num(); ++VertexIdx)
	{
		int32 TransformIdx = BoneMap[VertexIdx];
		if (TransformIdx >= 0 && TransformIdx < NumTransforms)
		{
			VertexIndicesByTransform[TransformIdx].Add(VertexIdx);
		}
	}
	// Triangle 분류
	for (int32 TriIdx = 0; TriIdx < Indices.Num(); ++TriIdx)
	{
		const FIntVector& Tri = Indices[TriIdx];
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
	// 각 Transform별로 DynamicMeshComponent생성
	//=========================================================================

	CellMeshComponents.Reserve(NumTransforms);
	int32 ExtractedCount = 0;

	for (int32 TransformIdx = 0; TransformIdx < NumTransforms; ++TransformIdx)
	{
		const TArray<int32>& MyVertexIndices = VertexIndicesByTransform[TransformIdx];
		const TArray<FTriangleData>& MyTriangles = TrianglesByTransform[TransformIdx];
		 
		// 빈 조각 + 첫 번째 스킵
		if (TransformIdx == 0 || MyVertexIndices.Num() == 0 || MyTriangles.Num() == 0)
		{
			CellMeshComponents.Add(nullptr);
			continue;
		}

		// DynamicMeshComponent 생성 ( RF_Transactional : Ctrl Z 지원) )
		UDynamicMeshComponent* CellComp = NewObject<UDynamicMeshComponent>(
			GetOwner(),
			UDynamicMeshComponent::StaticClass(),
			*FString::Printf(TEXT("Cell_%d"), TransformIdx),
			RF_Transactional
		);

		if (!CellComp)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to create CellMeshComponent %d"), TransformIdx);
			CellMeshComponents.Add(nullptr);
			continue;
		}


		// Compoment기본 설정
		//CellComp->SetupAttachment(this);
		if (AActor* Owner = GetOwner())
		{
			CellComp->SetupAttachment(Owner->GetRootComponent());
		}
		//CellComp->SetRelativeTransform(FTransform::Identity);
			 
		// Collision 설정
		CellComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CellComp->SetCollisionProfileName(TEXT("BlockAll"));
		CellComp->SetComplexAsSimpleCollisionEnabled(true);
		

		// Tick 활성화   
		CellComp->PrimaryComponentTick.bCanEverTick = false;

		// Global - > Local Index 맵핑
		TMap<int32, int32> GlobalToLocalVertex;
		GlobalToLocalVertex.Reserve(MyVertexIndices.Num());

		TArray<FVector3f> LocalVertices;
		TArray<FVector3f> LocalNormals;
		TArray<FVector2f> LocalUVs;

		if (Normals)
		{
			LocalNormals.Reserve(MyVertexIndices.Num());
		}
		if (UVsArray)
		{
			LocalUVs.Reserve(MyVertexIndices.Num());
		}

		for (int32 GlobalIdx : MyVertexIndices)
		{
			int LocalIdx = LocalVertices.Num();
			GlobalToLocalVertex.Add(GlobalIdx, LocalIdx);

			LocalVertices.Add(Vertices[GlobalIdx]);
			if (Normals) LocalNormals.Add((*Normals)[GlobalIdx]);
			if (UVsArray) LocalUVs.Add((*UVsArray)[GlobalIdx]);
		}

		// Triangle 인덱스 로컬로 변환
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
			CellComp->DestroyComponent();  // 이미 등록한 컴포넌트 정리
			CellMeshComponents.Add(nullptr);
			continue;
		}

		// DynamicMesh3 생성
		//TSharedPtr<UE::Geometry::FDynamicMesh3> NewMesh = MakeShared<UE::Geometry::FDynamicMesh3>();


		// 내부 메시 가져오기
		FDynamicMesh3* NewMesh = CellComp->GetMesh();

		// 속성 활성화
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


		//=========================================================================
		// 동일 위치 엣지 병합 (UV seam 등으로 인한 정점 분리 해결)
		// - 연결성 분석(CellGraph)이 정확하게 동작하도록 토폴로지 정리
		//=========================================================================
		{
			UE::Geometry::FMergeCoincidentMeshEdges MergeOp(NewMesh);
			MergeOp.MergeSearchTolerance = 0.001;  // 매우 가까운 정점만 병합 (0.001cm = 0.01mm)
			MergeOp.OnlyUniquePairs = false;       // 모든 coincident 엣지 병합
			if (MergeOp.Apply())
			{
				UE_LOG(LogTemp, Log, TEXT("Cell_%d: Merged coincident edges"), TransformIdx);
			}
		}

		// [핵심 해결책 1] 이 컴포넌트는 에디터 레벨 인스턴스의 일부라고 명시합니다.
		// 이 설정이 없으면 Actor를 움직일 때마다 컴포넌트가 초기화되거나 연결이 끊깁니다.
		CellComp->CreationMethod = EComponentCreationMethod::Instance;

		// [핵심 해결책 2] 부착 대상을 명확히 합니다.
		// Actor의 Root보다는, 현재 부서지고 있는 '이 컴포넌트(this)'에 붙이는 것이 
		// 계층 구조상 더 안전하며, 부모의 Transform을 그대로 상속받기 좋습니다.
		CellComp->SetupAttachment(this);
		// 만약 이미 Register된 상태라면 SetupAttachment 대신 아래를 사용하세요:
		// CellComp->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);

		// 위치 초기화 (부모 기준 0,0,0 = 부모와 겹침)
		CellComp->SetRelativeTransform(FTransform::Identity);


		// 등록 
		CellComp->RegisterComponent();

		// 메시 변경 알림
		CellComp->NotifyMeshUpdated();
		// CellMeshComponent에 머티리얼 설정 (GC에서 복사)
		const TArray<UMaterialInterface*>& GCMaterials = FracturedGeometryCollection->Materials;
		if (GCMaterials.Num() > 0)
		{

			// ConfigureMaterialSet으로 메테리얼 배열 설정 (다중 메테리얼 지원)
			CellComp->ConfigureMaterialSet(GCMaterials);
		}
#if WITH_EDITOR
		// 에디터에게 이 컴포넌트 관리를 위임
		if (AActor* Owner = GetOwner())
		{
			Owner->AddInstanceComponent(CellComp);
		}

#endif
		CellMeshComponents.Add(CellComp);
		++ExtractedCount;
	}

	// Bounds 계산
	CellBounds.SetNum(NumTransforms);
	for (int32 i = 0; i < NumTransforms; ++i)
	{
		if (CellMeshComponents[i])
		{
			const FDynamicMesh3* Mesh = CellMeshComponents[i]->GetMesh();
			if (Mesh && Mesh->TriangleCount() > 0)
			{
				UE::Geometry::FAxisAlignedBox3d MeshBounds = Mesh->GetBounds();
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

	UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGeometryCollection: Extracted %d meshes from %d transforms"),
		ExtractedCount, NumTransforms);

	if (bCellMeshesValid)
	{
		if (UDynamicMesh* ParentMesh = GetDynamicMesh())
		{
			ParentMesh->EditMesh([](FDynamicMesh3& Mesh) {
				Mesh.Clear();
				});
		}
		//SetVisibility(false, false);
		SetSourceMeshEnabled(false);

		NotifyMeshUpdated();
		MarkRenderStateDirty();

		// GridToChunkMap 구축 (그리드 인덱스 -> ChunkId 매핑)
		BuildGridToChunkMap();

		// CellGraph 및 StructuralIntegritySystem 초기화
		BuildCellGraph();

#if WITH_EDITOR
		// 에디터 뷰포트 및 Details 패널 갱신
		if (AActor* Owner = GetOwner())
		{
			Owner->Modify();

			// 에디터 뷰포트 갱신 강제
			if (GEditor)
			{
				GEditor->RedrawLevelEditingViewports(true);
			}
		}

#endif
	}
	return ExtractedCount;
}

void URealtimeDestructibleMeshComponent::BuildGridToChunkMap()
{
	GridToChunkMap.Reset();

	if (SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		return;
	}

	const int32 ExpectedChunkCount = SliceCount.X * SliceCount.Y * SliceCount.Z;
	GridToChunkMap.Init(INDEX_NONE, ExpectedChunkCount);

	// 메시 바운드 계산
	FBox MeshBounds(ForceInit);
	if (SourceStaticMesh)
	{
		MeshBounds = SourceStaticMesh->GetBoundingBox();
	}
	else
	{
		for (const UDynamicMeshComponent* CellComp : CellMeshComponents)
		{
			if (CellComp)
			{
				MeshBounds += CellComp->Bounds.GetBox();
			}
		}
	}
	const FVector BoundsSize = MeshBounds.GetSize();
	const FVector CellSize(
		BoundsSize.X / SliceCount.X,
		BoundsSize.Y / SliceCount.Y,
		BoundsSize.Z / SliceCount.Z);

	CachedMeshBounds = MeshBounds;
	CachedCellSize = CellSize;
	if (!MeshBounds.IsValid)
	{
		return;
	} 

	UE_LOG(LogTemp, Log, TEXT("BuildGridToChunkMap: MeshBounds Min=(%.2f, %.2f, %.2f) Max=(%.2f, %.2f, %.2f)"),
		MeshBounds.Min.X, MeshBounds.Min.Y, MeshBounds.Min.Z,
		MeshBounds.Max.X, MeshBounds.Max.Y, MeshBounds.Max.Z);
	UE_LOG(LogTemp, Log, TEXT("BuildGridToChunkMap: CellSize=(%.2f, %.2f, %.2f), SliceCount=(%d, %d, %d)"),
		CellSize.X, CellSize.Y, CellSize.Z, SliceCount.X, SliceCount.Y, SliceCount.Z);

	// CellMeshComponents[0]은 루트 본(nullptr)이므로 인덱스 1부터 시작
	for (int32 ChunkId = 1; ChunkId < CellMeshComponents.Num(); ++ChunkId)
	{
		if (!CellMeshComponents[ChunkId])
		{
			continue;
		}

		// 프래그먼트의 월드 바운드 중심을 부모 컴포넌트의 로컬 좌표계로 변환
		const FBox WorldChunkBounds = CellMeshComponents[ChunkId]->Bounds.GetBox();
		const FVector WorldCenter = WorldChunkBounds.GetCenter();
		const FVector Center = GetComponentTransform().InverseTransformPosition(WorldCenter);

		// 그리드 좌표 계산 (클램프로 경계 처리)
		const int32 GridX = FMath::Clamp(
			static_cast<int32>((Center.X - MeshBounds.Min.X) / CellSize.X),
			0, SliceCount.X - 1);
		const int32 GridY = FMath::Clamp(
			static_cast<int32>((Center.Y - MeshBounds.Min.Y) / CellSize.Y),
			0, SliceCount.Y - 1);
		const int32 GridZ = FMath::Clamp(
			static_cast<int32>((Center.Z - MeshBounds.Min.Z) / CellSize.Z),
			0, SliceCount.Z - 1);

		const int32 GridIndex = GridX + GridY * SliceCount.X + GridZ * SliceCount.X * SliceCount.Y;

		if (GridIndex >= 0 && GridIndex < ExpectedChunkCount)
		{
			if (GridToChunkMap[GridIndex] != INDEX_NONE)
			{
				UE_LOG(LogTemp, Warning, TEXT("    GridIndex %d already occupied by ChunkId %d, overwriting with %d"),
					GridIndex, GridToChunkMap[GridIndex], ChunkId);
			}
			GridToChunkMap[GridIndex] = ChunkId;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BuildGridToChunkMap: Built map for %d grid cells"), ExpectedChunkCount);
}

bool URealtimeDestructibleMeshComponent::BuildCellGraph()
{
	// 1. 사전 조건 검사
	if (!bCellMeshesValid || CellMeshComponents.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellGraph: Cell meshes not valid. Call BuildCellMeshesFromGeometryCollection first."));
		return false;
	}

	if (SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellGraph: Invalid SliceCount (%d, %d, %d)"),
			SliceCount.X, SliceCount.Y, SliceCount.Z);
		return false;
	}

	const int32 ExpectedChunkCount = SliceCount.X * SliceCount.Y * SliceCount.Z;
	// CellMeshComponents[0]은 GC 루트 본(nullptr)이므로, 실제 프래그먼트는 인덱스 1부터 시작
	// 따라서 유효한 프래그먼트 슬롯 수는 CellMeshComponents.Num() - 1
	const int32 ActualFragmentSlots = CellMeshComponents.Num() - 1;
	if (ActualFragmentSlots != ExpectedChunkCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellGraph: Chunk count mismatch. Expected %d (from SliceCount), got %d (excluding root bone)"),
			ExpectedChunkCount, ActualFragmentSlots);
		return false;
	}

	// 2. SourceStaticMesh 바운드 가져오기
	FBox MeshBounds(ForceInit);
	if (SourceStaticMesh)
	{
		MeshBounds = SourceStaticMesh->GetBoundingBox();
	}
	else
	{
		// SourceStaticMesh가 없으면 모든 Cell 메시에서 바운드 계산
		for (const UDynamicMeshComponent* CellComp : CellMeshComponents)
		{
			if (CellComp)
			{
				MeshBounds += CellComp->Bounds.GetBox();
			}
		}
	}

	if (!MeshBounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellGraph: Failed to compute mesh bounds"));
		return false;
	}

	// 3. GridToChunkMap 검증 (BuildGridToChunkMap에서 미리 계산됨)
	if (GridToChunkMap.Num() != ExpectedChunkCount)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellGraph: GridToChunkMap not initialized. Expected %d, got %d. Call BuildGridToChunkMap first."),
			ExpectedChunkCount, GridToChunkMap.Num());
		return false;
	}

	// 4. 분할 평면 생성
	CellGraph.Reset();
	CellGraph.BuildDivisionPlanesFromGrid(MeshBounds, SliceCount, GridToChunkMap);

	// 5. 청크 메시 포인터 배열 생성
	TArray<FDynamicMesh3*> ChunkMeshes;
	ChunkMeshes.SetNum(CellMeshComponents.Num());
	int32 ValidMeshCount = 0;
	for (int32 i = 0; i < CellMeshComponents.Num(); ++i)
	{
		if (CellMeshComponents[i])
		{
			ChunkMeshes[i] = CellMeshComponents[i]->GetMesh();
			if (ChunkMeshes[i] && ChunkMeshes[i]->TriangleCount() > 0)
			{
				++ValidMeshCount;
			}
		}
		else
		{
			ChunkMeshes[i] = nullptr;
		}
	}

	// 6. 그래프 구축
	const float PlaneTolerance = 0.1f;
	const float RectTolerance = 0.1f;
	CellGraph.BuildGraph(ChunkMeshes, PlaneTolerance, RectTolerance, FloorHeightThreshold);

	// Grid -> Chunk -> Cell 매핑 통합 로그
	UE_LOG(LogTemp, Log, TEXT("BuildCellGraph: %d grid cells, %d valid meshes"), GridToChunkMap.Num(), ValidMeshCount);
	for (int32 GridIdx = 0; GridIdx < GridToChunkMap.Num(); ++GridIdx)
	{
		const int32 ChunkId = GridToChunkMap[GridIdx];
		if (ChunkId == INDEX_NONE || !ChunkMeshes.IsValidIndex(ChunkId) || ChunkMeshes[ChunkId] == nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("  Grid %d -> Chunk %d (invalid)"), GridIdx, ChunkId);
			continue;
		}

		const int32 TriCount = ChunkMeshes[ChunkId]->TriangleCount();
		const FChunkCellCache* CellCache = CellGraph.GetChunkCellCache(ChunkId);
		const int32 CellCount = CellCache ? CellCache->CellIds.Num() : 0;

		UE_LOG(LogTemp, Log, TEXT("  Grid %d -> Chunk %d (%d tris, %d cells)"),
			GridIdx, ChunkId, TriCount, CellCount);
	}

	// 7. IntegritySystem 초기화 (신규 SyncGraph API 사용)
	FStructuralIntegrityGraphSnapshot Snapshot = CellGraph.BuildGraphSnapshot();
	IntegritySystem.Reset();
	IntegritySystem.SyncGraph(Snapshot);

	UE_LOG(LogTemp, Log, TEXT("BuildCellGraph: Built graph with %d nodes, %d anchors, snapshot has %d keys"),
		CellGraph.GetNodeCount(), IntegritySystem.GetAnchorCount(), Snapshot.GetNodeCount());

	return CellGraph.IsGraphBuilt();
}

void URealtimeDestructibleMeshComponent::UpdateCellGraphForModifiedChunks()
{
	// CellGraph가 빌드되지 않았거나 수정된 청크가 없으면 스킵
	if (!CellGraph.IsGraphBuilt() || ModifiedChunkIds.Num() == 0)
	{
		ModifiedChunkIds.Reset();
		return;
	}

	// Cell 메쉬가 없으면 스킵
	if (CellMeshComponents.Num() == 0)
	{
		ModifiedChunkIds.Reset();
		return;
	}

	// 1. ChunkMeshes 배열 구성 (FDynamicMesh3* 배열)
	TArray<FDynamicMesh3*> ChunkMeshes;
	ChunkMeshes.SetNum(CellMeshComponents.Num());

	for (int32 i = 0; i < CellMeshComponents.Num(); ++i)
	{
		if (CellMeshComponents[i] != nullptr)
		{
			UDynamicMesh* DynMesh = CellMeshComponents[i]->GetDynamicMesh();
			if (DynMesh)
			{
				ChunkMeshes[i] = &DynMesh->GetMeshRef();
			}
			else
			{
				ChunkMeshes[i] = nullptr;
			}
		}
		else
		{
			ChunkMeshes[i] = nullptr;
		}
	}

	// 2. 수정된 청크들의 Cell 재계산 및 매핑 생성
	TArray<FChunkUpdateResult> UpdateResults = CellGraph.UpdateModifiedChunks(
		ModifiedChunkIds, ChunkMeshes);

	// 3. 영향받은 Division Plane의 연결 재검사
	CellGraph.RebuildConnectionsForChunks(UpdateResults, ChunkMeshes);

	// 4. IntegritySystem에 그래프 동기화
	FStructuralIntegrityGraphSnapshot Snapshot = CellGraph.BuildGraphSnapshot();
	IntegritySystem.SyncGraph(Snapshot);

	// 5. 연결성 재계산 및 Detached 탐지
	FStructuralIntegrityResult Result = IntegritySystem.RefreshConnectivity();

	// 6. Detached 그룹 처리 (디버그 시각화)
	if (Result.DetachedGroups.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UpdateCellGraphForModifiedChunks: Detected %d detached groups"),
			Result.DetachedGroups.Num());

		// 디버그: Detached 셀 시각화
		if (bShowCellGraphDebug)
		{
			DrawDetachedGroupsDebug(Result.DetachedGroups);
		}

		// 스폰된 것으로 처리 (나중에 실제 물리 파편 스폰으로 교체)
		TArray<FCellKey> SpawnedKeys;
		for (const FDetachedCellGroup& Group : Result.DetachedGroups)
		{
			SpawnedKeys.Append(Group.CellKeys);
		}
		IntegritySystem.MarkCellsAsDestroyed(SpawnedKeys);
	}

	// 7. ModifiedChunkIds 초기화
	ModifiedChunkIds.Reset();
	bShouldDebugUpdate = true;
}

void URealtimeDestructibleMeshComponent::FindChunksAlongLineInternal(const FVector& WorldStart, const FVector& WorldEnd, TArray<int32>& OutChunkIndices)
{
	if (GridToChunkMap.Num() == 0 || SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		return;
	}

	const FVector& ChunkSize = CachedCellSize;
	const FBox& MeshBounds = CachedMeshBounds;

	// World to Local
	FVector LocalStart = GetComponentTransform().InverseTransformPosition(WorldStart);
	FVector LocalEnd = GetComponentTransform().InverseTransformPosition(WorldEnd);

	/*
	 * Slab method로 라인이 메시 내부에 있는 지 검사할 필요가 없음
	 * 라인의 시작점은 항상 메시 표면/내부이고 끝점의 경우 clamp로 처리
	 */

	// 그리드 공간으로 변환
	auto ToGridSpace = [&](const FVector& Position) -> FVector
	{
		return FVector(
			(Position.X - MeshBounds.Min.X) / ChunkSize.X,
			(Position.Y - MeshBounds.Min.Y) / ChunkSize.Y,
			(Position.Z - MeshBounds.Min.Z) / ChunkSize.Z
			);
	};
	FVector GridStart = ToGridSpace(LocalStart);
	FVector GridEnd = ToGridSpace(LocalEnd);

	// 인덱스 변환 및 클램핑
	// End를 박스 내부로 제한
	int32 CurrentX = FMath::Clamp(FMath::FloorToInt(GridStart.X), 0, SliceCount.X - 1);
	int32 CurrentY = FMath::Clamp(FMath::FloorToInt(GridStart.Y), 0, SliceCount.Y - 1);
	int32 CurrentZ = FMath::Clamp(FMath::FloorToInt(GridStart.Z), 0, SliceCount.Z - 1);

	int32 EndX = FMath::Clamp(FMath::FloorToInt(GridEnd.X), 0, SliceCount.X - 1);
	int32 EndY = FMath::Clamp(FMath::FloorToInt(GridEnd.Y), 0, SliceCount.Y - 1);
	int32 EndZ = FMath::Clamp(FMath::FloorToInt(GridEnd.Z), 0, SliceCount.Z - 1);

	// DDA 초기화(amanatides & woo의 fast voxel traversal algorithm)
	int32 StepX = (GridEnd.X >= GridStart.X) ? 1 : -1;
	int32 StepY = (GridEnd.Y >= GridStart.Y) ? 1 : -1;
	int32 StepZ = (GridEnd.Z >= GridStart.Z) ? 1 : -1;

	// tDelta
	FVector Direction = GridEnd - GridStart;
	float tDeltaX = (FMath::Abs(Direction.X) > KINDA_SMALL_NUMBER) ? (1.0f / FMath::Abs(Direction.X)) : FLT_MAX;
	float tDeltaY = (FMath::Abs(Direction.Y) > KINDA_SMALL_NUMBER) ? (1.0f / FMath::Abs(Direction.Y)) : FLT_MAX;
	float tDeltaZ = (FMath::Abs(Direction.Z) > KINDA_SMALL_NUMBER) ? (1.0f / FMath::Abs(Direction.Z)) : FLT_MAX;

	// tMax
	float FracX = GridStart.X - FMath::FloorToFloat(GridStart.X);
	float FracY = GridStart.Y - FMath::FloorToFloat(GridStart.Y);
	float FracZ = GridStart.Z - FMath::FloorToFloat(GridStart.Z);

	float tMaxX = (StepX > 0) ? (1.0f - FracX) * tDeltaX : FracX * tDeltaX;
	float tMaxY = (StepY > 0) ? (1.0f - FracY) * tDeltaY : FracY * tDeltaY;
	float tMaxZ = (StepZ > 0) ? (1.0f - FracZ) * tDeltaZ : FracZ * tDeltaZ;

	int32 MaxIteration = SliceCount.X + SliceCount.Y + SliceCount.Z;

	// 그리드 순회
	for (int32 i = 0; i < MaxIteration; i++)
	{
		if (CurrentX >= 0 && CurrentX < SliceCount.X &&
			CurrentY >= 0 && CurrentY < SliceCount.Y &&
			CurrentZ >= 0 && CurrentZ < SliceCount.Z)
		{
			int32 GridIndex = CurrentX + CurrentY * SliceCount.X + CurrentZ * SliceCount.X * SliceCount.Y;
			if (GridToChunkMap.IsValidIndex(GridIndex))
			{
				int32 ChunkIndex = GridToChunkMap[GridIndex];
				if (ChunkIndex != INDEX_NONE)
				{
					OutChunkIndices.Add(ChunkIndex);
				}
			}
		}

		// 목표지점 도달 시 종료
		if (CurrentX == EndX && CurrentY == EndY && CurrentZ == EndZ)
		{
			break;
		}

		// tMax가 가장 작은 축으로 한 칸 이동(먼저 부딪히는 벽쪽으로 이동)
		if (tMaxX < tMaxY)
		{
			if (tMaxX < tMaxZ)
			{
				CurrentX += StepX;
				tMaxX += tDeltaX;
			}
			else
			{
				CurrentZ += StepZ;
				tMaxZ += tDeltaZ;
			}
		}
		else
		{
			if (tMaxY < tMaxZ)
			{
				CurrentY += StepY;
				tMaxY += tDeltaY;
			}
			else
			{
				CurrentZ += StepZ;
				tMaxZ += tDeltaZ;
			}
		}
	}
}

void URealtimeDestructibleMeshComponent::DrawDetachedGroupsDebug(const TArray<FDetachedCellGroup>& Groups)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& ComponentTransform = GetComponentTransform();

	// 각 그룹을 다른 색상으로 표시
	const TArray<FColor> GroupColors = {
		FColor::Red, FColor::Orange, FColor::Magenta,
		FColor::Purple, FColor::Turquoise, FColor::Emerald
	};

	for (int32 GroupIdx = 0; GroupIdx < Groups.Num(); ++GroupIdx)
	{
		const FDetachedCellGroup& Group = Groups[GroupIdx];
		const FColor GroupColor = GroupColors[GroupIdx % GroupColors.Num()];

		UE_LOG(LogTemp, Warning, TEXT("Detached Group %d: %d cells, %d keys"),
			Group.GroupId, Group.CellIds.Num(), Group.CellKeys.Num());

		for (const FCellKey& Key : Group.CellKeys)
		{
			// Cell의 바운딩 박스 가져오기
			const FChunkCellCache* Cache = CellGraph.GetChunkCellCache(Key.ChunkId);
			if (Cache && Key.CellId >= 0 && Key.CellId < Cache->CellBounds.Num())
			{
				const FBox& CellBoundingBox = Cache->CellBounds[Key.CellId];
				const FVector LocalCenter = CellBoundingBox.GetCenter();
				const FVector LocalExtent = CellBoundingBox.GetExtent();

				// 월드 좌표로 변환
				const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);

				// 회전도 적용하려면 OrientedBox를 그려야 하지만, 간단히 AABB로 표시
				DrawDebugBox(World, WorldCenter, LocalExtent * ComponentTransform.GetScale3D(),
					ComponentTransform.GetRotation(), GroupColor, false, 5.0f, SDPG_Foreground, 3.0f);

				// 그룹 ID 텍스트 표시
				DrawDebugString(World, WorldCenter,
					FString::Printf(TEXT("G%d"), Group.GroupId),
					nullptr, GroupColor, 5.0f, false, 1.5f);
			}
		}
	}
}

FBox URealtimeDestructibleMeshComponent::CalculateCellBounds(int32 CellId) const
{
	FBox ResultBounds(ForceInit);

	if (!CellMeshComponents.IsValidIndex(CellId) || !CellMeshComponents[CellId])
	{
		return ResultBounds;
	}

	// CellMesh의 모든 Vertex를 순회하여 Bounds 계산
	const UE::Geometry::FDynamicMesh3* Mesh = CellMeshComponents[CellId]->GetMesh();
	if (!Mesh)
	{
		return ResultBounds;
	}

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

	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, SourceStaticMesh))
	{
		UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty Mesh Name: %s"), *SourceStaticMesh.GetName());

		// 기존 CellMeshComponents 정리
		for (UDynamicMeshComponent* Comp : CellMeshComponents)
		{
			if (Comp)
			{
				Comp->DestroyComponent();
			}
		}
		CellMeshComponents.Empty();
		CellBounds.Empty();
		GridToChunkMap.Reset();
		bCellMeshesValid = false;

		// 새 메시로 초기화
		bIsInitialized = false;  // 강제 재초기화
		if (SourceStaticMesh)
		{
			InitializeFromStaticMeshInternal(SourceStaticMesh, true);  // bForce = true
		}

		UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty: SourceStaticMesh changed, reinitialized"));
	}

	// bShowCellGraphDebug가 변경되면 처리
	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, bShowCellGraphDebug))
	{
		if (bShowCellGraphDebug)
		{
			// 디버그 켜기: 그래프 그리기
			DrawCellGraphDebug();
		}
		else
		{
			// 디버그 끄기: 기존 persistent 라인 제거
			if (UWorld* World = GetWorld())
			{
				FlushPersistentDebugLines(World);
			}
		}
	}
}


void URealtimeDestructibleMeshComponent::AutoFractureAndAssign()
{
	// 0. 스태틱 메시 유효성 검사
	UStaticMesh* InStaticMesh = SourceStaticMesh.Get();
	if (!InStaticMesh)
	{
		return;
	}

	// 1. UGC 임시객체 생성하고 스태틱 메시를 옮길 FGC 얻어오기
	// Transient는 디스크에 저장하지않고, 메모리에만 존재하는 데이터를 담는데 유용 
	
	// 에셋 이름, 패키징 경로 설정 
	FString ActorLabel = GetOwner() ? GetOwner()->GetActorLabel() : TEXT("Unknown");

	ActorLabel = ActorLabel.Replace(TEXT(" "), TEXT("_"));
	ActorLabel = ActorLabel.Replace(TEXT("."), TEXT("_"));
	ActorLabel = ActorLabel.Replace(TEXT(","), TEXT("_"));

	FString AssetName = FString::Printf(TEXT("GC_%s"), *ActorLabel);
	FString PackagePath = TEXT("/Game/GeneratedGeometryCollections/");
	FString FullPath = PackagePath + AssetName;

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		return;
	}
	Package->FullyLoad();
	
	UGeometryCollection* GeometryCollection = NewObject<UGeometryCollection>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone
	); 
	if (!GeometryCollection)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed To Create Geometry Collection!!"));
		return;
	}
	
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GCPtr = GeometryCollection->GetGeometryCollection();
	if (!GCPtr.IsValid())
	{
		GCPtr = MakeShared<FGeometryCollection>();
		GeometryCollection->SetGeometryCollection(GCPtr);
	}
	 

	// 2. Source Static Mesh를 GC에 추가 (단일 조각으로 시작)
	TArray<UMaterialInterface*> Materials;
	for (const FStaticMaterial& StaticMat : InStaticMesh->GetStaticMaterials())
	{
		Materials.Add(StaticMat.MaterialInterface);
	}
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

	// 3. SliceCutter로 GC 격자 형태로 자르기
	// GC에 있는 조각(TransformGroup)을 전부 선택 상태로 만들기
	FDataflowTransformSelection TransformSelection;
	TransformSelection.InitializeFromCollection(*GCPtr, true);
	// 슬라이싱 영역 지정을 위해 원본 메시의 바운딩 볼륨 얻어오기
	FBox BoundingBox = InStaticMesh->GetBoundingBox();
	// Slice Cutter 실행
	// 주의: 깔끔한 육면체 절단을 위해 노이즈 관련 값은 0으로 둡니다.
	int32 NumCreated = FFractureEngineFracturing::SliceCutter(
		*GCPtr.Get(),           // 레퍼런스로 전달 (&InOutCollection)
		TransformSelection,     // 선택 영역
		BoundingBox,            // 자를 범위
		SliceCount.X - 1,           // X축 몇번 자를 지
		SliceCount.Y - 1,           // Y축 몇번 자를 지
		SliceCount.Z - 1,           // Z축 몇번 자를 지
		0.0f,					// 0 이면 수직, 수평
		0.0f,                   // 0 이면 정간격
		0,                      // Deterministic해야 하므로 Random seed는 상수 0 고정
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
	// 후처리로 데이터 무결성 갱신
	// 이 코드가 없으면 "Name not mapped" 또는 "Serialize not deterministic" 에러 발생
	// =========================================================================

	// Collection 무효화 및 재구성 (중요!)
	// GC 무효화 및 알림. 이전에 기록된 모든 캐시가 이 컬렉션을 사용할 수 없게 만듦.
	GeometryCollection->Materials = Materials;
	GeometryCollection->InvalidateCollection();

	GCPtr = GeometryCollection->GetGeometryCollection();
	//	바운딩 박스 재계산
	GCPtr->UpdateBoundingBox();

	// 에디터 변경 알림 (직렬화 준비)
	GeometryCollection->PostEditChange();
	GCPtr = GeometryCollection->GetGeometryCollection();

	// 에셋을 디스크에도 저장을 따로하자 
	FAssetRegistryModule::AssetCreated(GeometryCollection);

	// 저장
	GeometryCollection->MarkPackageDirty();
	Package->MarkPackageDirty();

	// 실제 파일 경로 계산
	FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension()  // .uasset
	);

	// 디렉토리가 없으면 생성
	FString DirectoryPath = FPaths::GetPath(PackageFileName);
	if (!IFileManager::Get().DirectoryExists(*DirectoryPath))
	{
		IFileManager::Get().MakeDirectory(*DirectoryPath, true);
	}

	// 패키지 저장
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GError;
	SaveArgs.bForceByteSwapping = false;
	SaveArgs.bWarnOfLongFilename = true;

	bool SaveResult = UPackage::SavePackage(
		Package,
		GeometryCollection,
		*PackageFileName,
		SaveArgs
	);

	if (!SaveResult)
	{
		UE_LOG(LogTemp, Log, TEXT("Failed to save GeometryCollectio: %s"), *PackageFileName);
	}


	// 컴포넌트에 자동 할당
	FracturedGeometryCollection = GeometryCollection;


	// Cell 메시 빌드
	int32 CellCount = BuildCellMeshesFromGeometryCollection();

	if (AActor* Owner = GetOwner())
	{
		Owner->Modify();
		Owner->RerunConstructionScripts();
	}

	return;
}
void URealtimeDestructibleMeshComponent::RevertFracture()
{
	// Cell로 나눠진 상태가 아니면  return
	if (!bUseCellMeshes && CellMeshComponents.Num() == 0)
	{
		return;
	}

	// Ctrl Z를 위한 Snap Shot
	if (AActor* Owner = GetOwner())
	{
		Owner->Modify();
		this->Modify();
	}

	// 생성된 Cell Components
	for (UDynamicMeshComponent* Cell : CellMeshComponents)
	{
		if (Cell)
		{
			Cell->DestroyComponent();
		}
	}

	CellMeshComponents.Empty();
	CellBounds.Empty();
	GridToChunkMap.Reset();

	// 원본 메쉬로 데이터 리셋
	bUseCellMeshes = false;
	bCellMeshesValid = false;
	//SetVisibility(true, true);
	SetSourceMeshEnabled(true);

	ResetToSourceMesh();

	// Editor 강제 갱신
	if (AActor* Owner = GetOwner())
	{
		Owner->RerunConstructionScripts();
	}

}
#endif

FRealtimeDestructibleMeshComponentInstanceData::FRealtimeDestructibleMeshComponentInstanceData(
	const URealtimeDestructibleMeshComponent* SourceComponent)
	: FActorComponentInstanceData(SourceComponent)
{
	if (SourceComponent)
	{
		SavedSourceStaticMesh = SourceComponent->SourceStaticMesh;
		bSavedIsInitialized = SourceComponent->bIsInitialized;
		bSavedUseCellMeshes = SourceComponent->bUseCellMeshes;
		bSavedCellMeshesValid = SourceComponent->bCellMeshesValid;

		SavedSliceCount = SourceComponent->SliceCount;
		bSavedShowCellGraphDebug = SourceComponent->bShowCellGraphDebug;

		// 포인터 대신 컴포넌트 이름을 저장 (PIE 복제 시 이름으로 찾기 위함)
		SavedCellComponentNames.Empty();
		SavedCellComponentNames.Reserve(SourceComponent->CellMeshComponents.Num());
		for (const UDynamicMeshComponent* Cell : SourceComponent->CellMeshComponents)
		{
			if (Cell)
			{
				SavedCellComponentNames.Add(Cell->GetName());
			}
			else
			{
				SavedCellComponentNames.Add(FString());  // nullptr은 빈 문자열로
			}
		}

		UE_LOG(LogTemp, Warning, TEXT("InstanceData Constructor: bUseCellMeshes=%d, bCellMeshesValid=%d, CellMeshComponents.Num=%d, SavedNames.Num=%d"),
			bSavedUseCellMeshes, bSavedCellMeshesValid, SourceComponent->CellMeshComponents.Num(), SavedCellComponentNames.Num());
	}
}

void FRealtimeDestructibleMeshComponentInstanceData::ApplyToComponent(
	UActorComponent* Component,
	const ECacheApplyPhase CacheApplyPhase)
{
	UE_LOG(LogTemp, Warning, TEXT("ApplyToComponent: Phase=%d, bSavedUseCellMeshes=%d, bSavedCellMeshesValid=%d, SavedCellNames.Num=%d"),
		(int32)CacheApplyPhase, bSavedUseCellMeshes, bSavedCellMeshesValid, SavedCellComponentNames.Num());

	Super::ApplyToComponent(Component, CacheApplyPhase);

	if (URealtimeDestructibleMeshComponent* DestructComp = Cast<URealtimeDestructibleMeshComponent>(Component))
	{
		// BP 기본값 대신 저장된 인스턴스 값으로 복원
		DestructComp->SourceStaticMesh = SavedSourceStaticMesh;
		DestructComp->SliceCount = SavedSliceCount;
		DestructComp->bShowCellGraphDebug = bSavedShowCellGraphDebug;

		// Cell 모드 상태 복원
		DestructComp->bUseCellMeshes = bSavedUseCellMeshes;
		DestructComp->bCellMeshesValid = bSavedCellMeshesValid;

		// PIE에서는 포인터가 유효하지 않으므로 이름으로 복제된 컴포넌트를 찾음
		if (AActor* Owner = DestructComp->GetOwner())
		{
			DestructComp->CellMeshComponents.Empty();
			DestructComp->CellMeshComponents.SetNum(SavedCellComponentNames.Num());

			TArray<UDynamicMeshComponent*> FoundCells;
			Owner->GetComponents<UDynamicMeshComponent>(FoundCells);

			UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: Found %d DynamicMeshComponents in owner"), FoundCells.Num());

			for (int32 i = 0; i < SavedCellComponentNames.Num(); ++i)
			{
				if (SavedCellComponentNames[i].IsEmpty())
				{
					// 인덱스 0은 루트(nullptr)
					DestructComp->CellMeshComponents[i] = nullptr;
					continue;
				}

				// 이름으로 복제된 컴포넌트 찾기
				UDynamicMeshComponent* FoundCell = nullptr;
				for (UDynamicMeshComponent* Cell : FoundCells)
				{
					if (Cell && Cell->GetName() == SavedCellComponentNames[i])
					{
						FoundCell = Cell;
						break;
					}
				}

				if (FoundCell)
				{
					DestructComp->CellMeshComponents[i] = FoundCell;
					// 부모 연결 확인
					if (FoundCell->GetAttachParent() != DestructComp)
					{
						FoundCell->AttachToComponent(DestructComp, FAttachmentTransformRules::KeepRelativeTransform);
					}
					UE_LOG(LogTemp, Verbose, TEXT("ApplyToComponent: Found Cell_%d by name: %s"), i, *SavedCellComponentNames[i]);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("ApplyToComponent: Could not find Cell by name: %s"), *SavedCellComponentNames[i]);
					DestructComp->CellMeshComponents[i] = nullptr;
				}
			}

			UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: Rebuilt CellMeshComponents with %d entries"), DestructComp->CellMeshComponents.Num());
		}

		// Cell 모드가 활성화 되어 있고, 유효하면 CellGraph만 재구축
		if (bSavedUseCellMeshes && bSavedCellMeshesValid)
		{
			// CellGraph, IntegritySystem, GridToChunkMap은 저장되지 않으므로 재구축
			DestructComp->BuildGridToChunkMap();
			DestructComp->BuildCellGraph();
			return;
		}


		// Cell 모드가 아닐 때, 메시 재초기화
		if (SavedSourceStaticMesh)
		{
			DestructComp->bIsInitialized = false;  // 강제 재초기화
			DestructComp->InitializeFromStaticMesh(SavedSourceStaticMesh);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Server Validation (서버 검증)
//////////////////////////////////////////////////////////////////////////

bool URealtimeDestructibleMeshComponent::ValidateDestructionRequest(
	const FRealtimeDestructionRequest& Request,
	APlayerController* RequestingPlayer,
	EDestructionRejectReason& OutReason)
{
	OutReason = EDestructionRejectReason::None;

	// 플레이어가 없으면 검증 스킵 (서버 직접 호출 등)
	if (!RequestingPlayer)
	{
		return true;
	}

	// 1. 최대 구멍 수 체크
	if (MaxHoleCount > 0 && CurrentHoleCount >= MaxHoleCount)
	{
		OutReason = EDestructionRejectReason::MaxHoleReached;
		return false;
	}

	// 2. 사거리 체크
	if (APawn* Pawn = RequestingPlayer->GetPawn())
	{
		const float Distance = FVector::Dist(Pawn->GetActorLocation(), Request.ImpactPoint);
		if (Distance > MaxDestructionRange)
		{
			OutReason = EDestructionRejectReason::OutOfRange;
			return false;
		}
	}

	// 3. 시야 체크 (LineTrace)
	if (bEnableLineOfSightCheck)
	{
		if (APawn* Pawn = RequestingPlayer->GetPawn())
		{
			FHitResult HitResult;
			FCollisionQueryParams QueryParams;
			QueryParams.AddIgnoredActor(Pawn);

			const FVector Start = Pawn->GetActorLocation();
			const FVector End = Request.ImpactPoint;

			if (GetWorld()->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, QueryParams))
			{
				// 히트한 컴포넌트가 이 컴포넌트가 아니면 시야 차단
				if (HitResult.GetComponent() != this && HitResult.GetComponent() != nullptr)
				{
					// Cell 메시 중 하나인지 확인
					bool bHitOurCell = false;
					for (const auto& CellComp : CellMeshComponents)
					{
						if (HitResult.GetComponent() == CellComp)
						{
							bHitOurCell = true;
							break;
						}
					}

					if (!bHitOurCell)
					{
						OutReason = EDestructionRejectReason::LineOfSightBlocked;
						return false;
					}
				}
			}
		}
	}

	// 4. 연사 제한 (TODO: 플레이어별 추적 필요)
	// 현재는 단순 구현 - 추후 TMap<APlayerController*, FRateLimitInfo> 사용

	// 5. 유효한 위치 체크 (메시 내부인지)
	// TODO: 필요시 구현

	return true;
}

void URealtimeDestructibleMeshComponent::ClientDestructionRejected_Implementation(uint16 Sequence, EDestructionRejectReason Reason)
{
	// 클라이언트에서 거부 처리
	UE_LOG(LogTemp, Warning, TEXT("[Destruction] Request rejected - Seq: %d, Reason: %s"),
		Sequence, *UEnum::GetValueAsString(Reason));

	// 블루프린트/C++ 이벤트 브로드캐스트
	OnDestructionRejected.Broadcast(static_cast<int32>(Sequence), Reason);
}

TStructOnScope<FActorComponentInstanceData> URealtimeDestructibleMeshComponent::GetComponentInstanceData() const
{
	UE_LOG(LogTemp, Warning, TEXT("GetComponentInstanceData"));

	return MakeStructOnScope<FActorComponentInstanceData, FRealtimeDestructibleMeshComponentInstanceData>(this);
}	