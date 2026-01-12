#include "DecalSizeEditorWindow.h"
#include "DecalSizeEditorViewport.h"
#include "Components/DestructionProjectileComponent.h"

#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "BoneControllers/AnimNode_AnimDynamics.h"
#include "Widgets/Input/SSpinBox.h"
#include "PropertyCustomizationHelpers.h"

#define LOCTEXT_NAMESPACE "DecalSizeEditorWindow"

static const FName DecalSizeEditorTabId("DecalSizeEditorTab");

void SDecalSizeEditorWindow::Construct(const FArguments& InArgs)
{
	TargetComponent = InArgs._TargetComponent;

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
			if (InRowName == FName("Open Decal Size Editor"))
			{
				return false;
			}
			return true;
		}));
	
	// Target Component를 Dectai View에 설정
	if (TargetComponent.IsValid())
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
				SAssignNew(Viewport, SDecalSizeEditorViewport)
				.TargetComponent(TargetComponent.Get())
			]
		]

		// 오른쪽: Property Panel
		+ SSplitter::Slot()
		.Value(0.3f)
		[
			SNew(SVerticalBox)

              // 타이틀
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(8.0f)
              [
                  SNew(STextBlock)
                  .Text(LOCTEXT("Title", "Decal Size Editor"))
                  .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
              ]

              // Decal Transform 섹션
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f)
              [
                  CreateDecalTransformSection()
              ]

	          + SVerticalBox::Slot()
	            .AutoHeight()
	            .Padding(4.0f)
		    [
		        CreateMaterialSection()
		    ]
                  // Tool Shape 섹션 (Radius, Height만)
                  + SVerticalBox::Slot()
                  .AutoHeight()
                  .Padding(4.0f)
                  [
                      CreateToolShapeSection()
                  ]

                  // DetailsView (기타 프로퍼티)
                  + SVerticalBox::Slot()
                  .FillHeight(1.0f)
                  .Padding(4.0f)
                  [
                      DetailsView.ToSharedRef()
                  ]

                  // Refresh 버튼
                  + SVerticalBox::Slot()
                  .AutoHeight()
                  .Padding(8.0f)
                  [
                      SNew(SButton)
                      .Text(LOCTEXT("Refresh", "Refresh Preview"))
                      .HAlign(HAlign_Center)
                      .OnClicked_Lambda([this]()
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->RefreshPreview();
                          }
                          return FReply::Handled();
                      })
                  ]
              ] 
      ];
  }


void SDecalSizeEditorWindow::SetTargetComponent(UDestructionProjectileComponent* InComponent)
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

void SDecalSizeEditorWindow::OpenWindow(UDestructionProjectileComponent* Component)
{
	// 독립 윈도우로 열기
	
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("DecalSizeEditorTitle", "Decal Size Editor"))
		.ClientSize(FVector2D(1200, 600))
		.SupportsMinimize(true)
		.SupportsMaximize(true);

	
	TSharedRef<SDecalSizeEditorWindow> EditorWidget = SNew(SDecalSizeEditorWindow)
		.TargetComponent(Component);

	Window->SetContent(EditorWidget);

	FSlateApplication::Get().AddWindow(Window);
}

TSharedRef<SWidget> SDecalSizeEditorWindow::CreateMaterialSection()
{
	
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("DecalMaterial", "Decal Material"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SVerticalBox)

			// Material 선택
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
					.AllowedClass(UMaterialInterface::StaticClass())
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
						}
					})
				]
			] 

            + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(4.0f, 8.0f, 4.0f, 4.0f)
                [
                    SNew(STextBlock)
                        .Text(FText::FromString("Decal Size (Depth, Width, Height)"))
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
                                        .MinValue(1.0f).MaxValue(100.0f)
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

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f)
            [
                SNew(SButton)
                .Text(FText::FromString("Apply DecalSize to Component"))
                .HAlign(HAlign_Center)
                .OnClicked_Lambda([this](){
                    
                    if (TargetComponent.IsValid() && Viewport.IsValid())
                    {
                        FVector CurrentDecalSize = Viewport->GetDecalSize();

                        TargetComponent->bUseDecalSizeOverride = true;
                        TargetComponent->DecalSizeOverride = CurrentDecalSize;

                        TargetComponent->MarkPackageDirty();
                    }

                    return FReply::Handled();
                })
            ]
		];
}

void SDecalSizeEditorWindow::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent,
                                              FProperty* PropertyThatChanged)
{
	if (Viewport.IsValid())
	{
		Viewport->RefreshPreview();
	}
}

TSharedRef<SWidget> SDecalSizeEditorWindow::CreateDecalTransformSection()
{
	 return SNew(SExpandableArea)
          .AreaTitle(LOCTEXT("DecalTransform", "Decal Transform"))
          .InitiallyCollapsed(false)
          .BodyContent()
          [
              SNew(SVerticalBox)

              // ===== Location =====
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 4.0f)
              [
                  SNew(STextBlock)
                  .Text(FText::FromString("Location"))
                  .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
              ]

              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(8.0f, 2.0f)
              [
                  SNew(SHorizontalBox)

                  // X
                  + SHorizontalBox::Slot()
                  .FillWidth(0.33f)
                  [
                      SNew(SHorizontalBox)
                      + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
                      [ SNew(STextBlock).Text(FText::FromString("X")) ]
                      + SHorizontalBox::Slot().FillWidth(1.0f)
                      [
                          SNew(SSpinBox<float>)
                          .MinValue(-1000.0f).MaxValue(1000.0f)
                          .Value_Lambda([this]() {
                              return Viewport.IsValid() ? Viewport->GetDecalTransform().GetLocation().X : 0.0f;
                          })
                          .OnValueChanged_Lambda([this](float V) {
                              if (Viewport.IsValid()) {
                                  FTransform T = Viewport->GetDecalTransform();
                                  FVector Loc = T.GetLocation();
                                  Loc.X = V;
                                  T.SetLocation(Loc);
                                  Viewport->SetDecalTransform(T);
                              }
                          })
                      ]
                  ]

                  // Y
                  + SHorizontalBox::Slot()
                  .FillWidth(0.33f)
                  .Padding(4.0f, 0.0f)
                  [
                      SNew(SHorizontalBox)
                      + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
                      [ SNew(STextBlock).Text(FText::FromString("Y")) ]
                      + SHorizontalBox::Slot().FillWidth(1.0f)
                      [
                          SNew(SSpinBox<float>)
                          .MinValue(-1000.0f).MaxValue(1000.0f)
                          .Value_Lambda([this]() {
                              return Viewport.IsValid() ? Viewport->GetDecalTransform().GetLocation().Y : 0.0f;
                          })
                          .OnValueChanged_Lambda([this](float V) {
                              if (Viewport.IsValid()) {
                                  FTransform T = Viewport->GetDecalTransform();
                                  FVector Loc = T.GetLocation();
                                  Loc.Y = V;
                                  T.SetLocation(Loc);
                                  Viewport->SetDecalTransform(T);
                              }
                          })
                      ]
                  ]

                  // Z
                  + SHorizontalBox::Slot()
                  .FillWidth(0.33f)
                  .Padding(4.0f, 0.0f)
                  [
                      SNew(SHorizontalBox)
                      + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
                      [ SNew(STextBlock).Text(FText::FromString("Z")) ]
                      + SHorizontalBox::Slot().FillWidth(1.0f)
                      [
                          SNew(SSpinBox<float>)
                          .MinValue(-1000.0f).MaxValue(1000.0f)
                          .Value_Lambda([this]() {
                              return Viewport.IsValid() ? Viewport->GetDecalTransform().GetLocation().Z : 0.0f;
                          })
                          .OnValueChanged_Lambda([this](float V) {
                              if (Viewport.IsValid()) {
                                  FTransform T = Viewport->GetDecalTransform();
                                  FVector Loc = T.GetLocation();
                                  Loc.Z = V;
                                  T.SetLocation(Loc);
                                  Viewport->SetDecalTransform(T);
                              }
                          })
                      ]
                  ]
              ]

              // ===== Rotation =====
              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(4.0f, 8.0f, 4.0f, 4.0f)
              [
                  SNew(STextBlock)
                  .Text(FText::FromString("Rotation"))
                  .Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
              ]

              + SVerticalBox::Slot()
              .AutoHeight()
              .Padding(8.0f, 2.0f)
              [
                  SNew(SHorizontalBox)

                  // Pitch
                  + SHorizontalBox::Slot()
                  .FillWidth(0.33f)
                  [
                      SNew(SHorizontalBox)
                      + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
                      [ SNew(STextBlock).Text(FText::FromString("P")) ]
                      + SHorizontalBox::Slot().FillWidth(1.0f)
                      [
                          SNew(SSpinBox<float>)
                          .MinValue(-180.0f).MaxValue(180.0f)
                          .Value_Lambda([this]() {
                              return Viewport.IsValid() ? Viewport->GetDecalTransform().GetRotation().Rotator().Pitch : 0.0f;
                          })
                          .OnValueChanged_Lambda([this](float V) {
                              if (Viewport.IsValid()) {
                                  FTransform T = Viewport->GetDecalTransform();
                                  FRotator Rot = T.GetRotation().Rotator();
                                  Rot.Pitch = V;
                                  T.SetRotation(Rot.Quaternion());
                                  Viewport->SetDecalTransform(T);
                              }
                          })
                      ]
                  ]

                  // Yaw
                  + SHorizontalBox::Slot()
                  .FillWidth(0.33f)
                  .Padding(4.0f, 0.0f)
                  [
                      SNew(SHorizontalBox)
                      + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
                      [ SNew(STextBlock).Text(FText::FromString("Y")) ]
                      + SHorizontalBox::Slot().FillWidth(1.0f)
                      [
                          SNew(SSpinBox<float>)
                          .MinValue(-180.0f).MaxValue(180.0f)
                          .Value_Lambda([this]() {
                              return Viewport.IsValid() ? Viewport->GetDecalTransform().GetRotation().Rotator().Yaw : 0.0f;
                          })
                          .OnValueChanged_Lambda([this](float V) {
                              if (Viewport.IsValid()) {
                                  FTransform T = Viewport->GetDecalTransform();
                                  FRotator Rot = T.GetRotation().Rotator();
                                  Rot.Yaw = V;
                                  T.SetRotation(Rot.Quaternion());
                                  Viewport->SetDecalTransform(T);
                              }
                          })
                      ]
                  ]

                  // Roll
                  + SHorizontalBox::Slot()
                  .FillWidth(0.33f)
                  .Padding(4.0f, 0.0f)
                  [
                      SNew(SHorizontalBox)
                      + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
                      [ SNew(STextBlock).Text(FText::FromString("R")) ]
                      + SHorizontalBox::Slot().FillWidth(1.0f)
                      [
                          SNew(SSpinBox<float>)
                          .MinValue(-180.0f).MaxValue(180.0f)
                          .Value_Lambda([this]() {
                              return Viewport.IsValid() ? Viewport->GetDecalTransform().GetRotation().Rotator().Roll : 0.0f;
                          })
                          .OnValueChanged_Lambda([this](float V) {
                              if (Viewport.IsValid()) {
                                  FTransform T = Viewport->GetDecalTransform();
                                  FRotator Rot = T.GetRotation().Rotator();
                                  Rot.Roll = V;
                                  T.SetRotation(Rot.Quaternion());
                                  Viewport->SetDecalTransform(T);
                              }
                          })
                      ]
                  ]
              ]
  
          ];
}

TSharedRef<SWidget> SDecalSizeEditorWindow::CreateToolShapeSection()
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
      SNew(SHorizontalBox)

      // X
      + SHorizontalBox::Slot()
      .FillWidth(0.33f)
      [
          SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
          [ SNew(STextBlock).Text(FText::FromString("X")) ]
          + SHorizontalBox::Slot().FillWidth(1.0f)
          [
              SNew(SSpinBox<float>)
              .MinValue(-1000.0f).MaxValue(1000.0f)
              .Value_Lambda([this]() {
                  return Viewport.IsValid() ? Viewport->GetToolShapeLocation().X : 0.0f;
              })
              .OnValueChanged_Lambda([this](float V) {
                  if (Viewport.IsValid()) {
                      FVector Loc = Viewport->GetToolShapeLocation();
                      Loc.X = V;
                      Viewport->SetToolShapeLocation(Loc);
                  }
              })
          ]
      ]

      // Y
      + SHorizontalBox::Slot()
      .FillWidth(0.33f)
      .Padding(4.0f, 0.0f)
      [
          SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
          [ SNew(STextBlock).Text(FText::FromString("Y")) ]
          + SHorizontalBox::Slot().FillWidth(1.0f)
          [
              SNew(SSpinBox<float>)
              .MinValue(-1000.0f).MaxValue(1000.0f)
              .Value_Lambda([this]() {
                  return Viewport.IsValid() ? Viewport->GetToolShapeLocation().Y : 0.0f;
              })
              .OnValueChanged_Lambda([this](float V) {
                  if (Viewport.IsValid()) {
                      FVector Loc = Viewport->GetToolShapeLocation();
                      Loc.Y = V;
                      Viewport->SetToolShapeLocation(Loc);
                  }
              })
          ]
      ]

      // Z
      + SHorizontalBox::Slot()
      .FillWidth(0.33f)
      .Padding(4.0f, 0.0f)
      [
          SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
          [ SNew(STextBlock).Text(FText::FromString("Z")) ]
          + SHorizontalBox::Slot().FillWidth(1.0f)
          [
              SNew(SSpinBox<float>)
              .MinValue(-1000.0f).MaxValue(1000.0f)
              .Value_Lambda([this]() {
                  return Viewport.IsValid() ? Viewport->GetToolShapeLocation().Z : 0.0f;
              })
              .OnValueChanged_Lambda([this](float V) {
                  if (Viewport.IsValid()) {
                      FVector Loc = Viewport->GetToolShapeLocation();
                      Loc.Z = V;
                      Viewport->SetToolShapeLocation(Loc);
                  }
              })
          ]
      ]
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
      SNew(SHorizontalBox)

      // Pitch
      + SHorizontalBox::Slot()
      .FillWidth(0.33f)
      [
          SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
          [ SNew(STextBlock).Text(FText::FromString("P")) ]
          + SHorizontalBox::Slot().FillWidth(1.0f)
          [
              SNew(SSpinBox<float>)
              .MinValue(-180.0f).MaxValue(180.0f)
              .Value_Lambda([this]() {
                  return Viewport.IsValid() ? Viewport->GetToolShapeRotation().Pitch : 0.0f;
              })
              .OnValueChanged_Lambda([this](float V) {
                  if (Viewport.IsValid()) {
                      FRotator Rot = Viewport->GetToolShapeRotation();
                      Rot.Pitch = V;
                      Viewport->SetToolShapeRotation(Rot);
                  }
              })
          ]
      ]

      // Yaw
      + SHorizontalBox::Slot()
      .FillWidth(0.33f)
      .Padding(4.0f, 0.0f)
      [
          SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
          [ SNew(STextBlock).Text(FText::FromString("Y")) ]
          + SHorizontalBox::Slot().FillWidth(1.0f)
          [
              SNew(SSpinBox<float>)
              .MinValue(-180.0f).MaxValue(180.0f)
              .Value_Lambda([this]() {
                  return Viewport.IsValid() ? Viewport->GetToolShapeRotation().Yaw : 0.0f;
              })
              .OnValueChanged_Lambda([this](float V) {
                  if (Viewport.IsValid()) {
                      FRotator Rot = Viewport->GetToolShapeRotation();
                      Rot.Yaw = V;
                      Viewport->SetToolShapeRotation(Rot);
                  }
              })
          ]
      ]

      // Roll
      + SHorizontalBox::Slot()
      .FillWidth(0.33f)
      .Padding(4.0f, 0.0f)
      [
          SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
          [ SNew(STextBlock).Text(FText::FromString("R")) ]
          + SHorizontalBox::Slot().FillWidth(1.0f)
          [
              SNew(SSpinBox<float>)
              .MinValue(-180.0f).MaxValue(180.0f)
              .Value_Lambda([this]() {
                  return Viewport.IsValid() ? Viewport->GetToolShapeRotation().Roll : 0.0f;
              })
              .OnValueChanged_Lambda([this](float V) {
                  if (Viewport.IsValid()) {
                      FRotator Rot = Viewport->GetToolShapeRotation();
                      Rot.Roll = V;
                      Viewport->SetToolShapeRotation(Rot);
                  }
              })
          ]
      ]
  ]
              // Sphere Radius
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

	              +SHorizontalBox::Slot()
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
									 {
										 return FText::FromString("Sphere");
									 }
								 case EDestructionToolShape::Cylinder:
									 {
										 return FText::FromString("Cylinder");
									 }
								 	default:
									 {
										 return FText::FromString("Cylinder");
									 }
								 }
							 })
						 ]
	              ]
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
                      .Value(InitSphereRadius)
                      .OnValueChanged_Lambda([this](float V)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewSphere(V);
                          }
                      })
                  ]
              ]

              // Cylinder Radius
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
                      .Text(FText::FromString("Cylinder Radius"))
                  ]

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  [
                      SNew(SSpinBox<float>)
                      .MinValue(1.0f)
                      .MaxValue(1000.0f)
                      .Value(InitCylinderRadius)
                      .OnValueChanged_Lambda([this](float V)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewCylinderRadius(V);
                          }
                      })
                  ]
              ]

              // Cylinder Height
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
                      .Text(FText::FromString("Cylinder Height"))
                  ]

                  + SHorizontalBox::Slot()
                  .FillWidth(0.5f)
                  [
                      SNew(SSpinBox<float>)
                      .MinValue(1.0f)
                      .MaxValue(2000.0f)
                      .Value(InitCylinderHeight)
                      .OnValueChanged_Lambda([this](float V)
                      {
                          if (Viewport.IsValid())
                          {
                              Viewport->SetPreviewCylinderHeight(V);
                          }
                      })
                  ]
              ]
          ];
}

#undef LOCTEXT_NAMESPACE
