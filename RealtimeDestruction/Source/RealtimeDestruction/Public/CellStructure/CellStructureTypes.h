#pragma once

#include "CoreMinimal.h"
#include "CellStructureTypes.generated.h"

UENUM(BlueprintType)
enum class ENeighborhoodMode : uint8
{
	Use6Neighbors,
	Use18Neighbors,
	Use26Neighbors
};

USTRUCT(BlueprintType)
struct FCellStructureSettings
{
	GENERATED_BODY()

	// Base voxel resolution (applies to the smallest AABB extent).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure", meta=(ClampMin="1"))
	int32 BaseResolution = 0;
	// Target number of seed voxels to generate.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure")
	int32 TargetSeedCount = 0;
	// Global seed for deterministic hashing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure")
	int64 GlobalSeed = 0;
	// Number of neighbors when building adjacency.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure")
	ENeighborhoodMode NeighborMode = ENeighborhoodMode::Use6Neighbors;
};

USTRUCT(BlueprintType)
struct FCellStructureDebugOptions
{
	GENERATED_BODY()

	// Draw all voxels (true) or only boundary voxels (false).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure|Debug")
	bool bDrawAllVoxels = true;
	// Draw cell boundaries as colored boxes (each cell has a unique color).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure|Debug")
	bool bDrawCellBoundaries = true;
	// Draw lines connecting neighboring cells.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure|Debug")
	bool bDrawNeighborConnections = true;
	// Draw error cases (validation issues).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure|Debug")
	bool bDrawErrors = true;
	// Duration for debug drawing (in seconds).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure|Debug", meta=(ClampMin="0.1"))
	float DrawDuration = 10.0f;
	// Maximum number of debug elements to draw (to prevent performance issues).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="CellStructure|Debug", meta=(ClampMin="1"))
	int32 MaxDrawCount = 10000;
};

struct FCellStructureData
{
	static constexpr int32 InvalidCellId = INDEX_NONE;

	FVector GridOrigin = FVector::ZeroVector;
	FIntVector VoxelResolution = FIntVector::ZeroValue;
	// Computed voxel edge length in world units.
	float VoxelSize = 1.0f;

	// voxel index i가 속한 cellId (INDEX_NONE = empty/outside)
	TArray<int32> VoxelCellIds;
	//  voxel index i가 inside인지 (1 = true / 0 = false)
	TArray<uint8> VoxelInsideMask;
	// cellId i의 seed voxel 좌표
	TArray<FIntVector> CellSeedVoxels;
	// cellId i의 이웃 cellId 목록
	TArray<TArray<int32>> CellNeighbors;
	// cellId i에 속한 triangleId 목록
	TArray<TArray<int32>> CellTriangles;
	// triangleId i가 속한 cellId (size = MaxTriangleID + 1, INDEX_NONE if unused)
	TArray<int32> TriangleToCell;

	void Reset()
	{
		GridOrigin = FVector::ZeroVector;
		VoxelResolution = FIntVector::ZeroValue;
		VoxelSize = 1.0f;
		VoxelCellIds.Reset();
		VoxelInsideMask.Reset();
		CellSeedVoxels.Reset();
		CellNeighbors.Reset();
		CellTriangles.Reset();
		TriangleToCell.Reset();
	}

	bool IsValid() const
	{
		return VoxelResolution.X > 0 && VoxelResolution.Y > 0 && VoxelResolution.Z > 0 && VoxelSize > 0.0f;
	}

	FORCEINLINE int32 GetVoxelIndex(const FIntVector& Coord) const
	{
		// X -> Y -> Z 방향 순으로 쌓아나감. => Z가 가장 큰 단위
		return (Coord.Z * VoxelResolution.Y * VoxelResolution.X) + (Coord.Y * VoxelResolution.X) + Coord.X;
	}
};
