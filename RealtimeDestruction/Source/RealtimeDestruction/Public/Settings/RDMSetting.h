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

class UDecalMaterialDataAsset;

UENUM(BlueprintType)
enum class ERDMThreadMode : uint8
{
	Absolute	UMETA(DisplayName = "Absolute"),
	Percentage	UMETA(DisplayName = "Percentage (%)")
};

USTRUCT()
struct FDecalDataAssetEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "DecalEntry")
	TSoftObjectPtr<UDecalMaterialDataAsset> DataAsset;

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

	//Setting 접근자
	static URDMSetting* Get();

	///** ConfigID 변경 시 Entry의 ConfigID도 업데이트 */
	void UpdateEntryConfigID(FName OldConfigID, FName NewConfigID);

	// 카테고리 설정
	virtual FName GetCategoryName() const override {return TEXT("Plugins"); }

	// 섹션 이름
	virtual FName GetSectionName() const override { return TEXT("Realtime Destructible Mesh"); }
	
public:
	// thread 설정 모드
	UPROPERTY(config, EditAnywhere, Category = "Thread Settings" , meta = (DisplayName =  "Thread Mode"))
	ERDMThreadMode ThreadMode = ERDMThreadMode::Absolute; 

	// Absolute Mode
	UPROPERTY(config ,EditAnywhere, Category = "Thread Settings", meta = (DisplayName = "Number Of Threads To Use", ClampMin= "1", ClampMax = "64",
		EditCondition = "ThreadMode == ERDMThreadMode::Absolute", EditConditionHides))
	int32 MaxThreadCount = 8;

	// Percetage Mode
	UPROPERTY(config, EditAnywhere, Category = "Thread Settings", meta = (DisplayName = "Thread Usage Rate", ClampMin ="0", ClampMax = "100", UIMin ="0", UIMax ="100",
		EditCondition = "ThreadMode == ERDMThreadMode::Percentage", EditConditionHides))
	int32 ThreadPercentage = 50;

	// 계산된 실제 thread 수 반환
	int32 GetEffectiveThreadCount() const ;
	
	// 시스템 thread 수 반환
	static int32 GetSystemThreadCount();

public:
	UPROPERTY(config, EditAnywhere, Category = "Decal Settings")
	TArray<FDecalDataAssetEntry> DecalDataAssets;

	UFUNCTION(BlueprintCallable, Category = "Decal")
	UDecalMaterialDataAsset* GetDecalDataAsset(FName ConfigID) const;

private:
	UPROPERTY(Transient)
	mutable TMap<FName, TObjectPtr<UDecalMaterialDataAsset>> CachedDataAssetMap;

	void BuildCacheIfNeeded() const;
};