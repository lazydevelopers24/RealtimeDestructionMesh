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
#include "Subsystems/GameInstanceSubsystem.h"
#include "DestructionGameInstanceSubsystem.generated.h"

class UImpactProfileDataAsset;

/**
 * GameInstance Subsystem for Destruction System
 * Manages global settings such as DecalDataAsset
 */
UCLASS(ClassGroup = (RealtimeDestruction))
class REALTIMEDESTRUCTION_API UDestructionGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void RegisterDecalDataAsset(UImpactProfileDataAsset* InAsset);
	
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void UnregisterDecalDataAsset(FName ConfigID);

	UFUNCTION(BlueprintCallable, Category = "Destruction")
	UImpactProfileDataAsset* FindDataAssetByConfigID(FName ConfigID) const;

	/** Updates the Map key when ConfigID changes */
	UFUNCTION(BlueprintCallable, Category = "Destruction")
	void RenameConfigID(FName OldConfigID, FName NewConfigID);
	
private:
	UPROPERTY()
      TMap<FName, TObjectPtr<UImpactProfileDataAsset>> DecalDataAssetMap;
};
