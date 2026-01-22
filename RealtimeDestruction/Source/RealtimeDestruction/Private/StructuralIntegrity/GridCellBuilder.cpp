// Copyright 2025. All Rights Reserved.

#include "StructuralIntegrity/GridCellBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"

using namespace UE::Geometry;

//=============================================================================
// Public Methods
//=============================================================================

bool FGridCellBuilder::BuildFromStaticMesh(
	const UStaticMesh* SourceMesh,
	const FVector& MeshScale,
	const FVector& CellSize,
	float AnchorHeightThreshold,
	FGridCellCache& OutCache)
{
	if (!SourceMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: SourceMesh is null"));
		return false;
	}

	OutCache.Reset();
	OutCache.CellSize = CellSize;

	// 1. 바운딩 박스 계산 (로컬 스페이스 유지)
	const FBox LocalBounds = SourceMesh->GetBoundingBox();
	if (!LocalBounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid mesh bounds"));
		return false;
	}

	// 스케일 저장 (Collision 체크 시 사용)
	OutCache.MeshScale = MeshScale;

	// 스케일 적용된 크기로 격자 차원 계산 (셀 수 결정용)
	const FVector ScaledSize = LocalBounds.GetSize() * MeshScale;
	const FIntVector GridDimensions(
		FMath::Max(1, FMath::CeilToInt(ScaledSize.X / CellSize.X)),
		FMath::Max(1, FMath::CeilToInt(ScaledSize.Y / CellSize.Y)),
		FMath::Max(1, FMath::CeilToInt(ScaledSize.Z / CellSize.Z))
	);

	// 로컬 스페이스 셀 크기 (스케일 역적용)
	const FVector LocalCellSize(
		CellSize.X / MeshScale.X,
		CellSize.Y / MeshScale.Y,
		CellSize.Z / MeshScale.Z
	);

	// 2. 격자 설정 (로컬 스페이스)
	OutCache.GridOrigin = LocalBounds.Min;
	OutCache.GridSize = GridDimensions;
	OutCache.CellSize = LocalCellSize;  // 로컬 스페이스 셀 크기

	const int32 TotalCells = OutCache.GetTotalCellCount();

	UE_LOG(LogTemp, Log, TEXT("BuildFromStaticMesh: Scale=(%.2f, %.2f, %.2f), ScaledSize=(%.1f, %.1f, %.1f), WorldCellSize=%.1f, LocalCellSize=(%.2f, %.2f, %.2f), Grid=(%d,%d,%d), Total=%d"),
		MeshScale.X, MeshScale.Y, MeshScale.Z,
		ScaledSize.X, ScaledSize.Y, ScaledSize.Z,
		CellSize.X,
		LocalCellSize.X, LocalCellSize.Y, LocalCellSize.Z,
		OutCache.GridSize.X, OutCache.GridSize.Y, OutCache.GridSize.Z,
		TotalCells);

	if (TotalCells <= 0 || TotalCells > 1000000)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid cell count: %d"), TotalCells);
		return false;
	}

	// 3. 비트필드 초기화 (0으로 초기화됨)
	OutCache.InitializeBitfields();

	 // 4. Collision 기반 복셀화 (우선순위: Convex > Box > Sphere > Capsule > BoundingBox)
	 //UBodySetup* BodySetup = SourceMesh->GetBodySetup();
	 //if (BodySetup)
	 //{
	 //	VoxelizeWithCollision(BodySetup, OutCache);
	 //}
	 //else
	 //{
	 //	// BodySetup 없으면 바운딩 박스로 채우기
	 //	UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: No BodySetup, filling bounding box"));
	 //	for (int32 i = 0; i < TotalCells; i++)
	 //	{
	 //		OutCache.SetCellExists(i, true);
	 //		OutCache.RegisterValidCell(i);
	 //	}
	 //}
	
	 VoxelizeWithTriangles(SourceMesh, OutCache);

	 FillInsideVoxels(OutCache);

	// 6. 인접 관계 계산
	CalculateNeighbors(OutCache);

	// 7. 앵커 판정
	DetermineAnchors(OutCache, AnchorHeightThreshold);

	UE_LOG(LogTemp, Log, TEXT("FGridCellBuilder: Built grid %dx%dx%d, valid cells: %d"),
		OutCache.GridSize.X, OutCache.GridSize.Y, OutCache.GridSize.Z,
		OutCache.GetValidCellCount());

	return true;
}

bool FGridCellBuilder::BuildFromDynamicMesh(
	const FDynamicMesh3& Mesh,
	const FVector& CellSize,
	float AnchorHeightThreshold,
	FGridCellCache& OutCache)
{
	OutCache.Reset();
	OutCache.CellSize = CellSize;

	// 1. 바운딩 박스 계산
	FAxisAlignedBox3d Bounds = Mesh.GetBounds();
	if (Bounds.IsEmpty() || Bounds.Volume() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid mesh bounds"));
		return false;
	}

	FBox UnrealBounds(
		FVector(Bounds.Min.X, Bounds.Min.Y, Bounds.Min.Z),
		FVector(Bounds.Max.X, Bounds.Max.Y, Bounds.Max.Z)
	);

	// 2. 격자 크기 계산
	CalculateGridDimensions(UnrealBounds, CellSize, OutCache);

	const int32 TotalCells = OutCache.GetTotalCellCount();
	if (TotalCells <= 0 || TotalCells > 1000000)  // 100만 셀 제한
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid cell count: %d"), TotalCells);
		return false;
	}

	// 3. 비트필드 초기화 (0으로 초기화됨)
	OutCache.InitializeBitfields();

	// 4. 삼각형 할당
	AssignTrianglesToCells(Mesh, OutCache);

	// 6. 인접 관계 계산
	CalculateNeighbors(OutCache);

	// 7. 앵커 판정
	DetermineAnchors(OutCache, AnchorHeightThreshold);

	UE_LOG(LogTemp, Log, TEXT("FGridCellBuilder: Built grid %dx%dx%d, valid cells: %d"),
		OutCache.GridSize.X, OutCache.GridSize.Y, OutCache.GridSize.Z,
		OutCache.GetValidCellCount());

	return true;
}

void FGridCellBuilder::SetAnchorsByFinitePlane(
	const FTransform& PlaneTransform,
	const FTransform& MeshTransform,
	FGridCellCache& OutCache,
	bool bIsEraser)
{
	const int32 TotalCells = OutCache.GetTotalCellCount();
	int32 AddedAnchors = 0;

	const float CubeExtent = 50.0f;

	for (int32 CellId = 0; CellId < TotalCells; ++CellId)
	{
		if (!OutCache.GetCellExists(CellId))
		{
			continue;
		}

		FVector LocalPos = OutCache.IdToLocalCenter(CellId);
		FVector WorldPos = MeshTransform.TransformPosition(LocalPos);

		FVector CubeSpacePos = PlaneTransform.InverseTransformPosition(WorldPos);

		bool bInsideY = FMath::Abs(CubeSpacePos.Y) <= CubeExtent;
		bool bInsideZ = FMath::Abs(CubeSpacePos.Z) <= CubeExtent;

		if (bInsideY && bInsideZ)
		{
			if (CubeSpacePos.X > 0.0f) 
			{
				if (bIsEraser)
				{
					if (OutCache.GetCellExists(CellId))
					{
						OutCache.SetCellIsAnchor(CellId, false);
					}
				}
				else
				{
					if (!OutCache.GetCellIsAnchor(CellId))
					{
						OutCache.SetCellIsAnchor(CellId, true);
						AddedAnchors++;
					}
				}
				
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("SetAnchorsByFinitePlane: %d cells marked as Anchor."), AddedAnchors);
}

void FGridCellBuilder::SetAnchorsByFiniteBox(
	const FTransform& BoxTransform,
	const FVector& BoxExtent,
	const FTransform& MeshTransform,
	FGridCellCache& OutCache,
	bool bIsEraser)
{
	const int32 TotalCells = OutCache.GetTotalCellCount();
	int32 AddedAnchors = 0;
	int32 RemovedAnchors = 0;

	for (int32 CellId = 0; CellId < TotalCells; ++CellId)
	{
		if (!OutCache.GetCellExists(CellId))
		{
			continue;
		}

		const FVector LocalPos = OutCache.IdToLocalCenter(CellId);
		const FVector WorldPos = MeshTransform.TransformPosition(LocalPos);

		// World -> Box Local (회전/스케일 포함)
		const FVector BoxSpacePos = BoxTransform.InverseTransformPosition(WorldPos);

		const bool bInside =
				FMath::Abs(BoxSpacePos.X) <= BoxExtent.X &&
				FMath::Abs(BoxSpacePos.Y) <= BoxExtent.Y &&
				FMath::Abs(BoxSpacePos.Z) <= BoxExtent.Z;

		if (!bInside)
		{
			continue;
		}

		if (bIsEraser)
		{
			if (OutCache.GetCellIsAnchor(CellId))
			{
				OutCache.SetCellIsAnchor(CellId, false);
				RemovedAnchors++;
			}
		}
		else
		{
			if (!OutCache.GetCellIsAnchor(CellId))
			{
				OutCache.SetCellIsAnchor(CellId, true);
				AddedAnchors++;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("SetAnchorsByFiniteBox: Added=%d, Removed=%d"), AddedAnchors, RemovedAnchors);
}

void FGridCellBuilder::SetAnchorsByFiniteSphere(
	const FTransform& SphereTransform,
	float SphereRadius,
	const FTransform& MeshTransform,
	FGridCellCache& OutCache,
	bool bIsEraser)
{
	const int32 TotalCells = OutCache.GetTotalCellCount();
	int32 AddedAnchors = 0;
	int32 RemovedAnchors = 0;

	const float Radius = FMath::Max(0.0f, SphereRadius);
	const float RadiusSq = Radius * Radius;

	for (int32 CellId = 0; CellId < TotalCells; ++CellId)
	{
		if (!OutCache.GetCellExists(CellId))
		{
			continue;
		}

		const FVector LocalPos = OutCache.IdToLocalCenter(CellId);
		const FVector WorldPos = MeshTransform.TransformPosition(LocalPos);

		// World -> Sphere Local (스케일까지 포함해 역변환)
		// 이 방식은 SphereTransform에 비균일 스케일이 들어오면 월드에서는 Ellipsoid 판정이 됩니다.
		const FVector SphereSpacePos = SphereTransform.InverseTransformPosition(WorldPos);

		const bool bInside = SphereSpacePos.SizeSquared() <= RadiusSq;
		if (!bInside)
		{
			continue;
		}

		if (bIsEraser)
		{
			if (OutCache.GetCellIsAnchor(CellId))
			{
				OutCache.SetCellIsAnchor(CellId, false);
				RemovedAnchors++;
			}
		}
		else
		{
			if (!OutCache.GetCellIsAnchor(CellId))
			{
				OutCache.SetCellIsAnchor(CellId, true);
				AddedAnchors++;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("SetAnchorsByFiniteSphere: Added=%d, Removed=%d, Radius=%.2f"), AddedAnchors,
	       RemovedAnchors, Radius);
}

void FGridCellBuilder::ClearAllAnchors(FGridCellCache& OutCache)
{
	const int32 TotalCells = OutCache.GetTotalCellCount();
	int32 ClearedCount = 0;

	for (int32 i = 0; i < TotalCells; ++i)
	{
		if (OutCache.GetCellExists(i) && OutCache.GetCellIsAnchor(i))
		{
			OutCache.SetCellIsAnchor(i, false);
			ClearedCount++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ClearAllAnchors: %d cells reset."), ClearedCount);
}

//=============================================================================
// Private Methods
//=============================================================================

void FGridCellBuilder::CalculateGridDimensions(
	const FBox& Bounds,
	const FVector& CellSize,
	FGridCellCache& OutCache)
{
	OutCache.GridOrigin = Bounds.Min;

	const FVector Size = Bounds.GetSize();

	OutCache.GridSize = FIntVector(
		FMath::Max(1, FMath::CeilToInt(Size.X / CellSize.X)),
		FMath::Max(1, FMath::CeilToInt(Size.Y / CellSize.Y)),
		FMath::Max(1, FMath::CeilToInt(Size.Z / CellSize.Z))
	);
}

void FGridCellBuilder::AssignTrianglesToCells(
	const FDynamicMesh3& Mesh,
	FGridCellCache& OutCache)
{
	// 1. 먼저 복셀화 수행 (유효 셀 등록)
	VoxelizeMesh(Mesh, OutCache);

	// 2. 삼각형을 해당 셀에 할당 (희소 배열 사용)
	for (int32 TriId : Mesh.TriangleIndicesItr())
	{
		const FIndex3i Tri = Mesh.GetTriangle(TriId);
		const FVector3d V0 = Mesh.GetVertex(Tri.A);
		const FVector3d V1 = Mesh.GetVertex(Tri.B);
		const FVector3d V2 = Mesh.GetVertex(Tri.C);
		const FVector3d TriCenter = (V0 + V1 + V2) / 3.0;

		const int32 X = FMath::Clamp(
			FMath::FloorToInt((TriCenter.X - OutCache.GridOrigin.X) / OutCache.CellSize.X),
			0, OutCache.GridSize.X - 1);
		const int32 Y = FMath::Clamp(
			FMath::FloorToInt((TriCenter.Y - OutCache.GridOrigin.Y) / OutCache.CellSize.Y),
			0, OutCache.GridSize.Y - 1);
		const int32 Z = FMath::Clamp(
			FMath::FloorToInt((TriCenter.Z - OutCache.GridOrigin.Z) / OutCache.CellSize.Z),
			0, OutCache.GridSize.Z - 1);

		const int32 CellId = OutCache.CoordToId(X, Y, Z);

		// 희소 배열에 삼각형 추가
		FIntArray* Triangles = OutCache.GetCellTrianglesMutable(CellId);
		if (Triangles)
		{
			Triangles->Add(TriId);
		}
	}
}

void FGridCellBuilder::VoxelizeMesh(
	const UE::Geometry::FDynamicMesh3& Mesh,
	FGridCellCache& OutCache)
{
	// DynamicMesh 버전 - 바운딩 박스로 채우기 (Convex 정보 없음)
	const int32 TotalCells = OutCache.GetTotalCellCount();
	for (int32 CellId = 0; CellId < TotalCells; CellId++)
	{
		OutCache.SetCellExists(CellId, true);
		OutCache.RegisterValidCell(CellId);
	}

	UE_LOG(LogTemp, Log, TEXT("VoxelizeMesh: Filled bounding box with %d cells"), TotalCells);
}

void FGridCellBuilder::VoxelizeWithCollision(
	const UBodySetup* BodySetup,
	FGridCellCache& OutCache)
{
	if (!BodySetup)
	{
		return;
	}

	const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
	const int32 TotalCells = OutCache.GetTotalCellCount();

	// Collision 타입별 개수 확인
	const int32 NumConvex = AggGeom.ConvexElems.Num();
	const int32 NumBox = AggGeom.BoxElems.Num();
	const int32 NumSphere = AggGeom.SphereElems.Num();
	const int32 NumCapsule = AggGeom.SphylElems.Num();

	UE_LOG(LogTemp, Log, TEXT("VoxelizeWithCollision: Convex=%d, Box=%d, Sphere=%d, Capsule=%d"),
		NumConvex, NumBox, NumSphere, NumCapsule);

	// Convex 데이터 상세 로그
	for (int32 i = 0; i < NumConvex; i++)
	{
		const FKConvexElem& Elem = AggGeom.ConvexElems[i];
		UE_LOG(LogTemp, Log, TEXT("  Convex[%d]: VertexData=%d, IndexData=%d"),
			i, Elem.VertexData.Num(), Elem.IndexData.Num());
	}

	// Box 데이터 상세 로그
	for (int32 i = 0; i < NumBox; i++)
	{
		const FKBoxElem& Elem = AggGeom.BoxElems[i];
		UE_LOG(LogTemp, Log, TEXT("  Box[%d]: Size=(%.1f, %.1f, %.1f), Center=(%.1f, %.1f, %.1f)"),
			i, Elem.X, Elem.Y, Elem.Z, Elem.Center.X, Elem.Center.Y, Elem.Center.Z);
	}

	// 아무 Collision도 없으면 바운딩 박스로 채우기
	if (NumConvex == 0 && NumBox == 0 && NumSphere == 0 && NumCapsule == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("VoxelizeWithCollision: No collision elements, filling bounding box"));
		for (int32 i = 0; i < TotalCells; i++)
		{
			OutCache.SetCellExists(i, true);
			OutCache.RegisterValidCell(i);
		}
		return;
	}

	// 각 셀에 대해 Collision 내부인지 판단
	// (셀 중심은 런타임 계산, Collision도 로컬 스페이스)
	for (int32 CellId = 0; CellId < TotalCells; CellId++)
	{
		const FVector CellCenterLocal = OutCache.IdToLocalCenter(CellId);
		bool bCellExists = false;

		// Convex 체크
		for (const FKConvexElem& Elem : AggGeom.ConvexElems)
		{
			if (IsPointInsideConvex(Elem, CellCenterLocal))
			{
				bCellExists = true;
				break;
			}
		}

		// Box 체크
		if (!bCellExists)
		{
			for (const FKBoxElem& Elem : AggGeom.BoxElems)
			{
				if (IsPointInsideBox(Elem, CellCenterLocal))
				{
					bCellExists = true;
					break;
				}
			}
		}

		// Sphere 체크
		if (!bCellExists)
		{
			for (const FKSphereElem& Elem : AggGeom.SphereElems)
			{
				if (IsPointInsideSphere(Elem, CellCenterLocal))
				{
					bCellExists = true;
					break;
				}
			}
		}

		// Capsule 체크
		if (!bCellExists)
		{
			for (const FKSphylElem& Elem : AggGeom.SphylElems)
			{
				if (IsPointInsideCapsule(Elem, CellCenterLocal))
				{
					bCellExists = true;
					break;
				}
			}
		}

		// 유효 셀 등록
		if (bCellExists)
		{
			OutCache.SetCellExists(CellId, true);
			OutCache.RegisterValidCell(CellId);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("VoxelizeWithCollision: Valid cells = %d / %d"),
		OutCache.GetValidCellCount(), TotalCells);
}

void FGridCellBuilder::VoxelizeWithConvex(
	const UBodySetup* BodySetup,
	FGridCellCache& OutCache)
{
	// 호환성을 위해 유지 - VoxelizeWithCollision으로 위임
	VoxelizeWithCollision(BodySetup, OutCache);
}


  void FGridCellBuilder::VoxelizeWithTriangles(
      const UStaticMesh* SourceMesh,
      FGridCellCache& OutCache)
  {
      // MeshDescription 가져오기 (Mesh에 대한 자세한 정보가 있음)
      UStaticMeshDescription* StaticMeshDesc = const_cast<UStaticMesh*>(SourceMesh)->GetStaticMeshDescription(0);
      const FMeshDescription* MeshDesc = StaticMeshDesc ? &StaticMeshDesc->GetMeshDescription() : nullptr;

      if (!MeshDesc)
      {
          // MeshDescription 없으면 바운딩 박스로 채우기 (fallback)
          const int32 TotalCells = OutCache.GetTotalCellCount();
          for (int32 i = 0; i < TotalCells; i++)
          {
              OutCache.SetCellExists(i, true);
              OutCache.RegisterValidCell(i);
          }
          return;
      }

      // 정점의 Attribute 가져오기
      FStaticMeshConstAttributes Attributes(*MeshDesc);
      TVertexAttributesConstRef<FVector3f> VertexPositions =
          Attributes.GetVertexPositions();

      // 모든 삼각형 순회
      for (const FTriangleID TriID : MeshDesc->Triangles().GetElementIDs())
      {
          // 삼각형의 3개 정점 인덱스 가져오기
          TArrayView<const FVertexID> TriVertices =
              MeshDesc->GetTriangleVertices(TriID);

          // 3개 정점 좌표
          const FVector V0 = FVector(VertexPositions[TriVertices[0]]);
          const FVector V1 = FVector(VertexPositions[TriVertices[1]]);
          const FVector V2 = FVector(VertexPositions[TriVertices[2]]);

          // 삼각형의 AABB 계산
          FVector TriMin, TriMax;
          TriMin.X = FMath::Min3(V0.X, V1.X, V2.X);
          TriMin.Y = FMath::Min3(V0.Y, V1.Y, V2.Y);
          TriMin.Z = FMath::Min3(V0.Z, V1.Z, V2.Z);
          TriMax.X = FMath::Max3(V0.X, V1.X, V2.X);
          TriMax.Y = FMath::Max3(V0.Y, V1.Y, V2.Y);
          TriMax.Z = FMath::Max3(V0.Z, V1.Z, V2.Z);

          // 삼각형 AABB가 걸치는 셀 범위 계산
          const int32 MinCellX = FMath::Clamp(
              FMath::FloorToInt((TriMin.X - OutCache.GridOrigin.X) / OutCache.CellSize.X),
              0, OutCache.GridSize.X - 1);
          const int32 MinCellY = FMath::Clamp(
              FMath::FloorToInt((TriMin.Y - OutCache.GridOrigin.Y) / OutCache.CellSize.Y),
              0, OutCache.GridSize.Y - 1);
          const int32 MinCellZ = FMath::Clamp(
              FMath::FloorToInt((TriMin.Z - OutCache.GridOrigin.Z) / OutCache.CellSize.Z),
              0, OutCache.GridSize.Z - 1);

          const int32 MaxCellX = FMath::Clamp(
              FMath::FloorToInt((TriMax.X - OutCache.GridOrigin.X) / OutCache.CellSize.X),
              0, OutCache.GridSize.X - 1);
          const int32 MaxCellY = FMath::Clamp(
              FMath::FloorToInt((TriMax.Y - OutCache.GridOrigin.Y) / OutCache.CellSize.Y),
              0, OutCache.GridSize.Y - 1);
          const int32 MaxCellZ = FMath::Clamp(
              FMath::FloorToInt((TriMax.Z - OutCache.GridOrigin.Z) / OutCache.CellSize.Z),
              0, OutCache.GridSize.Z - 1);

          // 해당 범위의 모든 셀을 유효하게 설정
          for (int32 Z = MinCellZ; Z <= MaxCellZ; Z++)
          {
              for (int32 Y = MinCellY; Y <= MaxCellY; Y++)
              {
                  for (int32 X = MinCellX; X <= MaxCellX; X++)
                  {
					  const int32 CellId = OutCache.CoordToId(X, Y, Z);

					  // 이미 포함했으면 skip
					  if (OutCache.GetCellExists(CellId))
					  {
						  continue;
					  }

					  // 없을 때만 교차 검사
					  FVector CellMin(
						  OutCache.GridOrigin.X + X * OutCache.CellSize.X,
						  OutCache.GridOrigin.Y + Y * OutCache.CellSize.Y,
						  OutCache.GridOrigin.Z + Z * OutCache.CellSize.Z
					  );
					  FVector CellMax = CellMin + OutCache.CellSize;

					  if (TriangleIntersectsAABB(V0, V1, V2, CellMin, CellMax))
					  {
						  OutCache.SetCellExists(CellId, true);
						  OutCache.RegisterValidCell(CellId);
					  }
                  }
              }
          }
      }

      UE_LOG(LogTemp, Log, TEXT("VoxelizeWithTriangles: Valid cells = %d"),
          OutCache.GetValidCellCount());
}

bool FGridCellBuilder::TriangleIntersectsAABB(const FVector& V0, const FVector& V1, const FVector& V2, const FVector& BoxMin, const FVector& BoxMax)
{
	// 박스가 0,0,0에 있다고 가정하고 식을 세움, 연산 편리성을 위해서
	 
	// 박스 중심과 반 크기 계산
	const FVector BoxCenter = (BoxMin + BoxMax) * 0.5f;
	const FVector BoxHalfSize = (BoxMax - BoxMin) * 0.5f;

	// 삼각형을 박스 중심 기준으로 이동
	const FVector T0 = V0 - BoxCenter;
	const FVector T1 = V1 - BoxCenter;
	const FVector T2 = V2 - BoxCenter;

	// 삼각형 엣지 벡터
	const FVector E0 = T1 - T0;
	const FVector E1 = T2 - T1;
	const FVector E2 = T0 - T2;

 	// 1. 3개 박스 축 (X, Y, Z)
 
	// X축
	{
		const float Min = FMath::Min3(T0.X, T1.X, T2.X);
		const float Max = FMath::Max3(T0.X, T1.X, T2.X);
		if (Min > BoxHalfSize.X || Max < -BoxHalfSize.X)
		{
			return false;
		}
	}

	// Y축
	{
		const float Min = FMath::Min3(T0.Y, T1.Y, T2.Y);
		const float Max = FMath::Max3(T0.Y, T1.Y, T2.Y);
		if (Min > BoxHalfSize.Y || Max < -BoxHalfSize.Y)
		{
			return false;
		}
	}

	// Z축
	{
		const float Min = FMath::Min3(T0.Z, T1.Z, T2.Z);
		const float Max = FMath::Max3(T0.Z, T1.Z, T2.Z);
		if (Min > BoxHalfSize.Z || Max < -BoxHalfSize.Z)
		{
			return false;
		}
	}

	// 2. 삼각형 노멀 축
	{
		const FVector Normal = FVector::CrossProduct(E0, E1);
		const float D = FVector::DotProduct(Normal, T0);
		const float R = BoxHalfSize.X * FMath::Abs(Normal.X) +
			BoxHalfSize.Y * FMath::Abs(Normal.Y) +
			BoxHalfSize.Z * FMath::Abs(Normal.Z);
		if (FMath::Abs(D) > R)
		{
			return false;
		}
	}

	// 3. 9개 교차 축 (Cross(박스, 삼각형 엣지))
	
	// 헬퍼 람다: 축에 대한 분리 테스트
	auto TestAxis = [&](const FVector& Axis) -> bool
		{
			// 축이 거의 0이면 스킵
			if (Axis.SizeSquared() < KINDA_SMALL_NUMBER)
			{
				return true; // 분리 안됨 (테스트 통과)
			}

			// 삼각형 정점들을 축에 투영
			const float P0 = FVector::DotProduct(Axis, T0);
			const float P1 = FVector::DotProduct(Axis, T1);
			const float P2 = FVector::DotProduct(Axis, T2);

			const float TriMin = FMath::Min3(P0, P1, P2);
			const float TriMax = FMath::Max3(P0, P1, P2);

			// 박스를 축에 투영 (박스 반크기의 축 투영 합)
			const float BoxR = BoxHalfSize.X * FMath::Abs(Axis.X) +
				BoxHalfSize.Y * FMath::Abs(Axis.Y) +
				BoxHalfSize.Z * FMath::Abs(Axis.Z);

			// 분리 테스트
			if (TriMin > BoxR || TriMax < -BoxR)
			{
				return false; // 분리됨!
			}
			return true; // 분리 안됨
		};

	// Cross(X축, 엣지들) 
	if (!TestAxis(FVector(0, -E0.Z, E0.Y))) return false; 
	if (!TestAxis(FVector(0, -E1.Z, E1.Y))) return false; 
	if (!TestAxis(FVector(0, -E2.Z, E2.Y))) return false; 

	// Cross(Y축, 엣지들)
	if (!TestAxis(FVector(E0.Z, 0, -E0.X))) return false; 
	if (!TestAxis(FVector(E1.Z, 0, -E1.X))) return false; 
	if (!TestAxis(FVector(E2.Z, 0, -E2.X))) return false; 

	// Cross(Z축, 엣지들)
	if (!TestAxis(FVector(-E0.Y, E0.X, 0))) return false; 
	if (!TestAxis(FVector(-E1.Y, E1.X, 0))) return false; 
	if (!TestAxis(FVector(-E2.Y, E2.X, 0))) return false; 

	// 모든 테스트 통과 == 교차!
	return true;
}

void FGridCellBuilder::FillInsideVoxels(FGridCellCache& OutCache)
{
	// 방문 여부 체크용 (메모리 아끼려면 TBitArray가 좋지만 편의상 TSet 사용)
	TSet<int32> VisitedOutside;
	TQueue<int32> Queue;

	const FIntVector GridSize = OutCache.GridSize;

	// 1. 초기화: 격자의 가장자리(Boundary) 6면을 큐에 넣음 (여기는 무조건 바깥 공기임)
	for (int32 Z = 0; Z < GridSize.Z; ++Z)
	{
		for (int32 Y = 0; Y < GridSize.Y; ++Y)
		{
			for (int32 X = 0; X < GridSize.X; ++X)
			{
				// 테두리 라인인지 확인
				if (X == 0 || X == GridSize.X - 1 ||
					Y == 0 || Y == GridSize.Y - 1 ||
					Z == 0 || Z == GridSize.Z - 1)
				{
					int32 CellId = OutCache.CoordToId(X, Y, Z);

					// 테두리인데 껍데기(Mesh)가 없다면 -> 확실한 공기(Air)
					if (!OutCache.GetCellExists(CellId))
					{
						Queue.Enqueue(CellId);
						VisitedOutside.Add(CellId);
					}
				}
			}
		}
	}

	// 6방향 탐색용
	static const FIntVector Directions[6] = {
		{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
	};

	// 2. BFS 탐색 (바깥 공기 전파)
	int32 CurrentId;
	while (Queue.Dequeue(CurrentId))
	{
		FIntVector CurrentCoord = OutCache.IdToCoord(CurrentId);

		for (const FIntVector& Dir : Directions)
		{
			FIntVector NextCoord = CurrentCoord + Dir;

			// 격자 밖이면 패스
			if (!OutCache.IsValidCoord(NextCoord)) continue;

			int32 NextId = OutCache.CoordToId(NextCoord);

			// 이미 방문했거나(공기), 껍데기(벽)라면 더 못 들어감
			if (VisitedOutside.Contains(NextId) || OutCache.GetCellExists(NextId))
			{
				continue;
			}

			// 여기까지 왔으면 빈 공간임 -> 방문 처리 후 큐에 추가
			VisitedOutside.Add(NextId);
			Queue.Enqueue(NextId);
		}
	}

	// 3. 반전 (Invert): 공기가 닿지 못한 곳 = 내부
	const int32 TotalCells = OutCache.GetTotalCellCount(); 

	for (int32 i = 0; i < TotalCells; ++i)
	{
		// 이미 껍데기면 건너뜀
		if (OutCache.GetCellExists(i)) continue;

		// 바깥 공기가 도달하지 못한 곳 == 내부
		if (!VisitedOutside.Contains(i))
		{
			OutCache.SetCellExists(i, true); // 채운다!
			OutCache.RegisterValidCell(i); 
		}
	}
	 
}
bool FGridCellBuilder::IsPointInsideConvex(
	const FKConvexElem& ConvexElem,
	const FVector& Point)
{
	const TArray<FVector>& Vertices = ConvexElem.VertexData;

	// VertexData가 없으면 ElemBox (Convex의 바운딩 박스)로 fallback
	if (Vertices.Num() < 4)
	{
		const FBox& ElemBox = ConvexElem.ElemBox;
		if (ElemBox.IsValid)
		{
			return ElemBox.IsInside(Point);
		}
		return false;
	}

	// 바운딩 박스 계산 (빠른 reject)
	FBox ConvexBounds(ForceInit);
	FVector Centroid = FVector::ZeroVector;
	for (const FVector& V : Vertices)
	{
		ConvexBounds += V;
		Centroid += V;
	}
	Centroid /= Vertices.Num();

	// 바운딩 박스 외부면 빠르게 거부
	if (!ConvexBounds.IsInside(Point))
	{
		return false;
	}

	const TArray<int32>& IndexData = ConvexElem.IndexData;

	// IndexData가 없으면 바운딩 박스 체크로 대체
	if (IndexData.Num() < 3)
	{
		return true;
	}

	// 삼각형 면들로 내부 판정
	// Centroid를 기준으로 법선 방향 결정 (Centroid는 항상 내부에 있음)
	for (int32 i = 0; i + 2 < IndexData.Num(); i += 3)
	{
		if (IndexData[i] >= Vertices.Num() ||
		    IndexData[i+1] >= Vertices.Num() ||
		    IndexData[i+2] >= Vertices.Num())
		{
			continue;
		}

		const FVector& V0 = Vertices[IndexData[i]];
		const FVector& V1 = Vertices[IndexData[i + 1]];
		const FVector& V2 = Vertices[IndexData[i + 2]];

		// 면의 법선 계산
		FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();

		// Centroid가 법선의 반대쪽에 있어야 함 (법선은 외부를 향해야 함)
		// 만약 Centroid가 법선 방향에 있으면 법선 뒤집기
		const float CentroidDist = FVector::DotProduct(Centroid - V0, Normal);
		if (CentroidDist > 0)
		{
			Normal = -Normal;  // 법선 뒤집기
		}

		// 점이 면의 바깥쪽에 있으면 Convex 외부
		const float Distance = FVector::DotProduct(Point - V0, Normal);
		if (Distance > KINDA_SMALL_NUMBER)
		{
			return false;
		}
	}

	return true;
}

bool FGridCellBuilder::IsPointInsideBox(
	const FKBoxElem& BoxElem,
	const FVector& Point)
{
	// Center 기준으로 상대 위치 계산
	FVector LocalPoint = Point - BoxElem.Center;

	// 회전이 있으면 적용
	if (!BoxElem.Rotation.IsNearlyZero())
	{
		LocalPoint = BoxElem.Rotation.UnrotateVector(LocalPoint);
	}

	// Half extents (X, Y, Z는 전체 크기이므로 절반으로)
	const FVector HalfExtent(BoxElem.X * 0.5f, BoxElem.Y * 0.5f, BoxElem.Z * 0.5f);

	// AABB 내부 판정
	return FMath::Abs(LocalPoint.X) <= HalfExtent.X &&
	       FMath::Abs(LocalPoint.Y) <= HalfExtent.Y &&
	       FMath::Abs(LocalPoint.Z) <= HalfExtent.Z;
}

bool FGridCellBuilder::IsPointInsideSphere(
	const FKSphereElem& SphereElem,
	const FVector& Point)
{
	// 구 중심에서 점까지 거리
	const FVector Center = SphereElem.Center;
	const float RadiusSq = SphereElem.Radius * SphereElem.Radius;

	return FVector::DistSquared(Point, Center) <= RadiusSq;
}

bool FGridCellBuilder::IsPointInsideCapsule(
	const FKSphylElem& CapsuleElem,
	const FVector& Point)
{
	// Capsule의 로컬 공간으로 변환
	const FTransform CapsuleTransform = CapsuleElem.GetTransform();
	const FVector LocalPoint = CapsuleTransform.InverseTransformPosition(Point);

	const float Radius = CapsuleElem.Radius;
	const float HalfLength = CapsuleElem.Length * 0.5f;

	// Z축 방향으로 캡슐이 정렬되어 있다고 가정
	// 캡슐 = 원통 + 양 끝 반구

	// Z 좌표가 원통 부분인지 반구 부분인지 확인
	if (FMath::Abs(LocalPoint.Z) <= HalfLength)
	{
		// 원통 부분: XY 평면에서 거리 체크
		const float DistXYSq = LocalPoint.X * LocalPoint.X + LocalPoint.Y * LocalPoint.Y;
		return DistXYSq <= Radius * Radius;
	}
	else
	{
		// 반구 부분: 가장 가까운 반구 중심에서 거리 체크
		const FVector HemiCenter(0, 0, LocalPoint.Z > 0 ? HalfLength : -HalfLength);
		return FVector::DistSquared(LocalPoint, HemiCenter) <= Radius * Radius;
	}
}

void FGridCellBuilder::CalculateNeighbors(FGridCellCache& OutCache)
{
	// 6방향 (±X, ±Y, ±Z)
	static const FIntVector Directions[6] = {
		{1, 0, 0}, {-1, 0, 0},
		{0, 1, 0}, {0, -1, 0},
		{0, 0, 1}, {0, 0, -1}
	};

	// 유효 셀만 순회 (희소 배열)
	for (int32 CellId : OutCache.GetValidCellIds())
	{
		const FIntVector Coord = OutCache.IdToCoord(CellId);
		FIntArray* Neighbors = OutCache.GetCellNeighborsMutable(CellId);
		if (!Neighbors)
		{
			continue;
		}

		for (const FIntVector& Dir : Directions)
		{
			const FIntVector NeighborCoord = Coord + Dir;

			// 범위 체크
			if (!OutCache.IsValidCoord(NeighborCoord))
			{
				continue;
			}

			const int32 NeighborId = OutCache.CoordToId(NeighborCoord);

			if (OutCache.GetCellExists(NeighborId))
			{
				Neighbors->Add(NeighborId);
			}
		}
	}
}

void FGridCellBuilder::DetermineAnchors(
	FGridCellCache& OutCache,
	float HeightThreshold)
{
	const float FloorZ = OutCache.GridOrigin.Z;

	// 유효 셀만 순회 (희소 배열)
	for (int32 CellId : OutCache.GetValidCellIds())
	{
		// Z=0 레이어이거나 바닥에 가까운 셀은 앵커
		const FIntVector Coord = OutCache.IdToCoord(CellId);
		const float CellMinZ = OutCache.GridOrigin.Z + Coord.Z * OutCache.CellSize.Z;

		if (CellMinZ - FloorZ <= HeightThreshold)
		{
			OutCache.SetCellIsAnchor(CellId, true);
		}
	}
}
