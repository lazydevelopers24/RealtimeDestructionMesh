// Fill out your copyright notice in the Description page of Project Settings.


#include "Actors/AnchorPlaneActor.h"


// Sets default values
AAnchorPlaneActor::AAnchorPlaneActor()
{
	PrimaryActorTick.bCanEverTick = false;
	
	bIsEditorOnlyActor = true;

#if WITH_EDITORONLY_DATA

	PlaneMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlaneMesh"));
	RootComponent = PlaneMesh;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeAsset(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeAsset.Succeeded())
	{
		PlaneMesh->SetStaticMesh(CubeAsset.Object);
	}

	PlaneMesh->SetHiddenInGame(true);
	PlaneMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	PlaneMesh->SetRelativeScale3D(FVector(0.01f, 1.0f, 1.0f));
	
#endif
}

// Called when the game starts or when spawned
void AAnchorPlaneActor::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void AAnchorPlaneActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

