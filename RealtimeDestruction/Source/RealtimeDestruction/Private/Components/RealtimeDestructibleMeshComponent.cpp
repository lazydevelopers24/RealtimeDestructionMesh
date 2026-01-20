// Fill out your copyright notice in the Description page of Project Settings.

#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Materials/MaterialInstanceDynamic.h"

// GeometryCollection
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionComponent.h"

#include "Settings/RDMSetting.h"
#if WITH_EDITOR
#include "GeometryCollection/GeometryCollectionConversion.h"
//Fracturing
#include "FractureSettings.h"
#include "FractureEngineFracturing.h"
#include "Editor.h"
#endif

// Packaging
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "DrawDebugHelpers.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "DynamicMesh/MeshTransforms.h"
#include "Selections/MeshConnectedComponents.h"
#include "Operations/MeshBoolean.h"
#include "BooleanProcessor/RealtimeBooleanProcessor.h"

#include "BulletClusterComponent.h"
#include "Algo/Unique.h"
#include "StructuralIntegrity/CellDestructionSystem.h"
#include "Data/DecalMaterialDataAsset.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Subsystems/DestructionGameInstanceSubsystem.h"

//////////////////////////////////////////////////////////////////////////
// FCompactDestructionOp 구현 (언리얼 내장 NetQuantize 사용)
//////////////////////////////////////////////////////////////////////////

FCompactDestructionOp FCompactDestructionOp::Compress(const FRealtimeDestructionRequest& Request, int32 Seq)
{
	FCompactDestructionOp Compact;

	// FVector_NetQuantize는 FVector와 호환 - 자동 변환
	Compact.ImpactPoint = Request.ImpactPoint;
	Compact.ImpactNormal = Request.ImpactNormal;
	Compact.ToolForwardVector = Request.ToolForwardVector;

	Compact.ToolCenterWorld = Request.ToolCenterWorld;

	// 반지름 압축 (1-255 cm) - ShapeParams에서 가져옴
	Compact.Radius = static_cast<uint8>(FMath::Clamp(Request.ShapeParams.Radius, 1.0f, 255.0f));

	// 시퀀스 (롤오버)
	Compact.Sequence = static_cast<uint16>(Seq & 0xFFFF);

	// ToolShape와 ShapeParams 복사
	Compact.ToolShape = Request.ToolShape;
	Compact.ShapeParams = Request.ShapeParams;

	// ChunkIndex 저장 (클라이언트가 계산한 값)
	Compact.ChunkIndex = (Request.ChunkIndex >= 0 && Request.ChunkIndex < 256)
		? static_cast<uint8>(Request.ChunkIndex)
		: 0;
	
	Compact.DecalSize = Request.DecalSize;
	Compact.DecalConfigID = Request.DecalConfigID;
	Compact.SurfaceType = Request.SurfaceType;
	return Compact;
}

FRealtimeDestructionRequest FCompactDestructionOp::Decompress() const
{
	FRealtimeDestructionRequest Request;

	// FVector_NetQuantize → FVector 자동 변환
	Request.ImpactPoint = ImpactPoint;
	Request.ImpactNormal = FVector(ImpactNormal).GetSafeNormal();
	Request.ToolForwardVector = FVector(ToolForwardVector).GetSafeNormal();

	// ToolShape와 ShapeParams 복원
	Request.ToolShape = ToolShape;
	Request.ShapeParams = ShapeParams;

	// Depth 설정 (Shape에 따라)
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		Request.Depth = ShapeParams.Height;
		break;
	case EDestructionToolShape::Sphere:
		Request.Depth = ShapeParams.Radius;
		break;
	default:
		Request.Depth = ShapeParams.Height;
		break;
	}

	// ChunkIndex 복원
	Request.ChunkIndex = static_cast<int32>(ChunkIndex);

	// ToolCenterWorld 계산 - DestructionProjectileComponent::SetShapeParameters와 동일한 공식 사용
	constexpr float SurfaceMargin = 2.0f;
	constexpr float PenetrationOffset = 0.5f;

	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		// Cylinder: ImpactPoint에서 Forward 반대 방향으로 SurfaceMargin 만큼 이동
		Request.ToolCenterWorld = Request.ImpactPoint - (Request.ToolForwardVector * SurfaceMargin);
		break;
	case EDestructionToolShape::Sphere:
		// Sphere: ImpactPoint에서 Forward 방향으로 PenetrationOffset 만큼 이동
		//Request.ToolCenterWorld = Request.ImpactPoint + (Request.ToolForwardVector * PenetrationOffset);
		Request.ToolCenterWorld = ToolCenterWorld;
		break;
	default:
		Request.ToolCenterWorld = Request.ImpactPoint - (Request.ToolForwardVector * SurfaceMargin);
		break;
	}

	// Decal 관련 복원
	Request.DecalSize = DecalSize;
	Request.DecalConfigID = DecalConfigID;
	Request.SurfaceType = SurfaceType;
	Request.bSpawnDecal = true;  // 네트워크 요청은 기본적으로 데칼 생성

	return Request;
}

//////////////////////////////////////////////////////////////////////////

#include "Components/StaticMeshComponent.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "UDynamicMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Debug/DestructionDebugger.h"
#include "HAL/PlatformTime.h"
#include "BooleanProcessor/RealtimeBooleanProcessor.h"
#include "Components/DecalComponent.h"
#include "StructuralIntegrity/GridCellBuilder.h"

URealtimeDestructibleMeshComponent::URealtimeDestructibleMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = true;  // 서버 배칭용
	SetMobility(EComponentMobility::Movable);
	SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SetCollisionProfileName(TEXT("BlockAll"));
	SetCastShadow(true);

	/*
	 * 초기값 false
	 * 마지막 불리언 연산 시 true로 변경
	 */
	BooleanOptions.bFillHoles = false;
	BooleanOptions.bSimplifyOutput = false;
}

URealtimeDestructibleMeshComponent::URealtimeDestructibleMeshComponent(FVTableHelper& Helper)
{
}

URealtimeDestructibleMeshComponent::~URealtimeDestructibleMeshComponent()
{
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->Shutdown();
		BooleanProcessor.Reset();
	}
	// ChunkComponent들은 UPROPERTY이므로 GC에 의해 자동 정리됨
}

UMaterialInterface* URealtimeDestructibleMeshComponent::GetMaterial(int32 ElementIndex) const
{
	if (OverrideMaterials.IsValidIndex(ElementIndex))
	{
		if (UMaterialInterface* OverrideMaterial = OverrideMaterials[ElementIndex])
		{
			return OverrideMaterial;
		}
	}

	return Super::GetMaterial(ElementIndex);
}

bool URealtimeDestructibleMeshComponent::InitializeFromStaticMesh(UStaticMesh* InMesh)
{
	SourceStaticMesh = InMesh;
	return InitializeFromStaticMeshInternal(InMesh, false);
}

bool URealtimeDestructibleMeshComponent::InitializeFromStaticMeshComponent(UStaticMeshComponent* InComp)
{
	if (!InComp || !InComp->GetStaticMesh())
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: StaticMeshComponent or StaticMesh is null"));
		return false;
	}

	SourceStaticMesh = InComp->GetStaticMesh();
	SetWorldTransform(InComp->GetComponentTransform());
	SetCastShadow(InComp->CastShadow);

	if (!InitializeFromStaticMeshInternal(SourceStaticMesh, false))
	{
		return false;
	}

	CopyMaterialsFromStaticMeshComponent(InComp);
	CopyCollisionFromStaticMeshComponent(InComp);

	InComp->SetVisibility(false);
	InComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	return true;
}

void URealtimeDestructibleMeshComponent::ResetToSourceMesh()
{
	if (!SourceStaticMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: No source mesh to reset to"));
		return;
	}

	/*
	 * SourceMesh가 리셋되면 이전의 BooleanProcessor 작업을 무효화 해야함
	 *
	 */
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->CancelAllOperations();
	}
	
	CurrentHoleCount = 0;
	bIsInitialized = false;
	InitializeFromStaticMeshInternal(SourceStaticMesh, true);
}

// 현재는 RequestDestruction에서만 호출됨
FDestructionOpId URealtimeDestructibleMeshComponent::EnqueueRequestLocal(const FRealtimeDestructionRequest& Request, bool bIsPenetration, UDecalComponent* TemporaryDecal)
{
	if (!BooleanProcessor.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Boolean Processor is null"));
		return FDestructionOpId();
	}
	FRealtimeDestructionOp Op;
	Op.OpId.Value = NextOpId++;
	Op.Sequence = NextSequence++;
	Op.Request = Request;
	Op.bIsPenetration = bIsPenetration;

	/*
	 * 기존 구조는 BooleanProcessor에 캐싱된 OwnerComponent에서 FDynamicMesh3를 가져와서 연산하는 방식이었음
	 * 파괴 요청 시 CellMesh 넘겨줘야함
	 */
	if (Op.Request.ChunkIndex != INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[EnqueueRequestLocal] ChunkIndex=%d → BooleanProcessor->EnqueueOp 호출"),
			Op.Request.ChunkIndex);
		BooleanProcessor->EnqueueOp(MoveTemp(Op), TemporaryDecal, ChunkMeshComponents[Op.Request.ChunkIndex].Get());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[EnqueueRequestLocal] ChunkIndex=INDEX_NONE → Boolean 연산 스킵!"));
	}

	// if (!bEnableMultiWorkers)
	// {
	// 	BooleanProcessor->KickProcessIfNeededPerChunk();
	// }

	return Op.OpId;
}

int32 URealtimeDestructibleMeshComponent::EnqueueBatch(const TArray<FRealtimeDestructionRequest>& Requests)
{
	int32 AddedCount = 0;
	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		// 이 함수는 안쓰는 것 같으니 true로 하드코딩 
		EnqueueRequestLocal(Request, true);
		++AddedCount;
	}

	return AddedCount;
}

// Projectile에서 호출해줌
bool URealtimeDestructibleMeshComponent::RequestDestruction(const FRealtimeDestructionRequest& Request)
{
	if (!bAsyncEnabled)
	{
		UE_LOG(LogTemp, Warning, TEXT("Async flag is false. Please turn true"));
		return false;
	}
	
	if (bEnableClustering && BulletClusterComponent)
	{
		BulletClusterComponent->RegisterRequest(Request); 
	}
	
	return ExecuteDestructionInternal(Request);
}

bool URealtimeDestructibleMeshComponent::ExecuteDestructionInternal(const FRealtimeDestructionRequest& Request)
{
	if (MaxHoleCount > 0 && CurrentHoleCount >= MaxHoleCount)
	{
		return false;
	}
	// 관통, 비관통 여부 확인, broadphase와 같은 효과
	float AdjustPenetration;
	bool bIsPenetration = CheckPenetration(Request, AdjustPenetration);

	// Cell 상태 업데이트 (Boolean 처리와 별개로 수행)
	//UpdateCellStateFromDestruction(Request);
	FDestructionResult Result = DestructionLogic(Request);
	PendingDestructionResults.Add(Result);

	UDecalComponent* TempDecal = nullptr;
	if (!bIsPenetration && Request.bSpawnDecal)
	{
		TempDecal = SpawnTemporaryDecal(Request);
	}

	// Offset에 따라 추가 관통처리
	if (bIsPenetration)
	{
		FRealtimeDestructionRequest PenetrationRequest = Request;

		//	 // Cylinder 중심을 벽 중간으로 이동 (Normal 반대 방향으로 Height/2만큼)
		FVector Offset = Request.ImpactNormal * (-AdjustPenetration * 0.5f);
		PenetrationRequest.ImpactPoint = Request.ImpactPoint + Offset;
		PenetrationRequest.ToolShape = EDestructionToolShape::Cylinder;

		if (bDebugPenetration)
		{
			// 시각화
			DrawDebugLine(GetWorld(), Request.ImpactPoint, PenetrationRequest.ImpactPoint,
				FColor::Red, false, 5.0f, 0, 3.0f);
		}

		// 구멍을 추가로 내주는거니, Decal을 필요없다. 
		EnqueueRequestLocal(PenetrationRequest, true, TempDecal);
	}
	else
	{
		// 기본 관통을 Enqeue
		EnqueueRequestLocal(Request, bIsPenetration, TempDecal);
	}
	return true;	
}

//=============================================================================
// Cell 상태 업데이트
//=============================================================================

void URealtimeDestructibleMeshComponent::UpdateCellStateFromDestruction(const FRealtimeDestructionRequest& Request)
{ 
	// ===== 로그 추가 =====
	static int32 CallCount = 0;
	CallCount++;
	UE_LOG(LogTemp, Warning, TEXT("[UpdateCellState #%d] Called from somewhere"), CallCount);


	TRACE_CPUPROFILER_EVENT_SCOPE(UpdateCellStateFromDestruction);
	// 구조적 무결성 비활성화 또는 GridCellCache 미생성 시 스킵
	if (!bEnableStructuralIntegrity || !GridCellCache.IsValid())
	{
		return;
	}

	FDestructionResult Result = DestructionLogic(Request);

	TArray<FDestructionResult> Results;
	Results.Add(Result);
	DisconnectedCellStateLogic(Results);
	

}

FDestructionResult URealtimeDestructibleMeshComponent::DestructionLogic(const FRealtimeDestructionRequest& Request)
{
	FDestructionResult DestructionResult;

	// Request를 FDestructionShape로 변환
	FCellDestructionShape Shape;
	Shape.Center = Request.ImpactPoint;
	Shape.Radius = Request.ShapeParams.Radius;

	// ToolShape에 따른 타입 변환
	switch (Request.ToolShape)
	{
	case EDestructionToolShape::Sphere:
		Shape.Type = EDestructionShapeType::Sphere;
		break;
	case EDestructionToolShape::Cylinder:
		Shape.Type = EDestructionShapeType::Line;  // Cylinder는 회전 미지원, Line으로 처리
		// 방향 계산 (ImpactNormal 반대 방향으로 깊이만큼)
		Shape.EndPoint = Request.ImpactPoint - Request.ImpactNormal * Request.ShapeParams.Height;
		Shape.LineThickness = Request.ShapeParams.Radius;
		break;
	default:
		Shape.Type = EDestructionShapeType::Sphere;
		break;
	}

	// 양자화된 입력 생성
	FQuantizedDestructionInput QuantizedInput = FQuantizedDestructionInput::FromDestructionShape(Shape);

	//=====================================================================
	// Phase 1: Cell / SubCell 파괴 처리
	//=====================================================================
	if (bEnableSubcell)
	{
		DestructionResult = FCellDestructionSystem::ProcessCellDestructionWithSubCells(
			GridCellCache,
			QuantizedInput,
			GetComponentTransform(),
			CellState);
	}
	else
	{
		DestructionResult = FCellDestructionSystem::CalculateDestroyedCells(
			GridCellCache,
			QuantizedInput,
			GetComponentTransform(),
			CellState);
	}

	if (!DestructionResult.HasAnyDestruction())
	{
		return DestructionResult; // 파괴 없음
	}

	// 가장 최근 파괴된 셀 디버그 시각화를 위한 정보 갱신
	if (DestructionResult.NewlyDestroyedCells.Num() > 0)
	{
		RecentDirectDestroyedCellIds.Reset();
		RecentDirectDestroyedCellIds.Append(DestructionResult.NewlyDestroyedCells);
	}

	// 히스토리에 추가 (NarrowPhase용)
	DestructionInputHistory.Add(QuantizedInput);

	// 파괴된 셀 데이터 전송 (클라이언트 CellState 동기화)
	if (DestructionResult.NewlyDestroyedCells.Num() > 0)
	{
		MulticastDestroyedCells(DestructionResult.NewlyDestroyedCells);

		// 서버 Cell Collision: 파괴된 셀과 이웃 셀의 청크를 dirty 마킹
		if (bServerCellCollisionInitialized)
		{
			TSet<int32> DirtyChunkIndices;
			for (int32 CellId : DestructionResult.NewlyDestroyedCells)
			{
				// 해당 셀의 청크
				int32 ChunkIdx = GetCollisionChunkIndexForCell(CellId);
				if (ChunkIdx != INDEX_NONE)
				{
					DirtyChunkIndices.Add(ChunkIdx);
				}

				// 이웃 셀들의 청크도 dirty (새로 표면이 될 수 있음)
				const FIntArray& Neighbors = GridCellCache.GetCellNeighbors(CellId);
				for (int32 NeighborId : Neighbors.Values)
				{
					int32 NeighborChunkIdx = GetCollisionChunkIndexForCell(NeighborId);
					if (NeighborChunkIdx != INDEX_NONE)
					{
						DirtyChunkIndices.Add(NeighborChunkIdx);
					}
				}
			}

			for (int32 ChunkIdx : DirtyChunkIndices)
			{
				MarkCollisionChunkDirty(ChunkIdx);
			}

			UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Marked %d chunks dirty from %d destroyed cells"),
				DirtyChunkIndices.Num(), DestructionResult.NewlyDestroyedCells.Num());
		}
	}

	if (bEnableSubcell)
	{
		UE_LOG(LogTemp, Log, TEXT("[Update Cell State] Phase 1: %d SubCells destroyed, %d Cells fully destroyed, %d Cells affected"),
			DestructionResult.DeadSubCellCount,
			DestructionResult.NewlyDestroyedCells.Num(),
			DestructionResult.AffectedCells.Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[Update Cell State] Phase 1: %d cells directly destroyed"),
			DestructionResult.NewlyDestroyedCells.Num());
	}

	//=====================================================================
// Phase 1.5: SuperCell 상태 업데이트 (bEnableSuperCell이 true일 때만)
		//=====================================================================
	if (bEnableSupercell && SupercellCache.IsValid())
	{
		// 영향받은 Cell들이 속한 SuperCell을 Broken으로 마킹
		SupercellCache.UpdateSupercellStates(DestructionResult.AffectedCells);

		// 파괴된 Cell들이 속한 SuperCell도 Broken으로 마킹
		for (int32 DestroyedCellId : DestructionResult.NewlyDestroyedCells)
		{
			SupercellCache.OnCellDestroyed(DestroyedCellId);
		}

		// SubCell 모드에서는 SubCell 파괴도 SuperCell 상태에 반영
		// 단, Standalone에서만 (네트워크에서는 SubCell 정보 미동기화 + SubCell BFS 미사용)
		if (bEnableSubcell)
		{
			const ENetMode CurrentNetMode = GetWorld()->GetNetMode();
			if (CurrentNetMode == NM_Standalone)
			{
				for (const auto& SubCellPair : DestructionResult.NewlyDeadSubCells)
				{
					for (int32 SubCellId : SubCellPair.Value.Values)
					{
						SupercellCache.OnSubCellDestroyed(SubCellPair.Key, SubCellId);
					}
				}
			}
		}
	}

	return DestructionResult;
}

void URealtimeDestructibleMeshComponent::DisconnectedCellStateLogic(const TArray< FDestructionResult>& AllResults)
{
	//파괴된게 없으면 패스
	bool bHasAnyDestruction = false;
	for (const FDestructionResult& Result : AllResults)
	{
		if (Result.HasAnyDestruction())
		{
			bHasAnyDestruction = true;
			break;
		}
	} 
	if (!bHasAnyDestruction)
	{
		return;
	}


	//=====================================================================
	// Phase 2: BFS로 앵커에서 분리된 셀 찾기
	// 통합 API 사용: bEnableSupercell, bEnableSubcell 플래그에 따라 자동 선택
	// Multiplayer: SubCell 상태는 Client에 동기화되지 않으므로 Standalone에서만 사용
	//======================================================== 

	// Standalon용 BFS 성능 LOG
	//int32 BFSCallCount = 0;
	//BFSCallCount++;
	//double BFSStartTime = FPlatformTime::Seconds();
	//
	//UE_LOG(LogTemp, Warning, TEXT("[BFS #%d] FindDisconnectedCells START (TotalCells: %d)"),
	//	BFSCallCount, GridCellCache.GetTotalCellCount()); 

	// ===== 여기에 로그 추가 =====
	UE_LOG(LogTemp, Warning, TEXT("    [Batch BFS] FindDisconnectedCells called"));

	const ENetMode NetMode = GetWorld()->GetNetMode();
	TSet<int32> DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
		GridCellCache,
		SupercellCache,
		CellState,
		bEnableSupercell && SupercellCache.IsValid(),
		bEnableSubcell && (NetMode == NM_Standalone)); // subcell 동기화 안 하므로 subcell은 standalone에서만 허용

	// Standalon용 BFS 성능 LOG
	//double BFSEndTime = FPlatformTime::Seconds();
	//UE_LOG(LogTemp, Warning, TEXT("[BFS #%d] FindDisconnectedCells END - took %.3f ms, found %d disconnected"),
	//	BFSCallCount, (BFSEndTime - BFSStartTime) * 1000.0, DisconnectedCells.Num());



	UE_LOG(LogTemp, Log, TEXT("[Cell] Phase 2: %d Cells disconnected"), DisconnectedCells.Num());



	if (DisconnectedCells.Num() > 0)
	{
		//=====================================================================
		// Phase 3: 분리된 셀 그룹화
		//=====================================================================
		TArray<TArray<int32>> NewDetachedGroups = FCellDestructionSystem::GroupDetachedCells(
			GridCellCache,
			DisconnectedCells,
			CellState.DestroyedCells);

		for (const TArray<int32>& Group : NewDetachedGroups)
		{
			CellState.AddDetachedGroup(Group);
		}

		//=====================================================================
		// Phase 4: 서버 → 클라이언트 신호 전송
		//=====================================================================
		// 클라이언트에게 Detach 발생 신호만 전송 (클라이언트가 자체 BFS 실행)
		UE_LOG(LogTemp, Warning, TEXT("[GridCell] MulticastDetachSignal SENDING - %d groups detached"),
			NewDetachedGroups.Num());
		MulticastDetachSignal();

		// 서버: 분리된 셀의 삼각형 삭제
		for (const TArray<int32>& Group : NewDetachedGroups)
		{
			RemoveTrianglesForDetachedCells(Group);
		}

		CellState.MoveAllDetachedToDestroyed();

		// 서버 Cell Collision: 분리된 셀들의 청크도 dirty 마킹
		if (bServerCellCollisionInitialized)
		{
			TSet<int32> DetachedDirtyChunks;
			for (int32 CellId : DisconnectedCells)
			{
				int32 ChunkIdx = GetCollisionChunkIndexForCell(CellId);
				if (ChunkIdx != INDEX_NONE)
				{
					DetachedDirtyChunks.Add(ChunkIdx);
				}
				// 이웃 셀 청크도 dirty (새 표면 될 수 있음)
				const FIntArray& Neighbors = GridCellCache.GetCellNeighbors(CellId);
				for (int32 NeighborId : Neighbors.Values)
				{
					int32 NeighborChunkIdx = GetCollisionChunkIndexForCell(NeighborId);
					if (NeighborChunkIdx != INDEX_NONE)
					{
						DetachedDirtyChunks.Add(NeighborChunkIdx);
					}
				}
			}
			for (int32 ChunkIdx : DetachedDirtyChunks)
			{
				MarkCollisionChunkDirty(ChunkIdx);
			}
			UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Marked %d chunks dirty from %d detached cells"),
				DetachedDirtyChunks.Num(), DisconnectedCells.Num());
		}

		UE_LOG(LogTemp, Log, TEXT("UpdateCellStateFromDestruction [Server]: %d cells disconnected (%d groups)"),
			DisconnectedCells.Num(), NewDetachedGroups.Num());
	}
	else
	{
		// 분리된 셀 없어도 파괴된 셀 있으면 파편 정리 (RemoveTrianglesForDetachedCells 통하지 않으므로)
		CleanupSmallFragments();
	}

	
		for (const FDestructionResult& Result : AllResults)
		{
			ProcessDecalRemoval(Result);
		}

		if (DisconnectedCells.Num() > 0)
		{
			FDestructionResult DetachResult;
			DetachResult.NewlyDestroyedCells = DisconnectedCells.Array();

		ProcessDecalRemoval(DetachResult);
	}

	UE_LOG(LogTemp, Log, TEXT("UpdateCellStateFromDestruction Complete: Destroyed=%d, DetachedGroups=%d"),
		CellState.DestroyedCells.Num(), CellState.DetachedGroups.Num());
}

int32 URealtimeDestructibleMeshComponent::GridCellIdToChunkId(int32 GridCellId) const
{
	if (!GridCellCache.IsValidCellId(GridCellId))
	{
		UE_LOG(LogTemp, Warning, TEXT("GridCellIdToChunkId: Invalid CellId=%d"), GridCellId);
		return INDEX_NONE;
	}
	if (GridToChunkMap.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GridCellIdToChunkId: GridToChunkMap is empty!"));
		return INDEX_NONE;
	}

	// GridCellCache에서 로컬 중심점 획득
	const FVector LocalCenter = GridCellCache.IdToLocalCenter(GridCellId);

	// SliceCount 기반 그리드 인덱스 계산
	int32 GridX = FMath::FloorToInt((LocalCenter.X - CachedMeshBounds.Min.X) / CachedChunkSize.X);
	int32 GridY = FMath::FloorToInt((LocalCenter.Y - CachedMeshBounds.Min.Y) / CachedChunkSize.Y);
	int32 GridZ = FMath::FloorToInt((LocalCenter.Z - CachedMeshBounds.Min.Z) / CachedChunkSize.Z);

	GridX = FMath::Clamp(GridX, 0, SliceCount.X - 1);
	GridY = FMath::Clamp(GridY, 0, SliceCount.Y - 1);
	GridZ = FMath::Clamp(GridZ, 0, SliceCount.Z - 1);

	const int32 GridIndex = GridX + GridY * SliceCount.X + GridZ * SliceCount.X * SliceCount.Y;
	return GridToChunkMap.IsValidIndex(GridIndex) ? GridToChunkMap[GridIndex] : INDEX_NONE;
}

//=============================================================================
// 서버 Cell Box Collision (Chunked BodySetup + Surface Voxel)
//=============================================================================

void URealtimeDestructibleMeshComponent::BuildServerCellCollision()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(BuildServerCellCollision);

	// Cell Box Collision 비활성화 시 원본 메시 콜리전 사용
	if (!bEnableServerCellCollision)
	{
		UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Disabled, using original mesh collision"));
		return;
	}

	// 서버(데디케이티드/리슨)에서만 실행
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (!GridCellCache.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] GridCellCache is not valid, skipping"));
		return;
	}

	// 동적 청크 분할 수 계산
	const int32 TotalCells = GridCellCache.GetValidCellCount();
	if (TotalCells == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] No valid cells, skipping"));
		return;
	}

	// 목표 청크 수 계산
	const int32 TargetChunkCount = FMath::Max(1, TotalCells / FMath::Max(1, TargetCellsPerCollisionChunk));

	// 3차원이므로 세제곱근으로 각 축 분할 수 계산
	CollisionChunkDivisions = FMath::Max(1, FMath::RoundToInt(FMath::Pow((float)TargetChunkCount, 1.0f / 3.0f)));

	// 최소 1, 최대 10으로 제한
	CollisionChunkDivisions = FMath::Clamp(CollisionChunkDivisions, 1, 10);

	const int32 TotalChunks = CollisionChunkDivisions * CollisionChunkDivisions * CollisionChunkDivisions;

	CollisionChunks.SetNum(TotalChunks);

	UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Dynamic chunking: %d cells / %d target = %d divisions (%d chunks, ~%d cells/chunk)"),
		TotalCells, TargetCellsPerCollisionChunk, CollisionChunkDivisions, TotalChunks,
		TotalChunks > 0 ? TotalCells / TotalChunks : 0);

	// 메시 바운드 기반으로 청크 크기 계산
	const FBox MeshBounds = CachedMeshBounds;
	const FVector BoundsSize = MeshBounds.GetSize();

	// Division by Zero 보호: 바운드가 너무 작으면 단일 청크로 처리
	if (BoundsSize.X < KINDA_SMALL_NUMBER || BoundsSize.Y < KINDA_SMALL_NUMBER || BoundsSize.Z < KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] Degenerate bounds detected: %s, using single chunk"), *BoundsSize.ToString());
		CollisionChunkDivisions = 1;
	}

	const FVector ChunkSize = BoundsSize / FMath::Max(1.0f, (float)CollisionChunkDivisions);

	// 각 유효 셀을 청크에 할당
	CellToCollisionChunkMap.Empty();

	for (int32 SparseIdx = 0; SparseIdx < GridCellCache.GetValidCellCount(); ++SparseIdx)
	{
		const int32 CellId = GridCellCache.SparseIndexToCellId[SparseIdx];
		const FVector LocalCenter = GridCellCache.IdToLocalCenter(CellId);

		// 청크 인덱스 계산 (Division by Zero 보호 포함)
		int32 ChunkX = (ChunkSize.X > KINDA_SMALL_NUMBER) ? FMath::FloorToInt((LocalCenter.X - MeshBounds.Min.X) / ChunkSize.X) : 0;
		int32 ChunkY = (ChunkSize.Y > KINDA_SMALL_NUMBER) ? FMath::FloorToInt((LocalCenter.Y - MeshBounds.Min.Y) / ChunkSize.Y) : 0;
		int32 ChunkZ = (ChunkSize.Z > KINDA_SMALL_NUMBER) ? FMath::FloorToInt((LocalCenter.Z - MeshBounds.Min.Z) / ChunkSize.Z) : 0;

		ChunkX = FMath::Clamp(ChunkX, 0, CollisionChunkDivisions - 1);
		ChunkY = FMath::Clamp(ChunkY, 0, CollisionChunkDivisions - 1);
		ChunkZ = FMath::Clamp(ChunkZ, 0, CollisionChunkDivisions - 1);

		const int32 ChunkIndex = ChunkX + ChunkY * CollisionChunkDivisions + ChunkZ * CollisionChunkDivisions * CollisionChunkDivisions;

		CollisionChunks[ChunkIndex].CellIds.Add(CellId);
		CellToCollisionChunkMap.Add(CellId, ChunkIndex);
	}

	// 서버에서 원본 메시: Pawn 채널만 Ignore (총알은 충돌, 캐릭터는 Cell Box로 처리)
	SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);

	// ChunkMeshComponents도 Pawn Ignore 설정
	for (UDynamicMeshComponent* ChunkMesh : ChunkMeshComponents)
	{
		if (ChunkMesh)
		{
			ChunkMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Mesh: Pawn Ignore (Main + %d ChunkMeshes), Cell Box handles Pawn"), ChunkMeshComponents.Num());

	// 각 청크의 콜리전 컴포넌트 및 BodySetup 생성
	for (int32 i = 0; i < TotalChunks; ++i)
	{
		BuildCollisionChunkBodySetup(i);
	}

	bServerCellCollisionInitialized = true;

	int32 TotalSurfaceCells = 0;
	int32 NonEmptyChunks = 0;
	for (int32 i = 0; i < CollisionChunks.Num(); ++i)
	{
		const FCollisionChunkData& Chunk = CollisionChunks[i];
		TotalSurfaceCells += Chunk.SurfaceCellIds.Num();
		if (Chunk.SurfaceCellIds.Num() > 0)
		{
			++NonEmptyChunks;
			// 처음 10개 비어있지 않은 청크 상세 로그
			if (NonEmptyChunks <= 10)
			{
				UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Chunk %d: %d cells, %d surface cells"),
					i, Chunk.CellIds.Num(), Chunk.SurfaceCellIds.Num());
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Initialized: %d chunks (%d non-empty), %d total cells, %d surface cells"),
		TotalChunks, NonEmptyChunks, GridCellCache.GetValidCellCount(), TotalSurfaceCells);
}

void URealtimeDestructibleMeshComponent::BuildCollisionChunkBodySetup(int32 ChunkIndex)
{
	if (!CollisionChunks.IsValidIndex(ChunkIndex))
	{
		return;
	}

	// GridCellCache 유효성 검사
	if (!GridCellCache.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] GridCellCache invalid, skipping chunk %d"), ChunkIndex);
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(BuildCollisionChunkBodySetup);

	FCollisionChunkData& Chunk = CollisionChunks[ChunkIndex];

	// 1. 콜리전 컴포넌트 생성/찾기
	UStaticMeshComponent* ChunkComp = Cast<UStaticMeshComponent>(Chunk.ChunkComponent);
	if (!ChunkComp)
	{
		// Owner 유효성 검사
		AActor* Owner = GetOwner();
		if (!Owner)
		{
			UE_LOG(LogTemp, Error, TEXT("[ServerCellCollision] Chunk %d: Owner is null, cannot create collision component"), ChunkIndex);
			return;
		}

		// 새 컴포넌트 생성
		ChunkComp = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
		if (!ChunkComp)
		{
			UE_LOG(LogTemp, Error, TEXT("[ServerCellCollision] Chunk %d: Failed to create UStaticMeshComponent"), ChunkIndex);
			return;
		}

		ChunkComp->SetupAttachment(this);
		ChunkComp->SetRelativeTransform(FTransform::Identity);
		ChunkComp->SetStaticMesh(nullptr);  // 메시 없이 콜리전만 사용
		ChunkComp->SetHiddenInGame(true);   // 렌더링 안함
		ChunkComp->SetCastShadow(false);
		ChunkComp->bAlwaysCreatePhysicsState = true;  // 메시 없이도 물리 상태 생성
		ChunkComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ChunkComp->SetCollisionObjectType(ECC_WorldStatic);
		ChunkComp->SetCollisionResponseToAllChannels(ECR_Ignore);
		ChunkComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);  // Pawn만 Block
		ChunkComp->SetCanEverAffectNavigation(false);  // NavMesh 영향 없음

		// BodyInstance 사전 설정
		FBodyInstance* BI = ChunkComp->GetBodyInstance();
		if (BI)
		{
			BI->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			BI->SetObjectType(ECC_WorldStatic);
			BI->SetResponseToAllChannels(ECR_Ignore);
			BI->SetResponseToChannel(ECC_Pawn, ECR_Block);
			BI->bSimulatePhysics = false;
			BI->bEnableGravity = false;
		}

		ChunkComp->RegisterComponent();

		Chunk.ChunkComponent = ChunkComp;
	}

	// 2. BodySetup 생성/갱신
	if (!Chunk.BodySetup)
	{
		Chunk.BodySetup = NewObject<UBodySetup>(ChunkComp, NAME_None, RF_Transient);
		if (!Chunk.BodySetup)
		{
			UE_LOG(LogTemp, Error, TEXT("[ServerCellCollision] Chunk %d: Failed to create UBodySetup"), ChunkIndex);
			return;
		}
		Chunk.BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		Chunk.BodySetup->bGenerateMirroredCollision = false;
		Chunk.BodySetup->bDoubleSidedGeometry = false;
	}

	FKAggregateGeom& ChunkAggGeom = Chunk.BodySetup->AggGeom;
	const int32 OldBoxCount = ChunkAggGeom.BoxElems.Num();
	ChunkAggGeom.BoxElems.Reset();
	Chunk.SurfaceCellIds.Reset();

	int32 SkippedDestroyedCount = 0;

	// 3. 표면 셀의 박스 추가
	for (int32 CellId : Chunk.CellIds)
	{
		// 파괴된 셀 스킵
		if (CellState.DestroyedCells.Contains(CellId))
		{
			++SkippedDestroyedCount;
			continue;
		}

		// 표면 셀만 추가 (Surface Voxel)
		if (!IsCellExposed(CellId))
		{
			continue;
		}

		Chunk.SurfaceCellIds.Add(CellId);

		const FVector LocalCenter = GridCellCache.IdToLocalCenter(CellId);

		FKBoxElem BoxElem;
		BoxElem.Center = LocalCenter;
		BoxElem.X = GridCellSize.X;
		BoxElem.Y = GridCellSize.Y;
		BoxElem.Z = GridCellSize.Z;
		BoxElem.Rotation = FRotator::ZeroRotator;

		ChunkAggGeom.BoxElems.Add(BoxElem);
	}

	// 4. 빈 청크 처리: 모든 셀이 파괴된 경우 콜리전 비활성화
	if (ChunkAggGeom.BoxElems.Num() == 0)
	{
		ChunkComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Chunk.bDirty = false;
		if (OldBoxCount > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Chunk %d: All cells destroyed, collision disabled"), ChunkIndex);
		}
		return;
	}

	// 콜리전이 비활성화되어 있었다면 다시 활성화
	if (ChunkComp->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
	{
		ChunkComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}

	// 5. 물리 메시 쿠킹
	Chunk.BodySetup->InvalidatePhysicsData();
	Chunk.BodySetup->CreatePhysicsMeshes();

	// 물리 메시 생성 확인
	UE_LOG(LogTemp, Log, TEXT("[CellBoxDebug] Chunk %d: CreatePhysicsMeshes called, bCreatedPhysicsMeshes=%d"),
		ChunkIndex, Chunk.BodySetup->bCreatedPhysicsMeshes ? 1 : 0);

	// 6. 컴포넌트의 BodySetup 갱신 및 물리 바디 직접 생성
	FBodyInstance* ChunkBodyInstance = ChunkComp->GetBodyInstance();
	if (ChunkBodyInstance)
	{
		// 기존 물리 바디 정리
		if (ChunkBodyInstance->IsValidBodyInstance())
		{
			ChunkBodyInstance->TermBody();
		}

		// BodyInstance 콜리전 설정 (InitBody 전에 설정해야 함)
		ChunkBodyInstance->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ChunkBodyInstance->SetObjectType(ECC_WorldStatic);
		ChunkBodyInstance->SetResponseToAllChannels(ECR_Ignore);
		ChunkBodyInstance->SetResponseToChannel(ECC_Pawn, ECR_Block);

		// 물리 시뮬레이션 비활성화 (정적 콜리전)
		ChunkBodyInstance->bSimulatePhysics = false;
		ChunkBodyInstance->bEnableGravity = false;

		// 물리 바디 직접 초기화
		UWorld* World = GetWorld();
		if (World && World->GetPhysicsScene())
		{
			// BodySetup 연결 (InitBody 전에 필요)
			ChunkBodyInstance->BodySetup = Chunk.BodySetup;

			// BodySetup 유효성 확인
			UE_LOG(LogTemp, Warning, TEXT("[CellBoxDebug] Chunk %d: BodySetup=%p, BoxElems=%d, PhysicsScene=%p"),
				ChunkIndex, Chunk.BodySetup.Get(), ChunkAggGeom.BoxElems.Num(), World->GetPhysicsScene());

			ChunkBodyInstance->InitBody(Chunk.BodySetup, ChunkComp->GetComponentTransform(), ChunkComp, World->GetPhysicsScene());

			// 콜리전 채널 물리에 적용
			ChunkBodyInstance->UpdatePhysicsFilterData();

			// 정적 바디로 설정 (시뮬레이션 없음)
			if (ChunkBodyInstance->IsValidBodyInstance())
			{
				ChunkBodyInstance->SetInstanceSimulatePhysics(false);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[CellBoxDebug] Chunk %d: World or PhysicsScene is null"), ChunkIndex);
		}

		// 디버그: 물리 바디 상태 확인
		bool bHasPhysicsBody = ChunkBodyInstance->IsValidBodyInstance();

		// InitBody가 실패했으면 RecreatePhysicsState 시도
		if (!bHasPhysicsBody)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CellBoxDebug] Chunk %d: InitBody failed, trying RecreatePhysicsState..."), ChunkIndex);
			ChunkComp->RecreatePhysicsState();
			bHasPhysicsBody = ChunkBodyInstance->IsValidBodyInstance();
		}

		UE_LOG(LogTemp, Warning, TEXT("[CellBoxDebug] Chunk %d: Boxes=%d, HasPhysicsBody=%d, CollisionEnabled=%d, BodySetupBoxes=%d"),
			ChunkIndex, ChunkAggGeom.BoxElems.Num(), bHasPhysicsBody ? 1 : 0,
			(int32)ChunkComp->GetCollisionEnabled(),
			Chunk.BodySetup ? Chunk.BodySetup->AggGeom.BoxElems.Num() : -1);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] Chunk %d: GetBodyInstance returned null"), ChunkIndex);
	}

	Chunk.bDirty = false;

	// 변경이 있을 때만 로그 출력 (초기화 시에는 OldBoxCount가 0)
	if (OldBoxCount > 0 || SkippedDestroyedCount > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Chunk %d rebuilt: %d -> %d boxes (skipped %d destroyed cells)"),
			ChunkIndex, OldBoxCount, ChunkAggGeom.BoxElems.Num(), SkippedDestroyedCount);
	}
}

bool URealtimeDestructibleMeshComponent::IsCellExposed(int32 CellId) const
{
	const FIntArray& Neighbors = GridCellCache.GetCellNeighbors(CellId);

	// 이웃이 6개 미만이면 경계 = 표면
	if (Neighbors.Values.Num() < 6)
	{
		return true;
	}

	// 이웃 중 하나라도 파괴되었으면 표면
	for (int32 NeighborId : Neighbors.Values)
	{
		if (CellState.DestroyedCells.Contains(NeighborId))
		{
			return true;
		}
	}

	return false; // 내부 셀
}

int32 URealtimeDestructibleMeshComponent::GetCollisionChunkIndexForCell(int32 CellId) const
{
	if (const int32* ChunkIndex = CellToCollisionChunkMap.Find(CellId))
	{
		return *ChunkIndex;
	}
	return INDEX_NONE;
}

void URealtimeDestructibleMeshComponent::MarkCollisionChunkDirty(int32 ChunkIndex)
{
	if (CollisionChunks.IsValidIndex(ChunkIndex))
	{
		CollisionChunks[ChunkIndex].bDirty = true;
	}
}

void URealtimeDestructibleMeshComponent::UpdateDirtyCollisionChunks()
{
	if (!bServerCellCollisionInitialized)
	{
		return;
	}

	// 프레임 버짓: 한 프레임에 처리할 최대 청크 수 (성능 스파이크 방지)
	constexpr int32 MaxChunksPerFrame = 5;

	// dirty 청크만 부분 재빌드 (다중 컴포넌트 방식의 핵심!)
	int32 UpdatedCount = 0;
	int32 RemainingDirty = 0;

	for (int32 i = 0; i < CollisionChunks.Num(); ++i)
	{
		if (CollisionChunks[i].bDirty)
		{
			if (UpdatedCount < MaxChunksPerFrame)
			{
				BuildCollisionChunkBodySetup(i);
				++UpdatedCount;
			}
			else
			{
				++RemainingDirty;  // 다음 프레임으로 연기
			}
		}
	}

	if (UpdatedCount > 0)
	{
		if (RemainingDirty > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Updated %d dirty chunks (%d deferred to next frame)"),
				UpdatedCount, RemainingDirty);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("[ServerCellCollision] Updated %d dirty chunks"), UpdatedCount);
		}
	}
}

void URealtimeDestructibleMeshComponent::RemoveTrianglesForDetachedCells(
	const TArray<int32>& DetachedCellIds)
{
	using namespace UE::Geometry;

	if (DetachedCellIds.Num() == 0 || ChunkMeshComponents.Num() == 0)
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("=== RemoveTrianglesForDetachedCells START ==="));
	UE_LOG(LogTemp, Warning, TEXT("DetachedCellIds.Num()=%d, ChunkMeshComponents.Num()=%d"),
		DetachedCellIds.Num(), ChunkMeshComponents.Num());

	const FVector CellSizeVec = GridCellCache.CellSize;

	// 파편 정리용 초기화
	LastOccupiedCells.Empty();
	LastCellSizeVec = CellSizeVec;

	// 1. 모든 분리된 셀들의 3D 점유 맵 생성
	TSet<FIntVector> BaseCells;
	for (int32 CellId : DetachedCellIds)
	{
		const FVector LocalMin = GridCellCache.IdToLocalMin(CellId);
		FIntVector GridPos(
			FMath::FloorToInt(LocalMin.X / CellSizeVec.X),
			FMath::FloorToInt(LocalMin.Y / CellSizeVec.Y),
			FMath::FloorToInt(LocalMin.Z / CellSizeVec.Z)
		);
		BaseCells.Add(GridPos);
	}

	// 인접 셀 26방향 확장
	TSet<FIntVector> OccupiedCells = BaseCells;
	for (const FIntVector& Pos : BaseCells)
	{
		for (int32 dx = -1; dx <= 1; ++dx)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dz = -1; dz <= 1; ++dz)
				{
					OccupiedCells.Add(FIntVector(Pos.X + dx, Pos.Y + dy, Pos.Z + dz));
				}
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("BaseCells=%d, ExpandedCells=%d"), BaseCells.Num(), OccupiedCells.Num());

	// 파편 정리용으로 저장
	LastOccupiedCells = OccupiedCells;

	// 2. Greedy Meshing으로 ToolMesh 생성
	const double BoxExpand = 1.0;

	FDynamicMesh3 ToolMesh;
	ToolMesh.EnableTriangleGroups();

	// 외곽 경계 계산
	FIntVector GridMin(TNumericLimits<int32>::Max());
	FIntVector GridMax(TNumericLimits<int32>::Lowest());
	for (const FIntVector& Pos : OccupiedCells)
	{
		GridMin.X = FMath::Min(GridMin.X, Pos.X);
		GridMin.Y = FMath::Min(GridMin.Y, Pos.Y);
		GridMin.Z = FMath::Min(GridMin.Z, Pos.Z);
		GridMax.X = FMath::Max(GridMax.X, Pos.X + 1);
		GridMax.Y = FMath::Max(GridMax.Y, Pos.Y + 1);
		GridMax.Z = FMath::Max(GridMax.Z, Pos.Z + 1);
	}

	// 공유 정점 맵
	TMap<FIntVector, int32> CornerToVertexId;

	auto GetOrCreateVertex = [&](const FIntVector& Corner) -> int32
	{
		if (int32* Existing = CornerToVertexId.Find(Corner))
		{
			return *Existing;
		}

		double ExpX = 0, ExpY = 0, ExpZ = 0;
		if (Corner.X == GridMin.X) ExpX = -BoxExpand;
		else if (Corner.X == GridMax.X) ExpX = BoxExpand;
		if (Corner.Y == GridMin.Y) ExpY = -BoxExpand;
		else if (Corner.Y == GridMax.Y) ExpY = BoxExpand;
		if (Corner.Z == GridMin.Z) ExpZ = -BoxExpand;
		else if (Corner.Z == GridMax.Z) ExpZ = BoxExpand;

		FVector3d VertexPos(
			Corner.X * CellSizeVec.X + ExpX,
			Corner.Y * CellSizeVec.Y + ExpY,
			Corner.Z * CellSizeVec.Z + ExpZ
		);

		int32 NewId = ToolMesh.AppendVertex(VertexPos);
		CornerToVertexId.Add(Corner, NewId);
		return NewId;
	};

	// 면 방향별 Greedy Meshing
	for (int32 FaceDir = 0; FaceDir < 6; ++FaceDir)
	{
		TSet<FIntVector> ExposedFaces;
		FIntVector Normal;

		switch (FaceDir)
		{
		case 0: Normal = FIntVector(0, 0, -1); break;
		case 1: Normal = FIntVector(0, 0, 1); break;
		case 2: Normal = FIntVector(-1, 0, 0); break;
		case 3: Normal = FIntVector(1, 0, 0); break;
		case 4: Normal = FIntVector(0, -1, 0); break;
		case 5: Normal = FIntVector(0, 1, 0); break;
		}

		for (const FIntVector& Pos : OccupiedCells)
		{
			if (!OccupiedCells.Contains(Pos + Normal))
			{
				ExposedFaces.Add(Pos);
			}
		}

		if (ExposedFaces.Num() == 0) continue;

		TSet<FIntVector> Processed;

		for (const FIntVector& Start : ExposedFaces)
		{
			if (Processed.Contains(Start)) continue;

			int32 Width = 1, Height = 1;
			int32 Axis1, Axis2;
			if (FaceDir <= 1) { Axis1 = 0; Axis2 = 1; }
			else if (FaceDir <= 3) { Axis1 = 1; Axis2 = 2; }
			else { Axis1 = 0; Axis2 = 2; }

			auto GetCoord = [](const FIntVector& V, int32 Axis) -> int32 {
				return Axis == 0 ? V.X : (Axis == 1 ? V.Y : V.Z);
			};
			auto SetCoord = [](FIntVector& V, int32 Axis, int32 Val) {
				if (Axis == 0) V.X = Val;
				else if (Axis == 1) V.Y = Val;
				else V.Z = Val;
			};

			while (true)
			{
				FIntVector Check = Start;
				SetCoord(Check, Axis1, GetCoord(Start, Axis1) + Width);
				if (ExposedFaces.Contains(Check) && !Processed.Contains(Check))
					Width++;
				else
					break;
			}

			while (true)
			{
				bool CanExpand = true;
				for (int32 w = 0; w < Width; ++w)
				{
					FIntVector Check = Start;
					SetCoord(Check, Axis1, GetCoord(Start, Axis1) + w);
					SetCoord(Check, Axis2, GetCoord(Start, Axis2) + Height);
					if (!ExposedFaces.Contains(Check) || Processed.Contains(Check))
					{
						CanExpand = false;
						break;
					}
				}
				if (CanExpand) Height++;
				else break;
			}

			for (int32 h = 0; h < Height; ++h)
			{
				for (int32 w = 0; w < Width; ++w)
				{
					FIntVector Cell = Start;
					SetCoord(Cell, Axis1, GetCoord(Start, Axis1) + w);
					SetCoord(Cell, Axis2, GetCoord(Start, Axis2) + h);
					Processed.Add(Cell);
				}
			}

			FIntVector C0 = Start, C1 = Start, C2 = Start, C3 = Start;

			switch (FaceDir)
			{
			case 0:
				C1.X += Width; C2.X += Width; C2.Y += Height; C3.Y += Height;
				break;
			case 1:
				C0.Z += 1; C1.Z += 1; C2.Z += 1; C3.Z += 1;
				C1.X += Width; C2.X += Width; C2.Y += Height; C3.Y += Height;
				break;
			case 2:
				C1.Y += Width; C2.Y += Width; C2.Z += Height; C3.Z += Height;
				break;
			case 3:
				C0.X += 1; C1.X += 1; C2.X += 1; C3.X += 1;
				C1.Y += Width; C2.Y += Width; C2.Z += Height; C3.Z += Height;
				break;
			case 4:
				C1.X += Width; C2.X += Width; C2.Z += Height; C3.Z += Height;
				break;
			case 5:
				C0.Y += 1; C1.Y += 1; C2.Y += 1; C3.Y += 1;
				C1.X += Width; C2.X += Width; C2.Z += Height; C3.Z += Height;
				break;
			}

			int32 I0 = GetOrCreateVertex(C0);
			int32 I1 = GetOrCreateVertex(C1);
			int32 I2 = GetOrCreateVertex(C2);
			int32 I3 = GetOrCreateVertex(C3);

			switch (FaceDir)
			{
			case 0:
				ToolMesh.AppendTriangle(I0, I2, I1);
				ToolMesh.AppendTriangle(I0, I3, I2);
				break;
			case 1:
				ToolMesh.AppendTriangle(I0, I1, I2);
				ToolMesh.AppendTriangle(I0, I2, I3);
				break;
			case 2:
				ToolMesh.AppendTriangle(I0, I3, I2);
				ToolMesh.AppendTriangle(I0, I2, I1);
				break;
			case 3:
				ToolMesh.AppendTriangle(I0, I1, I2);
				ToolMesh.AppendTriangle(I0, I2, I3);
				break;
			case 4:
				ToolMesh.AppendTriangle(I0, I1, I2);
				ToolMesh.AppendTriangle(I0, I2, I3);
				break;
			case 5:
				ToolMesh.AppendTriangle(I0, I3, I2);
				ToolMesh.AppendTriangle(I0, I2, I1);
				break;
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("ToolMesh: Tris=%d, Verts=%d"), ToolMesh.TriangleCount(), ToolMesh.VertexCount());

	// 노말 방향 반전 (Subtract용)
	ToolMesh.ReverseOrientation();

	// Laplacian Smoothing 적용 (계단 현상 완화)
	ApplyHCLaplacianSmoothing(ToolMesh);

	if (ToolMesh.TriangleCount() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("ToolMesh has 0 triangles!"));
		return;
	}

	// 디버그: 원본 메시 옆에 ToolMesh 스폰 (로컬→월드 변환 적용)
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		// 원본 메시 Transform 가져오기
		FTransform MeshTransform = GetComponentTransform();

		// ToolMesh 바운드 중심을 월드 좌표로 변환
		FAxisAlignedBox3d ToolBounds = ToolMesh.GetBounds();
		FVector ToolCenterLocal = FVector(ToolBounds.Center());
		FVector ToolCenterWorld = MeshTransform.TransformPosition(ToolCenterLocal);

		// 오른쪽으로 오프셋 (바운드 크기 기준)
		FVector BoundsSize = FVector(ToolBounds.Extents()) * 2.0f * MeshTransform.GetScale3D();
		FVector RightOffset = MeshTransform.GetRotation().GetRightVector() * (BoundsSize.Y + 100.0f);
		FVector DebugLocation = ToolCenterWorld + RightOffset;

		AActor* DebugActor = World->SpawnActor<AActor>(AActor::StaticClass(), DebugLocation, MeshTransform.Rotator(), SpawnParams);
		if (DebugActor)
		{
			DebugActor->SetActorScale3D(MeshTransform.GetScale3D());

			UDynamicMeshComponent* DebugComp = NewObject<UDynamicMeshComponent>(DebugActor);
			DebugComp->RegisterComponent();
			DebugActor->AddInstanceComponent(DebugComp);
			DebugActor->SetRootComponent(DebugComp);

			// ToolMesh 복사 후 중심을 원점으로 이동
			FDynamicMesh3 CenteredToolMesh = ToolMesh;
			MeshTransforms::Translate(CenteredToolMesh, -ToolBounds.Center());
			*DebugComp->GetMesh() = CenteredToolMesh;
			DebugComp->NotifyMeshUpdated();

			// 10초 후 삭제
			DebugActor->SetLifeSpan(10.0f);

			UE_LOG(LogTemp, Warning, TEXT("DEBUG: ToolMesh Center Local=%s, World=%s, BoundsSize=%s"),
				*ToolCenterLocal.ToString(), *ToolCenterWorld.ToString(), *BoundsSize.ToString());
		}
	}

	// 3. 모든 ChunkMeshComponents에 Boolean Subtract 적용
	FGeometryScriptMeshBooleanOptions BoolOptions;
	BoolOptions.bFillHoles = true;
	BoolOptions.bSimplifyOutput = false;

	int32 TotalChunksProcessed = 0;
	FAxisAlignedBox3d ToolBounds = ToolMesh.GetBounds();

	UE_LOG(LogTemp, Warning, TEXT("ToolMesh Bounds: Min=%s, Max=%s"),
		*FVector(ToolBounds.Min).ToString(), *FVector(ToolBounds.Max).ToString());

	for (int32 ChunkIdx = 0; ChunkIdx < ChunkMeshComponents.Num(); ++ChunkIdx)
	{
		UDynamicMeshComponent* ChunkMesh = ChunkMeshComponents[ChunkIdx];
		if (!ChunkMesh || !ChunkMesh->GetMesh()) continue;

		FDynamicMesh3* TargetMesh = ChunkMesh->GetMesh();
		if (TargetMesh->TriangleCount() == 0) continue;

		// AABB 겹침 체크
		FAxisAlignedBox3d ChunkBounds = TargetMesh->GetBounds();
		bool bBoundsOverlap = ChunkBounds.Intersects(ToolBounds);

		UE_LOG(LogTemp, Warning, TEXT("Chunk[%d]: Bounds Min=%s Max=%s, Overlap=%s"),
			ChunkIdx,
			*FVector(ChunkBounds.Min).ToString(),
			*FVector(ChunkBounds.Max).ToString(),
			bBoundsOverlap ? TEXT("YES") : TEXT("NO"));

		if (!bBoundsOverlap)
		{
			UE_LOG(LogTemp, Warning, TEXT("Chunk[%d]: SKIP (no bounds overlap)"), ChunkIdx);
			continue;
		}

		FDynamicMesh3 ResultMesh;
		bool bSuccess = FRealtimeBooleanProcessor::ApplyMeshBooleanAsync(
			TargetMesh,
			&ToolMesh,
			&ResultMesh,
			EGeometryScriptBooleanOperation::Subtract,
			BoolOptions
		);

		if (!bSuccess)
		{
			UE_LOG(LogTemp, Error, TEXT("Chunk[%d]: Boolean FAILED"), ChunkIdx);
		}
		else if (ResultMesh.TriangleCount() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("Chunk[%d]: Result 0 tris (fully consumed)"), ChunkIdx);
			// 완전히 제거된 경우도 적용
			*TargetMesh = MoveTemp(ResultMesh);
			ChunkMesh->NotifyMeshUpdated();
			TotalChunksProcessed++;
		}
		else if (ResultMesh.TriangleCount() == TargetMesh->TriangleCount())
		{
			UE_LOG(LogTemp, Warning, TEXT("Chunk[%d]: No change (%d tris) - no actual intersection?"), ChunkIdx, TargetMesh->TriangleCount());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Chunk[%d]: %d -> %d tris (SUCCESS)"), ChunkIdx, TargetMesh->TriangleCount(), ResultMesh.TriangleCount());
			*TargetMesh = MoveTemp(ResultMesh);
			ChunkMesh->NotifyMeshUpdated();
			TotalChunksProcessed++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("RemoveTrianglesForDetachedCells: %d cells, %d chunks affected"),
		DetachedCellIds.Num(), TotalChunksProcessed);

	// 즉시 파편 정리
	CleanupSmallFragments();

	// 0.5초 후 추가 정리
	GetWorld()->GetTimerManager().SetTimer(
		FragmentCleanupTimerHandle,
		this,
		&URealtimeDestructibleMeshComponent::CleanupSmallFragments,
		0.5f,
		false
	);
}

void URealtimeDestructibleMeshComponent::CleanupSmallFragments()
{
	// 데디케이티드 서버에서는 파편 처리 스킵 (물리 NaN 오류 방지)
	if (IsRunningDedicatedServer())
	{
		return;
	}

	using namespace UE::Geometry;

	// Anchor에서 분리된 셀 집합 미리 계산 (BFS)
	// 통합 API 사용: bEnableSupercell, bEnableSubcell 플래그에 따라 자동 선택
	// Multiplayer: SubCell 상태는 Client에 동기화되지 않으므로 Standalone에서만 사용
	TSet<int32> DisconnectedCells;
	if (GridCellCache.IsValid())
	{
		const ENetMode NetMode = GetWorld()->GetNetMode();
		DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
			GridCellCache,
			SupercellCache,
			CellState,
			bEnableSupercell && SupercellCache.IsValid(),
			bEnableSubcell && (NetMode == NM_Standalone)); // subcell 동기화 안 하므로 subcell은 standalone에서만 허용
	}

	int32 TotalRemoved = 0;

	for (UDynamicMeshComponent* ChunkMesh : ChunkMeshComponents)
	{
		if (!ChunkMesh || !ChunkMesh->GetMesh()) continue;

		FDynamicMesh3* Mesh = ChunkMesh->GetMesh();
		if (Mesh->TriangleCount() == 0) continue;

		FMeshConnectedComponents ConnectedComponents(Mesh);
		ConnectedComponents.FindConnectedTriangles();

		if (ConnectedComponents.Num() <= 1) continue;

		FTransform MeshTransform = ChunkMesh->GetComponentTransform();

		int32 RemovedCount = 0;
		for (int32 i = 0; i < ConnectedComponents.Num(); ++i)
		{
			const auto& Comp = ConnectedComponents.GetComponent(i);

			// 바운딩 박스 계산
			FAxisAlignedBox3d BoundingBox = FAxisAlignedBox3d::Empty();
			FVector3d Centroid = FVector3d::Zero();
			int32 ValidCount = 0;

			for (int32 Tid : Comp.Indices)
			{
				if (!Mesh->IsTriangle(Tid)) continue;

				FIndex3i Tri = Mesh->GetTriangle(Tid);
				for (int32 j = 0; j < 3; ++j)
				{
					FVector3d Vertex = Mesh->GetVertex(Tri[j]);
					BoundingBox.Contain(Vertex);
				}

				Centroid += Mesh->GetTriCentroid(Tid);
				ValidCount++;
			}

			if (ValidCount > 0 && BoundingBox.Volume() > 0)
			{
				Centroid /= ValidCount;
				FVector WorldPos = MeshTransform.TransformPosition(FVector(Centroid));

				// CellState 기반 분리 판정: 컴포넌트가 속한 모든 셀이 파괴되었는지 확인
				bool bShouldRemove = false;
				int32 TotalCellCount = 0;
				int32 DestroyedCellCount = 0;

				if (GridCellCache.IsValid())
				{
					// 컴포넌트가 속한 고유 셀 ID 수집
					TSet<int32> ComponentCellIds;

					// 헬퍼 람다: 위치 → 셀 ID 추가
					auto AddCellIdFromPosition = [&](const FVector& Position)
					{
						FVector RelativePos = Position - GridCellCache.GridOrigin;
						FIntVector GridCoord(
							FMath::FloorToInt(RelativePos.X / GridCellCache.CellSize.X),
							FMath::FloorToInt(RelativePos.Y / GridCellCache.CellSize.Y),
							FMath::FloorToInt(RelativePos.Z / GridCellCache.CellSize.Z)
						);

						if (GridCoord.X >= 0 && GridCoord.X < GridCellCache.GridSize.X &&
							GridCoord.Y >= 0 && GridCoord.Y < GridCellCache.GridSize.Y &&
							GridCoord.Z >= 0 && GridCoord.Z < GridCellCache.GridSize.Z)
						{
							ComponentCellIds.Add(GridCellCache.CoordToId(GridCoord));
						}
					};

					for (int32 Tid : Comp.Indices)
					{
						if (!Mesh->IsTriangle(Tid)) continue;

						FIndex3i Tri = Mesh->GetTriangle(Tid);
						FVector3d V0 = Mesh->GetVertex(Tri[0]);
						FVector3d V1 = Mesh->GetVertex(Tri[1]);
						FVector3d V2 = Mesh->GetVertex(Tri[2]);

						// 버텍스 3개 검사
						AddCellIdFromPosition(FVector(V0));
						AddCellIdFromPosition(FVector(V1));
						AddCellIdFromPosition(FVector(V2));

						// 삼각형 중심점 검사
						AddCellIdFromPosition(FVector(Mesh->GetTriCentroid(Tid)));

						// Edge 중점 3개 검사
						AddCellIdFromPosition(FVector((V0 + V1) * 0.5));
						AddCellIdFromPosition(FVector((V1 + V2) * 0.5));
						AddCellIdFromPosition(FVector((V2 + V0) * 0.5));
					}

					TotalCellCount = ComponentCellIds.Num();

					// Anchor 연결성 검사: 컴포넌트의 셀 중 하나라도 Anchor에 연결되어 있는지 확인
					bool bConnectedToAnchor = false;
					for (int32 CellId : ComponentCellIds)
					{
						if (CellState.DestroyedCells.Contains(CellId))
						{
							DestroyedCellCount++;
						}
						// 파괴되지 않았고, Anchor에 연결된 셀이면 (DisconnectedCells에 없으면)
						else if (!DisconnectedCells.Contains(CellId))
						{
							bConnectedToAnchor = true;
						}
					}

					// 분리 조건: Anchor에 연결된 셀이 하나도 없으면 제거
					bShouldRemove = !bConnectedToAnchor;
				}

				// 디버그 색상: 빨강 = 삭제 대상 (Anchor 분리), 초록 = 유지 (Anchor 연결)
				if (bShowCellSpawnPosition)
				{
					FColor PointColor = bShouldRemove ? FColor::Red : FColor::Green;
					DrawDebugPoint(GetWorld(), WorldPos, 15.0f, PointColor, false, 10.0f);
					DrawDebugString(GetWorld(), WorldPos, FString::Printf(TEXT("%s (%d/%d destroyed)"),
						bShouldRemove ? TEXT("Detached") : TEXT("Anchored"), DestroyedCellCount, TotalCellCount), nullptr, PointColor, 10.0f);
				}

				// 모든 셀이 파괴된 컴포넌트는 파편으로 스폰 후 삭제
				if (bShouldRemove)
				{
					// 메쉬 데이터 추출
					TMap<int32, int32> OldToNewVertexMap;
					TArray<FVector> DebrisVertices;
					TArray<int32> DebrisTriangles;

					for (int32 Tid : Comp.Indices)
					{
						if (!Mesh->IsTriangle(Tid)) continue;

						FIndex3i Tri = Mesh->GetTriangle(Tid);
						int32 NewTriIndices[3];

						for (int32 j = 0; j < 3; ++j)
						{
							int32 OldVid = Tri[j];
							if (int32* Found = OldToNewVertexMap.Find(OldVid))
							{
								NewTriIndices[j] = *Found;
							}
							else
							{
								int32 NewIdx = DebrisVertices.Num();
								OldToNewVertexMap.Add(OldVid, NewIdx);
								// 로컬 -> 월드 -> 스폰위치 기준 상대좌표로 변환
								FVector3d LocalPos = Mesh->GetVertex(OldVid);
								FVector WorldVertexPos = MeshTransform.TransformPosition(FVector(LocalPos));
								DebrisVertices.Add(WorldVertexPos - WorldPos);
								NewTriIndices[j] = NewIdx;
							}
						}

						// 원래 와인딩 순서 유지
						DebrisTriangles.Add(NewTriIndices[0]);
						DebrisTriangles.Add(NewTriIndices[1]);
						DebrisTriangles.Add(NewTriIndices[2]);
					}

					// 파편 액터 스폰
					if (DebrisVertices.Num() >= 3 && DebrisTriangles.Num() >= 3)
					{
						UWorld* World = GetWorld();
						if (World)
						{
							// 1. 유효성 검사 먼저 수행
							FBox DebrisBounds(DebrisVertices);
							FVector BoundsSize = DebrisBounds.GetSize();
							float DebrisSize = BoundsSize.Size();
							float MinAxisSize = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);

							// NaN/Inf 체크
							bool bHasValidVerts = true;
							for (const FVector& V : DebrisVertices)
							{
								if (V.ContainsNaN() || !FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y) || !FMath::IsFinite(V.Z))
								{
									bHasValidVerts = false;
									break;
								}
							}

							// 물리 사용 가능 조건 (더 보수적)
							// - 유효한 정점
							// - 최소 12개 정점 (더 안정적인 Convex)
							// - 전체 크기 5cm 이상
							// - 각 축 최소 2cm 이상 (납작한 형태 방지)
							bool bCanUsePhysics = bHasValidVerts
								&& DebrisVertices.Num() >= 12
								&& DebrisSize >= 5.0f
								&& MinAxisSize >= 2.0f;
							if (!bCanUsePhysics)
							{
								UE_LOG(LogTemp, Verbose, TEXT("Debris skipped: Verts=%d, Size=%.1f, MinAxis=%.2f %s"),
									DebrisVertices.Num(), DebrisSize, MinAxisSize,
									!bHasValidVerts ? TEXT("(NaN)") :
									DebrisVertices.Num() < 12 ? TEXT("(Verts<12)") :
									DebrisSize < 5.0f ? TEXT("(Size<5)") :
									MinAxisSize < 2.0f ? TEXT("(Flat)") : TEXT(""));

								// 원본 메쉬에서 삼각형 삭제 (PCM 스폰 안해도 반드시 수행)
								for (int32 Tid : Comp.Indices)
								{
									Mesh->RemoveTriangle(Tid);
								}
								RemovedCount++;
								continue;
							}


							FActorSpawnParameters SpawnParams;
							SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

							AActor* DebrisActor = World->SpawnActor<AActor>(AActor::StaticClass(), WorldPos, FRotator::ZeroRotator, SpawnParams);
							if (DebrisActor)
							{
								// ProceduralMeshComponent 생성
								UProceduralMeshComponent* ProcMesh = NewObject<UProceduralMeshComponent>(DebrisActor, UProceduralMeshComponent::StaticClass(), TEXT("DebrisMesh"));
								ProcMesh->SetMobility(EComponentMobility::Movable);
								ProcMesh->bUseComplexAsSimpleCollision = false;

								// UV 생성
								TArray<FVector2D> DebrisUVs;
								DebrisUVs.SetNum(DebrisVertices.Num());

								// 메쉬 섹션 생성 (노말 자동 계산)
								ProcMesh->CreateMeshSection_LinearColor(0, DebrisVertices, DebrisTriangles, TArray<FVector>(), DebrisUVs,
									TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);

								// 원본 머티리얼 적용
								if (ChunkMesh->GetNumMaterials() > 0)
								{
									UMaterialInterface* OrigMat = ChunkMesh->GetMaterial(0);
									if (OrigMat)
									{
										ProcMesh->SetMaterial(0, OrigMat);
									}
								}

								// ★ 순서 중요: RootComponent 설정 → RegisterComponent → SetActorLocation → Collision 추가
								DebrisActor->SetRootComponent(ProcMesh);
								ProcMesh->RegisterComponent();
								DebrisActor->AddInstanceComponent(ProcMesh);

								// 위치 명시적 설정 (Collision 추가 전에 반드시 설정)
								DebrisActor->SetActorLocation(WorldPos);

								// 물리 설정 - 유효성 검사 통과 + 데디케이티드 서버 아닐 때만
								if (bCanUsePhysics && !IsRunningDedicatedServer())
								{
									// ★ Transform 설정 완료 후에 Convex Collision 추가 (Local Space 좌표)
									ProcMesh->AddCollisionConvexMesh(DebrisVertices);

									// 질량 자동 계산 방지 - 고정 질량 사용
									ProcMesh->SetMassOverrideInKg(NAME_None, 5.0f, true);

									ProcMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
									ProcMesh->SetCollisionResponseToAllChannels(ECR_Block);
									ProcMesh->SetSimulatePhysics(true);

									// 약간 아래로 초기 속도
									ProcMesh->SetPhysicsLinearVelocity(FVector(0, 0, -50.0f));

									// 랜덤 회전
									FVector RandomAngular(
										FMath::RandRange(-90.0f, 90.0f),
										FMath::RandRange(-90.0f, 90.0f),
										FMath::RandRange(-90.0f, 90.0f)
									);
									ProcMesh->SetPhysicsAngularVelocityInDegrees(RandomAngular);
								}
								else
								{
									// 물리 사용 불가 시 충돌 비활성화하고 렌더링만
									ProcMesh->SetSimulatePhysics(false);
									ProcMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
								}

								// 10초 후 삭제
								DebrisActor->SetLifeSpan(10.0f);

								UE_LOG(LogTemp, Warning, TEXT("Debris: Verts=%d, Size=%.1f, MinAxis=%.2f, Physics=%s %s"),
									DebrisVertices.Num(), DebrisSize, MinAxisSize,
									bCanUsePhysics ? TEXT("ON") : TEXT("OFF"),
									!bCanUsePhysics ?
										(!bHasValidVerts ? TEXT("(NaN)") :
										 DebrisVertices.Num() < 12 ? TEXT("(Verts<12)") :
										 DebrisSize < 5.0f ? TEXT("(Size<5)") :
										 MinAxisSize < 2.0f ? TEXT("(Flat)") : TEXT(""))
										: TEXT(""));

								// 디버그: 스폰 위치에 파란 구체 표시
								if (bShowCellSpawnPosition)
								{
									DrawDebugSphere(World, WorldPos, 20.0f, 8, FColor::Blue, false, 10.0f);
								}
							}
						}
					}

					// 원본 메쉬에서 삼각형 삭제
					for (int32 Tid : Comp.Indices)
					{
						Mesh->RemoveTriangle(Tid);
					}
					RemovedCount++;
				}
			}
		}

		if (RemovedCount > 0)
		{
			Mesh->CompactInPlace();
			ChunkMesh->NotifyMeshUpdated();
			TotalRemoved += RemovedCount;
		}
	}

	if (TotalRemoved > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CleanupSmallFragments: Removed %d fragments (overlaps destroyed cells)"),
			TotalRemoved);
	}
}

void URealtimeDestructibleMeshComponent::SpawnDebrisFromCells(const TArray<int32>& DetachedCellIds, const FVector& InitialLocation, const FVector& InitialVelocity)
{
	// 파편 스폰은 이제 CleanupSmallFragments에서 직접 처리됨
	// 이 함수는 향후 확장을 위해 유지
}

bool URealtimeDestructibleMeshComponent::ServerEnqueueOps_Validate(const TArray<FRealtimeDestructionRequest>& Requests)
{
	// 1단계: 명백한 치트 검사 → 실패 시 클라이언트 킥

	// 비정상적으로 많은 요청 (DoS 시도)
	if (Requests.Num() > MaxRequestsPerRPC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerValidate] 비정상적 요청 수: %d (최대: %d) → 킥"),
			Requests.Num(), MaxRequestsPerRPC);
		return false;
	}

	// 각 요청의 기본 유효성 검사
	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		// 비정상적으로 큰 파괴 반경
		if (Request.ShapeParams.Radius > MaxAllowedRadius)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerValidate] 비정상 반경: %.1f (최대: %.1f) → 킥"),
				Request.ShapeParams.Radius, MaxAllowedRadius);
			return false;
		}

		// 유효하지 않은 ChunkIndex
		if (Request.ChunkIndex != INDEX_NONE &&
			ChunkMeshComponents.Num() > 0 &&
			Request.ChunkIndex >= ChunkMeshComponents.Num())
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerValidate] 유효하지 않은 ChunkIndex: %d (최대: %d) → 킥"),
				Request.ChunkIndex, ChunkMeshComponents.Num() - 1);
			return false;
		}
	}

	return true;
}

bool URealtimeDestructibleMeshComponent::CheckRateLimit(APlayerController* Player)
{
	if (!Player)
	{
		return true;  // 플레이어 없으면 검증 스킵
	}

	const double CurrentTime = FPlatformTime::Seconds();
	FRateLimitInfo& Info = PlayerRateLimits.FindOrAdd(Player);

	// 1초 윈도우 리셋
	if (CurrentTime - Info.WindowStartTime > 1.0)
	{
		Info.WindowStartTime = CurrentTime;
		Info.RequestCount = 0;
	}

	Info.RequestCount++;

	// 연사 제한 초과
	if (Info.RequestCount > static_cast<int32>(MaxDestructionsPerSecond))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RateLimit] 플레이어 %s: 초당 %d회 (최대: %.0f)"),
			*Player->GetName(), Info.RequestCount, MaxDestructionsPerSecond);
		return false;
	}

	return true;
}

void URealtimeDestructibleMeshComponent::ServerEnqueueOps_Implementation(const TArray<FRealtimeDestructionRequest>& Requests)
{
	UE_LOG(LogTemp, Display, TEXT("ServerEnqueueOps: 서버에서 %d개 요청 수신"), Requests.Num());

	TArray<FRealtimeDestructionOp> Ops;
	Ops.Reserve(Requests.Num());

	for (const FRealtimeDestructionRequest& Request : Requests)
	{
		// 세부 검증 (킥은 안 하고 요청만 거부)
		EDestructionRejectReason Reason;
		if (!ValidateDestructionRequest(Request, nullptr, Reason))
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerEnqueueOps] 요청 거부 - 사유: %d"), static_cast<int32>(Reason));
			continue;
		}

		FRealtimeDestructionOp Op;
		Op.OpId.Value = NextOpId++;
		Op.Sequence = NextSequence++;
		Op.Request = Request;
		Ops.Add(Op);
	}

	// 유효한 Op가 있으면 Multicast
	if (Ops.Num() > 0)
	{
		MulticastApplyOps(Ops);
	}
}

void URealtimeDestructibleMeshComponent::MulticastApplyOps_Implementation(const TArray<FRealtimeDestructionOp>& Ops)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		ApplyOpsDeterministic(Ops);
		return;
	}

	// 네트워크 역할 확인
	const ENetMode NetMode = World->GetNetMode();

	// 클라이언트에서 수신 데이터 기록 및 RTT 측정
	if (NetMode == NM_Client)
	{
		if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
		{
			// 수신 데이터 크기 기록 (비압축)
			constexpr int32 UNCOMPRESSED_OP_SIZE = 40;
			constexpr int32 RPC_OVERHEAD = 8;
			Debugger->RecordBytesReceived(Ops.Num() * UNCOMPRESSED_OP_SIZE + RPC_OVERHEAD);

			for (const FRealtimeDestructionOp& Op : Ops)
			{
				// ClientSendTime이 설정되어 있으면 RTT 계산
				if (Op.Request.ClientSendTime > 0.0)
				{
					double CurrentTime = FPlatformTime::Seconds();
					float RTTMs = static_cast<float>((CurrentTime - Op.Request.ClientSendTime) * 1000.0);
					Debugger->RecordRTT(RTTMs);
				}
			}
		}
	}

	// UE_LOG(LogTemp, Warning, TEXT("MulticastApplyOps: [%s] %d개 Op 적용"), *RoleName, Ops.Num());
	ApplyOpsDeterministic(Ops);
}

void URealtimeDestructibleMeshComponent::MulticastApplyOpsCompact_Implementation(const TArray<FCompactDestructionOp>& CompactOps)
{
	// 클라이언트에서 수신 데이터 크기 기록 (압축)
	if (UWorld* World = GetWorld())
	{
		if (World->GetNetMode() == NM_Client)
		{
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				constexpr int32 COMPACT_OP_SIZE = 15;
				constexpr int32 RPC_OVERHEAD = 8;
				Debugger->RecordBytesReceived(CompactOps.Num() * COMPACT_OP_SIZE + RPC_OVERHEAD);
			}
		}
	}

	// 압축 해제 후 적용
	TArray<FRealtimeDestructionOp> Ops;
	Ops.Reserve(CompactOps.Num());

	for (const FCompactDestructionOp& CompactOp : CompactOps)
	{
		FRealtimeDestructionOp Op;
		Op.Request = CompactOp.Decompress();
		Ops.Add(Op);
	}

	ApplyOpsDeterministic(Ops);
}

void URealtimeDestructibleMeshComponent::MulticastDestroyedCells_Implementation(const TArray<int32>& DestroyedCellIds)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const ENetMode NetMode = World->GetNetMode();

	// DedicatedServer는 스킵 (렌더링 없음)
	if (NetMode == NM_DedicatedServer)
	{
		return;
	}

	// 서버는 이미 로컬에서 처리했으므로 스킵
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}
	
	if (DestroyedCellIds.Num() > 0)
	{
		RecentDirectDestroyedCellIds.Reset();
		RecentDirectDestroyedCellIds.Append(DestroyedCellIds);
	}

	// 클라이언트: CellState에 파괴된 셀 추가 + SuperCell 상태 업데이트
	for (int32 CellId : DestroyedCellIds)
	{
		CellState.DestroyedCells.Add(CellId);

		// SuperCell 상태 업데이트 (Cell 파괴 정보만으로 Server와 동기화)
		if (bEnableSupercell && SupercellCache.IsValid())
		{
			SupercellCache.OnCellDestroyed(CellId);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[Client] MulticastDestroyedCells: +%d cells, Total=%d"),
		DestroyedCellIds.Num(), CellState.DestroyedCells.Num());
}

void URealtimeDestructibleMeshComponent::MulticastDetachSignal_Implementation()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const ENetMode NetMode = World->GetNetMode();

	// DedicatedServer는 스킵 (렌더링 없음)
	if (NetMode == NM_DedicatedServer)
	{
		return;
	}

	// 서버는 이미 로컬에서 처리했으므로 스킵
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Client] MulticastDetachSignal RECEIVED - Running local BFS"));

	// 클라이언트: 자체 BFS 실행하여 분리된 셀 찾기
	// 통합 API 사용: 서버와 동일한 알고리즘으로 일관성 유지
	// Multiplayer: SubCell 상태는 Client에 동기화되지 않으므로 Standalone에서만 사용
	TSet<int32> DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
		GridCellCache,
		SupercellCache,
		CellState,
		bEnableSupercell && SupercellCache.IsValid(),
		bEnableSubcell && (NetMode == NM_Standalone)); // subcell 동기화 안 하므로 subcell은 standalone에서만 허용 

	if (DisconnectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Client] BFS result: No disconnected cells"));
		return;
		}

	UE_LOG(LogTemp, Warning, TEXT("[Client] BFS result: %d disconnected cells"), DisconnectedCells.Num());

	// 분리된 셀 그룹화
	TArray<TArray<int32>> DetachedGroups = FCellDestructionSystem::GroupDetachedCells(
		GridCellCache,
		DisconnectedCells,
		CellState.DestroyedCells);

	UE_LOG(LogTemp, Warning, TEXT("[Client] Grouped into %d debris groups"), DetachedGroups.Num());

	// 각 그룹에 대해 처리
	for (const TArray<int32>& Group : DetachedGroups)
	{
		// CellState에 Detached 그룹 추가
		CellState.AddDetachedGroup(Group);

		// 분리된 셀의 삼각형 삭제 (시각적 처리)
		RemoveTrianglesForDetachedCells(Group);
	}

	// 분리된 셀들을 파괴됨 상태로 이동
	CellState.MoveAllDetachedToDestroyed();

	UE_LOG(LogTemp, Warning, TEXT("[Client] Detach processing complete"));
}

void URealtimeDestructibleMeshComponent::ApplyOpsDeterministic(const TArray<FRealtimeDestructionOp>& Ops)
{
	if (Ops.IsEmpty())
	{
		return;
	}

	// 서버는 이미 로컬에서 처리했으므로 Multicast 수신 시 스킵
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	for (const FRealtimeDestructionOp& Op : Ops)
	{
		// 클라이언트: ToolMeshPtr가 없으면 ShapeParams로 재생성
		FRealtimeDestructionRequest ModifiableRequest = Op.Request;
		if (!ModifiableRequest.ToolMeshPtr.IsValid())
		{
			ModifiableRequest.ToolMeshPtr = CreateToolMeshPtrFromShapeParams(
				ModifiableRequest.ToolShape,
				ModifiableRequest.ShapeParams
			);
		}

		// ToolCenterWorld는 Decompress()에서 이미 계산됨

		// DecalMaterial 조회 (네트워크로 전송된 ConfigID로 로컬에서 조회)
		// 1. 컴포넌트에 설정된 DecalDataAsset 사용
		// 2. 없으면 GameInstanceSubsystem에서 조회 
		UDecalMaterialDataAsset* DataAssetToUse = nullptr;
		if (UGameInstance* GI = GetWorld()->GetGameInstance())
		{
			if (UDestructionGameInstanceSubsystem* Subsystem = GI->GetSubsystem<UDestructionGameInstanceSubsystem>())
			{
				DataAssetToUse = Subsystem->FindDataAssetByConfigID(ModifiableRequest.DecalConfigID);
			}
		}


		if (DataAssetToUse && ModifiableRequest.bSpawnDecal)
		{
			FDecalSizeConfig FoundConfig;
			if (DataAssetToUse->GetConfigRandom(ModifiableRequest.SurfaceType, FoundConfig))
			{
				ModifiableRequest.DecalMaterial = FoundConfig.DecalMaterial;
				ModifiableRequest.DecalSize = FoundConfig.DecalSize;
				ModifiableRequest.DecalLocationOffset = FoundConfig.LocationOffset;
				ModifiableRequest.DecalRotationOffset = FoundConfig.RotationOffset;
			}
		}

		// 데칼 생성 (관통이 아닐 때만 - 로컬 경로와 동일)
		UDecalComponent* TempDecal = nullptr;
		if (!Op.bIsPenetration)
		{
			TempDecal = SpawnTemporaryDecal(ModifiableRequest);
		}

		// 비동기 경로로 처리 (워커 스레드 사용)
		EnqueueRequestLocal(ModifiableRequest, Op.bIsPenetration, TempDecal);
	}
}

void URealtimeDestructibleMeshComponent::GetDestructionSettings(int32& OutMaxHoleCount, int32& OutMaxBatchSize)
{
	OutMaxHoleCount = MaxHoleCount;
	OutMaxBatchSize = MaxBatchSize;
}


bool URealtimeDestructibleMeshComponent::InitializeFromStaticMeshInternal(UStaticMesh* InMesh, bool bForce)
{
	// 1. 유효성 검사
	if (!InMesh)
	{
		// 메쉬가 None이 되면 화면에서도 지워줍니다.
		if (UDynamicMesh* Mesh = GetDynamicMesh())
		{
			Mesh->Reset();
		}
		ApplyRenderUpdate();
		return false;
	}
	UE_LOG(LogTemp, Warning, TEXT("New Static Mesh Name: %s"), *InMesh->GetName());

	// 2. 이미 초기화되었고 강제(bForce)가 아니면 스킵
	if (bIsInitialized && !bForce)
	{
		return true;
	}

	UDynamicMesh* DynamicMesh = GetDynamicMesh();
	if (!DynamicMesh)
	{
		return false;
	}
	DynamicMesh->EditMesh([](FDynamicMesh3& Mesh) {
		Mesh.Clear();
		});

	// =========================================================
	// [핵심 1] 에디터에게 "나 수정된다!"라고 알림 (Undo/Redo 및 뷰포트 갱신 트리거)
	// =========================================================
#if WITH_EDITOR
	Modify();
	DynamicMesh->Modify();
#endif

	// =========================================================
	// [핵심 2] 기존 데이터 완전 초기화 (찌꺼기 제거)
	// =========================================================
	DynamicMesh->Reset();


	// 3. Static Mesh 복사 설정
	FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
	CopyOptions.bApplyBuildSettings = true;
	CopyOptions.bRequestTangents = true;

	// 만약 Static Mesh의 'Allow CPU Access'가 꺼져있어도 에디터에서는 동작하도록 설정
	CopyOptions.bIgnoreRemoveDegenerates = false;

	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	// 4. 복사 실행
	UDynamicMesh* ResultMesh = UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh(
		InMesh,
		DynamicMesh,
		CopyOptions,
		FGeometryScriptMeshReadLOD(),
		Outcome
	);

	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to copy mesh"));
		return false;
	}
	
	// 내부 material을 사용하기 위한 설정
	if (ResultMesh)
	{
		ResultMesh->EditMesh([&](FDynamicMesh3& EditMesh) {

			// Att 기능 활성화
			EditMesh.EnableAttributes();

			// Material ID 활성화 
			EditMesh.Attributes()->EnableMaterialID();
			});
	}
	
	// 5. 머티리얼 및 콜리전 복사
	CopyMaterialsFromStaticMesh(InMesh);
	SetComplexAsSimpleCollisionEnabled(true);

	// =========================================================
	// [핵심 3] 강제 렌더링 상태 재생성 (기존 ApplyRenderUpdate보다 강력하게)
	// =========================================================
	//NotifyMeshUpdated();        // 내부 데이터 버전 올림
	//MarkRenderStateDirty();     // 렌더 상태 더러움 표시
	//RecreatePhysicsState();     // 물리 상태 재생성 (Bounds 갱신을 위해 필수)
	//RecreateRenderState_Concurrent(); // 렌더 프록시 재생성
	SetMesh(MoveTemp(ResultMesh->GetMeshRef()));

	// 상태값 갱신
	CurrentHoleCount = 0;
	bIsInitialized = true; // Construction Script에서 중복 실행 방지
	OnInitialized.Broadcast();

	return true;
}

UDynamicMesh* URealtimeDestructibleMeshComponent::CreateToolMeshFromRequest(const FRealtimeDestructionRequest& Request)
{
	UDynamicMesh* ToolMesh = NewObject<UDynamicMesh>();
	if (!ToolMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create ToolMesh"));
		return nullptr;
	}

	return ToolMesh;
}

TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> URealtimeDestructibleMeshComponent::CreateToolMeshPtrFromShapeParams(
	EDestructionToolShape ToolShape,
	const FDestructionToolShapeParams& ShapeParams)
{
	UDynamicMesh* TempMesh = NewObject<UDynamicMesh>(this);
	if (!TempMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateToolMeshPtrFromShapeParams: Failed to create TempMesh"));
		return nullptr;
	}

	FGeometryScriptPrimitiveOptions PrimitiveOptions;
	PrimitiveOptions.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::SingleGroup;

	switch (ToolShape)
	{
	case EDestructionToolShape::Sphere:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.StepsPhi,
			ShapeParams.StepsTheta,
			EGeometryScriptPrimitiveOriginMode::Center
		);
		break;

	case EDestructionToolShape::Cylinder:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.Height,
			ShapeParams.RadiusSteps,
			ShapeParams.HeightSubdivisions,
			ShapeParams.bCapped,
			EGeometryScriptPrimitiveOriginMode::Base
		);
		break;

	default:
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
			TempMesh,
			PrimitiveOptions,
			FTransform::Identity,
			ShapeParams.Radius,
			ShapeParams.Height,
			ShapeParams.RadiusSteps,
			ShapeParams.HeightSubdivisions,
			ShapeParams.bCapped,
			EGeometryScriptPrimitiveOriginMode::Base
		);
		break;
	}

	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> Result = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	TempMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Source)
		{
			*Result = Source;
		});

	return Result;
}

void URealtimeDestructibleMeshComponent::CopyMaterialsFromStaticMesh(UStaticMesh* InMesh)
{
	if (!InMesh)
	{
		return;
	}

	const int32 NumMaterials = InMesh->GetStaticMaterials().Num();
	for (int32 Index = 0; Index < NumMaterials; ++Index)
	{
		if (UMaterialInterface* Material = InMesh->GetMaterial(Index))
		{
			SetMaterial(Index, Material);
		}
	}
}

// 아직 사용되는 경로 없음
void URealtimeDestructibleMeshComponent::CopyMaterialsFromStaticMeshComponent(UStaticMeshComponent* InComp)
{
	if (!InComp)
	{
		return;
	}

	const int32 NumMaterials = InComp->GetNumMaterials();
	for (int32 Index = 0; Index < NumMaterials; ++Index)
	{
		if (UMaterialInterface* Material = InComp->GetMaterial(Index))
		{
			SetMaterial(Index, Material);
		}
	}
}

void URealtimeDestructibleMeshComponent::CopyCollisionFromStaticMeshComponent(UStaticMeshComponent* InComp)
{
	if (!InComp)
	{
		return;
	}

	SetCollisionEnabled(InComp->GetCollisionEnabled());
	SetCollisionProfileName(InComp->GetCollisionProfileName());
	SetCollisionResponseToChannels(InComp->GetCollisionResponseToChannels());
	SetGenerateOverlapEvents(InComp->GetGenerateOverlapEvents());
	SetComplexAsSimpleCollisionEnabled(true);
}

void URealtimeDestructibleMeshComponent::ApplyRenderUpdate()
{
	NotifyMeshUpdated();
	MarkRenderStateDirty();
	RecreateRenderState_Concurrent();

}

void URealtimeDestructibleMeshComponent::ApplyCollisionUpdate(UDynamicMeshComponent* TargetComp)
{
	TargetComp->UpdateCollision();

	/*
	 * deprecated_realdestruction
	 * UpdateCollision 내부에서 RecreatePhysicsState함수 호출
	 */
	 // RecreatePhysicsState();
}

void URealtimeDestructibleMeshComponent::ApplyCollisionUpdateAsync(UDynamicMeshComponent* TargetComp)
{
	UE_LOG(LogTemp, Display, TEXT("Call Collision Update %f"), FPlatformTime::Seconds());
	TargetComp->UpdateCollision(true);
}

bool URealtimeDestructibleMeshComponent::CheckPenetration(const FRealtimeDestructionRequest& Request, float& OutPenetration)
{
	FVector StartPoint = Request.ImpactPoint;
	FVector ForwardDir = Request.ImpactNormal * -1.0f; // 총알 진행 방향

	// 관통 체크할 최소 두께 .
	float MaxPenetrationDepth = 150.0f;

	// 위에서 설정한 길이 만큼 벽 뒤로가서 Ray를 쏜다.
	FVector ProbeStart = StartPoint + (ForwardDir * MaxPenetrationDepth);
	FVector ProbeEnd = StartPoint;

	FHitResult BackHit;
	FCollisionQueryParams Params;

	Params.bTraceComplex = true; // Mesh의 정확한 폴리곤을 찍기 위해 켭니다.

	// 뒤에서 앞으로 쏘는 Ray 
	bool bHitBack = GetWorld()->LineTraceSingleByChannel(BackHit, ProbeStart, ProbeEnd, ECC_Visibility, Params);

	if (bDebugPenetration)
	{
		// 잘 되는 지 시각화 용  
		DrawDebugLine(GetWorld(), ProbeStart, bHitBack ? BackHit.ImpactPoint : ProbeEnd, FColor::Purple, false, 5.0f, 0, 1.0f);
	}

	if (bHitBack)
	{
		// Hit된게 본인이여한다. (다른 벽 말고) 		
		if (BackHit.GetActor() == GetOwner())
		{

			// 두께 계산: (원래 맞은 앞면) <-> (지금 맞은 뒷면) 거리
			float Thickness = FVector::Dist(StartPoint, BackHit.ImpactPoint);

			// 디버그 출력
			if (bDebugPenetration)
			{
				DrawDebugPoint(GetWorld(), BackHit.ImpactPoint, 10.0f, FColor::Cyan, false, 5.0f);
				FString Msg = FString::Printf(TEXT("Wall Thickness: %.2f"), Thickness);
				DrawDebugString(GetWorld(), BackHit.Location, Msg, nullptr, FColor::White, 5.0f);
			}

			// ThicknessOffset이  0일 때, 임의의 로직으로 계산해준다
			if (ThicknessOffset == 0)
			{
				switch (Request.ToolShape)
				{
				case EDestructionToolShape::Sphere:
					ThicknessOffset = Request.Depth * 2.0f;
					break;

				case EDestructionToolShape::Cylinder:
					ThicknessOffset = Request.Depth * 1.5f;
					break;

				default:
					ThicknessOffset = Request.Depth * 1.5f;
					break;
				}
			}

			// 두께가 얇으면 관통 성공  
			if (Thickness <= ThicknessOffset)
			{
				OutPenetration = Thickness * 1.1f;
				return true;
			}
		}
	}
	return false;
}

void URealtimeDestructibleMeshComponent::SettingAsyncOption(bool& OutMultiWorker)
{
	OutMultiWorker = bEnableMultiWorkers;
}

int32 URealtimeDestructibleMeshComponent::GetChunkIndex(const UPrimitiveComponent* ChunkMesh)
{
	if (int32* Index = ChunkIndexMap.Find(ChunkMesh))
	{
		return *Index;
	}

	return INDEX_NONE;
}

bool URealtimeDestructibleMeshComponent::IsChunkValid(int32 ChunkIndex) const
{
	return GetChunkMeshComponent(ChunkIndex) != nullptr;
}

UDynamicMeshComponent* URealtimeDestructibleMeshComponent::GetChunkMeshComponent(int32 ChunkIndex) const
{
	if (ChunkIndex == INDEX_NONE)
	{
		return nullptr;
	}

	return ChunkMeshComponents[ChunkIndex].Get();
}

bool URealtimeDestructibleMeshComponent::GetChunkMesh(FDynamicMesh3& OutMesh, int32 ChunkIndex) const
{
	if (auto MeshComp = GetChunkMeshComponent(ChunkIndex))
	{
		MeshComp->ProcessMesh([&](const FDynamicMesh3& Source)
			{
				OutMesh = Source;
			});

		return true;
	}

	return false;
}

bool URealtimeDestructibleMeshComponent::CheckAndSetChunkBusy(int32 ChunkIndex)
{
	// 비트 배열의 인덱스
	// 0 ~ 63(0), 64 ~ 127(1) ...
	const int32 BitIndex = ChunkIndex / 64;
	if (!ChunkBusyBits.IsValidIndex(BitIndex))
	{
		// 유효하지 않은 CellIndex의 경우 로그를 출력하고 true를 반환해서 작업하지 못하게 한다.
		UE_LOG(LogTemp, Warning, TEXT("Invalid Cell Index: %d"), ChunkIndex);
		return true;
	}

	// 해당 비트 마스크에서 비트 위치
	const int32 BitOffset = ChunkIndex % 64;
	const uint64 BitMask = 1ULL << BitOffset;

	const bool bIsBusy = (ChunkBusyBits[BitIndex] & BitMask) != 0;
	if (!bIsBusy)
	{
		ChunkBusyBits[BitIndex] |= BitMask;
	}

	return bIsBusy;
}

void URealtimeDestructibleMeshComponent::FindChunksInRadius(const FVector& WorldCenter, float Radius, TArray<int32>& OutChunkIndices, bool bAppend)
{
	if (!bAppend)
	{
		OutChunkIndices.Reset();
	}

	if (GridToChunkMap.Num() == 0 || SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		return;
	}

	// 월드 좌표를 로컬 좌표로 변환
	FVector LocalCenter = GetComponentTransform().InverseTransformPosition(WorldCenter);

	// 스케일도 고려해서 Radius 변환 (비균일 스케일이면 근사치)
	FVector LocalRadius = GetComponentTransform().InverseTransformVector(FVector(Radius));
	float LocalRadiusScalar = LocalRadius.GetAbsMax();  // 또는 평균값

	const FVector& CellSize = CachedChunkSize;
	const FBox& MeshBounds = CachedMeshBounds;

	//  로컬 좌표로 계산
	FVector MinPos = LocalCenter - FVector(LocalRadiusScalar);
	FVector MaxPos = LocalCenter + FVector(LocalRadiusScalar);

	int32 MinGridX = FMath::Clamp(static_cast<int32>((MinPos.X - MeshBounds.Min.X) / CellSize.X), 0, SliceCount.X - 1);
	int32 MaxGridX = FMath::Clamp(static_cast<int32>((MaxPos.X - MeshBounds.Min.X) / CellSize.X), 0, SliceCount.X - 1);
	int32 MinGridY = FMath::Clamp(static_cast<int32>((MinPos.Y - MeshBounds.Min.Y) / CellSize.Y), 0, SliceCount.Y - 1);
	int32 MaxGridY = FMath::Clamp(static_cast<int32>((MaxPos.Y - MeshBounds.Min.Y) / CellSize.Y), 0, SliceCount.Y - 1);
	int32 MinGridZ = FMath::Clamp(static_cast<int32>((MinPos.Z - MeshBounds.Min.Z) / CellSize.Z), 0, SliceCount.Z - 1);
	int32 MaxGridZ = FMath::Clamp(static_cast<int32>((MaxPos.Z - MeshBounds.Min.Z) / CellSize.Z), 0, SliceCount.Z - 1);


	/*
	 * 1. GridToChunk를 만들 때 하나의 그리드에 하나의 청크가 매핑되도록 생성됨
	 * 2. 3중 for문을 돌면서 GridIndex가 다름
	 * 위 2가지 이유로 청크인덱스가 중복될 수 없으므로 TSet을 주석처리해서 배열의 메모리 재활용 하도록 변경 
	 */
	// 해당 범위의 그리드 셀만 순회
	// TSet<int32> UniqueChunks;
	for (int32 Z = MinGridZ; Z <= MaxGridZ; ++Z)
	{
		for (int32 Y = MinGridY; Y <= MaxGridY; ++Y)
		{
			for (int32 X = MinGridX; X <= MaxGridX; ++X)
			{
				int32 GridIndex = X + Y * SliceCount.X + Z * SliceCount.X * SliceCount.Y;

				if (GridIndex >= 0 && GridIndex < GridToChunkMap.Num())
				{
					int32 ChunkId = GridToChunkMap[GridIndex];
					if (ChunkId != INDEX_NONE)
					{
						OutChunkIndices.Add(ChunkId);
						// UniqueChunks.Add(ChunkId);
					}
				}
			}
		}
	}

	// OutChunkIndices = UniqueChunks.Array();
}

void URealtimeDestructibleMeshComponent::FindChunksAlongLine(const FVector& WorldStart, const FVector& WorldEnd, float Radius, TArray<int32>& OutChunkIndices, bool bAppend)
{
	if (!bAppend)
	{
		OutChunkIndices.Reset();
	}
	
	FVector Forward = WorldEnd - WorldStart;	
	if (Forward.IsNearlyZero())
	{
		return;
	}

	Forward.Normalize();
	FVector Right, Up;
	Forward.FindBestAxisVectors(Right, Up);

	const float OffsetRadius = Radius * 0.9f;

	// 실린더의 중심과 4방향의 offset
	FVector Offsets[] = {
		FVector::ZeroVector,
		Right * OffsetRadius,
		-Right * OffsetRadius,
		Up * OffsetRadius,
		-Up * OffsetRadius
	};

	// 5방향 DDA 시작
	for (const FVector& Offset : Offsets)
	{
		FVector RayStart = WorldStart + Offset;
		FVector RayEnd = WorldEnd + Offset;

		FindChunksAlongLineInternal(RayStart, RayEnd, OutChunkIndices);
	}

	if (OutChunkIndices.Num() > 1)
	{
		OutChunkIndices.Sort();
		int32 UniqueCount = Algo::Unique(OutChunkIndices);
		OutChunkIndices.SetNum(UniqueCount);
	}
}

void URealtimeDestructibleMeshComponent::ClearChunkBusy(int32 ChunkIndex)
{
	const int32 BitIndex = ChunkIndex / 64;
	if (!ChunkBusyBits.IsValidIndex(BitIndex))
	{
		// 유효하지 않은 CellIndex의 경우 로그를 출력하고 true를 반환해서 작업하지 못하게 한다.
		UE_LOG(LogTemp, Warning, TEXT("Invalid Cell Index: %d"), ChunkIndex);
		return;
	}

	// 해당 비트 마스크에서 비트 위치
	const int32 BitOffset = ChunkIndex % 64;

	// 해당 위치의 비트를 1로 설정한 뒤 반전
	// 반전된 결과를 AND 연산
	// 원하는 위치의 비트는 0이 되고 나머지 비트는 기존의 값을 유지
	ChunkBusyBits[BitIndex] &= ~(1ULL << BitOffset);
}

void URealtimeDestructibleMeshComponent::ClearAllChunkBusyBits()
{
	for (auto& BitMask : ChunkBusyBits)
	{
		BitMask = 0ULL;
	}
} 
void URealtimeDestructibleMeshComponent::SetChunkBits(int32 ChunkIndex, int32& BitIndex, int32& BitOffset)
{
	// 64bit 를 사용 중이니, bit indexing 
	BitIndex = ChunkIndex / 64;

	if (!ChunkSubtractBusyBits.IsValidIndex(BitIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("ClearChunkSubtractBusy: Invalid ChunkIndex: %d"), ChunkIndex);
		return;
	}
	BitOffset = ChunkIndex % 64;
}

void URealtimeDestructibleMeshComponent::ApplyBooleanOperationResult(FDynamicMesh3&& NewMesh, const int32 ChunkIndex, bool bDelayedCollisionUpdate)
{
	if (ChunkIndex == INDEX_NONE)
	{
		return;
	}

	UDynamicMeshComponent* TargetComp = GetChunkMeshComponent(ChunkIndex);
	if (!TargetComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("TargetComp is invalid"));
		return;
	}

	TargetComp->EditMesh([&](FDynamicMesh3& InternalMesh)
		{
			InternalMesh = MoveTemp(NewMesh);
		});

	// 수정된 청크 추적
	ModifiedChunkIds.Add(ChunkIndex);
	// 디버그 텍스트 갱신 플래그는 기본적으로 구조적 무결성 갱신 후 업데이트되지만, 청크 없는 경우 여기에서 대신 갱신 
	if (ChunkMeshComponents.Num() == 0)
	{
		bShouldDebugUpdate = true;
	}

	if (bDelayedCollisionUpdate)
	{
		RequestDelayedCollisionUpdate(TargetComp);
	}
	else
	{
		ApplyCollisionUpdate(TargetComp);
	}
}

void URealtimeDestructibleMeshComponent::RequestDelayedCollisionUpdate(UDynamicMeshComponent* TargetComp)
{
	if (!TargetComp)
	{
		return;
	}
	// InRate 이내에 호출되는 경우 타이머 리셋
	if (UWorld* World = GetWorld())
	{
		FTimerDelegate CollsionTimerDelegate;
		CollsionTimerDelegate.BindUObject(this, &URealtimeDestructibleMeshComponent::ApplyCollisionUpdateAsync, TargetComp);
		UE_LOG(LogTemp, Display, TEXT("Set Collision Timer %f"), FPlatformTime::Seconds());
		World->GetTimerManager().SetTimer(
			CollisionUpdateTimerHandle,
			CollsionTimerDelegate,			
			0.05f,
			false);
	}
}

void URealtimeDestructibleMeshComponent::UpdateDebugText()
{
	// 메시 정보 가져오기
	int32 VertexCount = 0;
	int32 TriangleCount = 0;

	if (UDynamicMesh* DynMesh = GetDynamicMesh())
	{
		DynMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				VertexCount = Mesh.VertexCount();
				TriangleCount = Mesh.TriangleCount();
			});
	}

	// BooleanProcessor의 hole count 가져오기 (비동기 처리 시 여기서 관리됨)
	int32 HoleCount = BooleanProcessor.IsValid() ? BooleanProcessor->GetCurrentHoleCount() : CurrentHoleCount;

	// 네트워크 모드 가져오기
	FString NetModeStr = TEXT("Unknown");
	if (UWorld* World = GetWorld())
	{
		switch (World->GetNetMode())
		{
		case NM_Standalone:
			NetModeStr = TEXT("Standalone");
			break;
		case NM_DedicatedServer:
			NetModeStr = TEXT("Dedicated Server");
			break;
		case NM_ListenServer:
			NetModeStr = TEXT("Listen Server");
			break;
		case NM_Client:
			NetModeStr = TEXT("Client");
			break;
		default:
			NetModeStr = TEXT("Unknown");
			break;
		}
	}

	const int32 ChunkCount = ChunkMeshComponents.Num();
	const int32 CellCount = GridCellCache.GetValidCellCount();
	const int32 AnchorCount = GridCellCache.GetAnchorCount();
	const int32 DestroyedCount = CellState.DestroyedCells.Num();

	// 디버그 텍스트 생성
	DebugText = FString::Printf(
		TEXT("Vertices: %d\nTriangles: %d\nHoles: %d / %d\nInitialized: %s\nNetwork Mode: %s\n<Grid Cells>\nChunks: %d | Cells: %d | Anchors: %d | Destroyed: %d"),
		VertexCount,
		TriangleCount,
		HoleCount,
		MaxHoleCount,
		bIsInitialized ? TEXT("Yes") : TEXT("No"),
		*NetModeStr,
		ChunkCount,
		CellCount,
		AnchorCount,
		DestroyedCount
	);

	bShouldDebugUpdate = false;
}

void URealtimeDestructibleMeshComponent::DrawDebugText() const
{
#if !UE_BUILD_SHIPPING
	UWorld* DebugWorld = GetWorld();
	if (!DebugWorld)
	{
		return;
	}

	float BoundsHeight = Bounds.BoxExtent.Z * 2.0f;
	if (ChunkMeshComponents.Num() > 0)
	{
		if (SliceCount.Z > 0 && CachedChunkSize.Z > 0.0f)
		{
			BoundsHeight = CachedChunkSize.Z * SliceCount.Z;
		}
	}

	const float WorldScaleZ = GetComponentTransform().GetScale3D().Z;
	const FVector TextLocation = GetComponentLocation() + FVector(0, 0, BoundsHeight * WorldScaleZ);
	DrawDebugString(DebugWorld, TextLocation, DebugText, nullptr, FColor::Cyan, 0.0f, true, 1.5f);
#endif
}

void URealtimeDestructibleMeshComponent::DrawGridCellDebug()
{
	if (!GridCellCache.IsValid() || !GridCellCache.HasValidSparseData())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& ComponentTransform = GetComponentTransform();

	// 디버그 로그는 첫 프레임만 출력 (스팸 방지)
	static bool bFirstGridDraw = true;
	if (bFirstGridDraw)
	{
		UE_LOG(LogTemp, Log, TEXT("DrawGridCellDebug: Grid %dx%dx%d, Valid cells: %d, Anchors: %d"),
			GridCellCache.GridSize.X, GridCellCache.GridSize.Y, GridCellCache.GridSize.Z,
			GridCellCache.GetValidCellCount(), GridCellCache.GetAnchorCount());
		bFirstGridDraw = false;
	}

	// // 파괴된 셀 개수 확인 로그 (1초마다)
	// static double LastLogTime = 0.0;
	// double CurrentTime = FPlatformTime::Seconds();
	// if (CurrentTime - LastLogTime > 1.0)
	// {
	// 	int32 DestroyedCount = 0;
	// 	int32 DetachedCount = 0;
	// 	for (int32 CellId : GridCellCache.GetValidCellIds())
	// 	{
	// 		if (CellState.DestroyedCells.Contains(CellId)) DestroyedCount++;
	// 		if (CellState.IsCellDetached(CellId)) DetachedCount++;
	// 	}
	// 	UE_LOG(LogTemp, Warning, TEXT("[DrawGridCellDebug] DestroyedCells.Num=%d, ValidCells에서 Destroyed=%d, Detached=%d, bShowDestroyedCells=%d"),
	// 		CellState.DestroyedCells.Num(), DestroyedCount, DetachedCount, bShowDestroyedCells);
	// 	LastLogTime = CurrentTime;
	// }

	// 1. 유효 셀만 그리기 (희소 배열)
	for (int32 CellId : GridCellCache.GetValidCellIds())
	{
		const bool bIsDestroyed = CellState.DestroyedCells.Contains(CellId);
		const bool bIsDetached = CellState.IsCellDetached(CellId);
		const bool bIsRecentlyDestroyed = RecentDirectDestroyedCellIds.Contains(CellId);

		// 파괴된 셀 표시 옵션 확인 (분리 대기 중인 셀도 포함)
		if ((bIsDestroyed || bIsDetached) && !bShowDestroyedCells)
		{
			continue;
		}

		// 셀 색상 결정: 최근 파괴=노랑, 파괴됨=빨강, 분리대기=주황, 앵커=밝은녹색, 일반=시안
		FColor CellColor;
		if (bIsRecentlyDestroyed)
		{
			CellColor = FColor(255, 255, 0);
		}
		else if (bIsDestroyed)
		{
			CellColor = FColor::Red;
		}
		else if (bIsDetached)
		{
			CellColor = FColor::Orange;  // 분리 대기 중 (파편 스폰 예정)
		}
		else if (GridCellCache.GetCellIsAnchor(CellId))
		{
			CellColor = FColor(0, 255, 0);  // 밝은 녹색
		}
		else
		{
			CellColor = FColor::Cyan;
		}

		// 셀 중심점 그리기 (점으로만 표시 - 성능 최적화)
		const FVector LocalCenter = GridCellCache.IdToLocalCenter(CellId);
		const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);

		DrawDebugPoint(World, WorldCenter, 5.0f, CellColor, false, 0.0f, SDPG_Foreground);
	}
}

void URealtimeDestructibleMeshComponent::DrawServerCollisionDebug()
{
	if (!bServerCellCollisionInitialized)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& ComponentTransform = GetComponentTransform();
	const FVector HalfExtent = GridCellSize * 0.5f;

	int32 TotalBoxes = 0;
	TMap<int32, int32> ChunkBoxCounts;  // 청크별 박스 수 기록

	for (int32 ChunkIdx = 0; ChunkIdx < CollisionChunks.Num(); ++ChunkIdx)
	{
		const FCollisionChunkData& Chunk = CollisionChunks[ChunkIdx];

		// 청크 인덱스 기반으로 고유 색상 생성 (더 다양한 색상)
		const uint8 R = static_cast<uint8>((ChunkIdx * 73) % 256);
		const uint8 G = static_cast<uint8>((ChunkIdx * 137 + 50) % 256);
		const uint8 B = static_cast<uint8>((ChunkIdx * 199 + 100) % 256);
		const FColor ChunkColor(R, G, B, 255);

		int32 ChunkBoxCount = 0;

		// Surface Cell만 박스로 그리기
		for (int32 CellId : Chunk.SurfaceCellIds)
		{
			// 파괴된 셀은 스킵
			if (CellState.DestroyedCells.Contains(CellId))
			{
				continue;
			}

			const FVector LocalCenter = GridCellCache.IdToLocalCenter(CellId);
			const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);

			DrawDebugBox(World, WorldCenter, HalfExtent, ComponentTransform.GetRotation(), ChunkColor, false, 0.0f, SDPG_World, 1.0f);
			++TotalBoxes;
			++ChunkBoxCount;
		}

		if (ChunkBoxCount > 0)
		{
			ChunkBoxCounts.Add(ChunkIdx, ChunkBoxCount);
		}
	}

	// 첫 프레임만 로그 출력 (청크별 상세)
	static bool bFirstDraw = true;
	if (bFirstDraw)
	{
		UE_LOG(LogTemp, Log, TEXT("[ServerCollisionDebug] Drawing %d collision boxes from %d chunks"), TotalBoxes, CollisionChunks.Num());

		// 비어있지 않은 청크 개수와 상위 5개 청크 출력
		int32 LogCount = 0;
		for (const auto& Pair : ChunkBoxCounts)
		{
			if (LogCount < 5)
			{
				UE_LOG(LogTemp, Log, TEXT("[ServerCollisionDebug] Chunk %d: %d boxes"), Pair.Key, Pair.Value);
				++LogCount;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("[ServerCollisionDebug] Total %d non-empty chunks"), ChunkBoxCounts.Num());

		bFirstDraw = false;
	}
}

void URealtimeDestructibleMeshComponent::DrawActivePhysicsBoxesDebug()
{
	if (!bServerCellCollisionInitialized)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& ComponentTransform = GetComponentTransform();
	int32 TotalActiveBoxes = 0;
	int32 DisabledChunks = 0;

	for (int32 ChunkIdx = 0; ChunkIdx < CollisionChunks.Num(); ++ChunkIdx)
	{
		const FCollisionChunkData& Chunk = CollisionChunks[ChunkIdx];

		// BodySetup이 없으면 스킵
		if (!Chunk.BodySetup)
		{
			continue;
		}

		// 콜리전 컴포넌트 상태 확인
		UStaticMeshComponent* ChunkComp = Cast<UStaticMeshComponent>(Chunk.ChunkComponent);
		if (!ChunkComp || ChunkComp->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
		{
			++DisabledChunks;
			continue;
		}

		// 청크 인덱스 기반으로 고유 색상 생성
		const uint8 R = static_cast<uint8>((ChunkIdx * 73) % 256);
		const uint8 G = static_cast<uint8>((ChunkIdx * 137 + 50) % 256);
		const uint8 B = static_cast<uint8>((ChunkIdx * 199 + 100) % 256);
		const FColor ChunkColor(R, G, B, 255);

		// 실제 BodySetup의 BoxElems에서 점으로 그리기 (성능 최적화)
		const FKAggregateGeom& DebugAggGeom = Chunk.BodySetup->AggGeom;
		for (const FKBoxElem& BoxElem : DebugAggGeom.BoxElems)
		{
			const FVector WorldCenter = ComponentTransform.TransformPosition(BoxElem.Center);
			DrawDebugPoint(World, WorldCenter, 5.0f, ChunkColor, false, 0.0f, SDPG_World);
			++TotalActiveBoxes;
		}
	}

	// 매 프레임 상태 표시 (화면 왼쪽 상단)
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Yellow,
			FString::Printf(TEXT("[PhysicsBoxes] Active: %d boxes, Disabled chunks: %d, Destroyed cells: %d"),
				TotalActiveBoxes, DisabledChunks, CellState.DestroyedCells.Num()));
	}
}

void URealtimeDestructibleMeshComponent::SetSourceMeshEnabled(bool bEnabled)
{
	SetVisibility(bEnabled, false);
	if (bEnabled)
	{
		SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); 
		
	}
	else
	{
		SetCollisionEnabled(ECollisionEnabled::NoCollision); 
	}
	SetComponentTickEnabled(bEnabled);
	
	// 물리 상태 강제 갱신
	RecreatePhysicsState();
}

void URealtimeDestructibleMeshComponent::RegisterDecalToSubCells(UDecalComponent* Decal, const FRealtimeDestructionRequest& Request)
{
	if (!Decal)
	{
		UE_LOG(LogTemp, Warning, TEXT("RegisterDecalToSubCells : Decal Invalid"));
		return;
	}

	if (!GridCellCache.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("RegisterDecalToSubCells : Grid Cell Invalid"));
		return;
	}

	FManagedDecal NewDecal;
	NewDecal.Decal = TWeakObjectPtr(Decal);

	// 데칼의 로컬 x축으로의 탐색 깊이 설정
	// 셀 사이즈보다 살짝 작게 탐색
	// 셀 사이즈와 동일하거나 
	constexpr float SearchDepthRatio = 0.7f;
	const float MaxCellSize = GridCellSize.GetMax();
	const float TargetDepth = SearchDepthRatio * MaxCellSize;

	// 데칼 크기 보정
	// box extent는 박스의 절반
	FVector EffectiveExtent = Decal->DecalSize;
	EffectiveExtent.X = TargetDepth * 0.5f;

	// 박스의 중심점을 밀어넣어서 [0 ~ Depth]범위 탐색
	FVector SearchCenter = Decal->GetComponentLocation();

	FCellDestructionShape DecalShape;
	DecalShape.Type = EDestructionShapeType::Box;
	DecalShape.Center = SearchCenter;
	DecalShape.BoxExtent = EffectiveExtent;
	DecalShape.Rotation = Decal->GetComponentRotation();

	FQuantizedDestructionInput QuantizedDecal = FQuantizedDestructionInput::FromDestructionShape(DecalShape);

	// 데칼 영역 내의 후보 cell 탐색
	FBox ThinLocalBox(-EffectiveExtent, EffectiveExtent);
	FTransform ThinBoxTransform(Decal->GetComponentQuat(), SearchCenter);
	FBox ThinWorldBox = ThinLocalBox.TransformBy(ThinBoxTransform);
	
	const FTransform& MeshTransform = GetComponentTransform();
	TArray<int32> CandidateCells = GridCellCache.GetCellsInAABB(ThinWorldBox, MeshTransform);
	
	for (int32 CellID : CandidateCells)
	{
		if (CellState.DestroyedCells.Contains(CellID))
		{
			continue;
		}

		FCellOBB CellWorldOBB = GridCellCache.GetCellWorldOBB(CellID, MeshTransform);

		if (QuantizedDecal.IntersectsOBB(CellWorldOBB))
		{
			NewDecal.CoveredCells.Add(CellID);
			}
			}

	if (NewDecal.CoveredCells.Num() > 0)
	{
		ActiveDecals.Add(NewDecal);
	}
}

void URealtimeDestructibleMeshComponent::ProcessDecalRemoval(const FDestructionResult& Result)
{
	if (ActiveDecals.Num() == 0)
	{
		return;
	}

	if (!Result.HasAnyDestruction())
	{
		return;
	}

	for (int32 DecalIndex = ActiveDecals.Num() - 1; DecalIndex >= 0; DecalIndex--)
	{
		FManagedDecal& Decal = ActiveDecals[DecalIndex];

		if (!Decal.Decal.IsValid())
		{
			ActiveDecals.RemoveAtSwap(DecalIndex);
			continue;
		}

		for (int32 CoverageIndex = Decal.CoveredCells.Num() - 1; CoverageIndex >= 0; CoverageIndex--)
		{
			int32 CellID = Decal.CoveredCells[CoverageIndex];

			if (Result.NewlyDestroyedCells.Contains(CellID))
				{
					Decal.CoveredCells.RemoveAtSwap(CoverageIndex);
				}
			}

		if (Decal.CoveredCells.Num() == 0)
		{
			if (UDecalComponent* DecalToRemove = Decal.Decal.Get())
			{
				DecalToRemove->DestroyComponent();
			}
			ActiveDecals.RemoveAtSwap(DecalIndex);
		}
	}
}

void URealtimeDestructibleMeshComponent::OnRegister()
{
	Super::OnRegister();

	if (ChunkMeshComponents.Num() > 0)
	{
		return;  // Cell 모드에서 이미 셀이 있으면 스킵
	}

	if (SourceStaticMesh && !bIsInitialized)
	{
		InitializeFromStaticMeshInternal(SourceStaticMesh, false);
	} 
}

void URealtimeDestructibleMeshComponent::InitializeComponent()
{
	Super::InitializeComponent();
}
void URealtimeDestructibleMeshComponent::BeginPlay()
{
	Super::BeginPlay();


	// 1. 하드웨어 정보
	int32 PhysicalCores = FPlatformMisc::NumberOfCores();
	int32 LogicalCores = FPlatformMisc::NumberOfCoresIncludingHyperthreads();

	// 2. 언리얼 Worker 스레드 수 (엔진이 권장하는 수)
	int32 RecommendedWorkers = FPlatformMisc::NumberOfWorkerThreadsToSpawn();

	// 3. 실제 TaskGraph Worker 스레드 수
	int32 TaskGraphWorkers = FTaskGraphInterface::Get().GetNumWorkerThreads();

	// 4. 언리얼 필수 스레드 (대략적인 수치)
	// - Game Thread: 1
	// - Render Thread: 1 (RHI Thread 포함 시 +1)
	// - Audio Thread: 1
	// - Stats Thread: 1 (WITH_STATS일 때)
	// - 기타 시스템 스레드: 2~3
	const int32 ReservedThreads = 4;  // GT, RT, Audio, 기타

	// 5. 실제 가용 Worker 스레드
	int32 AvailableWorkers = FMath::Max(0, RecommendedWorkers);

	UE_LOG(LogTemp, Warning, TEXT("========== Thread Info =========="));
	UE_LOG(LogTemp, Warning, TEXT("Physical Cores: %d"), PhysicalCores);
	UE_LOG(LogTemp, Warning, TEXT("Logical Cores (with HT): %d"), LogicalCores);
	UE_LOG(LogTemp, Warning, TEXT("Reserved Threads (GT/RT/Audio/etc): ~%d"), ReservedThreads);
	UE_LOG(LogTemp, Warning, TEXT("TaskGraph Workers: %d"), TaskGraphWorkers);
	UE_LOG(LogTemp, Warning, TEXT("Recommended Workers: %d"), RecommendedWorkers);
	UE_LOG(LogTemp, Warning, TEXT("Available for Boolean: %d"), AvailableWorkers);


	UE_LOG(LogTemp, Warning, TEXT("================================="));
	UE_LOG(LogTemp, Display, TEXT("CellMesh Num %d"), ChunkMeshComponents.Num());

	// 멀티플레이어 동기화를 위해 Owner Actor의 Replication 활성화
	if (AActor* Owner = GetOwner())
	{
		if (!Owner->GetIsReplicated())
		{
			Owner->SetReplicates(true);
			Owner->SetReplicateMovement(false);  // 움직임은 복제 안 함 (정적 오브젝트)
			Owner->bAlwaysRelevant = true;       // 모든 클라이언트에 항상 관련됨
			UE_LOG(LogTemp, Warning, TEXT("RealtimeDestructibleMeshComponent: Owner Actor의 Replication 활성화됨"));
		}
	}

	if (SourceStaticMesh && !bIsInitialized)
	{
		InitializeFromStaticMeshInternal(SourceStaticMesh, false);
	}

	

	for (int32 i = 0; i < ChunkMeshComponents.Num(); i++)
	{
		ChunkIndexMap.Add(ChunkMeshComponents[i].Get(), i);
	}

	int32 NumBits = (ChunkMeshComponents.Num() + 63) / 64;
	ChunkBusyBits.Init(0ULL, NumBits);
	ChunkSubtractBusyBits.Init(0ULL, NumBits);

	// 런타임 시작 시 GridCellCache가 유효하지 않으면 구축
	if (SourceStaticMesh && !GridCellCache.IsValid())
	{
		BuildGridCells();
	}
	if (bIsInitialized && !BooleanProcessor.IsValid())
	{
		BooleanProcessor = MakeUnique<FRealtimeBooleanProcessor>();
		if (!BooleanProcessor->Initialize(this))
		{
			UE_LOG(LogTemp, Warning, TEXT("불리언 프로세서 초기화 실패"));
		}
	}

	// 기존 저장 데이터 호환: CellMeshComponents가 있으면 bCellMeshesValid 자동 설정
	if (!bChunkMeshesValid && ChunkMeshComponents.Num() > 1)
	{
		int32 ValidCount = 0;
		for (const auto& Cell : ChunkMeshComponents)
		{
			if (Cell && Cell->IsValidLowLevel()) ValidCount++;
		}
		if (ValidCount > 0)
		{
			bChunkMeshesValid = true;
			UE_LOG(LogTemp, Log, TEXT("BeginPlay: Auto-detected %d valid CellMeshComponents, setting bCellMeshesValid=true"), ValidCount);
		}
	}

	// 런타임 시작 시 GridCell 상태 로그
	UE_LOG(LogTemp, Log, TEXT("BeginPlay: bCellMeshesValid=%d, GridCellCache.IsValid=%d, CellMeshComponents.Num=%d"),
		bChunkMeshesValid, GridCellCache.IsValid(), ChunkMeshComponents.Num());

	/** Culstering 관련 초기화 */
	if (bEnableClustering)
	{
		// Cluster Comp가 없을 경우 생성
		if (!BulletClusterComponent)
		{
			BulletClusterComponent = NewObject<UBulletClusterComponent>(GetOwner()); 
			BulletClusterComponent->RegisterComponent();
		}

		if (BulletClusterComponent)
		{
			BulletClusterComponent->Init(MaxMergeDistance , MaxClusterRadius, MinClusterCount, ClusterRaidusOffset);
			BulletClusterComponent->SetOwnerMesh(this);
		}
	}

	// 서버 Cell Box Collision 초기화 (데디케이티드 서버에서만)
	BuildServerCellCollision();
}

void URealtimeDestructibleMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (PendingDestructionResults.Num() > 0)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			const ENetMode NetMode = World->GetNetMode();
			// Dedicated Server가 아닐 때만 여기서 Phase 2 실행
			if (NetMode != NM_DedicatedServer)
			{
				DisconnectedCellStateLogic(PendingDestructionResults);
			}
		}
		PendingDestructionResults.Empty();
	}
#if !UE_BUILD_SHIPPING
	if (bShowDebugText)
	{
		if (bShouldDebugUpdate)
		{
			UpdateDebugText();
		}
		DrawDebugText();
	}
#endif

	if (BooleanProcessor.IsValid() && GetChunkNum() > 0)
	{
		// 매틱 Subtract Queue를 비워준다.
		//BooleanProcessor->KickProcessIfNeeded();
		BooleanProcessor->KickProcessIfNeededPerChunk();
	}

	// GridCell 디버그 표시
	if (bShowGridCellDebug)
	{
		DrawGridCellDebug();
	}

	// 서버 콜리전 박스 디버그 표시
	if (bShowServerCollisionDebug)
	{
		DrawServerCollisionDebug();
	}

	// 실제 활성화된 물리 박스 디버그 표시 (BodySetup 기반)
	if (bShowActivePhysicsBoxes)
	{
		DrawActivePhysicsBoxesDebug();
	}

	// 서버 Cell Box Collision: Dirty 청크 업데이트 (서버에서만)
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		UpdateDirtyCollisionChunks();
	}

	// 서버 배칭 처리
	if (!bUseServerBatching)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 서버에서만 배칭 처리
	const ENetMode NetMode = World->GetNetMode();
	if (NetMode != NM_DedicatedServer && NetMode != NM_ListenServer)
	{
		return;
	}

	// 대기 중인 요청이 없으면 스킵
	const int32 PendingCount = bUseCompactMulticast ? PendingServerBatchOpsCompact.Num() : PendingServerBatchOps.Num();
	if (PendingCount == 0)
	{
		ServerBatchTimer = 0.0f;
		return;
	}

	// 타이머 업데이트
	ServerBatchTimer += DeltaTime;

	// 배치 간격 도달 시 전송
	if (ServerBatchTimer >= ServerBatchInterval)
	{
		FlushServerBatch();
		ServerBatchTimer = 0.0f;
	}
}

void URealtimeDestructibleMeshComponent::OnUnregister()
{
	Super::OnUnregister();
}

void URealtimeDestructibleMeshComponent::BeginDestroy()
{
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->Shutdown();
		BooleanProcessor.Reset();
	}
	Super::BeginDestroy();
}

void URealtimeDestructibleMeshComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (BooleanProcessor.IsValid())
	{
		BooleanProcessor->Shutdown();
		BooleanProcessor.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

void URealtimeDestructibleMeshComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

void URealtimeDestructibleMeshComponent::EnqueueForServerBatch(const FRealtimeDestructionOp& Op)
{
	if (bUseCompactMulticast)
	{
		// 압축해서 저장
		FCompactDestructionOp CompactOp = FCompactDestructionOp::Compress(Op.Request, ServerBatchSequence++);
		PendingServerBatchOpsCompact.Add(CompactOp);

		// 최대 배치 크기 도달 시 즉시 전송 -> 음 이건 관통,비관통 문제가 있을 수 있으니 수정 해야겠군요 
		if (PendingServerBatchOpsCompact.Num() >= MaxServerBatchSize)
		{
			FlushServerBatch();
		}
	}
	else
	{
		// 비압축 저장
		PendingServerBatchOps.Add(Op);

		// 최대 배치 크기 도달 시 즉시 전송
		if (PendingServerBatchOps.Num() >= MaxServerBatchSize)
		{
			FlushServerBatch();
		}
	}
}

void URealtimeDestructibleMeshComponent::FlushServerBatch()
{
	if (bUseCompactMulticast)
	{
		// 압축 모드
		if (PendingServerBatchOpsCompact.Num() == 0)
		{
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("[ServerBatching] Flushing %d ops (Compact)"), PendingServerBatchOpsCompact.Num());

		// 디버거에 Multicast RPC 기록 (압축, 데이터 크기 포함)
		if (UWorld* World = GetWorld())
		{
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				Debugger->RecordMulticastRPCWithSize(PendingServerBatchOpsCompact.Num(), true);
			}
		}

		// Late Join: Op 히스토리에 기록 (서버에서만)
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			for (const FCompactDestructionOp& CompactOp : PendingServerBatchOpsCompact)
			{
				if (AppliedOpHistory.Num() < MaxOpHistorySize)
				{
					AppliedOpHistory.Add(CompactOp);
				}
			}
		}

		// 데디서버: Multicast는 자기 자신에게 실행 안 됨, GridCell 판정만 직접 처리
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() == NM_DedicatedServer)
		{
			// ===== 로그 추가 ===== 
			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH START] Ops=%d ##########"), PendingServerBatchOpsCompact.Num());

			UE_LOG(LogTemp, Warning, TEXT("[ServerBatching] DedicatedServer: GridCell processing %d ops"), PendingServerBatchOpsCompact.Num());

			TArray<FDestructionResult> AllResults;
			AllResults.Reserve(PendingServerBatchOpsCompact.Num()); 
			for (const FCompactDestructionOp& CompactOp : PendingServerBatchOpsCompact)
			{
				FRealtimeDestructionRequest Request = CompactOp.Decompress();
				FDestructionResult Result = DestructionLogic(Request);
				AllResults.Add(Result);
			} 

			DisconnectedCellStateLogic(AllResults);

			// ===== 로그 추가 =====
			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH END] ##########")); 
		}

		// 압축된 데이터로 전파
		MulticastApplyOpsCompact(PendingServerBatchOpsCompact);

		// 대기열 비우기
		PendingServerBatchOpsCompact.Empty();
	}
	else
	{
		// 비압축 모드
		if (PendingServerBatchOps.Num() == 0)
		{
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("[ServerBatching] Flushing %d ops"), PendingServerBatchOps.Num());

		// 디버거에 Multicast RPC 기록 (비압축, 데이터 크기 포함)
		if (UWorld* World = GetWorld())
		{
			if (UDestructionDebugger* Debugger = World->GetSubsystem<UDestructionDebugger>())
			{
				Debugger->RecordMulticastRPCWithSize(PendingServerBatchOps.Num(), false);
			}
		}

		// Late Join: Op 히스토리에 기록 (서버에서만)
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			for (const FRealtimeDestructionOp& Op : PendingServerBatchOps)
			{
				if (AppliedOpHistory.Num() < MaxOpHistorySize)
				{
					AppliedOpHistory.Add(FCompactDestructionOp::Compress(Op.Request, Op.Sequence));
				}
			}
		}

		// 데디서버: Multicast는 자기 자신에게 실행 안 됨, GridCell 판정만 직접 처리
		UWorld* WorldNonCompact = GetWorld();
		if (WorldNonCompact && WorldNonCompact->GetNetMode() == NM_DedicatedServer)
		{ 
			UE_LOG(LogTemp, Warning, TEXT(""));
			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH START] Ops=%d (non-compact) ##########"), PendingServerBatchOpsCompact.Num());

			UE_LOG(LogTemp, Warning, TEXT("[ServerBatching] DedicatedServer: GridCell processing %d ops (non-compact)"), PendingServerBatchOps.Num());

			TArray<FDestructionResult> AllResults;
			AllResults.Reserve(PendingServerBatchOps.Num());
			for (const FRealtimeDestructionOp& Op : PendingServerBatchOps)
			{
				FDestructionResult Result = DestructionLogic(Op.Request);
				AllResults.Add(Result);	UpdateCellStateFromDestruction(Op.Request);
			}

			DisconnectedCellStateLogic(AllResults);

			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH END] ##########"));
			UE_LOG(LogTemp, Warning, TEXT("")); 
		}

		// 비압축 데이터로 전파
		MulticastApplyOps(PendingServerBatchOps);

		// 대기열 비우기
		PendingServerBatchOps.Empty();
	}
}

UDecalComponent* URealtimeDestructibleMeshComponent::SpawnTemporaryDecal(const FRealtimeDestructionRequest& Request)
{
	if (!Request.bSpawnDecal)
	{
		return nullptr;
	}
	
	UMaterialInterface* MaterialToUse = nullptr;
	FVector SizeToUse = FVector::ZeroVector;
	FVector LocationOffsetToUse = FVector::ZeroVector;
	FRotator RotationOffsetToUse = FRotator::ZeroRotator; 

	if (Request.DecalMaterial)
	{
		MaterialToUse = Request.DecalMaterial; 
		SizeToUse = Request.DecalSize;
		LocationOffsetToUse = Request.DecalLocationOffset;
		RotationOffsetToUse = Request.DecalRotationOffset;
	}  
	
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	UDecalComponent* Decal = NewObject<UDecalComponent>(Owner);
	if (!Decal)
	{
		return nullptr;
	}

	//Decal->SetDecalMaterial(HoleDecal);
	Decal->SetDecalMaterial(MaterialToUse);
	
	//Request에 decalSize가 없을 때, 기본값 사용
	//Decal->DecalSize = Request.DecalSize.IsNearlyZero() ? DecalSize : Request.DecalSize;
	Decal->DecalSize = Request.DecalSize.IsNearlyZero() ? SizeToUse : Request.DecalSize;
	
	// Sphere타입은 폭발 중심과의 거리에 반비례하여 데칼 크기 조절
	// (projectile 도 동일하게 처리해도 무방)
	if (Request.ToolShape == EDestructionToolShape::Sphere)
	{
		const float Distance = FVector::Dist(Request.ToolCenterWorld, Request.ImpactPoint);
		const float MaxRadius = Request.ShapeParams.Radius;

		if (MaxRadius > KINDA_SMALL_NUMBER)
		{

			if (Distance < MaxRadius)
			{  
				const float Ratio = Distance / MaxRadius;
				const float SphericalScale = FMath::Sqrt(1.0f - (Ratio * Ratio));

				if (SphericalScale <= 0.1f)
				{
					return nullptr;
				}

				FVector OriginalSize = Decal->DecalSize;
				Decal->DecalSize = FVector(
					OriginalSize.X,                      // 깊이는 유지
					OriginalSize.Y * SphericalScale,     // 너비 스케일
					OriginalSize.Z * SphericalScale      // 높이 스케일
				);
			}
			else
			{
				return nullptr;
			}
		}
	}

	//데칼이 항상 보이도록 처리 
	Decal->SetFadeScreenSize(0.0f);
	Decal->FadeStartDelay = 0.0f;
	Decal->FadeDuration = 0.0f; 
	 
	  
	// decal 방향 설정 
	FRotator DecalRotation = Request.ImpactNormal.Rotation() + RotationOffsetToUse;
	 
	if (Request.bRandomRotation)
	{
		float RandomRoll = FMath::RandRange(0.0f, 360.0f);
		DecalRotation.Roll+= RandomRoll;
	}
	FRotator TransformBasis = DecalRotation;
	TransformBasis.Yaw += 180.0f;  // 에디터 좌표계와 일치시킴

	FTransform DecalTransform(TransformBasis, Request.ImpactPoint);
	FVector WorldOffset = DecalTransform.TransformVector(LocationOffsetToUse);
	FVector DecalLocation = Request.ImpactPoint + (Request.ImpactNormal * 0.5f) + WorldOffset;
		

	Decal->SetWorldLocationAndRotation(DecalLocation, DecalRotation); 

	
	Decal->RegisterComponent();

	RegisterDecalToSubCells(Decal, Request);

	return Decal;
}

//////////////////////////////////////////////////////////////////////////
// Cell Mesh Parallel Processing
//////////////////////////////////////////////////////////////////////////

int32 URealtimeDestructibleMeshComponent::BuildChunkMeshesFromGeometryCollection()
{
	if (!FracturedGeometryCollection)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: FracturedGeometryCollection is not set."));
		return 0;
	}

	// 기존에 있던 Old DynamicMeshComponent 정리
	for (UDynamicMeshComponent* OldComp : ChunkMeshComponents)
	{
		if (OldComp)
		{
			OldComp->DestroyComponent();
		}
	}
	ChunkMeshComponents.Empty();

	// GeometryCollection 데이터 가져오기
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GeometryCollectionPtr = FracturedGeometryCollection->GetGeometryCollection();

	if (!GeometryCollectionPtr.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: Invalid GeometryCollection data."));
		return 0;
	}

	const FGeometryCollection& GC = *GeometryCollectionPtr;

	// 만들어진 조각이 없다면 return;
	const int32 NumTransforms = GC.NumElements(FGeometryCollection::TransformGroup);
	if (NumTransforms == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildCellMeshesFromGeometryCollection: No transforms in GeometryCollection."));
		return 0;
	}

	// Geometry Group에서 실제 메시 데이터 가져오기
	const TManagedArray<FVector3f>& Vertices = GC.Vertex;
	const TManagedArray<int32>& BoneMap = GC.BoneMap;
	const TManagedArray<FIntVector>& Indices = GC.Indices;
	const TManagedArray<FVector3f>* Normals = GC.FindAttribute<FVector3f>("Normal", FGeometryCollection::VerticesGroup);

	// 디버깅: GeometryCollection의 모든 속성 이름 출력
	UE_LOG(LogTemp, Log, TEXT("=== GeometryCollection Attributes ==="));
	for (const FName& GroupName : GC.GroupNames())
	{
		UE_LOG(LogTemp, Log, TEXT("Group: %s"), *GroupName.ToString());
		for (const FName& AttrName : GC.AttributeNames(GroupName))
		{
			UE_LOG(LogTemp, Log, TEXT("  - %s"), *AttrName.ToString());
		}
	}
	UE_LOG(LogTemp, Log, TEXT("====================================="));

	// UV 찾기 - UVLayer0 속성 사용
	const TManagedArray<FVector2f>* UVsArray = GC.FindAttribute<FVector2f>("UVLayer0", FGeometryCollection::VerticesGroup);

	if (UVsArray && UVsArray->Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGC: Found UVLayer0 with %d elements"), UVsArray->Num());
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGC: Found UVLayer0 with %d elements"), UVsArray->Num());
	}

	// MaterialID 가져오기 (FacesGroup에 저장됨)
	const TManagedArray<int32>* MaterialIDs = GC.FindAttribute<int32>("MaterialID", FGeometryCollection::FacesGroup);

	//=========================================================================
	// 1패스: 버텍스를 조각별로 분류 O(M)
	//=========================================================================
	TArray<TArray<int32>> VertexIndicesByTransform;
	VertexIndicesByTransform.SetNum(NumTransforms);


	struct FTriangleData
	{
		FIntVector Indices;
		int32 MaterialID;
	};

	TArray<TArray<FTriangleData>> TrianglesByTransform;
	TrianglesByTransform.SetNum(NumTransforms);

	// Vertex 분류
	for (int32 VertexIdx = 0; VertexIdx < Vertices.Num(); ++VertexIdx)
	{
		int32 TransformIdx = BoneMap[VertexIdx];
		if (TransformIdx >= 0 && TransformIdx < NumTransforms)
		{
			VertexIndicesByTransform[TransformIdx].Add(VertexIdx);
		}
	}
	// Triangle 분류
	for (int32 TriIdx = 0; TriIdx < Indices.Num(); ++TriIdx)
	{
		const FIntVector& Tri = Indices[TriIdx];
		int32 TransformIdx = BoneMap[Tri.X];

		if (TransformIdx >= 0 && TransformIdx < NumTransforms)
		{
			FTriangleData TriData;
			TriData.Indices = Tri;
			TriData.MaterialID = (MaterialIDs && TriIdx < MaterialIDs->Num()) ? (*MaterialIDs)[TriIdx] : 0;
			TrianglesByTransform[TransformIdx].Add(TriData);
		}
	}

	//=========================================================================
	// 각 Transform별로 DynamicMeshComponent생성
	//=========================================================================

	ChunkMeshComponents.Reserve(NumTransforms);
	int32 ExtractedCount = 0;

	for (int32 TransformIdx = 0; TransformIdx < NumTransforms; ++TransformIdx)
	{
		const TArray<int32>& MyVertexIndices = VertexIndicesByTransform[TransformIdx];
		const TArray<FTriangleData>& MyTriangles = TrianglesByTransform[TransformIdx];

		// 빈 조각 + 첫 번째 스킵
		if (TransformIdx == 0 || MyVertexIndices.Num() == 0 || MyTriangles.Num() == 0)
		{
			ChunkMeshComponents.Add(nullptr);
			continue;
		}

		// DynamicMeshComponent 생성 ( RF_Transactional : Ctrl Z 지원) )
		UDynamicMeshComponent* CellComp = NewObject<UDynamicMeshComponent>(
			GetOwner(),
			UDynamicMeshComponent::StaticClass(),
			*FString::Printf(TEXT("Cell_%d"), TransformIdx),
			RF_Transactional
		);

		if (!CellComp)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to create CellMeshComponent %d"), TransformIdx);
			ChunkMeshComponents.Add(nullptr);
			continue;
		}


		// Compoment기본 설정
		//CellComp->SetupAttachment(this);
		if (AActor* Owner = GetOwner())
		{
			CellComp->SetupAttachment(Owner->GetRootComponent());
		}
		//CellComp->SetRelativeTransform(FTransform::Identity);

		// Collision 설정
		CellComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CellComp->SetCollisionProfileName(TEXT("BlockAll"));
		CellComp->SetComplexAsSimpleCollisionEnabled(true);


		// Tick 활성화   
		CellComp->PrimaryComponentTick.bCanEverTick = false;

		// Global - > Local Index 맵핑
		TMap<int32, int32> GlobalToLocalVertex;
		GlobalToLocalVertex.Reserve(MyVertexIndices.Num());

		TArray<FVector3f> LocalVertices;
		TArray<FVector3f> LocalNormals;
		TArray<FVector2f> LocalUVs;

		if (Normals)
		{
			LocalNormals.Reserve(MyVertexIndices.Num());
		}
		if (UVsArray)
		{
			LocalUVs.Reserve(MyVertexIndices.Num());
		}

		for (int32 GlobalIdx : MyVertexIndices)
		{
			int LocalIdx = LocalVertices.Num();
			GlobalToLocalVertex.Add(GlobalIdx, LocalIdx);

			LocalVertices.Add(Vertices[GlobalIdx]);
			if (Normals) LocalNormals.Add((*Normals)[GlobalIdx]);
			if (UVsArray) LocalUVs.Add((*UVsArray)[GlobalIdx]);
		}

		// Triangle 인덱스 로컬로 변환
		struct FLocalTriangleData
		{
			FIntVector Indices;
			int32 MaterialID;
		};

		TArray<FLocalTriangleData> LocalTriangles;
		LocalTriangles.Reserve(MyTriangles.Num());

		for (const FTriangleData& TriData : MyTriangles)
		{
			const FIntVector& Tri = TriData.Indices;
			if (GlobalToLocalVertex.Contains(Tri.X) &&
				GlobalToLocalVertex.Contains(Tri.Y) &&
				GlobalToLocalVertex.Contains(Tri.Z))
			{
				FLocalTriangleData LocalTriData;
				LocalTriData.Indices = FIntVector(
					GlobalToLocalVertex[Tri.X],
					GlobalToLocalVertex[Tri.Y],
					GlobalToLocalVertex[Tri.Z]
				);
				LocalTriData.MaterialID = TriData.MaterialID;
				LocalTriangles.Add(LocalTriData);
			}
		}

		if (LocalTriangles.Num() == 0)
		{
			CellComp->DestroyComponent();  // 이미 등록한 컴포넌트 정리
			ChunkMeshComponents.Add(nullptr);
			continue;
		}

		// DynamicMesh3 생성
		//TSharedPtr<UE::Geometry::FDynamicMesh3> NewMesh = MakeShared<UE::Geometry::FDynamicMesh3>();


		// 내부 메시 가져오기
		FDynamicMesh3* NewMesh = CellComp->GetMesh();

		// 속성 활성화
		NewMesh->EnableTriangleGroups();
		NewMesh->EnableAttributes();
		NewMesh->Attributes()->EnablePrimaryColors();
		NewMesh->Attributes()->EnableMaterialID();

		UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = NewMesh->Attributes()->PrimaryUV();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = NewMesh->Attributes()->PrimaryNormals();
		UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDAttr = NewMesh->Attributes()->GetMaterialID();

		// 버텍스 추가
		TArray<int32> VertexIDs;
		VertexIDs.Reserve(LocalVertices.Num());

		for (const FVector3f& V : LocalVertices)
		{
			int32 Vid = NewMesh->AppendVertex(FVector3d(V.X, V.Y, V.Z));
			VertexIDs.Add(Vid);
		}

		// 삼각형 추가
		for (const FLocalTriangleData& TriData : LocalTriangles)
		{
			const FIntVector& Tri = TriData.Indices;
			int32 TriId = NewMesh->AppendTriangle(VertexIDs[Tri.X], VertexIDs[Tri.Y], VertexIDs[Tri.Z]);

			if (TriId >= 0)
			{
				// MaterialID 설정
				if (MaterialIDAttr)
				{
					MaterialIDAttr->SetValue(TriId, TriData.MaterialID);
				}

				// UV 설정
				if (UVOverlay && LocalUVs.Num() > 0)
				{
					int32 UV0 = UVOverlay->AppendElement(FVector2f(LocalUVs[Tri.X]));
					int32 UV1 = UVOverlay->AppendElement(FVector2f(LocalUVs[Tri.Y]));
					int32 UV2 = UVOverlay->AppendElement(FVector2f(LocalUVs[Tri.Z]));
					UVOverlay->SetTriangle(TriId, UE::Geometry::FIndex3i(UV0, UV1, UV2));
				}

				// Normal 설정
				if (NormalOverlay && LocalNormals.Num() > 0)
				{
					int32 N0 = NormalOverlay->AppendElement(FVector3f(LocalNormals[Tri.X]));
					int32 N1 = NormalOverlay->AppendElement(FVector3f(LocalNormals[Tri.Y]));
					int32 N2 = NormalOverlay->AppendElement(FVector3f(LocalNormals[Tri.Z]));
					NormalOverlay->SetTriangle(TriId, UE::Geometry::FIndex3i(N0, N1, N2));
				}
			}
		}


		//=========================================================================
		// 동일 위치 엣지 병합 (UV seam 등으로 인한 정점 분리 해결)
		// - 연결성 분석(GridCell)이 정확하게 동작하도록 토폴로지 정리
		//=========================================================================
		{
			UE::Geometry::FMergeCoincidentMeshEdges MergeOp(NewMesh);
			MergeOp.MergeSearchTolerance = 0.001;  // 매우 가까운 정점만 병합 (0.001cm = 0.01mm)
			MergeOp.OnlyUniquePairs = false;       // 모든 coincident 엣지 병합
			if (MergeOp.Apply())
			{
				UE_LOG(LogTemp, Log, TEXT("Cell_%d: Merged coincident edges"), TransformIdx);
			}
		}

		// [핵심 해결책 1] 이 컴포넌트는 에디터 레벨 인스턴스의 일부라고 명시합니다.
		// 이 설정이 없으면 Actor를 움직일 때마다 컴포넌트가 초기화되거나 연결이 끊깁니다.
		CellComp->CreationMethod = EComponentCreationMethod::Instance;

		// [핵심 해결책 2] 부착 대상을 명확히 합니다.
		// Actor의 Root보다는, 현재 부서지고 있는 '이 컴포넌트(this)'에 붙이는 것이 
		// 계층 구조상 더 안전하며, 부모의 Transform을 그대로 상속받기 좋습니다.
		CellComp->SetupAttachment(this);
		// 만약 이미 Register된 상태라면 SetupAttachment 대신 아래를 사용하세요:
		// CellComp->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);

		// 위치 초기화 (부모 기준 0,0,0 = 부모와 겹침)
		CellComp->SetRelativeTransform(FTransform::Identity);


		// 등록 
		CellComp->RegisterComponent();

		// 메시 변경 알림
		CellComp->NotifyMeshUpdated();
		// CellMeshComponent에 머티리얼 설정 (GC에서 복사)
		const TArray<UMaterialInterface*>& GCMaterials = FracturedGeometryCollection->Materials;
		if (GCMaterials.Num() > 0)
		{

			// ConfigureMaterialSet으로 메테리얼 배열 설정 (다중 메테리얼 지원)
			CellComp->ConfigureMaterialSet(GCMaterials);
		}
#if WITH_EDITOR
		// 에디터에게 이 컴포넌트 관리를 위임
		if (AActor* Owner = GetOwner())
		{
			Owner->AddInstanceComponent(CellComp);
		}

#endif
		ChunkMeshComponents.Add(CellComp);
		++ExtractedCount;
	}

	// GeometryCollection에서 머티리얼 복사
	const TArray<UMaterialInterface*>& GCMaterials = FracturedGeometryCollection->Materials;
	if (GCMaterials.Num() > 0)
	{
		// OverrideMaterials 배열 크기 조정
		if (OverrideMaterials.Num() < GCMaterials.Num())
		{
			OverrideMaterials.SetNum(GCMaterials.Num());
		}

		for (int32 MatIdx = 0; MatIdx < GCMaterials.Num(); ++MatIdx)
		{
			if (GCMaterials[MatIdx])
			{
				OverrideMaterials[MatIdx] = GCMaterials[MatIdx];
			}
		}

		// 렌더 업데이트
		MarkRenderStateDirty();

		UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGeometryCollection: Copied %d materials from GeometryCollection"), GCMaterials.Num());
	}

	bChunkMeshesValid = ExtractedCount > 0;

	UE_LOG(LogTemp, Log, TEXT("BuildCellMeshesFromGeometryCollection: Extracted %d meshes from %d transforms"),
		ExtractedCount, NumTransforms);

	if (bChunkMeshesValid)
	{
		if (UDynamicMesh* ParentMesh = GetDynamicMesh())
		{
			ParentMesh->EditMesh([](FDynamicMesh3& Mesh) {
				Mesh.Clear();
				});
		}
		//SetVisibility(false, false);
		SetSourceMeshEnabled(false);

		NotifyMeshUpdated();
		MarkRenderStateDirty();

		// GridToChunkMap 구축 (그리드 인덱스 -> ChunkId 매핑)
		BuildGridToChunkMap();

		// GridCellCache 초기화
		BuildGridCells();

#if WITH_EDITOR
		// 에디터 뷰포트 및 Details 패널 갱신
		if (AActor* Owner = GetOwner())
		{
			Owner->Modify();

			// 에디터 뷰포트 갱신 강제
			if (GEditor)
			{
				GEditor->RedrawLevelEditingViewports(true);
			}
		}

#endif
	}
	return ExtractedCount;
}

void URealtimeDestructibleMeshComponent::BuildGridToChunkMap()
{
	GridToChunkMap.Reset();

	if (SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		return;
	}

	const int32 ExpectedChunkCount = SliceCount.X * SliceCount.Y * SliceCount.Z;
	GridToChunkMap.Init(INDEX_NONE, ExpectedChunkCount);

	// 메시 바운드 계산
	FBox MeshBounds(ForceInit);
	if (SourceStaticMesh)
	{
		MeshBounds = SourceStaticMesh->GetBoundingBox();
	}
	else
	{
		for (const UDynamicMeshComponent* CellComp : ChunkMeshComponents)
		{
			if (CellComp)
			{
				MeshBounds += CellComp->Bounds.GetBox();
			}
		}
	}
	const FVector BoundsSize = MeshBounds.GetSize();
	const FVector CellSize(
		BoundsSize.X / SliceCount.X,
		BoundsSize.Y / SliceCount.Y,
		BoundsSize.Z / SliceCount.Z);

	CachedMeshBounds = MeshBounds;
	CachedChunkSize = CellSize;
	if (!MeshBounds.IsValid)
	{
		return;
	} 

	UE_LOG(LogTemp, Log, TEXT("BuildGridToChunkMap: MeshBounds Min=(%.2f, %.2f, %.2f) Max=(%.2f, %.2f, %.2f)"),
		MeshBounds.Min.X, MeshBounds.Min.Y, MeshBounds.Min.Z,
		MeshBounds.Max.X, MeshBounds.Max.Y, MeshBounds.Max.Z);
	UE_LOG(LogTemp, Log, TEXT("BuildGridToChunkMap: CellSize=(%.2f, %.2f, %.2f), SliceCount=(%d, %d, %d)"),
		CellSize.X, CellSize.Y, CellSize.Z, SliceCount.X, SliceCount.Y, SliceCount.Z);

	// CellMeshComponents[0]은 루트 본(nullptr)이므로 인덱스 1부터 시작
	for (int32 ChunkId = 1; ChunkId < ChunkMeshComponents.Num(); ++ChunkId)
	{
		if (!ChunkMeshComponents[ChunkId])
		{
			continue;
		}

		// 프래그먼트의 월드 바운드 중심을 부모 컴포넌트의 로컬 좌표계로 변환
		const FBox WorldChunkBounds = ChunkMeshComponents[ChunkId]->Bounds.GetBox();
		const FVector WorldCenter = WorldChunkBounds.GetCenter();
		const FVector Center = GetComponentTransform().InverseTransformPosition(WorldCenter);

		// 그리드 좌표 계산 (클램프로 경계 처리)
		const int32 GridX = FMath::Clamp(
			static_cast<int32>((Center.X - MeshBounds.Min.X) / CellSize.X),
			0, SliceCount.X - 1);
		const int32 GridY = FMath::Clamp(
			static_cast<int32>((Center.Y - MeshBounds.Min.Y) / CellSize.Y),
			0, SliceCount.Y - 1);
		const int32 GridZ = FMath::Clamp(
			static_cast<int32>((Center.Z - MeshBounds.Min.Z) / CellSize.Z),
			0, SliceCount.Z - 1);

		const int32 GridIndex = GridX + GridY * SliceCount.X + GridZ * SliceCount.X * SliceCount.Y;

		if (GridIndex >= 0 && GridIndex < ExpectedChunkCount)
		{
			if (GridToChunkMap[GridIndex] != INDEX_NONE)
			{
				UE_LOG(LogTemp, Warning, TEXT("    GridIndex %d already occupied by ChunkId %d, overwriting with %d"),
					GridIndex, GridToChunkMap[GridIndex], ChunkId);
			}
			GridToChunkMap[GridIndex] = ChunkId;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BuildGridToChunkMap: Built map for %d grid cells"), ExpectedChunkCount);
}

bool URealtimeDestructibleMeshComponent::BuildGridCells()
{
	// 1. SourceStaticMesh 확인
	if (!SourceStaticMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildGridCells: SourceStaticMesh is null"));
		return false;
	}

	// 2. 컴포넌트 스케일 가져오기
	const FVector WorldScale = GetComponentScale();

	// 3. GridCellBuilder를 사용하여 캐시 생성
	// - GridCellSize: 월드 좌표계 기준 (사용자 설정값)
	// - WorldScale: 컴포넌트 스케일 (빌더 내부에서 로컬 변환에 사용)
	// - FloorHeightThreshold: 앵커 판정용 (빌더 내부가 로컬 스페이스이므로 변환 필요)
	GridCellCache.Reset();
	CellState.Reset();

	const float LocalFloorThreshold = FloorHeightThreshold / FMath::Max(WorldScale.Z, KINDA_SMALL_NUMBER);

	const bool bSuccess = FGridCellBuilder::BuildFromStaticMesh(
		SourceStaticMesh,
		WorldScale,           // MeshScale (새 파라미터)
		GridCellSize,         // 월드 스페이스 셀 크기 (빌더가 내부에서 로컬로 변환)
		LocalFloorThreshold,  // 앵커 높이 (빌더 내부가 로컬 스페이스이므로 변환 필요)
		GridCellCache
	);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildGridCells: Failed to build grid cells"));
		return false;
	}

	// 4. 캐시된 정보 저장
	// CachedMeshBounds = SourceStaticMesh->GetBoundingBox();
	CachedCellSize = GridCellCache.CellSize;  // 빌더가 저장한 로컬 스페이스 셀 크기

	UE_LOG(LogTemp, Log, TEXT("BuildGridCells: WorldCellSize=(%.1f, %.1f, %.1f), Scale=(%.2f, %.2f, %.2f), LocalCellSize=(%.2f, %.2f, %.2f), Grid %dx%dx%d, Valid cells: %d, Anchors: %d"),
		GridCellSize.X, GridCellSize.Y, GridCellSize.Z,
		WorldScale.X, WorldScale.Y, WorldScale.Z,
		GridCellCache.CellSize.X, GridCellCache.CellSize.Y, GridCellCache.CellSize.Z,
		GridCellCache.GridSize.X, GridCellCache.GridSize.Y, GridCellCache.GridSize.Z,
		GridCellCache.GetValidCellCount(),
		GridCellCache.GetAnchorCount());

	// 5. SuperCell 캐시 빌드 (BFS 최적화용)
	SupercellCache.BuildFromGridCache(GridCellCache);

	return true;
}

void URealtimeDestructibleMeshComponent::EditorBuildGridCells()
{
	if (BuildGridCells())
	{
		UE_LOG(LogTemp, Warning, TEXT("EditorBuildGridCells: SUCCESS - Grid cells built. Save the level to persist!"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("EditorBuildGridCells: FAILED - Check if SourceStaticMesh is set"));
	}
}

void URealtimeDestructibleMeshComponent::FindChunksAlongLineInternal(const FVector& WorldStart, const FVector& WorldEnd, TArray<int32>& OutChunkIndices)
{
	if (GridToChunkMap.Num() == 0 || SliceCount.X <= 0 || SliceCount.Y <= 0 || SliceCount.Z <= 0)
	{
		return;
	}

	const FVector& ChunkSize = CachedChunkSize;
	const FBox& MeshBounds = CachedMeshBounds;

	// World to Local
	FVector LocalStart = GetComponentTransform().InverseTransformPosition(WorldStart);
	FVector LocalEnd = GetComponentTransform().InverseTransformPosition(WorldEnd);

	/*
	 * Slab method로 라인이 메시 내부에 있는 지 검사할 필요가 없음
	 * 라인의 시작점은 항상 메시 표면/내부이고 끝점의 경우 clamp로 처리
	 */

	// 그리드 공간으로 변환
	auto ToGridSpace = [&](const FVector& Position) -> FVector
	{
		return FVector(
			(Position.X - MeshBounds.Min.X) / ChunkSize.X,
			(Position.Y - MeshBounds.Min.Y) / ChunkSize.Y,
			(Position.Z - MeshBounds.Min.Z) / ChunkSize.Z
			);
	};
	FVector GridStart = ToGridSpace(LocalStart);
	FVector GridEnd = ToGridSpace(LocalEnd);

	// 인덱스 변환 및 클램핑
	// End를 박스 내부로 제한
	int32 CurrentX = FMath::Clamp(FMath::FloorToInt(GridStart.X), 0, SliceCount.X - 1);
	int32 CurrentY = FMath::Clamp(FMath::FloorToInt(GridStart.Y), 0, SliceCount.Y - 1);
	int32 CurrentZ = FMath::Clamp(FMath::FloorToInt(GridStart.Z), 0, SliceCount.Z - 1);

	int32 EndX = FMath::Clamp(FMath::FloorToInt(GridEnd.X), 0, SliceCount.X - 1);
	int32 EndY = FMath::Clamp(FMath::FloorToInt(GridEnd.Y), 0, SliceCount.Y - 1);
	int32 EndZ = FMath::Clamp(FMath::FloorToInt(GridEnd.Z), 0, SliceCount.Z - 1);

	// DDA 초기화(amanatides & woo의 fast voxel traversal algorithm)
	int32 StepX = (GridEnd.X >= GridStart.X) ? 1 : -1;
	int32 StepY = (GridEnd.Y >= GridStart.Y) ? 1 : -1;
	int32 StepZ = (GridEnd.Z >= GridStart.Z) ? 1 : -1;

	// tDelta
	FVector Direction = GridEnd - GridStart;
	float tDeltaX = (FMath::Abs(Direction.X) > KINDA_SMALL_NUMBER) ? (1.0f / FMath::Abs(Direction.X)) : FLT_MAX;
	float tDeltaY = (FMath::Abs(Direction.Y) > KINDA_SMALL_NUMBER) ? (1.0f / FMath::Abs(Direction.Y)) : FLT_MAX;
	float tDeltaZ = (FMath::Abs(Direction.Z) > KINDA_SMALL_NUMBER) ? (1.0f / FMath::Abs(Direction.Z)) : FLT_MAX;

	// tMax
	float FracX = GridStart.X - FMath::FloorToFloat(GridStart.X);
	float FracY = GridStart.Y - FMath::FloorToFloat(GridStart.Y);
	float FracZ = GridStart.Z - FMath::FloorToFloat(GridStart.Z);

	float tMaxX = (StepX > 0) ? (1.0f - FracX) * tDeltaX : FracX * tDeltaX;
	float tMaxY = (StepY > 0) ? (1.0f - FracY) * tDeltaY : FracY * tDeltaY;
	float tMaxZ = (StepZ > 0) ? (1.0f - FracZ) * tDeltaZ : FracZ * tDeltaZ;

	int32 MaxIteration = SliceCount.X + SliceCount.Y + SliceCount.Z;

	// 그리드 순회
	for (int32 i = 0; i < MaxIteration; i++)
	{
		if (CurrentX >= 0 && CurrentX < SliceCount.X &&
			CurrentY >= 0 && CurrentY < SliceCount.Y &&
			CurrentZ >= 0 && CurrentZ < SliceCount.Z)
		{
			int32 GridIndex = CurrentX + CurrentY * SliceCount.X + CurrentZ * SliceCount.X * SliceCount.Y;
			if (GridToChunkMap.IsValidIndex(GridIndex))
			{
				int32 ChunkIndex = GridToChunkMap[GridIndex];
				if (ChunkIndex != INDEX_NONE)
				{
					OutChunkIndices.Add(ChunkIndex);
				}
			}
		}

		// 목표지점 도달 시 종료
		if (CurrentX == EndX && CurrentY == EndY && CurrentZ == EndZ)
		{
			break;
		}

		// tMax가 가장 작은 축으로 한 칸 이동(먼저 부딪히는 벽쪽으로 이동)
		if (tMaxX < tMaxY)
		{
			if (tMaxX < tMaxZ)
			{
				CurrentX += StepX;
				tMaxX += tDeltaX;
			}
			else
			{
				CurrentZ += StepZ;
				tMaxZ += tDeltaZ;
			}
		}
		else
		{
			if (tMaxY < tMaxZ)
			{
				CurrentY += StepY;
				tMaxY += tDeltaY;
			}
			else
			{
				CurrentZ += StepZ;
				tMaxZ += tDeltaZ;
			}
		}
	}
}

#if WITH_EDITOR
void URealtimeDestructibleMeshComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName = (PropertyChangedEvent.Property != nullptr)
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;

	// FracturedGeometryCollection 또는 bUseCellMeshes가 변경되면 자동 빌드
	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, FracturedGeometryCollection))
	{
		if (FracturedGeometryCollection)
		{
			int32 CellCount = BuildChunkMeshesFromGeometryCollection();
			UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty: Auto-built %d cell meshes"), CellCount);
		}
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, SourceStaticMesh))
	{
		UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty Mesh Name: %s"), *SourceStaticMesh.GetName());

		// 기존 CellMeshComponents 정리
		for (UDynamicMeshComponent* Comp : ChunkMeshComponents)
		{
			if (Comp)
			{
				Comp->DestroyComponent();
			}
		}
		ChunkMeshComponents.Empty();
		GridToChunkMap.Reset();
		bChunkMeshesValid = false;

		// 새 메시로 초기화
		bIsInitialized = false;  // 강제 재초기화
		if (SourceStaticMesh)
		{
			InitializeFromStaticMeshInternal(SourceStaticMesh, true);  // bForce = true
		}

		UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty: SourceStaticMesh changed, reinitialized"));
	}

	// bShowGridCellDebug가 변경되면 처리
	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, bShowGridCellDebug))
	{
		if (bShowGridCellDebug)
		{
			// 디버그 켜기: 그리드 셀 그리기
			DrawGridCellDebug();
		}
		else
		{
			// 디버그 끄기: 기존 persistent 라인 제거
			if (UWorld* World = GetWorld())
			{
				FlushPersistentDebugLines(World);
			}
		}
	}
}
#endif // WITH_EDITOR

int32 URealtimeDestructibleMeshComponent::GetMaterialIDFromFaceIndex(int32 FaceIndex)
{
	if (FaceIndex == INDEX_NONE)
	{
		return 0;
	} 
	 
	if (UDynamicMesh* DynMesh = GetDynamicMesh())
	{
		const UE::Geometry::FDynamicMesh3& Mesh = DynMesh->GetMeshRef();

		if (Mesh.HasAttributes() && Mesh.Attributes()->HasMaterialID())
		{
			return Mesh.Attributes()->GetMaterialID()->GetValue(FaceIndex);
		}
	}

	return 0;
}

#if WITH_EDITOR
void URealtimeDestructibleMeshComponent::AutoFractureAndAssign()
{
	// 0. 스태틱 메시 유효성 검사
	UStaticMesh* InStaticMesh = SourceStaticMesh.Get();
	if (!InStaticMesh)
	{
		return;
	}

	// 1. UGC 임시객체 생성하고 스태틱 메시를 옮길 FGC 얻어오기
	// Transient는 디스크에 저장하지않고, 메모리에만 존재하는 데이터를 담는데 유용 
	
	// 에셋 이름, 패키징 경로 설정 
	FString ActorLabel = GetOwner() ? GetOwner()->GetActorLabel() : TEXT("Unknown");

	ActorLabel = ActorLabel.Replace(TEXT(" "), TEXT("_"));
	ActorLabel = ActorLabel.Replace(TEXT("."), TEXT("_"));
	ActorLabel = ActorLabel.Replace(TEXT(","), TEXT("_"));

	FString AssetName = FString::Printf(TEXT("GC_%s"), *ActorLabel);
	FString PackagePath = TEXT("/Game/GeneratedGeometryCollections/");
	FString FullPath = PackagePath + AssetName;

	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		return;
	}
	Package->FullyLoad();
	
	UGeometryCollection* GeometryCollection = NewObject<UGeometryCollection>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone
	); 
	if (!GeometryCollection)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed To Create Geometry Collection!!"));
		return;
	}
	
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GCPtr = GeometryCollection->GetGeometryCollection();
	if (!GCPtr.IsValid())
	{
		GCPtr = MakeShared<FGeometryCollection>();
		GeometryCollection->SetGeometryCollection(GCPtr);
	}
	 

	// 2. Source Static Mesh를 GC에 추가 (단일 조각으로 시작)
	TArray<UMaterialInterface*> Materials;
	for (const FStaticMaterial& StaticMat : InStaticMesh->GetStaticMaterials())
	{
		Materials.Add(StaticMat.MaterialInterface);
	}
	FGeometryCollectionConversion::AppendStaticMesh(
		InStaticMesh,
		Materials,
		FTransform::Identity,
		GeometryCollection,
		true
	);

	GCPtr = GeometryCollection->GetGeometryCollection();
	if (!GCPtr.IsValid())
	{
		return;
	}

	// 3. SliceCutter로 GC 격자 형태로 자르기
	// GC에 있는 조각(TransformGroup)을 전부 선택 상태로 만들기
	FDataflowTransformSelection TransformSelection;
	TransformSelection.InitializeFromCollection(*GCPtr, true);
	// 슬라이싱 영역 지정을 위해 원본 메시의 바운딩 볼륨 얻어오기
	FBox BoundingBox = InStaticMesh->GetBoundingBox();
	// Slice Cutter 실행
	// 주의: 깔끔한 육면체 절단을 위해 노이즈 관련 값은 0으로 둡니다.
	int32 NumCreated = FFractureEngineFracturing::SliceCutter(
		*GCPtr.Get(),           // 레퍼런스로 전달 (&InOutCollection)
		TransformSelection,     // 선택 영역
		BoundingBox,            // 자를 범위
		SliceCount.X - 1,           // X축 몇번 자를 지
		SliceCount.Y - 1,           // Y축 몇번 자를 지
		SliceCount.Z - 1,           // Z축 몇번 자를 지
		0.0f,					// 0 이면 수직, 수평
		0.0f,                   // 0 이면 정간격
		0,                      // Deterministic해야 하므로 Random seed는 상수 0 고정
		1.0f,                   // 파괴 확률 (ChanceToFracture) - 1.0f = 100%
		false,                  // 섬 분리 여부 (SplitIslands)
		0.0f,                   // Grout (틈새 벌리기) 
		0.0f,                   // Amplitude (노이즈 진폭) 
		0.0f,                   // Frequency (노이즈 빈도)
		0.0f,                   // Persistence
		0.0f,                   // Lacunarity
		0,                      // OctaveNumber
		0.0f,                   // PointSpacing
		false,                   // AddSamplesForCollision
		0.0f
	);
	if (NumCreated <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SliceCutter FAILED! Returned %d"), NumCreated);
		return;

	}
	int32 NumTransformsAfterSlice = GCPtr->NumElements(FGeometryCollection::TransformGroup);

	// =========================================================================
	// 후처리로 데이터 무결성 갱신
	// 이 코드가 없으면 "Name not mapped" 또는 "Serialize not deterministic" 에러 발생
	// =========================================================================

	// Collection 무효화 및 재구성 (중요!)
	// GC 무효화 및 알림. 이전에 기록된 모든 캐시가 이 컬렉션을 사용할 수 없게 만듦.
	GeometryCollection->Materials = Materials;
	GeometryCollection->InvalidateCollection();

	GCPtr = GeometryCollection->GetGeometryCollection();
	//	바운딩 박스 재계산
	GCPtr->UpdateBoundingBox();

	// 에디터 변경 알림 (직렬화 준비)
	GeometryCollection->PostEditChange();
	GCPtr = GeometryCollection->GetGeometryCollection();

	// 에셋을 디스크에도 저장을 따로하자 
	FAssetRegistryModule::AssetCreated(GeometryCollection);

	// 저장
	GeometryCollection->MarkPackageDirty();
	Package->MarkPackageDirty();

	// 실제 파일 경로 계산
	FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension()  // .uasset
	);

	// 디렉토리가 없으면 생성
	FString DirectoryPath = FPaths::GetPath(PackageFileName);
	if (!IFileManager::Get().DirectoryExists(*DirectoryPath))
	{
		IFileManager::Get().MakeDirectory(*DirectoryPath, true);
	}

	// 패키지 저장
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GError;
	SaveArgs.bForceByteSwapping = false;
	SaveArgs.bWarnOfLongFilename = true;

	bool SaveResult = UPackage::SavePackage(
		Package,
		GeometryCollection,
		*PackageFileName,
		SaveArgs
	);

	if (!SaveResult)
	{
		UE_LOG(LogTemp, Log, TEXT("Failed to save GeometryCollectio: %s"), *PackageFileName);
	}


	// 컴포넌트에 자동 할당
	FracturedGeometryCollection = GeometryCollection;


	// Cell 메시 빌드
	int32 CellCount = BuildChunkMeshesFromGeometryCollection();

	if (AActor* Owner = GetOwner())
	{
		Owner->Modify();
		Owner->RerunConstructionScripts();
	}

	return;
}
void URealtimeDestructibleMeshComponent::RevertFracture()
{
	// Cell로 나눠진 상태가 아니면  return
	if (ChunkMeshComponents.Num() == 0)
	{
		return;
	}

	// Ctrl Z를 위한 Snap Shot
	if (AActor* Owner = GetOwner())
	{
		Owner->Modify();
		this->Modify();
	}

	// 생성된 Cell Components
	for (UDynamicMeshComponent* Cell : ChunkMeshComponents)
	{
		if (Cell)
		{
			Cell->DestroyComponent();
		}
	}

	ChunkMeshComponents.Empty();
	GridToChunkMap.Reset();

	bChunkMeshesValid = false;
	SetSourceMeshEnabled(true);

	ResetToSourceMesh();

	// Editor 강제 갱신
	if (AActor* Owner = GetOwner())
	{
		Owner->RerunConstructionScripts();
	}

}
#endif

FRealtimeDestructibleMeshComponentInstanceData::FRealtimeDestructibleMeshComponentInstanceData(
	const URealtimeDestructibleMeshComponent* SourceComponent)
	: FActorComponentInstanceData(SourceComponent)
{
	if (SourceComponent)
	{
		SavedSourceStaticMesh = SourceComponent->SourceStaticMesh;
		bSavedIsInitialized = SourceComponent->bIsInitialized;
		bSavedChunkMeshesValid = SourceComponent->bChunkMeshesValid;

		SavedSliceCount = SourceComponent->SliceCount;
		bSavedShowGridCellDebug = SourceComponent->bShowGridCellDebug;

		// 포인터 대신 컴포넌트 이름을 저장 (PIE 복제 시 이름으로 찾기 위함)
		SavedChunkComponentNames.Empty();
		SavedChunkComponentNames.Reserve(SourceComponent->ChunkMeshComponents.Num());
		for (const UDynamicMeshComponent* Cell : SourceComponent->ChunkMeshComponents)
		{
			if (Cell)
			{
				SavedChunkComponentNames.Add(Cell->GetName());
			}
			else
			{
				SavedChunkComponentNames.Add(FString());  // nullptr은 빈 문자열로
			}
		}

		UE_LOG(LogTemp, Warning, TEXT("InstanceData Constructor: bCellMeshesValid=%d, CellMeshComponents.Num=%d, SavedNames.Num=%d"),
			bSavedChunkMeshesValid, SourceComponent->ChunkMeshComponents.Num(), SavedChunkComponentNames.Num());
	}
}

void FRealtimeDestructibleMeshComponentInstanceData::ApplyToComponent(
	UActorComponent* Component,
	const ECacheApplyPhase CacheApplyPhase)
{
	UE_LOG(LogTemp, Warning, TEXT("ApplyToComponent: Phase=%d, bSavedCellMeshesValid=%d, SavedCellNames.Num=%d"),
		(int32)CacheApplyPhase, bSavedChunkMeshesValid, SavedChunkComponentNames.Num());

	Super::ApplyToComponent(Component, CacheApplyPhase);

	if (URealtimeDestructibleMeshComponent* DestructComp = Cast<URealtimeDestructibleMeshComponent>(Component))
	{
		// BP 기본값 대신 저장된 인스턴스 값으로 복원
		DestructComp->SourceStaticMesh = SavedSourceStaticMesh;
		DestructComp->SliceCount = SavedSliceCount;
		DestructComp->bShowGridCellDebug = bSavedShowGridCellDebug;

		// Cell 모드 상태 복원
		DestructComp->bChunkMeshesValid = bSavedChunkMeshesValid;

		// PIE에서는 포인터가 유효하지 않으므로 이름으로 복제된 컴포넌트를 찾음
		if (AActor* Owner = DestructComp->GetOwner())
		{
			DestructComp->ChunkMeshComponents.Empty();
			DestructComp->ChunkMeshComponents.SetNum(SavedChunkComponentNames.Num());

			TArray<UDynamicMeshComponent*> FoundCells;
			Owner->GetComponents<UDynamicMeshComponent>(FoundCells);

			UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: Found %d DynamicMeshComponents in owner"), FoundCells.Num());

			for (int32 i = 0; i < SavedChunkComponentNames.Num(); ++i)
			{
				if (SavedChunkComponentNames[i].IsEmpty())
				{
					// 인덱스 0은 루트(nullptr)
					DestructComp->ChunkMeshComponents[i] = nullptr;
					continue;
				}

				// 이름으로 복제된 컴포넌트 찾기
				UDynamicMeshComponent* FoundCell = nullptr;
				for (UDynamicMeshComponent* Cell : FoundCells)
				{
					if (Cell && Cell->GetName() == SavedChunkComponentNames[i])
					{
						FoundCell = Cell;
						break;
					}
				}

				if (FoundCell)
				{
					DestructComp->ChunkMeshComponents[i] = FoundCell;
					// 부모 연결 확인
					if (FoundCell->GetAttachParent() != DestructComp)
					{
						FoundCell->AttachToComponent(DestructComp, FAttachmentTransformRules::KeepRelativeTransform);
					}
					UE_LOG(LogTemp, Verbose, TEXT("ApplyToComponent: Found Cell_%d by name: %s"), i, *SavedChunkComponentNames[i]);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("ApplyToComponent: Could not find Cell by name: %s"), *SavedChunkComponentNames[i]);
					DestructComp->ChunkMeshComponents[i] = nullptr;
				}
			}

			UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: Rebuilt CellMeshComponents with %d entries"), DestructComp->ChunkMeshComponents.Num());
		}

		// Cell 모드가 활성화 되어 있고, 유효하면
		if (bSavedChunkMeshesValid)
		{
			// GridToChunkMap은 저장되지 않으므로 재구축
			DestructComp->BuildGridToChunkMap();

			// GridCellCache는 저장되므로, 유효하지 않을 때만 재구축
			if (!DestructComp->GridCellCache.IsValid())
			{
				UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: GridCellCache is invalid, rebuilding..."));
			DestructComp->BuildGridCells();
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: GridCellCache loaded from saved data (ValidCells=%d)"),
					DestructComp->GridCellCache.GetValidCellCount());
			}
			return;
		}


		// Cell 모드가 아닐 때, 메시 재초기화
		if (SavedSourceStaticMesh)
		{
			DestructComp->bIsInitialized = false;  // 강제 재초기화
			DestructComp->InitializeFromStaticMesh(SavedSourceStaticMesh);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Server Validation (서버 검증)
//////////////////////////////////////////////////////////////////////////

bool URealtimeDestructibleMeshComponent::ValidateDestructionRequest(
	const FRealtimeDestructionRequest& Request,
	APlayerController* RequestingPlayer,
	EDestructionRejectReason& OutReason)
{
	OutReason = EDestructionRejectReason::None;

	// 플레이어가 없으면 검증 스킵 (서버 직접 호출 등)
	if (!RequestingPlayer)
	{
		return true;
	}

	// 1. 최대 구멍 수 체크
	if (MaxHoleCount > 0 && CurrentHoleCount >= MaxHoleCount)
	{
		OutReason = EDestructionRejectReason::MaxHoleReached;
		return false;
	}

	// 2. 사거리 체크
	if (APawn* Pawn = RequestingPlayer->GetPawn())
	{
		const float Distance = FVector::Dist(Pawn->GetActorLocation(), Request.ImpactPoint);
		if (Distance > MaxDestructionRange)
		{
			OutReason = EDestructionRejectReason::OutOfRange;
			return false;
		}
	}

	// 3. 시야 체크 (LineTrace)
	if (bEnableLineOfSightCheck)
	{
		if (APawn* Pawn = RequestingPlayer->GetPawn())
		{
			FHitResult HitResult;
			FCollisionQueryParams QueryParams;
			QueryParams.AddIgnoredActor(Pawn);

			const FVector Start = Pawn->GetActorLocation();
			const FVector End = Request.ImpactPoint;

			if (GetWorld()->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, QueryParams))
			{
				// 히트한 컴포넌트가 이 컴포넌트가 아니면 시야 차단
				if (HitResult.GetComponent() != this && HitResult.GetComponent() != nullptr)
				{
					// Cell 메시 중 하나인지 확인
					bool bHitOurCell = false;
					for (const auto& CellComp : ChunkMeshComponents)
					{
						if (HitResult.GetComponent() == CellComp)
						{
							bHitOurCell = true;
							break;
						}
					}

					if (!bHitOurCell)
					{
						OutReason = EDestructionRejectReason::LineOfSightBlocked;
						return false;
					}
				}
			}
		}
	}

	// 4. 연사 제한 (TODO: 플레이어별 추적 필요)
	// 현재는 단순 구현 - 추후 TMap<APlayerController*, FRateLimitInfo> 사용

	// 5. 유효한 위치 체크 (메시 내부인지)
	// TODO: 필요시 구현

	return true;
}

void URealtimeDestructibleMeshComponent::ClientDestructionRejected_Implementation(uint16 Sequence, EDestructionRejectReason Reason)
{
	// 클라이언트에서 거부 처리
	UE_LOG(LogTemp, Warning, TEXT("[Destruction] Request rejected - Seq: %d, Reason: %s"),
		Sequence, *UEnum::GetValueAsString(Reason));

	// 블루프린트/C++ 이벤트 브로드캐스트
	OnDestructionRejected.Broadcast(static_cast<int32>(Sequence), Reason);
}

TStructOnScope<FActorComponentInstanceData> URealtimeDestructibleMeshComponent::GetComponentInstanceData() const
{
	UE_LOG(LogTemp, Warning, TEXT("GetComponentInstanceData"));

	return MakeStructOnScope<FActorComponentInstanceData, FRealtimeDestructibleMeshComponentInstanceData>(this);
}	

void URealtimeDestructibleMeshComponent::ApplyHCLaplacianSmoothing(FDynamicMesh3& Mesh)
{
	if (SmoothingIterations <= 0 || Mesh.TriangleCount() == 0)
	{
		return;
	}

	// 원본 위치 저장 (HC Laplacian 보정용)
	TMap<int32, FVector3d> OriginalPositions;
	for (int32 Vid : Mesh.VertexIndicesItr())
	{
		OriginalPositions.Add(Vid, Mesh.GetVertex(Vid));
	}

	for (int32 Iter = 0; Iter < SmoothingIterations; Iter++)
	{
		// 1단계: Uniform Laplacian Smoothing
		// 각 정점을 이웃 정점들의 평균 위치로 이동
		TMap<int32, FVector3d> SmoothedPositions;

		for (int32 Vid : Mesh.VertexIndicesItr())
		{
			FVector3d Sum = FVector3d::Zero();
			int32 Count = 0;

			// 1-ring 이웃 정점 수집
			// EnumerateVertexVertices(Vid, Lambda):
			//   - Vid와 엣지로 직접 연결된 모든 인접 정점(1-ring neighbors)을 순회
			//   - 내부적으로 Vid에 연결된 모든 엣지를 찾고, 각 엣지의 반대편 정점 ID를 Lambda에 전달
			//   - 메시 토폴로지 기반 순회이므로 공간 거리와 무관하게 연결된 정점만 반환
			//   - Laplacian Smoothing에서 이 이웃들의 평균 위치로 현재 정점을 이동시킴
			Mesh.EnumerateVertexVertices(Vid, [&](int32 Nid)
			{
				Sum += Mesh.GetVertex(Nid);
				Count++;
			});

			if (Count > 0)
			{
				FVector3d Current = Mesh.GetVertex(Vid);
				FVector3d Average = Sum / static_cast<double>(Count);
				SmoothedPositions.Add(Vid, FMath::Lerp(Current, Average, static_cast<double>(SmoothingStrength)));
			}
		}

		// 스무딩된 위치 적용
		for (const auto& Pair : SmoothedPositions)
		{
			Mesh.SetVertex(Pair.Key, Pair.Value);
		}

		// 2단계: HC Laplacian 보정 (수축 방지)
		// b = p' - original (차이 벡터)
		// 최종 = p' - (β × b + (1-β) × 이웃들의 b 평균)
		TMap<int32, FVector3d> DifferenceVectors;
		for (int32 Vid : Mesh.VertexIndicesItr())
		{
			FVector3d Smoothed = Mesh.GetVertex(Vid);
			FVector3d Original = OriginalPositions[Vid];
			DifferenceVectors.Add(Vid, Smoothed - Original);
		}

		// 보정 적용
		TMap<int32, FVector3d> CorrectedPositions;
		for (int32 Vid : Mesh.VertexIndicesItr())
		{
			FVector3d Smoothed = Mesh.GetVertex(Vid);
			FVector3d B = DifferenceVectors[Vid];

			// 이웃들의 차이 벡터 평균 계산
			FVector3d NeighborBSum = FVector3d::Zero();
			int32 NeighborCount = 0;
			Mesh.EnumerateVertexVertices(Vid, [&](int32 Nid)
			{
				NeighborBSum += DifferenceVectors[Nid];
				NeighborCount++;
			});

			FVector3d NeighborBAvg = (NeighborCount > 0)
				? NeighborBSum / static_cast<double>(NeighborCount)
				: FVector3d::Zero();

			// 보정: p'' = p' - (β × b + (1-β) × 이웃 b 평균)
			double Beta = static_cast<double>(HCBeta);
			FVector3d Correction = Beta * B + (1.0 - Beta) * NeighborBAvg;
			CorrectedPositions.Add(Vid, Smoothed - Correction);
		}

		// 보정된 위치 적용
		for (const auto& Pair : CorrectedPositions)
		{
			Mesh.SetVertex(Pair.Key, Pair.Value);
		}

		// 다음 반복을 위해 원본 위치 갱신
		for (int32 Vid : Mesh.VertexIndicesItr())
		{
			OriginalPositions[Vid] = Mesh.GetVertex(Vid);
		}
	}
}	

