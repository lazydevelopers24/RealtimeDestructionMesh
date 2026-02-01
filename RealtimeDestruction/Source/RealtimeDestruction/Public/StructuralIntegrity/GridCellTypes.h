// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "GeometryCollection/GeometryCollectionParticlesData.h"
#include "GridCellTypes.generated.h"


// Forward declaration
struct FRealtimeDestructionRequest;

//=========================================================================
// SubCell configuration constants
//=========================================================================

/** SubCell divisions per axis - 2x2x2 = 8 subcells */
inline constexpr int32 SUBCELL_DIVISION = 2;

/** Total SubCell count */
inline constexpr int32 SUBCELL_COUNT = SUBCELL_DIVISION * SUBCELL_DIVISION * SUBCELL_DIVISION;  // 8

/** SubCell 3D coord -> SubCell ID */
inline constexpr int32 SubCellCoordToId(int32 X, int32 Y, int32 Z)
{
	return Z * (SUBCELL_DIVISION * SUBCELL_DIVISION) + Y * SUBCELL_DIVISION + X;
}

/** SubCell ID -> 3D coord */
inline FIntVector SubCellIdToCoord(int32 SubCellId)
{
	const int32 XY = SUBCELL_DIVISION * SUBCELL_DIVISION;
	const int32 Z = SubCellId / XY;
	const int32 Remainder = SubCellId % XY;
	const int32 Y = Remainder / SUBCELL_DIVISION;
	const int32 X = Remainder % SUBCELL_DIVISION;
	return FIntVector(X, Y, Z);
}

/** 6-direction offsets (+/-X, +/-Y, +/-Z) */
inline constexpr int32 DIRECTION_OFFSETS[6][3] = {
	{-1, 0, 0},  // -X
	{+1, 0, 0},  // +X
	{0, -1, 0},  // -Y
	{0, +1, 0},  // +Y
	{0, 0, -1},  // -Z
	{0, 0, +1},  // +Z
};

// =======================================================
// Low Level Utility
// =======================================================

USTRUCT(BlueprintType)
struct FIntArray
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> Values;

	void Add(int32 Value) { Values.Add(Value); }
	int32 Num() const { return Values.Num(); }
	void Empty() { Values.Empty(); }
	int32& operator[](int32 Index) { return Values[Index]; }
	const int32& operator[](int32 Index) const { return Values[Index]; }

	// Range-based for loop support
	TArray<int32>::RangedForIteratorType begin() { return Values.begin(); }
	TArray<int32>::RangedForIteratorType end() { return Values.end(); }
	TArray<int32>::RangedForConstIteratorType begin() const { return Values.begin(); }
	TArray<int32>::RangedForConstIteratorType end() const { return Values.end(); }
};

/**
 * Oriented Bounding Box (OBB) for subcells.
 * Represents a rotated box in world space.
 * Note: defined separately to avoid a name clash with UE's FOrientedBox.
 */
struct FCellOBB
{
	/** Box center (world space) */
	FVector Center;

	/** Half extents (local axes) */
	FVector HalfExtents;

	/** Local axes (world space, normalized orthogonal vectors) */
	FVector AxisX;
	FVector AxisY;
	FVector AxisZ;

	FCellOBB()
		: Center(FVector::ZeroVector)
		, HalfExtents(FVector::ZeroVector)
		, AxisX(FVector::ForwardVector)
		, AxisY(FVector::RightVector)
		, AxisZ(FVector::UpVector)
	{}

	FCellOBB(const FVector& InCenter, const FVector& InHalfExtents, const FQuat& Rotation)
		: Center(InCenter)
		, HalfExtents(InHalfExtents)
	{
		AxisX = Rotation.RotateVector(FVector::ForwardVector);
		AxisY = Rotation.RotateVector(FVector::RightVector);
		AxisZ = Rotation.RotateVector(FVector::UpVector);
	}

	/** Transform a point into OBB local space. */
	FVector WorldToLocal(const FVector& WorldPoint) const
	{
		const FVector Delta = WorldPoint - Center;
		return FVector(
			FVector::DotProduct(Delta, AxisX),
			FVector::DotProduct(Delta, AxisY),
			FVector::DotProduct(Delta, AxisZ)
		);
	}

	/** Transform a local OBB point into world space. */
	FVector LocalToWorld(const FVector& LocalPoint) const
	{
		return Center + AxisX * LocalPoint.X + AxisY * LocalPoint.Y + AxisZ * LocalPoint.Z;
	}

	/** Find the closest point on or inside the OBB to a world-space point. */
	FVector GetClosestPoint(const FVector& WorldPoint) const
	{
		const FVector LocalPoint = WorldToLocal(WorldPoint);
		const FVector ClampedLocal(
			FMath::Clamp(LocalPoint.X, -HalfExtents.X, HalfExtents.X),
			FMath::Clamp(LocalPoint.Y, -HalfExtents.Y, HalfExtents.Y),
			FMath::Clamp(LocalPoint.Z, -HalfExtents.Z, HalfExtents.Z)
		);
		return LocalToWorld(ClampedLocal);
	}
	 
};

/**
 * Shape for cell destruction.
 * Do not confuse with mesh tool shape types.
 */
UENUM(BlueprintType)
enum class ECellDestructionShapeType : uint8
{
	Sphere,     // Sphere (explosion)
	Box,        // Box (breaching)
	Cylinder,   // Cylinder
	Line        // Line (bullet)
};

// =======================================================
// Destruction Input & Shape
// =======================================================

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FCellDestructionShape
{
	GENERATED_BODY()

	/** Shape type. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	ECellDestructionShapeType Type = ECellDestructionShapeType::Sphere;

	/**
	 * Center point.
	 * Sphere/Box/Cylinder -> center
	 * Line -> start point
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	FVector Center = FVector::ZeroVector;

	/** Sphere/Cylinder radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	float Radius = 50.0f;

	/**
	 * Box extent (box only).
	 * Cylinder uses the Z value as height.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	FVector BoxExtent = FVector::ZeroVector;

	/** Rotation (box/cylinder). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	FRotator Rotation = FRotator::ZeroRotator;

	/** Line end point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	FVector EndPoint = FVector::ZeroVector;

	/** Line thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DestructionShape")
	float LineThickness = 5.0f;

	/** Check whether a point is inside the destruction shape. */
	bool ContainsPoint(const FVector& Point) const;

	/**
	 * Build a FCellDestructionShape from a FRealtimeDestructionRequest.
	 * Converts ToolShape to Sphere/Line and maps Cylinder to a Line along ToolForwardVector.
	 */
	static FCellDestructionShape CreateFromRequest(const FRealtimeDestructionRequest& Request);
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FQuantizedDestructionInput
{
	GENERATED_BODY()

	/** Destruction shape type. */
	UPROPERTY()
	ECellDestructionShapeType Type = ECellDestructionShapeType::Sphere;

	/** Center (mm, integer) - cm * 10. */
	UPROPERTY()
	FIntVector CenterMM = FIntVector::ZeroValue;

	/** Radius (mm, integer) - cm * 10. */
	UPROPERTY()
	int32 RadiusMM = 0;

	/** Box extent (mm). */
	UPROPERTY()
	FIntVector BoxExtentMM = FIntVector::ZeroValue;

	/** Rotation (0.01-degree units, integer). */
	UPROPERTY()
	FIntVector RotationCentidegrees = FIntVector::ZeroValue;

	/** Line end point (mm). */
	UPROPERTY()
	FIntVector EndPointMM = FIntVector::ZeroValue;

	/** Line thickness (mm). */
	UPROPERTY()
	int32 LineThicknessMM = 0;

	/** Build quantized input from float values. */
	static FQuantizedDestructionInput FromDestructionShape(const FCellDestructionShape& Shape);

	/** Restore to a FCellDestructionShape. */
	FCellDestructionShape ToDestructionShape() const;

	/** Check whether a point is inside the shape (quantized values). */
	bool ContainsPoint(const FVector& Point) const;

	/**
	 * Check whether the OBB (Oriented Bounding Box) intersects the destruction shape.
	 * Used for accurate intersection even on non-uniformly scaled meshes.
	 *
	 * @param OBB - OBB in world space
	 * @return Whether they intersect
	 */
	bool IntersectsOBB(const FCellOBB& OBB) const;
};

// =======================================================
// Static Grid Layout
// =======================================================

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FGridCellLayout
{
	GENERATED_BODY()

	//=========================================================================
	// Grid information
	//=========================================================================

	/** Grid size (cell counts in X, Y, Z). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GridLayout")
	FIntVector GridSize = FIntVector::ZeroValue;

	/** Cell size in world space (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridLayout")
	FVector CellSize = FVector(5.0f, 5.0f, 5.0f);

	/** Grid origin (mesh bounds min). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GridLayout")
	FVector GridOrigin = FVector::ZeroVector;

	/** Mesh scale (component scale at build time). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GridLayout")
	FVector MeshScale = FVector::OneVector;

	//=========================================================================
	// Bitfield data (memory optimization)
	//=========================================================================

	/** Cell existence bitfield (1 uint32 per 32 cells). */
	UPROPERTY()
	TArray<uint32> CellExistsBits;

	/** Anchor cell bitfield (1 uint32 per 32 cells). */
	UPROPERTY()
	TArray<uint32> CellIsAnchorBits;

	//=========================================================================
	// Sparse array data (valid cells only)
	//=========================================================================

	/** Valid cell ID -> sparse index mapping. */
	UPROPERTY()
	TMap<int32, int32> CellIdToSparseIndex;

	/** Sparse index -> cell ID reverse mapping. */
	UPROPERTY()
	TArray<int32> SparseIndexToCellId;

	/** Sparse array: per-cell triangle indices (valid cells only). */
	UPROPERTY()
	TArray<FIntArray> SparseCellTriangles;

	/** Sparse array: per-cell neighbor IDs (valid cells only). */
	UPROPERTY()
	TArray<FIntArray> SparseCellNeighbors;

	//=========================================================================
	// Bitfield accessors
	//=========================================================================

	/** Check if a cell exists. */
	FORCEINLINE bool GetCellExists(int32 CellId) const
	{
		const int32 WordIndex = CellId >> 5;  // CellId / 32
		const uint32 BitMask = 1u << (CellId & 31);  // CellId % 32
		return CellExistsBits.IsValidIndex(WordIndex) && (CellExistsBits[WordIndex] & BitMask) != 0;
	}

	/** Set cell existence. */
	FORCEINLINE void SetCellExists(int32 CellId, bool bExists)
	{
		const int32 WordIndex = CellId >> 5;
		const uint32 BitMask = 1u << (CellId & 31);
		if (CellExistsBits.IsValidIndex(WordIndex))
		{
			if (bExists)
				CellExistsBits[WordIndex] |= BitMask;
			else
				CellExistsBits[WordIndex] &= ~BitMask;
		}
	}

	/** Check if a cell is an anchor. */
	FORCEINLINE bool GetCellIsAnchor(int32 CellId) const
	{
		const int32 WordIndex = CellId >> 5;
		const uint32 BitMask = 1u << (CellId & 31);
		return CellIsAnchorBits.IsValidIndex(WordIndex) && (CellIsAnchorBits[WordIndex] & BitMask) != 0;
	}

	/** Set anchor flag. */
	FORCEINLINE void SetCellIsAnchor(int32 CellId, bool bIsAnchor)
	{
		const int32 WordIndex = CellId >> 5;
		const uint32 BitMask = 1u << (CellId & 31);
		if (CellIsAnchorBits.IsValidIndex(WordIndex))
		{
			if (bIsAnchor)
				CellIsAnchorBits[WordIndex] |= BitMask;
			else
				CellIsAnchorBits[WordIndex] &= ~BitMask;
		}
	}

	//=========================================================================
	// Sparse array accessors
	//=========================================================================

	/** Get triangle index array for a cell (empty if none). */
	const FIntArray& GetCellTriangles(int32 CellId) const
	{
		const int32* SparseIdx = CellIdToSparseIndex.Find(CellId);
		if (SparseIdx && SparseCellTriangles.IsValidIndex(*SparseIdx))
		{
			return SparseCellTriangles[*SparseIdx];
		}
		static const FIntArray EmptyArray;
		return EmptyArray;
	}

	/** Get neighbor array for a cell (empty if none). */
	const FIntArray& GetCellNeighbors(int32 CellId) const
	{
		const int32* SparseIdx = CellIdToSparseIndex.Find(CellId);
		if (SparseIdx && SparseCellNeighbors.IsValidIndex(*SparseIdx))
		{
			return SparseCellNeighbors[*SparseIdx];
		}
		static const FIntArray EmptyArray;
		return EmptyArray;
	}

	/** Get mutable triangle index array for a cell. */
	FIntArray* GetCellTrianglesMutable(int32 CellId)
	{
		const int32* SparseIdx = CellIdToSparseIndex.Find(CellId);
		if (SparseIdx && SparseCellTriangles.IsValidIndex(*SparseIdx))
		{
			return &SparseCellTriangles[*SparseIdx];
		}
		return nullptr;
	}

	/** Get mutable neighbor array for a cell. */
	FIntArray* GetCellNeighborsMutable(int32 CellId)
	{
		const int32* SparseIdx = CellIdToSparseIndex.Find(CellId);
		if (SparseIdx && SparseCellNeighbors.IsValidIndex(*SparseIdx))
		{
			return &SparseCellNeighbors[*SparseIdx];
		}
		return nullptr;
	}

	/** Register a valid cell (add to sparse arrays). */
	int32 RegisterValidCell(int32 CellId)
	{
		if (CellIdToSparseIndex.Contains(CellId))
		{
			return CellIdToSparseIndex[CellId];
		}

		const int32 SparseIndex = SparseIndexToCellId.Num();
		CellIdToSparseIndex.Add(CellId, SparseIndex);
		SparseIndexToCellId.Add(CellId);
		SparseCellTriangles.AddDefaulted();
		SparseCellNeighbors.AddDefaulted();
		return SparseIndex;
	}

	/** Valid cell count. */
	int32 GetValidCellCount() const
	{
		return SparseIndexToCellId.Num();
	}

	/** Initialize bitfields (call after GridSize is set). */
	void InitializeBitfields()
	{
		const int32 TotalCells = GetTotalCellCount();
		const int32 RequiredWords = (TotalCells + 31) >> 5;  // ceil(TotalCells / 32)

		CellExistsBits.SetNumZeroed(RequiredWords);
		CellIsAnchorBits.SetNumZeroed(RequiredWords);
	}

	/** Valid cell ID list (safe to return empty array). */
	const TArray<int32>& GetValidCellIds() const
	{
		return SparseIndexToCellId;
	}

	/** Check if sparse arrays are valid. */
	bool HasValidSparseData() const
	{
		return SparseIndexToCellId.Num() > 0 &&
		       SparseCellTriangles.Num() == SparseIndexToCellId.Num() &&
		       SparseCellNeighbors.Num() == SparseIndexToCellId.Num();
	}

	//=========================================================================
	// Helper functions
	//=========================================================================

	/** Total cell count. */
	int32 GetTotalCellCount() const
	{
		return GridSize.X * GridSize.Y * GridSize.Z;
	}

	/** Anchor cell count. */
	int32 GetAnchorCount() const;

	/** 3D coord -> cell ID. */
	int32 CoordToId(int32 X, int32 Y, int32 Z) const
	{
		return Z * (GridSize.X * GridSize.Y) + Y * GridSize.X + X;
	}

	int32 CoordToId(const FIntVector& Coord) const
	{
		return CoordToId(Coord.X, Coord.Y, Coord.Z);
	}

	/** Cell ID -> 3D coord. */
	FIntVector IdToCoord(int32 CellId) const
	{
		const int32 XY = GridSize.X * GridSize.Y;
		const int32 Z = CellId / XY;
		const int32 Remainder = CellId % XY;
		const int32 Y = Remainder / GridSize.X;
		const int32 X = Remainder % GridSize.X;
		return FIntVector(X, Y, Z);
	}

	/** Check if a coord is valid. */
	FORCEINLINE bool IsValidCoord(const FIntVector& Coord) const
	{
		return Coord.X >= 0 && Coord.X < GridSize.X &&
		       Coord.Y >= 0 && Coord.Y < GridSize.Y &&
		       Coord.Z >= 0 && Coord.Z < GridSize.Z;
	}

	/** Check if a coord is valid (int overload). */
	FORCEINLINE bool IsValidCoord(int32 X, int32 Y, int32 Z) const
	{
		return X >= 0 && X < GridSize.X &&
		       Y >= 0 && Y < GridSize.Y &&
		       Z >= 0 && Z < GridSize.Z;
	}

	/** Check if a cell ID is valid. */
	bool IsValidCellId(int32 CellId) const
	{
		return CellId >= 0 && CellId < GetTotalCellCount();
	}

	/** World position -> cell ID (INDEX_NONE if invalid). */
	int32 WorldPosToId(const FVector& WorldPos, const FTransform& MeshTransform) const;

	/** Cell ID -> world center. */
	FVector IdToWorldCenter(int32 CellId, const FTransform& MeshTransform) const;

	/** Cell ID -> local center. */
	FVector IdToLocalCenter(int32 CellId) const;

	/** Cell ID -> world min. */
	FVector IdToWorldMin(int32 CellId, const FTransform& MeshTransform) const;

	/** Cell ID -> local min. */
	FVector IdToLocalMin(int32 CellId) const;

	/** Get the 8 cell vertices (local space). */
	TArray<FVector> GetCellVertices(int32 CellId) const;

	/** Reset layout. */
	void Reset();

	/** Validate layout. */
	bool IsValid() const;

	//=========================================================================
	// SubCell helper functions
	//=========================================================================

	/** SubCell size (local space). */
	FVector GetSubCellSize() const
	{
		return CellSize / static_cast<float>(SUBCELL_DIVISION);
	}

	/** SubCell local center (cell-local space). */
	FVector GetSubCellLocalOffset(int32 SubCellId) const
	{
		const FIntVector SubCoord = SubCellIdToCoord(SubCellId);
		const FVector SubCellSz = GetSubCellSize();
		return FVector(
			(SubCoord.X + 0.5f) * SubCellSz.X,
			(SubCoord.Y + 0.5f) * SubCellSz.Y,
			(SubCoord.Z + 0.5f) * SubCellSz.Z
		);
	}

	/** SubCell local center (mesh local space). */
	FVector GetSubCellLocalCenter(int32 CellId, int32 SubCellId) const
	{
		const FVector CellMin = IdToLocalMin(CellId);
		return CellMin + GetSubCellLocalOffset(SubCellId);
	}
	
	/** SubCell world center. */
	FVector GetSubCellWorldCenter(int32 CellId, int32 SubCellId, const FTransform& MeshTransform) const
	{
		const FVector LocalCenter = GetSubCellLocalCenter(CellId, SubCellId);
		return MeshTransform.TransformPosition(LocalCenter);
	}

	/**
	 * SubCell world OBB (Oriented Bounding Box).
	 * Accurately accounts for mesh rotation and non-uniform scale.
	 */
	FCellOBB GetSubCellWorldOBB(int32 CellId, int32 SubCellId, const FTransform& MeshTransform) const
	{
		const FVector CellMin = IdToLocalMin(CellId);
		const FIntVector SubCoord = SubCellIdToCoord(SubCellId);
		const FVector SubCellSz = GetSubCellSize();

		// SubCell center in local space
		const FVector LocalMin = CellMin + FVector(
			SubCoord.X * SubCellSz.X,
			SubCoord.Y * SubCellSz.Y,
			SubCoord.Z * SubCellSz.Z
		);
		const FVector LocalCenter = LocalMin + SubCellSz * 0.5f;

		// Transform to world space
		const FVector WorldCenter = MeshTransform.TransformPosition(LocalCenter);

		// Half extents with scale applied (local subcell size x transform scale)
		const FVector TransformScale = MeshTransform.GetScale3D();
		const FVector WorldHalfExtents = SubCellSz * 0.5f * TransformScale;

		// Create OBB (rotation only; scale is baked into HalfExtents)
		return FCellOBB(WorldCenter, WorldHalfExtents, MeshTransform.GetRotation());
	}

	FCellOBB GetCellWorldOBB(int32 CellID, const FTransform& MeshTransform) const
	{
		const FVector CellLocalCenter = IdToLocalCenter(CellID);
		const FVector CellWorldCenter = MeshTransform.TransformPosition(CellLocalCenter);
		const FVector HalfExtents = CellSize * 0.5f;

		FCellOBB CellWorldOBB(CellWorldCenter, HalfExtents, MeshTransform.GetRotation());

		return CellWorldOBB;
	}

	/** Get cell IDs inside an AABB. */
	TArray<int32> GetCellsInAABB(const FBox& WorldAABB, const FTransform& MeshTransform) const;
};

USTRUCT()
struct FSubCell
{
	GENERATED_BODY()

	/**
	 * Bitmask (each bit indicates subcell alive state).
	 * 0 = Dead, 1 = Alive
	 * 8 bits represent 8 subcells.
	 * SubCellId = X + Y * 2 + Z * 4
	 */
	UPROPERTY()
	uint8 Bits = 0xFF;  // All subcells alive

	bool IsSubCellAlive(int32 SubCellId) const
	{
		return (Bits & (1 << SubCellId)) != 0;
	}

	void DestroySubCell(int32 SubCellId)
	{
		Bits &= ~(1 << SubCellId);
	}

	/** Check if all subcells are destroyed. */
	bool IsFullyDestroyed() const
	{
		return Bits == 0;
	}

	/** Reset (all subcells alive). */
	void Reset()
	{
		Bits = 0xFF;
	}
};

// =======================================================
// Destruction Results & State
// =======================================================

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDestructionResult
{
	GENERATED_BODY()

	/** Newly destroyed subcell count. */
	UPROPERTY()
	int32 DeadSubCellCount = 0;

	/** Cells affected by subcell destruction. */
	UPROPERTY()
	TArray<int32> AffectedCells;

	/** Newly destroyed subcells (CellId -> SubCellId list). */
	UPROPERTY()
	TMap<int32, FIntArray> NewlyDeadSubCells;

	/** Cells that became destroyed (all subcells destroyed). */
	UPROPERTY()
	TArray<int32> NewlyDestroyedCells;

	/** Whether any destruction occurred. */
	bool HasAnyDestruction() const
	{
		return DeadSubCellCount > 0 || NewlyDestroyedCells.Num() > 0;
	}
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedGroupWithSubCell
{
	GENERATED_BODY()

	/** Fully detached cell IDs (determined by cell-level BFS). */
	UPROPERTY()
	TArray<int32> DetachedCellIds;

	/**
	 * Additional included subcells (determined by subcell flooding).
	 * Key: CellId, Value: included SubCellId list for that cell.
	 * - Alive subcells adjacent to detached cells
	 * - Dead subcells on the flooding boundary
	 */
	UPROPERTY()
	TMap<int32, FIntArray> IncludedSubCells;

	/** Check if the group is empty. */
	bool IsEmpty() const
	{
		return DetachedCellIds.Num() == 0 && IncludedSubCells.Num() == 0;
	}
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FCellState
{
	GENERATED_BODY()

	/** Set of fully destroyed cell IDs. */
	UPROPERTY()
	TSet<int32> DestroyedCells;

	/** Detached cell groups (not yet spawned as debris). */
	UPROPERTY()
	TArray<FDetachedGroupWithSubCell> DetachedGroups;

	/**
	 * Subcell state storage.
	 * Cells touched by a destruction shape gain dead subcells and are added here.
	 * Cells not added here have all subcells alive.
	 */
	UPROPERTY()
	TMap<int32, FSubCell> SubCellStates;

	/** Check if a cell is destroyed. */
	bool IsCellDestroyed(int32 CellId) const
	{
		return DestroyedCells.Contains(CellId);
	}

	/** Check if a subcell is alive. */
	bool IsSubCellAlive(int32 CellId, int32 SubCellId) const
	{
		if (DestroyedCells.Contains(CellId))
		{
			return false;
		}

		const FSubCell* SubCellState = SubCellStates.Find(CellId);
		if (SubCellState)
		{
			return SubCellState->IsSubCellAlive(SubCellId);
		}

		// If no subcell state exists, all subcells are alive
		return true;
	}

	/** Check if a cell is pending detachment. */
	bool IsCellDetached(int32 CellId) const
	{
		for (const FDetachedGroupWithSubCell& Group : DetachedGroups)
		{
			if (Group.DetachedCellIds.Contains(CellId))
			{
				return true;
			}
		}
		return false;
	}

	/** Mark cells destroyed. */
	void DestroyCells(const TArray<int32>& CellIds)
	{
		for (int32 CellId : CellIds)
		{
			DestroyedCells.Add(CellId);
		}
	}

	/** Add detached group (cell IDs only, legacy). */
	void AddDetachedGroup(const TArray<int32>& CellIds)
	{
		FDetachedGroupWithSubCell Group;
		Group.DetachedCellIds = CellIds;
		DetachedGroups.Add(MoveTemp(Group));
	}

	/** Add detached group (with subcell info). */
	void AddDetachedGroup(const FDetachedGroupWithSubCell& Group)
	{
		DetachedGroups.Add(Group);
	}

	/** Add detached group (with subcell info, move). */
	void AddDetachedGroup(FDetachedGroupWithSubCell&& Group)
	{
		DetachedGroups.Add(MoveTemp(Group));
	}

	/** Move detached group to destroyed state (call after debris spawn). */
	void MoveDetachedToDestroyed(int32 GroupIndex)
	{
		if (DetachedGroups.IsValidIndex(GroupIndex))
		{
			const FDetachedGroupWithSubCell& Group = DetachedGroups[GroupIndex];

			// DetachedCellIds -> DestroyedCells
			for (int32 CellId : Group.DetachedCellIds)
			{
				DestroyedCells.Add(CellId);
			}

			// IncludedSubCells -> mark dead in SubCellStates
			for (const auto& SubCellPair : Group.IncludedSubCells)
			{
				const int32 CellId = SubCellPair.Key;
				FSubCell& SubCellState = SubCellStates.FindOrAdd(CellId);
				for (int32 SubCellId : SubCellPair.Value.Values)
				{
					SubCellState.DestroySubCell(SubCellId);
				}
			}

			DetachedGroups.RemoveAt(GroupIndex);
		}
	}

	/** Move all detached groups to destroyed state. */
	void MoveAllDetachedToDestroyed()
	{
		for (const FDetachedGroupWithSubCell& Group : DetachedGroups)
		{
			// DetachedCellIds -> DestroyedCells
			for (int32 CellId : Group.DetachedCellIds)
			{
				DestroyedCells.Add(CellId);
			}

			// IncludedSubCells -> mark dead in SubCellStates
			for (const auto& SubCellPair : Group.IncludedSubCells)
			{
				const int32 CellId = SubCellPair.Key;
				FSubCell& SubCellState = SubCellStates.FindOrAdd(CellId);
				for (int32 SubCellId : SubCellPair.Value.Values)
				{
					SubCellState.DestroySubCell(SubCellId);
				}
			}
		}
		DetachedGroups.Empty();
	}

	/** Reset state. */
	void Reset()
	{
		DestroyedCells.Empty();
		DetachedGroups.Empty();
	}
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedDebrisInfo
{
	GENERATED_BODY()

	/** Debris unique ID. */
	UPROPERTY()
	int32 DebrisId = 0;

	/** Included cell IDs. */
	UPROPERTY()
	TArray<int32> CellIds;

	/** Initial location. */
	UPROPERTY()
	FVector_NetQuantize InitialLocation;

	/** Initial velocity. */
	UPROPERTY()
	FVector_NetQuantize InitialVelocity;
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FBatchedDestructionEvent
{
	GENERATED_BODY()

	/** Quantized destruction inputs (for boolean rendering). */
	UPROPERTY()
	TArray<FQuantizedDestructionInput> DestructionInputs;

	/** All destroyed cell IDs (deduplicated). */
	UPROPERTY()
	TArray<int16> DestroyedCellIds;

	/** Detached debris. */
	UPROPERTY()
	TArray<FDetachedDebrisInfo> DetachedDebris;
};

// =======================================================
// BFS & SuperCell
// =======================================================

/**
 * 2-level hierarchical BFS node.
 * Union type representing a SuperCell or an individual cell.
 *
 * - bIsSupercell = true: Id is a SuperCell ID
 * - bIsSupercell = false: Id is a Cell ID
 */
struct REALTIMEDESTRUCTION_API FCellNode
{
	/** Node ID (SuperCell ID or Cell ID). */
	int32 Id = INDEX_NONE;

	/** Whether this is a SuperCell node. */
	bool bIsSupercell = false;

	FCellNode() = default;

	FCellNode(int32 InId, bool bInIsSupercell)
		: Id(InId), bIsSupercell(bInIsSupercell)
	{}

	/** Create a SuperCell node. */
	static FCellNode MakeSupercell(int32 SupercellId)
	{
		return FCellNode(SupercellId, true);
	}

	/** Create a Cell node. */
	static FCellNode MakeCell(int32 CellId)
	{
		return FCellNode(CellId, false);
	}

	bool IsValid() const { return Id != INDEX_NONE; }

	bool operator==(const FCellNode& Other) const
	{
		return Id == Other.Id && bIsSupercell == Other.bIsSupercell;
	}
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FSuperCellState
{
	GENERATED_BODY()

	//=========================================================================
	// SuperCell grid info
	//=========================================================================

	/** Cells per SuperCell per axis (max 4x4x4). */
	UPROPERTY()
	FIntVector SupercellSize = FIntVector(4, 4, 4);

	/** SuperCell grid size (SuperCell counts in X, Y, Z). */
	UPROPERTY()
	FIntVector SupercellCount = FIntVector::ZeroValue;

	//=========================================================================
	// SuperCell state bitfield
	//=========================================================================

	/**
	 * Intact state bitfield (1 uint64 per 64 SuperCells).
	 * 1 = Intact (all SubCells alive), 0 = Broken
	 */
	UPROPERTY()
	TArray<uint64> IntactBits;

	//=========================================================================
	// Cell <-> SuperCell mapping
	//=========================================================================

	/**
	 * Cell ID -> SuperCell ID mapping.
	 * INDEX_NONE (-1) means orphan cell (not included in a SuperCell).
	 */
	UPROPERTY()
	TArray<int32> CellToSupercell;

	/**
	 * Orphan cell ID list.
	 * Cells not included in a SuperCell (leftover cells at grid edges).
	 */
	UPROPERTY()
	TArray<int32> OrphanCellIds;

	// initial valid number of cells in each Supercell (excluding intact=false cells)
	UPROPERTY()
	TArray<int32> InitialValidCellCounts;

	// Current number of destroyed cells in each Supercell 
	UPROPERTY()
	TArray<int32> DestroyedCellCounts;

	//=========================================================================
	// SuperCell coord <-> ID conversion
	//=========================================================================

	/** 3D coord -> SuperCell ID. */
	FORCEINLINE int32 SupercellCoordToId(int32 X, int32 Y, int32 Z) const
	{
		return Z * (SupercellCount.X * SupercellCount.Y) + Y * SupercellCount.X + X;
	}

	FORCEINLINE int32 SupercellCoordToId(const FIntVector& Coord) const
	{
		return SupercellCoordToId(Coord.X, Coord.Y, Coord.Z);
	}

	/** SuperCell ID -> 3D coord. */
	FORCEINLINE FIntVector SupercellIdToCoord(int32 SupercellId) const
	{
		const int32 XY = SupercellCount.X * SupercellCount.Y;
		const int32 Z = SupercellId / XY;
		const int32 Remainder = SupercellId % XY;
		const int32 Y = Remainder / SupercellCount.X;
		const int32 X = Remainder % SupercellCount.X;
		return FIntVector(X, Y, Z);
	}

	/** Total SuperCell count. */
	FORCEINLINE int32 GetTotalSupercellCount() const
	{
		return SupercellCount.X * SupercellCount.Y * SupercellCount.Z;
	}

	/** Check if SuperCell coord is valid. */
	FORCEINLINE bool IsValidSupercellCoord(const FIntVector& Coord) const
	{
		return Coord.X >= 0 && Coord.X < SupercellCount.X &&
		       Coord.Y >= 0 && Coord.Y < SupercellCount.Y &&
		       Coord.Z >= 0 && Coord.Z < SupercellCount.Z;
	}

	/** Check if SuperCell ID is valid. */
	FORCEINLINE bool IsValidSupercellId(int32 SupercellId) const
	{
		return SupercellId >= 0 && SupercellId < GetTotalSupercellCount();
	}

	//=========================================================================
	// Intact bitfield accessors
	//=========================================================================

	/** Check if a SuperCell is intact. */
	FORCEINLINE bool IsSupercellIntact(int32 SupercellId) const
	{
		const int32 WordIndex = SupercellId >> 6;  // SupercellId / 64
		const uint64 BitMask = 1ull << (SupercellId & 63);  // SupercellId % 64
		return IntactBits.IsValidIndex(WordIndex) && (IntactBits[WordIndex] & BitMask) != 0;
	}

	/** Set SuperCell intact state. */
	FORCEINLINE void SetSupercellIntact(int32 SupercellId, bool bIntact)
	{
		const int32 WordIndex = SupercellId >> 6;
		const uint64 BitMask = 1ull << (SupercellId & 63);
		if (IntactBits.IsValidIndex(WordIndex))
		{
			if (bIntact)
				IntactBits[WordIndex] |= BitMask;
			else
				IntactBits[WordIndex] &= ~BitMask;
		}
	}

	/** Mark SuperCell as broken. */
	FORCEINLINE void MarkSupercellBroken(int32 SupercellId)
	{
		SetSupercellIntact(SupercellId, false);
	}

	//=========================================================================
	// Cell <-> SuperCell relations
	//=========================================================================

	/** Get SuperCell ID for a cell (INDEX_NONE if orphan). */
	FORCEINLINE int32 GetSupercellForCell(int32 CellId) const
	{
		return CellToSupercell.IsValidIndex(CellId) ? CellToSupercell[CellId] : INDEX_NONE;
	}

	/** Check if a cell is orphan. */
	FORCEINLINE bool IsCellOrphan(int32 CellId) const
	{
		return GetSupercellForCell(CellId) == INDEX_NONE;
	}

	/** Cell coord -> SuperCell coord. */
	FORCEINLINE FIntVector CellCoordToSupercellCoord(const FIntVector& CellCoord) const
	{
		return FIntVector(
			CellCoord.X / SupercellSize.X,
			CellCoord.Y / SupercellSize.Y,
			CellCoord.Z / SupercellSize.Z
		);
	}

	/** Check if a cell is on a SuperCell boundary (6-direction check). */
	bool IsCellOnSupercellBoundary(const FIntVector& CellCoord, const FIntVector& SupercellCoord) const;

	//=========================================================================
	// Iterating cells inside a SuperCell
	//=========================================================================

	/** Build list of cell IDs in a SuperCell. */
	void GetCellsInSupercell(int32 SupercellId, const FGridCellLayout& GridLayout, TArray<int32>& OutCellIds) const;

	/** Build list of cell IDs on the SuperCell boundary (6 faces). */
	void GetBoundaryCellsOfSupercell(int32 SupercellId, const FGridCellLayout& GridLayout, TArray<int32>& OutCellIds) const;

	//=========================================================================
	// Build and initialization
	//=========================================================================

	/** Build SuperCell state (call after GridCellLayout is built). */
	void BuildFromGridLayout(const FGridCellLayout& GridLayout);

	/** Initialize intact bitfield (set all SuperCells to intact). */
	void InitializeIntactBits();

	/** Reset state. */
	void Reset();

	/** Validate state. */
	bool IsValid() const;

	//=========================================================================
	// Hierarchical BFS helpers
	//=========================================================================

	/**
	 * Check whether a SuperCell is truly intact (including subcell state).
	 *
	 * Behavior depends on bEnableSubcell:
	 * - bEnableSubcell = true: every subcell of every cell must be alive
	 * - bEnableSubcell = false: no cell may exist in DestroyedCells
	 *
	 * @param SupercellId - SuperCell ID to check
	 * @param GridLayout - grid layout (for cell coord conversion)
	 * @param CellState - cell state (destruction/subcell state)
	 * @param bEnableSubcell - whether subcell mode is enabled
	 * @return Whether the SuperCell is intact
	 */
	bool IsSupercellTrulyIntact(
		int32 SupercellId,
		const FGridCellLayout& GridLayout,
		const FCellState& CellState,
		bool bEnableSubcell) const;

	/**
	 * Update SuperCell states affected by destroyed cells/subcells.
	 *
	 * Call on destruction to mark the owning SuperCell as broken.
	 *
	 * @param AffectedCellIds - affected cell IDs
	 */
	void UpdateSupercellStates(const TArray<int32>& AffectedCellIds);

	/**
	 * Update SuperCell state for a single cell destruction.
	 *
	 * @param CellId - destroyed cell ID
	 */
	void OnCellDestroyed(int32 CellId);

	/**
	 * Update SuperCell state for a subcell destruction.
	 * Call only when bEnableSubcell is true.
	 *
	 * @param CellId - cell ID containing the subcell
	 * @param SubCellId - destroyed SubCell ID
	 */
	void OnSubCellDestroyed(int32 CellId, int32 SubCellId);

	/**
	 * Get boundary cell IDs in a given SuperCell direction.
	 *
	 * @param SupercellId - SuperCell ID
	 * @param Direction - direction (0:-X, 1:+X, 2:-Y, 3:+Y, 4:-Z, 5:+Z)
	 * @param GridLayout - grid layout
	 * @param OutCellIds - boundary cell IDs (output)
	 */
	void GetBoundaryCellsInDirection(
		int32 SupercellId,
		int32 Direction,
		const FGridCellLayout& GridLayout,
		TArray<int32>& OutCellIds) const;
};

struct FConnectivityContext
{
	TArray<uint32> ConnectedCellBits = {};
	TArray<uint32> VisitedSuperCellBits = {};

	TArray<int32> ConnectedCellIds = {};

	TArray<FCellNode> WorkStack = {};

	FConnectivityContext() = default;
	~FConnectivityContext()
	{
		ConnectedCellBits.Empty();
		VisitedSuperCellBits.Empty();
		WorkStack.Empty();
	}

	void Reset(int32 MaxCells, int32 MaxSuperCells)
	{
		// Cell Count
		const int32 RequiredCellWords = (MaxCells + 31) >> 5;	// divide by 32(2^5)

		// Mem re-alloc
		if (ConnectedCellBits.Num() < RequiredCellWords)
		{
			ConnectedCellBits.SetNumUninitialized(RequiredCellWords);
		}

		// Initialize elements to 0
		FMemory::Memzero(ConnectedCellBits.GetData(), sizeof(uint32) * ConnectedCellBits.Num());

		// Super Cell Count
		const int32 RequiredSuperCellWords = (MaxSuperCells + 31) >> 5;	// divide by 32(2^5)

		// Mem re-alloc
		if (VisitedSuperCellBits.Num() < RequiredSuperCellWords)
		{
			VisitedSuperCellBits.SetNumUninitialized(RequiredSuperCellWords);
		}

		// Initialize elements to 0
		FMemory::Memzero(VisitedSuperCellBits.GetData(), sizeof(uint32) * VisitedSuperCellBits.Num());

		// Reset Stack, Not release mem
		WorkStack.Reset();

		// Memory management policy for the connected cell collection array
		// If capacity exceeds 8KB and utilization is under 25%, reallocate to 2x current usage
		int32 CurrentUsage = ConnectedCellIds.Num();			// Current usage
		int32 CurrentArrayCapacity = ConnectedCellIds.Max();	// Current capacity 
		if (CurrentArrayCapacity > 2048 && CurrentUsage < (CurrentArrayCapacity / 4))
		{
			int32 NewCapacity = CurrentUsage * 2;
			ConnectedCellIds.Empty(NewCapacity);
		}
		else
		{
			ConnectedCellIds.Reset();
		}
	}

	FORCEINLINE bool IsCellConnected(int32 CellId)
	{
		if (CellId < 0)
		{
			return false;
		}

		const int32 WordIndex = CellId >> 5;	// Divide by 32
		const uint32 BitMask = 1u << (CellId & 31); // Modulo by 32

		return (ConnectedCellBits[WordIndex] & BitMask) != 0;
	}

	FORCEINLINE void SetCellConnected(int32 CellId)
	{
		if (CellId < 0)
		{
			return;
		}

		const int32 WordIndex = CellId >> 5;	// Divide by 32
		const uint32 BitMask = 1u << (CellId & 31); // Modulo by 32

		if (ConnectedCellBits[WordIndex] & BitMask)
		{
			return;
		}

		ConnectedCellIds.Add(CellId);
		
		// Check visit
		ConnectedCellBits[WordIndex] |= BitMask;
	}

	FORCEINLINE bool IsSuperCellVisited(int32 SuperCellId)
	{
		if (SuperCellId < 0)
		{
			return false;
		}

		const int32 WordIndex = SuperCellId >> 5;	// Divide by 32
		const uint32 BitMask = 1u << (SuperCellId & 31); // Modulo by 32

		return (VisitedSuperCellBits[WordIndex] & BitMask) != 0;
	}

	FORCEINLINE void SetSuperCellVisited(int32 SuperCellId)
	{
		if (SuperCellId < 0)
		{
			return;
		}

		const int32 WordIndex = SuperCellId >> 5;	// Divide by 32
		const uint32 BitMask = 1u << (SuperCellId & 31); // Modulo by 32

		// Check visit
		VisitedSuperCellBits[WordIndex] |= BitMask;
	}

	FORCEINLINE bool CheckAndSetCell(int32 CellId)
	{
		if (CellId < 0)
		{
			return true;
		}
		
		const int32 WordIndex = CellId >> 5;	// Divide by 32
		const uint32 BitMask = 1u << (CellId & 31); // Modulo by 32

		// Visited Cell
		if (ConnectedCellBits[WordIndex] & BitMask)
		{
			return true;
		}

		// Check visit
		ConnectedCellBits[WordIndex] |= BitMask;
		
		return false;
	}

	FORCEINLINE bool CheckAndSetSuperCell(int32 SuperCellId)
	{
		if (SuperCellId < 0)
		{
			return true;
		}
		
		const int32 WordIndex = SuperCellId >> 5;	// Divide by 32
		const uint32 BitMask = 1u << (SuperCellId & 31); // Modulo by 32

		// Visited Cell
		if (VisitedSuperCellBits[WordIndex] & BitMask)
		{
			return true;
		}

		// Check visit
		VisitedSuperCellBits[WordIndex] |= BitMask;
		return false;
	}

	void CollectConnectedCells(TSet<int32>& OutConnectedCells)
	{
		const int32 NumWord = ConnectedCellBits.Num();
		for (int32 i = 0 ; i < NumWord; i++)
		{
			uint32 Word = ConnectedCellBits[i];
			if (Word == 0)
			{
				continue;
			}

			for (int32 Bit = 0 ; Bit < 32; Bit++)
			{
				if (Word & (1u << Bit))
				{
					OutConnectedCells.Add((i << 5) | Bit);
				}
			}
		}
	}
};