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

FDestructionResult FCellDestructionSystem::CalculateDestroyedCells(
	const FGridCellCache& Cache,
	const FQuantizedDestructionInput& Shape,
	const FTransform& MeshTransform,
	FCellState& InOutCellState)
{
	FDestructionResult Result;
	Result.NewlyDestroyedCells = CalculateDestroyedCells(Cache, Shape, MeshTransform, InOutCellState.DestroyedCells);
	InOutCellState.DestroyCells(Result.NewlyDestroyedCells);
	return Result;
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
	FSupercellCache& SupercellCache,
	const FCellState& CellState,
	bool bEnableSupercell,
	bool bEnableSubcell)
{
	UE_LOG(LogTemp, Warning, TEXT("FindDisconnectedCells: bEnableSupercell=%d, bEnableSubcell=%d"),
		bEnableSupercell ? 1 : 0, bEnableSubcell ? 1 : 0);

	if (bEnableSupercell)
	{
		UE_LOG(LogTemp, Warning, TEXT("  -> Using HierarchicalLevel"));
		return FindDisconnectedCellsHierarchicalLevel(
			Cache,
			SupercellCache,
			CellState,
			bEnableSubcell);
	}
	if (bEnableSubcell)
	{
		UE_LOG(LogTemp, Warning, TEXT("  -> Using SubCellLevel"));
		return FindDisconnectedCellsSubCellLevel(
			Cache,
			CellState);
	}

	UE_LOG(LogTemp, Warning, TEXT("  -> Using CellLevel"));
	return FindDisconnectedCellsCellLevel(Cache, CellState.DestroyedCells);
}

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsCellLevel(
	const FGridCellCache& Cache,
	const TSet<int32>& DestroyedCells)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FindDisconnectedCellsCellLevel)
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
	int32 ValidCellCount = 0;
	int32 AnchorCount = 0;
	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); CellId++)
	{
		if (Cache.GetCellExists(CellId))
		{
			ValidCellCount++;
			if (Cache.GetCellIsAnchor(CellId)) AnchorCount++;

			if (!DestroyedCells.Contains(CellId) &&
			    !Connected.Contains(CellId))
			{
				Disconnected.Add(CellId);
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("FindDisconnectedCellsCellLevel: Valid=%d, Anchor=%d, Destroyed=%d, Connected=%d, Disconnected=%d"),
		ValidCellCount, AnchorCount, DestroyedCells.Num(), Connected.Num(), Disconnected.Num());

	return Disconnected;
}

TArray<TArray<int32>> FCellDestructionSystem::GroupDetachedCells(
	const FGridCellCache& Cache,
	const TSet<int32>& DisconnectedCells,
	const TSet<int32>& DestroyedCells)
{
	TArray<TArray<int32>> Groups;
	TSet<int32> Visited;

	//=========================================================================
	// Phase 1: BFS로 Disconnected Cell 그룹화
	//=========================================================================
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
	TSet<int32> Disconnected = FCellDestructionSystem::FindDisconnectedCellsCellLevel(
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

	/**
	 * SubCell 내부 인접 테이블 (2x2x2 전용, 6방향)
	 * 각 SubCell에서 6방향으로 인접한 SubCell ID (-1이면 해당 방향에 인접 없음)
	 * 순서: -X, +X, -Y, +Y, -Z, +Z
	 */
	inline constexpr int32 SUBCELL_ADJACENCY[8][6] = {
		// SubCell 0 (0,0,0): -X=없음, +X=1, -Y=없음, +Y=2, -Z=없음, +Z=4
		{-1, 1, -1, 2, -1, 4},
		// SubCell 1 (1,0,0): -X=0, +X=없음, -Y=없음, +Y=3, -Z=없음, +Z=5
		{0, -1, -1, 3, -1, 5},
		// SubCell 2 (0,1,0): -X=없음, +X=3, -Y=0, +Y=없음, -Z=없음, +Z=6
		{-1, 3, 0, -1, -1, 6},
		// SubCell 3 (1,1,0): -X=2, +X=없음, -Y=1, +Y=없음, -Z=없음, +Z=7
		{2, -1, 1, -1, -1, 7},
		// SubCell 4 (0,0,1): -X=없음, +X=5, -Y=없음, +Y=6, -Z=0, +Z=없음
		{-1, 5, -1, 6, 0, -1},
		// SubCell 5 (1,0,1): -X=4, +X=없음, -Y=없음, +Y=7, -Z=1, +Z=없음
		{4, -1, -1, 7, 1, -1},
		// SubCell 6 (0,1,1): -X=없음, +X=7, -Y=4, +Y=없음, -Z=2, +Z=없음
		{-1, 7, 4, -1, 2, -1},
		// SubCell 7 (1,1,1): -X=6, +X=없음, -Y=5, +Y=없음, -Z=3, +Z=없음
		{6, -1, 5, -1, 3, -1},
	};

	/**
	 * 반대 방향 반환
	 * 0(-X) <-> 1(+X), 2(-Y) <-> 3(+Y), 4(-Z) <-> 5(+Z)
	 */
	inline constexpr int32 GetOppositeDirection(int32 Direction)
	{
		return Direction ^ 1;  // 0<->1, 2<->3, 4<->5
	}

	/**
	 * 특정 방향의 경계 SubCell ID 목록 반환 (4개)
	 * @param Direction - 0:-X, 1:+X, 2:-Y, 3:+Y, 4:-Z, 5:+Z
	 */
	inline void GetBoundarySubCellIds(int32 Direction, int32 OutIds[4])
	{
		const FBoundarySubCellPair* Pairs = GetBoundaryPairs(Direction);
		if (Pairs)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				OutIds[i] = Pairs[i].Current;
			}
		}
	}

	/**
	 * Detached Cell 경계에서 Connected Cell 내부로 SubCell Flooding
	 * 경계 SubCell에서 시작하여 Dead SubCell을 만날 때까지 확장
	 *
	 * @param CellState - 셀 상태
	 * @param ConnectedCellId - Connected Cell ID
	 * @param DirectionFromDetached - Detached → Connected 방향 (0-5)
	 * @return Flooding된 SubCell ID 목록
	 */
	TArray<int32> FloodSubCellsFromBoundary(
		const FCellState& CellState,
		int32 ConnectedCellId,
		int32 DirectionFromDetached)
	{
		TArray<int32> Result;

		// Detached → Connected 방향의 반대 = Connected Cell에서 Detached와 맞닿은 면
		const int32 BoundaryDirection = GetOppositeDirection(DirectionFromDetached);

		// 경계 SubCell ID들 가져오기
		int32 BoundarySubCellIds[4];
		GetBoundarySubCellIds(BoundaryDirection, BoundarySubCellIds);

		// BFS를 위한 자료구조
		TSet<int32> Visited;
		TQueue<int32> Queue;

		// 경계 SubCell들을 시작점으로 추가
		for (int32 i = 0; i < 4; ++i)
		{
			const int32 SubCellId = BoundarySubCellIds[i];
			if (!Visited.Contains(SubCellId))
			{
				Visited.Add(SubCellId);
				Queue.Enqueue(SubCellId);
			}
		}

		// BFS 탐색
		while (!Queue.IsEmpty())
		{
			int32 CurrentSubCellId;
			Queue.Dequeue(CurrentSubCellId);

			const bool bIsAlive = CellState.IsSubCellAlive(ConnectedCellId, CurrentSubCellId);

			// 결과에 추가 (살아있든 죽었든)
			Result.Add(CurrentSubCellId);

			// 죽은 SubCell이면 확장 중단 (경계 역할)
			if (!bIsAlive)
			{
				continue;
			}

			// 살아있는 SubCell이면 인접 SubCell로 확장
			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				const int32 NeighborSubCellId = SUBCELL_ADJACENCY[CurrentSubCellId][Dir];

				// 유효하지 않거나 이미 방문한 SubCell은 스킵
				if (NeighborSubCellId < 0 || Visited.Contains(NeighborSubCellId))
				{
					continue;
				}

				Visited.Add(NeighborSubCellId);
				Queue.Enqueue(NeighborSubCellId);
			}
		}

		return Result;
	}

	/**
	 * Detached 그룹의 경계 Cell 정보
	 */
	struct FBoundaryCellInfo
	{
		int32 BoundaryCellId = INDEX_NONE;
		TArray<TPair<int32, int32>> AdjacentConnectedCells;  // (CellId, Direction)
	};

	/**
	 * Detached 그룹에서 경계 Cell 목록 추출 (인접 Connected Cell 정보 포함)
	 */
	TArray<FBoundaryCellInfo> GetGroupBoundaryCellsWithAdjacency(
		const FGridCellCache& Cache,
		const TArray<int32>& GroupCellIds,
		const FCellState& CellState)
	{
		TArray<FBoundaryCellInfo> Result;

		// 빠른 검색을 위해 그룹 Cell들을 Set으로 변환
		TSet<int32> GroupCellSet;
		GroupCellSet.Reserve(GroupCellIds.Num());
		for (int32 CellId : GroupCellIds)
		{
			GroupCellSet.Add(CellId);
		}

		// 각 그룹 Cell에 대해 경계 여부 판정
		for (int32 CellId : GroupCellIds)
		{
			FBoundaryCellInfo Info;
			Info.BoundaryCellId = CellId;

			const FIntVector CellCoord = Cache.IdToCoord(CellId);

			// 6방향 이웃 탐색
			for (int32 Dir = 0; Dir < 6; ++Dir)
			{
				const FIntVector NeighborCoord = CellCoord + FIntVector(
					DIRECTION_OFFSETS[Dir][0],
					DIRECTION_OFFSETS[Dir][1],
					DIRECTION_OFFSETS[Dir][2]
				);

				// 유효하지 않은 좌표는 스킵
				if (!Cache.IsValidCoord(NeighborCoord))
				{
					continue;
				}

				const int32 NeighborCellId = Cache.CoordToId(NeighborCoord);

				// 그룹에 속한 Cell은 스킵
				if (GroupCellSet.Contains(NeighborCellId))
				{
					continue;
				}

				// 존재하지 않는 Cell은 스킵
				if (!Cache.GetCellExists(NeighborCellId))
				{
					continue;
				}

				// 파괴된 Cell은 스킵 (Connected Cell만 대상)
				if (CellState.DestroyedCells.Contains(NeighborCellId))
				{
					continue;
				}

				// Connected Cell 발견 → 인접 목록에 추가
				Info.AdjacentConnectedCells.Add(TPair<int32, int32>(NeighborCellId, Dir));
			}

			// 인접한 Connected Cell이 하나라도 있으면 경계 Cell
			if (Info.AdjacentConnectedCells.Num() > 0)
			{
				Result.Add(MoveTemp(Info));
			}
		}

		return Result;
	}
}

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsSubCellLevel(
	const FGridCellCache& Cache,
	const FCellState& CellState)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FindDisconnectedCellsSubCellLevel);
	using namespace SubCellBFSHelper;

	TSet<int32> Connected;

	// 1. 모든 Anchor Cell에서 BFS 시작
	TQueue<int32> Queue;
	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); CellId++)
	{
		if (Cache.GetCellExists(CellId) &&
			Cache.GetCellIsAnchor(CellId) &&
			!CellState.DestroyedCells.Contains(CellId) &&
			HasAliveSubCell(CellId, CellState))
		{
			Queue.Enqueue(CellId);
			Connected.Add(CellId);
		}
	}

	// 2. BFS: SubCell 경계 연결성으로 도달 가능한 모든 Cell 찾기
	while (!Queue.IsEmpty())
	{
		int32 CurrCellId;
		Queue.Dequeue(CurrCellId);

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

			if (Connected.Contains(NeighborCellId))
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

			// SubCell 경계 연결성 검사
			if (HasConnectedBoundary(CurrCellId, NeighborCellId, Dir, CellState))
			{
				Connected.Add(NeighborCellId);
				Queue.Enqueue(NeighborCellId);
			}
		}
	}

	// 3. 전체 Grid에서 Connected가 아닌 Cell = Disconnected
	TSet<int32> Disconnected;
	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); CellId++)
	{
		if (Cache.GetCellExists(CellId) &&
			!CellState.DestroyedCells.Contains(CellId) &&
			!Connected.Contains(CellId))
		{
			Disconnected.Add(CellId);
		}
	}

	return Disconnected;
}

TArray<FDetachedGroupWithSubCell> FCellDestructionSystem::GroupDetachedCellsWithSubCells(
	const FGridCellCache& Cache,
	const TSet<int32>& DisconnectedCells,
	const FCellState& CellState)
{
	using namespace SubCellBFSHelper;

	TArray<FDetachedGroupWithSubCell> Results;

	//=========================================================================
	// Phase 1: 기존 로직으로 Disconnected Cell 그룹화
	//=========================================================================
	TArray<TArray<int32>> CellGroups = GroupDetachedCells(Cache, DisconnectedCells, CellState.DestroyedCells);

	//=========================================================================
	// Phase 2: 각 그룹에 대해 SubCell Flooding
	//=========================================================================
	for (const TArray<int32>& CellGroup : CellGroups)
	{
		FDetachedGroupWithSubCell GroupResult;
		GroupResult.DetachedCellIds = CellGroup;

		// 경계 Cell들과 인접 Connected Cell 정보 수집
		TArray<FBoundaryCellInfo> BoundaryCells = GetGroupBoundaryCellsWithAdjacency(Cache, CellGroup, CellState);

		// 각 경계 Cell에서 인접 Connected Cell로 Flooding
		for (const FBoundaryCellInfo& BoundaryInfo : BoundaryCells)
		{
			for (const TPair<int32, int32>& Adjacent : BoundaryInfo.AdjacentConnectedCells)
			{
				const int32 ConnectedCellId = Adjacent.Key;
				const int32 Direction = Adjacent.Value;

				// Flooding 수행
				TArray<int32> FloodedSubCells = FloodSubCellsFromBoundary(CellState, ConnectedCellId, Direction);

				// 결과 병합 (중복 제거)
				if (FloodedSubCells.Num() > 0)
				{
					FIntArray* ExistingSubCells = GroupResult.IncludedSubCells.Find(ConnectedCellId);
					if (ExistingSubCells)
					{
						// 기존 목록에 추가 (중복 제거)
						for (int32 SubCellId : FloodedSubCells)
						{
							ExistingSubCells->Values.AddUnique(SubCellId);
						}
					}
					else
					{
						// 새 항목 추가
						FIntArray NewSubCells;
						NewSubCells.Values = MoveTemp(FloodedSubCells);
						GroupResult.IncludedSubCells.Add(ConnectedCellId, MoveTemp(NewSubCells));
					}
				}
			}
		}

		Results.Add(MoveTemp(GroupResult));
	}

	return Results;
}

//=============================================================================
// FCellDestructionSystem - Hierarchical BFS (SuperCell 최적화)
//=============================================================================

namespace HierarchicalBFSHelper
{
	/**
	 * SuperCell의 Cell 좌표 범위 계산
	 * TArray 할당 없이 직접 좌표 순회용
	 */
	struct FSupercellCellRange
	{
		int32 StartX, StartY, StartZ;
		int32 EndX, EndY, EndZ;

		FSupercellCellRange(int32 SupercellId, const FSupercellCache& SupercellCache, const FGridCellCache& GridCache)
		{
			const FIntVector SupercellCoord = SupercellCache.SupercellIdToCoord(SupercellId);
			StartX = SupercellCoord.X * SupercellCache.SupercellSize.X;
			StartY = SupercellCoord.Y * SupercellCache.SupercellSize.Y;
			StartZ = SupercellCoord.Z * SupercellCache.SupercellSize.Z;

			EndX = FMath::Min(StartX + SupercellCache.SupercellSize.X, GridCache.GridSize.X);
			EndY = FMath::Min(StartY + SupercellCache.SupercellSize.Y, GridCache.GridSize.Y);
			EndZ = FMath::Min(StartZ + SupercellCache.SupercellSize.Z, GridCache.GridSize.Z);
		}
	};

	/**
	 * SuperCell 내 유효한 모든 Cell을 Connected로 마킹
	 * TArray 할당 없이 직접 좌표 순회
	 */
	void MarkAllCellsInSupercell(
		int32 SupercellId,
		const FSupercellCache& SupercellCache,
		const FGridCellCache& GridCache,
		const FCellState& CellState,
		TSet<int32>& ConnectedCells)
	{
		const FSupercellCellRange Range(SupercellId, SupercellCache, GridCache);

		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				for (int32 X = Range.StartX; X < Range.EndX; ++X)
				{
					const int32 CellId = GridCache.CoordToId(X, Y, Z);
					// 파괴된 Cell은 제외
					if (GridCache.GetCellExists(CellId) && !CellState.DestroyedCells.Contains(CellId))
					{
						ConnectedCells.Add(CellId);
					}
				}
			}
		}
	}

	/**
	 * Neighbor Cell 추가 헬퍼 (SubCell 모드 분기 포함)
	 */
	FORCEINLINE void TryAddNeighborCell(
		int32 BoundaryCellId,
		int32 NeighborCellId,
		int32 Dir,
		const FGridCellCache& GridCache,
		const FCellState& CellState,
		bool bEnableSubcell,
		TQueue<FBFSNode>& Queue,
		TSet<int32>& ConnectedCells)
	{
		if (ConnectedCells.Contains(NeighborCellId))
		{
			return;
		}

		if (!GridCache.GetCellExists(NeighborCellId))
		{
			return;
		}

		if (CellState.DestroyedCells.Contains(NeighborCellId))
		{
			return;
		}

		if (bEnableSubcell)
		{
			if (SubCellBFSHelper::HasConnectedBoundary(BoundaryCellId, NeighborCellId, Dir, CellState))
			{
				ConnectedCells.Add(NeighborCellId);
				Queue.Enqueue(FBFSNode::MakeCell(NeighborCellId));
			}
		}
		else
		{
			ConnectedCells.Add(NeighborCellId);
			Queue.Enqueue(FBFSNode::MakeCell(NeighborCellId));
		}
	}

	/**
	 * SuperCell 노드 처리 (Intact SuperCell에서 인접 노드 탐색)
	 *
	 * 성능 최적화:
	 * - IsSupercellIntact() (비트필드 O(1))만 사용
	 * - TArray 할당 없이 직접 좌표 순회
	 */
	void ProcessSupercellNode(
		int32 SupercellId,
		const FGridCellCache& GridCache,
		FSupercellCache& SupercellCache,
		const FCellState& CellState,
		bool bEnableSubcell,
		TQueue<FBFSNode>& Queue,
		TSet<int32>& ConnectedCells,
		TSet<int32>& VisitedSupercells)
	{
		const FSupercellCellRange Range(SupercellId, SupercellCache, GridCache);
		const FIntVector SupercellCoord = SupercellCache.SupercellIdToCoord(SupercellId);

		// 6방향 인접 SuperCell 탐색
		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const FIntVector NeighborSCCoord = SupercellCoord + FIntVector(
				DIRECTION_OFFSETS[Dir][0],
				DIRECTION_OFFSETS[Dir][1],
				DIRECTION_OFFSETS[Dir][2]
			);

			if (!SupercellCache.IsValidSupercellCoord(NeighborSCCoord))
			{
				continue;
			}

			const int32 NeighborSupercellId = SupercellCache.SupercellCoordToId(NeighborSCCoord);

			// 이미 방문한 SuperCell은 스킵
			if (VisitedSupercells.Contains(NeighborSupercellId))
			{
				continue;
			}

			// 인접 SuperCell이 Intact인지 확인 (비트필드 체크만 - O(1))
			if (SupercellCache.IsSupercellIntact(NeighborSupercellId))
			{
				// Intact SuperCell → SuperCell 노드로 추가
				VisitedSupercells.Add(NeighborSupercellId);
				Queue.Enqueue(FBFSNode::MakeSupercell(NeighborSupercellId));
				MarkAllCellsInSupercell(NeighborSupercellId, SupercellCache, GridCache, CellState, ConnectedCells); 
			}
			else
			{
				// Broken SuperCell → 경계 Cell에서 인접 Cell로 직접 연결
				// TArray 할당 없이 직접 좌표 순회

				// 방향별 경계면 Cell 순회 및 인접 Cell 처리
				switch (Dir)
				{
				case 0: // -X: 우리의 X=StartX 면 → 인접 Cell은 X=StartX-1
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
						{
							const int32 BoundaryCellId = GridCache.CoordToId(Range.StartX, Y, Z);
							const int32 NeighborCellId = GridCache.CoordToId(Range.StartX - 1, Y, Z);
							if (GridCache.IsValidCoord(Range.StartX - 1, Y, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 1: // +X: 우리의 X=EndX-1 면 → 인접 Cell은 X=EndX
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
						{
							const int32 BoundaryCellId = GridCache.CoordToId(Range.EndX - 1, Y, Z);
							const int32 NeighborCellId = GridCache.CoordToId(Range.EndX, Y, Z);
							if (GridCache.IsValidCoord(Range.EndX, Y, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 2: // -Y: 우리의 Y=StartY 면 → 인접 Cell은 Y=StartY-1
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridCache.CoordToId(X, Range.StartY, Z);
							const int32 NeighborCellId = GridCache.CoordToId(X, Range.StartY - 1, Z);
							if (GridCache.IsValidCoord(X, Range.StartY - 1, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 3: // +Y: 우리의 Y=EndY-1 면 → 인접 Cell은 Y=EndY
					for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridCache.CoordToId(X, Range.EndY - 1, Z);
							const int32 NeighborCellId = GridCache.CoordToId(X, Range.EndY, Z);
							if (GridCache.IsValidCoord(X, Range.EndY, Z))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 4: // -Z: 우리의 Z=StartZ 면 → 인접 Cell은 Z=StartZ-1
					for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridCache.CoordToId(X, Y, Range.StartZ);
							const int32 NeighborCellId = GridCache.CoordToId(X, Y, Range.StartZ - 1);
							if (GridCache.IsValidCoord(X, Y, Range.StartZ - 1))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;

				case 5: // +Z: 우리의 Z=EndZ-1 면 → 인접 Cell은 Z=EndZ
					for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
					{
						for (int32 X = Range.StartX; X < Range.EndX; ++X)
						{
							const int32 BoundaryCellId = GridCache.CoordToId(X, Y, Range.EndZ - 1);
							const int32 NeighborCellId = GridCache.CoordToId(X, Y, Range.EndZ);
							if (GridCache.IsValidCoord(X, Y, Range.EndZ))
							{
								TryAddNeighborCell(BoundaryCellId, NeighborCellId, Dir, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
							}
						}
					}
					break;
				}
			}
		}

		// Orphan Cell과의 연결 (SuperCell 경계의 Cell들에서 외부 Orphan Cell로)
		// TArray 할당 없이 6면의 경계 Cell을 직접 순회

		// -X 경계면
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				const int32 BoundaryCellId = GridCache.CoordToId(Range.StartX, Y, Z);
				const int32 NeighborX = Range.StartX - 1;
				if (GridCache.IsValidCoord(NeighborX, Y, Z))
				{
					const int32 NeighborCellId = GridCache.CoordToId(NeighborX, Y, Z);
					if (SupercellCache.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 0, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// +X 경계면
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
			{
				const int32 BoundaryCellId = GridCache.CoordToId(Range.EndX - 1, Y, Z);
				const int32 NeighborX = Range.EndX;
				if (GridCache.IsValidCoord(NeighborX, Y, Z))
				{
					const int32 NeighborCellId = GridCache.CoordToId(NeighborX, Y, Z);
					if (SupercellCache.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 1, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// -Y 경계면
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridCache.CoordToId(X, Range.StartY, Z);
				const int32 NeighborY = Range.StartY - 1;
				if (GridCache.IsValidCoord(X, NeighborY, Z))
				{
					const int32 NeighborCellId = GridCache.CoordToId(X, NeighborY, Z);
					if (SupercellCache.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 2, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// +Y 경계면
		for (int32 Z = Range.StartZ; Z < Range.EndZ; ++Z)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridCache.CoordToId(X, Range.EndY - 1, Z);
				const int32 NeighborY = Range.EndY;
				if (GridCache.IsValidCoord(X, NeighborY, Z))
				{
					const int32 NeighborCellId = GridCache.CoordToId(X, NeighborY, Z);
					if (SupercellCache.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 3, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// -Z 경계면
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridCache.CoordToId(X, Y, Range.StartZ);
				const int32 NeighborZ = Range.StartZ - 1;
				if (GridCache.IsValidCoord(X, Y, NeighborZ))
				{
					const int32 NeighborCellId = GridCache.CoordToId(X, Y, NeighborZ);
					if (SupercellCache.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 4, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}

		// +Z 경계면
		for (int32 Y = Range.StartY; Y < Range.EndY; ++Y)
		{
			for (int32 X = Range.StartX; X < Range.EndX; ++X)
			{
				const int32 BoundaryCellId = GridCache.CoordToId(X, Y, Range.EndZ - 1);
				const int32 NeighborZ = Range.EndZ;
				if (GridCache.IsValidCoord(X, Y, NeighborZ))
				{
					const int32 NeighborCellId = GridCache.CoordToId(X, Y, NeighborZ);
					if (SupercellCache.IsCellOrphan(NeighborCellId))
					{
						TryAddNeighborCell(BoundaryCellId, NeighborCellId, 5, GridCache, CellState, bEnableSubcell, Queue, ConnectedCells);
					}
				}
			}
		}
	}

	/**
	 * Cell 노드 처리 (개별 Cell에서 인접 노드 탐색)
	 *
	 * 성능 최적화: IsSupercellIntact() (비트필드 O(1))만 사용
	 */
	void ProcessCellNode(
		int32 CellId,
		const FGridCellCache& GridCache,
		FSupercellCache& SupercellCache,
		const FCellState& CellState,
		bool bEnableSubcell,
		TQueue<FBFSNode>& Queue,
		TSet<int32>& ConnectedCells,
		TSet<int32>& VisitedSupercells)
	{
		const FIntVector CellCoord = GridCache.IdToCoord(CellId);

		for (int32 Dir = 0; Dir < 6; ++Dir)
		{
			const FIntVector NeighborCoord = CellCoord + FIntVector(
				DIRECTION_OFFSETS[Dir][0],
				DIRECTION_OFFSETS[Dir][1],
				DIRECTION_OFFSETS[Dir][2]
			);

			if (!GridCache.IsValidCoord(NeighborCoord))
			{
				continue;
			}

			const int32 NeighborCellId = GridCache.CoordToId(NeighborCoord);

			if (!GridCache.GetCellExists(NeighborCellId))
			{
				continue;
			}

			if (CellState.DestroyedCells.Contains(NeighborCellId))
			{
				continue;
			}

			if (ConnectedCells.Contains(NeighborCellId))
			{
				continue;
			}

			// SubCell 모드에서는 경계 연결성 확인
			bool bIsConnected = true;
			if (bEnableSubcell)
			{
				bIsConnected = SubCellBFSHelper::HasConnectedBoundary(CellId, NeighborCellId, Dir, CellState);
			}

			if (!bIsConnected)
			{
				continue;
			}

			const int32 NeighborSupercellId = SupercellCache.GetSupercellForCell(NeighborCellId);

			// Neighbor가 Intact SuperCell에 속하는지 확인 (비트필드 체크만 - O(1))
			if (NeighborSupercellId != INDEX_NONE &&
			    !VisitedSupercells.Contains(NeighborSupercellId) &&
			    SupercellCache.IsSupercellIntact(NeighborSupercellId))
			{
				// Intact SuperCell → SuperCell 노드로 확장
				VisitedSupercells.Add(NeighborSupercellId);
				Queue.Enqueue(FBFSNode::MakeSupercell(NeighborSupercellId));
				MarkAllCellsInSupercell(NeighborSupercellId, SupercellCache, GridCache, CellState, ConnectedCells);
			}
			else
			{
				// Broken SuperCell 또는 Orphan → Cell 단위 추가
				ConnectedCells.Add(NeighborCellId);
				Queue.Enqueue(FBFSNode::MakeCell(NeighborCellId));
			}
		}
	}
}

TSet<int32> FCellDestructionSystem::FindConnectedCellsHierarchical(
	const FGridCellCache& Cache,
	FSupercellCache& SupercellCache,
	const FCellState& CellState,
	bool bEnableSubcell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FindConnectedCellsHierarchical);
	using namespace HierarchicalBFSHelper;

	TSet<int32> ConnectedCells;
	TSet<int32> VisitedSupercells;
	TQueue<FBFSNode> Queue;

	//=========================================================================
	// Step 1: Anchor 초기화
	// 성능 최적화: IsSupercellIntact() (비트필드 O(1))만 사용
	//=========================================================================
	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); ++CellId)
	{
		if (!Cache.GetCellExists(CellId))
		{
			continue;
		}

		if (!Cache.GetCellIsAnchor(CellId))
		{
			continue;
		}

		if (CellState.DestroyedCells.Contains(CellId))
		{
			continue;
		}

		// SubCell 모드에서는 살아있는 SubCell이 있어야 함
		if (bEnableSubcell && !SubCellBFSHelper::HasAliveSubCell(CellId, CellState))
		{
			continue;
		}

		const int32 SupercellId = SupercellCache.GetSupercellForCell(CellId);

		// Intact 여부는 비트필드로만 확인 (O(1))
		if (SupercellId != INDEX_NONE &&
		    !VisitedSupercells.Contains(SupercellId) &&
		    SupercellCache.IsSupercellIntact(SupercellId))
		{
			// Intact SuperCell → 단일 노드로 추가
			VisitedSupercells.Add(SupercellId);
			Queue.Enqueue(FBFSNode::MakeSupercell(SupercellId));
			MarkAllCellsInSupercell(SupercellId, SupercellCache, Cache, CellState, ConnectedCells);
		}
		else
		{
			// Broken SuperCell 또는 Orphan → Cell 단위 추가
			if (!ConnectedCells.Contains(CellId))
			{
				ConnectedCells.Add(CellId);
				Queue.Enqueue(FBFSNode::MakeCell(CellId));
			}
		}
	}

	//=========================================================================
	// Step 2: BFS 탐색
	//=========================================================================
	while (!Queue.IsEmpty())
	{
		FBFSNode Current;
		Queue.Dequeue(Current);

		if (Current.bIsSupercell)
		{
			// Case A: Intact SuperCell 노드
			ProcessSupercellNode(
				Current.Id,
				Cache,
				SupercellCache,
				CellState,
				bEnableSubcell,
				Queue,
				ConnectedCells,
				VisitedSupercells);
		}
		else
		{
			// Case B: 개별 Cell 노드
			ProcessCellNode(
				Current.Id,
				Cache,
				SupercellCache,
				CellState,
				bEnableSubcell,
				Queue,
				ConnectedCells,
				VisitedSupercells);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[HierarchicalBFS] Connected: %d cells, Visited SuperCells: %d"),
		ConnectedCells.Num(), VisitedSupercells.Num());

	return ConnectedCells;
}

TSet<int32> FCellDestructionSystem::FindDisconnectedCellsHierarchicalLevel(
	const FGridCellCache& Cache,
	FSupercellCache& SupercellCache,
	const FCellState& CellState,
	bool bEnableSubcell)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FindDisconnectedCellsHierarchicalLevel);

	// 1. 앵커에 연결된 Cell 찾기
	TSet<int32> ConnectedCells = FindConnectedCellsHierarchical(
		Cache, SupercellCache, CellState, bEnableSubcell);

	// 2. 전체 Grid에서 Connected가 아닌 Cell = Disconnected
	TSet<int32> Disconnected;

	for (int32 CellId = 0; CellId < Cache.GetTotalCellCount(); ++CellId)
	{
		if (!Cache.GetCellExists(CellId))
		{
			continue;
		}

		if (CellState.DestroyedCells.Contains(CellId))
		{
			continue;
		}

		if (!ConnectedCells.Contains(CellId))
		{
			Disconnected.Add(CellId);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[HierarchicalBFS] Disconnected: %d cells"),
		Disconnected.Num());

	return Disconnected;
}
