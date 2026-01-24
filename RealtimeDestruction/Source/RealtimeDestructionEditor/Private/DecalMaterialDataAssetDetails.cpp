// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "DecalMaterialDataAssetDetails.h"

#include "DecalSizeEditorWindow.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Data/DecalMaterialDataAsset.h"

TSharedRef<IDetailCustomization> FDecalMaterialDataAssetDetails::MakeInstance()
{
	return MakeShareable(new FDecalMaterialDataAssetDetails);
}

void FDecalMaterialDataAssetDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	if ( Objects.Num() ==  0)
	{
		return;
	}

	TargetDataAsset = Cast<UDecalMaterialDataAsset>(Objects[0].Get());
	IDetailCategoryBuilder& DecalCategory = DetailBuilder.EditCategory("Decal");

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
			 .OnClicked(this, &FDecalMaterialDataAssetDetails::OnOpenEditorClicked)
		 ];
}

FReply FDecalMaterialDataAssetDetails::OnOpenEditorClicked()
{
	if (TargetDataAsset.IsValid())
	{
		SDecalSizeEditorWindow::OpenWindowForDataAsset(TargetDataAsset.Get()); 
	}

	return FReply::Handled();
}
