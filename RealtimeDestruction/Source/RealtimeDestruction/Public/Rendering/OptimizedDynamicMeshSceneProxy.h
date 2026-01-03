#pragma once
#include "CoreMinimal.h"
#include "MyDynamicMeshSceneProxy.h"

class FOptimizedDynamicMeshSceneProxy : public FMyDynamicMeshSceneProxy
{
public: 
	FOptimizedDynamicMeshSceneProxy(UDynamicMeshComponent* Component); 
	
	virtual ~FOptimizedDynamicMeshSceneProxy() {}

	// 버퍼 크기를 미리 할당해주는 곳
	void Initialize(); 

	// GPU에 Chunk로 올라가나 버퍼의 부분을 업데이트해주는 함수
	void FastUpdate_IncreamentalVertices(
		const TArray<FVector3f>& NewPositions,
		const TArray<FVector3f>& NewNormals,
		const TArray<uint32>& NewIndices,
		int32 VertexOffset,
		int32 IndexOffset
	);

	void FastUpdate_EntireMesh(const TArray<FVector3f>& AllPositions,
		const TArray<FVector3f>& AllNormals,
		const TArray<uint32>& AllIndices);


	// 실제 렌더링을 담당하는 함수 
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override; 
	  
	  
private:
	// 앞으로 얼마나 더 추가될지 예상하는 여유 분 
	// TODO: Component에서 설정가능하도록 OR 기본 Vertex의 비율로 변경하면 좋을듯
	int32 ExtraVertexCapacity = 10000;
	int32 ExtraIndexCapacity = 30000;

	// Max = Current (기존) + Extra
	int32 MaxVertexCapacity = 0;
	int32 MaxIndexCapacity = 0;

	// 실제로 그려야할 버텍스 / 인덱스 수
	int32 CurrentValidVertexCount = 0;
	int32 CurrentValidIndexCount = 0;

};