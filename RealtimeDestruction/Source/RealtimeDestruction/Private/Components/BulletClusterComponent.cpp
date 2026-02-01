// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "BulletClusterComponent.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"

UBulletClusterComponent::UBulletClusterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UBulletClusterComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UBulletClusterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearPendingRequests();

	Super::EndPlay(EndPlayReason);
}

void UBulletClusterComponent::Init(float InMergeDistance, float InMaxCluserRadius, int InClusterCountThreshold, float InRaidusOffset)
{
	MergeDistanceThreshold = InMergeDistance;
	MaxClusterRadius = InMaxCluserRadius;
	ClusterCountThreshold = InClusterCountThreshold;
	ClusterRadiusOffset = InRaidusOffset;
}

void UBulletClusterComponent::SetOwnerMesh(URealtimeDestructibleMeshComponent* InOwnerMesh)
{
	OwnerMesh = InOwnerMesh;
}

void UBulletClusterComponent::RegisterRequest(const FRealtimeDestructionRequest& Request)
{
	// 요청 추가
	FPendingClusteringRequest NewRequest;
	NewRequest.ImpactPoint = Request.ImpactPoint;
	NewRequest.ImpactNormal = Request.ImpactNormal;
	NewRequest.Radius = Request.ShapeParams.Radius;
	NewRequest.ChunkIndex = Request.ChunkIndex;
	NewRequest.ToolForwardVector = Request.ToolForwardVector;
	NewRequest.ToolOriginWorld = Request.ImpactPoint - (Request.ToolForwardVector * Request.ShapeParams.SurfaceMargin);
	NewRequest.Depth = (Request.ShapeParams.Height + Request.ShapeParams.SurfaceMargin) * 0.9f;
	PendingRequests.Add(NewRequest);

	// 타이머 시작
	if (!bTimerActive && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
			ClusterTimerHandle,
			this,
			&UBulletClusterComponent::OnClusterWindowExpired,
			ClusterWindowTime,
			false);

		bTimerActive = true;
	}

}


void UBulletClusterComponent::OnClusterWindowExpired()
{
	bTimerActive = false;

	// 임계 Request 이상 쌓이지 않으면 초기화 
	if (PendingRequests.Num() < ClusterCountThreshold)
	{
		// 버퍼 클리어
		ClearPendingRequests();
		return;
	}

	//군집화 수행
	TArray<FBulletCluster> Clusters = ProcessClustering();
	 
	// 파괴
	if (Clusters.Num() > 0)
	{
		ExecuteDestruction(Clusters);
	}

	// 버퍼 클리어
	ClearPendingRequests();

}

TArray<FBulletCluster> UBulletClusterComponent::ProcessClustering()
{
	TArray<FBulletCluster> ResultClusters;
	int32 N = PendingRequests.Num();


	// 임계치 이상 안넘으면 return
	if (N < ClusterCountThreshold)
	{
		return ResultClusters;
	}

	// Union - Find
	FUnionFind ClusterUF;
	ClusterUF.Init(N);

	const float CosThreshold = FMath::Cos(FMath::DegreesToRadians(15.0f));

	for (int32 i = 0; i < N; ++i)
	{
		for (int32 j = i + 1; j < N; ++j)
		{
			float Dist = FVector::Dist(
				PendingRequests[i].ImpactPoint,
				PendingRequests[j].ImpactPoint
			);

			if (Dist <= MergeDistanceThreshold)
			{
				ClusterUF.Union(i, j);
			}
		}
	}
	
	

	// 클러스터 그룹핑
	TMap<int32, FBulletCluster> RootToCluster;
	 
	for (int32 i = 0; i < N; ++i)
	{
		int32 Root = ClusterUF.Find(i);
		FPendingClusteringRequest& Req = PendingRequests[i];

		FBulletCluster* FoundCluster = RootToCluster.Find(Root);

		// 아직 등록이 안되어있다면
		if (!FoundCluster)
		{
			FBulletCluster NewCluster;
			NewCluster.Init(Req.ImpactPoint, Req.ImpactNormal, Req.ToolForwardVector, Req.ToolOriginWorld, Req.Radius, Req.ChunkIndex, Req.Depth);
			RootToCluster.Add(Root, NewCluster);
		}
		else
		{
			// Cluster에 새로운 총알이 추가 되었을 때,
			// 생기는 원의 반지름 크기를 예측해서 넣을지 말지 정한다.
			float PredicatedRauius = FoundCluster->PredictRadius(Req.ImpactPoint, Req.Radius);

			if (PredicatedRauius <= MaxClusterRadius)
			{
				FoundCluster->AddMember(Req.ImpactPoint, Req.ImpactNormal, Req.ToolForwardVector, Req.Radius, Req.ChunkIndex);
			}

		}
	}
	 

	for (auto& Pair : RootToCluster)
	{
		if (Pair.Value.MemberPoints.Num() < ClusterCountThreshold)
		{
			continue;
		}

		ResultClusters.Add(Pair.Value);
	}

	return ResultClusters;
}

void UBulletClusterComponent::ExecuteDestruction(const TArray<FBulletCluster>& Clusters)
{  
	URealtimeDestructibleMeshComponent* Mesh = OwnerMesh.Get();
	if (!Mesh || !IsValid(Mesh)) return;

	// 서버에서만 실행
	if (!Mesh->GetOwner()->HasAuthority())
	{
		return;
	}
	UWorld* World = GetWorld();;
	if (!World)
	{
		return;
	}
	
	const ENetMode NetMode = World->GetNetMode();
	/*
	 * for문 안에 있어서 매 반복마다 메모리 재할당이 발생함
	 * 여기도 3 x 3 x 3 고려해서 27개 할당
	 */
	TArray<int32> AffectedChunks;
	AffectedChunks.Reserve(27);
	for (const FBulletCluster& Cluster : Clusters)
	{
		float FinalRadius = Cluster.Radius * ClusterRadiusOffset;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Clustering_FindChunks);
			Mesh->FindChunksInRadius(Cluster.Center, FinalRadius, AffectedChunks);
		}
		if (AffectedChunks.Num() == 0) continue;

		// 모든 청크가 동일한 Center를 사용하여 높이가 일관되게 유지됨
		for (int32 ChunkIndex : AffectedChunks)
		{
			FRealtimeDestructionRequest Request;
			Request.ImpactPoint = Cluster.Center; // 모든 청크에 동일한 Center 사용
			Request.ImpactNormal = Cluster.Normal;
			Request.ToolShape = EDestructionToolShape::Cylinder;
			Request.ShapeParams.Radius = FinalRadius;
			Request.ChunkIndex = ChunkIndex;
			Request.ToolForwardVector = Cluster.AverageForwardVector;
			Request.ToolOriginWorld = Cluster.ToolOriginWorld;
			Request.ShapeParams.Height = Cluster.Depth;

			 
			Request.ToolMeshPtr = Mesh->CreateToolMeshPtrFromShapeParams(
				Request.ToolShape, Request.ShapeParams);
			  
			Mesh->ExecuteDestructionInternal(Request); 
		 

			// 서버에서 직접 실행 
			if (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer)
			{
				//Multicast로 클라에 전파
				FRealtimeDestructionOp Op;
				Op.Request = Request;
				
				if (Mesh->bUseServerBatching)
				{
					Mesh->EnqueueForServerBatch(Op);
				}
				else
				{
					TArray<FCompactDestructionOp> CompactOps;
					CompactOps.Add(FCompactDestructionOp::Compress(Op.Request, 0));
					Mesh->MulticastApplyOpsCompact(CompactOps);
				}
			}

		}
	}

	// 파편 정리 예약
	Mesh->bPendingCleanup = true;
}

void UBulletClusterComponent::ClearPendingRequests()
{
	// 타이머도 초기화
	if (bTimerActive && GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ClusterTimerHandle);
		bTimerActive = false;
	}

	// 대기열 초기화
	PendingRequests.Empty();
}