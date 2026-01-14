// Copyright 2025. All Rights Reserved.

#include "StructuralIntegrity/CellDestructionSystem.h"
#include "StructuralIntegrity/SubCellProcessor.h"

//=============================================================================
// FCellDestructionSystem - SubCell Level API
//=============================================================================

FDestructionResult FCellDestructionSystem::ProcessCellDestructionWithSubCells(
	const FGridCellCache& Cache,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform,
	FCellState& InOutCellState)
{
	FDestructionResult Result;

	if (!Cache.IsValid())
	{
		return Result;
	}

	// 1. SubCellProcessor를 통한 SubCell 파괴 처리
	TArray<int32> AffectedCells;
	TMap<int32, TArray<int32>> NewlyDeadSubCells;

	FSubCellProcessor::ProcessSubCellDestruction(
		Shape,
		MeshTransform,
		Cache,
		InOutCellState,
		AffectedCells,
		&NewlyDeadSubCells
	);

	// 2. 결과 정리
	Result.AffectedCells = MoveTemp(AffectedCells);

	// NewlyDeadSubCells (TMap<int32, TArray<int32>>) → FDestructionResult.NewlyDeadSubCells (TMap<int32, FIntArray>)
	for (auto& Pair : NewlyDeadSubCells)
	{
		FIntArray SubCellArray;
		SubCellArray.Values = MoveTemp(Pair.Value);
		Result.DeadSubCellCount += SubCellArray.Num();
		Result.NewlyDeadSubCells.Add(Pair.Key, MoveTemp(SubCellArray));
	}

	// 3. 완전히 파괴된 Cell 수집 (SubCellProcessor 내부에서 이미 DestroyedCells에 추가됨)
	for (int32 CellId : Result.AffectedCells)
	{
		if (InOutCellState.DestroyedCells.Contains(CellId))
		{
			Result.NewlyDestroyedCells.Add(CellId);
		}
	}

	return Result;
}

ECellDamageLevel FCellDestructionSystem::GetCellDamageLevel(int32 CellId, const FCellState& CellState)
{
	// Destroyed 확인
	if (CellState.DestroyedCells.Contains(CellId))
	{
		return ECellDamageLevel::Destroyed;
	}

	// SubCell 상태 확인
	const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);
	if (!SubCellState)
	{
		return ECellDamageLevel::Intact;  // SubCell 상태 없음 = 손상 없음
	}

	if (SubCellState->IsFullyDestroyed())
	{
		return ECellDamageLevel::Destroyed;
	}

	return ECellDamageLevel::Damaged;
}

//=============================================================================
// FCellDestructionSystem - 셀 파괴 판정 (기존 Cell Level API)
//=============================================================================

TArray<int32> FCellDestructionSystem::CalculateDestroyedCells(
	const FGridCellCache& Cache,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform,
	const TSet<int32>& DestroyedCells)
{
	TArray<int32> NewlyDestroyed;

	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); CellId++)
	{
		// 이미 파괴되었거나 존재하지 않는 셀은 스킵
		if (!Cache.GetCellExists(CellId) || DestroyedCells.Contains(CellId))
		{
			continue;
		}

		if (IsCellDestroyed(Cache, CellId, Shape, MeshTransform))
		{
			NewlyDestroyed.Add(CellId);
		}
	}

	return NewlyDestroyed;
}

bool FCellDestructionSystem::IsCellDestroyed(
	const FGridCellCache& Cache,
	int32 CellId,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform)
{
	// Phase 1: 중심점 검사 (빠른 판정)
	const FVector WorldCenter = Cache.IdToWorldCenter(CellId, MeshTransform);
	if (Shape.ContainsPoint(WorldCenter))
	{
		return true;
	}

	// Phase 2: 꼭지점 과반수 검사 (경계 케이스)
	const TArray<FVector> LocalVertices = Cache.GetCellVertices(CellId);
	int32 DestroyedVertices = 0;

	for (const FVector& LocalVertex : LocalVertices)
	{
		const FVector WorldVertex = MeshTransform.TransformPosition(LocalVertex);
		if (Shape.ContainsPoint(WorldVertex))
		{
			// 과반수 (4개) 도달 시 즉시 반환
			if (++DestroyedVertices >= 4)
			{
				return true;
			}
		}
	}

	return false;
}

//=============================================================================
// FCellDestructionSystem - 구조적 무결성 검사
//=============================================================================

TSet<int32> FCellDestructionSystem::FindDisconnectedCells(
	const FGridCellCache& Cache,
	const TSet<int32>& DestroyedCells)
{
	TSet<int32> Connected;
	TQueue<int32> Queue;

	// 1. 앵커에서 BFS 시작
	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); CellId++)
	{
		if (Cache.GetCellExists(CellId) &&
		    Cache.GetCellIsAnchor(CellId) &&
		    !DestroyedCells.Contains(CellId))
		{
			Queue.Enqueue(CellId);
			Connected.Add(CellId);
		}
	}

	// 2. BFS 탐색
	while (!Queue.IsEmpty())
	{
		int32 Current;
		Queue.Dequeue(Current);

		for (int32 Neighbor : Cache.GetCellNeighbors(Current))
		{
			if (!DestroyedCells.Contains(Neighbor) &&
			    !Connected.Contains(Neighbor))
			{
				Connected.Add(Neighbor);
				Queue.Enqueue(Neighbor);
			}
		}
	}

	// 3. 연결되지 않은 셀 = 분리됨
	TSet<int32> Disconnected;
	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); CellId++)
	{
		if (Cache.GetCellExists(CellId) &&
		    !DestroyedCells.Contains(CellId) &&
		    !Connected.Contains(CellId))
		{
			Disconnected.Add(CellId);
		}
	}

	return Disconnected;
}

TArray<TArray<int32>> FCellDestructionSystem::GroupDetachedCells(
	const FGridCellCache& Cache,
	const TSet<int32>& DisconnectedCells,
	const TSet<int32>& DestroyedCells)
{
	TArray<TArray<int32>> Groups;
	TSet<int32> Visited;

	for (int32 StartCell : DisconnectedCells)
	{
		if (Visited.Contains(StartCell))
		{
			continue;
		}

		// BFS로 연결된 분리 셀 그룹 찾기
		TArray<int32> Group;
		TQueue<int32> Queue;

		Queue.Enqueue(StartCell);
		Visited.Add(StartCell);

		while (!Queue.IsEmpty())
		{
			int32 Current;
			Queue.Dequeue(Current);
			Group.Add(Current);

			for (int32 Neighbor : Cache.GetCellNeighbors(Current))
			{
				if (DisconnectedCells.Contains(Neighbor) &&
				    !Visited.Contains(Neighbor))
				{
					Visited.Add(Neighbor);
					Queue.Enqueue(Neighbor);
				}
			}
		}

		Groups.Add(MoveTemp(Group));
	}

	return Groups;
}

//=============================================================================
// FCellDestructionSystem - 유틸리티
//=============================================================================

FVector FCellDestructionSystem::CalculateGroupCenter(
	const FGridCellCache& Cache,
	const TArray<int32>& CellIds,
	const FTransform& MeshTransform)
{
	if (CellIds.Num() == 0)
	{
		return FVector::ZeroVector;
	}

	FVector Sum = FVector::ZeroVector;
	for (int32 CellId : CellIds)
	{
		Sum += Cache.IdToWorldCenter(CellId, MeshTransform);
	}

	return Sum / CellIds.Num();
}

FVector FCellDestructionSystem::CalculateDebrisVelocity(
	const FVector& DebrisCenter,
	const TArray<FQuantizedDestructionInput>& DestructionInputs,
	float BaseSpeed)
{
	if (DestructionInputs.Num() == 0)
	{
		return FVector::ZeroVector;
	}

	// 가장 가까운 파괴 입력 찾기
	float MinDistSq = MAX_FLT;
	FVector ClosestCenter = FVector::ZeroVector;

	for (const auto& Input : DestructionInputs)
	{
		const FVector Center = FVector(Input.CenterMM.X, Input.CenterMM.Y, Input.CenterMM.Z) * 0.1f;
		const float DistSq = FVector::DistSquared(DebrisCenter, Center);

		if (DistSq < MinDistSq)
		{
			MinDistSq = DistSq;
			ClosestCenter = Center;
		}
	}

	// 폭발 방향으로 속도
	const FVector Direction = (DebrisCenter - ClosestCenter).GetSafeNormal();
	return Direction * BaseSpeed;
}

bool FCellDestructionSystem::IsBoundaryCell(
	const FGridCellCache& Cache,
	int32 CellId,
	const TSet<int32>& DestroyedCells)
{
	for (int32 Neighbor : Cache.GetCellNeighbors(CellId))
	{
		if (DestroyedCells.Contains(Neighbor))
		{
			return true;  // 파괴된 셀과 인접 = 경계
		}
	}
	return false;
}

//=============================================================================
// FDestructionBatchProcessor
//=============================================================================

FDestructionBatchProcessor::FDestructionBatchProcessor()
	: AccumulatedTime(0.0f)
	, CachePtr(nullptr)
	, CellStatePtr(nullptr)
	, MeshTransform(FTransform::Identity)
	, DebrisIdCounter(0)
{
}

void FDestructionBatchProcessor::QueueDestruction(const FCellDestructionShape& Shape)
{
	// 양자화하여 저장
	PendingDestructions.Add(FQuantizedDestructionInput::FromDestructionShape(Shape));
}

bool FDestructionBatchProcessor::Tick(float DeltaTime)
{
	AccumulatedTime += DeltaTime;

	if (AccumulatedTime >= BatchInterval && PendingDestructions.Num() > 0)
	{
		AccumulatedTime = 0.0f;
		ProcessBatch();
		return true;
	}

	return false;
}

void FDestructionBatchProcessor::FlushQueue()
{
	if (PendingDestructions.Num() > 0)
	{
		ProcessBatch();
		AccumulatedTime = 0.0f;
	}
}

void FDestructionBatchProcessor::SetContext(
	const FGridCellCache* InCache,
	FCellState* InCellState,
	const FTransform& InMeshTransform)
{
	CachePtr = InCache;
	CellStatePtr = InCellState;
	MeshTransform = InMeshTransform;
}

void FDestructionBatchProcessor::ProcessBatch()
{
	if (!CachePtr || !CellStatePtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("FDestructionBatchProcessor: Context not set"));
		PendingDestructions.Empty();
		return;
	}

	// 결과 초기화
	LastBatchResult = FBatchedDestructionEvent();
	LastBatchResult.DestructionInputs = PendingDestructions;

	//=====================================================
	// Phase 1: 모든 파괴 입력으로 셀 판정
	//=====================================================
	TSet<int32> NewlyDestroyed;

	for (const auto& Input : PendingDestructions)
	{
		TArray<int32> Cells = FCellDestructionSystem::CalculateDestroyedCells(
			*CachePtr, Input, MeshTransform, CellStatePtr->DestroyedCells);

		for (int32 CellId : Cells)
		{
			NewlyDestroyed.Add(CellId);
		}
	}

	if (NewlyDestroyed.Num() == 0)
	{
		PendingDestructions.Empty();
		return;
	}

	//=====================================================
	// Phase 2: 셀 상태 업데이트
	//=====================================================
	for (int32 CellId : NewlyDestroyed)
	{
		CellStatePtr->DestroyedCells.Add(CellId);
	}

	//=====================================================
	// Phase 3: BFS 1회만 실행 (배칭의 핵심)
	//=====================================================
	TSet<int32> Disconnected = FCellDestructionSystem::FindDisconnectedCells(
		*CachePtr, CellStatePtr->DestroyedCells);

	TArray<TArray<int32>> DetachedGroups = FCellDestructionSystem::GroupDetachedCells(
		*CachePtr, Disconnected, CellStatePtr->DestroyedCells);

	//=====================================================
	// Phase 4: 분리된 셀들도 파괴 처리
	//=====================================================
	for (const auto& Group : DetachedGroups)
	{
		for (int32 CellId : Group)
		{
			CellStatePtr->DestroyedCells.Add(CellId);
		}
	}

	//=====================================================
	// Phase 5: 이벤트 생성
	//=====================================================
	for (int32 CellId : NewlyDestroyed)
	{
		LastBatchResult.DestroyedCellIds.Add((int16)CellId);
	}

	// 파편 정보 생성
	for (const auto& Group : DetachedGroups)
	{
		FDetachedDebrisInfo DebrisInfo;
		DebrisInfo.DebrisId = ++DebrisIdCounter;

		for (int32 CellId : Group)
		{
			DebrisInfo.CellIds.Add((int16)CellId);
			LastBatchResult.DestroyedCellIds.Add((int16)CellId);
		}

		DebrisInfo.InitialLocation = FCellDestructionSystem::CalculateGroupCenter(
			*CachePtr, Group, MeshTransform);

		DebrisInfo.InitialVelocity = FCellDestructionSystem::CalculateDebrisVelocity(
			DebrisInfo.InitialLocation, PendingDestructions);

		LastBatchResult.DetachedDebris.Add(DebrisInfo);
	}

	// 큐 초기화
	PendingDestructions.Empty();

	UE_LOG(LogTemp, Log, TEXT("FDestructionBatchProcessor: Processed %d destroyed cells, %d debris groups"),
		LastBatchResult.DestroyedCellIds.Num(), LastBatchResult.DetachedDebris.Num());
}

//=============================================================================
// FCellDestructionSystem - SubCell 레벨 연결성 검사 (2x2x2 최적화)
//=============================================================================

namespace SubCellBFSHelper
{
	/**
	 * 경계면 SubCell 쌍 테이블 (2x2x2 전용)
	 * 각 방향별로 (현재 Cell SubCell, 이웃 Cell SubCell) 쌍 4개
	 *
	 * SubCell 배치:
	 *   Z=0: 0(0,0,0), 1(1,0,0), 2(0,1,0), 3(1,1,0)
	 *   Z=1: 4(0,0,1), 5(1,0,1), 6(0,1,1), 7(1,1,1)
	 */
	struct FBoundarySubCellPair
	{
		int32 Current;   // 현재 Cell의 경계 SubCell
		int32 Neighbor;  // 이웃 Cell의 대응 SubCell
	};

	// +X 방향: X=1 (1,3,5,7) → 이웃의 X=0 (0,2,4,6)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_POS_X[4] = {
		{1, 0}, {3, 2}, {5, 4}, {7, 6}
	};

	// -X 방향: X=0 (0,2,4,6) → 이웃의 X=1 (1,3,5,7)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_NEG_X[4] = {
		{0, 1}, {2, 3}, {4, 5}, {6, 7}
	};

	// +Y 방향: Y=1 (2,3,6,7) → 이웃의 Y=0 (0,1,4,5)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_POS_Y[4] = {
		{2, 0}, {3, 1}, {6, 4}, {7, 5}
	};

	// -Y 방향: Y=0 (0,1,4,5) → 이웃의 Y=1 (2,3,6,7)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_NEG_Y[4] = {
		{0, 2}, {1, 3}, {4, 6}, {5, 7}
	};

	// +Z 방향: Z=1 (4,5,6,7) → 이웃의 Z=0 (0,1,2,3)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_POS_Z[4] = {
		{4, 0}, {5, 1}, {6, 2}, {7, 3}
	};

	// -Z 방향: Z=0 (0,1,2,3) → 이웃의 Z=1 (4,5,6,7)
	inline constexpr FBoundarySubCellPair BOUNDARY_PAIRS_NEG_Z[4] = {
		{0, 4}, {1, 5}, {2, 6}, {3, 7}
	};

	/**
	 * 방향별 경계 SubCell 쌍 배열 반환
	 * @param Direction - 0:-X, 1:+X, 2:-Y, 3:+Y, 4:-Z, 5:+Z
	 */
	inline const FBoundarySubCellPair* GetBoundaryPairs(int32 Direction)
	{
		switch (Direction)
		{
		case 0: return BOUNDARY_PAIRS_NEG_X;
		case 1: return BOUNDARY_PAIRS_POS_X;
		case 2: return BOUNDARY_PAIRS_NEG_Y;
		case 3: return BOUNDARY_PAIRS_POS_Y;
		case 4: return BOUNDARY_PAIRS_NEG_Z;
		case 5: return BOUNDARY_PAIRS_POS_Z;
		default: return nullptr;
		}
	}

	/**
	 * 두 Cell 사이에 연결된 경계 SubCell 쌍이 있는지 확인
	 * @param Direction - CellA → CellB 방향 (0-5)
	 * @return 양쪽 모두 살아있는 경계 SubCell 쌍이 하나라도 있으면 true
	 */
	bool HasConnectedBoundary(
		int32 CellA,
		int32 CellB,
		int32 Direction,
		const FCellState& CellState)
	{
		const FBoundarySubCellPair* Pairs = GetBoundaryPairs(Direction);
		if (!Pairs)
		{
			return false;
		}

		for (int32 i = 0; i < 4; ++i)
		{
			if (CellState.IsSubCellAlive(CellA, Pairs[i].Current) &&
				CellState.IsSubCellAlive(CellB, Pairs[i].Neighbor))
			{
				return true;  // 연결된 쌍 발견
			}
		}

		return false;  // 연결 없음
	}

	/**
	 * Cell에 살아있는 SubCell이 있는지 확인
	 */
	bool HasAliveSubCell(int32 CellId, const FCellState& CellState)
	{
		if (CellState.DestroyedCells.Contains(CellId))
		{
			return false;
		}

		const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);
		if (!SubCellState)
		{
			return true;  // 상태 없으면 모두 살아있음
		}

		return !SubCellState->IsFullyDestroyed();
	}

	/**
	 * Cell 단위 BFS로 Anchor 도달 여부 확인 (2x2x2 최적화)
	 *
	 * 2x2x2에서는 Cell 내 모든 SubCell이 서로 연결되므로,
	 * Cell 단위로 탐색하고 경계 연결만 SubCell 레벨로 검사
	 *
	 * @param Cache - 격자 캐시
	 * @param CellState - 셀 상태
	 * @param StartCellId - 시작 Cell
	 * @param ConfirmedConnected - 이미 Connected 확인된 Cell들
	 * @param OutVisitedCells - 방문한 Cell 집합 (출력)
	 * @return Anchor 도달 여부
	 */
	bool PerformSubCellBFS(
		const FGridCellCache& Cache,
		const FCellState& CellState,
		int32 StartCellId,
		const TSet<int32>& ConfirmedConnected,
		TSet<int32>& OutVisitedCells)
	{
		OutVisitedCells.Reset();

		// 시작 Cell에 살아있는 SubCell이 있는지 확인
		if (!HasAliveSubCell(StartCellId, CellState))
		{
			return false;
		}

		// Cell 단위 BFS
		TQueue<int32> CellQueue;
		TSet<int32> VisitedCells;

		CellQueue.Enqueue(StartCellId);
		VisitedCells.Add(StartCellId);
		OutVisitedCells.Add(StartCellId);

		while (!CellQueue.IsEmpty())
		{
			int32 CurrCellId;
			CellQueue.Dequeue(CurrCellId);

			// Anchor Cell에 도달했는지 확인
			if (Cache.GetCellIsAnchor(CurrCellId))
			{
				return true;
			}

			// 이미 Connected 확인된 Cell에 도달
			if (ConfirmedConnected.Contains(CurrCellId))
			{
				return true;
			}

			// 6방향 이웃 Cell 탐색
			const FIntVector CurrCoord = Cache.IdToCoord(CurrCellId);

			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				const FIntVector NeighborCoord = CurrCoord + FIntVector(
					DIRECTION_OFFSETS[Dir][0],
					DIRECTION_OFFSETS[Dir][1],
					DIRECTION_OFFSETS[Dir][2]
				);

				if (!Cache.IsValidCoord(NeighborCoord))
				{
					continue;
				}

				const int32 NeighborCellId = Cache.CoordToId(NeighborCoord);

				// 이미 방문했거나 유효하지 않은 Cell 스킵
				if (VisitedCells.Contains(NeighborCellId))
				{
					continue;
				}

				if (!Cache.GetCellExists(NeighborCellId))
				{
					continue;
				}

				if (CellState.DestroyedCells.Contains(NeighborCellId))
				{
					continue;
				}

				// 경계 SubCell 연결 검사
				if (HasConnectedBoundary(CurrCellId, NeighborCellId, Dir, CellState))
				{
					VisitedCells.Add(NeighborCellId);
					CellQueue.Enqueue(NeighborCellId);
					OutVisitedCells.Add(NeighborCellId);
				}
			}
		}

		// Anchor에 도달하지 못함
		return false;
	}
}

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsWithSubCells(
	const FGridCellCache& Cache,
	const FCellState& CellState,
	const TArray<int32>& AffectedCells)
{
	using namespace SubCellBFSHelper;

	TSet<int32> Disconnected;
	TSet<int32> ConfirmedConnected;
	TSet<int32> Processed;

	// 1. 분리 후보 수집: AffectedCells + 인접 Cell들
	TSet<int32> Candidates;
	for (int32 CellId : AffectedCells)
	{
		// 파괴된 Cell은 제외
		if (CellState.DestroyedCells.Contains(CellId))
		{
			continue;
		}

		Candidates.Add(CellId);

		// 인접 Cell들도 추가 (경계 연결이 끊겼을 수 있음)
		for (int32 NeighborId : Cache.GetCellNeighbors(CellId).Values)
		{
			if (!CellState.DestroyedCells.Contains(NeighborId))
			{
				Candidates.Add(NeighborId);
			}
		}
	}

	// 2. 각 후보 Cell에서 SubCell BFS
	for (int32 CandidateCell : Candidates)
	{
		if (Processed.Contains(CandidateCell))
		{
			continue;
		}

		// SubCell 레벨 BFS 수행
		TSet<int32> VisitedCells;
		const bool bReachedAnchor = PerformSubCellBFS(
			Cache, CellState, CandidateCell, ConfirmedConnected, VisitedCells);

		// 방문한 모든 Cell 처리
		for (int32 VisitedCell : VisitedCells)
		{
			Processed.Add(VisitedCell);

			if (bReachedAnchor)
			{
				ConfirmedConnected.Add(VisitedCell);
			}
			else
			{
				Disconnected.Add(VisitedCell);
			}
		}
	}

	return Disconnected;
}
