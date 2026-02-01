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
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DestructionTypes.h"
#include "Components/SceneComponent.h"

#include "DestructionProjectileComponent.generated.h"
class UMaterialInterface;
class UNiagaraSystem;

//=============================================================================
// Delegates
//=============================================================================

// Called when a destruction request is sent
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDestructionRequested, const FVector&, ImpactPoint, const FVector&, ImpactNormal);

// Called when hitting a non-destructible object
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNonDestructibleHit, const FHitResult&, HitResult);

/**
 * Destruction component for projectiles.
 *
 * Attach this component to a projectile Actor to:
 * 1. Send destruction requests to hit RealtimeDestructibleMeshComponents
 * 2. Display immediate feedback decals
 * 3. Automatically destroy the projectile
 *
 * [Network Requirements]
 * In multiplayer games, this component only processes destruction
 * on projectiles spawned by the server. Client-only projectiles
 * (for visual effects) will not trigger destruction.
 *
 * Recommended Pattern (Client Prediction + Server Authority):
 * 1. Client: Spawn local projectile (for effects/feedback, optional)
 * 2. Client -> Server RPC -> Server: Spawn projectile (for actual hit detection)
 * 3. On server projectile hit, propagate destruction to all clients via MulticastApplyOps
 *
 * Usage:
 * 1. Add this component to your projectile Blueprint
 * 2. Configure HoleRadius and decal settings
 * 3. For multiplayer: Ensure projectiles are spawned on the server
 */
UCLASS(ClassGroup = (RealtimeDestruction), meta = (BlueprintSpawnableComponent))
class REALTIMEDESTRUCTION_API UDestructionProjectileComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UDestructionProjectileComponent();

#if	WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	//=========================================================================
	// Destruction Settings
	//=========================================================================

	/**
	 * Whether to automatically bind to OnHit events.
	 *
	 * true: Component automatically detects collisions (when used standalone)
	 * false: Requires external call to RequestDestructionManual()
	 *        (when another component or Actor already handles URealtimeDestructibleMeshComponent-related collisions)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	bool bAutoBindHit = false;

	/** Hole radius (cm) - for compatibility */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	float HoleRadius = 10.0f;

	// Variable for changing Tool Shape
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape")
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Debug")
	bool bShowToolShape = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Debug")
	bool bShowAffetedChunks = false;

	//=========================================================================
	// Cylinder-specific Parameters
	//=========================================================================
	/** Cylinder Radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Cylinder", EditConditionHides))
	float CylinderRadius = 10.0f;

	/** Cylinder Height */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Cylinder", EditConditionHides))
	float CylinderHeight = 400.0f;

	/** Number of radial segments for circular cross-section */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (ClampMin = 3, ClampMax = 64,
			EditCondition = "ToolShape == EDestructionToolShape::Cylinder",
			EditConditionHides))
	int32 RadialSteps = 12;

	/** Number of height subdivisions */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (ClampMin = 0, ClampMax = 32,
			EditCondition = "ToolShape == EDestructionToolShape::Cylinder",
			EditConditionHides))
	int32 HeightSubdivisions = 0;

	/** Whether the cylinder is capped */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Cylinder",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Cylinder",
			EditConditionHides))
	bool bCapped = true;

	//=========================================================================
	// Sphere-specific Parameters
	//=========================================================================

	/** Sphere Radius */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Sphere",
		meta = (EditCondition = "ToolShape == EDestructionToolShape::Sphere", EditConditionHides))
	float SphereRadius = 10.0f;

	/** Sphere latitude subdivisions (phi) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Sphere",
		meta = (ClampMin = 3, ClampMax = 128,
			EditCondition = "ToolShape == EDestructionToolShape::Sphere",
			EditConditionHides))
	int32 SphereStepsPhi = 8;

	/** Sphere longitude subdivisions (theta) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction|Shape|Sphere",
		meta = (ClampMin = 3, ClampMax = 128,
			EditCondition = "ToolShape == EDestructionToolShape::Sphere",
			EditConditionHides))
	int32 SphereStepsTheta = 16;

	/** Automatically destroy the projectile after collision */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	bool bDestroyOnHit = true;

	/** Destroy projectile even when hitting non-destructible objects */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction")
	bool bDestroyOnNonDestructibleHit = true;

	//=========================================================================
	// Decal Parameters
	//=========================================================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal")
	bool  bUseDecalSizeOverride = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "bUseDecalSizeOverride", EditConditionHides))
	FVector DecalSizeOverride = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "bUseDecalSizeOverride", EditConditionHides))
	FVector DecalLocationOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "bUseDecalSizeOverride", EditConditionHides))
	FRotator DecalRotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Decal", meta = (EditCondition = "!bUseDecalSizeOverride", EditConditionHides))
	float DecalSizeMultiplier = 1.0f;

	//=========================================================================
	// Decal Editor Value Storage
	//=========================================================================

	UPROPERTY()
	FVector DecalLocationInEditor = FVector::ZeroVector;

	UPROPERTY()
	FRotator DecalRotationInEditor = FRotator::ZeroRotator;

	UPROPERTY()
	FVector DecalScaleInEditor = FVector::OneVector;

	UPROPERTY()
	FVector ToolShapeLocationInEditor = FVector::ZeroVector;

	UPROPERTY()
	FRotator ToolShapeRotationInEditor = FRotator::ZeroRotator;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> DecalMaterialInEditor = nullptr;

	UPROPERTY()
	TObjectPtr<UImpactProfileDataAsset> CachedDecalDataAsset;

	UPROPERTY()
	FName DecalConfig = FName("Default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FName DecalConfigID = FName("Default");


	//=========================================================================
	// Immediate Feedback Settings
	//=========================================================================

	/** Show immediate feedback (before server response) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Feedback")
	bool bShowImmediateFeedback = true;

	//=========================================================================
	// Events
	//=========================================================================

	/** Called when a destruction request is sent */
	UPROPERTY(BlueprintAssignable, Category="Destruction|Events")
	FOnDestructionRequested OnDestructionRequested;

	/** Called when hitting a non-destructible object */
	UPROPERTY(BlueprintAssignable, Category="Destruction|Events")
	FOnNonDestructibleHit OnNonDestructibleHit;

	//=========================================================================
	// Manual Invocation Functions
	//=========================================================================

	/**
	 * Manually send a destruction request.
	 * Use this instead of automatic collision detection.
	 */
	UFUNCTION(BlueprintCallable, Category="Destruction")
	void RequestDestructionManual(const FHitResult& HitResult);

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void RequestDestructionAtLocation(const FVector& Center);

public:
	UFUNCTION(BlueprintCallable, Category="Destruction|Decal")
	void GetCalculateDecalSize(FName SurfaceType,FVector& LocationOffset,  FRotator& RotatorOffset, FVector& SizeOffset) const;

	/** Collision event handler */

	// Blueprint-exposed function that can be directly bound to collision shape's OnComponentHit event
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void ProcessProjectileHit(UPrimitiveComponent* HitComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

	// Wrapper function for line trace usage
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void ProcessDestructionRequestForChunk(URealtimeDestructibleMeshComponent* DestructComp, const FHitResult& Hit);

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void ProcessSphereDestructionRequestForChunk(URealtimeDestructibleMeshComponent* DestructComp, const FVector& ExplosionCenter );

protected:
	virtual void BeginPlay() override;

	//=========================================================================
	// Internal Functions
	//=========================================================================


private:
	bool EnsureToolMesh();

	void SetShapeParameters(FRealtimeDestructionRequest& OutRequest);

	void DrawDebugToolShape(const FVector& Center, const FVector& Direction, const FColor& Color) const;

	void DrawDebugAffetedChunks(const FBox& ChunkBox, const FColor& Color) const;

	void DrawDebugCylinderInternal(const FVector& Center, const FVector& Direction, const FColor& Color) const;

	void DrawDebugSphereInternal(const FVector& Center, const FColor& Color) const;

	FVector GetToolDirection(const FHitResult& Hit, AActor* Owner) const;

	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> ToolMeshPtr = nullptr;


	float SurfaceMargin = 0.0f;
};
