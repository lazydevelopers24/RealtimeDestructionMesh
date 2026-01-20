#pragma once
#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FRdmSettingsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	void OnPercentageChanged();
	void UpdateConfigIDs();
	
	TWeakObjectPtr<class URDMSetting> SettingsPtr;
	TSharedPtr<class STextBlock> ResultTextBlock;
};