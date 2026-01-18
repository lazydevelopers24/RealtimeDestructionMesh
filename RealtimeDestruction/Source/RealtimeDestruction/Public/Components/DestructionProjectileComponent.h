// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DestructionTypes.h"
#include "Components/SceneComponent.h"

#include "DestructionProjectileComponent.generated.h"
class UMaterialInterface;
class UNiagaraSystem;

//=============================================================================
// Delegates
//=============================================================================

// 파괴 요청 전송 시 호출
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDestructionRequested, const FVector&, ImpactPoint, const FVector&, ImpactNormal);

// 파괴 대상이 아닌 것에 충돌 시 호출
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNonDestructibleHit, const FHitResult&, HitResult);

/**
 * 투사체용 파괴 컴포넌트
 *
 * 투사체 Actor에 이 컴포넌트를 붙이면:
 * 1. 충돌 시 자동으로 RealtimeDestructibleMeshComponent를 찾아서 파괴 요청
 * 2. 즉시 피드백 (데칼, 파티클) 표시
 * 3. 투사체 자동 제거
 *
 * [네트워크 요구사항]
 * 멀티플레이어 게임에서 이 컴포넌트는 서버에서 스폰된 투사체에서만
 * 파괴를 처리합니다. 클라이언트 전용 투사체(이펙트용)에서는
 * 파괴가 동작하지 않습니다.
 *
 * 권장 패턴 (클라이언트 예측 + 서버 권위):
 * 1. 클라이언트: 로컬 투사체 스폰 (이펙트/피드백용, 선택사항)
 * 2. 클라이언트 -> Server RPC -> 서버: 투사체 스폰 (실제 판정용)
 * 3. 서버 투사체 충돌 시 MulticastApplyOps로 모든 클라이언트에 파괴 전파
 *
 * 사용법:
 * 1. 투사체 블루프린트에 이 컴포넌트 추가
 * 2. HoleRadius, 데칼, 파티클 설정
 * 3. 멀티플레이어: 투사체를 서버에서 스폰하도록 구현
 */
UCLASS(ClassGroup=(Destruction), meta=(BlueprintSpawnableComponent))
class REALTIMEDESTRUCTION_API UDestructionProjectileComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UDestructionProjectileComponent();

#if	WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override; 
#endif
	 
	//=========================================================================
	// 파괴 설정
	//=========================================================================

	/**
	 * 자동으로 OnHit 이벤트 바인딩할지 여부
	 *
	 * true: 컴포넌트가 자동으로 충돌 감지 (단독 사용 시)
	 * false: 외부에서 RequestDestructionManual() 호출 필요 (AShooterProjectile 등과 함께 사용 시)
	 *
	 * 블루프린트에서 AShooterProjectile에 붙일 때는 false로 설정하세요!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	bool bAutoBindHit = true;

	/** 구멍 반지름 (cm) - 호환성용 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	float HoleRadius = 10.0f;

	// Tool Shape 변경을 위한 변수
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape")
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Debug")
	bool bShowToolShape = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Debug")
	bool bShowAffetedChunks = false;

	//=========================================================================
	//  Cylinder / Cone  전용 파라매터
	//=========================================================================  
	/** Cylinder Radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Cylinder || ToolShape == EDestructionToolShape::Cone", EditConditionHides))
	float CylinderRadius = 10.0f;

	/** Cylinder 높이 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Cylinder || ToolShape == EDestructionToolShape::Cone", EditConditionHides))
	float CylinderHeight = 400.0f;

	/** 원형 단면의 분할 수 */	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (ClampMin = 3, ClampMax = 64,
			EditCondition = "ToolShape == EDestructionToolShape::Cylinder || ToolShape == EDestructionToolShape::Cone",
			EditConditionHides))
	int32 RadialSteps = 12;

	/** 높이 방향 분할 수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (ClampMin = 0, ClampMax = 32,
			EditCondition = "ToolShape == EDestructionToolShape::Cylinder || ToolShape == EDestructionToolShape::Cone",
			EditConditionHides))
	int32 HeightSubdivisions = 0;

	/** 닫힌 원통인지 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Cylinder || ToolShape == EDestructionToolShape::Cone",
			EditConditionHides)) 
	bool bCapped = true;

	//=========================================================================
	//  Sphere 전용 파라매터
	//=========================================================================  

	/** Sphere Radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Sphere",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Sphere", EditConditionHides))
	float SphereRadius = 10.0f;

	/** 구의 위도 분할 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Sphere",
		meta = (ClampMin = 3, ClampMax = 128,
			EditCondition = "ToolShape == EDestructionToolShape::Sphere",
			EditConditionHides))
	int32 SphereStepsPhi = 8;

	/** 구의 경도 분할 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Sphere",
		meta = (ClampMin = 3, ClampMax = 128,
			EditCondition = "ToolShape == EDestructionToolShape::Sphere",
			EditConditionHides))
	int32 SphereStepsTheta = 16;

	//=========================================================================
	// Box 전용 파라미터
	//=========================================================================
	  /** Box 크기 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Box",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Box",
			EditConditionHides))
	FVector BoxSize = FVector(20.0f, 20.0f, 20.0f);


	/** 충돌 후 투사체 자동 제거 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	bool bDestroyOnHit = true;

	/** 파괴 불가능한 오브젝트에 충돌해도 제거할지 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	bool bDestroyOnNonDestructibleHit = true;

	//=========================================================================
	// Decal 파라미터
	//=========================================================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal")
	bool  bUseDecalSizeOverride = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "bUseDecalSizeOverride", EditConditionHides))
	FVector DecalSizeOverride = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "bUseDecalSizeOverride", EditConditionHides))
	FVector DecalLocationOffset = FVector::ZeroVector;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "bUseDecalSizeOverride", EditConditionHides))
	FRotator DecalRotationOffset = FRotator::ZeroRotator;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "!bUseDecalSizeOverride", EditConditionHides))
	float DecalSizeMultiplier = 1.0f;

	//=========================================================================
	// Decal Editor 값 저장용 
	//=========================================================================

	UPROPERTY()
	FVector DecalLocationInEditor = FVector::ZeroVector;

	UPROPERTY()
	FRotator DecalRotationInEditor = FRotator::ZeroRotator;

	UPROPERTY()
	FVector DecalScaleInEditor = FVector::OneVector;
	
	UPROPERTY()
	FVector ToolShapeLocationInEditor = FVector::ZeroVector;

	UPROPERTY()
	FRotator ToolShapeRotationInEditor = FRotator::ZeroRotator;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> DecalMaterialInEditor = nullptr;

	UPROPERTY()
	FName DecalConfig = FName("Default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	TObjectPtr<UDecalMaterialDataAsset> DecalDataAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FName DecalConfigID = FName("Default");
	
	
	//=========================================================================
	// 즉시 피드백 설정
	//=========================================================================

	/** 즉시 피드백 표시 (서버 응답 전) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Feedback")
	bool bShowImmediateFeedback = true;

	//=========================================================================
	// 이벤트
	//=========================================================================

	/** 파괴 요청 전송 시 */
	UPROPERTY(BlueprintAssignable, Category="Destruction|Events")
	FOnDestructionRequested OnDestructionRequested;

	/** 파괴 대상이 아닌 것에 충돌 시 */
	UPROPERTY(BlueprintAssignable, Category="Destruction|Events")
	FOnNonDestructibleHit OnNonDestructibleHit;

	//=========================================================================
	// 수동 호출용 함수
	//=========================================================================

	/**
	 * 수동으로 파괴 요청 전송
	 * 자동 충돌 감지 대신 직접 호출할 때 사용
	 */
	UFUNCTION(BlueprintCallable, Category="Destruction")
	void RequestDestructionManual(const FHitResult& HitResult);

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void RequestDestructionAtLocation(const FVector& Center);

public:
	UFUNCTION(BlueprintCallable, Category="Destruction|Decal")
	void GetCalculateDecalSize(FName SurfaceType,FVector& LocationOffset,  FRotator& RotatorOffset, FVector& SizeOffset) const;

protected:
	virtual void BeginPlay() override;

	//=========================================================================
	// 내부 함수
	//=========================================================================

	/** 충돌 이벤트 핸들러 */
	UFUNCTION()
	void OnProjectileHit(UPrimitiveComponent* HitComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

	void ProcessDestructionRequestForChunk(URealtimeDestructibleMeshComponent* DestructComp, const FHitResult& Hit);
	void ProcessSphereDestructionRequestForChunk(URealtimeDestructibleMeshComponent* DestructComp, const FVector& ExplosionCenter );

private:
	bool EnsureToolMesh();

	void SetShapeParameters(FRealtimeDestructionRequest& OutRequest);

	void DrawDebugToolShape(const FVector& Center, const FVector& Direction, const FColor& Color) const;

	void DrawDebugAffetedChunks(const FBox& ChunkBox, const FColor& Color) const;

	void DrawDebugCylinderInternal(const FVector& Center, const FVector& Direction, const FColor& Color) const;

	void DrawDebugSphereInternal(const FVector& Center, const FColor& Color) const;

	FVector GetToolDirection(const FHitResult& Hit, AActor* Owner) const;
	
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> ToolMeshPtr = nullptr;


	float SurfaceMargin = 0.0f;
};
