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
#include "Data/DecalMaterialDataAsset.h"
#include "Debug/DestructionDebugger.h"
#include "Subsystems/DestructionGameInstanceSubsystem.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "HAL/PlatformTime.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/MessageDialog.h" 

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

	UFunction* Func = FindFunction(TEXT("OpenDecalSizeEditor"));
	UE_LOG(LogTemp, Warning, TEXT("=== OpenDecalSizeEditor UFunction: %s ==="),
		Func ? TEXT("FOUND") : TEXT("NOT FOUND"));

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

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 자기 자신과 충돌한 경우 무시
	if (OtherActor == Owner)
	{
		return;
	}

	// Owner가 Pawn/Character인 경우 무시 (캐릭터 사망 방지)
	if (Owner->IsA<APawn>())
	{
		return;
	}

	// 맞은 Actor에서 파괴 컴포넌트 찾기
	URealtimeDestructibleMeshComponent* DestructComp =
		OtherActor->FindComponentByClass<URealtimeDestructibleMeshComponent>();

	if (DestructComp)
	{
		// 파괴 가능한 오브젝트에 충돌
		int32 ChunkNum = DestructComp->GetChunkNum();
		if (ChunkNum == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s : No chunk. Make chunk"), *DestructComp->GetName());
			if (bDestroyOnHit)
			{
				GetOwner()->Destroy();
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ProcessDestructionRequestForCell %d"), ChunkNum);
			ProcessDestructionRequestForChunk(DestructComp, Hit);
		}
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

void UDestructionProjectileComponent::ProcessDestructionRequestForChunk(URealtimeDestructibleMeshComponent* DestructComp, const FHitResult& Hit)
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
	
	// ===== DataAsset에서 Tool Shape 로드 (메시 생성 전에!) =====
	// ===== 절~~~~때 아래로 빼지마 
	FName SurfaceTypeForShape = DestructComp->SurfaceType;
	bool bHasDecalConfig = false;
	FDecalSizeConfig OverrideDecalConfig;
	
	if (DecalDataAsset)
	{ 
		bHasDecalConfig = DecalDataAsset->GetConfigRandom(DecalConfigID, SurfaceTypeForShape, OverrideDecalConfig);
		if (bHasDecalConfig)
		{
			bool bShapeChanged = (CylinderRadius != OverrideDecalConfig.CylinderRadius ||
				CylinderHeight != OverrideDecalConfig.CylinderHeight ||
				SphereRadius != OverrideDecalConfig.SphereRadius);

			CylinderRadius = OverrideDecalConfig.CylinderRadius;
			CylinderHeight = OverrideDecalConfig.CylinderHeight;
			SphereRadius = OverrideDecalConfig.SphereRadius;

			if (bShapeChanged && ToolMeshPtr.IsValid())
			{
				ToolMeshPtr.Reset();
			}
		}
	}
	
	float ToolRadius = ToolShape == EDestructionToolShape::Cylinder ? CylinderRadius : SphereRadius;
	// 오버랩 영역에 여유분 추가
	float OverlappedRadius = ToolRadius * 1.2f;
	
	// 자신과 겹치는 경우 제외
	TArray<AActor*> ActorToIgnore;
	ActorToIgnore.Add(Owner);
	
	// 중복 방지용 Set
	TSet<int32> Targets;
	
	// 직접 피격된 청크 반드시 포함
	int32 HitChunkIndex = DestructComp->GetChunkIndex(Hit.GetComponent());
	if (HitChunkIndex != INDEX_NONE)
	{
		Targets.Add(HitChunkIndex);
	}
	
	AActor* TargetActor = DestructComp->GetOwner();

	// 인접 청크 인덱스 추출
	TArray<int32> NearbyChunkIndices;
	NearbyChunkIndices.Reserve(32);
	DestructComp->FindChunksInRadius(Hit.ImpactPoint, OverlappedRadius, NearbyChunkIndices, false);
	
	const FBox ToolBounds = FBox::BuildAABB(Hit.ImpactPoint, FVector(OverlappedRadius));
	for (const int32 ChunkIndex : NearbyChunkIndices)
	{
		if (Targets.Contains(ChunkIndex))
		{
			continue;
		}
		
		UDynamicMeshComponent* Chunk = DestructComp->GetChunkMeshComponent(ChunkIndex);
		if (!Chunk)
		{
			continue;
		}
		
		if (!Chunk->IsVisible() || Chunk->GetOwner() != TargetActor)
		{
			continue;
		}

		const FBox ChunkBox = Chunk->Bounds.GetBox();
		if (ToolBounds.Intersect(ChunkBox))
		{
			DrawDebugAffetedChunks(ToolBounds, FColor::Black);
			Targets.Add(ChunkIndex);
		}
	}	
 
	
	FVector Direction = GetToolDirection(Hit, Owner);
	FVector ToolStart = Hit.ImpactPoint;
	FVector ToolEnd = ToolStart + (Direction * CylinderHeight);
	TArray<int32> LineAlongChunkIndices;
	LineAlongChunkIndices.Reserve(DestructComp->GetChunkNum());
	DestructComp->FindChunksAlongLine(ToolStart, ToolEnd, ToolRadius, LineAlongChunkIndices, false);
	for (int32 ChunkIndex : LineAlongChunkIndices)
	{
		if (auto ChunkComp = DestructComp->GetChunkMeshComponent(ChunkIndex))
		{
			Targets.Add(ChunkIndex);			
		}
	}

	if (!ToolMeshPtr.IsValid())
	{
		if (!EnsureToolMesh())
		{
			UE_LOG(LogTemp, Warning, TEXT("DestructionProjectileComponent: Tool mesh is invalid."));
		}
	}
	
	APawn* InstigatorPawn = Owner->GetInstigator();
	APlayerController* PC = InstigatorPawn ? Cast<APlayerController>(InstigatorPawn->GetController()) : nullptr;
	UDestructionNetworkComponent* NetworkComp = PC ? PC->FindComponentByClass<UDestructionNetworkComponent>() : nullptr;
	for (int32 TargetIndex : Targets)
	{
		FRealtimeDestructionRequest Request;
		Request.ImpactPoint = Hit.ImpactPoint;
		Request.ImpactNormal = Hit.ImpactNormal;
		Request.ChunkIndex = TargetIndex;
		Request.ToolForwardVector = GetToolDirection(Hit, Owner);		

		Request.ToolMeshPtr = ToolMeshPtr;
		Request.ToolShape = ToolShape;
	
		int32 HitMaterialID = DestructComp->GetMaterialIDFromFaceIndex(Hit.FaceIndex);
		if (HitMaterialID == 1)
		{
			Request.bSpawnDecal = false;
		}
		else
		{
			Request.bSpawnDecal = true;
		}
		FName SurfaceType = DestructComp->SurfaceType;
		Request.SurfaceType = SurfaceType;
		Request.DecalConfigID = DecalConfigID;  // 네트워크 전송용

		// GameInstanceSubsystem에 DecalDataAsset 설정 (서버/클라 모두)
		if (DecalDataAsset)
		{
			if (UGameInstance* GI = GetWorld()->GetGameInstance())
			{
				if (UDestructionGameInstanceSubsystem* Subsystem = GI->GetSubsystem<UDestructionGameInstanceSubsystem>())
				{
					if (!Subsystem->GetDecalDataAsset())
					{
						Subsystem->SetDecalDataAsset(DecalDataAsset);
					}
				}
			}
		}

		if (bHasDecalConfig)
		{
			Request.DecalSize = OverrideDecalConfig.DecalSize;
			Request.DecalLocationOffset = OverrideDecalConfig.LocationOffset;
			Request.DecalRotationOffset = OverrideDecalConfig.RotationOffset;
			Request.DecalMaterial = OverrideDecalConfig.DecalMaterial;
			Request.bRandomRotation = OverrideDecalConfig.bRandomDecalRotation; 
		}  

		SetShapeParameters(Request);		
	
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
	
		// Debug
		{
			DrawDebugToolShape(Request.ToolCenterWorld, Request.ToolForwardVector, FColor::Cyan);
			if (bShowAffetedChunks)
			{
				FBox ChunkBox = DestructComp->GetChunkMeshComponent(TargetIndex)->Bounds.GetBox();
				DrawDebugAffetedChunks(ChunkBox, FColor::Red);
			}
		}
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
		{
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
		}
	case EDestructionToolShape::Cylinder:
		{
			SurfaceMargin = CylinderHeight;
			UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			   TempMesh,
			   PrimitiveOptions,
			   FTransform::Identity,
			   CylinderRadius,
			   CylinderHeight + SurfaceMargin,
			   RadialSteps,
			   HeightSubdivisions,
			   bCapped,
			   EGeometryScriptPrimitiveOriginMode::Base
		   );
		}
		break;
	}

	
	ToolMeshPtr = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	TempMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Source)
	{
		*ToolMeshPtr = Source;
	});

	// Material 만들기 ( 구멍 내는 부분에 채워줄 material ) 
	const int32 InternalMaterialID = 1;
	
	// Attributes 활성화
	if (!ToolMeshPtr->HasAttributes())
	{
		ToolMeshPtr->EnableAttributes();
	}

	// MaterialID 속성 활성화
	if (!ToolMeshPtr->Attributes()->HasMaterialID())
	{
		ToolMeshPtr->Attributes()->EnableMaterialID();
	}

	// 모든 삼각형에 Material ID 설정
	UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDAttr = ToolMeshPtr->Attributes()->GetMaterialID();
	for (int32 TriId : ToolMeshPtr->TriangleIndicesItr())
	{
		MaterialIDAttr->SetValue(TriId, InternalMaterialID);
	}
	return true;
}

void UDestructionProjectileComponent::SetShapeParameters(FRealtimeDestructionRequest& OutRequest)
{
	const float PenetrationOffset = 0.5f; // 조절 가능
	const float TotalHeight = CylinderHeight + SurfaceMargin;
	switch (OutRequest.ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		{
			/*
			 * Cylinder 생성 시 Base로 생성 - 원기둥의 바닥을 원점(0, 0, 0)으로 해서 +z축으로 생성 
			 */
		OutRequest.Depth = CylinderHeight;
			OutRequest.ToolCenterWorld = OutRequest.ImpactPoint - (OutRequest.ToolForwardVector * SurfaceMargin);
		break;
		}
	case EDestructionToolShape::Sphere:
		{
		OutRequest.Depth = SphereRadius;
			OutRequest.ToolCenterWorld = OutRequest.ImpactPoint + (OutRequest.ToolForwardVector * PenetrationOffset);
		}
		break;
	default:
		{
		OutRequest.Depth = CylinderHeight;
			OutRequest.ToolCenterWorld = OutRequest.ImpactPoint - (OutRequest.ToolForwardVector * SurfaceMargin);
		}
		break;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Client] ToolCenterWorld=%s, ImpactPoint=%s, Forward=%s, SurfaceMargin=%.2f, Shape=%d"),
		*OutRequest.ToolCenterWorld.ToString(),
		*OutRequest.ImpactPoint.ToString(),
		*OutRequest.ToolForwardVector.ToString(),
		SurfaceMargin,
		(int32)ToolShape);

	// Shape별로 파라미터 채우기 (네트워크 전송용)
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		OutRequest.ShapeParams.Radius = CylinderRadius;
		OutRequest.ShapeParams.Height = CylinderHeight;
		OutRequest.ShapeParams.RadiusSteps = RadialSteps;
		OutRequest.ShapeParams.HeightSubdivisions = HeightSubdivisions;
		OutRequest.ShapeParams.bCapped = bCapped;
		OutRequest.ShapeParams.SurfaceMargin = SurfaceMargin;
		break;

	case EDestructionToolShape::Sphere:
		OutRequest.ShapeParams.Radius = SphereRadius;
		OutRequest.ShapeParams.StepsPhi = SphereStepsPhi;
		OutRequest.ShapeParams.StepsTheta = SphereStepsTheta;
		break;

	default:
		OutRequest.ShapeParams.Radius = CylinderRadius;
		OutRequest.ShapeParams.Height = CylinderHeight;
		OutRequest.ShapeParams.RadiusSteps = RadialSteps;
		OutRequest.ShapeParams.HeightSubdivisions = HeightSubdivisions;
		OutRequest.ShapeParams.bCapped = bCapped;
		OutRequest.ShapeParams.SurfaceMargin = SurfaceMargin;
		break;
	} 
}

void UDestructionProjectileComponent::DrawDebugToolShape(const FVector& Center, const FVector& Direction, const FColor& Color) const
{
	if (!GetWorld() || !bShowToolShape) 
	{
		return;
	}	
	
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		{
			DrawDebugCylinderInternal(Center, Direction, Color);
			break;
		}
	case EDestructionToolShape::Sphere:
		{
			DrawDebugSphereInternal(Center, Color);
			break;
		}
	}	
}

void UDestructionProjectileComponent::DrawDebugAffetedChunks(const FBox& ChunkBox, const FColor& Color) const
{
	if (!bShowAffetedChunks || !GetWorld())
	{
		return;
	}

	DrawDebugBox(GetWorld(), ChunkBox.GetCenter(),
		ChunkBox.GetExtent() + FVector(0.5f), Color, false,
		2.0f, 0, 2.5f);
}

void UDestructionProjectileComponent::DrawDebugCylinderInternal(const FVector& Center, const FVector& Direction,
                                                                const FColor& Color) const
{
	float TotalHeight = CylinderHeight + SurfaceMargin;	
	FVector Start = Center;
	FVector End = Center + (Direction * TotalHeight);
	DrawDebugCylinder(GetWorld(), Start, End, CylinderRadius, 16, Color, false, 5.0f, 0, 1.5f);
	DrawDebugPoint(GetWorld(), Start + (Direction * SurfaceMargin), 10.0f, FColor::Red, false, 5.0f);
}

void UDestructionProjectileComponent::DrawDebugSphereInternal(const FVector& Center, const FColor& Color) const
{
	DrawDebugSphere(GetWorld(), Center, SphereRadius, 16, Color, false, 5.0f, 0, 1.5f);
}

FVector UDestructionProjectileComponent::GetToolDirection(const FHitResult& Hit, AActor* Owner) const
{
	FVector Direction = (Hit.TraceEnd - Hit.TraceStart);

	if (Direction.IsNearlyZero() && Owner)
	{
		Direction = Owner->GetVelocity();
	}

	if (Direction.IsNearlyZero() && Owner)
	{
		Direction = Owner->GetActorForwardVector();
	}

	if (Direction.IsNearlyZero() && Owner)
	{
		Direction = GetForwardVector();
	}

	if (Direction.IsNearlyZero())
	{
		Direction = Hit.ImpactNormal;
	}

	return Direction.GetSafeNormal();
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
		// 파괴 가능한 오브젝트에 충돌
		int32 ChunkNum = DestructComp->GetChunkNum();
		if (ChunkNum == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s : No chunk. Make chunk"), *DestructComp->GetName());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ProcessDestructionRequestForCell %d"), ChunkNum);
			ProcessDestructionRequestForChunk(DestructComp, HitResult);
		}
	}
	else
	{
		// 파괴 불가능한 오브젝트에 충돌
		OnNonDestructibleHit.Broadcast(HitResult);

		if (bDestroyOnNonDestructibleHit && bDestroyOnHit)
		{
			GetOwner()->Destroy();
		}
	}
}

void UDestructionProjectileComponent::GetCalculateDecalSize(FName SurfaceType, FVector& LocationOffset, FRotator& RotatorOffset,
	FVector& SizeOffset) const
{
	if (DecalDataAsset)
	{
		// SurfaceType이 없으면 Default 사용
		FName ActualSurfaceType = SurfaceType.IsNone() ? FName("Default") : SurfaceType;

		FDecalSizeConfig Config;
		if (DecalDataAsset->GetConfig(DecalConfigID, ActualSurfaceType, 0,Config))
		{
			LocationOffset = Config.LocationOffset;
			RotatorOffset = Config.RotationOffset;
			SizeOffset = Config.DecalSize;
			return;
		}
	}
	if (bUseDecalSizeOverride)
	{
		LocationOffset = DecalLocationOffset;
		RotatorOffset = DecalRotationOffset;
		SizeOffset = DecalSizeOverride;
		return ;
	}

	float BaseSize = 0.0f;
	LocationOffset = FVector::ZeroVector;  
	RotatorOffset = FRotator::ZeroRotator; 
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		BaseSize = CylinderRadius;
		break;

	case EDestructionToolShape::Sphere:
		BaseSize = SphereRadius;
		break;

	default: 
		break;
	}

	float FinalSize	 = BaseSize * DecalSizeMultiplier;
	SizeOffset = FVector(FinalSize,FinalSize,FinalSize);
}