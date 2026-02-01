// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#pragma once

#include "CoreMinimal.h"
#include "Actors/AnchorActor.h"
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

UCLASS(ClassGroup = (RealtimeDestruction))
class REALTIMEDESTRUCTION_API AAnchorVolumeActor : public AAnchorActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AAnchorVolumeActor();

	virtual void ApplyToAnchors(const FTransform& MeshTransform, FGridCellLayout& CellCache) override;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;

	UPROPERTY()
	TObjectPtr<UBoxComponent> Box;

	UPROPERTY()
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
