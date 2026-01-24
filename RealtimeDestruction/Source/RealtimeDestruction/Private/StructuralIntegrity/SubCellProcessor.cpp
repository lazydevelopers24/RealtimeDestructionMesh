// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "StructuralIntegrity/SubCellProcessor.h"

// SubCell 디버그 로그 활성화
#define SUBCELL_DEBUG_LOG 0

#if SUBCELL_DEBUG_LOG
DEFINE_LOG_CATEGORY_STATIC(LogSubCellDebug, Log, All);
#endif

bool FSubCellProcessor::ProcessSubCellDestruction(
	const FQuantizedDestructionInput& QuantizedShape,
	const FTransform& MeshTransform,
	const FGridCellLayout& GridLayout,
	FCellState& InOutCellState,
	TArray<int32>& OutAffectedCells,
	TMap<int32, TArray<int32>>* OutNewlyDeadSubCells)
{
	OutAffectedCells.Reset();

	if (!GridLayout.IsValid())
	{
		return false;
	}

	// 1. Tool shape의 AABB로 후보 cell 필터링 (월드 공간)
	const FBox ShapeAABB = ComputeShapeAABB(QuantizedShape);
	const TArray<int32> CandidateCells = GridLayout.GetCellsInAABB(ShapeAABB, MeshTransform);

#if SUBCELL_DEBUG_LOG
	UE_LOG(LogSubCellDebug, Log, TEXT("=== ProcessSubCellDestruction ==="));
	UE_LOG(LogSubCellDebug, Log, TEXT("Shape Type: %d, CandidateCells: %d"), (int32)QuantizedShape.Type, CandidateCells.Num());
#endif

	if (CandidateCells.Num() == 0)
	{
		return false;
	}

	// 2. 각 후보 cell의 subcell 검사 (월드 공간 OBB 교차 검사)
	for (int32 CellId : CandidateCells)
	{
		// 이미 완전히 파괴된 cell은 스킵
		if (InOutCellState.DestroyedCells.Contains(CellId))
		{
			continue;
		}

		// SubCell 상태 가져오기 (없으면 생성)
		FSubCell& SubCellState = InOutCellState.SubCellStates.FindOrAdd(CellId);

		bool bAnySubCellNewlyDead = false;
		TArray<int32> NewlyDeadSubCells;

#if SUBCELL_DEBUG_LOG
		const FIntVector CellCoord = GridLayout.IdToCoord(CellId);
		UE_LOG(LogSubCellDebug, Log, TEXT("  Checking CellId=%d (Coord: %d,%d,%d)"), CellId, CellCoord.X, CellCoord.Y, CellCoord.Z);
#endif

		// 3. 각 subcell이 tool shape과 교차하는지 검사 (월드 공간 OBB)
		for (int32 SubCellId = 0; SubCellId < SUBCELL_COUNT; ++SubCellId)
		{
			// 이미 dead인 subcell은 스킵
			if (!SubCellState.IsSubCellAlive(SubCellId))
			{
				continue;
			}

			// SubCell의 월드 공간 OBB (메시의 회전과 비균일 스케일 정확히 반영)
			const FCellOBB SubCellOBB = GridLayout.GetSubCellWorldOBB(CellId, SubCellId, MeshTransform);

			// 월드 공간에서 Shape-OBB 교차 검사
			const bool bIntersects = QuantizedShape.IntersectsOBB(SubCellOBB);

#if SUBCELL_DEBUG_LOG
			UE_LOG(LogSubCellDebug, Log, TEXT("    SubCell %d: %s"),SubCellId, bIntersects ? TEXT("HIT") : TEXT("miss"));
#endif

			if (bIntersects)
			{
				SubCellState.DestroySubCell(SubCellId);
				bAnySubCellNewlyDead = true;

				if (OutNewlyDeadSubCells)
				{
					NewlyDeadSubCells.Add(SubCellId);
				}
			}
		}

		// 4. 영향받은 cell 기록
		if (bAnySubCellNewlyDead)
		{
			OutAffectedCells.Add(CellId);

#if SUBCELL_DEBUG_LOG
			FString DeadSubCellsStr;
			for (int32 i = 0; i < SUBCELL_COUNT; ++i)
			{
				DeadSubCellsStr += SubCellState.IsSubCellAlive(i) ? TEXT("O") : TEXT("X");
			}
			UE_LOG(LogSubCellDebug, Log, TEXT("  -> CellId=%d SubCell States: [%s] (O=Alive, X=Dead)"), CellId, *DeadSubCellsStr);
#endif

			if (OutNewlyDeadSubCells && NewlyDeadSubCells.Num() > 0)
			{
				OutNewlyDeadSubCells->Add(CellId, MoveTemp(NewlyDeadSubCells));
			}

			// 모든 subcell이 파괴되었으면 cell 자체를 파괴 상태로
			if (SubCellState.IsFullyDestroyed())
			{
				InOutCellState.DestroyedCells.Add(CellId);
				InOutCellState.SubCellStates.Remove(CellId);
#if SUBCELL_DEBUG_LOG
				UE_LOG(LogSubCellDebug, Log, TEXT("  -> CellId=%d FULLY DESTROYED"), CellId);
#endif
			}
		}
	}

	return true;
}

int32 FSubCellProcessor::CountLiveSubCells(int32 CellId, const FCellState& CellState)
{
	// Cell이 완전히 파괴되었으면 0
	if (CellState.DestroyedCells.Contains(CellId))
	{
		return 0;
	}

	const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);

	// SubCell 상태가 없으면 모든 subcell이 살아있음
	if (!SubCellState)
	{
		return SUBCELL_COUNT;
	}

	// 살아있는 subcell 개수 세기
	int32 Count = 0;
	for (int32 SubCellId = 0; SubCellId < SUBCELL_COUNT; ++SubCellId)
	{
		if (SubCellState->IsSubCellAlive(SubCellId))
		{
			Count++;
		}
	}

	return Count;
}

bool FSubCellProcessor::IsCellFullyDestroyed(int32 CellId, const FCellState& CellState)
{
	// DestroyedCells에 있으면 완전 파괴
	if (CellState.DestroyedCells.Contains(CellId))
	{
		return true;
	}

	// SubCell 상태 확인
	const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);
	if (SubCellState)
	{
		return SubCellState->IsFullyDestroyed();
	}

	// SubCell 상태가 없으면 아직 파괴되지 않음
	return false;
}

TArray<int32> FSubCellProcessor::GetBoundarySubCellIds(int32 Direction)
{
	TArray<int32> Result;
	Result.Reserve(SUBCELL_DIVISION * SUBCELL_DIVISION);  // 5x5 = 25

	// 방향에 따라 고정되는 축 결정
	// 0: -X (x=0), 1: +X (x=4), 2: -Y (y=0), 3: +Y (y=4), 4: -Z (z=0), 5: +Z (z=4)
	const int32 FixedAxis = Direction / 2;  // 0=X, 1=Y, 2=Z
	const int32 FixedValue = (Direction % 2 == 0) ? 0 : (SUBCELL_DIVISION - 1);

	for (int32 A = 0; A < SUBCELL_DIVISION; ++A)
	{
		for (int32 B = 0; B < SUBCELL_DIVISION; ++B)
		{
			int32 X, Y, Z;

			switch (FixedAxis)
			{
			case 0:  // X 고정
				X = FixedValue;
				Y = A;
				Z = B;
				break;
			case 1:  // Y 고정
				X = A;
				Y = FixedValue;
				Z = B;
				break;
			case 2:  // Z 고정
				X = A;
				Y = B;
				Z = FixedValue;
				break;
			default:
				continue;
			}

			Result.Add(SubCellCoordToId(X, Y, Z));
		}
	}

	return Result;
}

uint32 FSubCellProcessor::GetBoundaryLiveSubCellMask(int32 CellId, int32 Direction, const FCellState& CellState)
{
	// Cell이 완전히 파괴되었으면 0
	if (CellState.DestroyedCells.Contains(CellId))
	{
		return 0;
	}

	const FSubCell* SubCellState = CellState.SubCellStates.Find(CellId);

	uint32 Mask = 0;
	const TArray<int32> BoundarySubCells = GetBoundarySubCellIds(Direction);

	for (int32 i = 0; i < BoundarySubCells.Num(); ++i)
	{
		const int32 SubCellId = BoundarySubCells[i];

		bool bIsLive;
		if (SubCellState)
		{
			bIsLive = SubCellState->IsSubCellAlive(SubCellId);
		}
		else
		{
			bIsLive = true;  // SubCell 상태가 없으면 모두 살아있음
		}

		if (bIsLive)
		{
			Mask |= (1u << i);
		}
	}

	return Mask;
}

FBox FSubCellProcessor::ComputeShapeAABB(const FQuantizedDestructionInput& Shape)
{
	// mm → cm 변환 (0.1 곱하기)
	const FVector Center = FVector(Shape.CenterMM) * 0.1;
	const float Radius = Shape.RadiusMM * 0.1f;
	const FVector BoxExtent = FVector(Shape.BoxExtentMM) * 0.1;
	const FVector EndPoint = FVector(Shape.EndPointMM) * 0.1;
	const float LineThickness = Shape.LineThicknessMM * 0.1f;

	switch (Shape.Type)
	{
	case ECellDestructionShapeType::Sphere:
		return FBox(
			Center - FVector(Radius),
			Center + FVector(Radius)
		);

	case ECellDestructionShapeType::Box:
		{
			const FRotator Rotation = FRotator(
				Shape.RotationCentidegrees.Y * 0.01f,
				Shape.RotationCentidegrees.Z * 0.01f,
				Shape.RotationCentidegrees.X * 0.01f
			);

			if (Rotation.IsNearlyZero())
			{
				// 회전 없음: 간단한 AABB
				return FBox(
					Center - BoxExtent,
					Center + BoxExtent
				);
			}
			else
			{
				// 회전 있음: 8개 꼭지점으로 AABB 계산
				FBox Result(ForceInit);
				const FQuat Rot = Rotation.Quaternion();

				for (int32 i = 0; i < 8; ++i)
				{
					const FVector LocalCorner(
						((i & 1) ? BoxExtent.X : -BoxExtent.X),
						((i & 2) ? BoxExtent.Y : -BoxExtent.Y),
						((i & 4) ? BoxExtent.Z : -BoxExtent.Z)
					);
					Result += Center + Rot.RotateVector(LocalCorner);
				}
				return Result;
			}
		}

	case ECellDestructionShapeType::Cylinder:
		{
			const FVector CylinderExtent(Radius, Radius, BoxExtent.Z);
			const FRotator Rotation(
				Shape.RotationCentidegrees.X * 0.01f,
				Shape.RotationCentidegrees.Y * 0.01f,
				Shape.RotationCentidegrees.Z * 0.01f
			);

			if (Rotation.IsNearlyZero())
			{
				return FBox(
					Center - CylinderExtent,
					Center + CylinderExtent
				);
			}

			FBox Result(ForceInit);
			const FQuat Rot = Rotation.Quaternion();
			for (int32 i = 0; i < 8; ++i)
			{
				const FVector LocalCorner(
					((i & 1) ? CylinderExtent.X : -CylinderExtent.X),
					((i & 2) ? CylinderExtent.Y : -CylinderExtent.Y),
					((i & 4) ? CylinderExtent.Z : -CylinderExtent.Z)
				);
				Result += Center + Rot.RotateVector(LocalCorner);
			}
			return Result;
		}

	case ECellDestructionShapeType::Line:
		{
			// 선분의 AABB + 두께
			FBox Result(ForceInit);
			Result += Center;
			Result += EndPoint;
			return Result.ExpandBy(LineThickness);
		}
	}

	return FBox(ForceInit);
}
