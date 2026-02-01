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
#include "Engine/DataAsset.h"
#include "Materials/MaterialInterface.h"
#include "Components/DestructionTypes.h"
#include "ImpactProfileDataAsset.generated.h"
 
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FImpactProfileConfig
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FString VariantName;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	TObjectPtr<UMaterialInterface> DecalMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FVector DecalSize = FVector(1.0f, 10.0f, 10.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FVector LocationOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FRotator RotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	bool bRandomDecalRotation = true;

	// Tool Shape
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	EDestructionToolShape ToolShape = EDestructionToolShape::Cylinder;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	float CylinderRadius = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	float CylinderHeight = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	float SphereRadius = 10.0f;

	// 유효성 검사 함수 (기존에 있다면 유지)
	bool IsValid() const { return DecalMaterial != nullptr; } 
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FImpactProfileConfigArray
{
	GENERATED_BODY()

	/** 해당 surface에서 사용 가능한 impact profile 목록 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ImpactProfile")
	TArray<FImpactProfileConfig> Configs;

	/** 랜덤 선택 */
	const FImpactProfileConfig* GetRandom() const
	{
		if (Configs.Num() == 0)
		{
			return nullptr;
		}

		if (Configs.Num() == 1)
		{
			return &Configs[0];
		}

		int32 Index;
		Index = FMath::RandRange(0, Configs.Num() - 1);

		return &Configs[Index]; 
	}

	/** 개수 출력 */
	int32 Num() const { return Configs.Num(); }

	/** 유효성 검사 */
	bool IsValid() const { return (Configs.Num() > 0) && (Configs[0].IsValid()); }
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FProjectileImpactConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ImpactProfile")
	FName ConfigID = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ImpactProfile")
	TMap<FName,	FImpactProfileConfigArray> SurfaceConfigs;
};

UCLASS(ClassGroup = (RealtimeDestruction), BlueprintType)
class REALTIMEDESTRUCTION_API UImpactProfileDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	
private:
	// 변경 전 ConfigID를 임시 저장
	FName CachedConfigIDBeforeEdit;
#endif

public:  
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Decal")
	FName ConfigID = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ImpactProfile")
	TMap<FName,	FImpactProfileConfigArray> SurfaceConfigs;

public:
	UFUNCTION(BlueprintCallable, Category = "ImpactProfile")
	bool GetConfig( FName SurfaceType, int32 VariantIndex, FImpactProfileConfig& OutConfig ) const;

	UFUNCTION(BlueprintCallable, Category = "ImpactProfile")
	bool GetConfigRandom( FName SurfaceType, FImpactProfileConfig& OutConfig ) const;

	/** 보유하고 있는 Key의 수 */
	UFUNCTION(BlueprintCallable, Category = "ImpactProfile")
	int32 GetSurfaceConfigCount() { return SurfaceConfigs.Num(); };
	

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	FName CurrentEditingKey = NAME_None;
	
	UPROPERTY()
	FVector ToolShapeLocationInEditor = FVector::ZeroVector;
	
	UPROPERTY()
	FRotator ToolShapeRotationInEditor = FRotator::ZeroRotator;

	UPROPERTY()
	float SphereRadiusInEditor = 10.0f;

	UPROPERTY()
	float CylinderRadiusInEditor = 10.0f;
	
	UPROPERTY()
	float CylinderHeightInEditor = 10.0f;

	UPROPERTY()
	TSoftObjectPtr<UStaticMesh> PreviewMeshInEditor = nullptr;

	UPROPERTY()
	FVector PreviewMeshLocationInEditor = FVector::ZeroVector;

	UPROPERTY()
	FRotator PreviewMeshRotationInEditor = FRotator::ZeroRotator;
	
	UPROPERTY()
	FVector PreviewMeshScaleInEditor = FVector::OneVector;
	
#endif
};
