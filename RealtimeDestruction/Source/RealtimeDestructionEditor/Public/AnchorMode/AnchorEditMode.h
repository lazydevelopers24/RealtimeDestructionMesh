// Fill out your copyright notice in the Description page of Project Settings.

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

UCLASS()
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
