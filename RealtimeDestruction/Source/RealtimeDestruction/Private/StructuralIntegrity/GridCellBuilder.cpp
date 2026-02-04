// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

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
	FGridCellLayout& OutLayout,
	TMap<int32, FSubCell>* OutSubCellStates)
{
	if (!SourceMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: SourceMesh is null"));
		return false;
	}

	OutLayout.Reset();
	OutLayout.CellSize = CellSize;

	// 1. Compute bounding box (keep local space)
	const FBox LocalBounds = SourceMesh->GetBoundingBox();
	if (!LocalBounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid mesh bounds"));
		return false;
	}

	// Store scale (used for collision checks)
	OutLayout.MeshScale = MeshScale;

	// Compute grid dimensions using scaled size (for cell counts)
	const FVector ScaledSize = LocalBounds.GetSize() * MeshScale;
	const FIntVector GridDimensions(
		FMath::Max(1, FMath::CeilToInt(ScaledSize.X / CellSize.X)),
		FMath::Max(1, FMath::CeilToInt(ScaledSize.Y / CellSize.Y)),
		FMath::Max(1, FMath::CeilToInt(ScaledSize.Z / CellSize.Z))
	);

	// Local-space cell size (inverse scale applied)
	const FVector LocalCellSize(
		CellSize.X / MeshScale.X,
		CellSize.Y / MeshScale.Y,
		CellSize.Z / MeshScale.Z
	);

	// 2. Configure grid (local space)
	OutLayout.GridOrigin = LocalBounds.Min;
	OutLayout.GridSize = GridDimensions;
	OutLayout.CellSize = LocalCellSize;  // Local-space cell size

	const int32 TotalCells = OutLayout.GetTotalCellCount();

	UE_LOG(LogTemp, Log, TEXT("BuildFromStaticMesh: Scale=(%.2f, %.2f, %.2f), ScaledSize=(%.1f, %.1f, %.1f), WorldCellSize=%.1f, LocalCellSize=(%.2f, %.2f, %.2f), Grid=(%d,%d,%d), Total=%d"),
		MeshScale.X, MeshScale.Y, MeshScale.Z,
		ScaledSize.X, ScaledSize.Y, ScaledSize.Z,
		CellSize.X,
		LocalCellSize.X, LocalCellSize.Y, LocalCellSize.Z,
		OutLayout.GridSize.X, OutLayout.GridSize.Y, OutLayout.GridSize.Z,
		TotalCells);

	if (TotalCells <= 0 || TotalCells > 1000000)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid cell count: %d"), TotalCells);
		return false;
	}

	// 3. Initialize bitfields (zeroed)
	OutLayout.InitializeBitfields();

	 // 4. Collision-based voxelization (priority: Convex > Box > Sphere > Capsule > BoundingBox)
	 //UBodySetup* BodySetup = SourceMesh->GetBodySetup();
	 //if (BodySetup)
	 //{
	 //	VoxelizeWithCollision(BodySetup, OutLayout);
	 //}
	 //else
	 //{
	 //	// If no BodySetup, fill with the bounding box
	 //	UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: No BodySetup, filling bounding box"));
	 //	for (int32 i = 0; i < TotalCells; i++)
	 //	{
	 //		OutLayout.SetCellExists(i, true);
	 //		OutLayout.RegisterValidCell(i);
	 //	}
	 //}
	
	VoxelizeWithTriangles(SourceMesh, OutLayout, OutSubCellStates);

	 FillInsideVoxels(OutLayout);

	// 6. Compute neighbors
	CalculateNeighbors(OutLayout);

	// 7. Determine anchors
	DetermineAnchors(OutLayout, AnchorHeightThreshold);

	UE_LOG(LogTemp, Log, TEXT("FGridCellBuilder: Built grid %dx%dx%d, valid cells: %d"),
		OutLayout.GridSize.X, OutLayout.GridSize.Y, OutLayout.GridSize.Z,
		OutLayout.GetValidCellCount());

	return true;
}

bool FGridCellBuilder::BuildFromDynamicMesh(
	const FDynamicMesh3& Mesh,
	const FVector& CellSize,
	float AnchorHeightThreshold,
	FGridCellLayout& OutLayout)
{
	OutLayout.Reset();
	OutLayout.CellSize = CellSize;

	// 1. Compute bounding box
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

	// 2. Compute grid dimensions
	CalculateGridDimensions(UnrealBounds, CellSize, OutLayout);

	const int32 TotalCells = OutLayout.GetTotalCellCount();
	if (TotalCells <= 0 || TotalCells > 1000000)  // 1,000,000 cell limit
	{
		UE_LOG(LogTemp, Warning, TEXT("FGridCellBuilder: Invalid cell count: %d"), TotalCells);
		return false;
	}

	// 3. Initialize bitfields (zeroed)
	OutLayout.InitializeBitfields();

	// 4. Assign triangles
	AssignTrianglesToCells(Mesh, OutLayout);

	// 6. Compute neighbors
	CalculateNeighbors(OutLayout);

	// 7. Determine anchors
	DetermineAnchors(OutLayout, AnchorHeightThreshold);

	UE_LOG(LogTemp, Log, TEXT("FGridCellBuilder: Built grid %dx%dx%d, valid cells: %d"),
		OutLayout.GridSize.X, OutLayout.GridSize.Y, OutLayout.GridSize.Z,
		OutLayout.GetValidCellCount());

	return true;
}

void FGridCellBuilder::MarkIntersectingSubCellsAlive(
	const FVector& V0, const FVector& V1, const FVector& V2,
	const FVector& CellMin, const FVector& CellSize,
	FSubCell& OutSubCellState)
{
	const FVector SubCellSize = CellSize / static_cast<float>(SUBCELL_DIVISION);

	for (int32 SubCellId = 0; SubCellId < SUBCELL_COUNT; ++SubCellId)
	{
		if (OutSubCellState.IsSubCellAlive(SubCellId))
		{
			continue;
		}

		const FIntVector SubCoord = SubCellIdToCoord(SubCellId);
		const FVector SubCellMin = CellMin + FVector(
			SubCoord.X * SubCellSize.X,
			SubCoord.Y * SubCellSize.Y,
			SubCoord.Z * SubCellSize.Z);

		const FVector SubCellMax = SubCellMin + SubCellSize;

		if (TriangleIntersectsAABB(V0, V1, V2, SubCellMin, SubCellMax))
		{
			OutSubCellState.Bits |= (1 << SubCellId);
		}
	}
}

void FGridCellBuilder::SetAnchorsByFinitePlane(
	const FTransform& PlaneTransform,
	const FTransform& MeshTransform,
	FGridCellLayout& OutLayout,
	bool bIsEraser)
{
	const int32 TotalCells = OutLayout.GetTotalCellCount();
	int32 AddedAnchors = 0;

	const float CubeExtent = 50.0f;

	for (int32 CellId = 0; CellId < TotalCells; ++CellId)
	{
		if (!OutLayout.GetCellExists(CellId))
		{
			continue;
		}

		FVector LocalPos = OutLayout.IdToLocalCenter(CellId);
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
					if (OutLayout.GetCellExists(CellId))
					{
						OutLayout.SetCellIsAnchor(CellId, false);
					}
				}
				else
				{
					if (!OutLayout.GetCellIsAnchor(CellId))
					{
						OutLayout.SetCellIsAnchor(CellId, true);
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
	FGridCellLayout& OutLayout,
	bool bIsEraser)
{
	const int32 TotalCells = OutLayout.GetTotalCellCount();
	int32 AddedAnchors = 0;
	int32 RemovedAnchors = 0;

	for (int32 CellId = 0; CellId < TotalCells; ++CellId)
	{
		if (!OutLayout.GetCellExists(CellId))
		{
			continue;
		}

		const FVector LocalPos = OutLayout.IdToLocalCenter(CellId);
		const FVector WorldPos = MeshTransform.TransformPosition(LocalPos);

		// World -> box local (includes rotation/scale)
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
			if (OutLayout.GetCellIsAnchor(CellId))
			{
				OutLayout.SetCellIsAnchor(CellId, false);
				RemovedAnchors++;
			}
		}
		else
		{
			if (!OutLayout.GetCellIsAnchor(CellId))
			{
				OutLayout.SetCellIsAnchor(CellId, true);
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
	FGridCellLayout& OutLayout,
	bool bIsEraser)
{
	const int32 TotalCells = OutLayout.GetTotalCellCount();
	int32 AddedAnchors = 0;
	int32 RemovedAnchors = 0;

	const float Radius = FMath::Max(0.0f, SphereRadius);
	const float RadiusSq = Radius * Radius;

	for (int32 CellId = 0; CellId < TotalCells; ++CellId)
	{
		if (!OutLayout.GetCellExists(CellId))
		{
			continue;
		}

		const FVector LocalPos = OutLayout.IdToLocalCenter(CellId);
		const FVector WorldPos = MeshTransform.TransformPosition(LocalPos);

		// World -> sphere local (inverse transform includes scale)
		// With non-uniform scale in SphereTransform, this becomes an ellipsoid test in world space.
		const FVector SphereSpacePos = SphereTransform.InverseTransformPosition(WorldPos);

		const bool bInside = SphereSpacePos.SizeSquared() <= RadiusSq;
		if (!bInside)
		{
			continue;
		}

		if (bIsEraser)
		{
			if (OutLayout.GetCellIsAnchor(CellId))
			{
				OutLayout.SetCellIsAnchor(CellId, false);
				RemovedAnchors++;
			}
		}
		else
		{
			if (!OutLayout.GetCellIsAnchor(CellId))
			{
				OutLayout.SetCellIsAnchor(CellId, true);
				AddedAnchors++;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("SetAnchorsByFiniteSphere: Added=%d, Removed=%d, Radius=%.2f"), AddedAnchors,
	       RemovedAnchors, Radius);
}

void FGridCellBuilder::ClearAllAnchors(FGridCellLayout& OutLayout)
{
	const int32 TotalCells = OutLayout.GetTotalCellCount();
	int32 ClearedCount = 0;

	for (int32 i = 0; i < TotalCells; ++i)
	{
		if (OutLayout.GetCellExists(i) && OutLayout.GetCellIsAnchor(i))
		{
			OutLayout.SetCellIsAnchor(i, false);
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
	FGridCellLayout& OutLayout)
{
	OutLayout.GridOrigin = Bounds.Min;

	const FVector Size = Bounds.GetSize();

	OutLayout.GridSize = FIntVector(
		FMath::Max(1, FMath::CeilToInt(Size.X / CellSize.X)),
		FMath::Max(1, FMath::CeilToInt(Size.Y / CellSize.Y)),
		FMath::Max(1, FMath::CeilToInt(Size.Z / CellSize.Z))
	);
}

void FGridCellBuilder::AssignTrianglesToCells(
	const FDynamicMesh3& Mesh,
	FGridCellLayout& OutLayout)
{
	// 1. Voxelize first (register valid cells)
	VoxelizeMesh(Mesh, OutLayout);

	// 2. Assign triangles to cells (sparse array)
	for (int32 TriId : Mesh.TriangleIndicesItr())
	{
		const FIndex3i Tri = Mesh.GetTriangle(TriId);
		const FVector3d V0 = Mesh.GetVertex(Tri.A);
		const FVector3d V1 = Mesh.GetVertex(Tri.B);
		const FVector3d V2 = Mesh.GetVertex(Tri.C);
		const FVector3d TriCenter = (V0 + V1 + V2) / 3.0;

		const int32 X = FMath::Clamp(
			FMath::FloorToInt((TriCenter.X - OutLayout.GridOrigin.X) / OutLayout.CellSize.X),
			0, OutLayout.GridSize.X - 1);
		const int32 Y = FMath::Clamp(
			FMath::FloorToInt((TriCenter.Y - OutLayout.GridOrigin.Y) / OutLayout.CellSize.Y),
			0, OutLayout.GridSize.Y - 1);
		const int32 Z = FMath::Clamp(
			FMath::FloorToInt((TriCenter.Z - OutLayout.GridOrigin.Z) / OutLayout.CellSize.Z),
			0, OutLayout.GridSize.Z - 1);

		const int32 CellId = OutLayout.CoordToId(X, Y, Z);

		// Add triangle to sparse array
		FIntArray* Triangles = OutLayout.GetCellTrianglesMutable(CellId);
		if (Triangles)
		{
			Triangles->Add(TriId);
		}
	}
}

void FGridCellBuilder::VoxelizeMesh(
	const UE::Geometry::FDynamicMesh3& Mesh,
	FGridCellLayout& OutLayout)
{
	// DynamicMesh version - fill with bounding box (no convex data)
	const int32 TotalCells = OutLayout.GetTotalCellCount();
	for (int32 CellId = 0; CellId < TotalCells; CellId++)
	{
		OutLayout.SetCellExists(CellId, true);
		OutLayout.RegisterValidCell(CellId);
	}

	UE_LOG(LogTemp, Log, TEXT("VoxelizeMesh: Filled bounding box with %d cells"), TotalCells);
}

void FGridCellBuilder::VoxelizeWithCollision(
	const UBodySetup* BodySetup,
	FGridCellLayout& OutLayout)
{
	if (!BodySetup)
	{
		return;
	}

	const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
	const int32 TotalCells = OutLayout.GetTotalCellCount();

	// Check counts by collision type
	const int32 NumConvex = AggGeom.ConvexElems.Num();
	const int32 NumBox = AggGeom.BoxElems.Num();
	const int32 NumSphere = AggGeom.SphereElems.Num();
	const int32 NumCapsule = AggGeom.SphylElems.Num();

	UE_LOG(LogTemp, Log, TEXT("VoxelizeWithCollision: Convex=%d, Box=%d, Sphere=%d, Capsule=%d"),
		NumConvex, NumBox, NumSphere, NumCapsule);

	// Convex data detail log
	for (int32 i = 0; i < NumConvex; i++)
	{
		const FKConvexElem& Elem = AggGeom.ConvexElems[i];
		UE_LOG(LogTemp, Log, TEXT("  Convex[%d]: VertexData=%d, IndexData=%d"),
			i, Elem.VertexData.Num(), Elem.IndexData.Num());
	}

	// Box data detail log
	for (int32 i = 0; i < NumBox; i++)
	{
		const FKBoxElem& Elem = AggGeom.BoxElems[i];
		UE_LOG(LogTemp, Log, TEXT("  Box[%d]: Size=(%.1f, %.1f, %.1f), Center=(%.1f, %.1f, %.1f)"),
			i, Elem.X, Elem.Y, Elem.Z, Elem.Center.X, Elem.Center.Y, Elem.Center.Z);
	}

	// If no collision, fill with bounding box
	if (NumConvex == 0 && NumBox == 0 && NumSphere == 0 && NumCapsule == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("VoxelizeWithCollision: No collision elements, filling bounding box"));
		for (int32 i = 0; i < TotalCells; i++)
		{
			OutLayout.SetCellExists(i, true);
			OutLayout.RegisterValidCell(i);
		}
		return;
	}

	// For each cell, test whether it lies inside collision
	// (cell centers are computed at runtime; collision is in local space)
	for (int32 CellId = 0; CellId < TotalCells; CellId++)
	{
		const FVector CellCenterLocal = OutLayout.IdToLocalCenter(CellId);
		bool bCellExists = false;

		// Convex check
		for (const FKConvexElem& Elem : AggGeom.ConvexElems)
		{
			if (IsPointInsideConvex(Elem, CellCenterLocal))
			{
				bCellExists = true;
				break;
			}
		}

		// Box check
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

		// Sphere check
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

		// Capsule check
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

		// Register valid cell
		if (bCellExists)
		{
			OutLayout.SetCellExists(CellId, true);
			OutLayout.RegisterValidCell(CellId);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("VoxelizeWithCollision: Valid cells = %d / %d"),
		OutLayout.GetValidCellCount(), TotalCells);
}

void FGridCellBuilder::VoxelizeWithConvex(
	const UBodySetup* BodySetup,
	FGridCellLayout& OutLayout)
{
	// Kept for compatibility - delegate to VoxelizeWithCollision
	VoxelizeWithCollision(BodySetup, OutLayout);
}


void FGridCellBuilder::VoxelizeWithTriangles(
    const UStaticMesh* SourceMesh,
    FGridCellLayout& OutLayout,
    TMap<int32, FSubCell>* OutSubCellStates)
{
    // Method 0: Try cached triangle data first (works in packaged builds)
    if (OutLayout.HasCachedTriangleData())
    {
        UE_LOG(LogTemp, Log, TEXT("VoxelizeWithTriangles: Using CachedTriangleData (Vertices=%d, Triangles=%d)"),
            OutLayout.CachedVertices.Num(), OutLayout.CachedIndices.Num() / 3);

        VoxelizeFromArrays(OutLayout.CachedVertices, OutLayout.CachedIndices, OutLayout, OutSubCellStates);
        return;
    }

    // Method 1: MeshDescription (original method - most accurate in editor)
    UStaticMeshDescription* StaticMeshDesc = const_cast<UStaticMesh*>(SourceMesh)->GetStaticMeshDescription(0);
    const FMeshDescription* MeshDesc = StaticMeshDesc ? &StaticMeshDesc->GetMeshDescription() : nullptr;

    if (MeshDesc)
    {
        FStaticMeshConstAttributes Attributes(*MeshDesc);
        TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();

        const int32 NumVerts = MeshDesc->Vertices().Num();
        const int32 NumTris = MeshDesc->Triangles().Num();

        if (NumVerts > 0 && NumTris > 0)
        {
            UE_LOG(LogTemp, Log, TEXT("VoxelizeWithTriangles: Using MeshDescription (Vertices=%d, Triangles=%d)"), NumVerts, NumTris);

#if WITH_EDITOR
            // Prepare caching data
            TArray<FVector> CacheVertices;
            TArray<uint32> CacheIndices;
            TMap<FVertexID, uint32> VertexIDToIndex;

            CacheVertices.SetNum(NumVerts);
            CacheIndices.Reserve(NumTris * 3);

            int32 VertIdx = 0;
            for (const FVertexID VertID : MeshDesc->Vertices().GetElementIDs())
            {
                CacheVertices[VertIdx] = FVector(VertexPositions[VertID]);
                VertexIDToIndex.Add(VertID, VertIdx);
                VertIdx++;
            }
#endif

            // Original method: directly iterate triangles from MeshDescription
            for (const FTriangleID TriID : MeshDesc->Triangles().GetElementIDs())
            {
                TArrayView<const FVertexID> TriVertices = MeshDesc->GetTriangleVertices(TriID);

                // Get vertex positions directly (original method)
                const FVector V0 = FVector(VertexPositions[TriVertices[0]]);
                const FVector V1 = FVector(VertexPositions[TriVertices[1]]);
                const FVector V2 = FVector(VertexPositions[TriVertices[2]]);

#if WITH_EDITOR
                // Collect indices for caching
                CacheIndices.Add(VertexIDToIndex[TriVertices[0]]);
                CacheIndices.Add(VertexIDToIndex[TriVertices[1]]);
                CacheIndices.Add(VertexIDToIndex[TriVertices[2]]);
#endif

                // Original voxelization logic
                VoxelizeTriangle(V0, V1, V2, OutLayout, OutSubCellStates);
            }

#if WITH_EDITOR
            // Cache triangle data for runtime use
            if (!OutLayout.HasCachedTriangleData())
            {
                OutLayout.CachedVertices = MoveTemp(CacheVertices);
                OutLayout.CachedIndices = MoveTemp(CacheIndices);
                UE_LOG(LogTemp, Log, TEXT("VoxelizeWithTriangles: Cached triangle data for runtime use"));
            }
#endif

            UE_LOG(LogTemp, Log, TEXT("VoxelizeWithTriangles: Valid cells = %d"), OutLayout.GetValidCellCount());
            return;
        }
    }

    // Method 2: Fallback to bounding box fill
    UE_LOG(LogTemp, Warning, TEXT("VoxelizeWithTriangles: No triangle data available. Falling back to bounding box fill."));

    const int32 TotalCells = OutLayout.GetTotalCellCount();
    for (int32 CellId = 0; CellId < TotalCells; ++CellId)
    {
        OutLayout.SetCellExists(CellId, true);
        OutLayout.RegisterValidCell(CellId);
    }
}

void FGridCellBuilder::VoxelizeTriangle(
    const FVector& V0,
    const FVector& V1,
    const FVector& V2,
    FGridCellLayout& OutLayout,
    TMap<int32, FSubCell>* OutSubCellStates)
{
    // Compute triangle AABB
    FVector TriMin, TriMax;
    TriMin.X = FMath::Min3(V0.X, V1.X, V2.X);
    TriMin.Y = FMath::Min3(V0.Y, V1.Y, V2.Y);
    TriMin.Z = FMath::Min3(V0.Z, V1.Z, V2.Z);
    TriMax.X = FMath::Max3(V0.X, V1.X, V2.X);
    TriMax.Y = FMath::Max3(V0.Y, V1.Y, V2.Y);
    TriMax.Z = FMath::Max3(V0.Z, V1.Z, V2.Z);

    // Compute cell range overlapped by the triangle AABB
    const int32 MinCellX = FMath::Clamp(
        FMath::FloorToInt((TriMin.X - OutLayout.GridOrigin.X) / OutLayout.CellSize.X),
        0, OutLayout.GridSize.X - 1);
    const int32 MinCellY = FMath::Clamp(
        FMath::FloorToInt((TriMin.Y - OutLayout.GridOrigin.Y) / OutLayout.CellSize.Y),
        0, OutLayout.GridSize.Y - 1);
    const int32 MinCellZ = FMath::Clamp(
        FMath::FloorToInt((TriMin.Z - OutLayout.GridOrigin.Z) / OutLayout.CellSize.Z),
        0, OutLayout.GridSize.Z - 1);

    const int32 MaxCellX = FMath::Clamp(
        FMath::FloorToInt((TriMax.X - OutLayout.GridOrigin.X) / OutLayout.CellSize.X),
        0, OutLayout.GridSize.X - 1);
    const int32 MaxCellY = FMath::Clamp(
        FMath::FloorToInt((TriMax.Y - OutLayout.GridOrigin.Y) / OutLayout.CellSize.Y),
        0, OutLayout.GridSize.Y - 1);
    const int32 MaxCellZ = FMath::Clamp(
        FMath::FloorToInt((TriMax.Z - OutLayout.GridOrigin.Z) / OutLayout.CellSize.Z),
        0, OutLayout.GridSize.Z - 1);

    // Mark all cells in range as valid
    for (int32 Z = MinCellZ; Z <= MaxCellZ; Z++)
    {
        for (int32 Y = MinCellY; Y <= MaxCellY; Y++)
        {
            for (int32 X = MinCellX; X <= MaxCellX; X++)
            {
                const int32 CellId = OutLayout.CoordToId(X, Y, Z);

                FVector CellMin(
                    OutLayout.GridOrigin.X + X * OutLayout.CellSize.X,
                    OutLayout.GridOrigin.Y + Y * OutLayout.CellSize.Y,
                    OutLayout.GridOrigin.Z + Z * OutLayout.CellSize.Z
                );
                FVector CellMax = CellMin + OutLayout.CellSize;

                if (!OutLayout.GetCellExists(CellId))
                {
                    if (TriangleIntersectsAABB(V0, V1, V2, CellMin, CellMax))
                    {
                        OutLayout.SetCellExists(CellId, true);
                        OutLayout.RegisterValidCell(CellId);

                        if (OutSubCellStates)
                        {
                            FSubCell& SubCellState = OutSubCellStates->FindOrAdd(CellId);
                            SubCellState.Bits = 0x00;
                            MarkIntersectingSubCellsAlive(V0, V1, V2, CellMin, OutLayout.CellSize, SubCellState);
                        }
                    }
                }
                // Cell이 이미 존재하는 경우
                else if (OutSubCellStates)
                {
                    FSubCell* SubCellState = OutSubCellStates->Find(CellId);
                    // 아직 모든 subcell이 alive가 아니면, 한 번 더 체크
                    if (SubCellState && SubCellState->Bits != 0xFF)
                    {
                        if (TriangleIntersectsAABB(V0, V1, V2, CellMin, CellMax))
                        {
                            MarkIntersectingSubCellsAlive(V0, V1, V2, CellMin, OutLayout.CellSize, *SubCellState);
                        }
                    }
                }
            }
        }
    }
}

void FGridCellBuilder::VoxelizeFromArrays(
    const TArray<FVector>& Vertices,
    const TArray<uint32>& Indices,
    FGridCellLayout& OutLayout,
    TMap<int32, FSubCell>* OutSubCellStates)
{
    const uint32 NumVertices = Vertices.Num();
    const uint32 NumTriangles = Indices.Num() / 3;

    for (uint32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = Indices[TriIdx * 3 + 0];
        const uint32 I1 = Indices[TriIdx * 3 + 1];
        const uint32 I2 = Indices[TriIdx * 3 + 2];

        if (I0 >= NumVertices || I1 >= NumVertices || I2 >= NumVertices)
        {
            continue;
        }

        VoxelizeTriangle(Vertices[I0], Vertices[I1], Vertices[I2], OutLayout, OutSubCellStates);
    }

    UE_LOG(LogTemp, Log, TEXT("VoxelizeFromArrays: Valid cells = %d"), OutLayout.GetValidCellCount());
}

bool FGridCellBuilder::TriangleIntersectsAABB(const FVector& V0, const FVector& V1, const FVector& V2, const FVector& BoxMin, const FVector& BoxMax)
{
	// Assume the box is at (0,0,0) to simplify the math
	 
	// Compute box center and half size
	const FVector BoxCenter = (BoxMin + BoxMax) * 0.5f;

	const FVector BoxEpsilon = (BoxMax - BoxMin) * 0.5f * FVector(0.01f);

	const FVector BoxHalfSize = (BoxMax - BoxMin) * 0.5f + BoxEpsilon;

	// Move the triangle relative to the box center
	const FVector T0 = V0 - BoxCenter;
	const FVector T1 = V1 - BoxCenter;
	const FVector T2 = V2 - BoxCenter;

	// Triangle edge vectors
	const FVector E0 = T1 - T0;
	const FVector E1 = T2 - T1;
	const FVector E2 = T0 - T2;

 	// 1. 3 box axes (X, Y, Z)
 
	// X axis
	{
		const float Min = FMath::Min3(T0.X, T1.X, T2.X);
		const float Max = FMath::Max3(T0.X, T1.X, T2.X);
		if (Min > BoxHalfSize.X || Max < -BoxHalfSize.X)
		{
			return false;
		}
	}

	// Y axis
	{
		const float Min = FMath::Min3(T0.Y, T1.Y, T2.Y);
		const float Max = FMath::Max3(T0.Y, T1.Y, T2.Y);
		if (Min > BoxHalfSize.Y || Max < -BoxHalfSize.Y)
		{
			return false;
		}
	}

	// Z axis
	{
		const float Min = FMath::Min3(T0.Z, T1.Z, T2.Z);
		const float Max = FMath::Max3(T0.Z, T1.Z, T2.Z);
		if (Min > BoxHalfSize.Z || Max < -BoxHalfSize.Z)
		{
			return false;
		}
	}

	// 2. Triangle normal axis
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

	// 3. 9 cross axes (Cross(box axes, triangle edges))
	
	// Helper lambda: separating axis test
	auto TestAxis = [&](const FVector& Axis) -> bool
		{
			// Skip if the axis is nearly zero
			if (Axis.SizeSquared() < KINDA_SMALL_NUMBER)
			{
				return true; // Not separated (test passes)
			}

			// Project triangle vertices onto the axis
			const float P0 = FVector::DotProduct(Axis, T0);
			const float P1 = FVector::DotProduct(Axis, T1);
			const float P2 = FVector::DotProduct(Axis, T2);

			const float TriMin = FMath::Min3(P0, P1, P2);
			const float TriMax = FMath::Max3(P0, P1, P2);

			// Project the box onto the axis (sum of half-extent projections)
			const float BoxR = BoxHalfSize.X * FMath::Abs(Axis.X) +
				BoxHalfSize.Y * FMath::Abs(Axis.Y) +
				BoxHalfSize.Z * FMath::Abs(Axis.Z);

			// Separating axis test
			if (TriMin > BoxR || TriMax < -BoxR)
			{
				return false; // Separated!
			}
			return true; // Not separated
		};

	// Cross(X axis, edges)
	if (!TestAxis(FVector(0, -E0.Z, E0.Y))) return false; 
	if (!TestAxis(FVector(0, -E1.Z, E1.Y))) return false; 
	if (!TestAxis(FVector(0, -E2.Z, E2.Y))) return false; 

	// Cross(Y axis, edges)
	if (!TestAxis(FVector(E0.Z, 0, -E0.X))) return false; 
	if (!TestAxis(FVector(E1.Z, 0, -E1.X))) return false; 
	if (!TestAxis(FVector(E2.Z, 0, -E2.X))) return false; 

	// Cross(Z axis, edges)
	if (!TestAxis(FVector(-E0.Y, E0.X, 0))) return false; 
	if (!TestAxis(FVector(-E1.Y, E1.X, 0))) return false; 
	if (!TestAxis(FVector(-E2.Y, E2.X, 0))) return false; 

	// All tests passed == intersect
	return true;
}

void FGridCellBuilder::FillInsideVoxels(FGridCellLayout& OutLayout)
{
	// Visited check (TBitArray is more memory-friendly, but TSet is used for convenience)
	TSet<int32> VisitedOutside;
	TQueue<int32> Queue;

	const FIntVector GridSize = OutLayout.GridSize;
	 
	// 1. Initialize: enqueue the 6 boundary faces of the grid (always outside air)
	for (int32 Z = 0; Z < GridSize.Z; ++Z)
	{
		for (int32 Y = 0; Y < GridSize.Y; ++Y)
		{
			for (int32 X = 0; X < GridSize.X; ++X)
			{
				// Check if on the boundary
				if (X == 0 || X == GridSize.X - 1 ||
					Y == 0 || Y == GridSize.Y - 1 ||
					Z == 0 || Z == GridSize.Z - 1)
				{
					int32 CellId = OutLayout.CoordToId(X, Y, Z);

					// Boundary without shell (mesh) -> definitely air
					if (!OutLayout.GetCellExists(CellId))
					{
						Queue.Enqueue(CellId);
						VisitedOutside.Add(CellId);
					}
				}
			}
		}
	} 
	  
	// For 6-direction traversal
	static const FIntVector Directions[6] = {
		{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
	};

	// 2. BFS traversal (propagate outside air)
	int32 CurrentId;
	while (Queue.Dequeue(CurrentId))
	{
		FIntVector CurrentCoord = OutLayout.IdToCoord(CurrentId);

		for (const FIntVector& Dir : Directions)
		{
			FIntVector NextCoord = CurrentCoord + Dir;

			// Skip if outside the grid
			if (!OutLayout.IsValidCoord(NextCoord)) continue;

			int32 NextId = OutLayout.CoordToId(NextCoord);

			// If already visited (air) or shell (wall), cannot proceed
			if (VisitedOutside.Contains(NextId) || OutLayout.GetCellExists(NextId))
			{
				continue;
			}

			// Reached empty space -> mark visited and enqueue
			VisitedOutside.Add(NextId);
			Queue.Enqueue(NextId);
		}
	}

	// 3. Invert: areas unreachable by air are interior
	const int32 TotalCells = OutLayout.GetTotalCellCount(); 

	for (int32 i = 0; i < TotalCells; ++i)
	{
		// Skip if already shell
		if (OutLayout.GetCellExists(i)) continue;

		// Outside air cannot reach -> interior
		if (!VisitedOutside.Contains(i))
		{
			OutLayout.SetCellExists(i, true); // Fill
			OutLayout.RegisterValidCell(i); 
		}
	} 

}
bool FGridCellBuilder::IsPointInsideConvex(
	const FKConvexElem& ConvexElem,
	const FVector& Point)
{
	const TArray<FVector>& Vertices = ConvexElem.VertexData;

	// If no VertexData, fall back to ElemBox (convex bounding box)
	if (Vertices.Num() < 4)
	{
		const FBox& ElemBox = ConvexElem.ElemBox;
		if (ElemBox.IsValid)
		{
			return ElemBox.IsInside(Point);
		}
		return false;
	}

	// Compute bounding box (quick reject)
	FBox ConvexBounds(ForceInit);
	FVector Centroid = FVector::ZeroVector;
	for (const FVector& V : Vertices)
	{
		ConvexBounds += V;
		Centroid += V;
	}
	Centroid /= Vertices.Num();

	// Quick reject if outside the bounding box
	if (!ConvexBounds.IsInside(Point))
	{
		return false;
	}

	const TArray<int32>& IndexData = ConvexElem.IndexData;

	// If no IndexData, fall back to bounding box check
	if (IndexData.Num() < 3)
	{
		return true;
	}

	// Determine inside using triangle faces
	// Set normal direction using the centroid (centroid is always inside)
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

		// Compute face normal
		FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();

		// Centroid must be on the opposite side (normal should face outward)
		// Flip the normal if the centroid lies on the normal side
		const float CentroidDist = FVector::DotProduct(Centroid - V0, Normal);
		if (CentroidDist > 0)
		{
			Normal = -Normal;  // Flip normal
		}

		// If the point is outside any face, it's outside the convex
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
	// Compute relative position to the center
	FVector LocalPoint = Point - BoxElem.Center;

	// Apply rotation if present
	if (!BoxElem.Rotation.IsNearlyZero())
	{
		LocalPoint = BoxElem.Rotation.UnrotateVector(LocalPoint);
	}

	// Half extents (X, Y, Z are full sizes, so halve them)
	const FVector HalfExtent(BoxElem.X * 0.5f, BoxElem.Y * 0.5f, BoxElem.Z * 0.5f);

	// AABB containment check
	return FMath::Abs(LocalPoint.X) <= HalfExtent.X &&
	       FMath::Abs(LocalPoint.Y) <= HalfExtent.Y &&
	       FMath::Abs(LocalPoint.Z) <= HalfExtent.Z;
}

bool FGridCellBuilder::IsPointInsideSphere(
	const FKSphereElem& SphereElem,
	const FVector& Point)
{
	// Distance from sphere center to point
	const FVector Center = SphereElem.Center;
	const float RadiusSq = SphereElem.Radius * SphereElem.Radius;

	return FVector::DistSquared(Point, Center) <= RadiusSq;
}

bool FGridCellBuilder::IsPointInsideCapsule(
	const FKSphylElem& CapsuleElem,
	const FVector& Point)
{
	// Transform to capsule local space
	const FTransform CapsuleTransform = CapsuleElem.GetTransform();
	const FVector LocalPoint = CapsuleTransform.InverseTransformPosition(Point);

	const float Radius = CapsuleElem.Radius;
	const float HalfLength = CapsuleElem.Length * 0.5f;

	// Assume capsule is aligned along the Z axis
	// Capsule = cylinder + two hemispheres

	// Check whether Z is in the cylinder or hemisphere region
	if (FMath::Abs(LocalPoint.Z) <= HalfLength)
	{
		// Cylinder section: distance check in XY plane
		const float DistXYSq = LocalPoint.X * LocalPoint.X + LocalPoint.Y * LocalPoint.Y;
		return DistXYSq <= Radius * Radius;
	}
	else
	{
		// Hemisphere section: distance check from nearest hemisphere center
		const FVector HemiCenter(0, 0, LocalPoint.Z > 0 ? HalfLength : -HalfLength);
		return FVector::DistSquared(LocalPoint, HemiCenter) <= Radius * Radius;
	}
}

void FGridCellBuilder::CalculateNeighbors(FGridCellLayout& OutLayout)
{
	// 6 directions (+/-X, +/-Y, +/-Z)
	static const FIntVector Directions[6] = {
		{1, 0, 0}, {-1, 0, 0},
		{0, 1, 0}, {0, -1, 0},
		{0, 0, 1}, {0, 0, -1}
	};

	// Iterate valid cells only (sparse array)
	for (int32 CellId : OutLayout.GetValidCellIds())
	{
		const FIntVector Coord = OutLayout.IdToCoord(CellId);
		FIntArray* Neighbors = OutLayout.GetCellNeighborsMutable(CellId);
		if (!Neighbors)
		{
			continue;
		}

		for (const FIntVector& Dir : Directions)
		{
			const FIntVector NeighborCoord = Coord + Dir;

			// Range check
			if (!OutLayout.IsValidCoord(NeighborCoord))
			{
				continue;
			}

			const int32 NeighborId = OutLayout.CoordToId(NeighborCoord);

			if (OutLayout.GetCellExists(NeighborId))
			{
				Neighbors->Add(NeighborId);
			}
		}
	}
}

void FGridCellBuilder::DetermineAnchors(
	FGridCellLayout& OutLayout,
	float HeightThreshold)
{
	const float FloorZ = OutLayout.GridOrigin.Z;

	// Iterate valid cells only (sparse array)
	for (int32 CellId : OutLayout.GetValidCellIds())
	{
		// Cells on the Z=0 layer or near the floor are anchors
		const FIntVector Coord = OutLayout.IdToCoord(CellId);
		const float CellMinZ = OutLayout.GridOrigin.Z + Coord.Z * OutLayout.CellSize.Z;

		if (CellMinZ - FloorZ <= HeightThreshold)
		{
			OutLayout.SetCellIsAnchor(CellId, true);
		}
	}
}
