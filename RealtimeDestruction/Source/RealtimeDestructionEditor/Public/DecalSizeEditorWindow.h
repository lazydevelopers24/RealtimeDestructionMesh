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
class SDecalSizeEditorViewport;
class IDetailsView;

struct FDecalSizeConfig;
struct FDecalSizeConfigArray;
/**
 * Decal Size 편집 전용 에디터 윈도우  
 */

class UDecalMaterialDataAsset; 

class SDecalSizeEditorWindow : public SCompoundWidget, public FNotifyHook
{
public: 
	SLATE_BEGIN_ARGS(SDecalSizeEditorWindow): _TargetComponent(nullptr), _TargetDataAsset(nullptr) {}
        SLATE_ARGUMENT(UDestructionProjectileComponent*, TargetComponent)
		SLATE_ARGUMENT(UDecalMaterialDataAsset*, TargetDataAsset)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** 타겟 컴포넌트 설정 */
	void SetTargetComponent(UDestructionProjectileComponent* InComponent);
	
	/** 위도우를 탭으로 여는 static 함수 */
	static void OpenWindow(UDestructionProjectileComponent* Component);

	static void OpenWindowForDataAsset(UDecalMaterialDataAsset* DataAsset);
private:
	
	TArray<TSharedPtr<FString>> ToolShapeOptions;
private:
	/** propert 변경 콜백 */ 
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;

	void SaveToComponent();
	void SaveToDataAsset();
	void LoadConfigFromDataAsset(FName ConfigID, FName SurfaceType);
	 
	void RefreshConfigIDList();           
	void RefreshSurfaceTypeList();  
	void RefreshVariantIndexList();
	
	FDecalSizeConfig* GetCurrentDecalConfig(); 
	FDecalSizeConfigArray* GetCurrentDecalConfigArray();
	
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

	/** UI 생성 헬퍼 */
	TSharedRef<SWidget> CreateToolShapeSection();
	TSharedRef<SWidget> CreateConfigSelectionSection();
	TSharedRef<SWidget> CreatePreviewMeshSection();
	TSharedRef<SWidget> CreateDecalSection();
	
	/** Target Component */
	TWeakObjectPtr<UDestructionProjectileComponent> TargetComponent;
	TWeakObjectPtr<UDecalMaterialDataAsset> TargetDataAsset;

	/** Viewport Widget */
	TSharedPtr<SDecalSizeEditorViewport> Viewport;

	/** Detail View */
	TSharedPtr<IDetailsView> DetailsView;

	/** Decal Material */
	TObjectPtr<UMaterialInterface> SelectedDecalMaterial;

	enum class EEditMode { Component, DataAsset };
	EEditMode CurrentEditMode = EEditMode::Component;
    
	
	/** 현재 선택된 SurfaceType (표면 재질) */
	FName CurrentSurfaceType = NAME_None;
   
	/** ConfigID 목록 (콤보박스용) */
	TArray<TSharedPtr<FName>> ConfigIDList;

	/** SurfaceType 목록 (콤보박스용) - 현재 선택된 ConfigID의 Surface들 */
	TArray<TSharedPtr<FName>> SurfaceTypeList;

	/** 현재 선택된 Surface Type에 맞는 Variant Index 목록 */
	TArray<TSharedPtr<FString>> VariantIndexList;
	
	/** 현재 편집중인 matrial 인덱스 */
	int32 CurVariantIndex = 0;

	
};
