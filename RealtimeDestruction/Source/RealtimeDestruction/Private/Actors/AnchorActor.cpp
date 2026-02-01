// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.


#include "Actors/AnchorActor.h"


// Sets default values
AAnchorActor::AAnchorActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;
	bIsEditorOnlyActor = true;

}

// Called when the game starts or when spawned
void AAnchorActor::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void AAnchorActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void AAnchorActor::ApplyToAnchors(const FTransform& MeshTransform, FGridCellLayout& CellCache)
{
}

