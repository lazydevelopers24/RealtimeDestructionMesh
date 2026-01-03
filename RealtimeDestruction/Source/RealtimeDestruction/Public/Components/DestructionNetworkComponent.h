// DestructionNetworkComponent.h
// PlayerController에 추가하여 파괴 요청을 서버로 전달하는 컴포넌트
//
// [사용법]
// 1. PlayerController 블루프린트에서 이 컴포넌트 추가
// 2. 끝! 자동으로 Server RPC 처리됨

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RealtimeDestructibleMeshComponent.h"
#include "DestructionNetworkComponent.generated.h"

/**
 * 파괴 요청을 서버로 전달하는 네트워크 컴포넌트
 *
 * PlayerController에 이 컴포넌트를 추가하면
 * DestructionProjectileComponent가 자동으로 찾아서 사용합니다.
 *
 * 사용 예시:
 * 1. BP_PlayerController 열기
 * 2. Add Component → DestructionNetworkComponent 추가
 * 3. 완료!
 */
UCLASS(ClassGroup=(Destruction), meta=(BlueprintSpawnableComponent, DisplayName="Destruction Network"))
class REALTIMEDESTRUCTION_API UDestructionNetworkComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDestructionNetworkComponent();

	/**
	 * 파괴 요청을 서버로 전달
	 * DestructionProjectileComponent에서 자동으로 호출됨
	 *
	 * @param DestructComp - 파괴할 메시 컴포넌트
	 * @param Request - 파괴 요청 정보 (위치, 노말, 반지름)
	 */
	UFUNCTION(BlueprintCallable, Category="Destruction")
	void RequestDestruction(URealtimeDestructibleMeshComponent* DestructComp, const FRealtimeDestructionRequest& Request);

protected:
	virtual void BeginPlay() override;

	/**
	 * 서버에서 파괴 처리 (Server RPC) - 기존 방식
	 * 클라이언트에서 호출하면 서버에서 실행됨
	 */
	UFUNCTION(Server, Reliable)
	void ServerApplyDestruction(URealtimeDestructibleMeshComponent* DestructComp, const FRealtimeDestructionRequest& Request);

	/**
	 * 서버에서 파괴 처리 (Server RPC) - 압축 방식
	 * 네트워크 대역폭 ~65% 절감
	 */
	UFUNCTION(Server, Reliable)
	void ServerApplyDestructionCompact(URealtimeDestructibleMeshComponent* DestructComp, const FCompactDestructionOp& CompactOp);

	/** 파괴 요청 검증 (서버에서 호출) */
	bool ValidateDestructionRequest(URealtimeDestructibleMeshComponent* DestructComp, const FRealtimeDestructionRequest& Request) const;

protected:
	/** 최대 허용 파괴 반경 (치트 방지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Validation")
	float MaxAllowedRadius = 100.0f;

	/** 요청 검증 활성화 여부 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Validation")
	bool bEnableValidation = true;

	/**
	 * 압축된 네트워크 데이터 사용 여부
	 * true: FCompactDestructionOp 사용 (11 bytes)
	 * false: FRealtimeDestructionRequest 사용 (32+ bytes)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Network")
	bool bUseCompactData = true;

private:
	/** 시퀀스 카운터 (압축 데이터용) */
	int32 LocalSequence = 0;
};
