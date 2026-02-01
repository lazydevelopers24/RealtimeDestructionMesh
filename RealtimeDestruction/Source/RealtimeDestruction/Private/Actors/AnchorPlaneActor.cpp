// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Actors/AnchorPlaneActor.h"

#include "GridCellBuilder.h"

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

void AAnchorPlaneActor::ApplyToAnchors(const FTransform& MeshTransform, FGridCellLayout& CellCache)
{
	Super::ApplyToAnchors(MeshTransform, CellCache);

	FGridCellBuilder::SetAnchorsByFinitePlane(
				this->GetActorTransform(),
				MeshTransform,
				CellCache,
				this->bIsEraser);
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

