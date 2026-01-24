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

	/**
	 * 파괴 요청 검증 (서버에서 호출)
	 * RealtimeDestructibleMeshComponent의 ValidateDestructionRequest 호출
	 */
	bool ValidateDestructionRequest(
		URealtimeDestructibleMeshComponent* DestructComp,
		const FRealtimeDestructionRequest& Request,
		EDestructionRejectReason& OutReason) const;

	//////////////////////////////////////////////////////////////////////////
	// Late Join (Op 히스토리 기반 동기화)
	//////////////////////////////////////////////////////////////////////////

	/**
	 * 서버에 Op 히스토리 요청 (Server RPC)
	 * Late Join 시 클라이언트에서 호출
	 */
	UFUNCTION(Server, Reliable)
	void ServerRequestOpHistory(URealtimeDestructibleMeshComponent* DestructComp);

	/**
	 * 클라이언트에 Op 히스토리 전송 (Client RPC)
	 * 서버에서 호출하면 요청한 클라이언트에서 실행됨
	 * Op 개수가 많으면 여러 번 나눠서 전송 (64KB 제한)
	 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveOpHistory(URealtimeDestructibleMeshComponent* DestructComp, const TArray<FCompactDestructionOp>& Ops, bool bIsLastBatch);

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
