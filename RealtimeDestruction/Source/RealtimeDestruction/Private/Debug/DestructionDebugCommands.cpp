// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// DestructionDebugCommands.cpp
// 파괴 디버거용 콘솔 명령어
//
// 사용 가능한 명령어:
// - destruction.all [0/1]           : 모든 기능 한번에 켜기/끄기
// - destruction.debug [0/1]         : 디버거 활성화/비활성화
// - destruction.vis [0/1]           : 시각화 활성화/비활성화
// - destruction.hud [0/1]           : HUD 활성화/비활성화
// - destruction.stats               : 기본 통계 출력
// - destruction.net                 : 네트워크 통계 출력
// - destruction.clients             : 클라이언트별 통계 출력 (서버만)
// - destruction.perf                : 성능 통계 출력
// - destruction.history [count]     : 히스토리 출력
// - destruction.clear               : 히스토리 클리어
// - destruction.reset               : 기본 통계 리셋
// - destruction.resetall            : 모든 통계 리셋
// - destruction.filter [actor] [radius] : 필터 설정
// - destruction.export [history|stats] [path] : CSV 내보내기
// - destruction.summary             : 세션 요약 출력

#include "Debug/DestructionDebugger.h"
#include "Debug/DestructionProfiler.h"
#include "Testing/NetworkTestSubsystem.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

//-------------------------------------------------------------------
// destruction.all - 모든 기능 토글 (또는 0/1로 직접 지정)
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionAllCmd(
	TEXT("destruction.all"),
	TEXT("Toggle ALL destruction debug features. Usage: destruction.all [0/1]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.all: No world available"));
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.all: Debugger subsystem not found"));
			return;
		}

		bool bEnable;
		if (Args.Num() > 0)
		{
			bEnable = FCString::Atoi(*Args[0]) != 0;
		}
		else
		{
			// 토글: 하나라도 켜져있으면 전부 끄고, 전부 꺼져있으면 전부 켜기
			bEnable = !(Debugger->IsEnabled() && Debugger->IsVisualizationEnabled() && Debugger->IsHUDEnabled());
		}

		Debugger->SetEnabled(bEnable);
		Debugger->SetVisualizationEnabled(bEnable);
		Debugger->SetHUDEnabled(bEnable);

		UE_LOG(LogTemp, Log, TEXT("destruction.all: All features %s"),
			bEnable ? TEXT("ENABLED") : TEXT("DISABLED"));
	})
);

//-------------------------------------------------------------------
// destruction.debug [0/1] - 디버거 활성화/비활성화
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionDebugCmd(
	TEXT("destruction.debug"),
	TEXT("Enable/disable destruction debugger. Usage: destruction.debug [0/1]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.debug: No world available"));
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.debug: Debugger subsystem not found"));
			return;
		}

		if (Args.Num() > 0)
		{
			const bool bEnable = FCString::Atoi(*Args[0]) != 0;
			Debugger->SetEnabled(bEnable);
			UE_LOG(LogTemp, Log, TEXT("destruction.debug: %s"), bEnable ? TEXT("Enabled") : TEXT("Disabled"));
		}
		else
		{
			// 토글
			const bool bNewState = !Debugger->IsEnabled();
			Debugger->SetEnabled(bNewState);
			UE_LOG(LogTemp, Log, TEXT("destruction.debug: %s"), bNewState ? TEXT("Enabled") : TEXT("Disabled"));
		}
	})
);

//-------------------------------------------------------------------
// destruction.vis - 시각화 토글 (또는 0/1로 직접 지정)
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionVisCmd(
	TEXT("destruction.vis"),
	TEXT("Toggle destruction visualization. Usage: destruction.vis [0/1]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.vis: Debugger not found"));
			return;
		}

		bool bEnable;
		if (Args.Num() > 0)
		{
			bEnable = FCString::Atoi(*Args[0]) != 0;
		}
		else
		{
			// 토글
			bEnable = !Debugger->IsVisualizationEnabled();
		}

		Debugger->SetVisualizationEnabled(bEnable);
		UE_LOG(LogTemp, Log, TEXT("destruction.vis: Visualization %s"), bEnable ? TEXT("ENABLED") : TEXT("DISABLED"));
	})
);

//-------------------------------------------------------------------
// destruction.hud - HUD 토글 (또는 0/1로 직접 지정)
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionHUDCmd(
	TEXT("destruction.hud"),
	TEXT("Toggle destruction HUD overlay. Usage: destruction.hud [0/1]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.hud: Debugger not found"));
			return;
		}

		bool bEnable;
		if (Args.Num() > 0)
		{
			bEnable = FCString::Atoi(*Args[0]) != 0;
		}
		else
		{
			// 토글
			bEnable = !Debugger->IsHUDEnabled();
		}

		Debugger->SetHUDEnabled(bEnable);
		UE_LOG(LogTemp, Log, TEXT("destruction.hud: HUD %s"), bEnable ? TEXT("ENABLED") : TEXT("DISABLED"));
	})
);

//-------------------------------------------------------------------
// destruction.stats - 통계 출력
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionStatsCmd(
	TEXT("destruction.stats"),
	TEXT("Print destruction statistics to log"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.stats: Debugger not found"));
			return;
		}

		Debugger->PrintStats();
	})
);

//-------------------------------------------------------------------
// destruction.history [count] - 히스토리 출력
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionHistoryCmd(
	TEXT("destruction.history"),
	TEXT("Print destruction history to log. Usage: destruction.history [count=10]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.history: Debugger not found"));
			return;
		}

		int32 Count = 10; // 기본값
		if (Args.Num() > 0)
		{
			Count = FCString::Atoi(*Args[0]);
			Count = FMath::Max(1, Count); // 최소 1
		}

		Debugger->PrintHistory(Count);
	})
);

//-------------------------------------------------------------------
// destruction.clear - 히스토리 클리어
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionClearCmd(
	TEXT("destruction.clear"),
	TEXT("Clear destruction history"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.clear: Debugger not found"));
			return;
		}

		Debugger->ClearHistory();
	})
);

//-------------------------------------------------------------------
// destruction.reset - 기본 통계 리셋
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionResetCmd(
	TEXT("destruction.reset"),
	TEXT("Reset basic destruction statistics"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.reset: Debugger not found"));
			return;
		}

		Debugger->ResetStats();
		UE_LOG(LogTemp, Log, TEXT("destruction.reset: Basic stats reset"));
	})
);

//-------------------------------------------------------------------
// destruction.resetall - 모든 통계 리셋 (네트워크, 성능 포함)
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionResetAllCmd(
	TEXT("destruction.resetall"),
	TEXT("Reset ALL statistics (basic, network, performance, client)"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.resetall: Debugger not found"));
			return;
		}

		Debugger->ResetAllStats();
		UE_LOG(LogTemp, Log, TEXT("destruction.resetall: All stats reset"));
	})
);

//-------------------------------------------------------------------
// destruction.net - 네트워크 통계 출력
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionNetCmd(
	TEXT("destruction.net"),
	TEXT("Print network statistics (RPC counts, RTT, validation failures)"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.net: Debugger not found"));
			return;
		}

		Debugger->PrintNetworkStats();
	})
);

//-------------------------------------------------------------------
// destruction.clients - 클라이언트별 통계 출력 (서버에서만 유효)
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionClientsCmd(
	TEXT("destruction.clients"),
	TEXT("Print per-client statistics (server only)"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.clients: Debugger not found"));
			return;
		}

		Debugger->PrintClientStats();
	})
);

//-------------------------------------------------------------------
// destruction.perf - 성능 통계 출력
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionPerfCmd(
	TEXT("destruction.perf"),
	TEXT("Print performance statistics (frame drops, max frame time)"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.perf: Debugger not found"));
			return;
		}

		// Debugger->PrintPerformanceStats();
	})
);

//-------------------------------------------------------------------
// destruction.filter - 필터 설정
// 사용법: destruction.filter [actor_name] [min_radius]
// 예시: destruction.filter Wall 10
// 필터 해제: destruction.filter clear
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionFilterCmd(
	TEXT("destruction.filter"),
	TEXT("Set debug filters. Usage: destruction.filter [actor_name] [min_radius] OR destruction.filter clear"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.filter: Debugger not found"));
			return;
		}

		if (Args.Num() == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("destruction.filter: Usage - destruction.filter [actor_name] [min_radius] OR destruction.filter clear"));
			return;
		}

		// "clear" 입력 시 필터 해제
		if (Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase))
		{
			Debugger->ClearFilters();
			UE_LOG(LogTemp, Log, TEXT("destruction.filter: Filters cleared"));
			return;
		}

		// 액터 이름 필터
		Debugger->SetActorFilter(Args[0]);
		UE_LOG(LogTemp, Log, TEXT("destruction.filter: Actor filter set to '%s'"), *Args[0]);

		// 최소 반경 필터 (옵션)
		if (Args.Num() > 1)
		{
			const float MinRadius = FCString::Atof(*Args[1]);
			Debugger->SetMinRadiusFilter(MinRadius);
			UE_LOG(LogTemp, Log, TEXT("destruction.filter: Min radius filter set to %.1f"), MinRadius);
		}
	})
);

//-------------------------------------------------------------------
// destruction.export - CSV 내보내기
// 사용법: destruction.export history [path] 또는 destruction.export stats [path]
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionExportCmd(
	TEXT("destruction.export"),
	TEXT("Export to CSV. Usage: destruction.export [history|stats] [optional_path]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.export: Debugger not found"));
			return;
		}

		if (Args.Num() == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("destruction.export: Usage - destruction.export [history|stats] [optional_path]"));
			return;
		}

		const FString& Type = Args[0];
		FString FilePath;

		// 기본 경로 설정
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

		if (Args.Num() > 1)
		{
			FilePath = Args[1];
		}
		else
		{
			// 기본 경로: Saved/Logs/
			FilePath = FPaths::ProjectSavedDir() / TEXT("Logs");
		}

		bool bSuccess = false;

		if (Type.Equals(TEXT("history"), ESearchCase::IgnoreCase))
		{
			const FString FullPath = FilePath / FString::Printf(TEXT("DestructionHistory_%s.csv"), *Timestamp);
			bSuccess = Debugger->ExportHistoryToCSV(FullPath);
			if (bSuccess)
			{
				UE_LOG(LogTemp, Log, TEXT("destruction.export: History exported to %s"), *FullPath);
			}
		}
		else if (Type.Equals(TEXT("stats"), ESearchCase::IgnoreCase))
		{
			const FString FullPath = FilePath / FString::Printf(TEXT("DestructionStats_%s.csv"), *Timestamp);
			bSuccess = Debugger->ExportStatsToCSV(FullPath);
			if (bSuccess)
			{
				UE_LOG(LogTemp, Log, TEXT("destruction.export: Stats exported to %s"), *FullPath);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.export: Unknown type '%s'. Use 'history' or 'stats'"), *Type);
		}

		if (!bSuccess)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.export: Export failed"));
		}
	})
);

//-------------------------------------------------------------------
// destruction.summary - 세션 요약 출력
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionSummaryCmd(
	TEXT("destruction.summary"),
	TEXT("Print session summary (all stats combined)"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>();
		if (!Debugger)
		{
			UE_LOG(LogTemp, Warning, TEXT("destruction.summary: Debugger not found"));
			return;
		}

		Debugger->PrintSessionSummary();
	})
);

//-------------------------------------------------------------------
// destruction.help - 모든 명령어 목록 출력
//-------------------------------------------------------------------
static FAutoConsoleCommand GDestructionHelpCmd(
	TEXT("destruction.help"),
	TEXT("Print all destruction debug commands"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("========== Destruction Debug Commands =========="));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Control ==="));
		UE_LOG(LogTemp, Log, TEXT("  destruction.all [0/1]      - Toggle ALL features"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.debug [0/1]    - Toggle debugger"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.vis [0/1]      - Toggle visualization"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.hud [0/1]      - Toggle HUD"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Statistics ==="));
		UE_LOG(LogTemp, Log, TEXT("  destruction.stats          - Print basic stats"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.net            - Print network stats"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.clients        - Print per-client stats (server)"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.perf           - Print performance stats"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.summary        - Print full session summary"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== History ==="));
		UE_LOG(LogTemp, Log, TEXT("  destruction.history [n]    - Print last n entries (default 10)"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.clear          - Clear history"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Reset ==="));
		UE_LOG(LogTemp, Log, TEXT("  destruction.reset          - Reset basic stats"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.resetall       - Reset ALL stats"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Filter ==="));
		UE_LOG(LogTemp, Log, TEXT("  destruction.filter [actor] [radius] - Set filters"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.filter clear   - Clear filters"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Export ==="));
		UE_LOG(LogTemp, Log, TEXT("  destruction.export history [path] - Export history to CSV"));
		UE_LOG(LogTemp, Log, TEXT("  destruction.export stats [path]   - Export stats to CSV"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Network Test ==="));
		UE_LOG(LogTemp, Log, TEXT("  Destruction.NetPreset [preset]    - Set network preset (off/good/normal/bad/worst)"));
		UE_LOG(LogTemp, Log, TEXT("  Destruction.NetStatus             - Print current network test status"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("=== Profiling ==="));
		UE_LOG(LogTemp, Log, TEXT("  Destruction.ProfileStats          - Print profiler statistics"));
		UE_LOG(LogTemp, Log, TEXT("  Destruction.ProfileReset          - Reset profiler statistics"));
		UE_LOG(LogTemp, Log, TEXT("  Destruction.ProfileExport [path]  - Export profiler stats to CSV"));
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("================================================="));
	})
);

//=============================================================================
// 네트워크 테스트 명령어
//=============================================================================

//-------------------------------------------------------------------
// Destruction.NetPreset - 네트워크 프리셋 설정
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionNetPresetCmd(
	TEXT("Destruction.NetPreset"),
	TEXT("Set network simulation preset. Usage: Destruction.NetPreset [off|good|normal|bad|worst]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogTemp, Warning, TEXT("Destruction.NetPreset: No world available"));
			return;
		}

		UNetworkTestSubsystem* NetTest = World->GetSubsystem<UNetworkTestSubsystem>();
		if (!NetTest)
		{
			UE_LOG(LogTemp, Warning, TEXT("Destruction.NetPreset: NetworkTestSubsystem not found (only available in non-shipping builds)"));
			return;
		}

		if (Args.Num() == 0)
		{
			// 인자 없으면 현재 상태와 프리셋 목록 출력
			NetTest->PrintCurrentStatus();
			NetTest->PrintAvailablePresets();
			return;
		}

		if (!NetTest->ApplyPresetByName(Args[0]))
		{
			NetTest->PrintAvailablePresets();
		}
	})
);

//-------------------------------------------------------------------
// Destruction.NetStatus - 네트워크 상태 출력
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GDestructionNetStatusCmd(
	TEXT("Destruction.NetStatus"),
	TEXT("Print current network simulation status"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UNetworkTestSubsystem* NetTest = World->GetSubsystem<UNetworkTestSubsystem>();
		if (NetTest)
		{
			NetTest->PrintCurrentStatus();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Destruction.NetStatus: NetworkTestSubsystem not found"));
		}
	})
);

//=============================================================================
// 프로파일링 명령어
//=============================================================================

//-------------------------------------------------------------------
// Destruction.ProfileStats - 프로파일 통계 출력
//-------------------------------------------------------------------
static FAutoConsoleCommand GDestructionProfileStatsCmd(
	TEXT("Destruction.ProfileStats"),
	TEXT("Print destruction profiler statistics"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FDestructionProfilerStats::Get().PrintStats();
	})
);

//-------------------------------------------------------------------
// Destruction.ProfileReset - 프로파일 통계 리셋
//-------------------------------------------------------------------
static FAutoConsoleCommand GDestructionProfileResetCmd(
	TEXT("Destruction.ProfileReset"),
	TEXT("Reset destruction profiler statistics"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FDestructionProfilerStats::Get().ResetStats();
	})
);

//-------------------------------------------------------------------
// Destruction.ProfileExport - 프로파일 CSV 내보내기
//-------------------------------------------------------------------
static FAutoConsoleCommandWithWorldAndArgs GDestructionProfileExportCmd(
	TEXT("Destruction.ProfileExport"),
	TEXT("Export profiler stats to CSV. Usage: Destruction.ProfileExport [optional_path]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* /*World*/)
	{
		FString Path = Args.Num() > 0 ? Args[0] : TEXT("");
		if (FDestructionProfilerStats::Get().ExportToCSV(Path))
		{
			UE_LOG(LogTemp, Log, TEXT("Destruction.ProfileExport: Exported successfully"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Destruction.ProfileExport: Export failed"));
		}
	})
);
