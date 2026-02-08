// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Actors/AnchorVolumeActor.h"

#include "GridCellBuilder.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"

// Sets default values
AAnchorVolumeActor::AAnchorVolumeActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;
	bIsEditorOnlyActor = true;
#if WITH_EDITORONLY_DATA
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));

	if (Sprite)
	{
		RootComponent = Sprite;
		Sprite->SetHiddenInGame(true);
		Sprite->SetIsVisualizationComponent(true);
		Sprite->bIsScreenSizeScaled = true;

		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> Icon(TEXT("/Engine/EditorResources/S_Actor.S_Actor"));
		if (Icon.Succeeded())
		{
			Sprite->Sprite = Icon.Get();
		}
	}
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Sphere"));

    if (Box)
    {
	    Box->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	    Box->SetGenerateOverlapEvents(false);
	    Box->SetHiddenInGame(true);
	    Box->SetIsVisualizationComponent(true);
	    Box->bDrawOnlyIfSelected = false;
	    Box->SetupAttachment(RootComponent);
    }

	if (Sphere)
	{
		Sphere->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Sphere->SetGenerateOverlapEvents(false);
		Sphere->SetHiddenInGame(true);
		Sphere->SetIsVisualizationComponent(true);
		Sphere->bDrawOnlyIfSelected = false;
		Sphere->SetupAttachment(RootComponent);
	}
#endif

}

void AAnchorVolumeActor::ApplyToAnchors(const FTransform& MeshTransform, FGridCellLayout& CellCache)
{
	Super::ApplyToAnchors(MeshTransform, CellCache);

	if (this->Shape == EAnchorVolumeShape::Box)
	{
		FGridCellBuilder::SetAnchorsByFiniteBox(
		   this->GetActorTransform(),
		   this->BoxExtent,
		   MeshTransform,
		   CellCache,
		   this->bIsEraser);
	}

	if (this->Shape == EAnchorVolumeShape::Sphere)
	{
		FGridCellBuilder::SetAnchorsByFiniteSphere(
		   this->GetActorTransform(),
		   this->SphereRadius,
		   MeshTransform,
		   CellCache,
		   this->bIsEraser);
	}
}

// Called when the game starts or when spawned
void AAnchorVolumeActor::BeginPlay()
{
	Super::BeginPlay();
}

// Called every frame
void AAnchorVolumeActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void AAnchorVolumeActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisualization();
}

#if WITH_EDITOR
void AAnchorVolumeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RefreshVisualization();
}

void AAnchorVolumeActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

#if WITH_EDITORONLY_DATA
	if (!bFinished)
	{
		return;
	}

	if (bBakingScale)
	{
		return;
	}

	TGuardValue<bool> Guard(bBakingScale, true);
	CommitScaleToShapeParamAndReset();
#endif	
}

void AAnchorVolumeActor::EditorApplyScale(const FVector& DeltaScale, const FVector* PivotLocation, bool bAltDown,
	bool bShiftDown, bool bCtrlDown)
{
	Super::EditorApplyScale(DeltaScale, PivotLocation, bAltDown, bShiftDown, bCtrlDown);
#if WITH_EDITORONLY_DATA
	if (Shape != EAnchorVolumeShape::Sphere || !Sphere)
	{
		return;
	}

	// 스케일 드래그 시작 시점 스냅샷
	if (!bSphereScalePreview)
	{
		bSphereScalePreview = true;
		SphereRadiusAtScale = SphereRadius;
		SpherePreviewFactor = 1.0f;

		// 현재 스케일 드래그 트랜잭션에 이 액터가 포함되도록 1회 Modify
		Modify();
	}

	UpdateSphereScalePreviewFromActorScale();
#endif
}

void AAnchorVolumeActor::CommitScaleToShapeParamAndReset()
{
#if WITH_EDITORONLY_DATA
	const FVector Scale = GetActorScale3D();
	if (Scale.Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
	{
		return;
	}

	const FVector AbsScale = Scale.GetAbs();

	Modify();

	if (Shape == EAnchorVolumeShape::Box)
	{
		BoxExtent.X = FMath::Max(1.0f, BoxExtent.X * AbsScale.X);
		BoxExtent.Y = FMath::Max(1.0f, BoxExtent.Y * AbsScale.Y);
		BoxExtent.Z = FMath::Max(1.0f, BoxExtent.Z * AbsScale.Z);
	}
	else
	{
		// const float UniformScale = AbsScale.GetMax();
		// SphereRadius = FMath::Max(1.0f, SphereRadius * UniformScale);

		const float Factor = bSphereScalePreview ? SpherePreviewFactor : ComputeSphereFactorFromAbsScale(AbsScale);
		const float BaseRadius = bSphereScalePreview ? SphereRadiusAtScale : SphereRadius;
		SphereRadius = FMath::Max(1.0f, BaseRadius * FMath::Max(0.01f, Factor));
	}

	SetActorScale3D(FVector::OneVector);

	if (Box)
	{
		Box->SetRelativeScale3D(FVector::OneVector);
	}
	
	if (Sphere)
	{
		Sphere->SetRelativeScale3D(FVector::OneVector);
	}

	bSphereScalePreview = false;
	SphereRadiusAtScale = 0.0f;
	SpherePreviewFactor = 1.0f;

	RefreshVisualization();	
#endif
}

void AAnchorVolumeActor::UpdateSphereScalePreviewFromActorScale()
{
#if WITH_EDITORONLY_DATA
	if (!Sphere || Shape != EAnchorVolumeShape::Sphere)
	{
		return;
	}

	const FVector AbsScale = GetActorScale3D().GetAbs();
	if (AbsScale.Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
	{
		SpherePreviewFactor = 1.0f;
		Sphere->SetRelativeScale3D(FVector::OneVector);
		Sphere->SetSphereRadius(SphereRadiusAtScale, false);
		Sphere->MarkRenderStateDirty();
		return;
	}

	SpherePreviewFactor = FMath::Max(0.01f, ComputeSphereFactorFromAbsScale(AbsScale));
	const float PreviewRadius = FMath::Max(1.0f, SphereRadiusAtScale * SpherePreviewFactor);
	Sphere->SetSphereRadius(PreviewRadius, /*bUpdateOverlaps*/ false);

	Sphere->SetRelativeScale3D(SafeReciprocalAbsScale(AbsScale));
	Sphere->MarkRenderStateDirty();
#endif
}

float AAnchorVolumeActor::ComputeSphereFactorFromAbsScale(const FVector& AbsScale)
{
	const float Dx = FMath::Abs(AbsScale.X - 1.0f);
	const float Dy = FMath::Abs(AbsScale.Y - 1.0f);
	const float Dz = FMath::Abs(AbsScale.Z - 1.0f);

	if (Dx >= Dy && Dx >= Dz)
	{
		return AbsScale.X;
	}

	if (Dy >= Dx && Dy >= Dz)
	{
		return AbsScale.Y;
	}

	return AbsScale.Z;
}

FVector AAnchorVolumeActor::SafeReciprocalAbsScale(const FVector& AbsScale)
{
	constexpr float Min = KINDA_SMALL_NUMBER;
	return FVector(
		(AbsScale.X > Min) ? (1.0f / AbsScale.X) : 1.0f,
		(AbsScale.Y > Min) ? (1.0f / AbsScale.Y) : 1.0f,
		(AbsScale.Z > Min) ? (1.0f / AbsScale.Z) : 1.0f);
}
#endif

void AAnchorVolumeActor::RefreshVisualization()
{
#if WITH_EDITORONLY_DATA

	if (Box)
	{
		Box->SetBoxExtent(BoxExtent);
	}

	if (Sphere)
	{
		Sphere->SetSphereRadius(SphereRadius);
	}

	const bool bUseBox = (Shape == EAnchorVolumeShape::Box);

	if (Box)
	{
		Box->SetVisibility(bUseBox, true);
		Box->SetHiddenInGame(!bUseBox, true);
		Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (Sphere)
	{
		Sphere->SetVisibility(!bUseBox, true);
		Sphere->SetHiddenInGame(bUseBox, true);
		Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	const FColor ModeColor = bIsEraser ? FColor(255, 80, 80) : FColor(80, 255, 80);

	TInlineComponentArray<UShapeComponent*> ShapeComps;
	GetComponents<UShapeComponent>(ShapeComps);

	for (UShapeComponent* ShapeComp : ShapeComps)
	{
		if (!IsValid(ShapeComp))
		{
			continue;
		}

		ShapeComp->ShapeColor = ModeColor;		
		ShapeComp->SetLineThickness(2.0f);
		ShapeComp->MarkRenderStateDirty();
	}
#endif
}
