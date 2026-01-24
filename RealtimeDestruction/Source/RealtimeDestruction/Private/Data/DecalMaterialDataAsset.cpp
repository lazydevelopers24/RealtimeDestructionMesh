// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Data/DecalMaterialDataAsset.h"

#if WITH_EDITOR

#include "Settings/RDMSetting.h"
void UDecalMaterialDataAsset::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);

	// Config ID 변경 전에 현재 값 저장
	if (PropertyAboutToChange &&
		PropertyAboutToChange->GetFName() == GET_MEMBER_NAME_CHECKED(UDecalMaterialDataAsset, ConfigID))
	{
		CachedConfigIDBeforeEdit = ConfigID;
	}
}

void UDecalMaterialDataAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UDecalMaterialDataAsset, ConfigID))
	{
		// 값이 실제로 변경되었는지 확인
		if (!CachedConfigIDBeforeEdit.IsNone() && CachedConfigIDBeforeEdit != ConfigID)
		{
			// Project Settings 업데이트
			if (URDMSetting* Settings = URDMSetting::Get())
			{
				Settings->UpdateEntryConfigID(CachedConfigIDBeforeEdit, ConfigID);
			}
		}

		CachedConfigIDBeforeEdit = NAME_None; 
	}
}

#endif

bool UDecalMaterialDataAsset::GetConfig( FName SurfaceType, int32 VariantIndex,
	FDecalSizeConfig& OutConfig) const
{ 

	const FDecalSizeConfigArray* FoundArray = SurfaceConfigs.Find(SurfaceType);
	
	if (!FoundArray && SurfaceType != "Default")
	{
		FoundArray = SurfaceConfigs.Find("Default");
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

bool UDecalMaterialDataAsset::GetConfigRandom( FName SurfaceType, FDecalSizeConfig& OutConfig) const
{ 
	// SurfaceType으로 DecalConfig 찾기
	if ( const FDecalSizeConfigArray* FoundArray = SurfaceConfigs.Find(SurfaceType))
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
		if (const FDecalSizeConfigArray* DefaultConfig = SurfaceConfigs.Find("Default"))
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
  