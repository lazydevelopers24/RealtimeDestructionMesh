// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

// DestructionDebugger.h
// 파괴 시스템 디버깅 및 시각화 도구
//
// 기능:
// - 파괴 위치 시각화 (DrawDebug) + 네트워크 모드별 색상
// - 통계 추적 (초당 파괴 횟수, 처리 시간 등)
// - 네트워크 통계 (RPC 호출, 검증 실패, RTT)
// - 클라이언트별 요청 추적
// - 히스토리 기록 (최근 파괴 요청들)
// - 필터링 (액터/반경)
// - 프레임 드롭 감지
// - CSV 내보내기
// - 콘솔 명령어 지원
// - 화면 HUD 표시

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "DestructionDebugger.generated.h"

class URealtimeDestructibleMeshComponent;
class APlayerController;

/**
 * 파괴 요청 기록 구조체
 */
USTRUCT(BlueprintType)
struct FDestructionHistoryEntry
{
	GENERATED_BODY()

	/** 파괴 발생 시간 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float Timestamp = 0.0f;

	/** 파괴 위치 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FVector ImpactPoint = FVector::ZeroVector;

	/** 충돌 노말 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FVector ImpactNormal = FVector::UpVector;

	/** 파괴 반경 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float Radius = 0.0f;

	/** 요청자 (Instigator) 이름 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FString InstigatorName;

	/** 파괴된 액터 이름 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FString TargetActorName;

	/** 네트워크 모드 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	FString NetMode;

	/** 처리 시간 (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float ProcessingTimeMs = 0.0f;

	/** 서버에서 처리됨 여부 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	bool bFromServer = false;

	/** 클라이언트 ID (서버에서만 유효) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	int32 ClientId = -1;
};

/**
 * 기본 파괴 통계 구조체
 */
USTRUCT(BlueprintType)
struct FDestructionStats
{
	GENERATED_BODY()

	/** 총 파괴 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	int32 TotalDestructions = 0;

	/** 초당 파괴 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float DestructionsPerSecond = 0.0f;

	/** 평균 처리 시간 (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float AverageProcessingTimeMs = 0.0f;

	/** 최대 처리 시간 (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float MaxProcessingTimeMs = 0.0f;

	/** 평균 파괴 반경 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	float AverageRadius = 0.0f;

	/** 마지막 1초간 파괴 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug")
	int32 DestructionsLastSecond = 0;
};

/**
 * 네트워크 통계 구조체
 */
USTRUCT(BlueprintType)
struct FDestructionNetworkStats
{
	GENERATED_BODY()

	/** Server RPC 호출 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ServerRPCCount = 0;

	/** Multicast RPC 호출 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 MulticastRPCCount = 0;

	/** 검증 실패 횟수 (서버에서 거부됨) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ValidationFailures = 0;

	/** 평균 RTT (ms) - 클라이언트에서만 유효 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float AverageRTT = 0.0f;

	/** 최대 RTT (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float MaxRTT = 0.0f;

	/** 최소 RTT (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float MinRTT = 999999.0f;

	/** RTT 샘플 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 RTTSampleCount = 0;

	//--- 데이터 크기 통계 ---

	/** 총 송신 바이트 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int64 TotalBytesSent = 0;

	/** 총 수신 바이트 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int64 TotalBytesReceived = 0;

	/** RPC당 평균 송신 바이트 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float AvgBytesPerRPC = 0.0f;

	/** 압축 RPC 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 CompactRPCCount = 0;

	/** 비압축 RPC 횟수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 UncompressedRPCCount = 0;

	/** 압축으로 절약된 바이트 (추정) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int64 BytesSavedByCompression = 0;
};

/**
 * 클라이언트별 요청 통계 (서버에서만 사용)
 */
USTRUCT(BlueprintType)
struct FClientDestructionStats
{
	GENERATED_BODY()

	/** 클라이언트 ID */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ClientId = -1;

	/** 플레이어 이름 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	FString PlayerName;

	/** 총 요청 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 TotalRequests = 0;

	/** 검증 실패 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	int32 ValidationFailures = 0;

	/** 초당 요청 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float RequestsPerSecond = 0.0f;

	/** 마지막 요청 시간 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Network")
	float LastRequestTime = 0.0f;
};

/**
 * 성능 통계 구조체
 */
USTRUCT(BlueprintType)
struct FDestructionPerformanceStats
{
	GENERATED_BODY()

	/** 프레임 드롭 발생 횟수 (파괴 처리 중) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 FrameDropCount = 0;

	/** 최대 프레임 시간 (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MaxFrameTimeMs = 0.0f;

	/** 한 프레임 최대 파괴 처리 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 MaxDestructionsPerFrame = 0;

	/** 현재 프레임 파괴 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 CurrentFrameDestructions = 0;

	//--- FPS 영향 통계 ---

	/** 파괴 전 평균 FPS */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float AvgFPSBeforeDestruction = 0.0f;

	/** 파괴 중 최소 FPS */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MinFPSDuringDestruction = 999999.0f;

	/** 파괴 시 평균 FPS 드롭 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float AvgFPSDrop = 0.0f;

	/** 최대 FPS 드롭 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MaxFPSDrop = 0.0f;

	/** FPS 측정 샘플 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 FPSSampleCount = 0;

	//--- Boolean 연산 통계 ---

	/** 평균 Boolean 연산 시간 (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float AvgBooleanTimeMs = 0.0f;

	/** 최대 Boolean 연산 시간 (ms) */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	float MaxBooleanTimeMs = 0.0f;

	/** Boolean 연산 샘플 수 */
	UPROPERTY(BlueprintReadOnly, Category = "DestructionDebug|Performance")
	int32 BooleanSampleCount = 0;
};

/**
 * 파괴 시스템 디버거
 *
 * WorldSubsystem으로 구현되어 월드마다 자동 생성됨
 */
UCLASS(ClassGroup = (RealtimeDestruction))
class REALTIMEDESTRUCTION_API UDestructionDebugger : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	//~ End USubsystem Interface

	/** Tick 함수 (FTSTicker용, 통계 업데이트) */
	bool OnTick(float DeltaTime);

	//-------------------------------------------------------------------
	// 디버거 제어
	//-------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void SetEnabled(bool bEnable);

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	bool IsEnabled() const { return bIsEnabled; }

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void SetVisualizationEnabled(bool bEnable) { bShowVisualization = bEnable; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	bool IsVisualizationEnabled() const { return bShowVisualization; }

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void SetHUDEnabled(bool bEnable) { bShowHUD = bEnable; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	bool IsHUDEnabled() const { return bShowHUD; }

	//-------------------------------------------------------------------
	// 파괴 기록
	//-------------------------------------------------------------------

	/** 파괴 요청 기록 (확장) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void RecordDestruction(
		const FVector& ImpactPoint,
		const FVector& ImpactNormal,
		float Radius,
		AActor* Instigator,
		AActor* TargetActor,
		float ProcessingTimeMs = 0.0f);

	/** 파괴 요청 기록 (네트워크 정보 포함) */
	void RecordDestructionEx(
		const FVector& ImpactPoint,
		const FVector& ImpactNormal,
		float Radius,
		AActor* Instigator,
		AActor* TargetActor,
		float ProcessingTimeMs,
		bool bFromServer,
		int32 ClientId);

	//-------------------------------------------------------------------
	// 네트워크 통계 기록 (DestructionNetworkComponent에서 호출)
	//-------------------------------------------------------------------

	/** Server RPC 호출 기록 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordServerRPC();

	/** Multicast RPC 호출 기록 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordMulticastRPC();

	/** 검증 실패 기록 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordValidationFailure(int32 ClientId = -1);

	/** RTT 기록 (클라이언트에서 호출) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordRTT(float RTTMs);

	/** 클라이언트 요청 기록 (서버에서 호출) */
	void RecordClientRequest(int32 ClientId, const FString& PlayerName, bool bValidationFailed = false);

	//-------------------------------------------------------------------
	// 네트워크 데이터 크기 기록
	//-------------------------------------------------------------------

	/** 송신 데이터 크기 기록 (바이트) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordBytesSent(int32 Bytes, bool bIsCompact);

	/** 수신 데이터 크기 기록 (바이트) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordBytesReceived(int32 Bytes);

	/** Multicast RPC 호출 기록 (데이터 크기 포함) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordMulticastRPCWithSize(int32 OpCount, bool bIsCompact);

	/** Server RPC 호출 기록 (데이터 크기 포함) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Network")
	void RecordServerRPCWithSize(bool bIsCompact);

	//-------------------------------------------------------------------
	// 성능 상세 통계 기록
	//-------------------------------------------------------------------

	/** FPS 영향 기록 (파괴 전후 FPS 차이) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Performance")
	void RecordFPSImpact(float FPSBefore, float FPSAfter);

	/** Boolean 연산 시간 기록 (메시 연산 순수 시간) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Performance")
	void RecordBooleanOperationTime(float TimeMs);

	/** 현재 FPS 가져오기 (내부 헬퍼) */
	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Performance")
	float GetCurrentFPS() const;

	/** RTT 측정을 위한 요청 타임스탬프 저장 */
	void StoreRequestTimestamp(uint32 RequestId, double Timestamp);

	/** RTT 측정을 위한 응답 처리 (RequestId로 RTT 계산) */
	void ProcessResponseForRTT(uint32 RequestId);

	//-------------------------------------------------------------------
	// 통계 조회
	//-------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	FDestructionStats GetStats() const { return Stats; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Network")
	FDestructionNetworkStats GetNetworkStats() const { return NetworkStats; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Performance")
	FDestructionPerformanceStats GetPerformanceStats() const { return PerformanceStats; }

	UFUNCTION(BlueprintPure, Category="Destruction|Debug")
	TArray<FDestructionHistoryEntry> GetHistory() const { return History; }

	/** 클라이언트별 통계 가져오기 (서버에서만) */
	UFUNCTION(BlueprintPure, Category="Destruction|Debug|Network")
	TArray<FClientDestructionStats> GetClientStats() const;

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void ClearHistory();

	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void ResetStats();

	/** 모든 통계 리셋 (네트워크, 성능 포함) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void ResetAllStats();

	//-------------------------------------------------------------------
	// 필터링
	//-------------------------------------------------------------------

	/** 액터 이름 필터 설정 (빈 문자열이면 필터 없음) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Filter")
	void SetActorFilter(const FString& ActorNameFilter) { FilterActorName = ActorNameFilter; }

	/** 최소 반경 필터 설정 (0이면 필터 없음) */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Filter")
	void SetMinRadiusFilter(float MinRadius) { FilterMinRadius = MinRadius; }

	/** 필터 클리어 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Filter")
	void ClearFilters() { FilterActorName.Empty(); FilterMinRadius = 0.0f; }

	//-------------------------------------------------------------------
	// 시각화
	//-------------------------------------------------------------------

	/** 특정 위치에 디버그 시각화 그리기 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug")
	void DrawDestructionDebug(const FVector& Location, const FVector& Normal, float Radius, float Duration = 2.0f);

	/** 네트워크 모드별 색상으로 시각화 */
	void DrawDestructionDebugWithNetMode(const FVector& Location, const FVector& Normal, float Radius, bool bFromServer, float Duration = 2.0f);

	//-------------------------------------------------------------------
	// CSV 내보내기
	//-------------------------------------------------------------------

	/** 히스토리를 CSV 파일로 내보내기 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Export")
	bool ExportHistoryToCSV(const FString& FilePath);

	/** 통계를 CSV 파일로 내보내기 */
	UFUNCTION(BlueprintCallable, Category="Destruction|Debug|Export")
	bool ExportStatsToCSV(const FString& FilePath);

	//-------------------------------------------------------------------
	// 콘솔 명령어용 함수
	//-------------------------------------------------------------------

	void PrintStats() const;
	void PrintNetworkStats() const;
	void PrintClientStats() const;
	void PrintPerformanceStats() const;
	void PrintHistory(int32 Count = 10) const;
	void PrintSessionSummary() const;

	//-------------------------------------------------------------------
	// 배칭/시퀀스 상태 (HUD 표시용)
	//-------------------------------------------------------------------

	/** 대기 중인 서버 배치 Op 수 가져오기 */
	UFUNCTION(BlueprintPure, Category = "Destruction|Debug")
	int32 GetPendingBatchOpCount() const { return PendingBatchOpCount; }

	/** 서버 시퀀스 번호 가져오기 */
	UFUNCTION(BlueprintPure, Category = "Destruction|Debug")
	int32 GetServerSequence() const { return ServerSequence; }

	/** 로컬 시퀀스 번호 가져오기 */
	UFUNCTION(BlueprintPure, Category = "Destruction|Debug")
	int32 GetLocalSequence() const { return LocalSequence; }

	/** 대기 중인 배치 Op 수 설정 (RealtimeDestructibleMeshComponent에서 호출) */
	void SetPendingBatchOpCount(int32 Count) { PendingBatchOpCount = Count; }

	/** 서버 시퀀스 번호 설정 */
	void SetServerSequence(int32 Seq) { ServerSequence = Seq; }

	/** 로컬 시퀀스 번호 설정 */
	void SetLocalSequence(int32 Seq) { LocalSequence = Seq; }

protected:
	void UpdateHUD();
	void UpdateDestructionsPerSecond(float DeltaTime);
	void UpdatePerformanceStats(float DeltaTime);
	FString GetNetModeString() const;
	bool PassesFilter(const FString& ActorName, float Radius) const;
	FColor GetColorForNetMode(bool bFromServer) const;

protected:
	//-------------------------------------------------------------------
	// 설정
	//-------------------------------------------------------------------

	UPROPERTY()
	bool bIsEnabled = true;

	UPROPERTY()
	bool bShowVisualization = true;

	UPROPERTY()
	bool bShowHUD = false;

	UPROPERTY()
	int32 MaxHistorySize = 100;

	UPROPERTY()
	float VisualizationDuration = 3.0f;

	/** 서버에서 처리된 파괴 색상 (초록) */
	UPROPERTY()
	FColor ServerColor = FColor::Green;

	/** 클라이언트에서 요청된 파괴 색상 (주황) */
	UPROPERTY()
	FColor ClientColor = FColor::Orange;

	/** 싱글플레이어 파괴 색상 (노랑) */
	UPROPERTY()
	FColor StandaloneColor = FColor::Yellow;

	/** 노말 방향 색상 */
	UPROPERTY()
	FColor NormalColor = FColor::Blue;

	/** 필터: 액터 이름 (contains 매칭) */
	FString FilterActorName;

	/** 필터: 최소 반경 */
	float FilterMinRadius = 0.0f;

	/** 프레임 드롭 임계값 (ms) */
	float FrameDropThresholdMs = 33.33f; // 30 FPS 미만

	//-------------------------------------------------------------------
	// 데이터
	//-------------------------------------------------------------------

	UPROPERTY()
	FDestructionStats Stats;

	UPROPERTY()
	FDestructionNetworkStats NetworkStats;

	UPROPERTY()
	FDestructionPerformanceStats PerformanceStats;

	UPROPERTY()
	TArray<FDestructionHistoryEntry> History;

	/** 클라이언트별 통계 (서버에서만 사용) - Key: ClientId */
	TMap<int32, FClientDestructionStats> ClientStatsMap;

	/** 최근 1초간 파괴 타임스탬프 */
	TArray<float> RecentDestructionTimestamps;

	/** 클라이언트별 최근 요청 타임스탬프 */
	TMap<int32, TArray<float>> ClientRecentRequests;

	/** 총 처리 시간 (평균 계산용) */
	double TotalProcessingTime = 0.0;

	/** 총 반경 (평균 계산용) */
	double TotalRadius = 0.0;

	/** 총 RTT (평균 계산용) */
	double TotalRTT = 0.0;

	/** 총 FPS 드롭 (평균 계산용) */
	double TotalFPSDrop = 0.0;

	/** 총 FPS Before (평균 계산용) */
	double TotalFPSBefore = 0.0;

	/** 총 Boolean 연산 시간 (평균 계산용) */
	double TotalBooleanTime = 0.0;

	/** RTT 측정을 위한 요청 타임스탬프 맵 (RequestId -> Timestamp) */
	TMap<uint32, double> PendingRTTRequests;

	/** 최근 FPS 샘플들 (평균 계산용) */
	TArray<float> RecentFPSSamples;

	/** 세션 시작 시간 */
	float SessionStartTime = 0.0f;

	/** 마지막 프레임 시간 */
	float LastFrameTime = 0.0f;

	/** 현재 프레임 파괴 수 */
	int32 CurrentFrameDestructionCount = 0;

	/** FTSTicker 핸들 */
	FTSTicker::FDelegateHandle TickHandle;

	//-------------------------------------------------------------------
	// 배칭/시퀀스 상태 (HUD 표시용)
	//-------------------------------------------------------------------

	/** 대기 중인 서버 배치 Op 수 */
	int32 PendingBatchOpCount = 0;

	/** 서버 시퀀스 번호 */
	int32 ServerSequence = 0;

	/** 로컬 시퀀스 번호 */
	int32 LocalSequence = 0;
};
