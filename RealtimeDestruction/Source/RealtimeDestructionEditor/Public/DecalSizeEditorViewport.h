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
#include "SEditorViewport.h"
#include "SCommonEditorViewportToolbarBase.h"
#include "AdvancedPreviewScene.h"
#include "RealtimeDestruction/Public/Components/DestructionTypes.h"

class UDestructionProjectileComponent;
class FDecalSizeViewportClient;
class FAdvancedPreviewScene;

/**
 * DecalSize 편집용 프리뷰 뷰포트
 */
class SDecalSizeEditorViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SDecalSizeEditorViewport) {}
          SLATE_ARGUMENT(UDestructionProjectileComponent*, TargetComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SDecalSizeEditorViewport();

	// FGCObject Interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SDecalSizeEditorViewport"); };

	/** 타겟 컴포넌트 설정 */
	void SetTargetComponent(UDestructionProjectileComponent* InComponent);

	/** 프리뷰 갱신 */
	void RefreshPreview();

	/** Decal Transform 설정 */
	void SetDecalTransform(const FTransform& InTransform);
	FTransform GetDecalTransform() const { return DecalTransform; }
	
	/** ToolShape 크기 설정 */
	void SetToolShapeLocation(const FVector& InLocation);
	void SetToolShapeRotation(const FRotator& InRotation);

	void SetPreviewMesh(UStaticMesh* InPreviewMesh);
	void SetPreviewToolShape(EDestructionToolShape NewShape);
	void SetPreviewSphere(float InRadius);
	void SetPreviewCylinderRadius(float InRadius);
	void SetPreviewCylinderHeight( float InHeight);

	void SetPreviewMeshLocation(const FVector& InLocation);
	void SetPreviewMeshRotation(const FRotator& InRotator);

	FVector GetToolShapeLocation() const { return ToolShapeTransform.GetLocation(); }
	FRotator GetToolShapeRotation() const { return ToolShapeTransform.GetRotation().Rotator(); }
	
	EDestructionToolShape GetPreviewToolShape() const { return PreviewToolShape; }
	float GetPreviewSphereRadius() const { return PreviewSphereRadius; }
	float GetPreviewCylinderRadius() const { return PreviewCylinderRadius; }
	float GetPreviewCylinderHeight() const { return PreviewCylinderHeight; }
	UStaticMesh* GetPreviewMesh() const { return PreviewMesh; }

	FVector GetPreviewMeshLocation() const { return PreviewMeshLocation; }
	FRotator GetPreviewMeshRotation() const { return PreviewMeshRotation; }
	
	/** 프리뷰 메시만 업데이트 (전체 Refresh 없이) */ 
	void UpdateDecalMesh();   
	void UpdateDecalWireframe();
	
	 
	/** Decal Material */
	void SetDecalMaterial(UMaterialInterface* InMaterial);
	UMaterialInterface* GetDecalMaterial() const { return DecalMaterial; }

	void SetDecalSize(const FVector& InSize);
	FVector GetDecalSize() const { return DecalSize; }

	// Visibility Toggles
	void SetDecalVisible(bool bVisible);
	void SetToolShapeVisible(bool bVisible);
	void SetPreviewMeshVisible(bool bVisible);

	bool IsDecalVisible() const { return bShowDecal; }
	bool IsToolShapeVisible() const { return bShowToolShape; }
	bool IsPreviewMeshVisible() const { return bShowPreviewMesh; }

protected:
	//SEditorViewport Interface
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override; 

private:
	/** 타겟 컴포넌트 */
	TWeakObjectPtr<UDestructionProjectileComponent> TargetComponent;

	/** 프리뷰 씬 */
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;

	/** 뷰포트 클라이언트 */
	TSharedPtr<FDecalSizeViewportClient> ViewportClient;
	
	/** 프리뷰용 액터들 */
	TObjectPtr<AActor> PreviewActor = nullptr;
	TObjectPtr<class UDecalComponent> DecalPreviewComponent = nullptr;
	TObjectPtr<class UStaticMeshComponent> DecalTargetSurface = nullptr;  // 데칼 투영용 표면
	TObjectPtr<class ULineBatchComponent> DecalWireframe = nullptr;
	TObjectPtr<class UStaticMeshComponent> ProjectileMesh = nullptr;
	TObjectPtr<class ULineBatchComponent> ToolShapeWireframe = nullptr;
	TObjectPtr<UStaticMesh> PreviewMesh = nullptr;

	/** Decal Preview Tranform */
	FTransform DecalTransform; 
	FVector DecalSize = FVector(1.0f, 50.0f, 50.0f);

	/** ToolShape Preview Transform */
	EDestructionToolShape PreviewToolShape = EDestructionToolShape::Cylinder;
	FTransform ToolShapeTransform;

	/** Preview Mesh Data */
	FVector PreviewMeshLocation = FVector::ZeroVector;
	FRotator PreviewMeshRotation = FRotator::ZeroRotator;

	void UpdateToolShapeWireframe();
 
	float PreviewSphereRadius = 5.0f;
	float PreviewCylinderRadius = 5.0f;
	float PreviewCylinderHeight = 20.0f;

	/** Decal Material */
	TObjectPtr<UMaterialInterface> DecalMaterial;

	/** Visibility flags */
	bool bShowDecal = true;
	bool bShowToolShape = true;
	bool bShowPreviewMesh = true;

	/** 저장을 위한 함수 */
	void SaveState();
};


/** viewport client */
class FDecalSizeViewportClient: public FEditorViewportClient
{
public:
	FDecalSizeViewportClient(FAdvancedPreviewScene* InAdvancedPreviewScene,
		const TWeakPtr<SEditorViewport>& InEditorViewport);

	// FEditorViewportClient Interface
	virtual void Tick(float DeltaSeconds) override;
	virtual FSceneInterface* GetScene() const override;
	virtual FLinearColor GetBackgroundColor() const override;

private:
	FAdvancedPreviewScene* PreviewScene;
};
