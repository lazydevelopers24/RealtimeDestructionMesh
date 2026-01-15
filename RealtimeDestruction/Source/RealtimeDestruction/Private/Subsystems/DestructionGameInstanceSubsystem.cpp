#include "Subsystems/DestructionGameInstanceSubsystem.h"
#include "Data/DecalMaterialDataAsset.h"

void UDestructionGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 기본 경로가 설정되어 있으면 로드
	if (DefaultDecalDataAssetPath.IsValid())
	{
		DecalDataAsset = Cast<UDecalMaterialDataAsset>(DefaultDecalDataAssetPath.TryLoad());
		if (DecalDataAsset)
		{
			UE_LOG(LogTemp, Log, TEXT("[DestructionSubsystem] DecalDataAsset 로드 성공: %s"), *DecalDataAsset->GetName());
		}
	}
}

void UDestructionGameInstanceSubsystem::Deinitialize()
{
	DecalDataAsset = nullptr;
	Super::Deinitialize();
}
