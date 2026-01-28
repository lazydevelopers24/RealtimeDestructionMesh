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
	// Root: BoxComponent(물리 담당)
	// ProceduralMesh는 Rendering 담당
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debris")
	TObjectPtr<UBoxComponent> CollisionBox;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Debris")
	TObjectPtr<UProceduralMeshComponent> DebrisMesh;

	// Replicated Properties

	/** 매칭용 고유 ID */
	UPROPERTY(ReplicatedUsing = OnRep_DebrisParams)
	int32 DebrisId;

	/** 로컬용 CellIds (복제 안 함, 디코딩 결과 저장) */
	UPROPERTY()
	TArray<int32> CellIds;

	/** 압축된 셀 바운딩 박스 Min (복제용) */
	UPROPERTY(Replicated)
	FIntVector CellBoundsMin;

	/** 압축된 셀 바운딩 박스 Max (복제용) */
	UPROPERTY(Replicated)
	FIntVector CellBoundsMax;

	/** 압축된 셀 비트맵 (복제용) */
	UPROPERTY(Replicated)
	TArray<uint8> CellBitmap;


	/** 원본 메시 소유 Actor (CellID로 debirs 생성할 때 필요) */
	UPROPERTY(Replicated)
	TObjectPtr<AActor> SourceMeshOwner;

	/** 원본 청크 인덱스 */
	UPROPERTY(Replicated)
	int32 SourceChunkIndex;
	
	/** 머티리얼 */
	UPROPERTY(Replicated)
	TObjectPtr<UMaterialInterface> DebrisMaterial;

	// Settings
	UPROPERTY(EditDefaultsOnly, Category = "Debris")
	float DebrisLifetime;


	// public Methods
	
	/** 서버에서 호출: Debris 초기화 */
	void InitializeDebris(int32 InDebrisId, const TArray<int32>& InCellIds, int32 InChunkIndex, URealtimeDestructibleMeshComponent* InSourcMesh, UMaterialInterface* InMaterial);

	/** 서버에서 호출: 물리 활성화 */
	void EnablePhysics();

	/** 로컬 메시 데이터를 적용 (클라이언트에서 호출) */
	void ApplyLocalMesh(UProceduralMeshComponent* LocalMesh);

	/** Box Collision 크기 설정 */
	void SetCollisionBoxExtent(const FVector& Extent);
	
	/** 서버에서 호출: 직접 메시 설정 (이미 생성된 메시 데이터 사용) */
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
	/** DebrisId로 로컬 메쉬 찾기 */
	UProceduralMeshComponent* FindLocalDebrisMesh(int32 InDebrisId);

	/** CellIds로 메시 생성 (fallback) */
	void GenerateMeshFromCells();

	/** SourceMeshOwner에서 컴포넌트 가져오기 */
	URealtimeDestructibleMeshComponent* GetSourceMeshComponent() const;

	/** 수명 타이머 */
	void OnLifetimeExpired();

	/** CellIds를 비트맵으로 인코딩 (서버에서 호출) */
	void EncodeCellsToBitmap(const TArray<int32>& InCellIds, const struct FGridCellLayout& GridLayout);

	/** 비트맵을 CellIds로 디코딩 (클라이언트에서 호출) */
	void DecodeBitmapToCells(const struct FGridCellLayout& GridLayout);

	bool bMeshReady;
};