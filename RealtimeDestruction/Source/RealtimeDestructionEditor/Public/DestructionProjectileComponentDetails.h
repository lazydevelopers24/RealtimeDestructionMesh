#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class UDestructionProjectileComponent;

class FDestructionProjectileComponentDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TWeakObjectPtr<UDestructionProjectileComponent> TargetComponent;

	FReply OnOpenEditorClicked();
};