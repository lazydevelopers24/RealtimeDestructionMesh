#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "RDMSetting.generated.h"

class UDecalMaterialDataAsset;

UENUM(BlueprintType)
enum class ERDMThreadMode : uint8
{
	Absolute	UMETA(DisplayName = "Absoluite"),
	Percentage	UMETA(DisplayName = "Percentage (%)")
};

USTRUCT()
struct FDecalDataAssetEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	TSoftObjectPtr<UDecalMaterialDataAsset> DataAsset;
	
	UPROPERTY(VisibleAnywhere)
	FName ConfigID;
};

UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Realtime Destructible Mesh"))
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
	mutable TMap<FName, UDecalMaterialDataAsset*> CachedDataAssetMap;

	void BuildCacheIfNeeded() const;
};