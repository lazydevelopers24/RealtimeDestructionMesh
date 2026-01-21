// Fill out your copyright notice in the Description page of Project Settings.

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
