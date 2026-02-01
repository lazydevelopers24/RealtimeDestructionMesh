// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "ImpactProfileAssetDetails.h"

#include "ImpactProfileEditorWindow.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Data/ImpactProfileDataAsset.h"

TSharedRef<IDetailCustomization> FImpactProfileAssetDetails::MakeInstance()
{
	return MakeShareable(new FImpactProfileAssetDetails);
}

void FImpactProfileAssetDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	if ( Objects.Num() ==  0)
	{
		return;
	}

	TargetDataAsset = Cast<UImpactProfileDataAsset>(Objects[0].Get());
	IDetailCategoryBuilder& DecalCategory = DetailBuilder.EditCategory("Decal");

	DecalCategory.AddCustomRow(FText::FromString("Open Impact Profile Editor"))
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
			 .Text(FText::FromString("Open Impact Profile Editor"))
			 .HAlign(HAlign_Center)
			 .OnClicked(this, &FImpactProfileAssetDetails::OnOpenEditorClicked)
		 ];
}

FReply FImpactProfileAssetDetails::OnOpenEditorClicked()
{
	if (TargetDataAsset.IsValid())
	{
		SImpactProfileEditorWindow::OpenWindowForDataAsset(TargetDataAsset.Get()); 
	}

	return FReply::Handled();
}
