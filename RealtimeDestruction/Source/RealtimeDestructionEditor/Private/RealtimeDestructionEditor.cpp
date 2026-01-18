// Copyright Epic Games, Inc. All Rights Reserved.

#include "RealtimeDestructionEditor.h"

#include "DecalMaterialDataAssetDetails.h"
#include "RealtimeDestructibleMeshComponentDetails.h"
#include "Components/DestructionProjectileComponent.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "DestructionProjectileComponentVisualizer.h"
#include "DecalSizeEditorWindow.h"
#include "DestructionProjectileComponentDetails.h"
#include "PropertyEditorModule.h"
#include "Data/DecalMaterialDataAsset.h" 

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
		UDecalMaterialDataAsset::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FDecalMaterialDataAssetDetails::MakeInstance)
	);

	PropertyModule.RegisterCustomClassLayout(
		URealtimeDestructibleMeshComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FRealtimeDestructibleMeshComponentDetails::MakeInstance)
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
		PropertyModule.UnregisterCustomClassLayout(UDecalMaterialDataAsset::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomClassLayout(URealtimeDestructibleMeshComponent::StaticClass()->GetFName());
	} 

	UE_LOG(LogTemp, Log, TEXT("RealtimeDestructionEditor module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRealtimeDestructionEditorModule, RealtimeDestructionEditor)
