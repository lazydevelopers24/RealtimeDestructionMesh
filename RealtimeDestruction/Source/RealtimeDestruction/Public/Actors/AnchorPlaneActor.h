// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AnchorPlaneActor.generated.h"

UCLASS()
class REALTIMEDESTRUCTION_API AAnchorPlaneActor : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AAnchorPlaneActor();

#if WITH_EDITORONLY_DATA

	UPROPERTY(VisibleAnywhere, Category = "AnchorActor|Plane")
	TObjectPtr<UStaticMeshComponent> PlaneMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PlaneMID;
	
#endif

	UPROPERTY(EditAnywhere, Category = "AnchorActor|Options")
	bool bIsEraser = false;

	UPROPERTY(EditAnywhere, Category = "AnchorActor|Options")
	int32 Priority = 0;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	
	
};
