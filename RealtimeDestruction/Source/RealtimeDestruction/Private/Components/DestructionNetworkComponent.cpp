// DestructionNetworkComponent.cpp

#include "Components/DestructionNetworkComponent.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "NetworkLogMacros.h"
#include "Debug/DestructionDebugger.h"
#include "HAL/PlatformTime.h"

//////////////////////////////////////////////////////////////////////////
// UDestructionNetworkComponent 구현
//////////////////////////////////////////////////////////////////////////

UDestructionNetworkComponent::UDestructionNetworkComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// 네트워크 복제 설정
	SetIsReplicatedByDefault(true);
}

void UDestructionNetworkComponent::BeginPlay()
{
	Super::BeginPlay();

	// PlayerController에 부착되었는지 확인
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DestructionNetworkComponent: 이 컴포넌트는 PlayerController에 추가해야 합니다. 현재 Owner: %s"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("None"));
	}
}

void UDestructionNetworkComponent::RequestDestruction(
	URealtimeDestructibleMeshComponent* DestructComp,
	const FRealtimeDestructionRequest& Request)
{
	if (!DestructComp)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const ENetMode NetMode = World->GetNetMode();

	// 서버인 경우 서버 배칭에 추가
	if (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer)
	{
		// 리슨서버만: 호스트 화면에 파괴 표시
		if (NetMode == NM_ListenServer)
		{
			DestructComp->RequestDestruction(Request);
		}

		FRealtimeDestructionOp Op;
		Op.Request = Request;

		// 서버 배칭 사용 시 대기열에 추가
		if (DestructComp->bUseServerBatching)
		{
			DestructComp->EnqueueForServerBatch(Op);
		}
		else
		{
			// 디버거에 Multicast RPC 기록 (데이터 크기 포함)
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				Debugger->RecordMulticastRPCWithSize(1, DestructComp->bUseCompactMulticast);
			}

			// 배칭 OFF여도 압축 옵션 체크
			if (DestructComp->bUseCompactMulticast)
			{
				TArray<FCompactDestructionOp> CompactOps;
				CompactOps.Add(FCompactDestructionOp::Compress(Op.Request, 0));
				DestructComp->MulticastApplyOpsCompact(CompactOps);
			}
			else
			{
				TArray<FRealtimeDestructionOp> Ops;
				Ops.Add(Op);
				DestructComp->MulticastApplyOps(Ops);
			}
		}
	}
	// 클라이언트인 경우 서버로 RPC 전송
	else if (NetMode == NM_Client)
	{
		// 클라이언트 송신 데이터 크기 기록
		if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
		{
			Debugger->RecordServerRPCWithSize(bUseCompactData);
		}

		if (bUseCompactData)
		{
			// 압축 방식 (11 bytes)
			FCompactDestructionOp CompactOp = FCompactDestructionOp::Compress(Request, LocalSequence++);
			ServerApplyDestructionCompact(DestructComp, CompactOp);
		}
		else
		{
			// 기존 방식 (32+ bytes)
			FRealtimeDestructionRequest RequestWithTime = Request;
			RequestWithTime.ClientSendTime = FPlatformTime::Seconds();
			ServerApplyDestruction(DestructComp, RequestWithTime);
		}
	}
	// 싱글플레이어인 경우 바로 적용
	else // NM_Standalone
	{
		// NET_LOG_COMPONENT(this, "Standalone - 즉시 파괴 (Radius: %.1f)", Request.HoleRadius);
		DestructComp->RequestDestruction(Request);
	}
}

void UDestructionNetworkComponent::ServerApplyDestruction_Implementation(
	URealtimeDestructibleMeshComponent* DestructComp,
	const FRealtimeDestructionRequest& Request)
{
	// NET_LOG_COMPONENT(this, "Server RPC 수신 - 파괴 요청 처리");

	UWorld* World = GetWorld();

	// 디버거에 Server RPC 기록 (비압축 방식, 데이터 크기 포함)
	if (UDestructionDebugger* Debugger = World ? World->GetSubsystem<UDestructionDebugger>() : nullptr)
	{
		Debugger->RecordServerRPCWithSize(false);

		// 클라이언트 정보 기록
		APlayerController* PC = Cast<APlayerController>(GetOwner());
		if (PC)
		{
			int32 ClientId = PC->GetUniqueID();
			FString PlayerName = PC->PlayerState ? PC->PlayerState->GetPlayerName() : TEXT("Unknown");
			Debugger->RecordClientRequest(ClientId, PlayerName, false);
		}
	}

	if (!DestructComp)
	{
		NET_LOG_COMPONENT_WARNING(this, "DestructComp가 null입니다");
		return;
	}

	// 요청 검증
	if (bEnableValidation && !ValidateDestructionRequest(DestructComp, Request))
	{
		NET_LOG_COMPONENT_WARNING(this, "파괴 요청 검증 실패 - 요청 거부됨");

		// 디버거에 검증 실패 기록
		if (UDestructionDebugger* Debugger = World ? World->GetSubsystem<UDestructionDebugger>() : nullptr)
		{
			APlayerController* PC = Cast<APlayerController>(GetOwner());
			int32 ClientId = PC ? PC->GetUniqueID() : -1;
			Debugger->RecordValidationFailure(ClientId);
		}
		return;
	}

	// 클라이언트에서 온 Request는 ToolMeshPtr가 없음 - ShapeParams로 재생성
	FRealtimeDestructionRequest ModifiedRequest = Request;
	if (!ModifiedRequest.ToolMeshPtr.IsValid())
	{
		ModifiedRequest.ToolMeshPtr = DestructComp->CreateToolMeshPtrFromShapeParams(
			ModifiedRequest.ToolShape,
			ModifiedRequest.ShapeParams
		);
	}

	// 리슨서버만: 호스트 화면에 파괴 표시
	if (World && World->GetNetMode() == NM_ListenServer)
	{
		DestructComp->RequestDestruction(ModifiedRequest);
	}

	// 모든 클라이언트에 파괴 전파
	FRealtimeDestructionOp Op;
	Op.Request = ModifiedRequest;

	// 서버 배칭 사용 시 대기열에 추가
	if (DestructComp->bUseServerBatching)
	{
		DestructComp->EnqueueForServerBatch(Op);
	}
	else
	{
		// 디버거에 Multicast RPC 기록 (데이터 크기 포함)
		if (UDestructionDebugger* Debugger = World ? World->GetSubsystem<UDestructionDebugger>() : nullptr)
		{
			Debugger->RecordMulticastRPCWithSize(1, DestructComp->bUseCompactMulticast);
		}

		// 배칭 OFF여도 압축 옵션 체크
		if (DestructComp->bUseCompactMulticast)
		{
			TArray<FCompactDestructionOp> CompactOps;
			CompactOps.Add(FCompactDestructionOp::Compress(Op.Request, 0));
			DestructComp->MulticastApplyOpsCompact(CompactOps);
		}
		else
		{
			TArray<FRealtimeDestructionOp> Ops;
			Ops.Add(Op);
			DestructComp->MulticastApplyOps(Ops);
		}
	}
}

void UDestructionNetworkComponent::ServerApplyDestructionCompact_Implementation(
	URealtimeDestructibleMeshComponent* DestructComp,
	const FCompactDestructionOp& CompactOp)
{
	UWorld* World = GetWorld();

	// 디버거에 Server RPC 기록 (압축 방식, 데이터 크기 포함)
	if (UDestructionDebugger* Debugger = World ? World->GetSubsystem<UDestructionDebugger>() : nullptr)
	{
		Debugger->RecordServerRPCWithSize(true);

		APlayerController* PC = Cast<APlayerController>(GetOwner());
		if (PC)
		{
			int32 ClientId = PC->GetUniqueID();
			FString PlayerName = PC->PlayerState ? PC->PlayerState->GetPlayerName() : TEXT("Unknown");
			Debugger->RecordClientRequest(ClientId, PlayerName, true);  // true = 압축 방식
		}
	}

	if (!DestructComp)
	{
		NET_LOG_COMPONENT_WARNING(this, "DestructComp가 null입니다 (Compact)");
		return;
	}

	// 압축 해제
	FRealtimeDestructionRequest Request = CompactOp.Decompress();

	// 요청 검증
	if (bEnableValidation && !ValidateDestructionRequest(DestructComp, Request))
	{
		NET_LOG_COMPONENT_WARNING(this, "파괴 요청 검증 실패 (Compact) - 요청 거부됨");
		return;
	}

	// ToolMeshPtr 재생성 (압축 해제 후에도 없음)
	if (!Request.ToolMeshPtr.IsValid())
	{
		Request.ToolMeshPtr = DestructComp->CreateToolMeshPtrFromShapeParams(
			Request.ToolShape,
			Request.ShapeParams
		);
	}

	// 리슨서버만: 호스트 화면에 파괴 표시
	if (World && World->GetNetMode() == NM_ListenServer)
	{
		DestructComp->RequestDestruction(Request);
	}

	// 모든 클라이언트에 파괴 전파
	FRealtimeDestructionOp Op;
	Op.Request = Request;

	// 서버 배칭 사용 시 대기열에 추가
	if (DestructComp->bUseServerBatching)
	{
		DestructComp->EnqueueForServerBatch(Op);
	}
	else
	{
		// 디버거에 Multicast RPC 기록 (데이터 크기 포함)
		if (UDestructionDebugger* Debugger = World ? World->GetSubsystem<UDestructionDebugger>() : nullptr)
		{
			Debugger->RecordMulticastRPCWithSize(1, DestructComp->bUseCompactMulticast);
		}

		// 배칭 OFF여도 압축 옵션 체크
		if (DestructComp->bUseCompactMulticast)
		{
			TArray<FCompactDestructionOp> CompactOps;
			CompactOps.Add(FCompactDestructionOp::Compress(Op.Request, 0));
			DestructComp->MulticastApplyOpsCompact(CompactOps);
		}
		else
		{
			TArray<FRealtimeDestructionOp> Ops;
			Ops.Add(Op);
			DestructComp->MulticastApplyOps(Ops);
		}
	}
}



bool UDestructionNetworkComponent::ValidateDestructionRequest(
	URealtimeDestructibleMeshComponent* DestructComp,
	const FRealtimeDestructionRequest& Request) const
{
	// 반경 검증
	//if (Request.HoleRadius > MaxAllowedRadius)
	//{
	//	UE_LOG(LogTemp, Warning,
	//		TEXT("DestructionNetworkComponent: 요청된 반경(%.1f)이 최대 허용값(%.1f)을 초과"),
	//		Request.HoleRadius, MaxAllowedRadius);
	//	return false;
	//}

	// 추가 검증 로직을 여기에 구현 가능
	// 예: 플레이어와 충돌 지점 거리 검증, 쿨다운 등

	return true;
}

