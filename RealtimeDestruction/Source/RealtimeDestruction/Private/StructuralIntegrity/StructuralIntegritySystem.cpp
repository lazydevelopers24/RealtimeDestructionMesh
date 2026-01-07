// Fill out your copyright notice in the Description page of Project Settings.

#include "StructuralIntegrity/StructuralIntegritySystem.h"
#include "Async/ParallelFor.h"

//=========================================================================
// 초기화
//=========================================================================

void FStructuralIntegritySystem::Initialize(const FStructuralIntegrityInitData& InitData, const FStructuralIntegritySettings& InSettings)
{
	FWriteScopeLock WriteLock(DataLock);

	if (!InitData.IsValid())
	{
		return;
	}

	// 연결성 그래프 복사
	CellNeighbors = InitData.CellNeighbors;

	Settings = InSettings;

	const int32 CellCount = InitData.GetCellCount();
	Data.Initialize(CellCount);

	// Anchor 설정 (CellGraph에서 판정된 결과)
	for (int32 AnchorId : InitData.AnchorCellIds)
	{
		if (Data.IsValidCellId(AnchorId))
		{
			Data.AnchorCellIds.Add(AnchorId);
		}
	}

	NextGroupId = 0;
	bInitialized = true;
}

void FStructuralIntegritySystem::Reset()
{
	FWriteScopeLock WriteLock(DataLock);

	Data.Reset();
	CellNeighbors.Reset();
	KeyToId.Reset();
	IdToKey.Reset();
	NextInternalId = 0;
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
// Cell 파괴
//=========================================================================

FStructuralIntegrityResult FStructuralIntegritySystem::DestroyCells(const TArray<int32>& CellIds)
{
	FStructuralIntegrityResult Result;

	FWriteScopeLock WriteLock(DataLock);

	if (!bInitialized)
	{
		return Result;
	}

	// Cell들 파괴 처리
	for (int32 CellId : CellIds)
	{
		if (DestroyCellInternal(CellId))
		{
			Result.NewlyDestroyedCellIds.Add(CellId);
		}
	}

	// 새로 파괴된 Cell이 없으면 연결성 체크 불필요
	if (Result.NewlyDestroyedCellIds.Num() == 0)
	{
		return Result;
	}

	// 연결성 업데이트 및 분리 그룹 찾기
	Result.DetachedGroups = UpdateConnectivityAndFindDetached();

	// 전체 붕괴 체크 (모든 Anchor가 파괴됨)
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

FStructuralIntegrityResult FStructuralIntegritySystem::DestroyCell(int32 CellId)
{
	TArray<int32> SingleCell;
	SingleCell.Add(CellId);
	return DestroyCells(SingleCell);
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
	// 실제 계산은 DestroyCells에서 수행됨
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

bool FStructuralIntegritySystem::DestroyCellInternal(int32 CellId)
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
	Data.DestroyedCellIds.Add(CellId);
	Data.InvalidateCache();

	return true;
}

TArray<FDetachedCellGroup> FStructuralIntegritySystem::UpdateConnectivityAndFindDetached()
{
	// Lock은 호출자가 이미 잡았다고 가정

	if (CellNeighbors.Num() == 0)
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

	if (CellNeighbors.Num() == 0 || Data.AnchorCellIds.Num() == 0)
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
		if (CurrentCellId < CellNeighbors.Num())
		{
			const TArray<int32>& Neighbors = CellNeighbors[CurrentCellId];

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

	if (DetachedCellIds.Num() == 0 || CellNeighbors.Num() == 0)
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
			if (CurrentId < CellNeighbors.Num())
			{
				const TArray<int32>& Neighbors = CellNeighbors[CurrentId];

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

		// 그룹 속성: CellIds만 설정
		// CenterOfMass, TriangleIds는 CellGraph에서 조회하여 상위 레벨에서 채움
		Group.CellIds.Sort(); // 결정론적 순서
		Group.ApproximateMass = static_cast<float>(Group.CellIds.Num());

		Groups.Add(MoveTemp(Group));
	}

	return Groups;
}

//=========================================================================
// 그래프 동기화 API (신규)
//=========================================================================

void FStructuralIntegritySystem::SyncGraph(const FStructuralIntegrityGraphSnapshot& Snapshot)
{
	FWriteScopeLock WriteLock(DataLock);

	if (!Snapshot.IsValid())
	{
		return;
	}

	// 1. 스냅샷에 없는 기존 Key -> Destroyed 마킹
	TSet<FCellKey> SnapshotKeySet;
	for (const FCellKey& Key : Snapshot.NodeKeys)
	{
		SnapshotKeySet.Add(Key);
	}

	for (const auto& Pair : KeyToId)
	{
		const FCellKey& ExistingKey = Pair.Key;
		int32 CellId = Pair.Value;

		if (!SnapshotKeySet.Contains(ExistingKey))
		{
			// 스냅샷에 없음 -> Destroyed로 마킹
			if (CellId < Data.CellStates.Num())
			{
				Data.CellStates[CellId] = ECellStructuralState::Destroyed;
				Data.DestroyedCellIds.Add(CellId);
			}
		}
	}

	// 2. 스냅샷의 각 Key에 대해 ID 할당/조회
	for (const FCellKey& Key : Snapshot.NodeKeys)
	{
		FindOrAllocateCellId(Key, true);
	}

	// 3. CellStates 배열 확장 (새 ID가 할당되었을 수 있음)
	const int32 RequiredSize = NextInternalId;
	if (Data.CellStates.Num() < RequiredSize)
	{
		const int32 OldSize = Data.CellStates.Num();
		Data.CellStates.SetNum(RequiredSize);
		for (int32 i = OldSize; i < RequiredSize; ++i)
		{
			Data.CellStates[i] = ECellStructuralState::Intact;
		}
	}

	// 4. Anchor 집합 갱신
	Data.AnchorCellIds.Reset();
	for (const FCellKey& AnchorKey : Snapshot.AnchorKeys)
	{
		const int32* FoundId = KeyToId.Find(AnchorKey);
		if (FoundId)
		{
			Data.AnchorCellIds.Add(*FoundId);
		}
	}

	// 5. 이웃 리스트 재구축
	RebuildNeighborLists(Snapshot);

	// 6. 캐시 무효화
	Data.InvalidateCache();

	bInitialized = true;
}

FStructuralIntegrityResult FStructuralIntegritySystem::RefreshConnectivity()
{
	FStructuralIntegrityResult Result;

	FWriteScopeLock WriteLock(DataLock);

	if (!bInitialized || CellNeighbors.Num() == 0)
	{
		return Result;
	}

	// 1. Anchor로부터 연결된 모든 Cell 찾기 (BFS)
	TSet<int32> ConnectedCells = FindAllConnectedToAnchors_Internal();

	// 2. 캐시 업데이트
	Data.ConnectedToAnchorCache = ConnectedCells;
	Data.bCacheValid = true;

	// 3. 분리된 Cell 찾기 (파괴되지 않았고 연결되지 않은 Cell)
	TArray<int32> DetachedCellIds;
	const int32 CellCount = Data.CellStates.Num();

	for (int32 CellId = 0; CellId < CellCount; ++CellId)
	{
		if (!Data.DestroyedCellIds.Contains(CellId) && !ConnectedCells.Contains(CellId))
		{
			Data.CellStates[CellId] = ECellStructuralState::Detached;
			DetachedCellIds.Add(CellId);
		}
	}

	if (DetachedCellIds.Num() == 0)
	{
		return Result;
	}

	// 4. 분리된 Cell들을 연결 그룹으로 묶기
	Result.DetachedGroups = BuildDetachedGroups(DetachedCellIds);

	// 5. 각 그룹에 CellKeys 채우기
	for (FDetachedCellGroup& Group : Result.DetachedGroups)
	{
		Group.CellKeys.Reserve(Group.CellIds.Num());
		for (int32 CellId : Group.CellIds)
		{
			if (CellId < IdToKey.Num())
			{
				Group.CellKeys.Add(IdToKey[CellId]);
			}
		}
	}

	// 6. 전체 붕괴 체크
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

void FStructuralIntegritySystem::MarkCellsAsDestroyed(const TArray<FCellKey>& Keys)
{
	FWriteScopeLock WriteLock(DataLock);

	for (const FCellKey& Key : Keys)
	{
		const int32* FoundId = KeyToId.Find(Key);
		if (FoundId && *FoundId < Data.CellStates.Num())
		{
			Data.CellStates[*FoundId] = ECellStructuralState::Destroyed;
			Data.DestroyedCellIds.Add(*FoundId);
		}
	}

	Data.InvalidateCache();
}

//=========================================================================
// Key 기반 조회 API
//=========================================================================

int32 FStructuralIntegritySystem::GetCellIdForKey(const FCellKey& Key) const
{
	FReadScopeLock ReadLock(DataLock);

	const int32* FoundId = KeyToId.Find(Key);
	return FoundId ? *FoundId : INDEX_NONE;
}

FCellKey FStructuralIntegritySystem::GetKeyForCellId(int32 CellId) const
{
	FReadScopeLock ReadLock(DataLock);

	if (CellId >= 0 && CellId < IdToKey.Num())
	{
		return IdToKey[CellId];
	}
	return FCellKey();
}

TArray<FCellKey> FStructuralIntegritySystem::GetDestroyedCellKeys() const
{
	FReadScopeLock ReadLock(DataLock);

	TArray<FCellKey> DestroyedKeys;
	DestroyedKeys.Reserve(Data.DestroyedCellIds.Num());

	for (int32 CellId : Data.DestroyedCellIds)
	{
		if (CellId < IdToKey.Num())
		{
			DestroyedKeys.Add(IdToKey[CellId]);
		}
	}

	return DestroyedKeys;
}

//=========================================================================
// 내부 헬퍼 (신규)
//=========================================================================

int32 FStructuralIntegritySystem::FindOrAllocateCellId(const FCellKey& Key, bool bCreateIfNotFound)
{
	// Lock은 호출자가 이미 잡았다고 가정

	if (!Key.IsValid())
	{
		return INDEX_NONE;
	}

	const int32* FoundId = KeyToId.Find(Key);
	if (FoundId)
	{
		return *FoundId;
	}

	if (!bCreateIfNotFound)
	{
		return INDEX_NONE;
	}

	// 새 ID 할당 (단조 증가)
	const int32 NewId = NextInternalId++;

	KeyToId.Add(Key, NewId);

	// IdToKey 배열 확장
	if (IdToKey.Num() <= NewId)
	{
		IdToKey.SetNum(NewId + 1);
	}
	IdToKey[NewId] = Key;

	return NewId;
}

void FStructuralIntegritySystem::RebuildNeighborLists(const FStructuralIntegrityGraphSnapshot& Snapshot)
{
	// Lock은 호출자가 이미 잡았다고 가정

	// CellNeighbors 크기 맞추기
	const int32 RequiredSize = NextInternalId;
	CellNeighbors.SetNum(RequiredSize);

	// 기존 이웃 리스트 초기화
	for (TArray<int32>& Neighbors : CellNeighbors)
	{
		Neighbors.Reset();
	}

	// 스냅샷에서 이웃 정보 변환
	for (int32 NodeIndex = 0; NodeIndex < Snapshot.NodeKeys.Num(); ++NodeIndex)
	{
		const FCellKey& NodeKey = Snapshot.NodeKeys[NodeIndex];
		const int32* NodeId = KeyToId.Find(NodeKey);
		if (!NodeId)
		{
			continue;
		}

		const TArray<FCellKey>& NeighborKeys = Snapshot.NeighborKeys[NodeIndex].Neighbors;
		TArray<int32>& NeighborIds = CellNeighbors[*NodeId];
		NeighborIds.Reserve(NeighborKeys.Num());

		for (const FCellKey& NeighborKey : NeighborKeys)
		{
			const int32* NeighborId = KeyToId.Find(NeighborKey);
			if (NeighborId)
			{
				NeighborIds.Add(*NeighborId);
			}
		}

		// 결정론적 순서 보장
		NeighborIds.Sort();
	}
}
