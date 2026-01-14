// Copyright 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GridCellTypes.generated.h"

// Forward declaration
struct FRealtimeDestructionRequest;

//=========================================================================
// SubCell 설정 상수
//=========================================================================

/** SubCell 분할 수 (각 축당) - 2x2x2 = 8개 subcell */
inline constexpr int32 SUBCELL_DIVISION = 2;

/** 총 SubCell 개수 */
inline constexpr int32 SUBCELL_COUNT = SUBCELL_DIVISION * SUBCELL_DIVISION * SUBCELL_DIVISION;  // 8

/** SubCell 3D 좌표 -> SubCell ID */
inline constexpr int32 SubCellCoordToId(int32 X, int32 Y, int32 Z)
{
	return Z * (SUBCELL_DIVISION * SUBCELL_DIVISION) + Y * SUBCELL_DIVISION + X;
}

/** SubCell ID -> 3D 좌표 */
inline FIntVector SubCellIdToCoord(int32 SubCellId)
{
	const int32 XY = SUBCELL_DIVISION * SUBCELL_DIVISION;
	const int32 Z = SubCellId / XY;
	const int32 Remainder = SubCellId % XY;
	const int32 Y = Remainder / SUBCELL_DIVISION;
	const int32 X = Remainder % SUBCELL_DIVISION;
	return FIntVector(X, Y, Z);
}

/** 6방향 오프셋 (±X, ±Y, ±Z) */
inline constexpr int32 DIRECTION_OFFSETS[6][3] = {
	{-1, 0, 0},  // -X
	{+1, 0, 0},  // +X
	{0, -1, 0},  // -Y
	{0, +1, 0},  // +Y
	{0, 0, -1},  // -Z
	{0, 0, +1},  // +Z
};

/**
 * Cell 손상 수준
 * SubCell 상태에 따라 결정됨
 */
UENUM(BlueprintType)
enum class ECellDamageLevel : uint8
{
	Intact,     // 모든 SubCell 살아있음
	Damaged,    // 일부 SubCell 파괴됨
	Destroyed   // 모든 SubCell 파괴됨
};

/**
 * 파괴 형태 타입
 */
UENUM(BlueprintType)
enum class EDestructionShapeType : uint8
{
	Sphere,     // 구체 (폭발)
	Box,        // 박스 (브리칭)
	Cylinder,   // 원통
	Line        // 선형 (총알)
};

/**
 * int32 배열 래퍼 (UPROPERTY 지원용)
 */
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

	// Range-based for loop 지원
	TArray<int32>::RangedForIteratorType begin() { return Values.begin(); }
	TArray<int32>::RangedForIteratorType end() { return Values.end(); }
	TArray<int32>::RangedForConstIteratorType begin() const { return Values.begin(); }
	TArray<int32>::RangedForConstIteratorType end() const { return Values.end(); }
};

/**
 * 파괴 형태 정의
 * Note: 현재 원통은 Rotation 값은 있지만 계산에 포함되지 않아 항상 Z축과 평행한 방향만 지원.
 * 회전값 줄 경우 Line shape 사용할 것.
 * 추후 Line과 Cylinder를 통합하거나 Cylinder를 Axis-Aligned용으로 분리 요망.
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FCellDestructionShape
{
	GENERATED_BODY()

	/** 형태 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EDestructionShapeType Type = EDestructionShapeType::Sphere;

	/** 중심점 (월드 좌표) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector Center = FVector::ZeroVector;

	/** 구체/원통 반경 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Radius = 50.0f;

	/** 박스 범위 (박스 타입용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector BoxExtent = FVector::ZeroVector;

	/** 회전 (박스/원통용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FRotator Rotation = FRotator::ZeroRotator;

	/** 선형 파괴용 끝점 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector EndPoint = FVector::ZeroVector;

	/** 선형 파괴 두께 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float LineThickness = 5.0f;

	/** 점이 파괴 영역 안에 있는지 확인 */
	bool ContainsPoint(const FVector& Point) const;

	/** FRealtimeDestructionRequest로부터 FCellDestructionShape 생성 */
	static FCellDestructionShape CreateFromRequest(const FRealtimeDestructionRequest& Request);
};

/**
 * 양자화된 파괴 입력
 * 모든 클라이언트에서 동일한 Boolean 결과를 보장하기 위해 양자화
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FQuantizedDestructionInput
{
	GENERATED_BODY()

	/** 파괴 형태 타입 */
	UPROPERTY()
	EDestructionShapeType Type = EDestructionShapeType::Sphere;

	/** 중심점 (mm 단위, 정수) - cm * 10 */
	UPROPERTY()
	FIntVector CenterMM = FIntVector::ZeroValue;

	/** 반경 (mm 단위, 정수) - cm * 10 */
	UPROPERTY()
	int32 RadiusMM = 0;

	/** 박스 범위 (mm 단위) */
	UPROPERTY()
	FIntVector BoxExtentMM = FIntVector::ZeroValue;

	/** 회전 (0.01도 단위, 정수) */
	UPROPERTY()
	FIntVector RotationCentidegrees = FIntVector::ZeroValue;

	/** 선형 파괴 끝점 (mm 단위) */
	UPROPERTY()
	FIntVector EndPointMM = FIntVector::ZeroValue;

	/** 선형 파괴 두께 (mm 단위) */
	UPROPERTY()
	int32 LineThicknessMM = 0;

	/** float 값에서 양자화된 입력 생성 */
	static FQuantizedDestructionInput FromDestructionShape(const FCellDestructionShape& Shape);

	/** 양자화된 값을 DestructionShape로 복원 */
	FCellDestructionShape ToDestructionShape() const;

	/** 점이 파괴 영역 안에 있는지 확인 (양자화된 값 기반) */
	bool ContainsPoint(const FVector& Point) const;
};

/**
 * 3D 격자 셀 캐시 (메모리 최적화 버전)
 * 에디터에서 생성되어 저장됨. 런타임에서 변경 없음.
 *
 * 최적화:
 * 1. CellCenters 제거 - IdToLocalCenter()로 런타임 계산
 * 2. CellExists/CellIsAnchor → 비트필드 (1/8 메모리)
 * 3. CellTriangles/CellNeighbors → 희소 배열 (유효 셀만 저장)
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FGridCellCache
{
	GENERATED_BODY()

	//=========================================================================
	// 격자 정보
	//=========================================================================

	/** 격자 크기 (X, Y, Z 방향 셀 개수) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FIntVector GridSize = FIntVector::ZeroValue;

	/** 각 셀의 월드 크기 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FVector CellSize = FVector(5.0f, 5.0f, 5.0f);

	/** 격자 원점 (메시 바운드의 Min) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector GridOrigin = FVector::ZeroVector;

	/** 메시 스케일 (빌드 시점의 컴포넌트 스케일) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	FVector MeshScale = FVector::OneVector;

	//=========================================================================
	// 비트필드 데이터 (메모리 최적화)
	//=========================================================================

	/** 셀 존재 여부 비트필드 (32개 셀당 1 uint32) */
	UPROPERTY()
	TArray<uint32> CellExistsBits;

	/** 앵커 셀 여부 비트필드 (32개 셀당 1 uint32) */
	UPROPERTY()
	TArray<uint32> CellIsAnchorBits;

	//=========================================================================
	// 희소 배열 데이터 (유효 셀만 저장)
	//=========================================================================

	/** 유효 셀 ID → 희소 인덱스 매핑 */
	UPROPERTY()
	TMap<int32, int32> CellIdToSparseIndex;

	/** 희소 인덱스 → 셀 ID 역매핑 */
	UPROPERTY()
	TArray<int32> SparseIndexToCellId;

	/** 희소 배열: 셀별 삼각형 인덱스 (유효 셀만) */
	UPROPERTY()
	TArray<FIntArray> SparseCellTriangles;

	/** 희소 배열: 셀별 인접 셀 ID 목록 (유효 셀만) */
	UPROPERTY()
	TArray<FIntArray> SparseCellNeighbors;

	//=========================================================================
	// 비트필드 접근자
	//=========================================================================

	/** 셀 존재 여부 확인 */
	FORCEINLINE bool GetCellExists(int32 CellId) const
	{
		const int32 WordIndex = CellId >> 5;  // CellId / 32
		const uint32 BitMask = 1u << (CellId & 31);  // CellId % 32
		return CellExistsBits.IsValidIndex(WordIndex) && (CellExistsBits[WordIndex] & BitMask) != 0;
	}

	/** 셀 존재 여부 설정 */
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

	/** 앵커 셀 여부 확인 */
	FORCEINLINE bool GetCellIsAnchor(int32 CellId) const
	{
		const int32 WordIndex = CellId >> 5;
		const uint32 BitMask = 1u << (CellId & 31);
		return CellIsAnchorBits.IsValidIndex(WordIndex) && (CellIsAnchorBits[WordIndex] & BitMask) != 0;
	}

	/** 앵커 셀 여부 설정 */
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
	// 희소 배열 접근자
	//=========================================================================

	/** 셀의 삼각형 인덱스 배열 반환 (없으면 빈 배열) */
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

	/** 셀의 이웃 배열 반환 (없으면 빈 배열) */
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

	/** 셀의 삼각형 인덱스 배열에 접근 (수정용) */
	FIntArray* GetCellTrianglesMutable(int32 CellId)
	{
		const int32* SparseIdx = CellIdToSparseIndex.Find(CellId);
		if (SparseIdx && SparseCellTriangles.IsValidIndex(*SparseIdx))
		{
			return &SparseCellTriangles[*SparseIdx];
		}
		return nullptr;
	}

	/** 셀의 이웃 배열에 접근 (수정용) */
	FIntArray* GetCellNeighborsMutable(int32 CellId)
	{
		const int32* SparseIdx = CellIdToSparseIndex.Find(CellId);
		if (SparseIdx && SparseCellNeighbors.IsValidIndex(*SparseIdx))
		{
			return &SparseCellNeighbors[*SparseIdx];
		}
		return nullptr;
	}

	/** 유효 셀 등록 (희소 배열에 추가) */
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

	/** 유효 셀 개수 */
	int32 GetValidCellCount() const
	{
		return SparseIndexToCellId.Num();
	}

	/** 비트필드 초기화 (GridSize 설정 후 호출) */
	void InitializeBitfields()
	{
		const int32 TotalCells = GetTotalCellCount();
		const int32 RequiredWords = (TotalCells + 31) >> 5;  // ceil(TotalCells / 32)

		CellExistsBits.SetNumZeroed(RequiredWords);
		CellIsAnchorBits.SetNumZeroed(RequiredWords);
	}

	/** 유효 셀 순회용 반복자 (안전하게 빈 배열 반환) */
	const TArray<int32>& GetValidCellIds() const
	{
		return SparseIndexToCellId;
	}

	/** 희소 배열이 유효한지 확인 */
	bool HasValidSparseData() const
	{
		return SparseIndexToCellId.Num() > 0 &&
		       SparseCellTriangles.Num() == SparseIndexToCellId.Num() &&
		       SparseCellNeighbors.Num() == SparseIndexToCellId.Num();
	}

	//=========================================================================
	// 헬퍼 함수
	//=========================================================================

	/** 총 셀 개수 */
	int32 GetTotalCellCount() const
	{
		return GridSize.X * GridSize.Y * GridSize.Z;
	}

	/** 앵커 셀 개수 */
	int32 GetAnchorCount() const;

	/** 3D 좌표 -> 셀 ID */
	int32 CoordToId(int32 X, int32 Y, int32 Z) const
	{
		return Z * (GridSize.X * GridSize.Y) + Y * GridSize.X + X;
	}

	int32 CoordToId(const FIntVector& Coord) const
	{
		return CoordToId(Coord.X, Coord.Y, Coord.Z);
	}

	/** 셀 ID -> 3D 좌표 */
	FIntVector IdToCoord(int32 CellId) const
	{
		const int32 XY = GridSize.X * GridSize.Y;
		const int32 Z = CellId / XY;
		const int32 Remainder = CellId % XY;
		const int32 Y = Remainder / GridSize.X;
		const int32 X = Remainder % GridSize.X;
		return FIntVector(X, Y, Z);
	}

	/** 좌표가 유효한지 확인 */
	bool IsValidCoord(const FIntVector& Coord) const
	{
		return Coord.X >= 0 && Coord.X < GridSize.X &&
		       Coord.Y >= 0 && Coord.Y < GridSize.Y &&
		       Coord.Z >= 0 && Coord.Z < GridSize.Z;
	}

	/** 셀 ID가 유효한지 확인 */
	bool IsValidCellId(int32 CellId) const
	{
		return CellId >= 0 && CellId < GetTotalCellCount();
	}

	/** 월드 위치 -> 셀 ID (-1 if invalid) */
	int32 WorldPosToId(const FVector& WorldPos, const FTransform& MeshTransform) const;

	/** 셀 ID -> 월드 중심점 */
	FVector IdToWorldCenter(int32 CellId, const FTransform& MeshTransform) const;

	/** 셀 ID -> 로컬 중심점 */
	FVector IdToLocalCenter(int32 CellId) const;

	/** 셀 ID -> 월드 Min 좌표 */
	FVector IdToWorldMin(int32 CellId, const FTransform& MeshTransform) const;

	/** 셀 ID -> 로컬 Min 좌표 */
	FVector IdToLocalMin(int32 CellId) const;

	/** 셀의 8개 꼭지점 반환 (로컬 좌표) */
	TArray<FVector> GetCellVertices(int32 CellId) const;

	/** 캐시 초기화 */
	void Reset();

	/** 유효성 검사 */
	bool IsValid() const;

	//=========================================================================
	// SubCell 헬퍼 함수
	//=========================================================================

	/** SubCell 크기 (로컬 좌표) */
	FVector GetSubCellSize() const
	{
		return CellSize / static_cast<float>(SUBCELL_DIVISION);
	}

	/** SubCell 로컬 중심점 (Cell 로컬 좌표 기준) */
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

	/** SubCell 로컬 중심점 (메시 로컬 좌표) */
	FVector GetSubCellLocalCenter(int32 CellId, int32 SubCellId) const
	{
		const FVector CellMin = IdToLocalMin(CellId);
		return CellMin + GetSubCellLocalOffset(SubCellId);
	}

	/** SubCell 월드 중심점 */
	FVector GetSubCellWorldCenter(int32 CellId, int32 SubCellId, const FTransform& MeshTransform) const
	{
		const FVector LocalCenter = GetSubCellLocalCenter(CellId, SubCellId);
		return MeshTransform.TransformPosition(LocalCenter);
	}

	/** SubCell 월드 바운딩 박스 */
	FBox GetSubCellWorldBox(int32 CellId, int32 SubCellId, const FTransform& MeshTransform) const
	{
		const FVector CellMin = IdToLocalMin(CellId);
		const FIntVector SubCoord = SubCellIdToCoord(SubCellId);
		const FVector SubCellSz = GetSubCellSize();

		const FVector LocalMin = CellMin + FVector(
			SubCoord.X * SubCellSz.X,
			SubCoord.Y * SubCellSz.Y,
			SubCoord.Z * SubCellSz.Z
		);
		const FVector LocalMax = LocalMin + SubCellSz;

		return FBox(
			MeshTransform.TransformPosition(LocalMin),
			MeshTransform.TransformPosition(LocalMax)
		);
	}

	/** AABB 내에 있는 Cell ID 목록 반환 */
	TArray<int32> GetCellsInAABB(const FBox& WorldAABB, const FTransform& MeshTransform) const;
};

/** Subcell 비트마스크 (8개 subcell, 2x2x2) */
USTRUCT()
struct FSubCell
{
	GENERATED_BODY()

	/**
	 * 비트마스크 (각 비트가 subcell 존재 여부)
	 * 0 = Dead, 1 = Alive
	 * 8비트로 8개 SubCell 상태 표현
	 * SubCellId = X + Y * 2 + Z * 4
	 */
	UPROPERTY()
	uint8 Bits = 0xFF;  // 모든 SubCell 살아있음

	bool IsSubCellAlive(int32 SubCellId) const
	{
		return (Bits & (1 << SubCellId)) != 0;
	}

	void DestroySubCell(int32 SubCellId)
	{
		Bits &= ~(1 << SubCellId);
	}

	/** 모든 subcell이 파괴되었는지 확인 */
	bool IsFullyDestroyed() const
	{
		return Bits == 0;
	}

	/** 초기화 (모든 subcell 존재) */
	void Reset()
	{
		Bits = 0xFF;
	}
};

/**
 * 파괴 처리 결과
 * ProcessDestruction 또는 ProcessSubCellDestruction 호출 시 반환되는 일회성 리포트
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDestructionResult
{
	GENERATED_BODY()

	/** 새로 파괴된 SubCell 개수 */
	UPROPERTY()
	int32 DeadSubCellCount = 0;

	/** SubCell이 영향받은 Cell 목록 */
	UPROPERTY()
	TArray<int32> AffectedCells;

	/** 새로 파괴된 SubCell 정보 (CellId -> SubCellId 목록) */
	UPROPERTY()
	TMap<int32, FIntArray> NewlyDeadSubCells;

	/** Destroyed 상태로 전환된 Cell 목록 (모든 SubCell 파괴됨) */
	UPROPERTY()
	TArray<int32> NewlyDestroyedCells;

	/** 파괴가 발생했는지 */
	bool HasAnyDestruction() const
	{
		return DeadSubCellCount > 0 || NewlyDestroyedCells.Num() > 0;
	}
};

/**
 * 런타임 셀 상태 (Replicated)
 * 파괴된 셀과 분리된 셀 그룹 관리
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FCellState
{
	GENERATED_BODY()

	/** 완전히 파괴된 셀 ID 집합 */
	UPROPERTY()
	TSet<int32> DestroyedCells;

	/** 분리된 셀 그룹 (아직 파편으로 스폰되지 않은 상태) */
	UPROPERTY()
	TArray<FIntArray> DetachedGroups;

	/**
	 * Subcell 상태 관리.
	 * Destruction Shape와 접촉한 Cell은 죽은 subcell이 발생하며 여기에 추가.
	 * 여기 추가되지 않은 Cell은 모든 subcell이 다 생존한 상태.
	 */
	UPROPERTY()
	TMap<int32, FSubCell> SubCellStates;

	/** 셀이 파괴되었는지 확인 */
	bool IsCellDestroyed(int32 CellId) const
	{
		return DestroyedCells.Contains(CellId);
	}

	/** SubCell이 살아있는지 확인 */
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

		// SubCell 상태가 없으면 모두 살아있음
		return true;
	}

	/** 셀이 분리 대기 중인지 확인 */
	bool IsCellDetached(int32 CellId) const
	{
		for (const FIntArray& Group : DetachedGroups)
		{
			if (Group.Values.Contains(CellId))
			{
				return true;
			}
		}
		return false;
	}

	/** 셀 파괴 */
	void DestroyCells(const TArray<int32>& CellIds)
	{
		for (int32 CellId : CellIds)
		{
			DestroyedCells.Add(CellId);
		}
	}

	/** 분리된 그룹 추가 */
	void AddDetachedGroup(const TArray<int32>& CellIds)
	{
		FIntArray Group;
		Group.Values = CellIds;
		DetachedGroups.Add(Group);
	}

	/** 분리된 그룹을 파괴 상태로 전환 (파편 스폰 후 호출) */
	void MoveDetachedToDestroyed(int32 GroupIndex)
	{
		if (DetachedGroups.IsValidIndex(GroupIndex))
		{
			for (int32 CellId : DetachedGroups[GroupIndex].Values)
			{
				DestroyedCells.Add(CellId);
			}
			DetachedGroups.RemoveAt(GroupIndex);
		}
	}

	/** 모든 분리 그룹을 파괴 상태로 전환 */
	void MoveAllDetachedToDestroyed()
	{
		for (const FIntArray& Group : DetachedGroups)
		{
			for (int32 CellId : Group.Values)
			{
				DestroyedCells.Add(CellId);
			}
		}
		DetachedGroups.Empty();
	}

	/** 상태 초기화 */
	void Reset()
	{
		DestroyedCells.Empty();
		DetachedGroups.Empty();
	}
};

/**
 * SubCell 기반 Detached Group
 * Cell 단위 분리 판정 후 인접 SubCell까지 포함하는 확장된 그룹
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedGroupWithSubCell
{
	GENERATED_BODY()

	/** 완전히 분리된 Cell ID 목록 (Cell 단위 BFS로 판정) */
	UPROPERTY()
	TArray<int32> DetachedCellIds;

	/**
	 * 추가로 포함된 SubCell 목록 (SubCell Flooding으로 판정)
	 * Key: CellId, Value: 해당 Cell에서 포함된 SubCellId 목록
	 * - Detached Cell과 인접한 살아있는 SubCell들 (주황)
	 * - Flooding 경계의 죽은 SubCell들 (빗금)
	 */
	UPROPERTY()
	TMap<int32, FIntArray> IncludedSubCells;

	/** 그룹이 비어있는지 확인 */
	bool IsEmpty() const
	{
		return DetachedCellIds.Num() == 0 && IncludedSubCells.Num() == 0;
	}
};

/**
 * 분리된 파편 정보
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDetachedDebrisInfo
{
	GENERATED_BODY()

	/** 파편 고유 ID */
	UPROPERTY()
	int32 DebrisId = 0;

	/** 포함된 셀 ID 목록 */
	UPROPERTY()
	TArray<int32> CellIds;

	/** 초기 위치 */
	UPROPERTY()
	FVector_NetQuantize InitialLocation;

	/** 초기 속도 */
	UPROPERTY()
	FVector_NetQuantize InitialVelocity;
};

/**
 * 배칭된 파괴 이벤트 (서버 -> 클라이언트)
 * 16.6ms 동안 쌓인 모든 파괴를 한 번에 전송
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FBatchedDestructionEvent
{
	GENERATED_BODY()

	/** 양자화된 파괴 입력들 (Boolean 렌더링용) */
	UPROPERTY()
	TArray<FQuantizedDestructionInput> DestructionInputs;

	/** 총 파괴된 셀 ID (중복 제거됨) */
	UPROPERTY()
	TArray<int16> DestroyedCellIds;

	/** 분리된 파편들 */
	UPROPERTY()
	TArray<FDetachedDebrisInfo> DetachedDebris;
};
