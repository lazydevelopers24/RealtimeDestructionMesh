#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DestructionGameInstanceSubsystem.generated.h"

class UDecalMaterialDataAsset;

/**
 * 파괴 시스템용 GameInstance Subsystem
 * DecalDataAsset 등 전역 설정을 관리
 */
UCLASS()
class REALTIMEDESTRUCTION_API UDestructionGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** DecalDataAsset 설정 */
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void SetDecalDataAsset(UDecalMaterialDataAsset* InAsset) { DecalDataAsset = InAsset; }

	/** DecalDataAsset 조회 */
	UFUNCTION(BlueprintPure, Category = "Destruction")
	UDecalMaterialDataAsset* GetDecalDataAsset() const { return DecalDataAsset; }

	/** 기본 DecalDataAsset 경로 (에디터에서 설정 가능) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Destruction")
	FSoftObjectPath DefaultDecalDataAssetPath;

private:
	UPROPERTY()
	TObjectPtr<UDecalMaterialDataAsset> DecalDataAsset = nullptr;
};
