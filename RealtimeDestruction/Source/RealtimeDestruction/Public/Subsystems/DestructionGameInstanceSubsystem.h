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

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void RegisterDecalDataAsset(UDecalMaterialDataAsset* InAsset);
	
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void UnregisterDecalDataAsset(FName ConfigID);

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	UDecalMaterialDataAsset* FindDataAssetByConfigID(FName ConfigID) const;

	/** ConfigID 변경 시 Map의 Key도 업데이트 */
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void RenameConfigID(FName OldConfigID, FName NewConfigID);
	
private:
	UPROPERTY()
      TMap<FName, TObjectPtr<UDecalMaterialDataAsset>> DecalDataAssetMap;
};
