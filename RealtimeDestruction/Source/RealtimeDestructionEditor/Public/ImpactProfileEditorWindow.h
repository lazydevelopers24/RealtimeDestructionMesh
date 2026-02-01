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
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Misc/NotifyHook.h"

class UDestructionProjectileComponent;
class SImpactProfileEditorViewport;
class IDetailsView;

struct FImpactProfileConfig;
struct FImpactProfileConfigArray;
/**
 * Editor window dedicated to Decal Size editing
 */

class UImpactProfileDataAsset; 

class SImpactProfileEditorWindow : public SCompoundWidget, public FNotifyHook
{
public: 
	SLATE_BEGIN_ARGS(SImpactProfileEditorWindow): _TargetComponent(nullptr), _TargetDataAsset(nullptr) {}
        SLATE_ARGUMENT(UDestructionProjectileComponent*, TargetComponent)
		SLATE_ARGUMENT(UImpactProfileDataAsset*, TargetDataAsset)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Set target component */
	void SetTargetComponent(UDestructionProjectileComponent* InComponent);
	
	/** Static function to open window as tab */
	static void OpenWindow(UDestructionProjectileComponent* Component);

	static void OpenWindowForDataAsset(UImpactProfileDataAsset* DataAsset);
private:
	
	TArray<TSharedPtr<FString>> ToolShapeOptions;
private:
	/** Property change callback */ 
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;

	void SaveToComponent();
	void SaveToDataAsset();
	void LoadConfigFromDataAsset(FName ConfigID, FName SurfaceType);
	 
	void RefreshConfigIDList();           
	void RefreshSurfaceTypeList();  
	void RefreshVariantIndexList();
	
	FImpactProfileConfig* GetCurrentImpactConfig();
	FImpactProfileConfigArray* GetCurrentImpactConfigArray();
	
	void OnConfigIDSelected(FName SelectedConfigID);
	void OnSurfaceTypeSelected(FName SelectedSurfaceType);
	void OnVariantIndexSelected(int32 SelectedIndex);
	 
	void AddNewSurfaceType();
	void AddNewVariant();

	FName EnsureUniqueConfigID(FName NewName);
	FName EnsureUniqueSurfaceType(FName NewName);

	void DeleteCurrentConfigID();
	void DeleteCurrentSurfaceType();
	void DeleteCurrentVariant();

	void RenameCurrentConfigID(FName NewName);
	void RenameCurrentSurfaceType(FName NewName);

	/** UI creation helpers */
	TSharedRef<SWidget> CreateToolShapeSection();
	TSharedRef<SWidget> CreateConfigSelectionSection();
	TSharedRef<SWidget> CreatePreviewMeshSection();
	TSharedRef<SWidget> CreateDecalSection();
	
	/** Target Component */
	TWeakObjectPtr<UDestructionProjectileComponent> TargetComponent;
	TWeakObjectPtr<UImpactProfileDataAsset> TargetDataAsset;

	/** Viewport Widget */
	TSharedPtr<SImpactProfileEditorViewport> Viewport;

	/** Detail View */
	TSharedPtr<IDetailsView> DetailsView;

	/** Decal Material */
	TObjectPtr<UMaterialInterface> SelectedDecalMaterial;

	enum class EEditMode { Component, DataAsset };
	EEditMode CurrentEditMode = EEditMode::Component;
    
	
	/** Currently selected SurfaceType (surface material) */
	FName CurrentSurfaceType = NAME_None;
   
	/** ConfigID list (for combobox) */
	TArray<TSharedPtr<FName>> ConfigIDList;

	/** SurfaceType list (for combobox) - Surfaces of currently selected ConfigID */
	TArray<TSharedPtr<FName>> SurfaceTypeList;

	/** Variant Index list matching currently selected Surface Type */
	TArray<TSharedPtr<FString>> VariantIndexList;
	
	/** Currently editing material index */
	int32 CurVariantIndex = 0;

	
};
