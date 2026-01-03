// DestructionDebugger.cpp

#include "Debug/DestructionDebugger.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"

//-------------------------------------------------------------------
// Subsystem Lifecycle
//-------------------------------------------------------------------

void UDestructionDebugger::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 세션 시작 시간 기록
	if (UWorld* World = GetWorld())
	{
		SessionStartTime = World->GetTimeSeconds();
	}

	// Tick 등록
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UDestructionDebugger::OnTick),
		0.0f
	);

	UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: Initialized"));
}

void UDestructionDebugger::Deinitialize()
{
	// 세션 요약 출력
	if (bIsEnabled && Stats.TotalDestructions > 0)
	{
		PrintSessionSummary();
	}

	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	Super::Deinitialize();
	UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: Deinitialized"));
}

bool UDestructionDebugger::ShouldCreateSubsystem(UObject* Outer) const
{
	return true;
}

bool UDestructionDebugger::OnTick(float DeltaTime)
{
	if (!bIsEnabled)
	{
		return true;
	}

	UpdateDestructionsPerSecond(DeltaTime);
	UpdatePerformanceStats(DeltaTime);

	if (bShowHUD)
	{
		UpdateHUD();
	}

	// 프레임 끝에서 현재 프레임 파괴 수 리셋
	CurrentFrameDestructionCount = 0;

	return true;
}

//-------------------------------------------------------------------
// 디버거 제어
//-------------------------------------------------------------------

void UDestructionDebugger::SetEnabled(bool bEnable)
{
	bIsEnabled = bEnable;
	UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: %s"), bEnable ? TEXT("Enabled") : TEXT("Disabled"));
}

//-------------------------------------------------------------------
// 파괴 기록
//-------------------------------------------------------------------

void UDestructionDebugger::RecordDestruction(
	const FVector& ImpactPoint,
	const FVector& ImpactNormal,
	float Radius,
	AActor* Instigator,
	AActor* TargetActor,
	float ProcessingTimeMs)
{
	// 네트워크 모드 확인
	UWorld* World = GetWorld();
	bool bFromServer = false;
	if (World)
	{
		ENetMode NetMode = World->GetNetMode();
		bFromServer = (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer);
	}

	RecordDestructionEx(ImpactPoint, ImpactNormal, Radius, Instigator, TargetActor, ProcessingTimeMs, bFromServer, -1);
}

void UDestructionDebugger::RecordDestructionEx(
	const FVector& ImpactPoint,
	const FVector& ImpactNormal,
	float Radius,
	AActor* Instigator,
	AActor* TargetActor,
	float ProcessingTimeMs,
	bool bFromServer,
	int32 ClientId)
{
	if (!bIsEnabled)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FString TargetName = TargetActor ? TargetActor->GetName() : TEXT("Unknown");

	// 필터 체크
	if (!PassesFilter(TargetName, Radius))
	{
		return;
	}

	// 히스토리 엔트리 생성
	FDestructionHistoryEntry Entry;
	Entry.Timestamp = World->GetTimeSeconds();
	Entry.ImpactPoint = ImpactPoint;
	Entry.ImpactNormal = ImpactNormal;
	Entry.Radius = Radius;
	Entry.InstigatorName = Instigator ? Instigator->GetName() : TEXT("Unknown");
	Entry.TargetActorName = TargetName;
	Entry.NetMode = GetNetModeString();
	Entry.ProcessingTimeMs = ProcessingTimeMs;
	Entry.bFromServer = bFromServer;
	Entry.ClientId = ClientId;

	// 히스토리에 추가
	History.Insert(Entry, 0);
	if (History.Num() > MaxHistorySize)
	{
		History.SetNum(MaxHistorySize);
	}

	// 통계 업데이트
	Stats.TotalDestructions++;
	TotalProcessingTime += ProcessingTimeMs;
	Stats.AverageProcessingTimeMs = TotalProcessingTime / Stats.TotalDestructions;
	Stats.MaxProcessingTimeMs = FMath::Max(Stats.MaxProcessingTimeMs, ProcessingTimeMs);

	TotalRadius += Radius;
	Stats.AverageRadius = static_cast<float>(TotalRadius / Stats.TotalDestructions);

	RecentDestructionTimestamps.Add(Entry.Timestamp);

	// 현재 프레임 파괴 수 증가
	CurrentFrameDestructionCount++;
	PerformanceStats.CurrentFrameDestructions = CurrentFrameDestructionCount;
	PerformanceStats.MaxDestructionsPerFrame = FMath::Max(PerformanceStats.MaxDestructionsPerFrame, CurrentFrameDestructionCount);

	// 시각화
	if (bShowVisualization)
	{
	//	DrawDestructionDebugWithNetMode(ImpactPoint, ImpactNormal, Radius, bFromServer, VisualizationDuration);
	}
}

//-------------------------------------------------------------------
// 네트워크 통계 기록
//-------------------------------------------------------------------

void UDestructionDebugger::RecordServerRPC()
{
	if (!bIsEnabled) return;
	NetworkStats.ServerRPCCount++;
}

void UDestructionDebugger::RecordMulticastRPC()
{
	if (!bIsEnabled) return;
	NetworkStats.MulticastRPCCount++;
}

void UDestructionDebugger::RecordValidationFailure(int32 ClientId)
{
	if (!bIsEnabled) return;
	NetworkStats.ValidationFailures++;

	// 클라이언트별 통계 업데이트
	if (ClientId >= 0)
	{
		if (FClientDestructionStats* ClientStats = ClientStatsMap.Find(ClientId))
		{
			ClientStats->ValidationFailures++;
		}
	}
}

void UDestructionDebugger::RecordRTT(float RTTMs)
{
	if (!bIsEnabled) return;

	TotalRTT += RTTMs;
	NetworkStats.RTTSampleCount++;
	NetworkStats.AverageRTT = TotalRTT / NetworkStats.RTTSampleCount;
	NetworkStats.MaxRTT = FMath::Max(NetworkStats.MaxRTT, RTTMs);
	NetworkStats.MinRTT = FMath::Min(NetworkStats.MinRTT, RTTMs);
}

//-------------------------------------------------------------------
// 네트워크 데이터 크기 기록
//-------------------------------------------------------------------

// 압축/비압축 RPC 예상 크기 (바이트)
static constexpr int32 COMPACT_OP_SIZE = 15;      // FCompactDestructionOp: ~15 bytes
static constexpr int32 UNCOMPRESSED_OP_SIZE = 40; // FRealtimeDestructionRequest: ~40 bytes
static constexpr int32 RPC_OVERHEAD = 8;          // RPC 헤더 오버헤드 추정

void UDestructionDebugger::RecordBytesSent(int32 Bytes, bool bIsCompact)
{
	if (!bIsEnabled) return;

	NetworkStats.TotalBytesSent += Bytes;

	if (bIsCompact)
	{
		NetworkStats.CompactRPCCount++;
	}
	else
	{
		NetworkStats.UncompressedRPCCount++;
	}

	// 평균 계산
	int32 TotalRPCCount = NetworkStats.CompactRPCCount + NetworkStats.UncompressedRPCCount;
	if (TotalRPCCount > 0)
	{
		NetworkStats.AvgBytesPerRPC = static_cast<float>(NetworkStats.TotalBytesSent) / TotalRPCCount;
	}
}

void UDestructionDebugger::RecordBytesReceived(int32 Bytes)
{
	if (!bIsEnabled) return;
	NetworkStats.TotalBytesReceived += Bytes;
}

void UDestructionDebugger::RecordMulticastRPCWithSize(int32 OpCount, bool bIsCompact)
{
	if (!bIsEnabled) return;

	NetworkStats.MulticastRPCCount++;

	int32 DataSize = bIsCompact
		? (OpCount * COMPACT_OP_SIZE + RPC_OVERHEAD)
		: (OpCount * UNCOMPRESSED_OP_SIZE + RPC_OVERHEAD);

	RecordBytesSent(DataSize, bIsCompact);

	// 압축 시 절약된 바이트 계산
	if (bIsCompact)
	{
		int32 UncompressedSize = OpCount * UNCOMPRESSED_OP_SIZE + RPC_OVERHEAD;
		NetworkStats.BytesSavedByCompression += (UncompressedSize - DataSize);
	}
}

void UDestructionDebugger::RecordServerRPCWithSize(bool bIsCompact)
{
	if (!bIsEnabled) return;

	NetworkStats.ServerRPCCount++;

	int32 DataSize = bIsCompact
		? (COMPACT_OP_SIZE + RPC_OVERHEAD)
		: (UNCOMPRESSED_OP_SIZE + RPC_OVERHEAD);

	RecordBytesSent(DataSize, bIsCompact);

	// 압축 시 절약된 바이트 계산
	if (bIsCompact)
	{
		int32 UncompressedSize = UNCOMPRESSED_OP_SIZE + RPC_OVERHEAD;
		NetworkStats.BytesSavedByCompression += (UncompressedSize - DataSize);
	}
}

void UDestructionDebugger::RecordClientRequest(int32 ClientId, const FString& PlayerName, bool bValidationFailed)
{
	if (!bIsEnabled) return;

	UWorld* World = GetWorld();
	float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;

	FClientDestructionStats& ClientStats = ClientStatsMap.FindOrAdd(ClientId);
	ClientStats.ClientId = ClientId;
	ClientStats.PlayerName = PlayerName;
	ClientStats.TotalRequests++;
	ClientStats.LastRequestTime = CurrentTime;

	if (bValidationFailed)
	{
		ClientStats.ValidationFailures++;
	}

	// 초당 요청 수 계산을 위한 타임스탬프 기록
	TArray<float>& Timestamps = ClientRecentRequests.FindOrAdd(ClientId);
	Timestamps.Add(CurrentTime);

	// 1초 이전 타임스탬프 제거
	float OneSecondAgo = CurrentTime - 1.0f;
	Timestamps.RemoveAll([OneSecondAgo](float Timestamp) { return Timestamp < OneSecondAgo; });

	ClientStats.RequestsPerSecond = static_cast<float>(Timestamps.Num());
}

//-------------------------------------------------------------------
// 성능 상세 통계 기록
//-------------------------------------------------------------------

void UDestructionDebugger::RecordFPSImpact(float FPSBefore, float FPSAfter)
{
	if (!bIsEnabled) return;

	float FPSDrop = FPSBefore - FPSAfter;

	// 음수 드롭(FPS가 올라간 경우)은 통계에서 제외
	if (FPSDrop < 0.0f)
	{
		return;
	}

	TotalFPSDrop += FPSDrop;
	TotalFPSBefore += FPSBefore;
	PerformanceStats.FPSSampleCount++;

	PerformanceStats.AvgFPSDrop = static_cast<float>(TotalFPSDrop / PerformanceStats.FPSSampleCount);
	PerformanceStats.MaxFPSDrop = FMath::Max(PerformanceStats.MaxFPSDrop, FPSDrop);
	PerformanceStats.AvgFPSBeforeDestruction = static_cast<float>(TotalFPSBefore / PerformanceStats.FPSSampleCount);

	// 파괴 중 최소 FPS 추적
	if (FPSAfter > 0.0f)
	{
		PerformanceStats.MinFPSDuringDestruction = FMath::Min(PerformanceStats.MinFPSDuringDestruction, FPSAfter);
	}
}

void UDestructionDebugger::RecordBooleanOperationTime(float TimeMs)
{
	if (!bIsEnabled) return;

	TotalBooleanTime += TimeMs;
	PerformanceStats.BooleanSampleCount++;

	PerformanceStats.AvgBooleanTimeMs = static_cast<float>(TotalBooleanTime / PerformanceStats.BooleanSampleCount);
	PerformanceStats.MaxBooleanTimeMs = FMath::Max(PerformanceStats.MaxBooleanTimeMs, TimeMs);
}

float UDestructionDebugger::GetCurrentFPS() const
{
	// 엔진의 DeltaTime을 사용하여 FPS 계산
	if (GEngine && GEngine->GameViewport)
	{
		// 최근 프레임 시간 기반 FPS
		extern ENGINE_API float GAverageFPS;
		return GAverageFPS;
	}
	return 0.0f;
}

void UDestructionDebugger::StoreRequestTimestamp(uint32 RequestId, double /*Timestamp*/)
{
	if (!bIsEnabled) return;
	// 일관된 RTT 측정을 위해 항상 FPlatformTime::Seconds() 사용
	PendingRTTRequests.Add(RequestId, FPlatformTime::Seconds());

	// 오래된 요청 정리 (10초 이상 된 것)
	double CurrentTime = FPlatformTime::Seconds();
	TArray<uint32> ToRemove;
	for (const auto& Pair : PendingRTTRequests)
	{
		if (CurrentTime - Pair.Value > 10.0)
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (uint32 Id : ToRemove)
	{
		PendingRTTRequests.Remove(Id);
	}
}

void UDestructionDebugger::ProcessResponseForRTT(uint32 RequestId)
{
	if (!bIsEnabled) return;

	if (double* StartTime = PendingRTTRequests.Find(RequestId))
	{
		double EndTime = FPlatformTime::Seconds();
		float RTTMs = static_cast<float>((EndTime - *StartTime) * 1000.0);
		RecordRTT(RTTMs);
		PendingRTTRequests.Remove(RequestId);
	}
}

//-------------------------------------------------------------------
// 통계 조회
//-------------------------------------------------------------------

TArray<FClientDestructionStats> UDestructionDebugger::GetClientStats() const
{
	TArray<FClientDestructionStats> Result;
	for (const auto& Pair : ClientStatsMap)
	{
		Result.Add(Pair.Value);
	}
	return Result;
}

void UDestructionDebugger::ClearHistory()
{
	History.Empty();
	RecentDestructionTimestamps.Empty();
	UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: History cleared"));
}

void UDestructionDebugger::ResetStats()
{
	Stats = FDestructionStats();
	TotalProcessingTime = 0.0;
	TotalRadius = 0.0;
	RecentDestructionTimestamps.Empty();
	CurrentFrameDestructionCount = 0;
	UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: Stats reset"));
}

void UDestructionDebugger::ResetAllStats()
{
	ResetStats();
	NetworkStats = FDestructionNetworkStats();
	PerformanceStats = FDestructionPerformanceStats();
	ClientStatsMap.Empty();
	ClientRecentRequests.Empty();
	TotalRTT = 0.0;
	TotalFPSDrop = 0.0;
	TotalFPSBefore = 0.0;
	TotalBooleanTime = 0.0;
	PendingRTTRequests.Empty();
	RecentFPSSamples.Empty();
	UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: All stats reset"));
}

//-------------------------------------------------------------------
// 시각화
//-------------------------------------------------------------------

void UDestructionDebugger::DrawDestructionDebug(
	const FVector& Location,
	const FVector& Normal,
	float Radius,
	float Duration)
{
	DrawDestructionDebugWithNetMode(Location, Normal, Radius, false, Duration);
}

void UDestructionDebugger::DrawDestructionDebugWithNetMode(
	const FVector& Location,
	const FVector& Normal,
	float Radius,
	bool bFromServer,
	float Duration)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FColor MainColor = GetColorForNetMode(bFromServer);

	// 충돌 위치 (작은 구)
	DrawDebugSphere(World, Location, 5.0f, 8, MainColor, false, Duration, 0, 2.0f);

	// 파괴 반경 (큰 구, 와이어프레임)
	DrawDebugSphere(World, Location, Radius, 16, MainColor, false, Duration, 0, 1.0f);

	// 노말 방향 화살표
	const FVector ArrowEnd = Location + Normal * (Radius + 20.0f);
	DrawDebugDirectionalArrow(World, Location, ArrowEnd, 10.0f, NormalColor, false, Duration, 0, 2.0f);

	// 텍스트
	FString InfoText = FString::Printf(TEXT("R: %.1f %s"), Radius, bFromServer ? TEXT("[S]") : TEXT("[C]"));
	DrawDebugString(World, Location + FVector(0, 0, Radius + 15.0f), InfoText, nullptr, FColor::White, Duration, false, 1.0f);
}

FColor UDestructionDebugger::GetColorForNetMode(bool bFromServer) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return StandaloneColor;
	}

	ENetMode NetMode = World->GetNetMode();
	if (NetMode == NM_Standalone)
	{
		return StandaloneColor;
	}

	return bFromServer ? ServerColor : ClientColor;
}

//-------------------------------------------------------------------
// CSV 내보내기
//-------------------------------------------------------------------

bool UDestructionDebugger::ExportHistoryToCSV(const FString& FilePath)
{
	FString CSVContent = TEXT("Timestamp,ImpactX,ImpactY,ImpactZ,NormalX,NormalY,NormalZ,Radius,Instigator,Target,NetMode,ProcessingMs,FromServer,ClientId\n");

	for (const FDestructionHistoryEntry& Entry : History)
	{
		CSVContent += FString::Printf(
			TEXT("%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s,%s,%s,%.3f,%s,%d\n"),
			Entry.Timestamp,
			Entry.ImpactPoint.X, Entry.ImpactPoint.Y, Entry.ImpactPoint.Z,
			Entry.ImpactNormal.X, Entry.ImpactNormal.Y, Entry.ImpactNormal.Z,
			Entry.Radius,
			*Entry.InstigatorName,
			*Entry.TargetActorName,
			*Entry.NetMode,
			Entry.ProcessingTimeMs,
			Entry.bFromServer ? TEXT("true") : TEXT("false"),
			Entry.ClientId
		);
	}

	FString FullPath = FilePath.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("DestructionHistory.csv") : FilePath;

	if (FFileHelper::SaveStringToFile(CSVContent, *FullPath))
	{
		UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: History exported to %s"), *FullPath);
		return true;
	}

	UE_LOG(LogTemp, Warning, TEXT("DestructionDebugger: Failed to export history to %s"), *FullPath);
	return false;
}

bool UDestructionDebugger::ExportStatsToCSV(const FString& FilePath)
{
	FString CSVContent = TEXT("Category,Metric,Value\n");

	// 기본 통계
	CSVContent += FString::Printf(TEXT("Basic,TotalDestructions,%d\n"), Stats.TotalDestructions);
	CSVContent += FString::Printf(TEXT("Basic,DestructionsPerSecond,%.2f\n"), Stats.DestructionsPerSecond);
	CSVContent += FString::Printf(TEXT("Basic,AverageProcessingTimeMs,%.3f\n"), Stats.AverageProcessingTimeMs);
	CSVContent += FString::Printf(TEXT("Basic,MaxProcessingTimeMs,%.3f\n"), Stats.MaxProcessingTimeMs);
	CSVContent += FString::Printf(TEXT("Basic,AverageRadius,%.2f\n"), Stats.AverageRadius);

	// 네트워크 통계
	CSVContent += FString::Printf(TEXT("Network,ServerRPCCount,%d\n"), NetworkStats.ServerRPCCount);
	CSVContent += FString::Printf(TEXT("Network,MulticastRPCCount,%d\n"), NetworkStats.MulticastRPCCount);
	CSVContent += FString::Printf(TEXT("Network,ValidationFailures,%d\n"), NetworkStats.ValidationFailures);
	CSVContent += FString::Printf(TEXT("Network,AverageRTT,%.2f\n"), NetworkStats.AverageRTT);
	CSVContent += FString::Printf(TEXT("Network,MaxRTT,%.2f\n"), NetworkStats.MaxRTT);
	CSVContent += FString::Printf(TEXT("Network,MinRTT,%.2f\n"), NetworkStats.MinRTT);

	// 성능 통계
	CSVContent += FString::Printf(TEXT("Performance,FrameDropCount,%d\n"), PerformanceStats.FrameDropCount);
	CSVContent += FString::Printf(TEXT("Performance,MaxFrameTimeMs,%.2f\n"), PerformanceStats.MaxFrameTimeMs);
	CSVContent += FString::Printf(TEXT("Performance,MaxDestructionsPerFrame,%d\n"), PerformanceStats.MaxDestructionsPerFrame);

	FString FullPath = FilePath.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("DestructionStats.csv") : FilePath;

	if (FFileHelper::SaveStringToFile(CSVContent, *FullPath))
	{
		UE_LOG(LogTemp, Log, TEXT("DestructionDebugger: Stats exported to %s"), *FullPath);
		return true;
	}

	UE_LOG(LogTemp, Warning, TEXT("DestructionDebugger: Failed to export stats to %s"), *FullPath);
	return false;
}

//-------------------------------------------------------------------
// 콘솔 명령어용 함수
//-------------------------------------------------------------------

void UDestructionDebugger::PrintStats() const
{
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("========== Destruction Stats [%s] =========="), *GetNetModeString());
	UE_LOG(LogTemp, Log, TEXT("Total Destructions: %d"), Stats.TotalDestructions);
	UE_LOG(LogTemp, Log, TEXT("Destructions/Second: %.1f"), Stats.DestructionsPerSecond);
	UE_LOG(LogTemp, Log, TEXT("Average Processing Time: %.2f ms"), Stats.AverageProcessingTimeMs);
	UE_LOG(LogTemp, Log, TEXT("Max Processing Time: %.2f ms"), Stats.MaxProcessingTimeMs);
	UE_LOG(LogTemp, Log, TEXT("Average Radius: %.1f"), Stats.AverageRadius);
	UE_LOG(LogTemp, Log, TEXT("============================================="));
}

void UDestructionDebugger::PrintNetworkStats() const
{
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("========== Network Stats [%s] =========="), *GetNetModeString());
	UE_LOG(LogTemp, Log, TEXT("Server RPC Calls: %d"), NetworkStats.ServerRPCCount);
	UE_LOG(LogTemp, Log, TEXT("Multicast RPC Calls: %d"), NetworkStats.MulticastRPCCount);
	UE_LOG(LogTemp, Log, TEXT("Validation Failures: %d"), NetworkStats.ValidationFailures);
	UE_LOG(LogTemp, Log, TEXT("RTT - Avg: %.1f ms | Min: %.1f ms | Max: %.1f ms"),
		NetworkStats.AverageRTT,
		NetworkStats.MinRTT < 999999.0f ? NetworkStats.MinRTT : 0.0f,
		NetworkStats.MaxRTT);
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("--- Data Size ---"));
	UE_LOG(LogTemp, Log, TEXT("Total Sent: %lld B | Received: %lld B"),
		NetworkStats.TotalBytesSent,
		NetworkStats.TotalBytesReceived);
	UE_LOG(LogTemp, Log, TEXT("Avg Bytes/RPC: %.1f B"), NetworkStats.AvgBytesPerRPC);
	UE_LOG(LogTemp, Log, TEXT("Compact RPC: %d | Uncompressed: %d"),
		NetworkStats.CompactRPCCount, NetworkStats.UncompressedRPCCount);
	UE_LOG(LogTemp, Log, TEXT("Bytes Saved by Compression: %lld B"),
		NetworkStats.BytesSavedByCompression);
	UE_LOG(LogTemp, Log, TEXT("=========================================="));
}

void UDestructionDebugger::PrintClientStats() const
{
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("========== Client Stats (Server Only) =========="));

	if (ClientStatsMap.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("No client data recorded."));
	}
	else
	{
		for (const auto& Pair : ClientStatsMap)
		{
			const FClientDestructionStats& CS = Pair.Value;
			UE_LOG(LogTemp, Log, TEXT("[%d] %s - Requests: %d | Failures: %d | Req/Sec: %.1f"),
				CS.ClientId, *CS.PlayerName, CS.TotalRequests, CS.ValidationFailures, CS.RequestsPerSecond);
		}
	}
	UE_LOG(LogTemp, Log, TEXT("================================================"));
}

void UDestructionDebugger::PrintPerformanceStats() const
{
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("========== Performance Stats =========="));
	UE_LOG(LogTemp, Log, TEXT("Frame Drops: %d"), PerformanceStats.FrameDropCount);
	UE_LOG(LogTemp, Log, TEXT("Max Frame Time: %.2f ms"), PerformanceStats.MaxFrameTimeMs);
	UE_LOG(LogTemp, Log, TEXT("Max Destructions/Frame: %d"), PerformanceStats.MaxDestructionsPerFrame);
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("--- FPS Impact ---"));
	UE_LOG(LogTemp, Log, TEXT("Avg FPS Before: %.1f"), PerformanceStats.AvgFPSBeforeDestruction);
	UE_LOG(LogTemp, Log, TEXT("Min FPS During: %.1f"), PerformanceStats.MinFPSDuringDestruction < 999999.0f ? PerformanceStats.MinFPSDuringDestruction : 0.0f);
	UE_LOG(LogTemp, Log, TEXT("Avg FPS Drop: %.1f | Max FPS Drop: %.1f"), PerformanceStats.AvgFPSDrop, PerformanceStats.MaxFPSDrop);
	UE_LOG(LogTemp, Log, TEXT("FPS Samples: %d"), PerformanceStats.FPSSampleCount);
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("--- Boolean Operation ---"));
	UE_LOG(LogTemp, Log, TEXT("Avg Time: %.2f ms | Max Time: %.2f ms"), PerformanceStats.AvgBooleanTimeMs, PerformanceStats.MaxBooleanTimeMs);
	UE_LOG(LogTemp, Log, TEXT("Boolean Samples: %d"), PerformanceStats.BooleanSampleCount);
	UE_LOG(LogTemp, Log, TEXT("========================================"));
}

void UDestructionDebugger::PrintHistory(int32 Count) const
{
	const int32 PrintCount = FMath::Min(Count, History.Num());

	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("========== Destruction History (Last %d) =========="), PrintCount);

	for (int32 i = 0; i < PrintCount; ++i)
	{
		const FDestructionHistoryEntry& Entry = History[i];
		UE_LOG(LogTemp, Log, TEXT("[%.2f] %s -> %s | R: %.1f | %s | %.2f ms"),
			Entry.Timestamp,
			*Entry.InstigatorName,
			*Entry.TargetActorName,
			Entry.Radius,
			Entry.bFromServer ? TEXT("Server") : TEXT("Client"),
			Entry.ProcessingTimeMs);
	}

	UE_LOG(LogTemp, Log, TEXT("==================================================="));
}

void UDestructionDebugger::PrintSessionSummary() const
{
	UWorld* World = GetWorld();
	float SessionDuration = World ? (World->GetTimeSeconds() - SessionStartTime) : 0.0f;

	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("############################################"));
	UE_LOG(LogTemp, Log, TEXT("#         SESSION SUMMARY [%s]"), *GetNetModeString());
	UE_LOG(LogTemp, Log, TEXT("############################################"));
	UE_LOG(LogTemp, Log, TEXT("Session Duration: %.1f seconds"), SessionDuration);
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("--- Basic Stats ---"));
	UE_LOG(LogTemp, Log, TEXT("Total Destructions: %d"), Stats.TotalDestructions);
	UE_LOG(LogTemp, Log, TEXT("Avg Destructions/Sec: %.2f"), SessionDuration > 0 ? Stats.TotalDestructions / SessionDuration : 0.0f);
	UE_LOG(LogTemp, Log, TEXT("Avg Processing Time: %.2f ms"), Stats.AverageProcessingTimeMs);
	UE_LOG(LogTemp, Log, TEXT("Max Processing Time: %.2f ms"), Stats.MaxProcessingTimeMs);
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("--- Network Stats ---"));
	UE_LOG(LogTemp, Log, TEXT("Server RPCs: %d | Multicast RPCs: %d"), NetworkStats.ServerRPCCount, NetworkStats.MulticastRPCCount);
	UE_LOG(LogTemp, Log, TEXT("Validation Failures: %d"), NetworkStats.ValidationFailures);
	UE_LOG(LogTemp, Log, TEXT("Avg RTT: %.1f ms | Min: %.1f ms | Max: %.1f ms"),
		NetworkStats.AverageRTT,
		NetworkStats.MinRTT < 999999.0f ? NetworkStats.MinRTT : 0.0f,
		NetworkStats.MaxRTT);
	UE_LOG(LogTemp, Log, TEXT("Data - Sent: %lld B | Recv: %lld B | Avg: %.0f B/RPC | Saved: %lld B"),
		NetworkStats.TotalBytesSent,
		NetworkStats.TotalBytesReceived,
		NetworkStats.AvgBytesPerRPC,
		NetworkStats.BytesSavedByCompression);
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("--- Performance ---"));
	UE_LOG(LogTemp, Log, TEXT("Frame Drops: %d"), PerformanceStats.FrameDropCount);
	UE_LOG(LogTemp, Log, TEXT("Max Destructions/Frame: %d"), PerformanceStats.MaxDestructionsPerFrame);
	UE_LOG(LogTemp, Log, TEXT("FPS - Before: %.0f | Min: %.0f | Drop(Avg/Max): %.1f/%.1f"),
		PerformanceStats.AvgFPSBeforeDestruction,
		PerformanceStats.MinFPSDuringDestruction < 999999.0f ? PerformanceStats.MinFPSDuringDestruction : 0.0f,
		PerformanceStats.AvgFPSDrop, PerformanceStats.MaxFPSDrop);
	UE_LOG(LogTemp, Log, TEXT("Boolean Op - Avg: %.2f ms | Max: %.2f ms"), PerformanceStats.AvgBooleanTimeMs, PerformanceStats.MaxBooleanTimeMs);
	UE_LOG(LogTemp, Log, TEXT("############################################"));
}

//-------------------------------------------------------------------
// 내부 함수
//-------------------------------------------------------------------

void UDestructionDebugger::UpdateHUD()
{
	if (!GEngine)
	{
		return;
	}

	// 고유 키를 사용해서 화면에 고정 위치에 표시
	const int32 BaseKey = 9900;  // DestructionDebugger용 키 시작점
	int32 KeyOffset = 0;
	const float DisplayTime = 0.0f;  // 매 프레임 업데이트

	// 헤더
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Cyan,
		FString::Printf(TEXT("======== Destruction Debugger [%s] ========"), *GetNetModeString()));

	// 기본 통계
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::White,
		TEXT("--- Basic Stats ---"));
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Green,
		FString::Printf(TEXT("  Total: %d | Per Sec: %.1f | Last Sec: %d"),
			Stats.TotalDestructions, Stats.DestructionsPerSecond, Stats.DestructionsLastSecond));
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Green,
		FString::Printf(TEXT("  Process Time - Avg: %.2f ms | Max: %.2f ms"),
			Stats.AverageProcessingTimeMs, Stats.MaxProcessingTimeMs));
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Green,
		FString::Printf(TEXT("  Avg Radius: %.1f"), Stats.AverageRadius));

	// 네트워크 통계
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::White,
		TEXT("--- Network Stats ---"));
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Yellow,
		FString::Printf(TEXT("  Server RPC: %d | Multicast: %d"),
			NetworkStats.ServerRPCCount, NetworkStats.MulticastRPCCount));

	// 검증 실패는 빨간색으로 강조
	FColor ValidationColor = NetworkStats.ValidationFailures > 0 ? FColor::Red : FColor::Yellow;
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, ValidationColor,
		FString::Printf(TEXT("  Validation Failures: %d"), NetworkStats.ValidationFailures));

	// RTT (클라이언트에서만 의미 있음)
	if (NetworkStats.RTTSampleCount > 0)
	{
		GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Yellow,
			FString::Printf(TEXT("  RTT - Avg: %.1f ms | Min: %.1f | Max: %.1f"),
				NetworkStats.AverageRTT,
				NetworkStats.MinRTT < 999999.0f ? NetworkStats.MinRTT : 0.0f,
				NetworkStats.MaxRTT));
	}

	// 데이터 크기 통계
	if (NetworkStats.TotalBytesSent > 0 || NetworkStats.TotalBytesReceived > 0)
	{
		GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Yellow,
			FString::Printf(TEXT("  Sent: %lld B | Recv: %lld B | Avg: %.0f B/RPC"),
				NetworkStats.TotalBytesSent,
				NetworkStats.TotalBytesReceived,
				NetworkStats.AvgBytesPerRPC));
		GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Yellow,
			FString::Printf(TEXT("  Compact: %d | Uncompressed: %d | Saved: %lld B"),
				NetworkStats.CompactRPCCount,
				NetworkStats.UncompressedRPCCount,
				NetworkStats.BytesSavedByCompression));
	}

	// 성능 통계
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::White,
		TEXT("--- Performance ---"));

	// 프레임 드롭은 빨간색으로 강조
	FColor DropColor = PerformanceStats.FrameDropCount > 0 ? FColor::Red : FColor::Magenta;
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, DropColor,
		FString::Printf(TEXT("  Frame Drops: %d | Max Frame: %.1f ms"),
			PerformanceStats.FrameDropCount, PerformanceStats.MaxFrameTimeMs));
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Magenta,
		FString::Printf(TEXT("  Max Destructions/Frame: %d | Current: %d"),
			PerformanceStats.MaxDestructionsPerFrame, PerformanceStats.CurrentFrameDestructions));

	// FPS 영향 통계
	if (PerformanceStats.FPSSampleCount > 0)
	{
		float MinFPS = PerformanceStats.MinFPSDuringDestruction < 999999.0f ? PerformanceStats.MinFPSDuringDestruction : 0.0f;
		FColor FPSColor = MinFPS < 30.0f ? FColor::Red : (MinFPS < 60.0f ? FColor::Orange : FColor::Magenta);
		GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FPSColor,
			FString::Printf(TEXT("  FPS - Before: %.0f | Min: %.0f | Drop(Avg/Max): %.1f/%.1f"),
				PerformanceStats.AvgFPSBeforeDestruction, MinFPS, PerformanceStats.AvgFPSDrop, PerformanceStats.MaxFPSDrop));
	}

	// Boolean 연산 시간
	if (PerformanceStats.BooleanSampleCount > 0)
	{
		FColor BoolColor = PerformanceStats.MaxBooleanTimeMs > 10.0f ? FColor::Orange : FColor::Magenta;
		GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, BoolColor,
			FString::Printf(TEXT("  Boolean Op - Avg: %.2f ms | Max: %.2f ms"),
				PerformanceStats.AvgBooleanTimeMs, PerformanceStats.MaxBooleanTimeMs));
	}

	// 클라이언트 통계 (서버에서만, 클라이언트가 있을 때만)
	UWorld* World = GetWorld();
	if (World && (World->GetNetMode() == NM_DedicatedServer || World->GetNetMode() == NM_ListenServer))
	{
		if (ClientStatsMap.Num() > 0)
		{
			GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::White,
				TEXT("--- Clients ---"));

			for (const auto& Pair : ClientStatsMap)
			{
				const FClientDestructionStats& CS = Pair.Value;
				FColor ClientStatColor = CS.ValidationFailures > 0 ? FColor::Orange : FColor::Cyan;
				GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, ClientStatColor,
					FString::Printf(TEXT("  [%d] %s: %d req (%.1f/s) | Fail: %d"),
						CS.ClientId, *CS.PlayerName, CS.TotalRequests, CS.RequestsPerSecond, CS.ValidationFailures));
			}
		}
	}

	// 최근 히스토리 (마지막 3개)
	if (History.Num() > 0)
	{
		GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::White,
			TEXT("--- Recent ---"));

		const int32 ShowCount = FMath::Min(3, History.Num());
		for (int32 i = 0; i < ShowCount; ++i)
		{
			const FDestructionHistoryEntry& Entry = History[i];
			FColor HistoryColor = Entry.bFromServer ? FColor::Green : FColor::Orange;
			GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, HistoryColor,
				FString::Printf(TEXT("  [%.1fs] %s -> %s | R:%.0f"),
					Entry.Timestamp, *Entry.InstigatorName.Left(15), *Entry.TargetActorName.Left(15), Entry.Radius));
		}
	}

	// 푸터
	GEngine->AddOnScreenDebugMessage(BaseKey + KeyOffset++, DisplayTime, FColor::Cyan,
		TEXT("================================================"));
}

void UDestructionDebugger::UpdateDestructionsPerSecond(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float CurrentTime = World->GetTimeSeconds();
	const float OneSecondAgo = CurrentTime - 1.0f;

	RecentDestructionTimestamps.RemoveAll([OneSecondAgo](float Timestamp)
	{
		return Timestamp < OneSecondAgo;
	});

	Stats.DestructionsLastSecond = RecentDestructionTimestamps.Num();
	Stats.DestructionsPerSecond = static_cast<float>(Stats.DestructionsLastSecond);
}

void UDestructionDebugger::UpdatePerformanceStats(float DeltaTime)
{
	float FrameTimeMs = DeltaTime * 1000.0f;
	PerformanceStats.MaxFrameTimeMs = FMath::Max(PerformanceStats.MaxFrameTimeMs, FrameTimeMs);

	// 프레임 드롭 감지 (30 FPS 미만)
	if (FrameTimeMs > FrameDropThresholdMs && CurrentFrameDestructionCount > 0)
	{
		PerformanceStats.FrameDropCount++;
	}

	LastFrameTime = FrameTimeMs;
}

FString UDestructionDebugger::GetNetModeString() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return TEXT("Unknown");
	}

	switch (World->GetNetMode())
	{
	case NM_Standalone:
		return TEXT("Standalone");
	case NM_DedicatedServer:
		return TEXT("DedicatedServer");
	case NM_ListenServer:
		return TEXT("ListenServer");
	case NM_Client:
		return TEXT("Client");
	default:
		return TEXT("Unknown");
	}
}

bool UDestructionDebugger::PassesFilter(const FString& ActorName, float Radius) const
{
	// 액터 이름 필터
	if (!FilterActorName.IsEmpty() && !ActorName.Contains(FilterActorName))
	{
		return false;
	}

	// 최소 반경 필터
	if (FilterMinRadius > 0.0f && Radius < FilterMinRadius)
	{
		return false;
	}

	return true;
}
