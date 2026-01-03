// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogRealtimeDestruction, Log, All);

class FRealtimeDestructionModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static inline FRealtimeDestructionModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FRealtimeDestructionModule>("RealtimeDestruction");
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("RealtimeDestruction");
	}
};
