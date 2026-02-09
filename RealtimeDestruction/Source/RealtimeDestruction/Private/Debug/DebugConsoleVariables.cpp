// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.


#include "Debug/DebugConsoleVariables.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING

static TAutoConsoleVariable<int32> CVarSimplifyEnable(
	TEXT("RDM.Enable.Simplify"),
	1,
	TEXT("0=off, 1=on"),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarSimplifyMode(
	TEXT("RDM.Simplify.Mode"),
	0,
	TEXT("0=Protect Mat, 1=No Protect Mat"),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarBooleanAsync(
	TEXT("RDM.Enable.BooleanAsync"),
	1,
	TEXT("0=Sync, 1=Async"),
	ECVF_Cheat);

#endif

int32 FRDMCVarHelper::EnableSimplify()
{
#if !UE_BUILD_SHIPPING
	return CVarSimplifyEnable.GetValueOnAnyThread() != 0;
#else
	return 1;
#endif
}

int32 FRDMCVarHelper::GetSimplifyMode()
{
#if !UE_BUILD_SHIPPING
	return CVarSimplifyMode.GetValueOnAnyThread();
#else
	return 2;
#endif
}

int32 FRDMCVarHelper::EnableAsyncBooleanOp()
{
#if !UE_BUILD_SHIPPING
	return CVarBooleanAsync.GetValueOnAnyThread() != 0;
#else
	return 1;
#endif
}
