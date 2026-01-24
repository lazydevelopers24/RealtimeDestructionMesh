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
#include "Toolkits/BaseToolkit.h"

/**
 * 
 */
class REALTIMEDESTRUCTIONEDITOR_API FAnchorEditModeToolkit : public FModeToolkit
{
public:
	FAnchorEditModeToolkit() = default;
	~FAnchorEditModeToolkit()= default;

	virtual void Init(const TSharedPtr<IToolkitHost>& InitToolkitHost) override;

	virtual FName GetToolkitFName() const override { return FName("AnchorEditModeToolkit"); }
	virtual FText GetBaseToolkitName() const override { return FText::FromString("Anchor Editor"); }
	
	virtual TSharedPtr<SWidget> GetInlineContent() const override;

	void ForceRefreshDetails();

private:
	TSharedPtr<IDetailsView> AnchorDetailsView;
};
