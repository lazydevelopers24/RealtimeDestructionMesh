// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AnchorVolumeActor.generated.h"

class USphereComponent;
class UBoxComponent;
class UBillboardComponent;

UENUM()
enum class EAnchorVolumeShape : uint8
{
	Box,
	Sphere
};

UCLASS()
class REALTIMEDESTRUCTION_API AAnchorVolumeActor : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AAnchorVolumeActor();

#if WITH_EDITORONLY_DATA

	UPROPERTY(VisibleAnywhere, Category="AnchorActor|Shape")
	TObjectPtr<UBillboardComponent> Sprite;

	UPROPERTY(VisibleAnywhere, Category="AnchorActor|Shape")
	TObjectPtr<UBoxComponent> Box;

	UPROPERTY(VisibleAnywhere, Category="AnchorActor|Shape")
	TObjectPtr<USphereComponent> Sphere;	
#endif

	FORCEINLINE FVector GetLocalBoxExtent() const { return BoxExtent; }
	FORCEINLINE float GetLocalSphereRadius() const { return SphereRadius; }

	UPROPERTY(EditAnywhere, Category="AnchorActor|Options")
	EAnchorVolumeShape Shape = EAnchorVolumeShape::Box;

	UPROPERTY(EditAnywhere, Category="AnchorActor|Options")
	bool bIsEraser = false;

	UPROPERTY(EditAnywhere, Category="AnchorActor|Options")
	int32 Priority = 0;

	UPROPERTY(EditAnywhere, Category="AnchorActor|Options", meta=(ClampMin="1.0", EditCondition="Shape==EAnchorVolumeShape::Box", EditConditionHides))
	FVector BoxExtent = FVector(50.0f, 50.0f, 50.0f);

	UPROPERTY(EditAnywhere, Category="AnchorActor|Options", meta=(ClampMin="1.0", EditCondition="Shape==EAnchorVolumeShape::Sphere", EditConditionHides))
	float SphereRadius = 100.0f;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void RefreshVisualization();	
	
};
