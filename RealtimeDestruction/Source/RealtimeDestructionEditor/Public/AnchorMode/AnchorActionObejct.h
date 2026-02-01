// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/NoExportTypes.h"
#include "AnchorActionObejct.generated.h"

class URealtimeDestructibleMeshComponent;
class AAnchorActor;

UCLASS(ClassGroup = (RealtimeDestructionEditor))
class REALTIMEDESTRUCTIONEDITOR_API UAnchorActionObejct : public UObject
{
	GENERATED_BODY()
	
public:
	UFUNCTION(CallInEditor, Category = "1. Spawn", meta = (DisplayPriority = "1"))
	void SpawnAnchorPlane();
	
	UFUNCTION(CallInEditor, Category = "1. Spawn", meta = (DisplayPriority = "2"))
	void SpawnAnchorVolume();

	UFUNCTION(CallInEditor, Category = "2. Apply", meta = (DisplayPriority = "1"))
	void ApplyAllAnchorPlanes();
	
	UFUNCTION(CallInEditor, Category = "2. Apply", meta = (DisplayPriority = "2"))
	void ApplyAllAnchorVolumes();

	UFUNCTION(CallInEditor, Category = "3. Remove", meta = (DisplayPriority = "1"))
	void RemoveAllAnchorPlanes();

	UFUNCTION(CallInEditor, Category = "3. Remove", meta = (DisplayPriority = "2"))
	void RemoveAllAnchorVolumes();

	UFUNCTION(CallInEditor, Category = "3. Remove", meta = (DisplayPriority = "3"))
	void RemoveAllAnchors();

	UFUNCTION(CallInEditor, Category = "4. Selection", meta = (DisplayPriority = "1"))
	void ApplyAnchors();

	UFUNCTION(CallInEditor, Category = "4. Selection", meta = (DisplayPriority = "2"))
	void RemoveAnchors();	

	UPROPERTY(VisibleAnywhere, Category = "4. Selection",  meta = (DisplayPriority = "3"))
	FString SelectedComponentName = "None";

	UPROPERTY(VisibleAnywhere, Category = "4. Selection",  meta = (DisplayPriority = "4"))
	int32 TotalCellCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "4. Selection",  meta = (DisplayPriority = "5"))
	int32 ValidCellCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "4. Selection",  meta = (DisplayPriority = "6"))
	int32 AnchorCellCount = 0;

	UFUNCTION(CallInEditor, Category = "4. Selection")
	void BuildGridCellsForSelection();

	void UpdateSelectionFromEditor(UWorld* World);

	void UpdateCellCounts();

	void SetTargetComponent(URealtimeDestructibleMeshComponent* InComp) { TargetComp = InComp; }
	
	void ValidateAnchorArray();

	void CollectionExistingAnchorActors(UWorld* World);
	
	TObjectPtr<URealtimeDestructibleMeshComponent> TargetComp = nullptr;

	TArray<TWeakObjectPtr<AAnchorActor>> AnchorActors;
};
