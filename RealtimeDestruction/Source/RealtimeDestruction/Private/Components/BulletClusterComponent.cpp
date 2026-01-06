#include "BulletClusterComponent.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "TimerManager.h"

UBulletClusterComponent ::UBulletClusterComponent()
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

void UBulletClusterComponent::SetOwnerMesh(URealtimeDestructibleMeshComponent* InOwnerMesh)
{
	OwnerMesh = InOwnerMesh;
}

void UBulletClusterComponent::RegisterRequest(const FVector& ImpactPoint, const FVector& ImpactNormal,
	const float Radius)
{
	// 요청 추가
	FPendingClusteringRequest NewRequest;
	NewRequest.ImpactPoint  = ImpactPoint;
	NewRequest.ImpactNormal = ImpactNormal;
	NewRequest.Radius = Radius;
	PendingRequests.Add(NewRequest);
	
	// 타이머 시작
	if (!bTimerActive && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
			ClusterTimerHandle,
			this,
			&UBulletClusterComponent::OnClusterWindowExpried,
			ClusterWindowTime,
			false);

		bTimerActive = true;
	}
	
}

void UBulletClusterComponent::FlushPendingRequests()
{ 
	OnClusterWindowExpried();
}

void UBulletClusterComponent ::OnClusterWindowExpried()
{
	bTimerActive = false;
	
	if (PendingRequests.Num() < ClusterThreshold)
	{
		// 버퍼 클리어
		ClearPendingRequests();
	
		return;
	}
	
	//군집화 수행
	TArray<FBulletCluster> Clusters = ProcessClustering();
	
	// 파괴
	ExecuteDestruction(Clusters);
	
	// 버퍼 클리어
	ClearPendingRequests();
	
}

//TODO: 아직 완성안됐습니다.
TArray<FBulletCluster> UBulletClusterComponent::ProcessClustering()
{
	TArray<FBulletCluster> ResultClusters;
	int32 N = PendingRequests.Num();

	if ( N < ClusterThreshold)
	{
		return ResultClusters;
	}

	// Union - Find
	FUnionFind Cluster;
	Cluster.Init(N);

	// 거리기반 Union
	for (int32 i = 0 ; i < N; ++i)
	{
		for (int32 j = 0; j < N; ++j)
		{
			float Dist = FVector::Dist(
			PendingRequests[i].ImpactPoint,
			PendingRequests[j].ImpactPoint
			);

			if (Dist <= MergeDistanceThreshold)
			{
				Cluster.Union(i,j );
			}
		}
	}
	return ResultClusters;
}

void UBulletClusterComponent::ExecuteDestruction(const TArray<FBulletCluster>& Clusters)
{
	URealtimeDestructibleMeshComponent* Mesh = OwnerMesh.Get();
	
	if (!Mesh)
	{
		return;	
	}

	for (const FBulletCluster& Cluster : Clusters)
	{
		FRealtimeDestructionRequest Request;

		Request.ImpactPoint = Cluster.Center;
		Request.ImpactNormal = Cluster.Normal;
		Request.ToolShape = EDestructionToolShape::Cylinder;
		Request.ShapeParams.Radius = Cluster.Radius;

		Mesh->ExecuteDestructionInternal(Request);
	}
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
