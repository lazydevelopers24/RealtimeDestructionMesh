// Copyright 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StructuralIntegrity/GridCellTypes.h"
#include "VectorTypes.h"  // UE::Geometry::FVector3d

class UBodySetup;
struct FKConvexElem;
struct FKBoxElem;
struct FKSphereElem;
struct FKSphylElem;

namespace UE { namespace Geometry { class FDynamicMesh3; } }

/**
 * 격자 셀 빌더 (에디터 유틸리티)
 * StaticMesh 또는 DynamicMesh로부터 격자 셀 레이아웃을 생성
 */
class REALTIMEDESTRUCTION_API FGridCellBuilder
{
public:
	/**
	 * 스태틱 메시에서 격자 셀 레이아웃 생성
	 *
	 * @param SourceMesh - 소스 메시
	 * @param MeshScale - 메시 스케일 (컴포넌트 스케일)
	 * @param CellSize - 셀 크기 (cm, 월드 스페이스)
	 * @param AnchorHeightThreshold - 앵커 판정 높이 임계값 (cm)
	 * @param OutLayout - 출력 레이아웃
	 * @return 성공 여부
	 */
	static bool BuildFromStaticMesh(
		const UStaticMesh* SourceMesh,
		const FVector& MeshScale,
		const FVector& CellSize,
		float AnchorHeightThreshold,
		FGridCellLayout& OutLayout);

	/**
	 * 다이나믹 메시에서 격자 셀 레이아웃 생성
	 *
	 * @param Mesh - 소스 다이나믹 메시
	 * @param CellSize - 셀 크기 (cm)
	 * @param AnchorHeightThreshold - 앵커 판정 높이 임계값 (cm)
	 * @param OutLayout - 출력 레이아웃
	 * @return 성공 여부
	 */
	static bool BuildFromDynamicMesh(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const FVector& CellSize,
		float AnchorHeightThreshold,
		FGridCellLayout& OutLayout);

	static void SetAnchorsByFinitePlane(
		const FTransform& PlaneTransform,
		const FTransform& MeshTransform,
		FGridCellLayout& OutLayout,
		bool bIsEraser);

	static void SetAnchorsByFiniteBox(
		const FTransform& BoxTransform,
		const FVector& BoxExtent,
		const FTransform& MeshTransform,
		FGridCellLayout& OutLayout,
		bool bIsEraser);

	static void SetAnchorsByFiniteSphere(
		const FTransform& SphereTransform,
		float SphereRadius,
		const FTransform& MeshTransform,
		FGridCellLayout& OutLayout,
		bool bIsEraser);

	static void ClearAllAnchors(FGridCellLayout& OutLayout);

private:
	/**
	 * 바운딩 박스로부터 격자 크기 계산
	 */
	static void CalculateGridDimensions(
		const FBox& Bounds,
		const FVector& CellSize,
		FGridCellLayout& OutLayout);

	/**
	 * 삼각형을 셀에 할당
	 */
	static void AssignTrianglesToCells(
		const UE::Geometry::FDynamicMesh3& Mesh,
		FGridCellLayout& OutLayout);

	/**
	 * 인접 관계 계산 (6방향)
	 */
	static void CalculateNeighbors(FGridCellLayout& OutLayout);

	/**
	 * 앵커 셀 판정
	 */
	static void DetermineAnchors(
		FGridCellLayout& OutLayout,
		float HeightThreshold);
	
	/**
	 * 메시 복셀화 (내부 셀 채우기) - DynamicMesh 버전
	 */
	static void VoxelizeMesh(
		const UE::Geometry::FDynamicMesh3& Mesh,
		FGridCellLayout& OutLayout);

	/**
	 * 모든 Collision 타입 기반 복셀화 (Convex, Box, Sphere, Capsule)
	 */
	static void VoxelizeWithCollision(
		const UBodySetup* BodySetup,
		FGridCellLayout& OutLayout);

	/**
	 * Convex Collision 기반 복셀화 (호환성용)
	 */
	static void VoxelizeWithConvex(
		const UBodySetup* BodySetup,
		FGridCellLayout& OutLayout);

	/** StaticMesh의 실제 삼각형 기반 복셀화 */
	static void VoxelizeWithTriangles(
		const UStaticMesh* SourceMesh,
		FGridCellLayout& OutLayout);
	
	static bool TriangleIntersectsAABB(
		const FVector& V0, const FVector& V1, const FVector& V2,
		const FVector& BoxMin, const FVector& BoxMax
	);

	static void FillInsideVoxels(FGridCellLayout& OutLayout);

	/**
	 * 점이 Convex Hull 내부에 있는지 판단
	 */
	static bool IsPointInsideConvex(
		const FKConvexElem& ConvexElem,
		const FVector& Point);

	/**
	 * 점이 Box 내부에 있는지 판단
	 */
	static bool IsPointInsideBox(
		const FKBoxElem& BoxElem,
		const FVector& Point);

	/**
	 * 점이 Sphere 내부에 있는지 판단
	 */
	static bool IsPointInsideSphere(
		const FKSphereElem& SphereElem,
		const FVector& Point);

	/**
	 * 점이 Capsule 내부에 있는지 판단
	 */
	static bool IsPointInsideCapsule(
		const FKSphylElem& CapsuleElem,
		const FVector& Point);

};
