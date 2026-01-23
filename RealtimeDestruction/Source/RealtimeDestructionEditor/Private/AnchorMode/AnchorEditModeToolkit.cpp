// Fill out your copyright notice in the Description page of Project Settings.


#include "AnchorMode/AnchorEditModeToolkit.h"

#include "EditorModeManager.h"
#include "RealtimeDestructibleMeshComponent.h"
#include "AnchorMode/AnchorEditMode.h"
#include "AnchorMode/AnchorActionObejct.h"


struct FGridCellLayout;

void FAnchorEditModeToolkit::Init(const TSharedPtr<IToolkitHost>& InitToolkitHost)
{
	FModeToolkit::Init(InitToolkitHost);
}

TSharedPtr<SWidget> FAnchorEditModeToolkit::GetInlineContent() const
{
	UAnchorEditMode* AnchorMode = Cast<UAnchorEditMode>(
		GLevelEditorModeTools().GetActiveScriptableMode(UAnchorEditMode::EM_AnchorEditModeId));

	if (!AnchorMode || !AnchorMode->ActionObject)
	{
		return SNullWidget::NullWidget;
	}

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Automatic;

	// TSharedPtr<IDetailsView> AnchorDetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	const_cast<FAnchorEditModeToolkit*>(this)->AnchorDetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);

	AnchorDetailsView->SetObject(AnchorMode->ActionObject);	

	return AnchorDetailsView;
}

void FAnchorEditModeToolkit::ForceRefreshDetails()
{
	if (AnchorDetailsView.IsValid())
	{
		AnchorDetailsView->ForceRefresh();
	}
}
