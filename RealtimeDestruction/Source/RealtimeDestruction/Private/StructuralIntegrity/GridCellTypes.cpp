// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "StructuralIntegrity/GridCellTypes.h"
#include "Components/RealtimeDestructibleMeshComponent.h"

//=============================================================================
// FDestructionShape
//=============================================================================

bool FCellDestructionShape::ContainsPoint(const FVector& Point) const
{
	switch (Type)
	{
	case ECellDestructionShapeType::Sphere:
		return FVector::DistSquared(Point, Center) <= Radius * Radius;

	case ECellDestructionShapeType::Box:
		{
			// With rotation
			if (!Rotation.IsNearlyZero())
			{
				// Transform the point into the box's local space
				const FVector LocalPoint = Rotation.UnrotateVector(Point - Center);

				return FMath::Abs(LocalPoint.X) <= BoxExtent.X &&
				       FMath::Abs(LocalPoint.Y) <= BoxExtent.Y &&
				       FMath::Abs(LocalPoint.Z) <= BoxExtent.Z;
			}
			else
			{
				// No rotation: simple AABB check
				return FMath::Abs(Point.X - Center.X) <= BoxExtent.X &&
				       FMath::Abs(Point.Y - Center.Y) <= BoxExtent.Y &&
				       FMath::Abs(Point.Z - Center.Z) <= BoxExtent.Z;
			}
		}

	case ECellDestructionShapeType::Cylinder:
		{
			// XY distance + Z range check in cylinder local space
			const FVector LocalPoint = Rotation.IsNearlyZero()
				? (Point - Center)
				: Rotation.UnrotateVector(Point - Center);

			const float DistXYSq = FMath::Square(LocalPoint.X) + FMath::Square(LocalPoint.Y);
			return DistXYSq <= Radius * Radius &&
			       FMath::Abs(LocalPoint.Z) <= BoxExtent.Z;
		}

	case ECellDestructionShapeType::Line:
		{
			// Shortest distance to the line segment
			const FVector LineDir = EndPoint - Center;
			const float LineLength = LineDir.Size();
			if (LineLength < KINDA_SMALL_NUMBER)
			{
				return false;
			}

			const FVector LineDirNorm = LineDir / LineLength;
			const FVector ToPoint = Point - Center;
			const float Projection = FVector::DotProduct(ToPoint, LineDirNorm);

			// Check segment bounds
			if (Projection < 0.0f || Projection > LineLength)
			{
				return false;
			}

			// Distance check
			const FVector ClosestPoint = Center + LineDirNorm * Projection;
			return FVector::Dist(Point, ClosestPoint) <= LineThickness;
		}
	}

	return false;
}


FCellDestructionShape FCellDestructionShape::CreateFromRequest(const FRealtimeDestructionRequest& Request)
{
	FCellDestructionShape Shape;
	Shape.Center = Request.ImpactPoint;
	Shape.Radius = Request.ShapeParams.Radius;

	switch (Request.ToolShape)
	{
	case EDestructionToolShape::Sphere:
		Shape.Type = ECellDestructionShapeType::Sphere;
		break;

	case EDestructionToolShape::Cylinder:
		Shape.Type = ECellDestructionShapeType::Line;
		Shape.EndPoint = Request.ImpactPoint + Request.ToolForwardVector * Request.ShapeParams.Height;
		Shape.LineThickness = Request.ShapeParams.Radius;
		break;

	default:
		Shape.Type = ECellDestructionShapeType::Sphere;
		break;
	}

	return Shape;
}

//=============================================================================
// FQuantizedDestructionInput
//=============================================================================

FQuantizedDestructionInput FQuantizedDestructionInput::FromDestructionShape(const FCellDestructionShape& Shape)
{
	FQuantizedDestructionInput Result;
	Result.Type = Shape.Type;

	// Convert cm to mm (quantize)
	Result.CenterMM = FIntVector(
		FMath::RoundToInt(Shape.Center.X * 10.0f),
		FMath::RoundToInt(Shape.Center.Y * 10.0f),
		FMath::RoundToInt(Shape.Center.Z * 10.0f)
	);

	Result.RadiusMM = FMath::RoundToInt(Shape.Radius * 10.0f);

	Result.BoxExtentMM = FIntVector(
		FMath::RoundToInt(Shape.BoxExtent.X * 10.0f),
		FMath::RoundToInt(Shape.BoxExtent.Y * 10.0f),
		FMath::RoundToInt(Shape.BoxExtent.Z * 10.0f)
	);

	// Convert angles to 0.01-degree units
	Result.RotationCentidegrees = FIntVector(
		FMath::RoundToInt(Shape.Rotation.Pitch * 100.0f),
		FMath::RoundToInt(Shape.Rotation.Yaw * 100.0f),
		FMath::RoundToInt(Shape.Rotation.Roll * 100.0f)
	);

	Result.EndPointMM = FIntVector(
		FMath::RoundToInt(Shape.EndPoint.X * 10.0f),
		FMath::RoundToInt(Shape.EndPoint.Y * 10.0f),
		FMath::RoundToInt(Shape.EndPoint.Z * 10.0f)
	);

	Result.LineThicknessMM = FMath::RoundToInt(Shape.LineThickness * 10.0f);

	return Result;
}

FCellDestructionShape FQuantizedDestructionInput::ToDestructionShape() const
{
	FCellDestructionShape Result;
	Result.Type = Type;

	// Convert mm to cm
	Result.Center = FVector(
		CenterMM.X * 0.1f,
		CenterMM.Y * 0.1f,
		CenterMM.Z * 0.1f
	);

	Result.Radius = RadiusMM * 0.1f;

	Result.BoxExtent = FVector(
		BoxExtentMM.X * 0.1f,
		BoxExtentMM.Y * 0.1f,
		BoxExtentMM.Z * 0.1f
	);

	Result.Rotation = FRotator(
		RotationCentidegrees.X * 0.01f,
		RotationCentidegrees.Y * 0.01f,
		RotationCentidegrees.Z * 0.01f
	);

	Result.EndPoint = FVector(
		EndPointMM.X * 0.1f,
		EndPointMM.Y * 0.1f,
		EndPointMM.Z * 0.1f
	);

	Result.LineThickness = LineThicknessMM * 0.1f;

	return Result;
}

bool FQuantizedDestructionInput::ContainsPoint(const FVector& Point) const
{
	// Evaluate using quantized values
	const FVector Center = FVector(CenterMM.X, CenterMM.Y, CenterMM.Z) * 0.1f;
	const float RadiusCm = RadiusMM * 0.1f;
	const FVector BoxExtentCm = FVector(BoxExtentMM.X, BoxExtentMM.Y, BoxExtentMM.Z) * 0.1f;

	switch (Type)
	{
	case ECellDestructionShapeType::Sphere:
		return FVector::DistSquared(Point, Center) <= RadiusCm * RadiusCm;

	case ECellDestructionShapeType::Box:
		{
			if (RotationCentidegrees != FIntVector::ZeroValue)
			{
				const FRotator Rot(
					RotationCentidegrees.X * 0.01f,
					RotationCentidegrees.Y * 0.01f,
					RotationCentidegrees.Z * 0.01f
				);
				const FVector LocalPoint = Rot.UnrotateVector(Point - Center);

				return FMath::Abs(LocalPoint.X) <= BoxExtentCm.X &&
				       FMath::Abs(LocalPoint.Y) <= BoxExtentCm.Y &&
				       FMath::Abs(LocalPoint.Z) <= BoxExtentCm.Z;
			}
			else
			{
				return FMath::Abs(Point.X - Center.X) <= BoxExtentCm.X &&
				       FMath::Abs(Point.Y - Center.Y) <= BoxExtentCm.Y &&
				       FMath::Abs(Point.Z - Center.Z) <= BoxExtentCm.Z;
			}
		}

	case ECellDestructionShapeType::Cylinder:
		{
			const FRotator Rot(
				RotationCentidegrees.X * 0.01f,
				RotationCentidegrees.Y * 0.01f,
				RotationCentidegrees.Z * 0.01f
			);
			const FVector LocalPoint = Rot.IsNearlyZero()
				? (Point - Center)
				: Rot.UnrotateVector(Point - Center);

			const float DistXYSq = FMath::Square(LocalPoint.X) + FMath::Square(LocalPoint.Y);

			return DistXYSq <= RadiusCm * RadiusCm &&
			       FMath::Abs(LocalPoint.Z) <= BoxExtentCm.Z;
		}

	case ECellDestructionShapeType::Line:
		{
			const FVector EndPt = FVector(EndPointMM.X, EndPointMM.Y, EndPointMM.Z) * 0.1f;
			const float ThicknessCm = LineThicknessMM * 0.1f;

			const FVector LineDir = EndPt - Center;
			const float LineLength = LineDir.Size();
			if (LineLength < KINDA_SMALL_NUMBER)
			{
				return false;
			}

			const FVector LineDirNorm = LineDir / LineLength;
			const FVector ToPoint = Point - Center;
			const float Projection = FVector::DotProduct(ToPoint, LineDirNorm);

			if (Projection < 0.0f || Projection > LineLength)
			{
				return false;
			}

			const FVector ClosestPoint = Center + LineDirNorm * Projection;
			return FVector::Dist(Point, ClosestPoint) <= ThicknessCm;
		}
	}

	return false;
}

bool FQuantizedDestructionInput::IntersectsOBB(const FCellOBB& OBB) const
{
	// Convert quantized values to cm
	const FVector Center = FVector(CenterMM.X, CenterMM.Y, CenterMM.Z) * 0.1f;
	const float RadiusCm = RadiusMM * 0.1f;
	const FVector BoxExtentCm = FVector(BoxExtentMM.X, BoxExtentMM.Y, BoxExtentMM.Z) * 0.1f;

	switch (Type)
	{
	case ECellDestructionShapeType::Sphere:
		{
			// Sphere-OBB intersection: check if the closest point on the OBB is inside the sphere
			const FVector ClosestPoint = OBB.GetClosestPoint(Center);
			return FVector::DistSquared(ClosestPoint, Center) <= RadiusCm * RadiusCm;
		}

	case ECellDestructionShapeType::Box:
		{
			// OBB vs OBB intersection using SAT (Separating Axis Theorem)
			// 15-axis test: 3 axes per box + 9 edge cross axes

			// Compute shape rotation and axes
			FQuat ShapeQuat = FQuat::Identity;
			if (RotationCentidegrees != FIntVector::ZeroValue)
			{
				const FRotator ShapeRot(
					RotationCentidegrees.X * 0.01f,
					RotationCentidegrees.Y * 0.01f,
					RotationCentidegrees.Z * 0.01f
				);
				ShapeQuat = ShapeRot.Quaternion();
			}

			const FVector ShapeAxes[3] = {
				ShapeQuat.RotateVector(FVector::ForwardVector),
				ShapeQuat.RotateVector(FVector::RightVector),
				ShapeQuat.RotateVector(FVector::UpVector)
			};

			const FVector OBBAxes[3] = { OBB.AxisX, OBB.AxisY, OBB.AxisZ };

			// Vector between box centers
			const FVector D = OBB.Center - Center;

			// Separating axis test over 15 axes
			auto TestAxis = [&](const FVector& Axis) -> bool
			{
				if (Axis.SizeSquared() < KINDA_SMALL_NUMBER)
				{
					return true; // If axis is too small, treat as not separable
				}

				const FVector NormAxis = Axis.GetSafeNormal();

				// Projection radius of the shape box
				float ShapeProjection = 0.0f;
				ShapeProjection += FMath::Abs(FVector::DotProduct(ShapeAxes[0], NormAxis)) * BoxExtentCm.X;
				ShapeProjection += FMath::Abs(FVector::DotProduct(ShapeAxes[1], NormAxis)) * BoxExtentCm.Y;
				ShapeProjection += FMath::Abs(FVector::DotProduct(ShapeAxes[2], NormAxis)) * BoxExtentCm.Z;

				// Projection radius of the OBB
				float OBBProjection = 0.0f;
				OBBProjection += FMath::Abs(FVector::DotProduct(OBBAxes[0], NormAxis)) * OBB.HalfExtents.X;
				OBBProjection += FMath::Abs(FVector::DotProduct(OBBAxes[1], NormAxis)) * OBB.HalfExtents.Y;
				OBBProjection += FMath::Abs(FVector::DotProduct(OBBAxes[2], NormAxis)) * OBB.HalfExtents.Z;

				// Projection of center distance
				const float CenterDistance = FMath::Abs(FVector::DotProduct(D, NormAxis));

				// Separating axis test: if center distance > sum of radii, separated
				return CenterDistance <= ShapeProjection + OBBProjection;
			};

			// Test shape box axes
			for (int32 i = 0; i < 3; ++i)
			{
				if (!TestAxis(ShapeAxes[i]))
				{
					return false;
				}
			}

			// Test OBB axes
			for (int32 i = 0; i < 3; ++i)
			{
				if (!TestAxis(OBBAxes[i]))
				{
					return false;
				}
			}

			// Test 9 edge cross axes (ShapeAxis x OBBAxis)
			for (int32 i = 0; i < 3; ++i)
			{
				for (int32 j = 0; j < 3; ++j)
				{
					const FVector CrossAxis = FVector::CrossProduct(ShapeAxes[i], OBBAxes[j]);
					if (!TestAxis(CrossAxis))
					{
						return false;
					}
				}
			}

			// No separating axis found = intersect
			return true;
		}

	case ECellDestructionShapeType::Cylinder:
		{
			// Cylinder-OBB intersection in cylinder local space
			FQuat ShapeQuat = FQuat::Identity;
			if (RotationCentidegrees != FIntVector::ZeroValue)
			{
				const FRotator ShapeRot(
					RotationCentidegrees.X * 0.01f,
					RotationCentidegrees.Y * 0.01f,
					RotationCentidegrees.Z * 0.01f
				);
				ShapeQuat = ShapeRot.Quaternion();
			}

			const FQuat InvQuat = ShapeQuat.Inverse();
			const FVector LocalCenter = InvQuat.RotateVector(OBB.Center - Center);
			const FVector LocalAxisX = InvQuat.RotateVector(OBB.AxisX);
			const FVector LocalAxisY = InvQuat.RotateVector(OBB.AxisY);
			const FVector LocalAxisZ = InvQuat.RotateVector(OBB.AxisZ);

			// Z range check: local OBB Z projection vs cylinder Z range
			float OBBMinZ = FLT_MAX, OBBMaxZ = -FLT_MAX;
			for (int32 i = 0; i < 8; ++i)
			{
				const FVector LocalCorner(
					((i & 1) ? OBB.HalfExtents.X : -OBB.HalfExtents.X),
					((i & 2) ? OBB.HalfExtents.Y : -OBB.HalfExtents.Y),
					((i & 4) ? OBB.HalfExtents.Z : -OBB.HalfExtents.Z)
				);
				const FVector WorldCorner = LocalCenter
					+ LocalAxisX * LocalCorner.X
					+ LocalAxisY * LocalCorner.Y
					+ LocalAxisZ * LocalCorner.Z;
				OBBMinZ = FMath::Min(OBBMinZ, WorldCorner.Z);
				OBBMaxZ = FMath::Max(OBBMaxZ, WorldCorner.Z);
			}

			if (OBBMaxZ < -BoxExtentCm.Z || OBBMinZ > BoxExtentCm.Z)
			{
				return false;
			}

			// XY plane circle vs OBB projection in local space
			float MinDistSq = FLT_MAX;
			for (int32 i = 0; i < 8; ++i)
			{
				const FVector LocalCorner(
					((i & 1) ? OBB.HalfExtents.X : -OBB.HalfExtents.X),
					((i & 2) ? OBB.HalfExtents.Y : -OBB.HalfExtents.Y),
					((i & 4) ? OBB.HalfExtents.Z : -OBB.HalfExtents.Z)
				);
				const FVector WorldCorner = LocalCenter
					+ LocalAxisX * LocalCorner.X
					+ LocalAxisY * LocalCorner.Y
					+ LocalAxisZ * LocalCorner.Z;
				const float DistSq = FMath::Square(WorldCorner.X) + FMath::Square(WorldCorner.Y);
				MinDistSq = FMath::Min(MinDistSq, DistSq);
			}

			if (MinDistSq <= RadiusCm * RadiusCm)
			{
				return true;
			}

			const float CenterDistSq = FMath::Square(LocalCenter.X) + FMath::Square(LocalCenter.Y);
			if (CenterDistSq <= RadiusCm * RadiusCm)
			{
				return true;
			}

			// Conservative approximation: compare against projected OBB radius
			const float OBBRadiusXY = FMath::Sqrt(
				FMath::Square(OBB.HalfExtents.X * LocalAxisX.X + OBB.HalfExtents.Y * LocalAxisY.X) +
				FMath::Square(OBB.HalfExtents.X * LocalAxisX.Y + OBB.HalfExtents.Y * LocalAxisY.Y)
			) + FMath::Sqrt(
				FMath::Square(OBB.HalfExtents.Z * LocalAxisZ.X) +
				FMath::Square(OBB.HalfExtents.Z * LocalAxisZ.Y)
			);

			return CenterDistSq <= FMath::Square(RadiusCm + OBBRadiusXY);
		}

	case ECellDestructionShapeType::Line:
		{
 
		// Convert to cm
		const FVector EndPt = FVector(EndPointMM.X, EndPointMM.Y, EndPointMM.Z) * 0.1f;
		const float ThicknessCm = LineThicknessMM * 0.1f;

		// First-pass filter
		FCellOBB TestOBB = OBB;
		TestOBB.HalfExtents = OBB.HalfExtents + FVector(ThicknessCm);

		// Transform world coordinates to local
		const FVector LocalStart = TestOBB.WorldToLocal(Center);
		const FVector LocalEnd = TestOBB.WorldToLocal(EndPt);
		const FVector LocalDir = LocalEnd - LocalStart; 

		// Before the slab test, check distance to the center line and reject if outside the radius
		 
		// Pad by the subcell size for a looser bound
		const float SubCellRadius = OBB.HalfExtents.Size();
		const float HitRadius = ThicknessCm + SubCellRadius;

		const float DistToCenterSq = FMath::PointDistToSegmentSquared(FVector::ZeroVector, LocalStart, LocalEnd);
		if (DistToCenterSq > HitRadius * HitRadius)
		{
			return false; // Outside circular range
		}
		 
		// Original logic
		float tMin = 0.0f;
		float tMax = 1.0f;

		// Compute slab intersection for each axis
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			float Start, Dir, Extent;
			switch (Axis)
			{
			case 0: Start = LocalStart.X; Dir = LocalDir.X; Extent = TestOBB.HalfExtents.X; break;
			case 1: Start = LocalStart.Y; Dir = LocalDir.Y; Extent = TestOBB.HalfExtents.Y; break;
			case 2: Start = LocalStart.Z; Dir = LocalDir.Z; Extent = TestOBB.HalfExtents.Z; break;
			default: continue;
			}

			if (FMath::Abs(Dir) < KINDA_SMALL_NUMBER)
			{
				// Ray is parallel to slab
				if (Start < -Extent || Start > Extent)
				{
					return false; // Parallel outside slab = no intersection
				}
			}
			else
			{
				float t1 = (-Extent - Start) / Dir;
				float t2 = (Extent - Start) / Dir;

				if (t1 > t2) Swap(t1, t2);

				tMin = FMath::Max(tMin, t1);
				tMax = FMath::Min(tMax, t2);

				if (tMin > tMax)
				{
					return false; // No intersection interval
				}
			}
		}

		return true; // Passes both slab (length) and distance (thickness)
		}
	}

	return false;
}

//=============================================================================
// FGridCellLayout
//=============================================================================

int32 FGridCellLayout::GetAnchorCount() const
{
	int32 Count = 0;
	// Iterate valid cells only (sparse array)
	for (int32 CellId : SparseIndexToCellId)
	{
		if (GetCellIsAnchor(CellId))
		{
			Count++;
		}
	}
	return Count;
}

int32 FGridCellLayout::WorldPosToId(const FVector& WorldPos, const FTransform& MeshTransform) const
{
	// Transform world coordinates to local
	const FVector LocalPos = MeshTransform.InverseTransformPosition(WorldPos);

	// Compute grid coordinates
	const int32 X = FMath::FloorToInt((LocalPos.X - GridOrigin.X) / CellSize.X);
	const int32 Y = FMath::FloorToInt((LocalPos.Y - GridOrigin.Y) / CellSize.Y);
	const int32 Z = FMath::FloorToInt((LocalPos.Z - GridOrigin.Z) / CellSize.Z);

	// Range check
	if (X < 0 || X >= GridSize.X ||
	    Y < 0 || Y >= GridSize.Y ||
	    Z < 0 || Z >= GridSize.Z)
	{
		return INDEX_NONE;
	}

	return CoordToId(X, Y, Z);
}

FVector FGridCellLayout::IdToWorldCenter(int32 CellId, const FTransform& MeshTransform) const
{
	const FVector LocalCenter = IdToLocalCenter(CellId);
	return MeshTransform.TransformPosition(LocalCenter);
}

FVector FGridCellLayout::IdToLocalCenter(int32 CellId) const
{
	if (!IsValidCellId(CellId))
	{
		return FVector::ZeroVector;
	}

	const FIntVector Coord = IdToCoord(CellId);
	return FVector(
		GridOrigin.X + (Coord.X + 0.5f) * CellSize.X,
		GridOrigin.Y + (Coord.Y + 0.5f) * CellSize.Y,
		GridOrigin.Z + (Coord.Z + 0.5f) * CellSize.Z
	);
}

FVector FGridCellLayout::IdToWorldMin(int32 CellId, const FTransform& MeshTransform) const
{
	const FVector LocalMin = IdToLocalMin(CellId);
	return MeshTransform.TransformPosition(LocalMin);
}

FVector FGridCellLayout::IdToLocalMin(int32 CellId) const
{
	if (!IsValidCellId(CellId))
	{
		return FVector::ZeroVector;
	}

	const FIntVector Coord = IdToCoord(CellId);
	return FVector(
		GridOrigin.X + Coord.X * CellSize.X,
		GridOrigin.Y + Coord.Y * CellSize.Y,
		GridOrigin.Z + Coord.Z * CellSize.Z
	);
}

TArray<FVector> FGridCellLayout::GetCellVertices(int32 CellId) const
{
	TArray<FVector> Vertices;
	Vertices.Reserve(8);

	const FVector Min = IdToLocalMin(CellId);

	// 8 vertices (bit ops choose axis offsets)
	for (int32 i = 0; i < 8; i++)
	{
		Vertices.Add(FVector(
			Min.X + ((i & 1) ? CellSize.X : 0.0f),
			Min.Y + ((i & 2) ? CellSize.Y : 0.0f),
			Min.Z + ((i & 4) ? CellSize.Z : 0.0f)
		));
	}

	return Vertices;
}

void FGridCellLayout::Reset()
{
	GridSize = FIntVector::ZeroValue;
	GridOrigin = FVector::ZeroVector;
	MeshScale = FVector::OneVector;

	// Initialize bitfields
	CellExistsBits.Empty();
	CellIsAnchorBits.Empty();

	// Initialize sparse arrays
	CellIdToSparseIndex.Empty();
	SparseIndexToCellId.Empty();
	SparseCellTriangles.Empty();
	SparseCellNeighbors.Empty();

	// Note: Do NOT clear CachedVertices/CachedIndices here
	// They need to persist for runtime rebuilds
}

bool FGridCellLayout::IsValid() const
{
	if (GridSize.X <= 0 || GridSize.Y <= 0 || GridSize.Z <= 0)
	{
		return false;
	}

	const int32 TotalCells = GetTotalCellCount();
	const int32 RequiredWords = (TotalCells + 31) >> 5;  // ceil(TotalCells / 32)

	// Validate bitfield size
	if (CellExistsBits.Num() != RequiredWords || CellIsAnchorBits.Num() != RequiredWords)
	{
		return false;
	}

	// Validate sparse array consistency
	const int32 ValidCellCount = SparseIndexToCellId.Num();
	return SparseCellTriangles.Num() == ValidCellCount &&
	       SparseCellNeighbors.Num() == ValidCellCount &&
	       CellIdToSparseIndex.Num() == ValidCellCount;
}

TArray<int32> FGridCellLayout::GetCellsInAABB(const FBox& WorldAABB, const FTransform& MeshTransform) const
{
	TArray<int32> Result;

	if (!IsValid())
	{
		return Result;
	}

	// Convert 8 world AABB corners to local space to build a local AABB
	FBox LocalAABB(ForceInit);

	const FVector WorldCorners[8] = {
		FVector(WorldAABB.Min.X, WorldAABB.Min.Y, WorldAABB.Min.Z),
		FVector(WorldAABB.Max.X, WorldAABB.Min.Y, WorldAABB.Min.Z),
		FVector(WorldAABB.Min.X, WorldAABB.Max.Y, WorldAABB.Min.Z),
		FVector(WorldAABB.Max.X, WorldAABB.Max.Y, WorldAABB.Min.Z),
		FVector(WorldAABB.Min.X, WorldAABB.Min.Y, WorldAABB.Max.Z),
		FVector(WorldAABB.Max.X, WorldAABB.Min.Y, WorldAABB.Max.Z),
		FVector(WorldAABB.Min.X, WorldAABB.Max.Y, WorldAABB.Max.Z),
		FVector(WorldAABB.Max.X, WorldAABB.Max.Y, WorldAABB.Max.Z),
	};

	for (int32 i = 0; i < 8; ++i)
	{
		LocalAABB += MeshTransform.InverseTransformPosition(WorldCorners[i]);
	}

	// Convert local AABB to grid coordinate range
	const int32 MinX = FMath::Max(0, FMath::FloorToInt((LocalAABB.Min.X - GridOrigin.X) / CellSize.X));
	const int32 MinY = FMath::Max(0, FMath::FloorToInt((LocalAABB.Min.Y - GridOrigin.Y) / CellSize.Y));
	const int32 MinZ = FMath::Max(0, FMath::FloorToInt((LocalAABB.Min.Z - GridOrigin.Z) / CellSize.Z));

	const int32 MaxX = FMath::Min(GridSize.X - 1, FMath::FloorToInt((LocalAABB.Max.X - GridOrigin.X) / CellSize.X));
	const int32 MaxY = FMath::Min(GridSize.Y - 1, FMath::FloorToInt((LocalAABB.Max.Y - GridOrigin.Y) / CellSize.Y));
	const int32 MaxZ = FMath::Min(GridSize.Z - 1, FMath::FloorToInt((LocalAABB.Max.Z - GridOrigin.Z) / CellSize.Z));

	// Gather all cells in range
	Result.Reserve((MaxX - MinX + 1) * (MaxY - MinY + 1) * (MaxZ - MinZ + 1));

	for (int32 Z = MinZ; Z <= MaxZ; ++Z)
	{
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const int32 CellId = CoordToId(X, Y, Z);
				if (GetCellExists(CellId))
				{
					Result.Add(CellId);
				}
			}
		}
	}

	return Result;
}

//=============================================================================
// FSuperCellState
//=============================================================================

bool FSuperCellState::IsCellOnSupercellBoundary(const FIntVector& CellCoord, const FIntVector& SupercellCoord) const
{
	// Cell local coordinates within a SuperCell
	const int32 LocalX = CellCoord.X - SupercellCoord.X * SupercellSize.X;
	const int32 LocalY = CellCoord.Y - SupercellCoord.Y * SupercellSize.Y;
	const int32 LocalZ = CellCoord.Z - SupercellCoord.Z * SupercellSize.Z;

	// Boundary check: local coord is 0 or (Size - 1)
	return LocalX == 0 || LocalX == SupercellSize.X - 1 ||
	       LocalY == 0 || LocalY == SupercellSize.Y - 1 ||
	       LocalZ == 0 || LocalZ == SupercellSize.Z - 1;
}

void FSuperCellState::GetCellsInSupercell(int32 SupercellId, const FGridCellLayout& GridCache, TArray<int32>& OutCellIds) const
{
	OutCellIds.Reset();

	if (!IsValidSupercellId(SupercellId))
	{
		return;
	}

	const FIntVector SupercellCoord = SupercellIdToCoord(SupercellId);

	// Compute SuperCell cell coordinate range
	const int32 StartX = SupercellCoord.X * SupercellSize.X;
	const int32 StartY = SupercellCoord.Y * SupercellSize.Y;
	const int32 StartZ = SupercellCoord.Z * SupercellSize.Z;

	const int32 EndX = FMath::Min(StartX + SupercellSize.X, GridCache.GridSize.X);
	const int32 EndY = FMath::Min(StartY + SupercellSize.Y, GridCache.GridSize.Y);
	const int32 EndZ = FMath::Min(StartZ + SupercellSize.Z, GridCache.GridSize.Z);

	OutCellIds.Reserve((EndX - StartX) * (EndY - StartY) * (EndZ - StartZ));

	for (int32 Z = StartZ; Z < EndZ; ++Z)
	{
		for (int32 Y = StartY; Y < EndY; ++Y)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellId = GridCache.CoordToId(X, Y, Z);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
	}
}

void FSuperCellState::GetBoundaryCellsOfSupercell(int32 SupercellId, const FGridCellLayout& GridCache, TArray<int32>& OutCellIds) const
{
	OutCellIds.Reset();

	if (!IsValidSupercellId(SupercellId))
	{
		return;
	}

	const FIntVector SupercellCoord = SupercellIdToCoord(SupercellId);

	// Compute SuperCell cell coordinate range
	const int32 StartX = SupercellCoord.X * SupercellSize.X;
	const int32 StartY = SupercellCoord.Y * SupercellSize.Y;
	const int32 StartZ = SupercellCoord.Z * SupercellSize.Z;

	const int32 EndX = FMath::Min(StartX + SupercellSize.X, GridCache.GridSize.X);
	const int32 EndY = FMath::Min(StartY + SupercellSize.Y, GridCache.GridSize.Y);
	const int32 EndZ = FMath::Min(StartZ + SupercellSize.Z, GridCache.GridSize.Z);

	// Collect boundary cells only (6 faces)
	TSet<int32> BoundaryCellSet;

	for (int32 Z = StartZ; Z < EndZ; ++Z)
	{
		for (int32 Y = StartY; Y < EndY; ++Y)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				// Boundary check
				const bool bOnBoundary = (X == StartX || X == EndX - 1 ||
				                          Y == StartY || Y == EndY - 1 ||
				                          Z == StartZ || Z == EndZ - 1);

				if (bOnBoundary)
				{
					const int32 CellId = GridCache.CoordToId(X, Y, Z);
					if (GridCache.GetCellExists(CellId))
					{
						BoundaryCellSet.Add(CellId);
					}
				}
			}
		}
	}

	OutCellIds = BoundaryCellSet.Array();
}

void FSuperCellState::BuildFromGridLayout(const FGridCellLayout& GridCache)
{
	Reset();

	if (!GridCache.IsValid())
	{
		return;
	}

	// Compute SuperCellSize: min(GridSize, 8)
	SupercellSize = FIntVector(8, 8, 8);

	// A SuperCell requires a full SupercellSize fill along each axis
	// Example: GridSize.X = 5, SupercellSize.X = 4 -> SupercellCount.X = 1 (1 leftover is orphan)
	// even if we don't fill all the supercells, we will still make them supercells, but intact will be false.
	SupercellCount = FIntVector(
		(GridCache.GridSize.X  + SupercellSize.X - 1) / SupercellSize.X,
		(GridCache.GridSize.Y  + SupercellSize.Y - 1) / SupercellSize.Y,
		(GridCache.GridSize.Z  + SupercellSize.Z - 1) / SupercellSize.Z
	);

	//SupercellCount = FIntVector(
	//	(GridCache.GridSize.X) / SupercellSize.X,
	//	(GridCache.GridSize.Y) / SupercellSize.Y,
	//	(GridCache.GridSize.Z) / SupercellSize.Z
	//);


	// Initialize Cell -> SuperCell mapping
	const int32 TotalCells = GridCache.GetTotalCellCount();
	CellToSupercell.SetNum(TotalCells);

	// Initialize all cells to INDEX_NONE (orphan)
	for (int32 i = 0; i < TotalCells; ++i)
	{
		CellToSupercell[i] = INDEX_NONE;
	}


	// Initialize intact bitfield (all SuperCells intact)
	InitializeIntactBits();

	InitialValidCellCounts.SetNumZeroed(SupercellCount.X * SupercellCount.Y * SupercellCount.Z);
	DestroyedCellCounts.SetNumZeroed(SupercellCount.X * SupercellCount.Y * SupercellCount.Z);

	const int32 RequiredCellCount = SupercellSize.X * SupercellSize.Y * SupercellSize.Z;
	// Map cells that belong to SuperCells
	for (int32 SCZ = 0; SCZ < SupercellCount.Z; ++SCZ)
	{
		for (int32 SCY = 0; SCY < SupercellCount.Y; ++SCY)
		{
			for (int32 SCX = 0; SCX < SupercellCount.X; ++SCX)
			{

				const int32 SupercellId = SupercellCoordToId(SCX, SCY, SCZ);

				const int32 StartX = SCX * SupercellSize.X;
				const int32 StartY = SCY * SupercellSize.Y;
				const int32 StartZ = SCZ * SupercellSize.Z;

				// Step 1: count valid cells
				int32 ValidCount = 0;
				for (int32 LZ = 0; LZ < SupercellSize.Z; ++LZ)
				{
					for (int32 LY = 0; LY < SupercellSize.Y; ++LY)
					{
						for (int32 LX = 0; LX < SupercellSize.X; ++LX)
						{
							const int32 GX = StartX + LX;
							const int32 GY = StartY + LY;
							const int32 GZ = StartZ + LZ;

							// 범위 체크 추가
							if (GX >= GridCache.GridSize.X || GY >= GridCache.GridSize.Y || GZ >= GridCache.GridSize.Z)
							{
								continue;
							}

							const int32 CellId = GridCache.CoordToId(GX, GY, GZ);
							if (GridCache.GetCellExists(CellId))
							{
								ValidCount++;
							}
						}
					}
				}

				// Step 2: create SuperCell only if all 512 are present
				if (ValidCount == RequiredCellCount)
				{
					for (int32 LZ = 0; LZ < SupercellSize.Z; ++LZ)
					{
						for (int32 LY = 0; LY < SupercellSize.Y; ++LY)
						{
							for (int32 LX = 0; LX < SupercellSize.X; ++LX)
							{
								const int32 CellId = GridCache.CoordToId(StartX + LX, StartY + LY, StartZ + LZ);
								CellToSupercell[CellId] = SupercellId;
								InitialValidCellCounts[SupercellId]++;
							}
						}
					}
				}
				else
				{ 
					// Partially filled SuperCell: Map only valid cells and handle broken ones.
					for (int32 LZ = 0; LZ < SupercellSize.Z; ++LZ)
					{
						for (int32 LY = 0; LY < SupercellSize.Y; ++LY)
						{
							for (int32 LX = 0; LX < SupercellSize.X; ++LX)
							{
								const int32 GX = StartX + LX;
								const int32 GY = StartY + LY;
								const int32 GZ = StartZ + LZ;
								// Grid range check (may exceed due to ceiling division)
								if (GX >= GridCache.GridSize.X || GY >= GridCache.GridSize.Y || GZ >= GridCache.GridSize.Z)
								{
									continue;
								}
								const int32 CellId = GridCache.CoordToId(GX, GY, GZ);
								if (GridCache.GetCellExists(CellId))
								{
									CellToSupercell[CellId] = SupercellId;
									InitialValidCellCounts[SupercellId]++;
								}
							}
						}
					}

					MarkSupercellBroken(SupercellId);
				}
			}
		}
	}

	// Build orphan cell list (valid cells not belonging to any SuperCell)
	OrphanCellIds.Reset();
	for (int32 CellId : GridCache.GetValidCellIds())
	{
		if (CellToSupercell[CellId] == INDEX_NONE)
		{
			OrphanCellIds.Add(CellId);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("FSuperCellState::BuildFromGridLayout - GridSize: (%d, %d, %d), SupercellSize: (%d, %d, %d), SupercellCount: (%d, %d, %d), TotalSupercells: %d, OrphanCells: %d"),
		GridCache.GridSize.X, GridCache.GridSize.Y, GridCache.GridSize.Z,
		SupercellSize.X, SupercellSize.Y, SupercellSize.Z,
		SupercellCount.X, SupercellCount.Y, SupercellCount.Z,
		GetTotalSupercellCount(),
		OrphanCellIds.Num());
}

void FSuperCellState::InitializeIntactBits()
{
	const int32 TotalSupercells = GetTotalSupercellCount();
	const int32 RequiredWords = (TotalSupercells + 63) >> 6;  // ceil(TotalSupercells / 64)

	// Set all bits to 1 (intact)
	IntactBits.SetNum(RequiredWords);
	for (int32 i = 0; i < RequiredWords; ++i)
	{
		IntactBits[i] = ~0ull;  // All bits set
	}
}

void FSuperCellState::Reset()
{
	SupercellSize = FIntVector(4, 4, 4);
	SupercellCount = FIntVector::ZeroValue;
	IntactBits.Empty();
	CellToSupercell.Empty();
	OrphanCellIds.Empty();
}

bool FSuperCellState::IsValid() const
{
	if (SupercellCount.X <= 0 || SupercellCount.Y <= 0 || SupercellCount.Z <= 0)
	{
		return false;
	}

	const int32 TotalSupercells = GetTotalSupercellCount();
	const int32 RequiredWords = (TotalSupercells + 63) >> 6;

	return IntactBits.Num() == RequiredWords && CellToSupercell.Num() > 0;
}

bool FSuperCellState::IsSupercellTrulyIntact(
	int32 SupercellId,
	const FGridCellLayout& GridCache,
	const FCellState& CellState,
	bool bEnableSubcell) const
{
	if (!IsValidSupercellId(SupercellId))
	{
		return false;
	}

	// Early out if already marked broken
	if (!IsSupercellIntact(SupercellId))
	{
		return false;
	}

	// Check all cells in the SuperCell
	const FIntVector SupercellCoord = SupercellIdToCoord(SupercellId);

	const int32 StartX = SupercellCoord.X * SupercellSize.X;
	const int32 StartY = SupercellCoord.Y * SupercellSize.Y;
	const int32 StartZ = SupercellCoord.Z * SupercellSize.Z;

	const int32 EndX = FMath::Min(StartX + SupercellSize.X, GridCache.GridSize.X);
	const int32 EndY = FMath::Min(StartY + SupercellSize.Y, GridCache.GridSize.Y);
	const int32 EndZ = FMath::Min(StartZ + SupercellSize.Z, GridCache.GridSize.Z);

	for (int32 Z = StartZ; Z < EndZ; ++Z)
	{
		for (int32 Y = StartY; Y < EndY; ++Y)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellId = GridCache.CoordToId(X, Y, Z);

				// Skip if cell does not exist
				if (!GridCache.GetCellExists(CellId))
				{
					continue;
				}

				// Broken if cell is destroyed
				if (CellState.DestroyedCells.Contains(CellId))
				{
					return false;
				}

				// Check subcell state only in subcell mode
				if (bEnableSubcell)
				{
					const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);
					if (SubCellState)
					{
						// 0xFF = all subcells alive (all 8 bits set)
						if (SubCellState->Bits != 0xFF)
						{
							return false;
						}
					}
				}
			}
		}
	}

	return true;
}

void FSuperCellState::UpdateSupercellStates(const TArray<int32>& AffectedCellIds)
{
	for (int32 CellId : AffectedCellIds)
	{
		const int32 SupercellId = GetSupercellForCell(CellId);
		if (SupercellId != INDEX_NONE)
		{
			MarkSupercellBroken(SupercellId);
		}
	}
}

void FSuperCellState::OnCellDestroyed(int32 CellId)
{
	const int32 SupercellId = GetSupercellForCell(CellId);
	if (SupercellId != INDEX_NONE)
	{
		MarkSupercellBroken(SupercellId);
	}
}

void FSuperCellState::OnSubCellDestroyed(int32 CellId, int32 SubCellId)
{
	const int32 SupercellId = GetSupercellForCell(CellId);
	if (SupercellId != INDEX_NONE)
	{
		MarkSupercellBroken(SupercellId);
	}
}

void FSuperCellState::GetBoundaryCellsInDirection(
	int32 SupercellId,
	int32 Direction,
	const FGridCellLayout& GridCache,
	TArray<int32>& OutCellIds) const
{
	OutCellIds.Reset();

	if (!IsValidSupercellId(SupercellId) || Direction < 0 || Direction >= 6)
	{
		return;
	}

	const FIntVector SupercellCoord = SupercellIdToCoord(SupercellId);

	// Compute SuperCell cell coordinate range
	const int32 StartX = SupercellCoord.X * SupercellSize.X;
	const int32 StartY = SupercellCoord.Y * SupercellSize.Y;
	const int32 StartZ = SupercellCoord.Z * SupercellSize.Z;

	const int32 EndX = FMath::Min(StartX + SupercellSize.X, GridCache.GridSize.X);
	const int32 EndY = FMath::Min(StartY + SupercellSize.Y, GridCache.GridSize.Y);
	const int32 EndZ = FMath::Min(StartZ + SupercellSize.Z, GridCache.GridSize.Z);

	// Extract boundary cells for each direction
	switch (Direction)
	{
	case 0: // -X
		for (int32 Z = StartZ; Z < EndZ; ++Z)
		{
			for (int32 Y = StartY; Y < EndY; ++Y)
			{
				const int32 CellId = GridCache.CoordToId(StartX, Y, Z);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
		break;

	case 1: // +X
		for (int32 Z = StartZ; Z < EndZ; ++Z)
		{
			for (int32 Y = StartY; Y < EndY; ++Y)
			{
				const int32 CellId = GridCache.CoordToId(EndX - 1, Y, Z);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
		break;

	case 2: // -Y
		for (int32 Z = StartZ; Z < EndZ; ++Z)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellId = GridCache.CoordToId(X, StartY, Z);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
		break;

	case 3: // +Y
		for (int32 Z = StartZ; Z < EndZ; ++Z)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellId = GridCache.CoordToId(X, EndY - 1, Z);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
		break;

	case 4: // -Z
		for (int32 Y = StartY; Y < EndY; ++Y)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellId = GridCache.CoordToId(X, Y, StartZ);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
		break;

	case 5: // +Z
		for (int32 Y = StartY; Y < EndY; ++Y)
		{
			for (int32 X = StartX; X < EndX; ++X)
			{
				const int32 CellId = GridCache.CoordToId(X, Y, EndZ - 1);
				if (GridCache.GetCellExists(CellId))
				{
					OutCellIds.Add(CellId);
				}
			}
		}
		break;
	}
} 
