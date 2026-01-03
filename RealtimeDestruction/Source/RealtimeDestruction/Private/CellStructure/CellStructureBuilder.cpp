// Fill out your copyright notice in the Description page of Project Settings.


#include "CellStructure/CellStructureBuilder.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "Containers/Queue.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"

namespace
{
	struct FTriangleCache
	{
		FVector3d A;
		FVector3d B;
		FVector3d C;
		FVector3d Min; 
		FVector3d Max;
	};

	/**
	 * Build neighbor offsets for the requested neighborhood mode.
	 * Output is a list of integer deltas around the origin (excludes 0,0,0), e.g. 6-neighbors gives the axis-aligned 6.
	 * Purpose: used for voxel flood-fill assignment and for resolving missing cell IDs by checking adjacent voxels.
	 */
	void BuildNeighborOffsets(ENeighborhoodMode Mode, TArray<FIntVector>& OutOffsets)
	{
		OutOffsets.Reset();

		// 6-neighbors: Manhattan distance 1.
		// 18-neighbors: Manhattan distance <= 2 (exclude corners).
		// 26-neighbors: all offsets in the 3x3x3 cube except the origin.
		for (int32 Dz = -1; Dz <= 1; ++Dz)
		{
			for (int32 Dy = -1; Dy <= 1; ++Dy)
			{
				for (int32 Dx = -1; Dx <= 1; ++Dx)
				{
					if (Dx == 0 && Dy == 0 && Dz == 0)
					{	
						continue;
					}

					const int32 Manhattan = FMath::Abs(Dx) + FMath::Abs(Dy) + FMath::Abs(Dz);
					if (Mode == ENeighborhoodMode::Use6Neighbors && Manhattan != 1)
					{
						continue;
					}
					if (Mode == ENeighborhoodMode::Use18Neighbors && Manhattan > 2)
					{
						continue;
					}

					OutOffsets.Add(FIntVector(Dx, Dy, Dz));
				}
			}
		}
	}

	/**
	 * Cache triangle vertices and AABB bounds for point-in-mesh tests.
	 * Output stores each triangle's vertices plus min/max bounds in mesh space.
	 * Purpose: accelerates ray casting by quick AABB rejection before intersection checks.
	 */
	void BuildTriangleCache(const UE::Geometry::FDynamicMesh3& Mesh, TArray<FTriangleCache>& OutTriangles)
	{
		OutTriangles.Reset();
		OutTriangles.Reserve(Mesh.TriangleCount());

		for (int32 TriId = 0; TriId < Mesh.MaxTriangleID(); ++TriId)
		{
			if (!Mesh.IsTriangle(TriId))
			{
				continue;
			}

			const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriId);
			const FVector3d A = Mesh.GetVertex(Tri.A);
			const FVector3d B = Mesh.GetVertex(Tri.B);
			const FVector3d C = Mesh.GetVertex(Tri.C);
			const FVector3d Min = FVector3d(
				FMath::Min3(A.X, B.X, C.X),
				FMath::Min3(A.Y, B.Y, C.Y),
				FMath::Min3(A.Z, B.Z, C.Z));
			const FVector3d Max = FVector3d(
				FMath::Max3(A.X, B.X, C.X),
				FMath::Max3(A.Y, B.Y, C.Y),
				FMath::Max3(A.Z, B.Z, C.Z));

			OutTriangles.Add({A, B, C, Min, Max});
		}
	}

	/**
	 * Build cubic search offsets within the given radius (excluding origin).
	 * Output is all integer deltas in the cube [-R, R]^3 except (0,0,0), including corners/diagonals.
	 * Purpose: fallback search when neighbor offsets fail to resolve a voxel's cell.
	 */
	void BuildSearchOffsets(int32 Radius, TArray<FIntVector>& OutOffsets)
	{
		OutOffsets.Reset();
		if (Radius <= 0)
		{
			return;
		}

		for (int32 Dz = -Radius; Dz <= Radius; ++Dz)
		{
			for (int32 Dy = -Radius; Dy <= Radius; ++Dy)
			{
				for (int32 Dx = -Radius; Dx <= Radius; ++Dx)
				{
					if (Dx == 0 && Dy == 0 && Dz == 0)
					{
						continue;
					}
					OutOffsets.Add(FIntVector(Dx, Dy, Dz));
				}
			}
		}
	}
	
	struct FSeedCandidate
	{
		FIntVector Coord = FIntVector::ZeroValue;
		int32 VoxelIndex = INDEX_NONE;
		uint64 Hash = 0;
	};
	
	/**
	 * Mix a 64-bit value to produce a well-distributed hash.
	 * Output is a scrambled 64-bit value with better bit dispersion than the input.
	 * Purpose: provides deterministic but evenly distributed ordering for voxel seed selection.
	 */
	uint64 SplitMix64(uint64 X)
	{
		X += 0x9E3779B97F4A7C15ULL;
		X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ULL;
		X = (X ^ (X >> 27)) * 0x94D049BB133111EBULL;
		return X ^ (X >> 31);
	}

	/**
	 * Hash a voxel coordinate with a seed for deterministic ordering.
	 * Output combines X/Y/Z and Seed into a single 64-bit hash.
	 * Purpose: ranks voxel candidates reproducibly when picking seed voxels.
	 */
	uint64 HashCoord(uint32 X, uint32 Y, uint32 Z, uint64 Seed)
	{
		uint64 H = Seed;
		H ^= static_cast<uint64>(X) * 0x9E3779B185EBCA87ULL;
		H ^= static_cast<uint64>(Y) * 0xC2B2AE3D27D4EB4FULL;
		H ^= static_cast<uint64>(Z) * 0x165667B19E3779F9ULL;
		return SplitMix64(H);
	}

	/**
	 * Compare voxel coordinates in Z-Y-X order for stable sorting.
	 * Output returns true if A should come before B in lexicographic Z,Y,X order.
	 * Purpose: tie-breaker to make seed selection deterministic when hashes match.
	 */
	bool IsCoordLess(const FIntVector& A, const FIntVector& B)
	{
		if (A.Z != B.Z)
		{
			return A.Z < B.Z;
		}
		if (A.Y != B.Y)
		{
			return A.Y < B.Y;
		}
		return A.X < B.X;
	}

	/**
	 * Compute a coarse grid resolution that approximates the target seed count.
	 * Output is a reduced resolution (Sx,Sy,Sz) whose product is near the target count.
	 * Purpose: distributes seed candidates across coarse cells before trimming to TargetSeedCount.
	 */
	FIntVector ComputeCoarseResolution(const FIntVector& VoxelResolution, int32 TargetSeedCount)
	{
		const double TotalVoxels = static_cast<double>(VoxelResolution.X) * VoxelResolution.Y * VoxelResolution.Z;
		if (TotalVoxels <= 0.0 || TargetSeedCount <= 0)
		{
			return FIntVector(1, 1, 1);
		}

		const double Ratio = static_cast<double>(TargetSeedCount) / TotalVoxels;
		const double T = FMath::Pow(Ratio, 1.0 / 3.0);
		int32 Sx = FMath::Clamp(static_cast<int32>(FMath::RoundHalfToZero(VoxelResolution.X * T)), 1, VoxelResolution.X);
		int32 Sy = FMath::Clamp(static_cast<int32>(FMath::RoundHalfToZero(VoxelResolution.Y * T)), 1, VoxelResolution.Y);
		int32 Sz = FMath::Clamp(static_cast<int32>(FMath::RoundHalfToZero(VoxelResolution.Z * T)), 1, VoxelResolution.Z);

		int64 Product = static_cast<int64>(Sx) * Sy * Sz;
		if (Product == 0)
		{
			return FIntVector(1, 1, 1);
		}

		auto AxisOrderDesc = [&]()
		{
			TArray<int32> Order = {0, 1, 2};
			Order.Sort([&](int32 A, int32 B)
			{
				const int32 Va = (A == 0) ? VoxelResolution.X : (A == 1) ? VoxelResolution.Y : VoxelResolution.Z;
				const int32 Vb = (B == 0) ? VoxelResolution.X : (B == 1) ? VoxelResolution.Y : VoxelResolution.Z;
				if (Va != Vb)
				{
					return Va > Vb;
				}
				return A < B;
			});
			return Order;
		};

		auto AxisOrderAsc = [&]()
		{
			TArray<int32> Order = {0, 1, 2};
			Order.Sort([&](int32 A, int32 B)
			{
				const int32 Va = (A == 0) ? VoxelResolution.X : (A == 1) ? VoxelResolution.Y : VoxelResolution.Z;
				const int32 Vb = (B == 0) ? VoxelResolution.X : (B == 1) ? VoxelResolution.Y : VoxelResolution.Z;
				if (Va != Vb)
				{
					return Va < Vb;
				}
				return A < B;
			});
			return Order;
		};

		const TArray<int32> IncOrder = AxisOrderDesc();
		const TArray<int32> DecOrder = AxisOrderAsc();

		int32 Guard = 0;
		while (Product < TargetSeedCount && Guard++ < 1024)
		{
			bool bAdjusted = false;
			for (int32 Axis : IncOrder)
			{
				int32& Val = (Axis == 0) ? Sx : (Axis == 1) ? Sy : Sz;
				const int32 MaxVal = (Axis == 0) ? VoxelResolution.X : (Axis == 1) ? VoxelResolution.Y : VoxelResolution.Z;
				if (Val < MaxVal)
				{
					++Val;
					Product = static_cast<int64>(Sx) * Sy * Sz;
					bAdjusted = true;
					break;
				}
			}
			if (!bAdjusted)
			{
				break;
			}
		}

		Guard = 0;
		while (Product > TargetSeedCount && Guard++ < 1024)
		{
			bool bAdjusted = false;
			for (int32 Axis : DecOrder)
			{
				int32& Val = (Axis == 0) ? Sx : (Axis == 1) ? Sy : Sz;
				if (Val > 1)
				{
					--Val;
					Product = static_cast<int64>(Sx) * Sy * Sz;
					bAdjusted = true;
					break;
				}
			}
			if (!bAdjusted)
			{
				break;
			}
		}

		return FIntVector(Sx, Sy, Sz);
	}

	/**
	 * Test ray-triangle intersection excluding edges/vertices via epsilon.
	 * Output is true only when the ray hits the triangle interior (not edges/vertices).
	 * Purpose: used by ray casting to avoid double-counting on shared edges.
	 */
	bool RayIntersectsTriangleStrict(const FVector3d& Origin, const FVector3d& Dir,
		const FTriangleCache& Tri, double Eps)
	{
		const FVector3d Edge1 = Tri.B - Tri.A;
		const FVector3d Edge2 = Tri.C - Tri.A;
		const FVector3d Pvec = Dir.Cross(Edge2);
		const double Det = Edge1.Dot(Pvec);
		if (FMath::Abs(Det) <= Eps)
		{
			return false;
		}

		const double InvDet = 1.0 / Det;
		const FVector3d Tvec = Origin - Tri.A;
		const double U = Tvec.Dot(Pvec) * InvDet;
		if (U <= Eps || U >= 1.0 - Eps)
		{
			return false;
		}

		const FVector3d Qvec = Tvec.Cross(Edge1);
		const double V = Dir.Dot(Qvec) * InvDet;
		if (V <= Eps || (U + V) >= 1.0 - Eps)
		{
			return false;
		}

		const double T = Edge2.Dot(Qvec) * InvDet;
		return T > Eps;
	}

	/**
	 * Determine if a point is inside the mesh using +X ray casting.
	 * Output is true if the +X ray from the point hits an odd number of triangles.
	 * Purpose: marks inside voxels when building the cell grid.
	 */
	bool IsPointInsideMeshRayX(const FVector3d& Point, const TArray<FTriangleCache>& Triangles, double Eps)
	{
		const FVector3d RayDir(1.0, 0.0, 0.0);
		int32 HitCount = 0;

		for (const FTriangleCache& Tri : Triangles)
		{
			if (Point.Y < (Tri.Min.Y - Eps) || Point.Y > (Tri.Max.Y + Eps))
			{
				continue;
			}
			if (Point.Z < (Tri.Min.Z - Eps) || Point.Z > (Tri.Max.Z + Eps))
			{
				continue;
			}
			if (Point.X > (Tri.Max.X - Eps))
			{
				continue;
			}

			if (RayIntersectsTriangleStrict(Point, RayDir, Tri, Eps))
			{
				++HitCount;
			}
		}

		return (HitCount & 1) != 0;
	}
}

bool FCellStructureBuilder::BuildFromMesh(const UE::Geometry::FDynamicMesh3& Mesh,
	const FCellStructureSettings& Settings,
	FCellStructureData& OutData,
	UWorld* World,
	bool bValidate,
	const FTransform& DebugTransform) const
{
	OutData.Reset();
	OutData.VoxelSize = 0.0f;

	if (Settings.TargetSeedCount <= 0)
	{
		return false;
	}
	if (Settings.BaseResolution <= 0)
	{
		return false;
	}

	TArray<FIntVector> NeighborOffsets;
	BuildNeighborOffsets(Settings.NeighborMode, NeighborOffsets);
	if (NeighborOffsets.IsEmpty())
	{
		return false;
	}
	TArray<FIntVector> FallbackOffsets;
	BuildSearchOffsets(2, FallbackOffsets);

	const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh.GetBounds();
	const FVector3d Extents = Bounds.Max - Bounds.Min;
	const double MinExtent = FMath::Min3(Extents.X, Extents.Y, Extents.Z);
	if (MinExtent <= 0.0)
	{
		return false;
	}

	const double VoxelSize = MinExtent / static_cast<double>(Settings.BaseResolution);
	if (VoxelSize <= 0.0)
	{
		return false;
	}

	OutData.GridOrigin = FVector(Bounds.Min);
	OutData.VoxelSize = static_cast<float>(VoxelSize);
	OutData.VoxelResolution = FIntVector(
		FMath::Max(1, FMath::CeilToInt(Extents.X / VoxelSize)),
		FMath::Max(1, FMath::CeilToInt(Extents.Y / VoxelSize)),
		FMath::Max(1, FMath::CeilToInt(Extents.Z / VoxelSize)));

	const int32 VoxelCount = OutData.VoxelResolution.X * OutData.VoxelResolution.Y * OutData.VoxelResolution.Z;
	OutData.VoxelCellIds.Init(FCellStructureData::InvalidCellId, VoxelCount);
	OutData.VoxelInsideMask.Init(0, VoxelCount);

	TArray<FTriangleCache> Triangles;
	BuildTriangleCache(Mesh, Triangles);
	if (Triangles.IsEmpty())
	{
		return false;
	}

	const FVector3d GridOrigin = FVector3d(OutData.GridOrigin);
	const double Eps = 1e-6;

	for (int32 Z = 0; Z < OutData.VoxelResolution.Z; ++Z)
	{
		const double Pz = GridOrigin.Z + (static_cast<double>(Z) + 0.5) * VoxelSize;
		for (int32 Y = 0; Y < OutData.VoxelResolution.Y; ++Y)
		{
			const double Py = GridOrigin.Y + (static_cast<double>(Y) + 0.5) * VoxelSize;
			for (int32 X = 0; X < OutData.VoxelResolution.X; ++X)
			{
				const double Px = GridOrigin.X + (static_cast<double>(X) + 0.5) * VoxelSize;
				const FVector3d Sample(Px, Py, Pz);
				const int32 Index = (Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
					+ (Y * OutData.VoxelResolution.X) + X;
				if (IsPointInsideMeshRayX(Sample, Triangles, Eps))
				{
					OutData.VoxelInsideMask[Index] = 1;
				}
			}
		}
	}

	int32 InsideCount = 0;
	for (uint8 Value : OutData.VoxelInsideMask)
	{
		InsideCount += Value ? 1 : 0;
	}
	if (InsideCount <= 0)
	{
		return false;
	}

	const int32 TargetSeedCount = FMath::Min(Settings.TargetSeedCount, InsideCount);
	const FIntVector CoarseResolution = ComputeCoarseResolution(OutData.VoxelResolution, TargetSeedCount);
	const int32 CoarseCount = CoarseResolution.X * CoarseResolution.Y * CoarseResolution.Z;
	if (CoarseCount <= 0)
	{
		return false;
	}

	TArray<uint8> HasBest;
	TArray<uint64> BestHash;
	TArray<FIntVector> BestCoord;
	HasBest.Init(0, CoarseCount);
	BestHash.Init(TNumericLimits<uint64>::Max(), CoarseCount);
	BestCoord.Init(FIntVector::ZeroValue, CoarseCount);

	auto GetCoarseIndex = [&](int32 X, int32 Y, int32 Z)
	{
		const int32 Cx = (X * CoarseResolution.X) / OutData.VoxelResolution.X;
		const int32 Cy = (Y * CoarseResolution.Y) / OutData.VoxelResolution.Y;
		const int32 Cz = (Z * CoarseResolution.Z) / OutData.VoxelResolution.Z;
		return (Cz * CoarseResolution.Y * CoarseResolution.X) + (Cy * CoarseResolution.X) + Cx;
	};

	for (int32 Z = 0; Z < OutData.VoxelResolution.Z; ++Z)
	{
		for (int32 Y = 0; Y < OutData.VoxelResolution.Y; ++Y)
		{
			for (int32 X = 0; X < OutData.VoxelResolution.X; ++X)
			{
				const int32 Index = (Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
					+ (Y * OutData.VoxelResolution.X) + X;
				if (OutData.VoxelInsideMask[Index] == 0)
				{
					continue;
				}

				const int32 CoarseIndex = GetCoarseIndex(X, Y, Z);
				const uint64 Hash = HashCoord(static_cast<uint32>(X), static_cast<uint32>(Y), static_cast<uint32>(Z),
					static_cast<uint64>(Settings.GlobalSeed));
				if (!HasBest[CoarseIndex])
				{
					HasBest[CoarseIndex] = 1;
					BestHash[CoarseIndex] = Hash;
					BestCoord[CoarseIndex] = FIntVector(X, Y, Z);
				}
				else if (Hash < BestHash[CoarseIndex])
				{
					BestHash[CoarseIndex] = Hash;
					BestCoord[CoarseIndex] = FIntVector(X, Y, Z);
				}
				else if (Hash == BestHash[CoarseIndex] && IsCoordLess(FIntVector(X, Y, Z), BestCoord[CoarseIndex]))
				{
					BestCoord[CoarseIndex] = FIntVector(X, Y, Z);
				}
			}
		}
	}

	TArray<FSeedCandidate> Seeds;
	Seeds.Reserve(CoarseCount);
	for (int32 CoarseIndex = 0; CoarseIndex < CoarseCount; ++CoarseIndex)
	{
		if (!HasBest[CoarseIndex])
		{
			continue;
		}

		const FIntVector Coord = BestCoord[CoarseIndex];
		const int32 Index = (Coord.Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
			+ (Coord.Y * OutData.VoxelResolution.X) + Coord.X;
		Seeds.Add({Coord, Index, BestHash[CoarseIndex]});
	}

	auto SeedSort = [](const FSeedCandidate& A, const FSeedCandidate& B)
	{
		if (A.Hash != B.Hash)
		{
			return A.Hash < B.Hash;
		}
		return IsCoordLess(A.Coord, B.Coord);
	};

	if (Seeds.Num() > TargetSeedCount)
	{
		Algo::Sort(Seeds, SeedSort);
		Seeds.SetNum(TargetSeedCount, EAllowShrinking::No);
	}
	else if (Seeds.Num() < TargetSeedCount)
	{
		TArray<uint8> SeedMask;
		SeedMask.Init(0, OutData.VoxelInsideMask.Num());
		for (const FSeedCandidate& Seed : Seeds)
		{
			if (Seed.VoxelIndex >= 0 && Seed.VoxelIndex < SeedMask.Num())
			{
				SeedMask[Seed.VoxelIndex] = 1;
			}
		}

		TArray<FSeedCandidate> Extras;
		Extras.Reserve(InsideCount - Seeds.Num());

		for (int32 Z = 0; Z < OutData.VoxelResolution.Z; ++Z)
		{
			for (int32 Y = 0; Y < OutData.VoxelResolution.Y; ++Y)
			{
				for (int32 X = 0; X < OutData.VoxelResolution.X; ++X)
				{
					const int32 Index = (Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
						+ (Y * OutData.VoxelResolution.X) + X;
					if (OutData.VoxelInsideMask[Index] == 0 || SeedMask[Index] != 0)
					{
						continue;
					}

					const uint64 Hash = HashCoord(static_cast<uint32>(X), static_cast<uint32>(Y), static_cast<uint32>(Z),
						static_cast<uint64>(Settings.GlobalSeed));
					Extras.Add({FIntVector(X, Y, Z), Index, Hash});
				}
			}
		}

		if (!Extras.IsEmpty())
		{
			Algo::Sort(Extras, SeedSort);
			const int32 Needed = TargetSeedCount - Seeds.Num();
			for (int32 i = 0; i < Needed && i < Extras.Num(); ++i)
			{
				Seeds.Add(Extras[i]);
			}
		}
	}

	if (Seeds.IsEmpty())
	{
		return false;
	}

	Algo::Sort(Seeds, SeedSort);
	OutData.CellSeedVoxels.Reset(Seeds.Num());
	for (const FSeedCandidate& Seed : Seeds)
	{
		OutData.CellSeedVoxels.Add(Seed.Coord);
	}

	OutData.VoxelCellIds.Init(FCellStructureData::InvalidCellId, VoxelCount);
	TQueue<int32> Queue;

	for (int32 CellId = 0; CellId < OutData.CellSeedVoxels.Num(); ++CellId)
	{
		const FIntVector Coord = OutData.CellSeedVoxels[CellId];
		const int32 Index = (Coord.Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
			+ (Coord.Y * OutData.VoxelResolution.X) + Coord.X;
		if (Index < 0 || Index >= VoxelCount)
		{
			continue;
		}
		if (OutData.VoxelInsideMask[Index] == 0)
		{
			continue;
		}

		if (OutData.VoxelCellIds[Index] == FCellStructureData::InvalidCellId)
		{
			OutData.VoxelCellIds[Index] = CellId;
			Queue.Enqueue(Index);
		}
	}

	while (!Queue.IsEmpty())
	{
		int32 Index = INDEX_NONE;
		Queue.Dequeue(Index);

		const int32 X = Index % OutData.VoxelResolution.X;
		const int32 Y = (Index / OutData.VoxelResolution.X) % OutData.VoxelResolution.Y;
		const int32 Z = Index / (OutData.VoxelResolution.X * OutData.VoxelResolution.Y);
		const int32 CellId = OutData.VoxelCellIds[Index];

		for (const FIntVector& Offset : NeighborOffsets)
		{
			const int32 Nx = X + Offset.X;
			const int32 Ny = Y + Offset.Y;
			const int32 Nz = Z + Offset.Z;
			if (Nx < 0 || Ny < 0 || Nz < 0
				|| Nx >= OutData.VoxelResolution.X
				|| Ny >= OutData.VoxelResolution.Y
				|| Nz >= OutData.VoxelResolution.Z)
			{
				continue;
			}

			const int32 NIndex = (Nz * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
				+ (Ny * OutData.VoxelResolution.X) + Nx;
			if (OutData.VoxelInsideMask[NIndex] == 0)
			{
				continue;
			}

			if (OutData.VoxelCellIds[NIndex] == FCellStructureData::InvalidCellId)
			{
				OutData.VoxelCellIds[NIndex] = CellId;
				Queue.Enqueue(NIndex);
			}
		}
	}

	OutData.CellNeighbors.Reset(OutData.CellSeedVoxels.Num());
	OutData.CellNeighbors.SetNum(OutData.CellSeedVoxels.Num());

	for (int32 Z = 0; Z < OutData.VoxelResolution.Z; ++Z)
	{
		for (int32 Y = 0; Y < OutData.VoxelResolution.Y; ++Y)
		{
			for (int32 X = 0; X < OutData.VoxelResolution.X; ++X)
			{
				const int32 Index = (Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
					+ (Y * OutData.VoxelResolution.X) + X;
				if (OutData.VoxelInsideMask[Index] == 0)
				{
					continue;
				}

				const int32 CellId = OutData.VoxelCellIds[Index];
				if (CellId == FCellStructureData::InvalidCellId)
				{
					continue;
				}

				for (const FIntVector& Offset : NeighborOffsets)
				{
					const int32 Nx = X + Offset.X;
					const int32 Ny = Y + Offset.Y;
					const int32 Nz = Z + Offset.Z;
					if (Nx < 0 || Ny < 0 || Nz < 0
						|| Nx >= OutData.VoxelResolution.X
						|| Ny >= OutData.VoxelResolution.Y
						|| Nz >= OutData.VoxelResolution.Z)
					{
						continue;
					}

					const int32 NIndex = (Nz * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
						+ (Ny * OutData.VoxelResolution.X) + Nx;
					if (OutData.VoxelInsideMask[NIndex] == 0)
					{
						continue;
					}

					const int32 OtherCell = OutData.VoxelCellIds[NIndex];
					if (OtherCell == FCellStructureData::InvalidCellId || OtherCell == CellId)
					{
						continue;
					}

					if (OtherCell > CellId)
					{
						OutData.CellNeighbors[CellId].Add(OtherCell);
						OutData.CellNeighbors[OtherCell].Add(CellId);
					}
				}
			}
		}
	}

	for (TArray<int32>& Neighbors : OutData.CellNeighbors)
	{
		Neighbors.Sort();
		Neighbors.SetNum(Algo::Unique(Neighbors));
	}

	OutData.CellTriangles.Reset(OutData.CellSeedVoxels.Num());
	OutData.CellTriangles.SetNum(OutData.CellSeedVoxels.Num());
	OutData.TriangleToCell.Init(FCellStructureData::InvalidCellId, Mesh.MaxTriangleID());

	const FVector3d GridOriginD = FVector3d(OutData.GridOrigin);
	const double InvVoxelSize = (OutData.VoxelSize > 0.0f) ? (1.0 / OutData.VoxelSize) : 0.0;
	if (InvVoxelSize <= 0.0)
	{
		return false;
	}

	const double CoordEps = 1e-6;
	auto ResolveCellForPoint = [&](const FVector3d& Point)
	{
		const double LocalX = (Point.X - GridOriginD.X) * InvVoxelSize;
		const double LocalY = (Point.Y - GridOriginD.Y) * InvVoxelSize;
		const double LocalZ = (Point.Z - GridOriginD.Z) * InvVoxelSize;
		const double MaxX = static_cast<double>(OutData.VoxelResolution.X);
		const double MaxY = static_cast<double>(OutData.VoxelResolution.Y);
		const double MaxZ = static_cast<double>(OutData.VoxelResolution.Z);
		if (LocalX < -CoordEps || LocalY < -CoordEps || LocalZ < -CoordEps
			|| LocalX > MaxX + CoordEps || LocalY > MaxY + CoordEps || LocalZ > MaxZ + CoordEps)
		{
			return FCellStructureData::InvalidCellId;
		}

		const int32 X = FMath::Clamp(FMath::FloorToInt(LocalX), 0, OutData.VoxelResolution.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt(LocalY), 0, OutData.VoxelResolution.Y - 1);
		const int32 Z = FMath::Clamp(FMath::FloorToInt(LocalZ), 0, OutData.VoxelResolution.Z - 1);

		const int32 Index = (Z * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
			+ (Y * OutData.VoxelResolution.X) + X;
		int32 CellId = OutData.VoxelCellIds.IsValidIndex(Index)
			? OutData.VoxelCellIds[Index]
			: FCellStructureData::InvalidCellId;
		if (CellId != FCellStructureData::InvalidCellId)
		{
			return CellId;
		}

		auto ConsiderOffsets = [&](const TArray<FIntVector>& Offsets, int32& BestDistance, int32& BestCell)
		{
			for (const FIntVector& Offset : Offsets)
			{
				const int32 Nx = X + Offset.X;
				const int32 Ny = Y + Offset.Y;
				const int32 Nz = Z + Offset.Z;
				if (Nx < 0 || Ny < 0 || Nz < 0
					|| Nx >= OutData.VoxelResolution.X
					|| Ny >= OutData.VoxelResolution.Y
					|| Nz >= OutData.VoxelResolution.Z)
				{
					continue;
				}

				const int32 NIndex = (Nz * OutData.VoxelResolution.Y * OutData.VoxelResolution.X)
					+ (Ny * OutData.VoxelResolution.X) + Nx;
				const int32 NeighborCell = OutData.VoxelCellIds.IsValidIndex(NIndex)
					? OutData.VoxelCellIds[NIndex]
					: FCellStructureData::InvalidCellId;
				if (NeighborCell == FCellStructureData::InvalidCellId)
				{
					continue;
				}

				const int32 Distance = FMath::Abs(Offset.X) + FMath::Abs(Offset.Y) + FMath::Abs(Offset.Z);
				if (Distance < BestDistance
					|| (Distance == BestDistance && (BestCell == FCellStructureData::InvalidCellId || NeighborCell < BestCell)))
				{
					BestDistance = Distance;
					BestCell = NeighborCell;
				}
			}
		};

		int32 BestDistance = TNumericLimits<int32>::Max();
		int32 BestCell = FCellStructureData::InvalidCellId;
		ConsiderOffsets(NeighborOffsets, BestDistance, BestCell);
		if (BestCell == FCellStructureData::InvalidCellId)
		{
			ConsiderOffsets(FallbackOffsets, BestDistance, BestCell);
		}

		return BestCell;
	};

	for (int32 TriId = 0; TriId < Mesh.MaxTriangleID(); ++TriId)
	{
		if (!Mesh.IsTriangle(TriId))
		{
			continue;
		}

		const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriId);
		const FVector3d A = Mesh.GetVertex(Tri.A);
		const FVector3d B = Mesh.GetVertex(Tri.B);
		const FVector3d C = Mesh.GetVertex(Tri.C);
		const FVector3d Centroid = (A + B + C) / 3.0;

		const int32 CellId = ResolveCellForPoint(Centroid);

		if (CellId != FCellStructureData::InvalidCellId)
		{
			OutData.TriangleToCell[TriId] = CellId;
			OutData.CellTriangles[CellId].Add(TriId);
		}
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (bValidate)
	{
		ValidateCellStructureData(Mesh, Settings, OutData, World, 128, DebugTransform);
	}
#endif

	return true;
}

bool FCellStructureBuilder::ValidateCellStructureData(const UE::Geometry::FDynamicMesh3& Mesh,
	const FCellStructureSettings& Settings,
	const FCellStructureData& Data,
	UWorld* World,
	int32 MaxDrawCount,
	const FTransform& DebugTransform) const
{
	if (!Data.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("CellStructure validation failed: Data is not valid."));
		return false;
	}

	const int32 VoxelCount = Data.VoxelResolution.X * Data.VoxelResolution.Y * Data.VoxelResolution.Z;
	const int32 CellCount = Data.CellSeedVoxels.Num();
	const int32 MaxTriangleId = Mesh.MaxTriangleID();

	int32 IssueCount = 0;

	int32 SizeMismatchErrors = 0;
	int32 InsideVoxelErrors = 0;
	int32 OutsideVoxelErrors = 0;
	int32 SeedErrors = 0;
	int32 NeighborErrors = 0;
	int32 TriangleErrors = 0;
	int32 TriangleMismatchErrors = 0;
	int32 CellTriangleErrors = 0;

	const int32 MaxDetailLogs = 10;

	auto LogIssue = [&](int32& Counter, ELogVerbosity::Type Verbosity, const FString& Message)
	{
		++Counter;
		++IssueCount;
		if (Counter <= MaxDetailLogs)
		{
			switch (Verbosity)
			{
				case ELogVerbosity::Error:
					UE_LOG(LogTemp, Error, TEXT("CellStructure validation: %s"), *Message);
					break;
				case ELogVerbosity::Warning:
					UE_LOG(LogTemp, Warning, TEXT("CellStructure validation: %s"), *Message);
					break;
				default:
					UE_LOG(LogTemp, Log, TEXT("CellStructure validation: %s"), *Message);
					break;
			}
		}
	};

	// ===== Size mismatch checks =====
	if (Data.VoxelCellIds.Num() != VoxelCount)
	{
		LogIssue(SizeMismatchErrors, ELogVerbosity::Warning,
			FString::Printf(TEXT("VoxelCellIds size mismatch (expected %d, got %d)."),
				VoxelCount, Data.VoxelCellIds.Num()));
	}
	if (Data.VoxelInsideMask.Num() != VoxelCount)
	{
		LogIssue(SizeMismatchErrors, ELogVerbosity::Warning,
			FString::Printf(TEXT("VoxelInsideMask size mismatch (expected %d, got %d)."),
				VoxelCount, Data.VoxelInsideMask.Num()));
	}
	if (Data.CellNeighbors.Num() != CellCount)
	{
		LogIssue(SizeMismatchErrors, ELogVerbosity::Warning,
			FString::Printf(TEXT("CellNeighbors size mismatch (expected %d, got %d)."),
				CellCount, Data.CellNeighbors.Num()));
	}
	if (Data.CellTriangles.Num() != CellCount)
	{
		LogIssue(SizeMismatchErrors, ELogVerbosity::Warning,
			FString::Printf(TEXT("CellTriangles size mismatch (expected %d, got %d)."),
				CellCount, Data.CellTriangles.Num()));
	}
	if (Data.TriangleToCell.Num() != MaxTriangleId)
	{
		LogIssue(SizeMismatchErrors, ELogVerbosity::Warning,
			FString::Printf(TEXT("TriangleToCell size mismatch (expected %d, got %d)."),
				MaxTriangleId, Data.TriangleToCell.Num()));
	}

	TArray<FIntVector> NeighborOffsets;
	BuildNeighborOffsets(Settings.NeighborMode, NeighborOffsets);
	if (NeighborOffsets.IsEmpty())
	{
		LogIssue(SizeMismatchErrors, ELogVerbosity::Warning, TEXT("Neighbor offsets are empty."));
	}
	TArray<FIntVector> FallbackOffsets;
	BuildSearchOffsets(2, FallbackOffsets);

	// ===== Voxel validation =====
	if (VoxelCount > 0 && Data.VoxelInsideMask.Num() == VoxelCount && Data.VoxelCellIds.Num() == VoxelCount)
	{
		for (int32 Index = 0; Index < VoxelCount; ++Index)
		{
			const bool bInside = Data.VoxelInsideMask[Index] != 0;
			const int32 CellId = Data.VoxelCellIds[Index];
			if (bInside)
			{
				if (CellId == FCellStructureData::InvalidCellId || CellId < 0 || CellId >= CellCount)
				{
					LogIssue(InsideVoxelErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Inside voxel without valid cell (index %d, cell %d)."),
							Index, CellId));
				}
			}
			else
			{
				if (CellId != FCellStructureData::InvalidCellId)
				{
					LogIssue(OutsideVoxelErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Outside voxel has assigned cell (index %d, cell %d)."),
							Index, CellId));
				}
			}
		}
	}

	// ===== Seed validation =====
	if (CellCount > 0)
	{
		for (int32 CellId = 0; CellId < CellCount; ++CellId)
		{
			const FIntVector Seed = Data.CellSeedVoxels[CellId];
			const int32 Index = Data.GetVoxelIndex(Seed);
			const bool bIndexValid = Index >= 0 && Index < VoxelCount;
			const bool bInside = bIndexValid && Data.VoxelInsideMask.IsValidIndex(Index)
				&& Data.VoxelInsideMask[Index] != 0;
			if (!bInside)
			{
				LogIssue(SeedErrors, ELogVerbosity::Warning,
					FString::Printf(TEXT("Seed voxel is not inside (cell %d, coord %d %d %d)."),
						CellId, Seed.X, Seed.Y, Seed.Z));
			}
			else if (Data.VoxelCellIds.IsValidIndex(Index) && Data.VoxelCellIds[Index] != CellId)
			{
				LogIssue(SeedErrors, ELogVerbosity::Warning,
					FString::Printf(TEXT("Seed voxel cell mismatch (cell %d, coord %d %d %d, assigned %d)."),
						CellId, Seed.X, Seed.Y, Seed.Z, Data.VoxelCellIds[Index]));
			}
		}
	}

	// ===== Neighbor validation =====
	if (Data.CellNeighbors.Num() == CellCount)
	{
		for (int32 CellId = 0; CellId < CellCount; ++CellId)
		{
			const TArray<int32>& Neighbors = Data.CellNeighbors[CellId];
			for (int32 i = 0; i < Neighbors.Num(); ++i)
			{
				const int32 Neighbor = Neighbors[i];
				if (Neighbor == CellId)
				{
					LogIssue(NeighborErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Cell has self neighbor (cell %d)."), CellId));
					continue;
				}
				if (Neighbor < 0 || Neighbor >= CellCount)
				{
					LogIssue(NeighborErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Neighbor out of range (cell %d, neighbor %d)."), CellId, Neighbor));
					continue;
				}

				if (i > 0 && Neighbors[i] == Neighbors[i - 1])
				{
					LogIssue(NeighborErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Duplicate neighbor (cell %d, neighbor %d)."), CellId, Neighbor));
				}

				if (!Data.CellNeighbors[Neighbor].Contains(CellId))
				{
					LogIssue(NeighborErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Neighbor symmetry mismatch (cell %d, neighbor %d)."), CellId, Neighbor));
				}
			}
		}
	}

	// ===== Triangle validation =====
	const FVector3d GridOrigin = FVector3d(Data.GridOrigin);
	const double InvVoxelSize = (Data.VoxelSize > 0.0f) ? (1.0 / Data.VoxelSize) : 0.0;
	const double CoordEps = 1e-6;

	auto FindCellForPoint = [&](const FVector3d& Point)
	{
		if (InvVoxelSize <= 0.0)
		{
			return FCellStructureData::InvalidCellId;
		}

		const double LocalX = (Point.X - GridOrigin.X) * InvVoxelSize;
		const double LocalY = (Point.Y - GridOrigin.Y) * InvVoxelSize;
		const double LocalZ = (Point.Z - GridOrigin.Z) * InvVoxelSize;
		const double MaxX = static_cast<double>(Data.VoxelResolution.X);
		const double MaxY = static_cast<double>(Data.VoxelResolution.Y);
		const double MaxZ = static_cast<double>(Data.VoxelResolution.Z);
		if (LocalX < -CoordEps || LocalY < -CoordEps || LocalZ < -CoordEps
			|| LocalX > MaxX + CoordEps || LocalY > MaxY + CoordEps || LocalZ > MaxZ + CoordEps)
		{
			return FCellStructureData::InvalidCellId;
		}

		const int32 X = FMath::Clamp(FMath::FloorToInt(LocalX), 0, Data.VoxelResolution.X - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt(LocalY), 0, Data.VoxelResolution.Y - 1);
		const int32 Z = FMath::Clamp(FMath::FloorToInt(LocalZ), 0, Data.VoxelResolution.Z - 1);

		const int32 Index = (Z * Data.VoxelResolution.Y * Data.VoxelResolution.X)
			+ (Y * Data.VoxelResolution.X) + X;
		int32 CellId = Data.VoxelCellIds.IsValidIndex(Index)
			? Data.VoxelCellIds[Index]
			: FCellStructureData::InvalidCellId;
		if (CellId != FCellStructureData::InvalidCellId)
		{
			return CellId;
		}

		auto ConsiderOffsets = [&](const TArray<FIntVector>& Offsets, int32& BestDistance, int32& BestCell)
		{
			for (const FIntVector& Offset : Offsets)
			{
				const int32 Nx = X + Offset.X;
				const int32 Ny = Y + Offset.Y;
				const int32 Nz = Z + Offset.Z;
				if (Nx < 0 || Ny < 0 || Nz < 0
					|| Nx >= Data.VoxelResolution.X
					|| Ny >= Data.VoxelResolution.Y
					|| Nz >= Data.VoxelResolution.Z)
				{
					continue;
				}

				const int32 NIndex = (Nz * Data.VoxelResolution.Y * Data.VoxelResolution.X)
					+ (Ny * Data.VoxelResolution.X) + Nx;
				const int32 NeighborCell = Data.VoxelCellIds.IsValidIndex(NIndex)
					? Data.VoxelCellIds[NIndex]
					: FCellStructureData::InvalidCellId;
				if (NeighborCell == FCellStructureData::InvalidCellId)
				{
					continue;
				}

				const int32 Distance = FMath::Abs(Offset.X) + FMath::Abs(Offset.Y) + FMath::Abs(Offset.Z);
				if (Distance < BestDistance
					|| (Distance == BestDistance && (BestCell == FCellStructureData::InvalidCellId || NeighborCell < BestCell)))
				{
					BestDistance = Distance;
					BestCell = NeighborCell;
				}
			}
		};

		int32 BestDistance = TNumericLimits<int32>::Max();
		int32 BestCell = FCellStructureData::InvalidCellId;
		ConsiderOffsets(NeighborOffsets, BestDistance, BestCell);
		if (BestCell == FCellStructureData::InvalidCellId)
		{
			ConsiderOffsets(FallbackOffsets, BestDistance, BestCell);
		}

		return BestCell;
	};

	if (Data.TriangleToCell.Num() == MaxTriangleId && CellCount > 0)
	{
		for (int32 TriId = 0; TriId < MaxTriangleId; ++TriId)
		{
			if (!Mesh.IsTriangle(TriId))
			{
				continue;
			}

			const int32 CellId = Data.TriangleToCell[TriId];
			if (CellId == FCellStructureData::InvalidCellId || CellId < 0 || CellId >= CellCount)
			{
				LogIssue(TriangleErrors, ELogVerbosity::Warning,
					FString::Printf(TEXT("Triangle has invalid cell mapping (triangle %d, cell %d)."), TriId, CellId));
				continue;
			}

			if (!Data.CellTriangles.IsValidIndex(CellId) || !Data.CellTriangles[CellId].Contains(TriId))
			{
				LogIssue(TriangleErrors, ELogVerbosity::Warning,
					FString::Printf(TEXT("Triangle not in CellTriangles (triangle %d, cell %d)."), TriId, CellId));
			}

			const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriId);
			const FVector3d A = Mesh.GetVertex(Tri.A);
			const FVector3d B = Mesh.GetVertex(Tri.B);
			const FVector3d C = Mesh.GetVertex(Tri.C);
			const FVector3d Centroid = (A + B + C) / 3.0;
			const int32 ExpectedCell = FindCellForPoint(Centroid);
			if (ExpectedCell == FCellStructureData::InvalidCellId)
			{
				LogIssue(TriangleMismatchErrors, ELogVerbosity::Warning,
					FString::Printf(TEXT("Triangle centroid does not resolve to a cell (triangle %d)."), TriId));
			}
			else if (ExpectedCell != CellId)
			{
				LogIssue(TriangleMismatchErrors, ELogVerbosity::Warning,
					FString::Printf(TEXT("Triangle cell mismatch (triangle %d, expected %d, got %d)."),
						TriId, ExpectedCell, CellId));
			}
		}
	}

	// ===== CellTriangles validation =====
	if (Data.CellTriangles.Num() == CellCount)
	{
		for (int32 CellId = 0; CellId < CellCount; ++CellId)
		{
			const TArray<int32>& Triangles = Data.CellTriangles[CellId];
			for (int32 i = 0; i < Triangles.Num(); ++i)
			{
				const int32 TriId = Triangles[i];
				if (TriId < 0 || TriId >= MaxTriangleId || !Mesh.IsTriangle(TriId))
				{
					LogIssue(CellTriangleErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("Cell contains invalid triangle (cell %d, tri %d)."), CellId, TriId));
					continue;
				}
				if (Data.TriangleToCell.IsValidIndex(TriId) && Data.TriangleToCell[TriId] != CellId)
				{
					LogIssue(CellTriangleErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("CellTriangles mismatch (cell %d, tri %d, mapped %d)."),
							CellId, TriId, Data.TriangleToCell[TriId]));
				}
				if (i > 0 && Triangles[i] == Triangles[i - 1])
				{
					LogIssue(CellTriangleErrors, ELogVerbosity::Warning,
						FString::Printf(TEXT("CellTriangles duplicate (cell %d, tri %d)."), CellId, TriId));
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("CellStructure validation summary: issues=%d sizeMismatch=%d insideVoxel=%d outsideVoxel=%d seeds=%d neighbors=%d triInvalid=%d triMismatch=%d cellTris=%d"),
		IssueCount, SizeMismatchErrors, InsideVoxelErrors, OutsideVoxelErrors, SeedErrors, NeighborErrors,
		TriangleErrors, TriangleMismatchErrors, CellTriangleErrors);

	// Draw debug visualization if requested
	if (World && IssueCount > 0)
	{
		FCellStructureDebugOptions DebugOptions;
		DebugOptions.bDrawAllVoxels = false;
		DebugOptions.bDrawCellBoundaries = false;
		DebugOptions.bDrawNeighborConnections = false;
		DebugOptions.bDrawErrors = true;
		DebugOptions.MaxDrawCount = MaxDrawCount;
		DebugDrawCellStructure(Mesh, Settings, Data, DebugOptions, World, DebugTransform);
	}

	return IssueCount == 0;
}

void FCellStructureBuilder::DebugDrawCellStructure(const UE::Geometry::FDynamicMesh3& Mesh,
	const FCellStructureSettings& Settings,
	const FCellStructureData& Data,
	const FCellStructureDebugOptions& DebugOptions,
	UWorld* World,
	const FTransform& DebugTransform) const
{
	if (!World || !Data.IsValid())
	{
		return;
	}

	const int32 CellCount = Data.CellSeedVoxels.Num();
	if (CellCount <= 0)
	{
		return;
	}

	const int32 VoxelCount = Data.VoxelResolution.X * Data.VoxelResolution.Y * Data.VoxelResolution.Z;
	const bool bPersistentLines = !World->IsGameWorld();
	const float DrawDuration = DebugOptions.DrawDuration;
	int32 DrawCount = 0;

	// Generate unique color for each cell using HSV color wheel
	auto GetCellColor = [CellCount](int32 CellId) -> FColor
	{
		if (CellId < 0 || CellId == FCellStructureData::InvalidCellId)
		{
			return FColor::Black;
		}
		// Distribute hues evenly across the color wheel
		const float Hue = FMath::Fmod(static_cast<float>(CellId) * 137.508f, 360.0f); // Golden angle
		const float Saturation = 0.7f;
		const float Value = 0.9f;
		return FLinearColor::MakeFromHSV8(
			static_cast<uint8>(Hue / 360.0f * 255.0f),
			static_cast<uint8>(Saturation * 255.0f),
			static_cast<uint8>(Value * 255.0f)).ToFColor(true);
	};

	// Blend two colors
	auto BlendColors = [](const FColor& A, const FColor& B) -> FColor
	{
		return FColor(
			(A.R + B.R) / 2,
			(A.G + B.G) / 2,
			(A.B + B.B) / 2,
			255);
	};

	auto GetVoxelCenter = [&](const FIntVector& Coord) -> FVector
	{
		return Data.GridOrigin + FVector(
			(static_cast<float>(Coord.X) + 0.5f) * Data.VoxelSize,
			(static_cast<float>(Coord.Y) + 0.5f) * Data.VoxelSize,
			(static_cast<float>(Coord.Z) + 0.5f) * Data.VoxelSize);
	};

	auto GetCoordFromIndex = [&](int32 Index) -> FIntVector
	{
		const int32 X = Index % Data.VoxelResolution.X;
		const int32 Y = (Index / Data.VoxelResolution.X) % Data.VoxelResolution.Y;
		const int32 Z = Index / (Data.VoxelResolution.X * Data.VoxelResolution.Y);
		return FIntVector(X, Y, Z);
	};

	TArray<FIntVector> NeighborOffsets;
	BuildNeighborOffsets(Settings.NeighborMode, NeighborOffsets);

	// ===== Draw Cell Boundaries =====
	if (DebugOptions.bDrawCellBoundaries && Data.VoxelCellIds.Num() == VoxelCount)
	{
		const float BoxExtent = Data.VoxelSize * 0.45f; // Slightly smaller than voxel to show gaps
		const FVector ScaledBoxExtent = DebugTransform.GetScale3D() * BoxExtent;

		for (int32 Index = 0; Index < VoxelCount && DrawCount < DebugOptions.MaxDrawCount; ++Index)
		{
			const int32 CellId = Data.VoxelCellIds[Index];
			if (CellId == FCellStructureData::InvalidCellId)
			{
				continue;
			}

			const FIntVector Coord = GetCoordFromIndex(Index);
			const FVector Center = GetVoxelCenter(Coord);
			const FVector WorldCenter = DebugTransform.TransformPosition(Center);

			// Check if this voxel is on a cell boundary
			bool bIsBoundary = false;
			int32 NeighborCellId = FCellStructureData::InvalidCellId;

			for (const FIntVector& Offset : NeighborOffsets)
			{
				const int32 Nx = Coord.X + Offset.X;
				const int32 Ny = Coord.Y + Offset.Y;
				const int32 Nz = Coord.Z + Offset.Z;

				if (Nx < 0 || Ny < 0 || Nz < 0 ||
					Nx >= Data.VoxelResolution.X ||
					Ny >= Data.VoxelResolution.Y ||
					Nz >= Data.VoxelResolution.Z)
				{
					bIsBoundary = true; // Edge of grid
					continue;
				}

				const int32 NIndex = Data.GetVoxelIndex(FIntVector(Nx, Ny, Nz));
				const int32 NCellId = Data.VoxelCellIds.IsValidIndex(NIndex)
					? Data.VoxelCellIds[NIndex]
					: FCellStructureData::InvalidCellId;

				if (NCellId != CellId)
				{
					bIsBoundary = true;
					if (NCellId != FCellStructureData::InvalidCellId && NeighborCellId == FCellStructureData::InvalidCellId)
					{
						NeighborCellId = NCellId;
					}
				}
			}

			// Draw all voxels or only boundary voxels based on option
			if (DebugOptions.bDrawAllVoxels || bIsBoundary)
			{
				FColor VoxelColor = GetCellColor(CellId);

				// Blend with neighbor cell color if on inter-cell boundary
				if (bIsBoundary && NeighborCellId != FCellStructureData::InvalidCellId)
				{
					VoxelColor = BlendColors(VoxelColor, GetCellColor(NeighborCellId));
				}

				DrawDebugBox(World, WorldCenter, ScaledBoxExtent, DebugTransform.GetRotation(), VoxelColor, bPersistentLines, DrawDuration, SDPG_Foreground);
				++DrawCount;
			}
		}
	}

	// ===== Draw Neighbor Connections =====
	if (DebugOptions.bDrawNeighborConnections && Data.CellNeighbors.Num() == CellCount)
	{
		for (int32 CellId = 0; CellId < CellCount && DrawCount < DebugOptions.MaxDrawCount; ++CellId)
		{
			const FIntVector SeedCoord = Data.CellSeedVoxels[CellId];
			const FVector SeedCenter = GetVoxelCenter(SeedCoord);
			const FVector WorldSeedCenter = DebugTransform.TransformPosition(SeedCenter);
			const FColor CellColor = GetCellColor(CellId);

			for (int32 NeighborId : Data.CellNeighbors[CellId])
			{
				// Only draw each connection once (when CellId < NeighborId)
				if (NeighborId <= CellId || NeighborId >= CellCount)
				{
					continue;
				}

				const FIntVector NeighborSeedCoord = Data.CellSeedVoxels[NeighborId];
				const FVector NeighborSeedCenter = GetVoxelCenter(NeighborSeedCoord);
				const FVector WorldNeighborCenter = DebugTransform.TransformPosition(NeighborSeedCenter);
				const FColor BlendedColor = BlendColors(CellColor, GetCellColor(NeighborId));

				DrawDebugLine(World, WorldSeedCenter, WorldNeighborCenter, BlendedColor,
					bPersistentLines, DrawDuration, SDPG_Foreground, 2.0f);
				++DrawCount;
			}
		}
	}

	// ===== Draw Errors =====
	if (DebugOptions.bDrawErrors && Data.VoxelInsideMask.Num() == VoxelCount && Data.VoxelCellIds.Num() == VoxelCount)
	{
		// Inside voxels without valid cell
		for (int32 Index = 0; Index < VoxelCount && DrawCount < DebugOptions.MaxDrawCount; ++Index)
		{
			const bool bInside = Data.VoxelInsideMask[Index] != 0;
			const int32 CellId = Data.VoxelCellIds[Index];

			if (bInside && (CellId == FCellStructureData::InvalidCellId || CellId < 0 || CellId >= CellCount))
			{
				const FIntVector Coord = GetCoordFromIndex(Index);
				const FVector Center = GetVoxelCenter(Coord);
				const FVector WorldCenter = DebugTransform.TransformPosition(Center);
				DrawDebugPoint(World, WorldCenter, 10.0f, FColor::Red, bPersistentLines, DrawDuration, SDPG_Foreground);
				++DrawCount;
			}
			else if (!bInside && CellId != FCellStructureData::InvalidCellId)
			{
				const FIntVector Coord = GetCoordFromIndex(Index);
				const FVector Center = GetVoxelCenter(Coord);
				const FVector WorldCenter = DebugTransform.TransformPosition(Center);
				DrawDebugPoint(World, WorldCenter, 10.0f, FColor::Blue, bPersistentLines, DrawDuration, SDPG_Foreground);
				++DrawCount;
			}
		}

		// Seed errors
		for (int32 CellId = 0; CellId < CellCount && DrawCount < DebugOptions.MaxDrawCount; ++CellId)
		{
			const FIntVector Seed = Data.CellSeedVoxels[CellId];
			const int32 Index = Data.GetVoxelIndex(Seed);
			const bool bIndexValid = Index >= 0 && Index < VoxelCount;
			const bool bInside = bIndexValid && Data.VoxelInsideMask.IsValidIndex(Index) && Data.VoxelInsideMask[Index] != 0;

			if (!bInside || (Data.VoxelCellIds.IsValidIndex(Index) && Data.VoxelCellIds[Index] != CellId))
			{
				const FVector Center = GetVoxelCenter(Seed);
				const FVector WorldCenter = DebugTransform.TransformPosition(Center);
				DrawDebugPoint(World, WorldCenter, 12.0f, FColor::Yellow, bPersistentLines, DrawDuration, SDPG_Foreground);
				++DrawCount;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("DebugDrawCellStructure: drew %d elements (cells=%d, maxDraw=%d)"),
		DrawCount, CellCount, DebugOptions.MaxDrawCount);
}
