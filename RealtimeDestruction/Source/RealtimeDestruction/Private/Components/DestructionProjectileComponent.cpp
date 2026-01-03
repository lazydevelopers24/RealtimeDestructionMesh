// Fill out your copyright notice in the Description page of Project Settings.

#include "Components/DestructionProjectileComponent.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "Components/DestructionNetworkComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/DecalComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "NetworkLogMacros.h"
#include "Debug/DestructionDebugger.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "HAL/PlatformTime.h"
#include "DynamicMesh/DynamicMesh3.h"

#if WITH_EDITOR
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#endif


UDestructionProjectileComponent::UDestructionProjectileComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

#if WITH_EDITOR
void UDestructionProjectileComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName = (PropertyChangedEvent.Property != nullptr)
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;

	// ToolShape가 변경되었을 때
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UDestructionProjectileComponent, ToolShape))
	{
		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.NotifyCustomizationModuleChanged();
		}
	}
} 
#endif

void UDestructionProjectileComponent::BeginPlay()
{
	Super::BeginPlay();

	// bAutoBindHit이 false면 자동 바인딩 안 함
	// (AShooterProjectile 등에서 수동으로 RequestDestructionManual 호출)
	if (!bAutoBindHit)
	{
		return;
	}

	// Owner의 Root 컴포넌트가 PrimitiveComponent인 경우 OnHit 바인딩
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogTemp, Warning, TEXT("DestructionProjectileComponent: Owner is null"));
		return;
	}

	UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	if (RootPrimitive)
	{
		// Hit 이벤트 바인딩
		RootPrimitive->OnComponentHit.AddDynamic(this, &UDestructionProjectileComponent::OnProjectileHit);

		// Hit 이벤트가 발생하려면 Simulation Generates Hit Events가 true여야 함
		if (!RootPrimitive->GetBodyInstance()->bNotifyRigidBodyCollision)
		{
			UE_LOG(LogTemp, Warning, TEXT("DestructionProjectileComponent: 'Simulation Generates Hit Events' is disabled on root component. Enabling it."));
			RootPrimitive->SetNotifyRigidBodyCollision(true);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("DestructionProjectileComponent: Root component is not a PrimitiveComponent. Hit events will not work."));
	}

	if (!ToolMeshPtr.IsValid())
	{
		if (!EnsureToolMesh())
		{
			UE_LOG(LogTemp, Warning, TEXT("DestructionProjectileComponent: Tool mesh is invalid."));
		}		
	}
}

void UDestructionProjectileComponent::OnProjectileHit(
	UPrimitiveComponent* HitComp,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	if (!OtherActor)
	{
		return;
	}

	// 자기 자신과 충돌한 경우 무시
	if (OtherActor == GetOwner())
	{
		return;
	}

	// 맞은 Actor에서 파괴 컴포넌트 찾기
	URealtimeDestructibleMeshComponent* DestructComp =
		OtherActor->FindComponentByClass<URealtimeDestructibleMeshComponent>();

	if (DestructComp)
	{
		// 파괴 가능한 오브젝트에 충돌
		ProcessDestructionRequest(DestructComp, Hit);
	}
	else
	{
		// 파괴 불가능한 오브젝트에 충돌
		OnNonDestructibleHit.Broadcast(Hit);

		if (bDestroyOnNonDestructibleHit && bDestroyOnHit)
		{
			GetOwner()->Destroy();
		}
	}
}

void UDestructionProjectileComponent::ProcessDestructionRequest(
	URealtimeDestructibleMeshComponent* DestructComp,
	const FHitResult& Hit)
{
	if (!DestructComp)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 파괴 요청 생성
	FRealtimeDestructionRequest Request;
	Request.ImpactPoint = Hit.ImpactPoint;
	Request.ImpactNormal = Hit.ImpactNormal;

	if (!ToolMeshPtr.IsValid())
	{
		if (!EnsureToolMesh())
		{
			UE_LOG(LogTemp, Warning, TEXT("DestructionProjectileComponent: Tool mesh is invalid."));
		}	
	}	
	Request.ToolMeshPtr = ToolMeshPtr;
	Request.ToolShape = ToolShape;

	switch (Request.ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		Request.Depth = CylinderHeight;
		break;
	case EDestructionToolShape::Sphere:
		Request.Depth = SphereRadius;
		break;
	default:
		Request.Depth = CylinderHeight;
		break;
	}

	// Shape별로 파라미터 채우기 (네트워크 전송용)
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		Request.ShapeParams.Radius = CylinderRadius;
		Request.ShapeParams.Height = CylinderHeight;
		Request.ShapeParams.RadiusSteps = RadialSteps;
		Request.ShapeParams.HeightSubdivisions = HeightSubdivisions;
		Request.ShapeParams.bCapped = bCapped;
		break;

	case EDestructionToolShape::Sphere:
		Request.ShapeParams.Radius = SphereRadius;
		Request.ShapeParams.StepsPhi = SphereStepsPhi;
		Request.ShapeParams.StepsTheta = SphereStepsTheta;
		break;

	default:
		Request.ShapeParams.Radius = CylinderRadius;
		Request.ShapeParams.Height = CylinderHeight;
		Request.ShapeParams.RadiusSteps = RadialSteps;
		Request.ShapeParams.HeightSubdivisions = HeightSubdivisions;
		Request.ShapeParams.bCapped = bCapped;
		break;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Server] ToolShape: %d, ShapeParams - Radius: %.2f, Height: %.2f, RadiusSteps: %d"),
		static_cast<int32>(Request.ToolShape),
		Request.ShapeParams.Radius,
		Request.ShapeParams.Height,
		Request.ShapeParams.RadiusSteps); 

	// 처리 시간 측정 시작
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// FPS 영향 측정을 위한 파괴 전 FPS 기록
	float FPSBefore = 0.0f;
	if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
	{
		FPSBefore = Debugger->GetCurrentFPS();
	}

	// Instigator(Pawn) → PlayerController → NetworkComp 찾기
	APawn* InstigatorPawn = Owner->GetInstigator();
	APlayerController* PC = InstigatorPawn ? Cast<APlayerController>(InstigatorPawn->GetController()) : nullptr;
	UDestructionNetworkComponent* NetworkComp = PC ? PC->FindComponentByClass<UDestructionNetworkComponent>() : nullptr;

	if (NetworkComp)
	{
		// NetworkComp가 서버/클라이언트/스탠드얼론 모두 처리
		NetworkComp->RequestDestruction(DestructComp, Request);
	}
	else
	{
		// NetworkComp가 없으면 로컬에서 직접 처리 (스탠드얼론 또는 설정 오류)
		DestructComp->RequestDestruction(Request);
	}

	// 처리 시간 측정 종료 (밀리초 단위)
	const float ProcessTimeMs = static_cast<float>((FPlatformTime::Seconds() - StartTime) * 1000.0);

	// FPS 영향 기록 (파괴 후 FPS와 비교)
	if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
	{
		float FPSAfter = Debugger->GetCurrentFPS();
		Debugger->RecordFPSImpact(FPSBefore, FPSAfter);
	}

	// 즉시 피드백 표시 (데칼, 파티클) - 모든 네트워크 모드에서 로컬로 스폰
	if (bShowImmediateFeedback)
	{
		SpawnImmediateFeedback(Hit);
	}

	// 디버거에 파괴 기록
	if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
	{
		Debugger->RecordDestruction(
			Hit.ImpactPoint,
			Hit.ImpactNormal,
			HoleRadius,
			Owner->GetInstigator(),
			Hit.GetActor(),
			ProcessTimeMs
		);
	}

	// 이벤트 브로드캐스트
	OnDestructionRequested.Broadcast(Hit.ImpactPoint, Hit.ImpactNormal);

	// 투사체 제거
	if (bDestroyOnHit)
	{
		Owner->Destroy();
	}
}

bool UDestructionProjectileComponent::EnsureToolMesh()
{
	if (ToolMeshPtr.IsValid())
	{
		return true;
	}

	UDynamicMesh* TempMesh = NewObject<UDynamicMesh>(this);
	
	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	PrimitiveOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::SingleGroup;

	switch (ToolShape)
	{
	case EDestructionToolShape::Sphere:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			SphereRadius,
			SphereStepsPhi,
			SphereStepsTheta,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;
	case EDestructionToolShape::Cylinder:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			CylinderRadius,
			CylinderHeight,
			RadialSteps,
			HeightSubdivisions,
			bCapped,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;
	}

	
	ToolMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	TempMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Source)
	{
		*ToolMeshPtr = Source;
	});

	return true;
}

void UDestructionProjectileComponent::RequestDestructionManual(const FHitResult& HitResult)
{
	if (!HitResult.GetActor())
	{
		return;
	}

	URealtimeDestructibleMeshComponent* DestructComp =
		HitResult.GetActor()->FindComponentByClass<URealtimeDestructibleMeshComponent>();

	if (DestructComp)
	{
		ProcessDestructionRequest(DestructComp, HitResult);
	}
}

// [deprecated]
void UDestructionProjectileComponent::SpawnImmediateFeedback(const FHitResult& Hit)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 데칼 스폰
	//if (ImmediateDecalMaterial)
	//{
	//	const float DecalSize = HoleRadius * DecalSizeMultiplier;
	//
	//	UGameplayStatics::SpawnDecalAtLocation(
	//		World,
	//		ImmediateDecalMaterial,
	//		FVector(DecalSize, DecalSize, DecalSize),
	//		Hit.ImpactPoint,
	//		Hit.ImpactNormal.Rotation(),
	//		DecalLifeSpan
	//	);
	//}

	//// Niagara 파티클 스폰
	//if (ImmediateParticle)
	//{
	//	UNiagaraFunctionLibrary::SpawnSystemAtLocation(
	//		World,
	//		ImmediateParticle,
	//		Hit.ImpactPoint,
	//		Hit.ImpactNormal.Rotation(),
	//		FVector(1.0f),  // Scale
	//		true,           // bAutoDestroy
	//		true,           // bAutoActivate
	//		ENCPoolMethod::None,
	//		true            // bPreCullCheck
	//	);
	//}
}

