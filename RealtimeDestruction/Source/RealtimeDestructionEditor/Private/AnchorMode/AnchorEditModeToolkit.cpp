// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

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
