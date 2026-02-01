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
#include "Components/ActorComponent.h"
#include "DestructionTypes.h"
#include "BulletClusterComponent.generated.h"

class URealtimeDestructibleMeshComponent;

USTRUCT()
struct FPendingClusteringRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FVector ImpactPoint = FVector::ZeroVector;

	UPROPERTY()
	FVector ImpactNormal = FVector::UpVector;

	UPROPERTY()
	float Radius = 10.0f;

	UPROPERTY()
	float ChunkIndex = INDEX_NONE;

	UPROPERTY()
	FVector ToolForwardVector = FVector::ForwardVector;

	UPROPERTY()
	FVector ToolOriginWorld = FVector::ZeroVector;

	UPROPERTY()
	float Depth = 10.0f;
};

/**
 * Component that clusters bullet impact requests to optimize destruction operations.
 *
 * Collects multiple bullet impacts occurring within a short time window,
 * groups nearby impact points into clusters, and processes them as
 * a single large destruction instead of multiple small holes.
 * This reduces the number of Boolean operations and improves performance.
 *
 * [How It Works]
 * 1. Destruction requests are stored in a pending buffer via RegisterRequest()
 * 2. Requests are collected for ClusterWindowTime (default 0.3 seconds) from the first request
 * 3. When the time window expires, nearby impact points are clustered using Union-Find algorithm
 * 4. Only clusters meeting ClusterCountThreshold (default 5) are processed for destruction
 * 5. Actual destruction is performed by calling ApplyOp() on the owner mesh
 *
 * [Usage]
 * This component is automatically created and attached when
 * RealtimeDestructibleMeshComponent enables BulletClustering.
 * For manual use, call SetOwnerMesh() to specify the owner.
 */
UCLASS(ClassGroup = (RealtimeDestruction), meta = (BlueprintSpawnableComponent))
class UBulletClusterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBulletClusterComponent();

	// ===============================================
	// Properties
	// ===============================================

	// Clustering time window
	UPROPERTY()
	float ClusterWindowTime = 0.3f;

	UPROPERTY()
	float MergeDistanceThreshold = 10.0f;

	UPROPERTY()
	float MaxClusterRadius = 20.0f;

	UPROPERTY()
	int ClusterCountThreshold = 5;

	UPROPERTY()
	float ClusterRadiusOffset = 1.0f;

	// ===============================================
	// Functions
	// ===============================================
	void Init(float InMergeDistance, float InMaxCluserRadius, int InClusterCountThreshold, float InClusterRadiusOffset);

	void SetOwnerMesh(URealtimeDestructibleMeshComponent* InOwnerMesh);

	// Register request
	UFUNCTION()
	void RegisterRequest(const FRealtimeDestructionRequest& Request);


protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// Owner mesh
	UPROPERTY()
	TWeakObjectPtr<URealtimeDestructibleMeshComponent> OwnerMesh;

	// Pending requests
	UPROPERTY()
	TArray<FPendingClusteringRequest> PendingRequests;

	// Timer
	FTimerHandle ClusterTimerHandle;
	bool bTimerActive = false;

	// Timer Callback
	void OnClusterWindowExpired();

	// Perform clustering
	TArray<FBulletCluster> ProcessClustering();

	// Execute destruction
	void ExecuteDestruction(const TArray<FBulletCluster>& Clusters);

	// Clear buffer
	void ClearPendingRequests();
};
