// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "ImpactProfileEditorWindow.h"
#include "ImpactProfileEditorViewport.h"
#include "Components/DestructionProjectileComponent.h"

#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SNullWidget.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "BoneControllers/AnimNode_AnimDynamics.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SVectorInputBox.h"
#include "Widgets/Input/SRotatorInputBox.h"
#include "PropertyCustomizationHelpers.h"
#include "Data/ImpactProfileDataAsset.h"
#include "Subsystems/DestructionGameInstanceSubsystem.h"
#include "Settings/RDMSetting.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "DecalSizeEditorWindow"

static const FName DecalSizeEditorTabId("DecalSizeEditorTab");

void SImpactProfileEditorWindow::Construct(const FArguments& InArgs)
{
	TargetComponent = InArgs._TargetComponent;
	TargetDataAsset  = InArgs._TargetDataAsset;

	// 편집 모드 결정
	if (TargetDataAsset.IsValid())
	{
		CurrentEditMode = EEditMode::DataAsset;

		// ConfigID가 없으면 기본값 설정
		if (TargetDataAsset->ConfigID.IsNone())
		{
			TargetDataAsset->ConfigID = FName("Default");
			TargetDataAsset->MarkPackageDirty();
		}

		// SurfaceConfigs가 비어있으면 기본 Surface 추가
		if (TargetDataAsset->SurfaceConfigs.Num() == 0)
		{
			FImpactProfileConfigArray DefaultSurfaceArray;
			DefaultSurfaceArray.Configs.Add(FImpactProfileConfig());
			TargetDataAsset->SurfaceConfigs.Add(FName("Default"), DefaultSurfaceArray);
			TargetDataAsset->MarkPackageDirty();
		}

		RefreshConfigIDList();
		if (ConfigIDList.Num() > 0)
		{
			OnConfigIDSelected(*ConfigIDList[0]);

			// Material 가져오기
			FImpactProfileConfig* Config = GetCurrentImpactConfig();
			if (Config)
			{
				SelectedDecalMaterial = Config->DecalMaterial;
			}
		}
	}
	else if (TargetComponent.IsValid())
	{
		CurrentEditMode = EEditMode::Component;
		SelectedDecalMaterial = TargetComponent->DecalMaterialInEditor;
	}
	
	ToolShapeOptions.Add(MakeShared<FString>(TEXT("Sphere")));
	ToolShapeOptions.Add(MakeShared<FString>(TEXT("Cylinder")));
	
    //if(TargetComponent.IsValid() && TargetComponent->)
	// Create Detail View
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bShowOptions = false;
	DetailsViewArgs.bShowPropertyMatrixButton = false;
    DetailsViewArgs.NotifyHook = this;
	DetailsViewArgs.bShowCustomFilterOption = false;
	
	DetailsView = PropertyModule.CreateDetailView(DetailsViewArgs);

   
	// 기본 Transform 숨기기
	//DetailsView->SetIsPropertyVisibleDelegate(
	//	FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& PropertyAndParent)->bool
	//	{
	//		FString Category = PropertyAndParent.Property.GetMetaData(TEXT("Category"));
	//		FName PropertyName = PropertyAndParent.Property.GetFName();
	//
	//		// Transform 숨기기
	//		if (PropertyName == FName("RelativeLocation") ||
	//			PropertyName == FName("RelativeRotation") ||
	//			PropertyName == FName("RelativeScale"))
	//		{
	//			return false;
	//		}
	//
	//		// Destruction 카테고리만 보이기
	//		if (Category.Contains(TEXT("Destruction")))
	//		{
	//			return true;
	//		}
	//			
	//		return false;
	//	})
	//); 

	// Editor에 한 번 들어가면, button 숨기기
	DetailsView->SetIsCustomRowVisibleDelegate(FIsCustomRowVisible::CreateLambda(
		[](FName InRowName, FName InParentName)->bool
		{
			if (InRowName == FName("Open Impact Profile Editor"))
			{
				return false;
			}
			return true;
		}));
	
	// Target Component를 Dectai View에 설정 
	if (CurrentEditMode == EEditMode::DataAsset && TargetDataAsset.IsValid())
	{
		DetailsView->SetObject(TargetDataAsset.Get());
	}
	else if (TargetComponent.IsValid())
	{
		DetailsView->SetObject(TargetComponent.Get());
	}

	
	// UI 구성
	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(Orient_Horizontal)

		// 왼쪽: Viewport
		+ SSplitter::Slot()
		.Value(0.7f)
		[
			SNew(SBox)
			.MinDesiredWidth(400)
			.MinDesiredHeight(300)
			[
				SAssignNew(Viewport, SImpactProfileEditorViewport)
				.TargetComponent(TargetComponent.Get())
			]
		]

		// 오른쪽: Property Panel (스크롤 가능)
		+ SSplitter::Slot()
		.Value(0.3f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)

				// 타이틀
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Impact Profile Editor"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
				]

				// Config 선택 섹션 (DataAsset 모드에서만)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					CurrentEditMode == EEditMode::DataAsset
						? CreateConfigSelectionSection()
						: SNullWidget::NullWidget
				]

				// Decal 섹션 (Material + Transform)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					CreateDecalSection()
				]

				// Tool Shape 섹션 (Radius, Height만)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					CreateToolShapeSection()
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					CreatePreviewMeshSection()
				]

				// DetailsView (Component 모드에서만 표시)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					CurrentEditMode == EEditMode::Component
						? DetailsView.ToSharedRef()
						: SNullWidget::NullWidget
				]

				// Apply 버튼 (Component 모드에서만 표시)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8.0f)
				[
					CurrentEditMode == EEditMode::Component
						? SNew(SButton)
							.Text(FText::FromString("Apply DecalSize to Component"))
							.HAlign(HAlign_Center)
							.OnClicked_Lambda([this]()
							{
								SaveToComponent();
								return FReply::Handled();
							})
						: SNullWidget::NullWidget
				]
			]
		] 
      ];

	if (CurrentEditMode == EEditMode::DataAsset && TargetDataAsset.IsValid() && Viewport.IsValid())
	{
		LoadConfigFromDataAsset(TargetDataAsset->ConfigID, CurrentSurfaceType);
	}
	
  }


void SImpactProfileEditorWindow::SetTargetComponent(UDestructionProjectileComponent* InComponent)
{
	TargetComponent = InComponent;

	if (DetailsView.IsValid())
	{
		DetailsView->SetObject(InComponent);
	}

	if (Viewport.IsValid())
	{
		Viewport->SetTargetComponent(InComponent);
	}
}

void SImpactProfileEditorWindow::OpenWindow(UDestructionProjectileComponent* Component)
{
	// 독립 윈도우로 열기
	
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("ImpactProfileEditorTitle", "Impact Profile Editor"))
		.ClientSize(FVector2D(1200, 600))
		.SupportsMinimize(true)
		.SupportsMaximize(true);

	
	TSharedRef<SImpactProfileEditorWindow> EditorWidget = SNew(SImpactProfileEditorWindow)
		.TargetComponent(Component);

	Window->SetContent(EditorWidget);

	FSlateApplication::Get().AddWindow(Window);
}

void SImpactProfileEditorWindow::OpenWindowForDataAsset(UImpactProfileDataAsset* DataAsset)
{
	if (!DataAsset)
	{
		return;
	}
 
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(FString::Printf(TEXT("Impact Profile Editor - %s"), *DataAsset->GetName())))
		.ClientSize(FVector2D(1200, 600))
		.SupportsMinimize(true)
		.SupportsMaximize(true);

	TSharedRef<SImpactProfileEditorWindow> EditorWidget = SNew(SImpactProfileEditorWindow)
		.TargetDataAsset(DataAsset);

	Window->SetContent(EditorWidget);


    //창 닫을 때 delegate
    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda(
        [EditorWidget](const TSharedRef<SWindow>&) {
            EditorWidget->SaveToDataAsset();

        }
    ));

	FSlateApplication::Get().AddWindow(Window);
	
}

TSharedRef<SWidget> SImpactProfileEditorWindow::CreateDecalSection()
{
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("Decal", "Decal"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SVerticalBox)

			// Show Checkbox
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() {
					return Viewport.IsValid() && Viewport->IsDecalVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) {
					if (Viewport.IsValid())
					{
						Viewport->SetDecalVisible(NewState == ECheckBoxState::Checked);
					}
				})
				[
					SNew(STextBlock).Text(FText::FromString("Show Decal"))
				]
			]

			// ========== Material 선택 ==========
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Material"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(0.7f)
				[
					SNew(SObjectPropertyEntryBox)
					.AllowedClass(UMaterialInstance::StaticClass())
					.ObjectPath_Lambda([this]() -> FString
					{
						return SelectedDecalMaterial ? SelectedDecalMaterial->GetPathName() : FString();
					})
					.OnObjectChanged_Lambda([this](const FAssetData& AssetData)
					{
						SelectedDecalMaterial = Cast<UMaterialInterface>(AssetData.GetAsset());
						if (Viewport.IsValid())
						{
							Viewport->SetDecalMaterial(SelectedDecalMaterial);
							SaveToDataAsset();
						}
					})
				]
			]

			// ========== Decal Size ==========
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f, 8.0f, 4.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString("Size (Depth, Width, Height)"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				// Depth (X)
				+ SHorizontalBox::Slot()
				.FillWidth(0.33f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[SNew(STextBlock).Text(FText::FromString("D"))]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SSpinBox<float>)
						.MinValue(1.0f).MaxValue(1000.0f)
						.Value_Lambda([this]() {
							return Viewport.IsValid() ? Viewport->GetDecalSize().X : 10.0f;
						})
						.OnValueChanged_Lambda([this](float V) {
							if (Viewport.IsValid()) {
								FVector Size = Viewport->GetDecalSize();
								Size.X = V;
								Viewport->SetDecalSize(Size);
							}
						})
					]
				]

				// Width (Y)
				+ SHorizontalBox::Slot()
				.FillWidth(0.33f)
				.Padding(4.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[SNew(STextBlock).Text(FText::FromString("W"))]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SSpinBox<float>)
						.MinValue(1.0f).MaxValue(1000.0f)
						.Value_Lambda([this]() {
							return Viewport.IsValid() ? Viewport->GetDecalSize().Y : 50.0f;
						})
						.OnValueChanged_Lambda([this](float V) {
							if (Viewport.IsValid()) {
								FVector Size = Viewport->GetDecalSize();
								Size.Y = V;
								Viewport->SetDecalSize(Size);
							}
						})
					]
				]

				// Height (Z)
				+ SHorizontalBox::Slot()
				.FillWidth(0.33f)
				.Padding(4.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[SNew(STextBlock).Text(FText::FromString("H"))]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SSpinBox<float>)
						.MinValue(1.0f).MaxValue(1000.0f)
						.Value_Lambda([this]() {
							return Viewport.IsValid() ? Viewport->GetDecalSize().Z : 50.0f;
						})
						.OnValueChanged_Lambda([this](float V) {
							if (Viewport.IsValid()) {
								FVector Size = Viewport->GetDecalSize();
								Size.Z = V;
								Viewport->SetDecalSize(Size);
							}
						})
					]
				]
			]

			// ========== Location ==========
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f, 8.0f, 4.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString("Location Offset"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 2.0f)
			[
				SNew(SVectorInputBox)
				.bColorAxisLabels(true)
				.AllowSpin(true)
				.X_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetDecalTransform().GetLocation().X : 0.0f; })
				.Y_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetDecalTransform().GetLocation().Y : 0.0f; })
				.Z_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetDecalTransform().GetLocation().Z : 0.0f; })
				.OnXChanged_Lambda([this](float V) {
					if (Viewport.IsValid()) {
						FTransform T = Viewport->GetDecalTransform();
						FVector Loc = T.GetLocation();
						Loc.X = V;
						T.SetLocation(Loc);
						Viewport->SetDecalTransform(T);
					}
				})
				.OnYChanged_Lambda([this](float V) {
					if (Viewport.IsValid()) {
						FTransform T = Viewport->GetDecalTransform();
						FVector Loc = T.GetLocation();
						Loc.Y = V;
						T.SetLocation(Loc);
						Viewport->SetDecalTransform(T);
					}
				})
				.OnZChanged_Lambda([this](float V) {
					if (Viewport.IsValid()) {
						FTransform T = Viewport->GetDecalTransform();
						FVector Loc = T.GetLocation();
						Loc.Z = V;
						T.SetLocation(Loc);
						Viewport->SetDecalTransform(T);
					}
				})
			]

			// ========== Rotation ==========
			+SVerticalBox::Slot()
			.Padding(4.0f, 8.0f, 4.0f, 4.0f)
			[
				SNew(SHorizontalBox)

				  + SHorizontalBox::Slot()
                      .FillWidth(0.3f)
                      .VAlign(VAlign_Center)
                      [
                          SNew(STextBlock)
                          .Text(FText::FromString("Random Rotation"))
                      ]
                + SHorizontalBox::Slot()
				.FillWidth(0.7f)
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						FImpactProfileConfig* Config = GetCurrentImpactConfig();
			  			if (Config)
			  			{
							  return Config->bRandomDecalRotation ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			  			}
			  			return ECheckBoxState::Checked;  
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						FImpactProfileConfig* Config = GetCurrentImpactConfig();
						 if (Config)
						 {
							 Config->bRandomDecalRotation = (NewState == ECheckBoxState::Checked);
							 SaveToDataAsset();
						 }
					})
				 
				]
			]
			
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f, 8.0f, 4.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString("Rotation Offset"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 2.0f)
			[
				SNew(SRotatorInputBox)
				.bColorAxisLabels(true)
				.AllowSpin(true)
				.Roll_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetDecalTransform().GetRotation().Rotator().Roll : 0.0f; })
				.Pitch_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetDecalTransform().GetRotation().Rotator().Pitch : 0.0f; })
				.Yaw_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetDecalTransform().GetRotation().Rotator().Yaw : 0.0f; })
				.OnRollChanged_Lambda([this](float V) {
					if (Viewport.IsValid()) {
						FTransform T = Viewport->GetDecalTransform();
						FRotator Rot = T.GetRotation().Rotator();
						Rot.Roll = V;
						T.SetRotation(Rot.Quaternion());
						Viewport->SetDecalTransform(T);
					}
				})
				.OnPitchChanged_Lambda([this](float V) {
					if (Viewport.IsValid()) {
						FTransform T = Viewport->GetDecalTransform();
						FRotator Rot = T.GetRotation().Rotator();
						Rot.Pitch = V;
						T.SetRotation(Rot.Quaternion());
						Viewport->SetDecalTransform(T);
					}
				})
				.OnYawChanged_Lambda([this](float V) {
					if (Viewport.IsValid()) {
						FTransform T = Viewport->GetDecalTransform();
						FRotator Rot = T.GetRotation().Rotator();
						Rot.Yaw = V;
						T.SetRotation(Rot.Quaternion());
						Viewport->SetDecalTransform(T);
					}
				})
			]
		];
}

void SImpactProfileEditorWindow::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent,
                                              FProperty* PropertyThatChanged)
{
	if (Viewport.IsValid())
	{
		Viewport->RefreshPreview();
	}
}

TSharedRef<SWidget> SImpactProfileEditorWindow::CreateToolShapeSection()
{
    // 초기값
      float InitSphereRadius = 10.0f;
      float InitCylinderRadius = 10.0f;
      float InitCylinderHeight = 400.0f;

      if (TargetComponent.IsValid())
      {
          InitSphereRadius = TargetComponent->SphereRadius;
          InitCylinderRadius = TargetComponent->CylinderRadius;
          InitCylinderHeight = TargetComponent->CylinderHeight;
      }

      return SNew(SExpandableArea)
          .AreaTitle(LOCTEXT("ToolShape", "Tool Shape Parameters"))
          .InitiallyCollapsed(false)
          .BodyContent()
          [
              SNew(SVerticalBox)

  // Show Checkbox
  + SVerticalBox::Slot()
  .AutoHeight()
  .Padding(4.0f)
  [
      SNew(SCheckBox)
      .IsChecked_Lambda([this]() {
          return Viewport.IsValid() && Viewport->IsToolShapeVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
      })
      .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) {
          if (Viewport.IsValid())
          {
              Viewport->SetToolShapeVisible(NewState == ECheckBoxState::Checked);
          }
      })
      [
          SNew(STextBlock).Text(FText::FromString("Show Tool Shape"))
      ]
  ]

  + SVerticalBox::Slot()
  .AutoHeight()
  .Padding(4.0f, 8.0f, 4.0f, 4.0f)
  [
      SNew(STextBlock)
      .Text(FText::FromString("Tool Location"))
      .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
  ]

  + SVerticalBox::Slot()
  .AutoHeight()
  .Padding(8.0f, 2.0f)
  [
      SNew(SVectorInputBox)
      .bColorAxisLabels(true)
      .AllowSpin(true)
      .X_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetToolShapeLocation().X : 0.0f; })
      .Y_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetToolShapeLocation().Y : 0.0f; })
      .Z_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetToolShapeLocation().Z : 0.0f; })
      .OnXChanged_Lambda([this](float V) {
          if (Viewport.IsValid()) {
              FVector Loc = Viewport->GetToolShapeLocation();
              Loc.X = V;
              Viewport->SetToolShapeLocation(Loc);
          }
      })
      .OnYChanged_Lambda([this](float V) {
          if (Viewport.IsValid()) {
              FVector Loc = Viewport->GetToolShapeLocation();
              Loc.Y = V;
              Viewport->SetToolShapeLocation(Loc);
          }
      })
      .OnZChanged_Lambda([this](float V) {
          if (Viewport.IsValid()) {
              FVector Loc = Viewport->GetToolShapeLocation();
              Loc.Z = V;
              Viewport->SetToolShapeLocation(Loc);
          }
      })
  ]

  // ===== Tool Shape Rotation =====
  + SVerticalBox::Slot()
  .AutoHeight()
  .Padding(4.0f, 8.0f, 4.0f, 4.0f)
  [
      SNew(STextBlock)
      .Text(FText::FromString("Tool Rotation"))
      .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
  ]

  + SVerticalBox::Slot()
  .AutoHeight()
  .Padding(8.0f, 2.0f)
  [
      SNew(SRotatorInputBox)
      .bColorAxisLabels(true)
      .AllowSpin(true)
      .Roll_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetToolShapeRotation().Roll : 0.0f; })
      .Pitch_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetToolShapeRotation().Pitch : 0.0f; })
      .Yaw_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetToolShapeRotation().Yaw : 0.0f; })
      .OnRollChanged_Lambda([this](float V) {
          if (Viewport.IsValid()) {
              FRotator Rot = Viewport->GetToolShapeRotation();
              Rot.Roll = V;
              Viewport->SetToolShapeRotation(Rot);
          }
      })
      .OnPitchChanged_Lambda([this](float V) {
          if (Viewport.IsValid()) {
              FRotator Rot = Viewport->GetToolShapeRotation();
              Rot.Pitch = V;
              Viewport->SetToolShapeRotation(Rot);
          }
      })
      .OnYawChanged_Lambda([this](float V) {
          if (Viewport.IsValid()) {
              FRotator Rot = Viewport->GetToolShapeRotation();
              Rot.Yaw = V;
              Viewport->SetToolShapeRotation(Rot);
          }
      })
  ]
              // Tool Shape 선택
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 4.0f)
              [
                  SNew(SHorizontalBox)

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  .VAlign(VAlign_Center)
                  [
                      SNew(STextBlock)
                      .Text(FText::FromString("Tool Shape"))
                  ]

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  [
                      SNew(SComboBox<TSharedPtr<FString>>)
                      .OptionsSource(&ToolShapeOptions)
                      .OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewValue, ESelectInfo::Type)
                      {
                          if (!NewValue.IsValid() || !Viewport.IsValid()) return;

                          if (*NewValue == TEXT("Sphere"))
                          {
                              Viewport->SetPreviewToolShape(EDestructionToolShape::Sphere);
                          }
                          else if (*NewValue == TEXT("Cylinder"))
                          {
                              Viewport->SetPreviewToolShape(EDestructionToolShape::Cylinder);
                          }
                      })
                      .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
                      {
                          return SNew(STextBlock).Text(FText::FromString(*Item));
                      })
                      .Content()
                      [
                          SNew(STextBlock)
                          .Text_Lambda([this]() -> FText
                          {
                              if (!Viewport.IsValid()) return FText::FromString("Cylinder");

                              switch (Viewport->GetPreviewToolShape())
                              {
                              case EDestructionToolShape::Sphere:
                                  return FText::FromString("Sphere");
                              case EDestructionToolShape::Cylinder:
                                  return FText::FromString("Cylinder");
                              default:
                                  return FText::FromString("Cylinder");
                              }
                          })
                      ]
                  ]
              ]

              // Sphere Radius (Sphere일 때만 표시)
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 4.0f)
              [
                  SNew(SHorizontalBox)
                  .Visibility_Lambda([this]() {
                      return (Viewport.IsValid() && Viewport->GetPreviewToolShape() == EDestructionToolShape::Sphere)
                          ? EVisibility::Visible : EVisibility::Collapsed;
                  })

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  .VAlign(VAlign_Center)
                  [
                      SNew(STextBlock)
                      .Text(FText::FromString("Sphere Radius"))
                  ]

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  [
                      SNew(SSpinBox<float>)
                      .MinValue(1.0f)
                      .MaxValue(1000.0f)
                      .Value_Lambda([this]() {
                          return Viewport.IsValid() ? Viewport->GetPreviewSphereRadius() : 10.0f;
                      })
                      .OnValueChanged_Lambda([this](float V)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewSphere(V);
							  SaveToDataAsset();
                          }
                      })
                  ]
              ]

              // Cylinder Radius (Cylinder일 때만 표시)
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 4.0f)
              [
                  SNew(SHorizontalBox)
                  .Visibility_Lambda([this]() {
                      return (Viewport.IsValid() && Viewport->GetPreviewToolShape() == EDestructionToolShape::Cylinder)
                          ? EVisibility::Visible : EVisibility::Collapsed;
                  })

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  .VAlign(VAlign_Center)
                  [
                      SNew(STextBlock)
                      .Text(FText::FromString("Cylinder Radius"))
                  ]

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  [
                      SNew(SSpinBox<float>)
                      .MinValue(1.0f)
                      .MaxValue(1000.0f)
                      .Value_Lambda([this]() {
                          return Viewport.IsValid() ? Viewport->GetPreviewCylinderRadius() : 10.0f;
                      })
                      .OnValueChanged_Lambda([this](float V)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewCylinderRadius(V);
							  SaveToDataAsset();
                          }
                      })
                  ]
              ]

              // Cylinder Height (Cylinder일 때만 표시)
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 4.0f)
              [
                  SNew(SHorizontalBox)
                  .Visibility_Lambda([this]() {
                      return (Viewport.IsValid() && Viewport->GetPreviewToolShape() == EDestructionToolShape::Cylinder)
                          ? EVisibility::Visible : EVisibility::Collapsed;
                  })

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  .VAlign(VAlign_Center)
                  [
                      SNew(STextBlock)
                      .Text(FText::FromString("Cylinder Height"))
                  ]

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  [
                      SNew(SSpinBox<float>)
                      .MinValue(1.0f)
                      .MaxValue(2000.0f)
                      .Value_Lambda([this]() {
                          return Viewport.IsValid() ? Viewport->GetPreviewCylinderHeight() : 400.0f;
                      })
                      .OnValueChanged_Lambda([this](float V)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewCylinderHeight(V);
							  SaveToDataAsset();
                          }
                      })
                  ]
              ]
          ];
}

TSharedRef<SWidget> SImpactProfileEditorWindow::CreateConfigSelectionSection()
{
	return SNew(SExpandableArea)
		.AreaTitle(FText::FromString("Config Selection"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SVerticalBox)

			// Row 1: Config ID 선택
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Config ID"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SComboBox<TSharedPtr<FName>>)
					.OptionsSource(&ConfigIDList)
					.OnSelectionChanged_Lambda([this](TSharedPtr<FName> NewValue, ESelectInfo::Type)
					{
						if (NewValue.IsValid())
						{
							SaveToDataAsset();
							OnConfigIDSelected(*NewValue);
						}
					})
					.OnGenerateWidget_Lambda([](TSharedPtr<FName> Item)
					{
						return SNew(STextBlock).Text(FText::FromName(*Item));
					})
					.Content()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							return TargetDataAsset.IsValid() ? FText::FromName(TargetDataAsset->ConfigID) : FText::GetEmpty();
						})
					]
				]
			]

			// Row 2: Surface Type 선택
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Surface Type"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(0.5f)
				[
					SNew(SComboBox<TSharedPtr<FName>>)
					.OptionsSource(&SurfaceTypeList)
					.OnSelectionChanged_Lambda([this](TSharedPtr<FName> NewValue, ESelectInfo::Type)
					{
						if (NewValue.IsValid())
						{
							SaveToDataAsset();
							OnSurfaceTypeSelected(*NewValue);
						}
					})
					.OnGenerateWidget_Lambda([](TSharedPtr<FName> Item)
					{
						return SNew(STextBlock).Text(FText::FromName(*Item));
					})
					.Content()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							return FText::FromName(CurrentSurfaceType);
						})
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString("+"))
					.ToolTipText(FText::FromString("Add new Surface Type"))
					.OnClicked_Lambda([this]()
					{
						AddNewSurfaceType();
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString("-"))
					.ToolTipText(FText::FromString("Delete current Surface Type"))
					.IsEnabled_Lambda([this]()
					{
						// 최소 1개의 Surface Type은 유지
						return TargetDataAsset.IsValid() && TargetDataAsset->SurfaceConfigs.Num() > 1;
					})
					.OnClicked_Lambda([this]()
					{
						DeleteCurrentSurfaceType();
						return FReply::Handled();
					})
				]
			]

			// Row3 : Varient Index 설정
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SHorizontalBox)

				+SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Variant Index"))
				]

				+SHorizontalBox::Slot()
				.FillWidth(0.5)
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&VariantIndexList)
					.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewValue , ESelectInfo::Type)
					{
						if (NewValue.IsValid())
						{
							SaveToDataAsset();
							int32 SelectedIndex = FCString::Atoi(**NewValue);
							OnVariantIndexSelected(SelectedIndex);
						}
					} )
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
					{
						return SNew(STextBlock).Text(FText::FromString(*Item));
					})
					.Content()
					[
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							FImpactProfileConfig* Config = GetCurrentImpactConfig();
							if (Config && !Config->VariantName.IsEmpty())
							{
								return FText::FromString(FString::Printf(TEXT("%d: %s"), CurVariantIndex, *Config->VariantName));
							}
							return FText::FromString(FString::Printf(TEXT("%d"), CurVariantIndex));
						})
					] 
				]

				+SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString("+"))
                    .ToolTipText(FText::FromString("Add new Variant"))
					.OnClicked_Lambda([this]()
					 {
						 AddNewVariant();
						 return FReply::Handled();
					 })
				]
				+SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.0f, 0.0f)
				  [
					  SNew(SButton)
				  	  .Text(FText::FromString("-"))
					  .ToolTipText(FText::FromString("Delete current Variant"))
				  	.IsEnabled_Lambda([this]()
					 { 
						 FImpactProfileConfigArray* ConfigArray = GetCurrentImpactConfigArray();
						 return ConfigArray && ConfigArray->Configs.Num() > 1;
					 })
					  .OnClicked_Lambda([this]()
					  {
					  	DeleteCurrentVariant();
						return FReply::Handled();
					  })
				]
			]

			
			// Row 4: Rename Config ID
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Rename Config"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(0.7f)
				[
					SNew(SEditableTextBox)
					.Text_Lambda([this]() -> FText
					{
						return TargetDataAsset.IsValid() ? FText::FromName(TargetDataAsset->ConfigID) : FText::GetEmpty();
					})
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
					{
						if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
						{
							RenameCurrentConfigID(FName(*NewText.ToString()));
						}
					})
				]
			]

			// Row 5: Rename Surface Type
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Rename Surface"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(0.7f)
				[
					SNew(SEditableTextBox)
					.Text_Lambda([this]() -> FText
					{
						return FText::FromName(CurrentSurfaceType);
					})
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
					{
						if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
						{
							RenameCurrentSurfaceType(FName(*NewText.ToString()));
						}
					})
				]
			]
			+ SVerticalBox::Slot() 
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SHorizontalBox)

				+SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString("Rename Variant"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(0.7f)
				[
					
					SNew(SEditableTextBox)
					.Text_Lambda([this]() -> FText
					{
						FImpactProfileConfig* Config = GetCurrentImpactConfig();
						return Config ? FText::FromString(Config->VariantName) : FText::GetEmpty();
					})
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
					{
						if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
						{
							FImpactProfileConfig* Config = GetCurrentImpactConfig();
							if (Config)
							{
								Config->VariantName = NewText.ToString();
								if (TargetDataAsset.IsValid())
								{
									TargetDataAsset->MarkPackageDirty();
								}
								RefreshVariantIndexList();
							}
						}
					})
				]
			]
		];
}

TSharedRef<SWidget> SImpactProfileEditorWindow::CreatePreviewMeshSection()
{
 return SNew(SExpandableArea)
          .AreaTitle(FText::FromString("Preview Mesh"))
          .InitiallyCollapsed(false)
          .BodyContent()
          [
              SNew(SVerticalBox)

              // Show Checkbox
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f)
              [
                  SNew(SCheckBox)
                  .IsChecked_Lambda([this]() {
                      return Viewport.IsValid() && Viewport->IsPreviewMeshVisible() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                  })
                  .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) {
                      if (Viewport.IsValid())
                      {
                          Viewport->SetPreviewMeshVisible(NewState == ECheckBoxState::Checked);
                      }
                  })
                  [
                      SNew(STextBlock).Text(FText::FromString("Show Preview Mesh"))
                  ]
              ]

              // Mesh 선택
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f)
              [
                  SNew(SHorizontalBox)
                  + SHorizontalBox::Slot().FillWidth(0.3f).VAlign(VAlign_Center)
                  [ SNew(STextBlock).Text(FText::FromString("Mesh")) ]
                  + SHorizontalBox::Slot().FillWidth(0.7f)
                  [
                      SNew(SObjectPropertyEntryBox)
                      .AllowedClass(UStaticMesh::StaticClass())
                      .ObjectPath_Lambda([this]() -> FString
                      {
                          return (Viewport.IsValid() && Viewport->GetPreviewMesh())
                              ? Viewport->GetPreviewMesh()->GetPathName() : FString();
                      })
                      .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewMesh(Cast<UStaticMesh>(AssetData.GetAsset()));
                          }
                      })
                  ]
              ]

              // Location 라벨
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 8.0f, 4.0f, 4.0f)
              [
                  SNew(STextBlock)
                  .Text(FText::FromString("Location"))
                  .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
              ]

              // Location X, Y, Z
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(8.0f, 2.0f)
              [
                  SNew(SVectorInputBox)
                  .bColorAxisLabels(true)
                  .AllowSpin(true)
                  .X_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetPreviewMeshLocation().X : 0.0f; })
                  .Y_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetPreviewMeshLocation().Y : 0.0f; })
                  .Z_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetPreviewMeshLocation().Z : 0.0f; })
                  .OnXChanged_Lambda([this](float V) {
                      if (Viewport.IsValid()) {
                          FVector Loc = Viewport->GetPreviewMeshLocation();
                          Loc.X = V;
                          Viewport->SetPreviewMeshLocation(Loc);
                      }
                  })
                  .OnYChanged_Lambda([this](float V) {
                      if (Viewport.IsValid()) {
                          FVector Loc = Viewport->GetPreviewMeshLocation();
                          Loc.Y = V;
                          Viewport->SetPreviewMeshLocation(Loc);
                      }
                  })
                  .OnZChanged_Lambda([this](float V) {
                      if (Viewport.IsValid()) {
                          FVector Loc = Viewport->GetPreviewMeshLocation();
                          Loc.Z = V;
                          Viewport->SetPreviewMeshLocation(Loc);
                      }
                  })
              ]

              // Rotation 라벨
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 8.0f, 4.0f, 4.0f)
              [
                  SNew(STextBlock)
                  .Text(FText::FromString("Rotation"))
                  .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
              ]

              // Rotation Pitch, Yaw, Roll
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(8.0f, 2.0f)
              [
                  SNew(SRotatorInputBox)
                  .bColorAxisLabels(true)
                  .AllowSpin(true)
                  .Roll_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetPreviewMeshRotation().Roll : 0.0f; })
                  .Pitch_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetPreviewMeshRotation().Pitch : 0.0f; })
                  .Yaw_Lambda([this]() { return Viewport.IsValid() ? Viewport->GetPreviewMeshRotation().Yaw : 0.0f; })
                  .OnRollChanged_Lambda([this](float V) {
                      if (Viewport.IsValid()) {
                          FRotator Rot = Viewport->GetPreviewMeshRotation();
                          Rot.Roll = V;
                          Viewport->SetPreviewMeshRotation(Rot);
                      }
                  })
                  .OnPitchChanged_Lambda([this](float V) {
                      if (Viewport.IsValid()) {
                          FRotator Rot = Viewport->GetPreviewMeshRotation();
                          Rot.Pitch = V;
                          Viewport->SetPreviewMeshRotation(Rot);
                      }
                  })
                  .OnYawChanged_Lambda([this](float V) {
                      if (Viewport.IsValid()) {
                          FRotator Rot = Viewport->GetPreviewMeshRotation();
                          Rot.Yaw = V;
                          Viewport->SetPreviewMeshRotation(Rot);
                      }
                  })
              ]
	    ];
}

void SImpactProfileEditorWindow::SaveToComponent()
{
	if (!TargetComponent.IsValid() || !Viewport.IsValid())
	{
		return;
	}

	FVector CurrentDecalSize = Viewport->GetDecalSize();

	TargetComponent->bUseDecalSizeOverride = true;
	TargetComponent->DecalSizeOverride = CurrentDecalSize;

	FTransform DecalTransform = Viewport->GetDecalTransform();
	TargetComponent->DecalLocationOffset = DecalTransform.GetLocation();
	TargetComponent->DecalRotationOffset = DecalTransform.GetRotation().Rotator();

	TargetComponent->MarkPackageDirty();
}

void SImpactProfileEditorWindow::SaveToDataAsset()
{
	if (!TargetDataAsset.IsValid() || !Viewport.IsValid())
	{
		return;
	}

	// 현재 선택된 Config 가져오기
	FImpactProfileConfig* Config = GetCurrentImpactConfig();
	if (!Config)
	{
		return;
	}

	Config->DecalMaterial = SelectedDecalMaterial;
	Config->DecalSize = Viewport->GetDecalSize();

	FTransform DecalTransform = Viewport->GetDecalTransform();
	Config->LocationOffset = DecalTransform.GetLocation();
	Config->RotationOffset = DecalTransform.GetRotation().Rotator();

	Config->CylinderRadius = Viewport->GetPreviewCylinderRadius();
	Config->CylinderHeight = Viewport->GetPreviewCylinderHeight();
	Config->SphereRadius = Viewport->GetPreviewSphereRadius();
	Config->ToolShape = Viewport->GetPreviewToolShape(); 
	 
#if WITH_EDITORONLY_DATA
	// Tool Shape
	TargetDataAsset->ToolShapeLocationInEditor = Viewport->GetToolShapeLocation();
	TargetDataAsset->ToolShapeRotationInEditor = Viewport->GetToolShapeRotation();
	TargetDataAsset->SphereRadiusInEditor = Viewport->GetPreviewSphereRadius();
	TargetDataAsset->CylinderRadiusInEditor = Viewport->GetPreviewCylinderRadius();
	TargetDataAsset->CylinderHeightInEditor = Viewport->GetPreviewCylinderHeight();

	// Preview Mesh
	TargetDataAsset->PreviewMeshInEditor = Viewport->GetPreviewMesh();
	TargetDataAsset->PreviewMeshLocationInEditor = Viewport->GetPreviewMeshLocation();
	TargetDataAsset->PreviewMeshRotationInEditor = Viewport->GetPreviewMeshRotation();
#endif

	TargetDataAsset->MarkPackageDirty();
}


void SImpactProfileEditorWindow::LoadConfigFromDataAsset(FName ConfigID, FName SurfaceType)
{
	if (!TargetDataAsset.IsValid() || !Viewport.IsValid())
	{
		return;
	}

	FImpactProfileConfig Config;
	if (TargetDataAsset->GetConfig(SurfaceType, CurVariantIndex, Config))
	{
		SelectedDecalMaterial = Config.DecalMaterial;

		// Transform 먼저 설정
		FTransform DecalTransform;
		DecalTransform.SetLocation(Config.LocationOffset);
		DecalTransform.SetRotation(Config.RotationOffset.Quaternion());
		Viewport->SetDecalTransform(DecalTransform);
         
		Viewport->SetDecalSize(Config.DecalSize); 
		Viewport->SetDecalMaterial(Config.DecalMaterial); 

		// Tool Shape 로드 추가
		Viewport->SetPreviewCylinderRadius(Config.CylinderRadius);
		Viewport->SetPreviewCylinderHeight(Config.CylinderHeight);
		Viewport->SetPreviewSphere(Config.SphereRadius);
		Viewport->SetPreviewToolShape(Config.ToolShape);
	}

#if WITH_EDITOR
	// Tool Shape
	Viewport->SetToolShapeLocation(TargetDataAsset->ToolShapeLocationInEditor);
	Viewport->SetToolShapeRotation(TargetDataAsset->ToolShapeRotationInEditor);
	// Preview Mesh
	Viewport->SetPreviewMesh(TargetDataAsset->PreviewMeshInEditor.Get());
	Viewport->SetPreviewMeshLocation(TargetDataAsset->PreviewMeshLocationInEditor);
	Viewport->SetPreviewMeshRotation(TargetDataAsset->PreviewMeshRotationInEditor);
#endif
	Viewport->RefreshPreview();
} 
 

void SImpactProfileEditorWindow::RefreshConfigIDList()
{
	ConfigIDList.Empty();

	if (!TargetDataAsset.IsValid())
	{
		return;
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();

	 
	ConfigIDList.Add(MakeShared<FName>(DataAsset->ConfigID));
	 
}

void SImpactProfileEditorWindow::RefreshSurfaceTypeList()
{
	SurfaceTypeList.Empty();

	if (!TargetDataAsset.IsValid())
	{
		return;
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();
	 
	 
	// SurfaceConfigs의 모든 Key(SurfaceType) 추가
	TArray<FName> SurfaceKeys;
	DataAsset->SurfaceConfigs.GetKeys(SurfaceKeys);

	for (const FName& Key : SurfaceKeys)
	{
		SurfaceTypeList.Add(MakeShared<FName>(Key));
	} 
	 
	 
 
	CurVariantIndex = 0;
	RefreshVariantIndexList();
}

void SImpactProfileEditorWindow::RefreshVariantIndexList()
{
	VariantIndexList.Empty();

	FImpactProfileConfigArray* ConfigArray = GetCurrentImpactConfigArray();
	if (ConfigArray)
	{
		for (int32 i = 0; i < ConfigArray->Configs.Num(); i++)
		{
			const FImpactProfileConfig& Config = ConfigArray->Configs[i];
			FString DisplayName;
			if (Config.VariantName.IsEmpty())
			{
				DisplayName = FString::Printf(TEXT("%d"), i);
			}
			else
			{
				DisplayName = FString::Printf(TEXT("%d: %s"), i, *Config.VariantName);
			}
			VariantIndexList.Add(MakeShared<FString>(DisplayName));
		}
	}

	// 인덱스 범위 체크
	if (ConfigArray && ConfigArray->Configs.Num() > 0)
	{
		CurVariantIndex = FMath::Clamp(CurVariantIndex, 0, ConfigArray->Configs.Num() - 1);
	}
	else
	{
		CurVariantIndex = 0;
	}
	
}

FImpactProfileConfig* SImpactProfileEditorWindow::GetCurrentImpactConfig()
{
	FImpactProfileConfigArray* ConfigArray = GetCurrentImpactConfigArray();
	if (ConfigArray && ConfigArray->Configs.Num() > 0)
	{
		// 현재 선택된 인덱스의 Config 반환 (기본: 0)
		int32 Index = FMath::Clamp(CurVariantIndex, 0, ConfigArray->Configs.Num() - 1);
		return &ConfigArray->Configs[Index];
	}
	return nullptr;
}

FImpactProfileConfigArray* SImpactProfileEditorWindow::GetCurrentImpactConfigArray()
{
    
	if (!TargetDataAsset.IsValid() || CurrentSurfaceType.IsNone())
	{
		return nullptr;
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();
	return DataAsset->SurfaceConfigs.Find(CurrentSurfaceType);
}

void SImpactProfileEditorWindow::OnConfigIDSelected(FName SelectedConfigID)
{

	CurrentSurfaceType = NAME_None;

	RefreshSurfaceTypeList();
 
	if (SurfaceTypeList.Num() > 0)
	{
		CurrentSurfaceType = *SurfaceTypeList[0];
		OnSurfaceTypeSelected(CurrentSurfaceType);
	}

	//TODO: SUrface type 콤보박스 갱신

}

void SImpactProfileEditorWindow::OnSurfaceTypeSelected(FName SelectedSurfaceType)
{
	CurrentSurfaceType = SelectedSurfaceType;

	FImpactProfileConfig* Config = GetCurrentImpactConfig();
	if (Config)
	{
		// Material 멤버 변수 업데이트 (UI에서 사용)
		SelectedDecalMaterial = Config->DecalMaterial;

		if (Viewport.IsValid())
		{
			Viewport->SetDecalMaterial(Config->DecalMaterial);
			Viewport->SetDecalSize(Config->DecalSize);

			// Transform 설정
			FTransform DecalTransform;
			DecalTransform.SetLocation(Config->LocationOffset);
			DecalTransform.SetRotation(Config->RotationOffset.Quaternion());
			Viewport->SetDecalTransform(DecalTransform);

			// Tool Shape 설정
			Viewport->SetPreviewCylinderRadius(Config->CylinderRadius);
			Viewport->SetPreviewCylinderHeight(Config->CylinderHeight);
			Viewport->SetPreviewSphere(Config->SphereRadius);
			Viewport->SetPreviewToolShape(Config->ToolShape);

			Viewport->RefreshPreview();
		}
	}
	
	CurVariantIndex = 0;
	RefreshVariantIndexList();
}

void SImpactProfileEditorWindow::OnVariantIndexSelected(int32 SelectedIndex)
{
	CurVariantIndex = SelectedIndex;

	FImpactProfileConfig* Config = GetCurrentImpactConfig();
	if (Config && Viewport.IsValid())
	{
		SelectedDecalMaterial = Config->DecalMaterial;

		Viewport->SetDecalMaterial(SelectedDecalMaterial);
		Viewport->SetDecalSize(Config->DecalSize);
		
		// Transform 설정
		FTransform DecalTransform;
		DecalTransform.SetLocation(Config->LocationOffset);
		DecalTransform.SetRotation(Config->RotationOffset.Quaternion());
		Viewport->SetDecalTransform(DecalTransform);

	
		// Tool Shape 설정
		Viewport->SetPreviewCylinderRadius(Config->CylinderRadius);
		Viewport->SetPreviewCylinderHeight(Config->CylinderHeight);
		Viewport->SetPreviewSphere(Config->SphereRadius);
		Viewport->SetPreviewToolShape(Config->ToolShape);
		Viewport->RefreshPreview(); 
	}
}
 
void SImpactProfileEditorWindow::AddNewSurfaceType()
{
	if (!TargetDataAsset.IsValid())
	{
		return;
	}
	
	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();
	  
	// 고유한 SurfaceType 이름 생성
	FName NewSurfaceType = EnsureUniqueSurfaceType(FName("NewSurface"));

	// 새 DecalConfig 추가
	FImpactProfileConfigArray NewDecalConfig;
	NewDecalConfig.Configs.Add(FImpactProfileConfig());
	DataAsset->SurfaceConfigs.Add(NewSurfaceType, NewDecalConfig);

	DataAsset->MarkPackageDirty();

	// 목록 갱신 및 새 항목 선택
	RefreshSurfaceTypeList();
	OnSurfaceTypeSelected(NewSurfaceType); 
	 
}

void SImpactProfileEditorWindow::AddNewVariant()
{
	FImpactProfileConfigArray* ConfigArray = GetCurrentImpactConfigArray();

	if (!ConfigArray)
	{
		return;
	}

	FImpactProfileConfig NewConfig;
	if (ConfigArray->Configs.Num() > 0)
	{
		// Decal size를 다시 맞춰야하는 불편함을 없애기 위해서 curVariantIndex를 복사 
		NewConfig = ConfigArray->Configs[CurVariantIndex];
	}

	ConfigArray->Configs.Add(NewConfig);

	if (TargetDataAsset.IsValid())
	{
		TargetDataAsset->MarkPackageDirty();
	}

	// 새로 추가된 Variant 선택
	RefreshVariantIndexList();
	CurVariantIndex = ConfigArray->Configs.Num() - 1;
	OnVariantIndexSelected(CurVariantIndex);
}

FName SImpactProfileEditorWindow::EnsureUniqueConfigID(FName NewName)
{
	if (!TargetDataAsset.IsValid())
	{
		return FName();
	} 
	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();

	// 중복 체크
	auto IsConfigIDExists = [&]( FName Name )->bool
	{ 
		 
		if (DataAsset->ConfigID == Name)
		{
			return true;
		}
 

		return false;
	};
	
	if (!IsConfigIDExists(NewName))
	{
		return NewName;
	}

	// 중복이면 숫자 붙이기
	FString BaseString = NewName.ToString();
	int32 Counter = 1;

	while (true)
	{
		FName TempName = FName(*FString::Printf(TEXT("%s_%d"), *BaseString, Counter));
		if (!IsConfigIDExists(TempName))
		{
			return TempName;
		}
		Counter++;
	}
}

FName SImpactProfileEditorWindow::EnsureUniqueSurfaceType(FName NewName)
{  
	if (!TargetDataAsset.IsValid())
	{
		return FName();
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();
	  
	if (!DataAsset->SurfaceConfigs.Contains(NewName))
	{
		return NewName;
	}

	// 중복이면 숫자 붙이기
	FString BaseString = NewName.ToString();
	int32 Counter = 1;

	while (true)
	{
		FName TempName = FName(*FString::Printf(TEXT("%s_%d"), *BaseString, Counter));
		if (!DataAsset->SurfaceConfigs.Contains(TempName))
		{
			return TempName;
		}
		Counter++;
	}
 
 

	return FName();
}

void SImpactProfileEditorWindow::DeleteCurrentConfigID()
{
	if (!TargetDataAsset.IsValid())
	{
		return;
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();
	
	DataAsset->ConfigID = NAME_None;
	DataAsset->SurfaceConfigs.Empty();
 
	DataAsset->MarkPackageDirty();

	// 목록 갱신 
	CurrentSurfaceType = NAME_None;
	RefreshConfigIDList();

	// 첫 번째 항목 선택 (있다면)
	if (ConfigIDList.Num() > 0)
	{
		OnConfigIDSelected(*ConfigIDList[0]);
	}
}

void SImpactProfileEditorWindow::DeleteCurrentSurfaceType()
{
	if (!TargetDataAsset.IsValid() || CurrentSurfaceType.IsNone())
	{
		return;
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();

  
	DataAsset->SurfaceConfigs.Remove(CurrentSurfaceType);
	DataAsset->MarkPackageDirty();

	// 목록 갱신
	CurrentSurfaceType = NAME_None;
	RefreshSurfaceTypeList();

	// 첫 번째 항목 선택 (있다면)
	if (SurfaceTypeList.Num() > 0)
	{
		OnSurfaceTypeSelected(*SurfaceTypeList[0]);
	}
  
	
}

void SImpactProfileEditorWindow::DeleteCurrentVariant()
{
	FImpactProfileConfigArray* ConfigArray = GetCurrentImpactConfigArray();
	if (!ConfigArray || ConfigArray->Configs.Num() <= 1)
	{
		// 최소 1개는 유지
		return;
	}

	ConfigArray->Configs.RemoveAt(CurVariantIndex);

	if (TargetDataAsset.IsValid())
	{
		TargetDataAsset->MarkPackageDirty();
	}

	// 인덱스 조정
	RefreshVariantIndexList();
	CurVariantIndex = FMath::Clamp(CurVariantIndex - 1, 0, ConfigArray->Configs.Num() - 1);
	OnVariantIndexSelected(CurVariantIndex);
}

void SImpactProfileEditorWindow::RenameCurrentConfigID(FName NewName)
{
	if (NewName.IsNone() || !TargetDataAsset.IsValid())
	{
		return;
	}

	UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();

	// 변경 전 ConfigID 저장
	FName OldConfigID = DataAsset->ConfigID;

	// 같은 이름이면 무시
	if (OldConfigID == NewName)
	{
		return;
	}

	NewName = EnsureUniqueConfigID(NewName);

	// Data Asset의 ConfigID 변경
	DataAsset->ConfigID = NewName;
	DataAsset->MarkPackageDirty();

	// Subsystem의 Map Key도 업데이트
	if (GEditor)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			if (UGameInstance* GI = World->GetGameInstance())
			{
				if (UDestructionGameInstanceSubsystem* Subsystem = GI->GetSubsystem<UDestructionGameInstanceSubsystem>())
				{
					Subsystem->RenameConfigID(OldConfigID, NewName);
				}
			}
		}
	}

	// Project Settings (RDMSetting)의 Entry ConfigID도 업데이트
	if (URDMSetting* Settings = URDMSetting::Get())
	{
		Settings->UpdateEntryConfigID(OldConfigID, NewName);
	}

	RefreshConfigIDList();
}

void SImpactProfileEditorWindow::RenameCurrentSurfaceType(FName NewName)
{
    if (NewName.IsNone() || NewName == CurrentSurfaceType || !TargetDataAsset.IsValid()  )
    {
        return;
    }

    UImpactProfileDataAsset* DataAsset = TargetDataAsset.Get();

 
            FImpactProfileConfigArray* ExistingConfig = DataAsset->SurfaceConfigs.Find(CurrentSurfaceType);

            if (!ExistingConfig)
            {
                return; 
            }

            if (DataAsset->SurfaceConfigs.Contains(NewName))
            {
                NewName = EnsureUniqueSurfaceType(NewName);
            }

            FImpactProfileConfigArray ConfigCopy = *ExistingConfig;
            DataAsset->SurfaceConfigs.Remove(CurrentSurfaceType);
            DataAsset->SurfaceConfigs.Add(NewName, ConfigCopy);

            CurrentSurfaceType = NewName;
            DataAsset->MarkPackageDirty();
            RefreshSurfaceTypeList(); 
        
    
}

#undef LOCTEXT_NAMESPACE
