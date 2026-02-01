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
#include "Tools/LegacyEdModeWidgetHelpers.h"
#include "AnchorEditMode.generated.h"

class UAnchorActionObejct;
class URealtimeDestructibleMeshComponent;

enum class EAnchorToolType
{
	None,
	Plane,
	Volume,
	Paint
};

UCLASS(ClassGroup = (RealtimeDestructionEditor))
class REALTIMEDESTRUCTIONEDITOR_API UAnchorEditMode : public UBaseLegacyWidgetEdMode
{
	GENERATED_BODY()

public:
	UAnchorEditMode();
	~UAnchorEditMode() = default;
	
	const static FEditorModeID EM_AnchorEditModeId;
	
	virtual void Enter() override;
	virtual void Exit() override;

	virtual void CreateToolkit() override;

	virtual bool IsSelectionAllowed(AActor* InActor, bool bInSelection) const override;

	virtual void Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI) override;

	void OnEditorSelectionChanged(UObject* NewSelection);

	void ActorSelectionChangeNotify();
	
	UPROPERTY()
	TObjectPtr<UAnchorActionObejct> ActionObject;

	UPROPERTY(Transient)
	TObjectPtr<URealtimeDestructibleMeshComponent> SelectedComp;

protected:
	virtual TSharedRef<FLegacyEdModeWidgetHelper> CreateWidgetHelper() override;
};
