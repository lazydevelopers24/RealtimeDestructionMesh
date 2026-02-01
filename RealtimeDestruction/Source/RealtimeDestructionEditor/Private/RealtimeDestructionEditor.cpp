// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "RealtimeDestructionEditor.h"

#include "ImpactProfileAssetDetails.h"
#include "RealtimeDestructibleMeshComponentDetails.h"
#include "Components/DestructionProjectileComponent.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "DestructionProjectileComponentVisualizer.h"
#include "ImpactProfileEditorWindow.h"
#include "DestructionProjectileComponentDetails.h"
#include "PropertyEditorModule.h"
#include "Data/ImpactProfileDataAsset.h" 
#include"RDMSettingsCustomization.h"
#include "Settings/RDMSetting.h"

#define LOCTEXT_NAMESPACE "FRealtimeDestructionEditorModule"

void FRealtimeDestructionEditorModule::StartupModule()
{
	if (GUnrealEd)
	{
		TSharedPtr<FDestructionProjectileComponentVisualizer> Visualizer = MakeShareable(new FDestructionProjectileComponentVisualizer);

		GUnrealEd->RegisterComponentVisualizer(
			UDestructionProjectileComponent::StaticClass()->GetFName(),
			Visualizer
		);

		Visualizer->OnRegister();
	}
 
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(
		UDestructionProjectileComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FDestructionProjectileComponentDetails::MakeInstance)
	);
 
	PropertyModule.RegisterCustomClassLayout(
		UImpactProfileDataAsset::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FImpactProfileAssetDetails::MakeInstance)
	);

	PropertyModule.RegisterCustomClassLayout(
		URealtimeDestructibleMeshComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FRealtimeDestructibleMeshComponentDetails::MakeInstance)
	);

	PropertyModule.RegisterCustomClassLayout(
		URDMSetting::StaticClass()->GetFName(),
          FOnGetDetailCustomizationInstance::CreateStatic(&FRdmSettingsCustomization::MakeInstance)
	);
	UE_LOG(LogTemp, Log, TEXT("RealtimeDestructionEditor module started"));
}

void FRealtimeDestructionEditorModule::ShutdownModule()
{
	if (GUnrealEd)
	{
		GUnrealEd->UnregisterComponentVisualizer(
			UDestructionProjectileComponent::StaticClass()->GetFName()
		);
	}
 
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UDestructionProjectileComponent::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomClassLayout(UImpactProfileDataAsset::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomClassLayout(URealtimeDestructibleMeshComponent::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomClassLayout(URDMSetting::StaticClass()->GetFName());
	} 

	UE_LOG(LogTemp, Log, TEXT("RealtimeDestructionEditor module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRealtimeDestructionEditorModule, RealtimeDestructionEditor)
