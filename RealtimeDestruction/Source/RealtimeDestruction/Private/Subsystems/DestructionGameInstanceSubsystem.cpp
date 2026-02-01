// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Subsystems/DestructionGameInstanceSubsystem.h"
#include "Data/ImpactProfileDataAsset.h"
#include "Settings/RDMSetting.h"

void UDestructionGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

    // RDMSetting에서 Data Asset들 자동 등록
    URDMSetting* Settings = URDMSetting::Get();
    if (Settings)
    {
        for (const FImpactProfileDataAssetEntry& Entry : Settings->ImpactProfiles)
        {
            if (UImpactProfileDataAsset* Asset = Entry.DataAsset.LoadSynchronous())
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

void UDestructionGameInstanceSubsystem::RegisterDecalDataAsset(UImpactProfileDataAsset* InAsset)
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

UImpactProfileDataAsset* UDestructionGameInstanceSubsystem::FindDataAssetByConfigID(FName ConfigID) const
{
	const TObjectPtr<UImpactProfileDataAsset> * Found = DecalDataAssetMap.Find(ConfigID);
	return Found ? Found->Get() : nullptr;
}

void UDestructionGameInstanceSubsystem::RenameConfigID(FName OldConfigID, FName NewConfigID)
{
	if (OldConfigID == NewConfigID || OldConfigID.IsNone() || NewConfigID.IsNone())
	{
		return;
	}

	// 기존 Key로 Data Asset 찾기
	TObjectPtr<UImpactProfileDataAsset>* FoundAsset = DecalDataAssetMap.Find(OldConfigID);
	if (FoundAsset && *FoundAsset)
	{
		UImpactProfileDataAsset* Asset = *FoundAsset;

		// 기존 Key 제거 후 새 Key로 등록
		DecalDataAssetMap.Remove(OldConfigID);
		DecalDataAssetMap.Add(NewConfigID, Asset);
	}
}
