#include "Subsystems/DestructionGameInstanceSubsystem.h"
#include "Data/DecalMaterialDataAsset.h"
#include "Settings/RDMSetting.h"

void UDestructionGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

    // RDMSetting에서 Data Asset들 자동 등록
    URDMSetting* Settings = URDMSetting::Get();
    if (Settings)
    {
        for (const FDecalDataAssetEntry& Entry : Settings->DecalDataAssets)
        {
            if (UDecalMaterialDataAsset* Asset = Entry.DataAsset.LoadSynchronous())
            {
                RegisterDecalDataAsset(Asset);
            }
        }
    }

}

void UDestructionGameInstanceSubsystem::Deinitialize()
{
    DecalDataAssetMap.Empty();
	Super::Deinitialize();
}

void UDestructionGameInstanceSubsystem::RegisterDecalDataAsset(UDecalMaterialDataAsset* InAsset)
{
	if (InAsset && !InAsset->ConfigID.IsNone())
	{
		DecalDataAssetMap.Add(InAsset->ConfigID, InAsset);
	}
}

void UDestructionGameInstanceSubsystem::UnregisterDecalDataAsset(FName ConfigID)
{
      DecalDataAssetMap.Remove(ConfigID);
}

UDecalMaterialDataAsset* UDestructionGameInstanceSubsystem::FindDataAssetByConfigID(FName ConfigID) const
{
	const TObjectPtr<UDecalMaterialDataAsset> * Found = DecalDataAssetMap.Find(ConfigID);
	return Found ? Found->Get() : nullptr;
}

void UDestructionGameInstanceSubsystem::RenameConfigID(FName OldConfigID, FName NewConfigID)
{
	if (OldConfigID == NewConfigID || OldConfigID.IsNone() || NewConfigID.IsNone())
	{
		return;
	}

	// 기존 Key로 Data Asset 찾기
	TObjectPtr<UDecalMaterialDataAsset>* FoundAsset = DecalDataAssetMap.Find(OldConfigID);
	if (FoundAsset && *FoundAsset)
	{
		UDecalMaterialDataAsset* Asset = *FoundAsset;

		// 기존 Key 제거 후 새 Key로 등록
		DecalDataAssetMap.Remove(OldConfigID);
		DecalDataAssetMap.Add(NewConfigID, Asset);
	}
}
