// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "ImpactProfileEditorViewport.h"

#include "Components/DestructionProjectileComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "AdvancedPreviewScene.h"
#include "EditorViewportClient.h"
#include "Slate/SceneViewport.h"

#include "Engine/StaticMeshActor.h"
#include "UObject/ConstructorHelpers.h"

#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Components/DecalComponent.h" 
#include "Components/LineBatchComponent.h"
#include "DataWrappers/ChaosVDParticleDataWrapper.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
static const float UE_RadiusOffset = 50.0f;
static const float UE_HeightOffset = 50.0f;

void SImpactProfileEditorViewport::Construct(const FArguments& InArgs)
{ 
	// 외부에서 전달 받은 데이터를 저장
	TargetComponent = InArgs._TargetComponent;

	if (UDestructionProjectileComponent* Comp = TargetComponent.Get())
	{ 
		PreviewSphereRadius = Comp->SphereRadius;
		PreviewCylinderRadius = Comp->CylinderRadius;
		PreviewCylinderHeight = Comp->CylinderHeight;
		PreviewToolShape = Comp->ToolShape;

		// 저장해둔 Editor 상태 로드
		DecalTransform = FTransform(
		Comp->DecalRotationInEditor,
		Comp->DecalLocationInEditor,
		Comp->DecalScaleInEditor	
		);

		ToolShapeTransform = FTransform(
			 Comp->ToolShapeRotationInEditor,
			 Comp->ToolShapeLocationInEditor,
			 FVector::OneVector
		 );

		if (Comp->bUseDecalSizeOverride)
		{
			DecalSize = Comp->DecalSizeOverride;
		}

		DecalMaterial = Comp->DecalMaterialInEditor;
	}
	else
	{ 
		DecalTransform = FTransform(
			FRotator(0.0f, 0.0f, 90.0f),
			FVector::ZeroVector,      
			FVector(1.0f, 10.0f, 10.0f)
		);

		ToolShapeTransform = FTransform::Identity;

		PreviewSphereRadius = 10;
		PreviewCylinderRadius = 10;
		PreviewCylinderHeight = 5;
		PreviewToolShape = EDestructionToolShape::Cylinder;
	}
	
	// 프리뷰 씬 생성
	FAdvancedPreviewScene::ConstructionValues CVS;
	CVS.bCreatePhysicsScene = false;
	CVS.LightBrightness = 3.0f;
	CVS.SkyBrightness = 1.0f;  
	CVS.bDefaultLighting = true;       
	CVS.bAllowAudioPlayback = false;
	PreviewScene = MakeShared<FAdvancedPreviewScene>(CVS);
	PreviewScene->SetFloorVisibility(false);

	// 부모 뷰포트 초기화
	SEditorViewport::Construct(SEditorViewport::FArguments());

	// 초기 뷰포트 생성
	RefreshPreview(); 
}

SImpactProfileEditorViewport::~SImpactProfileEditorViewport()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient.Reset(); 
	}

	if (PreviewScene.IsValid())
	{
		UWorld* World = PreviewScene->GetWorld();
		if (World && PreviewActor && IsValid(PreviewActor))
		{
			World->DestroyActor(PreviewActor);
		}
	}

	PreviewActor = nullptr;
	ProjectileMesh = nullptr;
	ToolShapeWireframe = nullptr;
	DecalPreviewComponent = nullptr;
	DecalTargetSurface = nullptr;
	DecalWireframe = nullptr;
}

void SImpactProfileEditorViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	if (PreviewActor)
	{
		Collector.AddReferencedObject(PreviewActor);
	}
	if (ProjectileMesh)
	{
		Collector.AddReferencedObject(ProjectileMesh);
	}
	if (ToolShapeWireframe)
	{
		Collector.AddReferencedObject(ToolShapeWireframe);
	}
	if (DecalPreviewComponent)
	{
		Collector.AddReferencedObject(DecalPreviewComponent);
	}
	if (DecalTargetSurface)
	{
		Collector.AddReferencedObject(DecalTargetSurface);
	}
	if (DecalWireframe)
	{
		Collector.AddReferencedObject(DecalWireframe);
	}
	if (DecalMaterial)
	{
		Collector.AddReferencedObject(DecalMaterial);
	}

}

void SImpactProfileEditorViewport::RefreshPreview()
{
	if (!PreviewScene.IsValid())
	{
		return;
	}

	UWorld* PreviewWorld = PreviewScene->GetWorld();
	if (!PreviewWorld)
	{
		return;
	}

	if (PreviewActor)
	{
		PreviewWorld->DestroyActor(PreviewActor);
		PreviewActor = nullptr;
		ProjectileMesh = nullptr;
		ToolShapeWireframe = nullptr;
		DecalPreviewComponent = nullptr;
		DecalTargetSurface = nullptr;
		DecalWireframe = nullptr; 
	} 

	// 기본 메시 로드
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr,
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* CylinderMesh = LoadObject<UStaticMesh>(nullptr,
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr,
		TEXT("/Engine/BasicShapes/Plane.Plane"));


	// 프리뷰 액터 생성
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags = RF_Transient;
	PreviewActor = PreviewWorld->SpawnActor<AActor>(AActor::StaticClass(), SpawnParams);

	// Root Component
	USceneComponent* Root = NewObject<USceneComponent>(PreviewActor);
	PreviewActor->SetRootComponent(Root);
	Root->RegisterComponent();



	// preview mesh는 항상 생성
	ProjectileMesh = NewObject<UStaticMeshComponent>(PreviewActor);
	ProjectileMesh->SetupAttachment(Root);
	ProjectileMesh->SetRelativeLocation(FVector::ZeroVector);
	ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	
	// 실제 projectile mesh를  가져오는 코드 
	UDestructionProjectileComponent* Comp = TargetComponent.Get();
	if (Comp)
	{
	
		UObject* Outer = Comp->GetOuter();
		UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Outer);

		if (BPClass && BPClass->SimpleConstructionScript)
		{
			USCS_Node* MyNode = nullptr;
			USCS_Node* ParentNode = nullptr;

			const TArray<USCS_Node*>& AllNodes = BPClass->SimpleConstructionScript->GetAllNodes();

			// 1. 내 노드 찾기
			for (USCS_Node* Node : AllNodes)
			{
				if (Node->ComponentTemplate == Comp)
				{
					MyNode = Node;
					break;
				}
			}

			// 2. 부모 노드 찾기 (ChildNodes 역탐색)
			if (MyNode)
			{
				for (USCS_Node* PotentialParent : AllNodes)
				{
					if (PotentialParent->ChildNodes.Contains(MyNode))
					{
						ParentNode = PotentialParent;
						break;
					}
				}
			}

			// 3. 부모가 StaticMeshComponent면 메시 복사
			if (ParentNode && ParentNode->ComponentTemplate)
			{
				if (UStaticMeshComponent* ParentMesh = Cast<UStaticMeshComponent>(ParentNode->ComponentTemplate))
				{
					ProjectileMesh->SetStaticMesh(ParentMesh->GetStaticMesh());
					// 머티리얼도 복사
					for (int32 i = 0; i < ParentMesh->GetNumMaterials(); ++i)
					{
						ProjectileMesh->SetMaterial(i, ParentMesh->GetMaterial(i));
					}

					ProjectileMesh->SetRelativeLocation(ParentMesh->GetRelativeLocation());
					ProjectileMesh->SetRelativeRotation(ParentMesh->GetRelativeRotation());
					ProjectileMesh->SetRelativeScale3D(ParentMesh->GetRelativeScale3D());

				}
			}
		} 
	}
	else if (PreviewMesh)
	{
		ProjectileMesh->SetStaticMesh(PreviewMesh);
		ProjectileMesh->SetRelativeLocation(PreviewMeshLocation);
		ProjectileMesh->SetRelativeRotation(PreviewMeshRotation);
	}
	ProjectileMesh->RegisterComponent();

	

	
	// Tool Shape Wireframe
	ToolShapeWireframe = NewObject<ULineBatchComponent>(PreviewActor);
	ToolShapeWireframe->SetupAttachment(Root);
	ToolShapeWireframe->bCalculateAccurateBounds= false;
	ToolShapeWireframe->RegisterComponent(); 
 
	// 데칼 투영용 타겟 표면 (벽 역할)
	DecalTargetSurface = NewObject<UStaticMeshComponent>(PreviewActor);
	DecalTargetSurface->SetupAttachment(Root); 
	DecalTargetSurface->SetStaticMesh(PlaneMesh);

	// 벽처럼 세우기: 90도 회전하여 수직으로 세움
	DecalTargetSurface->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
	DecalTargetSurface->SetRelativeLocation(FVector(0.0f, -0.5f, 0.0f));
	DecalTargetSurface->SetRelativeScale3D(FVector(10.0f, 10.0f, 1.0f)); // 큰 표면
	DecalTargetSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 기본 머티리얼 (밝은 회색)
	UMaterial* DefaultMat = LoadObject<UMaterial>(nullptr,
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (DefaultMat)
	{
		DecalTargetSurface->SetMaterial(0, DefaultMat);
	}
	DecalTargetSurface->RegisterComponent();

	// Decal 프리뷰 컴포넌트
	DecalPreviewComponent = NewObject<UDecalComponent>(PreviewActor);
	DecalPreviewComponent->SetupAttachment(Root);  
	DecalPreviewComponent->SetRelativeLocation(FVector(0.0f, 0.0f, 0.0f)); 
	DecalPreviewComponent->SetRelativeRotation(FRotator(0.0f, 180.0f, 0.0f));
	DecalPreviewComponent->DecalSize = DecalSize;
	DecalPreviewComponent->RegisterComponent();

	if (DecalMaterial)
	{
		DecalPreviewComponent->SetDecalMaterial(DecalMaterial);
	}

	DecalWireframe = NewObject<ULineBatchComponent>(PreviewActor);
	DecalWireframe->SetupAttachment(Root);
	DecalWireframe->bCalculateAccurateBounds = false;
	DecalWireframe->RegisterComponent();

	// Transform / Scale 적용 
	UpdateToolShapeWireframe();
	UpdateDecalMesh();
 
	// 씬 갱신
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SImpactProfileEditorViewport::SetDecalTransform(const FTransform& InTransform)
{
	DecalTransform = InTransform;
	UpdateDecalMesh();
	SaveState();
}

void SImpactProfileEditorViewport::SetToolShapeLocation(const FVector& InLocation)
{
	ToolShapeTransform.SetLocation(InLocation);
	UpdateToolShapeWireframe(); 
	SaveState();
}

void SImpactProfileEditorViewport::SetToolShapeRotation(const FRotator& InRotation)
{
	ToolShapeTransform.SetRotation(InRotation.Quaternion()); 
	UpdateToolShapeWireframe();  
	SaveState();
}

void SImpactProfileEditorViewport::SetPreviewMesh(UStaticMesh* InPreviewMesh)
{
	PreviewMesh = InPreviewMesh;

	if ( ProjectileMesh && InPreviewMesh)
	{
		ProjectileMesh->SetStaticMesh(InPreviewMesh);
		ProjectileMesh->MarkRenderStateDirty();
	}
	
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}

	RefreshPreview(); 
}

void SImpactProfileEditorViewport::SetPreviewToolShape(EDestructionToolShape NewShape)
{
	PreviewToolShape = NewShape;
	RefreshPreview();
	SaveState();
}

void SImpactProfileEditorViewport::SetPreviewSphere(float InRadius)
{
	PreviewSphereRadius = InRadius;
	UpdateToolShapeWireframe(); 
	SaveState();
}

void SImpactProfileEditorViewport::SetPreviewCylinderRadius(float InRadius)
{
	PreviewCylinderRadius = InRadius;
	UpdateToolShapeWireframe(); 
	SaveState();
}

void SImpactProfileEditorViewport::SetPreviewCylinderHeight(float InHeight)
{
	PreviewCylinderHeight = InHeight;
	UpdateToolShapeWireframe(); 
	SaveState();
}

void SImpactProfileEditorViewport::SetPreviewMeshLocation(const FVector& InLocation)
{
	PreviewMeshLocation = InLocation;
	
	if (ProjectileMesh)
	{
		ProjectileMesh->SetRelativeLocation(InLocation);
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SImpactProfileEditorViewport::SetPreviewMeshRotation(const FRotator& InRotator)
{
	PreviewMeshRotation = InRotator;

	if (ProjectileMesh)
	{
		ProjectileMesh->SetRelativeRotation(InRotator);
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SImpactProfileEditorViewport::SetDecalVisible(bool bVisible)
{
	bShowDecal = bVisible;
	if (DecalPreviewComponent)
	{
		DecalPreviewComponent->SetVisibility(bVisible);
	}
	if (DecalWireframe)
	{
		DecalWireframe->SetVisibility(bVisible);
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SImpactProfileEditorViewport::SetToolShapeVisible(bool bVisible)
{
	bShowToolShape = bVisible;
	if (ToolShapeWireframe)
	{
		ToolShapeWireframe->SetVisibility(bVisible);
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SImpactProfileEditorViewport::SetPreviewMeshVisible(bool bVisible)
{
	bShowPreviewMesh = bVisible;
	if (ProjectileMesh)
	{
		ProjectileMesh->SetVisibility(bVisible);
	}
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SImpactProfileEditorViewport::UpdateDecalMesh()
{
	if (!DecalPreviewComponent)
	{
		return;
	}

	// 기본 오프셋
	// 표면 앞에 위치 + 표면을 향한 방향
	
	const FVector BaseOffset(0.0f, 0.0f, 0.0f);
	FVector FinalLocation = BaseOffset + DecalTransform.GetLocation(); 

	const FRotator BaseRotation(0.0f, 180.0f, 0.0f);
	FRotator FinalRotation = BaseRotation + DecalTransform.GetRotation().Rotator();

	DecalPreviewComponent->SetRelativeLocation(FinalLocation);
	DecalPreviewComponent->SetRelativeRotation(FinalRotation);

	FVector ScaledSize = DecalSize * DecalTransform.GetScale3D();
	DecalPreviewComponent->DecalSize = ScaledSize;

	DecalPreviewComponent->MarkRenderStateDirty();

	UpdateDecalWireframe();
	
	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	} 
}
 

void SImpactProfileEditorViewport::UpdateDecalWireframe()
{
	if (!DecalWireframe)
	{
		return;
	}

	DecalWireframe->Flush();

	const FColor WireColor = FColor::Green;
	const float Thickness= 2.0f;
	const float LifeTime = 0.0f;
	const uint8 DepthPriority = SDPG_Foreground;  // 항상 앞에 그리기

	const FVector HalfSize = DecalPreviewComponent->DecalSize;

	TArray<FVector> LocalCorners;
	LocalCorners.SetNum(8);
	
	LocalCorners[0] = FVector(-HalfSize.X, -HalfSize.Y,  HalfSize.Z);  // 전면 좌상
	LocalCorners[1] = FVector(-HalfSize.X,  HalfSize.Y,  HalfSize.Z);  // 전면 우상
	LocalCorners[2] = FVector(-HalfSize.X, -HalfSize.Y, -HalfSize.Z);  // 전면 좌하
	LocalCorners[3] = FVector(-HalfSize.X,  HalfSize.Y, -HalfSize.Z);  // 전면 우하
	LocalCorners[4] = FVector( HalfSize.X, -HalfSize.Y,  HalfSize.Z);  // 후면 좌상
	LocalCorners[5] = FVector( HalfSize.X,  HalfSize.Y,  HalfSize.Z);  // 후면 우상
	LocalCorners[6] = FVector( HalfSize.X, -HalfSize.Y, -HalfSize.Z);  // 후면 좌하
	LocalCorners[7] = FVector( HalfSize.X,  HalfSize.Y, -HalfSize.Z);  // 후면 우하

	FRotator Rotation = DecalPreviewComponent->GetRelativeRotation();
	FVector Location = DecalPreviewComponent->GetRelativeLocation();
	
	FTransform BoxTransform(Rotation, Location, FVector::OneVector);

	// 월드 좌표로 변환
	TArray<FVector> WorldCorners;
	WorldCorners.SetNum(8);
	for (int32 i = 0; i < 8; i++)
	{
		WorldCorners[i] = BoxTransform.TransformPosition(LocalCorners[i]);
	}

	// 12개의 Edge 그리기

	// 전면 (Front face)
	DecalWireframe->DrawLine(WorldCorners[0], WorldCorners[1], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[1], WorldCorners[3], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[3], WorldCorners[2], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[2], WorldCorners[0], WireColor, DepthPriority, Thickness, LifeTime);

	// 후면 (Back face) 
	DecalWireframe->DrawLine(WorldCorners[4], WorldCorners[5], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[5], WorldCorners[7], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[7], WorldCorners[6], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[6], WorldCorners[4], WireColor, DepthPriority, Thickness, LifeTime);

	// 연결 Edge  
	DecalWireframe->DrawLine(WorldCorners[0], WorldCorners[4], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[1], WorldCorners[5], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[2], WorldCorners[6], WireColor, DepthPriority, Thickness, LifeTime);
	DecalWireframe->DrawLine(WorldCorners[3], WorldCorners[7], WireColor, DepthPriority, Thickness, LifeTime);

	DecalWireframe->MarkRenderStateDirty(); 
}

void SImpactProfileEditorViewport::SetDecalMaterial(UMaterialInterface* InMaterial)
{
	DecalMaterial = InMaterial;

	if (DecalPreviewComponent)
	{
		DecalPreviewComponent->SetDecalMaterial(InMaterial);
		DecalPreviewComponent->MarkRenderStateDirty();

		if (ViewportClient.IsValid())
		{
			ViewportClient->Invalidate();
		}
	} 

	SaveState();
}

void SImpactProfileEditorViewport::SetDecalSize(const FVector& InSize)
{
	DecalSize = InSize;
	UpdateDecalMesh();
	SaveState();
}


void SImpactProfileEditorViewport::SetTargetComponent(UDestructionProjectileComponent* InComponent)
{
	TargetComponent = InComponent;
	RefreshPreview();
}


TSharedRef<FEditorViewportClient> SImpactProfileEditorViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShareable(new FImpactProfileViewportClient(
		  PreviewScene.Get(),
		  SharedThis(this)
	  ));

	ViewportClient->SetRealtime(true);
	// 카메라: 표면 앞쪽에서 바라봄 (-X에서 +X 방향으로)
	ViewportClient->SetViewLocation(FVector(-150.0f, 100.0f, 50.0f));
	ViewportClient->SetViewRotation(FRotator(-15.0f, -30.0f, 0.0f));

	return ViewportClient.ToSharedRef();
}

//////////////////////////////////////////////////////////////////////////
// FImpactProfileViewportClient
//////////////////////////////////////////////////////////////////////////

void SImpactProfileEditorViewport::UpdateToolShapeWireframe()
{
	if (!ToolShapeWireframe)
	{
		return;
	}

	ToolShapeWireframe->Flush();

	const FColor WireColor = FColor::Yellow;
	const float Thickness = 2.0f;
	const float LifeTime = 0.0f;
	const uint8 DepthPriority = SDPG_Foreground; 
	const int Segments = 6;
	FVector Location = ToolShapeTransform.GetLocation();
	FRotator Rotation = ToolShapeTransform.GetRotation().Rotator();
	float HalfHeight = PreviewCylinderHeight * 0.5f;
	switch (PreviewToolShape)
	{
		case EDestructionToolShape::Cylinder: 
		{ 
			FVector UpDir = Rotation.RotateVector(FVector::UpVector);
			FVector StartPoint = Location - (UpDir * HalfHeight); // 바닥 중심
			FVector EndPoint = Location + (UpDir * HalfHeight); // 천장 중심
			ToolShapeWireframe->DrawCylinder(StartPoint, EndPoint, PreviewCylinderRadius, Segments, WireColor, LifeTime, DepthPriority, Thickness);
		} 
		break;

		case EDestructionToolShape::Sphere:
		{
			ToolShapeWireframe->DrawSphere(Location, PreviewSphereRadius, Segments, WireColor, LifeTime, DepthPriority, Thickness);  
		}
		
		break; 

		default:
		{	
			FVector UpDir = Rotation.RotateVector(FVector::UpVector);
			FVector StartPoint = Location - (UpDir * HalfHeight); // 바닥 중심
			FVector EndPoint = Location + (UpDir * HalfHeight); // 천장 중심
			ToolShapeWireframe->DrawCylinder(StartPoint, EndPoint, PreviewCylinderRadius, Segments, WireColor, LifeTime, DepthPriority, Thickness);
			
		}
			break;	
	}

	ToolShapeWireframe->MarkRenderStateDirty();

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}
  
void SImpactProfileEditorViewport::SaveState()
{
	UDestructionProjectileComponent* Comp = TargetComponent.Get();
	if (!Comp)
	{
		return;
	}

	// Decal Transform 
	Comp->DecalLocationInEditor = DecalTransform.GetLocation(); 
	Comp->DecalRotationInEditor = DecalTransform.GetRotation().Rotator();
	Comp->DecalScaleInEditor = DecalTransform.GetScale3D();

	// ToolShape Transform
	Comp->ToolShapeLocationInEditor = ToolShapeTransform.GetLocation();
	Comp->ToolShapeRotationInEditor = ToolShapeTransform.GetRotation().Rotator();

	Comp->ToolShape = PreviewToolShape;
	Comp->SphereRadius = PreviewSphereRadius;
	Comp->CylinderRadius = PreviewCylinderRadius;
	Comp->CylinderHeight = PreviewCylinderHeight;

	// override
	Comp->bUseDecalSizeOverride = true;
	Comp->DecalLocationOffset = DecalTransform.GetLocation();
	Comp->DecalRotationOffset = DecalTransform.GetRotation().Rotator();
      
	// Material 저장 ← 추가
	Comp->DecalMaterialInEditor = DecalMaterial; 

	Comp->MarkPackageDirty();
}

FImpactProfileViewportClient::FImpactProfileViewportClient(
	FAdvancedPreviewScene* InPreviewScene,
	const TWeakPtr<SEditorViewport>& InEditorViewport)
	: FEditorViewportClient(nullptr, InPreviewScene, InEditorViewport)
	, PreviewScene(InPreviewScene)
{
	SetViewMode(VMI_Lit);

	// 카메라 설정
	SetViewportType(LVT_Perspective);
	SetViewLocation(FVector(-150.0f, 100.0f, 50.0f));
	SetViewRotation(FRotator(-15.0f, -30.0f, 0.0f));

	// 조작 설정
	bSetListenerPosition = false;
	EngineShowFlags.SetGrid(true);
}

void FImpactProfileViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	if (PreviewScene)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}
}

FSceneInterface* FImpactProfileViewportClient::GetScene() const
{
	return PreviewScene->GetScene();
}

FLinearColor FImpactProfileViewportClient::GetBackgroundColor() const
{
	return FLinearColor(0.1f, 0.1f, 0.1f, 1.0f);
}
