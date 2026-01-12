#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Misc/NotifyHook.h"

class UDestructionProjectileComponent;
class SDecalSizeEditorViewport;
class IDetailsView;

/**
 * Decal Size 편집 전용 에디터 윈도우  
 */

class SDecalSizeEditorWindow : public SCompoundWidget, public FNotifyHook
{
public:
	SLATE_BEGIN_ARGS(SDecalSizeEditorWindow) {} 
        SLATE_ARGUMENT(UDestructionProjectileComponent*, TargetComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** 타겟 컴포넌트 설정 */
	void SetTargetComponent(UDestructionProjectileComponent* InComponent);
	
	/** 위도우를 탭으로 여는 static 함수 */
	static void OpenWindow(UDestructionProjectileComponent* Component);

private:
	TSharedRef<SWidget> CreateMaterialSection();

	TArray<TSharedPtr<FString>> ToolShapeOptions;
private:
	/** propert 변경 콜백 */ 
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;

	/** UI 생성 헬퍼 */
	TSharedRef<SWidget> CreateDecalTransformSection();
	TSharedRef<SWidget> CreateToolShapeSection();
	
	
	/** Target Component */
	TWeakObjectPtr<UDestructionProjectileComponent> TargetComponent;

	/** Viewport Widget */
	TSharedPtr<SDecalSizeEditorViewport> Viewport;

	/** Detail View */
	TSharedPtr<IDetailsView> DetailsView;

	/** Decal Material */
	TObjectPtr<UMaterialInterface> SelectedDecalMaterial;
};
