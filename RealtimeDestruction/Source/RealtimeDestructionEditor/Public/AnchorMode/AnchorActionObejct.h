// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/NoExportTypes.h"
#include "AnchorActionObejct.generated.h"

class URealtimeDestructibleMeshComponent;

UCLASS()
class REALTIMEDESTRUCTIONEDITOR_API UAnchorActionObejct : public UObject
{
	GENERATED_BODY()
	
public:
	UFUNCTION(CallInEditor, Category = "1. Spawn")
	void SpawnAnchorPlane();
	
	UFUNCTION(CallInEditor, Category = "1. Spawn")
	void SpawnAnchorVolume();

	UFUNCTION(CallInEditor, Category = "2. Apply")
	void ApplyAllAnchorPlanes();
	
	UFUNCTION(CallInEditor, Category = "2. Apply")
	void ApplyAllAnchorVolumes();

	UFUNCTION(CallInEditor, Category = "3. Remove")
	void RemoveAllAnchorPlanes();

	UFUNCTION(CallInEditor, Category = "3. Remove")
	void RemoveAllAnchorVolumes();

	UFUNCTION(CallInEditor, Category = "3. Remove")
	void RemoveAllAnchors();

	UFUNCTION(CallInEditor, Category = "4. Selection")
	void ApplyAnchors();

	UFUNCTION(CallInEditor, Category = "4. Selection")
	void RemoveAnchors();	

	UPROPERTY(VisibleAnywhere, Category = "4. Selection")
	FString SelectedComponentName = "None";

	UPROPERTY(VisibleAnywhere, Category = "4. Selection")
	int32 TotalCellCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "4. Selection")
	int32 ValidCellCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "4. Selection")
	int32 AnchorCellCount = 0;

	UFUNCTION(CallInEditor, Category = "4. Selection")
	void BuildGridCellsForSelection();

	void UpdateSelectionFromEditor(UWorld* World);

	void UpdateCellCounts();

	void SetTargetComponent(URealtimeDestructibleMeshComponent* InComp) { TargetComp = InComp; }
	
	TObjectPtr<URealtimeDestructibleMeshComponent> TargetComp = nullptr;
};
