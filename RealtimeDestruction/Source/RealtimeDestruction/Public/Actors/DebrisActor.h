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
#include "GameFramework/Actor.h"
#include "DebrisActor.generated.h"

// Forward declarations
namespace UE { namespace Geometry { class FDynamicMesh3; } }
class UProceduralMeshComponent;
class URealtimeDestructibleMeshComponent;
class UBoxComponent;

UCLASS()
class REALTIMEDESTRUCTION_API ADebrisActor : public AActor
{
	GENERATED_BODY()

public:
	ADebrisActor();

	// Components
	// Root: BoxComponent (handles physics)
	// ProceduralMesh handles rendering only
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debris")
	TObjectPtr<UBoxComponent> CollisionBox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debris")
	TObjectPtr<UProceduralMeshComponent> DebrisMesh;

	// Replicated Properties

	/** Unique ID for matching */
	UPROPERTY(ReplicatedUsing = OnRep_DebrisParams)
	int32 DebrisId;

	/** Local CellIds (not replicated, stores decoded result) */
	UPROPERTY()
	TArray<int32> CellIds;

	/** Compressed cell bounding box Min (for replication) */
	UPROPERTY(Replicated)
	FIntVector CellBoundsMin;

	/** Compressed cell bounding box Max (for replication) */
	UPROPERTY(Replicated)
	FIntVector CellBoundsMax;

	/** Compressed cell bitmap (for replication) */
	UPROPERTY(Replicated)
	TArray<uint8> CellBitmap;
	
	/** Owner actor of source mesh (required for generating debris from CellIDs) */
	UPROPERTY(Replicated)
	TObjectPtr<AActor> SourceMeshOwner;

	/** Source chunk index */
	UPROPERTY(Replicated)
	int32 SourceChunkIndex;

	/** Material */
	UPROPERTY(Replicated)
	TObjectPtr<UMaterialInterface> DebrisMaterial;

	// Settings
	UPROPERTY(EditDefaultsOnly, Category = "Debris")
	float DebrisLifetime;


	// public Methods
	
	/** Server-only: Initialize debris */
	void InitializeDebris(int32 InDebrisId, const TArray<int32>& InCellIds, int32 InChunkIndex, URealtimeDestructibleMeshComponent* InSourcMesh, UMaterialInterface* InMaterial);

	/** Server-only: Enable physics */
	void EnablePhysics();

	/** Apply local mesh data (called from client) */
	void ApplyLocalMesh(UProceduralMeshComponent* LocalMesh);

	/** Set box collision extent */
	void SetCollisionBoxExtent(const FVector& Extent);

	/** Server-only: Set mesh directly using pre-generated mesh data */
	void SetMeshDirectly(const TArray<FVector>& Vertices,
		const TArray<int32>& Triangles,
		const TArray<FVector>& Normals,
		const TArray<FVector2D>& UVs);

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_DebrisParams();
	
private:
	/** Find local mesh by DebrisId */
	UProceduralMeshComponent* FindLocalDebrisMesh(int32 InDebrisId);

	/** Generate mesh from CellIds (fallback) */
	void GenerateMeshFromCells();

	/** Get component from SourceMeshOwner */
	URealtimeDestructibleMeshComponent* GetSourceMeshComponent() const;

	/** Lifetime expiration callback */
	void OnLifetimeExpired();

	/** Encode CellIds to bitmap (called from server) */
	void EncodeCellsToBitmap(const TArray<int32>& InCellIds, const struct FGridCellLayout& GridLayout);

	/** Decode bitmap to CellIds (called from client) */
	void DecodeBitmapToCells(const struct FGridCellLayout& GridLayout);

	bool bMeshReady;
};