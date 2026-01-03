// Copyright Epic Games, Inc. All Rights Reserved.

#include "RealtimeDestruction.h"

#define LOCTEXT_NAMESPACE "FRealtimeDestructionModule"

DEFINE_LOG_CATEGORY(LogRealtimeDestruction);

void FRealtimeDestructionModule::StartupModule()
{
	UE_LOG(LogRealtimeDestruction, Log, TEXT("RealtimeDestruction module started"));
}

void FRealtimeDestructionModule::ShutdownModule()
{
	UE_LOG(LogRealtimeDestruction, Log, TEXT("RealtimeDestruction module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRealtimeDestructionModule, RealtimeDestruction)
