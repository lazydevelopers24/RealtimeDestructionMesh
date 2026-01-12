#include "DestructionProjectileComponentDetails.h"  

#include "DestructionProjectileComponent.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "DecalSizeEditorWindow.h"

TSharedRef<IDetailCustomization> FDestructionProjectileComponentDetails::MakeInstance()
{
      return MakeShareable(new FDestructionProjectileComponentDetails);
}

void FDestructionProjectileComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
      TArray<TWeakObjectPtr<UObject>> Objects;

      DetailBuilder.GetObjectsBeingCustomized(Objects);

      if (Objects.Num() == 0 )
      {
            return;
      }
      TargetComponent = Cast<UDestructionProjectileComponent>(Objects[0].Get());

      IDetailCategoryBuilder& DecalCategory = DetailBuilder.EditCategory("Destruction|Decal");

      DecalCategory.AddCustomRow(FText::FromString("Open Decal Size Editor"))
      .NameContent()
      [
            SNew(STextBlock)
            .Text(FText::FromString("Decal Editor"))  
            .Font(IDetailLayoutBuilder::GetDetailFont())
      ]
      .ValueContent()
      .MaxDesiredWidth(200.f)
      [
          SNew(SButton)
          .Text(FText::FromString("Open Decal Size Editor"))
          .HAlign(HAlign_Center)
          .OnClicked(this, &FDestructionProjectileComponentDetails::OnOpenEditorClicked)
      ];
}


FReply FDestructionProjectileComponentDetails::OnOpenEditorClicked()
{
      if (TargetComponent.IsValid())
      {
            SDecalSizeEditorWindow::OpenWindow(TargetComponent.Get());
      }
      return FReply::Handled();
}


