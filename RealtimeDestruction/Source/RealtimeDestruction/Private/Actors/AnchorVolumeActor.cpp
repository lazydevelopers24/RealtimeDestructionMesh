// Fill out your copyright notice in the Description page of Project Settings.


#include "Actors/AnchorVolumeActor.h"

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

