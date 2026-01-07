// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "CoreMinimal.h"
#include "Components/DynamicMeshComponent.h"
#include "StructuralIntegrity/StructuralIntegritySystem.h"

namespace UE::Geometry
{
	class FDynamicMesh3;
	class FMeshConnectedComponents;
	struct FIndex3i;
}

using UE::Geometry::FDynamicMesh3;
using UE::Geometry::FMeshConnectedComponents;
using UE::Geometry::FIndex3i;

// 절단 평면의 유한 사각형 영역(슬라이스 면)을 표현
struct FChunkDivisionPlaneRect
{
	FVector PlaneOrigin = FVector::ZeroVector;     // 평면 위의 기준점 (Plane 방정식의 한 점)
	FVector PlaneNormal = FVector::UpVector;       // 평면의 법선 (정규화 권장)
	FVector RectCenter = FVector::ZeroVector;      // 사각형 중심점 (반드시 평면 위)
	FVector RectAxisU = FVector::ForwardVector;    // 평면 내 U 축 (정규화, PlaneNormal과 직교)
	FVector RectAxisV = FVector::RightVector;      // 평면 내 V 축 (정규화, PlaneNormal과 직교)
	FVector2D HalfExtents = FVector2D::ZeroVector; // 사각형 반 크기 (U/V 방향 길이의 절반)
	int32 ChunkA = INDEX_NONE;                     // 평면 한쪽에 있는 청크 ID
	int32 ChunkB = INDEX_NONE;                     // 평면 반대쪽에 있는 청크 ID
};

// 평면 사각형에 접한 삼각형의 2D 투영 데이터
struct FChunkBoundaryTriangle2D
{
	FVector2D P0 = FVector2D::ZeroVector; // 평면 UV 좌표 0
	FVector2D P1 = FVector2D::ZeroVector; // 평면 UV 좌표 1
	FVector2D P2 = FVector2D::ZeroVector; // 평면 UV 좌표 2
	FBox2D Bounds;                        // 삼각형 2D AABB
};

// 그래프 인접 정보: 현재 노드와 연결된 상대 노드
struct FChunkCellNeighbor
{
	int32 ChunkId = INDEX_NONE;            // 연결된 상대 Chunk ID
	int32 CellId = INDEX_NONE;             // 연결된 상대 Cell ID (상대 Chunk 내부에서 고유)
	int32 DivisionPlaneIndex = INDEX_NONE; // 연결 기준이 되는 분할 평면 인덱스 (없으면 INDEX_NONE)
};

// 그래프 노드: Chunk/Cell 단위 정보
struct FChunkCellNode
{
	int32 ChunkId = INDEX_NONE;               // Chunk ID (하나의 UDynamicMeshComponent에 대응)
	int32 CellId = INDEX_NONE;                // Cell ID (해당 Chunk 내부에서만 고유, FMeshConnectedComponent에 대응)
	TArray<FChunkCellNeighbor> Neighbors;     // 인접 노드 목록
	bool bIsAnchor = false;                   // Anchor 여부
};

// 한 Chunk 내부의 Cell(ConnectedComponent) 캐시
struct FChunkCellCache
{
	int32 ChunkId = INDEX_NONE;                 // Chunk ID
	TArray<int32> CellIds;                      // Cell ID 목록 (배열 인덱스와 1:1)
	TArray<TArray<int32>> CellTriangles;        // Cell별 삼각형 ID 목록
	TArray<FBox> CellBounds;                    // Cell별 AABB
	bool bHasGeometry = false;                  // 해당 Chunk에 유효한 지오메트리가 있는지
	int32 MeshRevision = 0;                     // 메쉬 갱신 버전 (옵션)
};

// Old Cell -> New Cell(s) 매핑 정보
struct FCellMapping
{
	int32 OldCellId = INDEX_NONE;       // 갱신 전 Cell ID
	TArray<int32> NewCellIds;           // 갱신 후 Cell ID들 (분할 시 여러 개)
	bool bDestroyed = false;            // 완전 소멸 여부
};

// Chunk 갱신 결과
struct FChunkUpdateResult
{
	int32 ChunkId = INDEX_NONE;         // 갱신된 Chunk ID
	TArray<FCellMapping> Mappings;      // Old -> New Cell 매핑 목록
	FChunkCellCache OldCache;           // 갱신 전 캐시
	FChunkCellCache NewCache;           // 갱신 후 캐시
};

// <추후 병목 발생시 도입 고려해볼 것들 메모>
// - (ChunkId, CellId) -> NodeIndex 조회 캐시: 인접/갱신 시 선형 탐색 최소화
// - Cell 매칭 결과 캐시: 재계산 후 Old/New CellId 매핑 유지
// - Anchor 노드 목록: BFS 시작점 스캔 비용 절감



/**
 * URealtimeDestructibleMesh가 소유한 메시들의 기하학적 데이터를 그래프로 구조화합니다.
 * 구조화된 그래프는 FStructuralIntegritySystem에 구조적 무결성 분석에 사용됩니다.
 */
class REALTIMEDESTRUCTION_API FRealDestructCellGraph
{
public:
	//=========================================================================
	// 초기화 및 그래프 구축
	//=========================================================================

	/**
	 * 격자 슬라이싱 결과로 분할 평면 리스트 생성.
	 * Bounds는 로컬 좌표계 AABB를 넣을 것
	 */
	void BuildDivisionPlanesFromGrid(
		const FBox& Bounds, // Source static mesh's AABB
		const FIntVector& SliceCount,
		const TArray<int32>& ChunkIdByGridIndex); // Grid cell index 에 대응하는 chunk id

	/**
	 * 각 청크의 Connected Component(Cell)를 계산하고,
	 * 분할 평면을 기준으로 청크 간 Cell 연결을 판정하여 그래프를 구성.
	 * - BuildDivisionPlanesFromGrid()가 먼저 호출되어야 함
	 * - ChunkMeshes 배열 크기는 DivisionPlanes에 참조된 ChunkId 범위를 포함해야 함
	 *
	 * @param ChunkMeshes - 청크별 메시 포인터 배열 (인덱스 = ChunkId)
	 * @param PlaneTolerance - 평면 거리 허용 오차 (단위: cm)
	 * @param RectTolerance - 사각형 영역 확장 허용 오차 (단위: cm)
	 * @param FloorHeightThreshold - 바닥 Anchor 감지 Z 높이 임계값 (단위: cm, Bounds.Min.Z 기준 상대값)
	 */
	void BuildGraph(
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance = 0.1f,
		float RectTolerance = 0.1f,
		float FloorHeightThreshold = 10.0f);

	/** CellGraph의 노드 구조를 IntegritySystem이 사용할 수 있는 1차원 인접 리스트 형태로 변환 */
	FStructuralIntegrityInitData BuildInitDataFromGraph() const;

	/**
	 * 현재 그래프 상태를 스냅샷으로 생성 (신규 SyncGraph API용)
	 * - 노드는 (ChunkId, CellId) 기준 정렬
	 * - Anchor 노드 목록 포함
	 */
	FStructuralIntegrityGraphSnapshot BuildGraphSnapshot() const;

	/** 그래프 초기화 상태 확인 */
	bool IsGraphBuilt() const { return Nodes.Num() > 0; }

	/** 그래프 리셋 */
	void Reset();

	//=========================================================================
	// 런타임 그래프 갱신
	//=========================================================================

	/**
	 * 수정된 청크들의 Cell을 재계산하고 Old-New 매핑 생성
	 *
	 * @param ModifiedChunkIds - 수정된 청크 ID 집합
	 * @param ChunkMeshes - 청크별 메시 포인터 배열 (인덱스 = ChunkId)
	 * @return 청크별 갱신 결과 (Old->New Cell 매핑 포함)
	 */
	TArray<FChunkUpdateResult> UpdateModifiedChunks(
		const TSet<int32>& ModifiedChunkIds,
		const TArray<FDynamicMesh3*>& ChunkMeshes);

	/**
	 * 갱신된 청크들과 관련된 Division Plane의 연결을 재검사
	 *
	 * @param UpdateResults - UpdateModifiedChunks()의 결과
	 * @param ChunkMeshes - 청크별 메시 포인터 배열
	 * @param PlaneTolerance - 평면 거리 허용 오차 (단위: cm)
	 * @param RectTolerance - 사각형 영역 확장 허용 오차 (단위: cm)
	 */
	void RebuildConnectionsForChunks(
		const TArray<FChunkUpdateResult>& UpdateResults,
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance = 0.1f,
		float RectTolerance = 0.1f);

	//=========================================================================
	// 경계 삼각형 및 연결 검사 (정적 유틸리티)
	//=========================================================================
	
	/**
	 * 특정 분할 평면의 사각 영역에 접하는 경계 삼각형이 존재하는지 검사
	 *
     * 주어진 메시의 삼각형들 중에서 분할 평면에 접하는 삼각형들을 찾고,
	 * 해당 삼각형들을 평면의 로컬 좌표계(U/V 축)로 투영하여 반환한다.
	 * (반환된 삼각형들은 반대쪽 청크의 삼각형과 실제로 맞닿는지 판정하는 데에 활용)
	 *
	 * 판정 기준:
 	 * 1. 삼각형의 세 정점 모두 평면으로부터 PlaneTolerance 이내에 위치
	 * 2. 투영된 삼각형이 사각형 영역(HalfExtents)과 RectTolerance 이내로 교차
	 *
	 * 사용 시나리오:
	 * - Boolean 연산 후 두 청크 사이의 연결 상태 확인
	 * - 경계 삼각형이 없으면 해당 방향으로의 연결이 끊어진 것으로 판정
	 *
	 * @param Mesh - 검사할 메시 (청크의 FDynamicMesh3)
	 * @param TriangleIds - 검사 대상 삼각형 ID 목록 (해당 Cell의 삼각형들)
	 * @param Plane - 분할 평면 정보 (평면 방정식 + 사각형 영역)
	 * @param PlaneTolerance - 평면 거리 허용 오차 (단위: cm). 정점이 이 거리 이내면 평면 위로 판정
	 * @param RectTolerance - 사각형 영역 확장 허용 오차 (단위: cm). 영역 경계를 이만큼 확장하여 판정
	 * @param OutTriangles - [출력] 평면에 접한 삼각형들의 2D 투영 데이터
	 * @param OutBounds - [출력] OutTriangles 전체를 감싸는 2D AABB
	 * @return 경계 삼각형이 하나 이상 존재하면 true
	 */
	static bool HasBoundaryTrianglesOnPlane(
		const FDynamicMesh3& Mesh,
		const TArray<int32>& TriangleIds,
		const FChunkDivisionPlaneRect& Plane,
		float PlaneTolerance,
		float RectTolerance,
		TArray<FChunkBoundaryTriangle2D>& OutTriangles,
		FBox2D& OutBounds);

	/**
	 * 두 노드(청크/셀)가 동일 분할 평면 기준으로 여전히 연결되어 있는지 검사
	 *
	 * 양쪽 메시 모두에서 분할 평면에 접하는 경계 삼각형을 찾고,
	 * 해당 삼각형들의 2D 투영이 서로 겹치는지 확인하여 연결 여부를 판정한다.
	 *
	 * 연결 판정 조건:
	 * 1. MeshA에 평면에 접하는 경계 삼각형이 존재
	 * 2. MeshB에 평면에 접하는 경계 삼각형이 존재
	 * 3. 양쪽 경계 삼각형의 2D 투영 영역이 서로 교차
	 *
	 * 사용 시나리오:
	 * - Boolean 연산 후 인접했던 두 청크의 연결 상태 재검사
	 * - false 반환 시 FStructuralIntegritySystem에 연결 끊김을 통보
	 *
	 * @param MeshA - 첫 번째 청크의 메시
	 * @param TriangleIdsA - MeshA에서 검사할 삼각형 ID 목록 (해당 Cell의 삼각형들)
	 * @param MeshB - 두 번째 청크의 메시
	 * @param TriangleIdsB - MeshB에서 검사할 삼각형 ID 목록 (해당 Cell의 삼각형들)
	 * @param Plane - 두 청크를 분리하는 분할 평면 정보
	 * @param PlaneTolerance - 평면 거리 허용 오차 (단위: cm)
	 * @param RectTolerance - 사각형 영역 확장 허용 오차 (단위: cm)
	 * @return 두 노드가 해당 평면에서 연결되어 있으면 true, 끊어졌으면 false
	 */
	static bool AreNodesConnectedByPlane(
		const FDynamicMesh3& MeshA,
		const TArray<int32>& TriangleIdsA,
		const FDynamicMesh3& MeshB,
		const TArray<int32>& TriangleIdsB,
		const FChunkDivisionPlaneRect& Plane,
		float PlaneTolerance,
		float RectTolerance);

	//=========================================================================
	// Getters
	//=========================================================================

	/** 노드 개수 반환 */
	int32 GetNodeCount() const { return Nodes.Num(); }

	/** 청크 개수 반환 */
	int32 GetChunkCount() const { return ChunkCellCaches.Num(); }

	/** 특정 노드 조회 (읽기 전용) */
	const FChunkCellNode* GetNode(int32 NodeIndex) const;

	/** (ChunkId, CellId)로 노드 인덱스 조회 */
	int32 FindNodeIndex(int32 ChunkId, int32 CellId) const;

	/** 특정 청크의 Cell 캐시 조회 */
	const FChunkCellCache* GetChunkCellCache(int32 ChunkId) const;

private:
	//=========================================================================
	// 내부 헬퍼 함수
	//=========================================================================

	/**
	 * 단일 청크의 Connected Component(Cell) 계산
	 * @param Mesh - 분석할 메시
	 * @param ChunkId - 청크 ID
	 * @param OutCache - 결과를 저장할 캐시
	 */
	void BuildChunkCellCache(const FDynamicMesh3& Mesh, int32 ChunkId, FChunkCellCache& OutCache);

	/**
	 * 분할 평면 기준 청크 간 Cell 연결 검사 및 노드 생성
	 * @param ChunkMeshes - 청크 메시 배열
	 * @param PlaneTolerance - 평면 거리 허용 오차
	 * @param RectTolerance - 사각형 영역 확장 허용 오차
	 */
	void BuildNodesAndConnections(
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance,
		float RectTolerance);

	/**
	 * Cell의 바닥 Anchor 여부 판정
	 * @param Cache - Cell이 속한 청크의 캐시
	 * @param CellId - Cell ID
	 * @param Mesh - 청크 메시
	 * @param FloorHeightThreshold - 바닥 높이 임계값
	 * @return 바닥에 접하면 true
	 */
	bool IsCellOnFloor(
		const FChunkCellCache& Cache,
		int32 CellId,
		const FDynamicMesh3& Mesh,
		float FloorHeightThreshold) const;

	/**
	 * AABB 중첩 기반 Old -> New Cell 매핑 생성
	 * @param OldCache - 갱신 전 캐시
	 * @param NewCache - 갱신 후 캐시
	 * @return Cell 매핑 배열
	 */
	TArray<FCellMapping> BuildCellMappings(
		const FChunkCellCache& OldCache,
		const FChunkCellCache& NewCache);

	/**
	 * 특정 Division Plane에 대한 연결 재구축
	 * 해당 평면과 관련된 기존 연결을 제거하고 새로 검사
	 */
	void RebuildConnectionsOnPlane(
		int32 PlaneIndex,
		const TArray<FDynamicMesh3*>& ChunkMeshes,
		float PlaneTolerance,
		float RectTolerance);

	/**
	 * 특정 청크의 모든 노드 제거
	 */
	void RemoveNodesForChunk(int32 ChunkId);

	/**
	 * 청크의 새 Cell들에 대한 노드 추가
	 */
	void AddNodesForChunk(int32 ChunkId, const FChunkCellCache& NewCache);

	//=========================================================================
	// 데이터
	//=========================================================================

	TArray<FChunkCellNode> Nodes;                   // 그래프 노드 (모든 Cell)
	TArray<FChunkDivisionPlaneRect> DivisionPlanes; // 청크 분할 평면
	TArray<FChunkCellCache> ChunkCellCaches;        // 청크별 Cell 캐시
	FBox MeshBounds;                                // 전체 메시 바운딩 박스 (Anchor 판정용)
};
