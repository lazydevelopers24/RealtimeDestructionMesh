#include "Data/DecalMaterialDataAsset.h" 

bool UDecalMaterialDataAsset::GetConfig(FName ConfigID, FName SurfaceType, int32 VariantIndex,
	FDecalSizeConfig& OutConfig) const
{
	const FProjectileDecalConfig* ProjectileConfig = FindProjectileConfig(ConfigID);

	if (!ProjectileConfig)
	{
		return false;	
	}

	const FDecalSizeConfigArray* FoundArray = ProjectileConfig->SurfaceConfigs.Find(SurfaceType);
	
	if (!FoundArray && SurfaceType != "Default")
	{
		FoundArray = ProjectileConfig->SurfaceConfigs.Find("Default");
	}

	if (!FoundArray || FoundArray->Configs.Num() == 0)
	{
		return false;
	}

	// VariantIndex 범위 체크
	int32 SafeIndex = FMath::Clamp(VariantIndex, 0, FoundArray->Configs.Num() - 1);
	OutConfig = FoundArray->Configs[SafeIndex];
	return true;
	
}

bool UDecalMaterialDataAsset::GetConfigRandom(FName ConfigID, FName SurfaceType, FDecalSizeConfig& OutConfig) const
{

	// ConfigID로 못찾으면 return false
    const FProjectileDecalConfig* ProjectileConfig = FindProjectileConfig(ConfigID);
	if (!ProjectileConfig)
	{
		return false;
	}

	// SurfaceType으로 DecalConfig 찾기
	if ( const FDecalSizeConfigArray* FoundArray = ProjectileConfig->SurfaceConfigs.Find(SurfaceType))
	{
		const FDecalSizeConfig* Selected = FoundArray->GetRandom();
		if (Selected)
		{
			OutConfig = *Selected;
			return true;
		}
	}

	// DecalConfig 못 찾았으면 default 값을 할당 시도
	if (SurfaceType != "Default")
	{
		if (const FDecalSizeConfigArray* DefaultConfig = ProjectileConfig->SurfaceConfigs.Find("Default"))
		{
			const FDecalSizeConfig* Selected = DefaultConfig->GetRandom();
			if (Selected)
			{ 
				OutConfig = *Selected;
				return true;
			}
		}
	}

	return false;
}

const FProjectileDecalConfig* UDecalMaterialDataAsset::FindProjectileConfig(FName ConfigID) const
{
	for (const FProjectileDecalConfig& Config : ProjectileConfigs)
      {
          if (Config.ConfigID == ConfigID)
          {
              return &Config;
          }
      }
      return nullptr;
}

TArray<FName> UDecalMaterialDataAsset::GetAllConfigIDs() const
{  
	TArray<FName> Result;
	for (const FProjectileDecalConfig& Config : ProjectileConfigs)
	{
		Result.Add(Config.ConfigID);
	}
	return Result;
}
  