// Fill out your copyright notice in the Description page of Project Settings.

#include "StructuralIntegrity/StructuralIntegritySystem.h"
#include "Async/ParallelFor.h"

//=========================================================================
// 초기화
//=========================================================================

void FStructuralIntegritySystem::Initialize(const FCellStructureData& CellData, const FStructuralIntegritySettings& InSettings)
{
	FWriteScopeLock WriteLock(DataLock);

	CellDataPtr = &CellData;
	Settings = InSettings;

	const int32 CellCount = CellData.CellSeedVoxels.Num();
	Data.Initialize(CellCount, Settings.DefaultCellHealth);

	NextGroupId = 0;
	bInitialized = true;

	// 자동 Anchor 감지
	if (Settings.bAutoDetectFloorAnchors)
	{
		// Lock 이미 잡혀있으므로 내부 버전 호출 불필요
		// AutoDetectFloorAnchors는 별도 Lock을 잡으므로 여기서 직접 처리

		if (!CellDataPtr || CellCount == 0)
		{
			return;
		}

		// 가장 낮은 Z 좌표 찾기
		float MinZ = MAX_flt;
		for (int32 CellId = 0; CellId < CellCount; ++CellId)
		{
			const FIntVector& SeedVoxel = CellDataPtr->CellSeedVoxels[CellId];
			const FVector WorldPos = VoxelToWorld(SeedVoxel);
			MinZ = FMath::Min(MinZ, WorldPos.Z);
		}

		// 임계값 이내의 Cell을 Anchor로 설정
		// FloorHeightThreshold가 1.0 이하면 VoxelSize의 배수로 해석
		const float ActualThreshold = (Settings.FloorHeightThreshold <= 1.0f)
			? CellDataPtr->VoxelSize * Settings.FloorHeightThreshold
			: Settings.FloorHeightThreshold;

		for (int32 CellId = 0; CellId < CellCount; ++CellId)
		{
			const FIntVector& SeedVoxel = CellDataPtr->CellSeedVoxels[CellId];
			const FVector WorldPos = VoxelToWorld(SeedVoxel);

			if (WorldPos.Z - MinZ <= ActualThreshold)
			{
				Data.AnchorCellIds.Add(CellId);
			}
		}

		Data.InvalidateCache();
	}
}

void FStructuralIntegritySystem::Reset()
{
	FWriteScopeLock WriteLock(DataLock);

	Data.Reset();
	CellDataPtr = nullptr;
	bInitialized = false;
	NextGroupId = 0;
}

int32 FStructuralIntegritySystem::GetCellCount() const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.GetCellCount();
}

//=========================================================================
// Anchor 관리
//=========================================================================

void FStructuralIntegritySystem::SetAnchor(int32 CellId, bool bIsAnchor)
{
	FWriteScopeLock WriteLock(DataLock);

	if (!Data.IsValidCellId(CellId))
	{
		return;
	}

	if (bIsAnchor)
	{
		Data.AnchorCellIds.Add(CellId);
	}
	else
	{
		Data.AnchorCellIds.Remove(CellId);
	}

	Data.InvalidateCache();
}

void FStructuralIntegritySystem::SetAnchors(const TArray<int32>& CellIds, bool bIsAnchor)
{
	FWriteScopeLock WriteLock(DataLock);

	for (int32 CellId : CellIds)
	{
		if (Data.IsValidCellId(CellId))
		{
			if (bIsAnchor)
			{
				Data.AnchorCellIds.Add(CellId);
			}
			else
			{
				Data.AnchorCellIds.Remove(CellId);
			}
		}
	}

	Data.InvalidateCache();
}

void FStructuralIntegritySystem::AutoDetectFloorAnchors(float HeightThreshold)
{
	FWriteScopeLock WriteLock(DataLock);

	if (!CellDataPtr || Data.GetCellCount() == 0)
	{
		return;
	}

	const int32 CellCount = Data.GetCellCount();

	// 가장 낮은 Z 좌표 찾기
	float MinZ = MAX_flt;
	for (int32 CellId = 0; CellId < CellCount; ++CellId)
	{
		const FIntVector& SeedVoxel = CellDataPtr->CellSeedVoxels[CellId];
		const FVector WorldPos = VoxelToWorld(SeedVoxel);
		MinZ = FMath::Min(MinZ, WorldPos.Z);
	}

	// 기존 Anchor 초기화
	Data.AnchorCellIds.Reset();

	// 임계값 이내의 Cell을 Anchor로 설정
	// HeightThreshold가 1.0 이하면 VoxelSize의 배수로 해석
	const float ActualThreshold = (HeightThreshold <= 1.0f)
		? CellDataPtr->VoxelSize * HeightThreshold
		: HeightThreshold;

	for (int32 CellId = 0; CellId < CellCount; ++CellId)
	{
		const FIntVector& SeedVoxel = CellDataPtr->CellSeedVoxels[CellId];
		const FVector WorldPos = VoxelToWorld(SeedVoxel);

		if (WorldPos.Z - MinZ <= ActualThreshold)
		{
			Data.AnchorCellIds.Add(CellId);
		}
	}

	Data.InvalidateCache();
}

TArray<int32> FStructuralIntegritySystem::GetAnchorCellIds() const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.AnchorCellIds.Array();
}

bool FStructuralIntegritySystem::IsAnchor(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.AnchorCellIds.Contains(CellId);
}

int32 FStructuralIntegritySystem::GetAnchorCount() const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.AnchorCellIds.Num();
}

//=========================================================================
// Hit 처리
//=========================================================================

FStructuralIntegrityResult FStructuralIntegritySystem::ProcessHit(int32 HitCellId, float Damage, int32 DamageRadius)
{
	FStructuralIntegrityResult Result;

	FWriteScopeLock WriteLock(DataLock);

	if (!bInitialized || !Data.IsValidCellId(HitCellId))
	{
		return Result;
	}

	// 이미 파괴된 Cell에 맞았으면 무시
	if (Data.DestroyedCellIds.Contains(HitCellId))
	{
		return Result;
	}

	// 1. 데미지 적용
	ApplyDamage(HitCellId, Damage, DamageRadius, Result.NewlyDestroyedCellIds);

	// 2. 새로 파괴된 Cell이 없으면 연결성 체크 불필요
	if (Result.NewlyDestroyedCellIds.Num() == 0)
	{
		return Result;
	}

	// 3. 연결성 업데이트 및 분리 그룹 찾기
	Result.DetachedGroups = UpdateConnectivityAndFindDetached();

	// 4. 전체 붕괴 체크 (모든 Anchor가 파괴됨)
	bool bAllAnchorsDestroyed = true;
	for (int32 AnchorId : Data.AnchorCellIds)
	{
		if (!Data.DestroyedCellIds.Contains(AnchorId))
		{
			bAllAnchorsDestroyed = false;
			break;
		}
	}
	Result.bStructureCollapsed = bAllAnchorsDestroyed && Data.AnchorCellIds.Num() > 0;

	Result.TotalDestroyedCount = Data.DestroyedCellIds.Num();

	return Result;
}

int32 FStructuralIntegritySystem::FindCellAtLocation(const FVector& WorldLocation) const
{
	FReadScopeLock ReadLock(DataLock);

	if (!CellDataPtr || !CellDataPtr->IsValid())
	{
		return INDEX_NONE;
	}

	// 월드 좌표를 Voxel 좌표로 변환
	const FVector LocalPos = WorldLocation - CellDataPtr->GridOrigin;
	const float VoxelSize = CellDataPtr->VoxelSize;

	const int32 VoxelX = FMath::FloorToInt(LocalPos.X / VoxelSize);
	const int32 VoxelY = FMath::FloorToInt(LocalPos.Y / VoxelSize);
	const int32 VoxelZ = FMath::FloorToInt(LocalPos.Z / VoxelSize);

	// 범위 체크
	if (VoxelX < 0 || VoxelX >= CellDataPtr->VoxelResolution.X ||
		VoxelY < 0 || VoxelY >= CellDataPtr->VoxelResolution.Y ||
		VoxelZ < 0 || VoxelZ >= CellDataPtr->VoxelResolution.Z)
	{
		return INDEX_NONE;
	}

	const int32 VoxelIndex = CellDataPtr->GetVoxelIndex(FIntVector(VoxelX, VoxelY, VoxelZ));

	if (VoxelIndex < 0 || VoxelIndex >= CellDataPtr->VoxelCellIds.Num())
	{
		return INDEX_NONE;
	}

	return CellDataPtr->VoxelCellIds[VoxelIndex];
}

FStructuralIntegrityResult FStructuralIntegritySystem::ProcessHitAtLocation(const FVector& WorldLocation, float Damage, int32 DamageRadius)
{
	const int32 CellId = FindCellAtLocation(WorldLocation);

	if (CellId == INDEX_NONE)
	{
		return FStructuralIntegrityResult();
	}

	return ProcessHit(CellId, Damage, DamageRadius);
}

//=========================================================================
// 상태 조회
//=========================================================================

ECellStructuralState FStructuralIntegritySystem::GetCellState(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);

	if (!Data.IsValidCellId(CellId))
	{
		return ECellStructuralState::Destroyed;
	}

	return Data.CellStates[CellId];
}

float FStructuralIntegritySystem::GetCellHealth(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);

	if (!Data.IsValidCellId(CellId))
	{
		return 0.0f;
	}

	return Data.CellHealth[CellId];
}

float FStructuralIntegritySystem::GetCellHealthNormalized(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.GetHealthNormalized(CellId);
}

bool FStructuralIntegritySystem::IsCellConnectedToAnchor(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);

	if (!Data.IsValidCellId(CellId))
	{
		return false;
	}

	if (Data.DestroyedCellIds.Contains(CellId))
	{
		return false;
	}

	// 캐시가 유효하면 사용
	if (Data.bCacheValid)
	{
		return Data.ConnectedToAnchorCache.Contains(CellId);
	}

	// 캐시가 무효하면 전체 재계산 필요 (쓰기 작업)
	// 읽기 전용 메서드이므로 여기서는 계산하지 않고 보수적으로 true 반환
	// 실제 계산은 ProcessHit에서 수행됨
	return true;
}

int32 FStructuralIntegritySystem::GetDestroyedCellCount() const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.DestroyedCellIds.Num();
}

TArray<int32> FStructuralIntegritySystem::GetDestroyedCellIds() const
{
	FReadScopeLock ReadLock(DataLock);
	return Data.DestroyedCellIds.Array();
}

FVector FStructuralIntegritySystem::GetCellWorldPosition(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);

	if (!CellDataPtr || !Data.IsValidCellId(CellId))
	{
		return FVector::ZeroVector;
	}

	const FIntVector& SeedVoxel = CellDataPtr->CellSeedVoxels[CellId];
	return VoxelToWorld(SeedVoxel);
}

//=========================================================================
// 강제 상태 설정
//=========================================================================

TArray<FDetachedCellGroup> FStructuralIntegritySystem::ForceSetDestroyedCells(const TArray<int32>& DestroyedIds)
{
	FWriteScopeLock WriteLock(DataLock);

	// 모든 파괴된 Cell 적용
	for (int32 CellId : DestroyedIds)
	{
		if (Data.IsValidCellId(CellId))
		{
			Data.CellStates[CellId] = ECellStructuralState::Destroyed;
			Data.CellHealth[CellId] = 0.0f;
			Data.DestroyedCellIds.Add(CellId);
		}
	}

	Data.InvalidateCache();

	// 분리 그룹 반환 (파편은 이미 떨어졌으므로 스폰하지 않을 수 있음)
	return UpdateConnectivityAndFindDetached();
}

void FStructuralIntegritySystem::SetSettings(const FStructuralIntegritySettings& NewSettings)
{
	FWriteScopeLock WriteLock(DataLock);
	Settings = NewSettings;
}

//=========================================================================
// 내부 알고리즘
//=========================================================================

void FStructuralIntegritySystem::ApplyDamage(int32 CenterCellId, float Damage, int32 Radius, TArray<int32>& OutNewlyDestroyed)
{
	// 반경 내 Cell들 찾기
	TArray<TPair<int32, int32>> CellsWithDistance;
	BFSFindCellsInRadius(CenterCellId, Radius, CellsWithDistance);

	// 결정론적 순서 보장: CellId 기준 정렬
	CellsWithDistance.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B)
	{
		return A.Key < B.Key;
	});

	// 데미지 적용
	for (const auto& Pair : CellsWithDistance)
	{
		const int32 CellId = Pair.Key;
		const int32 Distance = Pair.Value;

		// 이미 파괴됨
		if (Data.DestroyedCellIds.Contains(CellId))
		{
			continue;
		}

		// 거리에 따른 데미지 감쇠
		float AppliedDamage = Damage;
		if (Distance > 0 && Settings.DamageFalloff > 0.0f)
		{
			AppliedDamage *= FMath::Pow(1.0f - Settings.DamageFalloff, static_cast<float>(Distance));
		}

		// 체력 감소
		Data.CellHealth[CellId] = FMath::Max(0.0f, Data.CellHealth[CellId] - AppliedDamage);

		// 파괴 처리
		if (Data.CellHealth[CellId] <= 0.0f)
		{
			if (DestroyCell(CellId))
			{
				OutNewlyDestroyed.Add(CellId);
			}
		}
		else if (Data.CellStates[CellId] == ECellStructuralState::Intact)
		{
			Data.CellStates[CellId] = ECellStructuralState::Damaged;
		}
	}
}

bool FStructuralIntegritySystem::DestroyCell(int32 CellId)
{
	if (!Data.IsValidCellId(CellId))
	{
		return false;
	}

	if (Data.DestroyedCellIds.Contains(CellId))
	{
		return false;
	}

	Data.CellStates[CellId] = ECellStructuralState::Destroyed;
	Data.CellHealth[CellId] = 0.0f;
	Data.DestroyedCellIds.Add(CellId);
	Data.InvalidateCache();

	return true;
}

TArray<FDetachedCellGroup> FStructuralIntegritySystem::UpdateConnectivityAndFindDetached()
{
	// Lock은 호출자가 이미 잡았다고 가정

	if (!CellDataPtr)
	{
		return TArray<FDetachedCellGroup>();
	}

	// 1. Anchor로부터 연결된 모든 Cell 찾기
	TSet<int32> ConnectedCells = FindAllConnectedToAnchors_Internal();

	// 2. 캐시 업데이트
	Data.ConnectedToAnchorCache = ConnectedCells;
	Data.bCacheValid = true;

	// 3. 분리된 Cell 찾기
	TArray<int32> DetachedCellIds;
	const int32 CellCount = Data.GetCellCount();

	for (int32 CellId = 0; CellId < CellCount; ++CellId)
	{
		// 파괴되지 않았고 연결되지 않은 Cell
		if (!Data.DestroyedCellIds.Contains(CellId) && !ConnectedCells.Contains(CellId))
		{
			Data.CellStates[CellId] = ECellStructuralState::Detached;
			DetachedCellIds.Add(CellId);
		}
	}

	if (DetachedCellIds.Num() == 0)
	{
		return TArray<FDetachedCellGroup>();
	}

	// 4. 분리된 Cell들을 연결 그룹으로 묶기
	return BuildDetachedGroups(DetachedCellIds);
}

TSet<int32> FStructuralIntegritySystem::FindAllConnectedToAnchors_Internal() const
{
	TSet<int32> ConnectedCells;

	if (!CellDataPtr || Data.AnchorCellIds.Num() == 0)
	{
		return ConnectedCells;
	}

	// BFS 큐: (CellId)
	TArray<int32> Queue;
	Queue.Reserve(Data.GetCellCount());

	// 파괴되지 않은 Anchor들을 시작점으로 (결정론적 순서)
	TArray<int32> SortedAnchors = Data.AnchorCellIds.Array();
	SortedAnchors.Sort();

	for (int32 AnchorId : SortedAnchors)
	{
		if (!Data.DestroyedCellIds.Contains(AnchorId))
		{
			Queue.Add(AnchorId);
			ConnectedCells.Add(AnchorId);
		}
	}

	// BFS
	int32 QueueIndex = 0;
	while (QueueIndex < Queue.Num())
	{
		const int32 CurrentCellId = Queue[QueueIndex++];

		// 이웃 순회 (CellNeighbors는 이미 결정론적 순서)
		if (CurrentCellId < CellDataPtr->CellNeighbors.Num())
		{
			const TArray<int32>& Neighbors = CellDataPtr->CellNeighbors[CurrentCellId];

			for (int32 NeighborId : Neighbors)
			{
				// 파괴되지 않았고 아직 방문하지 않은 이웃
				if (!Data.DestroyedCellIds.Contains(NeighborId) && !ConnectedCells.Contains(NeighborId))
				{
					ConnectedCells.Add(NeighborId);
					Queue.Add(NeighborId);
				}
			}
		}
	}

	return ConnectedCells;
}

TArray<FDetachedCellGroup> FStructuralIntegritySystem::BuildDetachedGroups(const TArray<int32>& DetachedCellIds)
{
	TArray<FDetachedCellGroup> Groups;

	if (DetachedCellIds.Num() == 0 || !CellDataPtr)
	{
		return Groups;
	}

	// 방문 체크용 Set
	TSet<int32> Visited;
	TSet<int32> DetachedSet(DetachedCellIds);

	// 결정론적 순서를 위해 정렬된 Cell부터 시작
	TArray<int32> SortedDetached = DetachedCellIds;
	SortedDetached.Sort();

	for (int32 StartCellId : SortedDetached)
	{
		if (Visited.Contains(StartCellId))
		{
			continue;
		}

		// 새 그룹 시작 - BFS로 연결된 분리 Cell 모두 찾기
		FDetachedCellGroup Group;
		Group.GroupId = NextGroupId++;

		TArray<int32> GroupQueue;
		GroupQueue.Add(StartCellId);
		Visited.Add(StartCellId);

		int32 GroupQueueIndex = 0;
		while (GroupQueueIndex < GroupQueue.Num())
		{
			const int32 CurrentId = GroupQueue[GroupQueueIndex++];
			Group.CellIds.Add(CurrentId);

			// 이웃 중 분리된 Cell 찾기
			if (CurrentId < CellDataPtr->CellNeighbors.Num())
			{
				const TArray<int32>& Neighbors = CellDataPtr->CellNeighbors[CurrentId];

				for (int32 NeighborId : Neighbors)
				{
					if (DetachedSet.Contains(NeighborId) && !Visited.Contains(NeighborId))
					{
						Visited.Add(NeighborId);
						GroupQueue.Add(NeighborId);
					}
				}
			}
		}

		// 그룹 속성 계산
		Group.CellIds.Sort(); // 결정론적 순서
		Group.CenterOfMass = CalculateCenterOfMass(Group.CellIds);
		Group.ApproximateMass = static_cast<float>(Group.CellIds.Num());
		Group.TriangleIds = CollectTriangleIds(Group.CellIds);

		Groups.Add(MoveTemp(Group));
	}

	return Groups;
}

FVector FStructuralIntegritySystem::CalculateCenterOfMass(const TArray<int32>& CellIds) const
{
	if (CellIds.Num() == 0 || !CellDataPtr)
	{
		return FVector::ZeroVector;
	}

	FVector Sum = FVector::ZeroVector;

	for (int32 CellId : CellIds)
	{
		if (CellId >= 0 && CellId < CellDataPtr->CellSeedVoxels.Num())
		{
			const FIntVector& SeedVoxel = CellDataPtr->CellSeedVoxels[CellId];
			Sum += VoxelToWorld(SeedVoxel);
		}
	}

	return Sum / static_cast<float>(CellIds.Num());
}

TArray<int32> FStructuralIntegritySystem::CollectTriangleIds(const TArray<int32>& CellIds) const
{
	TArray<int32> TriangleIds;

	if (!CellDataPtr)
	{
		return TriangleIds;
	}

	for (int32 CellId : CellIds)
	{
		if (CellId >= 0 && CellId < CellDataPtr->CellTriangles.Num())
		{
			const TArray<int32>& CellTris = CellDataPtr->CellTriangles[CellId];
			TriangleIds.Append(CellTris);
		}
	}

	// 중복 제거 및 정렬 (결정론적)
	TriangleIds.Sort();

	// 중복 제거
	int32 WriteIndex = 0;
	for (int32 i = 0; i < TriangleIds.Num(); ++i)
	{
		if (i == 0 || TriangleIds[i] != TriangleIds[WriteIndex - 1])
		{
			TriangleIds[WriteIndex++] = TriangleIds[i];
		}
	}
	TriangleIds.SetNum(WriteIndex);

	return TriangleIds;
}

void FStructuralIntegritySystem::BFSFindCellsInRadius(int32 StartCellId, int32 MaxDistance,
	TArray<TPair<int32, int32>>& OutCellsWithDistance) const
{
	OutCellsWithDistance.Reset();

	if (!CellDataPtr || !Data.IsValidCellId(StartCellId))
	{
		return;
	}

	// BFS 큐: (CellId, Distance)
	TArray<TPair<int32, int32>> Queue;
	TSet<int32> Visited;

	Queue.Add(TPair<int32, int32>(StartCellId, 0));
	Visited.Add(StartCellId);

	int32 QueueIndex = 0;
	while (QueueIndex < Queue.Num())
	{
		const auto& Current = Queue[QueueIndex++];
		const int32 CellId = Current.Key;
		const int32 Distance = Current.Value;

		OutCellsWithDistance.Add(Current);

		// 최대 거리에 도달하면 이웃 탐색 중지
		if (Distance >= MaxDistance)
		{
			continue;
		}

		// 이웃 탐색
		if (CellId < CellDataPtr->CellNeighbors.Num())
		{
			const TArray<int32>& Neighbors = CellDataPtr->CellNeighbors[CellId];

			for (int32 NeighborId : Neighbors)
			{
				if (!Visited.Contains(NeighborId))
				{
					Visited.Add(NeighborId);
					Queue.Add(TPair<int32, int32>(NeighborId, Distance + 1));
				}
			}
		}
	}
}

FVector FStructuralIntegritySystem::VoxelToWorld(const FIntVector& VoxelCoord) const
{
	if (!CellDataPtr)
	{
		return FVector::ZeroVector;
	}

	const float VoxelSize = CellDataPtr->VoxelSize;
	const FVector& GridOrigin = CellDataPtr->GridOrigin;

	// Voxel 중심 좌표
	return GridOrigin + FVector(
		(static_cast<float>(VoxelCoord.X) + 0.5f) * VoxelSize,
		(static_cast<float>(VoxelCoord.Y) + 0.5f) * VoxelSize,
		(static_cast<float>(VoxelCoord.Z) + 0.5f) * VoxelSize
	);
}
