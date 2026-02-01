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
#include "Engine/DeveloperSettings.h"
#include "RDMSetting.generated.h"

class UImpactProfileDataAsset;

UENUM(BlueprintType)
enum class ERDMThreadMode : uint8
{
	Absolute	UMETA(DisplayName = "Absolute"),
	Percentage	UMETA(DisplayName = "Percentage (%)")
};

USTRUCT()
struct FImpactProfileDataAssetEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "DecalEntry")
	TSoftObjectPtr<UImpactProfileDataAsset> DataAsset;

	UPROPERTY(VisibleAnywhere, Category = "DecalEntry")
	FName ConfigID;
};

UCLASS(ClassGroup = (RealtimeDestruction), config = Game, defaultconfig, meta = (DisplayName = "Realtime Destructible Mesh"))
class REALTIMEDESTRUCTION_API URDMSetting : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	URDMSetting();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	
	static URDMSetting* Get();

	///** Sync Entry's ConfigID when ConfigID changed*/
	void UpdateEntryConfigID(FName OldConfigID, FName NewConfigID);
	
	virtual FName GetCategoryName() const override {return TEXT("Plugins"); }
	
	virtual FName GetSectionName() const override { return TEXT("Realtime Destructible Mesh"); }
	
public:
	UPROPERTY(config, EditAnywhere, Category = "Thread Settings" , meta = (DisplayName =  "Thread Mode"))
	ERDMThreadMode ThreadMode = ERDMThreadMode::Absolute; 

	// Absolute Mode
	UPROPERTY(config ,EditAnywhere, Category = "Thread Settings", meta = (DisplayName = "Number Of Threads To Use", ClampMin= "1", ClampMax = "64",
		EditCondition = "ThreadMode == ERDMThreadMode::Absolute", EditConditionHides))
	int32 MaxThreadCount = 8;

	// Percentage Mode
	UPROPERTY(config, EditAnywhere, Category = "Thread Settings", meta = (DisplayName = "Thread Usage Rate", ClampMin ="0", ClampMax = "100", UIMin ="0", UIMax ="100",
		EditCondition = "ThreadMode == ERDMThreadMode::Percentage", EditConditionHides))
	int32 ThreadPercentage = 50;

	// Returns calculated available threads depends on thread mode
	int32 GetEffectiveThreadCount() const ;
	
	// Returns system total threads
	static int32 GetSystemThreadCount();

public:
	UPROPERTY(config, EditAnywhere, Category = "Impact Profile Settings")
	TArray<FImpactProfileDataAssetEntry> ImpactProfiles;

	UFUNCTION(BlueprintCallable, Category = "Impact Profile")
	UImpactProfileDataAsset* GetImpactProfileDataAsset(FName ConfigID) const;

private:
	UPROPERTY(Transient)
	mutable TMap<FName, TObjectPtr<UImpactProfileDataAsset>> CachedDataAssetMap;

	void BuildCacheIfNeeded() const;
};