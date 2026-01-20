#include "Settings/RDMSetting.h"
#include "HAL/PlatformMisc.h"
#include "Misc/TypeContainer.h"
#include "Data/DecalMaterialDataAsset.h"

URDMSetting::URDMSetting()
{
	ThreadMode = ERDMThreadMode::Absolute;

	MaxThreadCount = 8;
	ThreadPercentage = 50;
}

URDMSetting* URDMSetting::Get()
{
	return GetMutableDefault<URDMSetting>();
}

#if WITH_EDITOR
void URDMSetting::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// DecalDataAssets 배열이 변경되면 각 Entry의 ConfigID를 Data Asset에서 동기화
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(URDMSetting, DecalDataAssets))
	{
		for (FDecalDataAssetEntry& Entry : DecalDataAssets)
		{
			if (UDecalMaterialDataAsset* LoadedAsset = Entry.DataAsset.LoadSynchronous())
			{
				Entry.ConfigID = LoadedAsset->ConfigID;
			}
			else
			{
				Entry.ConfigID = NAME_None;
			}
		}

		// 캐시 무효화
		CachedDataAssetMap.Empty();
	}
}
#endif

void URDMSetting::UpdateEntryConfigID(FName OldConfigID, FName NewConfigID)
{
	for (FDecalDataAssetEntry& Entry : DecalDataAssets)
	{
		if (Entry.ConfigID == OldConfigID)
		{
			Entry.ConfigID = NewConfigID;
			break;
		}
	}

	// 캐시 무효화
	CachedDataAssetMap.Empty();
}

int32 URDMSetting::	GetEffectiveThreadCount() const
{
	if (ThreadMode == ERDMThreadMode::Absolute)
	{
		return FMath::Clamp(MaxThreadCount, 1, GetSystemThreadCount());
	}
	else
	{
		int32 SystemThreads = GetSystemThreadCount();
		int32 CalculatedThreads = FMath::CeilToInt(SystemThreads *ThreadPercentage / 100.0f);
		return FMath::Clamp(CalculatedThreads, 1, SystemThreads);
	}
}

int32 URDMSetting::GetSystemThreadCount()
{
	return FPlatformMisc::NumberOfCoresIncludingHyperthreads();
}

UDecalMaterialDataAsset* URDMSetting::GetDecalDataAsset(FName ConfigID) const
{
	BuildCacheIfNeeded();

	if (auto* Found = CachedDataAssetMap.Find(ConfigID))
	{
		return *Found;
	}

	return nullptr;
}

void URDMSetting::BuildCacheIfNeeded() const
{
	if (CachedDataAssetMap.Num() == DecalDataAssets.Num() && DecalDataAssets.Num() >  0)
	{
		return;
	}

	CachedDataAssetMap.Empty();

	for (const FDecalDataAssetEntry& Entry : DecalDataAssets)
	{
		if (UDecalMaterialDataAsset* LoadedAsset = Entry.DataAsset.LoadSynchronous())
		{
			CachedDataAssetMap.Add(LoadedAsset->ConfigID, LoadedAsset);
		}
	}
}


