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
class FImpactProfileViewportClient;
class FAdvancedPreviewScene;

/**
 * Preview viewport for DecalSize editing
 */
class SImpactProfileEditorViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SImpactProfileEditorViewport) {}
          SLATE_ARGUMENT(UDestructionProjectileComponent*, TargetComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SImpactProfileEditorViewport();

	// FGCObject Interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SImpactProfileEditorViewport"); };

	/** Set target component */
	void SetTargetComponent(UDestructionProjectileComponent* InComponent);

	/** Refresh preview */
	void RefreshPreview();

	/** Set Decal Transform */
	void SetDecalTransform(const FTransform& InTransform);
	FTransform GetDecalTransform() const { return DecalTransform; }
	
	/** Set ToolShape transform */
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
	
	/** Update preview mesh only (without full Refresh) */ 
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
	/** Target component */
	TWeakObjectPtr<UDestructionProjectileComponent> TargetComponent;

	/** Preview scene */
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;

	/** Viewport client */
	TSharedPtr<FImpactProfileViewportClient> ViewportClient;
	
	/** Preview actors */
	TObjectPtr<AActor> PreviewActor = nullptr;
	TObjectPtr<class UDecalComponent> DecalPreviewComponent = nullptr;
	TObjectPtr<class UStaticMeshComponent> DecalTargetSurface = nullptr;  // Surface for decal projection
	TObjectPtr<class ULineBatchComponent> DecalWireframe = nullptr;
	TObjectPtr<class UStaticMeshComponent> ProjectileMesh = nullptr;
	TObjectPtr<class ULineBatchComponent> ToolShapeWireframe = nullptr;
	TObjectPtr<UStaticMesh> PreviewMesh = nullptr;

	/** Decal Preview Transform */
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

	/** Function for saving state */
	void SaveState();
};


/** viewport client */
class FImpactProfileViewportClient: public FEditorViewportClient
{
public:
	FImpactProfileViewportClient(FAdvancedPreviewScene* InAdvancedPreviewScene,
		const TWeakPtr<SEditorViewport>& InEditorViewport);

	// FEditorViewportClient Interface
	virtual void Tick(float DeltaSeconds) override;
	virtual FSceneInterface* GetScene() const override;
	virtual FLinearColor GetBackgroundColor() const override;

private:
	FAdvancedPreviewScene* PreviewScene;
};
