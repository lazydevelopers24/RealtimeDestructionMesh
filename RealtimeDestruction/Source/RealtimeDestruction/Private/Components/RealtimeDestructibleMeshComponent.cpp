// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Components/RealtimeDestructibleMeshComponent.h"
#include "TimerManager.h"
#include "GameFramework/Pawn.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshEditor.h"
#include "MeshBoundaryLoops.h"
#include "Operations/SimpleHoleFiller.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Algo/Sort.h"
#include <algorithm> // std::lower_bound
#include "Engine/World.h"
#include "Actors/DebrisActor.h"
#include "Components/BoxComponent.h"

#include "DynamicMesh/MeshNormals.h" 
#include "DrawDebugHelpers.h"

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
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
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
#include "Data/ImpactProfileDataAsset.h"
#include "ProceduralMeshComponent.h"
#if WITH_EDITOR
#include "Selection.h"
#endif
#include "DebugConsoleVariables.h"
#include "Net/UnrealNetwork.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
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

	Compact.ToolOriginWorld = Request.ToolOriginWorld;

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

	// ToolOriginWorld 계산 - DestructionProjectileComponent::SetShapeParameters와 동일한 공식 사용
	switch (ToolShape)
	{
	case EDestructionToolShape::Cylinder:
		Request.ToolOriginWorld = Request.ImpactPoint - (Request.ToolForwardVector * Request.ShapeParams.SurfaceMargin);
		break;
	case EDestructionToolShape::Sphere:
		Request.ToolOriginWorld = ToolOriginWorld;
		break;
	default:
		Request.ToolOriginWorld = Request.ImpactPoint - (Request.ToolForwardVector * Request.ShapeParams.SurfaceMargin);
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
#include <Selection/MeshTopologySelectionMechanic.h>

URealtimeDestructibleMeshComponent::URealtimeDestructibleMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = true;  // 서버 배칭용
	SetIsReplicatedByDefault(true);
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
	
	bIsInitialized = false;
	InitializeFromStaticMeshInternal(SourceStaticMesh, true);
}

// 현재는 RequestDestruction에서만 호출됨
FDestructionOpId URealtimeDestructibleMeshComponent::EnqueueRequestLocal(const FRealtimeDestructionRequest& Request, bool bIsPenetration, UDecalComponent* TemporaryDecal, int32 BatchId)
{
	if (!BooleanProcessor.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Boolean Processor is null"));
		NotifyBooleanSkipped(BatchId);
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
		if (FRDMCVarHelper::EnableAsyncBooleanOp() && ChunkMeshComponents.IsValidIndex(Op.Request.ChunkIndex))
		{
			BooleanProcessor->EnqueueOp(MoveTemp(Op), TemporaryDecal, ChunkMeshComponents[Op.Request.ChunkIndex].Get(),
			                            BatchId);
	}
	else
	{
			UE_LOG(LogTemp, Display, TEXT("BooleanSync"));
			BooleanProcessor->EnqueueOp(MoveTemp(Op), TemporaryDecal, this, BatchId);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[EnqueueRequestLocal] ChunkIndex=INDEX_NONE → Boolean 연산 스킵!"));
		NotifyBooleanSkipped(BatchId);
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

	// 서버에서만 Cluetering 등록
	if (bEnableClustering && BulletClusterComponent && GetOwner()->HasAuthority())
	{
		BulletClusterComponent->RegisterRequest(Request); 
	}
	
	return ExecuteDestructionInternal(Request);
}

bool URealtimeDestructibleMeshComponent::ExecuteDestructionInternal(const FRealtimeDestructionRequest& Request)
{
	TRACE_CPUPROFILER_EVENT_SCOPE("ExecuteDestructionInternal")
	
	// 벽 무너지는거 자연스럽게 하기 위해 forward를 캐싱해놓기
	CachedToolForwardVector = Request.ToolForwardVector;

	// 데디케이티드 서버에서는 Boolean 연산 스킵 (시각적 처리 불필요)
	// Cell 상태만 업데이트하고 콜리전 갱신은 별도 처리
	if (IsRunningDedicatedServer())
	{
		FDestructionResult Result = DestructionLogic(Request);
		PendingDestructionResults.Add(Result);
		return true;
	}

	// 관통 여부 판단 (큐 우선순위 결정용)
	bool bIsPenetrating = IsChunkPenetrated(Request);

	FDestructionResult Result = DestructionLogic(Request);
	PendingDestructionResults.Add(Result);

	UDecalComponent* TempDecal = nullptr;
	if (Request.bSpawnDecal)
	{
		TempDecal = SpawnTemporaryDecal(Request);
	}

	EnqueueRequestLocal(Request, bIsPenetrating, TempDecal);
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
	// 구조적 무결성 비활성화 또는 GridCellLayout 미생성 시 스킵
	if (!bEnableStructuralIntegrity || !GridCellLayout.IsValid())
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
	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_DestructionLogic);

	FDestructionResult DestructionResult;

	// Request를 FDestructionShape로 변환
	FCellDestructionShape Shape = FCellDestructionShape::CreateFromRequest(Request);

	// 양자화된 입력 생성
	FQuantizedDestructionInput QuantizedInput = FQuantizedDestructionInput::FromDestructionShape(Shape);

	//=====================================================================
	// Phase 1: Cell / SubCell 파괴 처리
	//=====================================================================
	if (bEnableSubcell)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_ProcessCellDestructionWithSubCells);

		DestructionResult = FCellDestructionSystem::ProcessCellDestructionSubCellLevel(
			GridCellLayout,
			QuantizedInput,
			GetComponentTransform(),
			CellState);
	}
	else
	{
		DestructionResult = FCellDestructionSystem::ProcessCellDestruction(
			GridCellLayout,
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

		// 서버는 supercell 남은 ratio 계산
		if (GetOwner() && GetOwner()->HasAuthority() && bEnableSupercell && SupercellState.IsValid())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Debris_CalcRatio_ForceRemove);

			TSet<int32> AffectedSupercells;
			

			for (int32 CellId : DestructionResult.NewlyDestroyedCells)
			{
				int32 SuperCellId = SupercellState.GetSupercellForCell(CellId);
				if (SuperCellId != INDEX_NONE && SupercellState.DestroyedCellCounts.IsValidIndex(SuperCellId))
				{
					SupercellState.DestroyedCellCounts[SuperCellId]++;
					AffectedSupercells.Add(SuperCellId);
				}
			}

			for (int32 SuperCellId : AffectedSupercells)
			{
				const int32 InitialCount = SupercellState.InitialValidCellCounts[SuperCellId];
				if (InitialCount <= 0)
				{
					continue;
				}

				const float DestroyRatio = static_cast<float>(SupercellState.DestroyedCellCounts[SuperCellId])
					/ static_cast<float>(InitialCount);

				if (DestroyRatio >= DestroyRatioThresholdForDebris)
				{
					ForceRemoveSupercell(SuperCellId);
					MulticastForceRemoveSupercell(SuperCellId);
				}
			}
		}

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
				const FIntArray& Neighbors = GridCellLayout.GetCellNeighbors(CellId);
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
	if (bEnableSupercell && SupercellState.IsValid())
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_UpdateSupercellStates);

		// 영향받은 Cell들이 속한 SuperCell을 Broken으로 마킹
		SupercellState.UpdateSupercellStates(DestructionResult.AffectedCells);

		// 파괴된 Cell들이 속한 SuperCell도 Broken으로 마킹
		for (int32 DestroyedCellId : DestructionResult.NewlyDestroyedCells)
		{
			SupercellState.OnCellDestroyed(DestroyedCellId);
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
					SupercellState.OnSubCellDestroyed(SubCellPair.Key, SubCellId);
				}
			}
		}
	}
	}

	return DestructionResult;
}

void URealtimeDestructibleMeshComponent::DisconnectedCellStateLogic(const TArray< FDestructionResult>& AllResults, bool bForceRun)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_DisconnectedCellStateLogic);

	// Structural Integrity 비활성화 시 분리 셀 처리 스킵
	if (!bEnableStructuralIntegrity)
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[DisconnectedCellStateLogic] ENTER: AllResults=%d, DestroyedCells=%d, bForceRun=%d"),
		AllResults.Num(), CellState.DestroyedCells.Num(), bForceRun ? 1 : 0);

	//파괴된게 없으면 패스 (bForceRun이면 무조건 BFS 실행)
	if (!bForceRun)
	{
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
			UE_LOG(LogTemp, Warning, TEXT("[DisconnectedCellStateLogic] EARLY RETURN: No destruction in AllResults"));
			return;
		}
	}

	TArray<int32> AffectedNeighborCells;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindAffectedNeighborCells);
		TSet<int32> UniqueNeighbors;

		for (const FDestructionResult& Result : AllResults)
		{
			for (int32 DestroyedCellId : Result.NewlyDestroyedCells)
			{
				const FIntArray& Neighbors = GridCellLayout.GetCellNeighbors(DestroyedCellId);
				for (int32 NeighborId : Neighbors.Values)
				{
					// 파괴되지않고, 존재하는 이웃 Cell만 순회 
					if (!CellState.DestroyedCells.Contains(NeighborId) &&
						GridCellLayout.GetCellExists(NeighborId))
					{
						UniqueNeighbors.Add(NeighborId);
					}
				}
			}
			
			if (bEnableSubcell)
			{
				for (int32 AffectedCellId : Result.AffectedCells)
				{
					const FIntArray& Neighbors = GridCellLayout.GetCellNeighbors(AffectedCellId);

					for (int32 NeighborId : Neighbors)
					{
						// 파괴되지않고, 존재하는 이웃 Cell만 순회 
						if (!CellState.DestroyedCells.Contains(NeighborId) &&
							GridCellLayout.GetCellExists(NeighborId))
						{
							UniqueNeighbors.Add(NeighborId);
						}

					}
					
				}
			}
		}
	
		AffectedNeighborCells = UniqueNeighbors.Array();
	}
	//=====================================================================
	// Phase 2: DFS로 앵커에서 분리된 셀 찾기
	// 통합 API 사용: bEnableSupercell, bEnableSubcell 플래그에 따라 자동 선택
	// Multiplayer: SubCell 상태는 Client에 동기화되지 않으므로 Standalone에서만 사용
	//======================================================== 
	  
	//[depricated]
	//TSet<int32> DisconnectedCells;
	//{
	//	TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindDisconnectedCells);
	//	const ENetMode NetMode = GetWorld()->GetNetMode();
	//	DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
	//		GridCellLayout,
	//		SupercellState,
	//		CellState,
	//		bEnableSupercell && SupercellState.IsValid(),
	//		bEnableSubcell && (NetMode == NM_Standalone),
	//		CellContext); // subcell 동기화 안 하므로 subcell은 standalone에서만 허용
	//}
	// Standalon용 BFS 성능 LOG
	//double BFSEndTime = FPlatformTime::Seconds();
	//UE_LOG(LogTemp, Warning, TEXT("[BFS #%d] FindDisconnectedCells END - took %.3f ms, found %d disconnected"),
	//	BFSCallCount, (BFSEndTime - BFSStartTime) * 1000.0, DisconnectedCells.Num());
	 
	TSet<int32> DisconnectedCells; 
	if (AffectedNeighborCells.Num() > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_FindDisconnectedCellsFromAffected);

		const ENetMode NetMode = GetWorld()->GetNetMode();
		DisconnectedCells = FCellDestructionSystem::FindDisconnectedCellsFromAffected(
			GridCellLayout,
			SupercellState ,
			CellState,
			AffectedNeighborCells,
			CellContext,
			true && SupercellState.IsValid() ,
			bEnableSubcell && (NetMode == NM_Standalone)
		);
	}
	else if (bForceRun)
	{
		const ENetMode NetMode = GetWorld()->GetNetMode();

		DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
			GridCellLayout,
			SupercellState,
			CellState,
			bEnableSupercell && SupercellState.IsValid(),
			bEnableSubcell && (NetMode == NM_Standalone),
			CellContext); // subcell 동기화 안 하므로 subcell은 standalone에서만 허용
	}
	 
	UE_LOG(LogTemp, Log, TEXT("[Cell] Phase 2: %d Cells disconnected"), DisconnectedCells.Num()); 

	if (DisconnectedCells.Num() > 0)
	{
		//=====================================================================
		// Phase 3: 분리된 셀 그룹화
		//=====================================================================
		TArray<TArray<int32>> NewDetachedGroups;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_Phase3); 
			NewDetachedGroups = FCellDestructionSystem::GroupDetachedCells(
				GridCellLayout,
				DisconnectedCells,
				CellState.DestroyedCells);
		}
		for (const TArray<int32>& Group : NewDetachedGroups)
		{
			CellState.AddDetachedGroup(Group);
		}

		//=====================================================================
		// Phase 4: 서버 → 클라이언트 신호 전송 (서버에서만)
		//=====================================================================
		// 클라이언트에게 Detach 발생 신호만 전송 (클라이언트가 자체 BFS 실행)

		// 분리된 셀의 삼각형 삭제 (데디서버: 렌더링 불필요, Cell Box만 업데이트)
 		{  
			const ENetMode NetMode = GetWorld() ? GetWorld()->GetNetMode() : NM_Standalone;
			const bool bIsDedicatedServerClient = bServerIsDedicatedServer && !GetOwner()->HasAuthority();

			TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_Phase4);

			// dedicated server는 메시 연산없이 메타 데이터만으로 actor 스폰
			if (NetMode == NM_DedicatedServer)
			{
				for (const TArray<int32>& Group : NewDetachedGroups)
				{
					SpawnDebrisActorForDedicatedServer(Group);
				}
			} 
			else if (bIsDedicatedServerClient)
			{
				// 크기가 작은 debris만 클라이언트가 자체 생성
				for (const TArray<int32>& Group : NewDetachedGroups)
				{
					float DebrisSize = CalculateDebrisBoundsExtent(Group);  // 헬퍼 함수 필요
					if (DebrisSize < MinDebrisSyncSize)
					{
						RemoveTrianglesForDetachedCells(Group);
					}
					// else: 큰 것은 서버에서 복제된 DebrisActor가 처리

				}
			}
			else
			{
				for (const TArray<int32>& Group : NewDetachedGroups)
				{
					RemoveTrianglesForDetachedCells(Group);
				}
			}

			// Cleanup은 IslandRemoval 완료 콜백에서 처리 (비동기)
			// FIslandRemovalContext::DisconnectedCellsForCleanup 사용
		}
		CellState.MoveAllDetachedToDestroyed();

		// 서버 Cell Collision: 분리된 셀들의 청크도 dirty 마킹
		if (bServerCellCollisionInitialized)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_MarkCollisionChunkDirty);

			TSet<int32> DetachedDirtyChunks;
			for (int32 CellId : DisconnectedCells)
			{
				int32 ChunkIdx = GetCollisionChunkIndexForCell(CellId);
				if (ChunkIdx != INDEX_NONE)
				{
					DetachedDirtyChunks.Add(ChunkIdx);
				}
				// 이웃 셀 청크도 dirty (새 표면 될 수 있음)
				const FIntArray& Neighbors = GridCellLayout.GetCellNeighbors(CellId);
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
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_CleanCell);

		// 분리된 셀 없어도 파괴된 셀 있으면 파편 정리 (데디서버 제외)
		if (!GetWorld() || GetWorld()->GetNetMode() != NM_DedicatedServer)
		{
			CleanupSmallFragments(DisconnectedCells);
		}
	}

	// 데칼 정리 (데디서버에서는 불필요)
	if (!GetWorld() || GetWorld()->GetNetMode() != NM_DedicatedServer)
	{ 
		TRACE_CPUPROFILER_EVENT_SCOPE(CellStructure_CleanDecal);
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
	}

	UE_LOG(LogTemp, Log, TEXT("UpdateCellStateFromDestruction Complete: Destroyed=%d, DetachedGroups=%d"),
		CellState.DestroyedCells.Num(), CellState.DetachedGroups.Num());

	// Late Join용: 현재 파괴 셀 상태 스냅샷 갱신 (서버에서만)
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		LateJoinDestroyedCells = CellState.DestroyedCells.Array();
	}

#if !UE_BUILD_SHIPPING
	// 디버그 텍스트 업데이트
	bShouldDebugUpdate = true;
#endif
}

float URealtimeDestructibleMeshComponent::CalculateDebrisBoundsExtent(const TArray<int32>& CellIds) const
{
	if (CellIds.Num() == 0)
	{
		return 0.0f;
	}

	FBox CellBounds(ForceInit);
	for (int32 CellId : CellIds)
	{
		FVector CellMin = GridCellLayout.IdToLocalMin(CellId);
		FVector CellMax = CellMin + GridCellLayout.CellSize;
		CellBounds += CellMin;
		CellBounds += CellMax;
	}

	FVector BoxExtent = CellBounds.GetExtent();
	BoxExtent *= GetComponentTransform().GetScale3D(); 

	return BoxExtent.GetMax();
}
void URealtimeDestructibleMeshComponent::ForceRemoveSupercell(int32 SuperCellId)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_ForceRemoveSupercell);
	// alive cell 수집
	TArray<int32> AllCellsInSupercell;
	SupercellState.GetCellsInSupercell(SuperCellId, GridCellLayout, AllCellsInSupercell);

	TArray<int32> AliveCells;
	for (int32 CellId : AllCellsInSupercell)
	{
		if (!CellState.DestroyedCells.Contains(CellId))
		{
			AliveCells.Add(CellId);
		}
	}

	if (AliveCells.Num() == 0) return;
	

	// 렌더링 처리 (Dedicated Server는 패스) 
	// ToolMesh(smoothed + DebrisExpandRatio) 형상과 겹치는 cell도 함께 수집
	const bool bIsDedicatedServer = GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer;
	const bool bIsDedicatedServerClient = bServerIsDedicatedServer && !GetOwner()->HasAuthority();
	
	TArray<int32> ToolMeshOverlappingCells;
	 
	if (bIsDedicatedServer)
	{
		SpawnDebrisActorForDedicatedServer(AllCellsInSupercell);
		CollectToolMeshOverlappingCells(AllCellsInSupercell, ToolMeshOverlappingCells);
	}
	else if (bIsDedicatedServerClient)
	{
		float DebrisSize = CalculateDebrisBoundsExtent(AllCellsInSupercell);
		if (DebrisSize < MinDebrisSyncSize)
		{
			// 작은 debris: 클라이언트가 자체 boolean + cell 수집
			RemoveTrianglesForDetachedCells(AllCellsInSupercell, nullptr, &ToolMeshOverlappingCells);
		}
		else
		{
			// 큰 debris: 서버 DebrisActor가 boolean 처리, cell 수집만 수행
			CollectToolMeshOverlappingCells(AllCellsInSupercell, ToolMeshOverlappingCells);
		}
	}
	else
	{
		RemoveTrianglesForDetachedCells(AllCellsInSupercell, nullptr, &ToolMeshOverlappingCells);
		// Cleanup은 IslandRemoval 완료 콜백에서 처리 (비동기)
	}

	// Supercell 삭제하면서 이웃 Supercell의 cell에 영향을 끼칠 수 있음, 
	// 이웃 supercell에 삭제된게 있으면 업데이트를 해준다. 
	if (ToolMeshOverlappingCells.Num() > 0)
	{
		// 원본 supercell cell과 합치기 (AddUnique로 수집했으므로 중복 없음)
		TSet<int32> OriginalCellSet(AllCellsInSupercell);
		for (int32 CellId : ToolMeshOverlappingCells)
		{
			if (!OriginalCellSet.Contains(CellId))
			{
				AllCellsInSupercell.Add(CellId);

				// 인접 supercell의 DestroyedCellCount 업데이트
				if (bEnableSupercell && SupercellState.IsValid())
				{
					const int32 NeighborSCId = SupercellState.GetSupercellForCell(CellId);
					if (NeighborSCId != INDEX_NONE && NeighborSCId != SuperCellId &&
						SupercellState.DestroyedCellCounts.IsValidIndex(NeighborSCId))
					{
						SupercellState.DestroyedCellCounts[NeighborSCId]++;
					}
				}
			}
		}
	}
	CellState.DestroyCells(AllCellsInSupercell);

	// hit count 리셋
	SupercellState.MarkSupercellBroken(SuperCellId);

	if (SupercellState.DestroyedCellCounts.IsValidIndex(SuperCellId))
	{
		SupercellState.DestroyedCellCounts[SuperCellId] = 0;
		SupercellState.InitialValidCellCounts[SuperCellId] = 0;
	}

	// 파편 정리 예약
	bPendingCleanup = true;
}

void URealtimeDestructibleMeshComponent::MulticastForceRemoveSupercell_Implementation(int32 SuperCellId)
{
	// DedicatedServer는 Pass 
	if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	
	// 서버는 로컬에서 처리함
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	ForceRemoveSupercell(SuperCellId);
}

int32 URealtimeDestructibleMeshComponent::GridCellIdToChunkId(int32 GridCellId) const
{
	if (!GridCellLayout.IsValidCellId(GridCellId))
	{
		UE_LOG(LogTemp, Warning, TEXT("GridCellIdToChunkId: Invalid CellId=%d"), GridCellId);
		return INDEX_NONE;
	}
	if (GridToChunkMap.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GridCellIdToChunkId: GridToChunkMap is empty!"));
		return INDEX_NONE;
	}

	// GridCellLayout에서 로컬 중심점 획득
	const FVector LocalCenter = GridCellLayout.IdToLocalCenter(GridCellId);

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

	// 데디케이티드 서버 및 클라이언트에서 실행 (Standalone/ListenServer는 원본 메시 콜리전 사용)
	if (!GetWorld())
	{
		return;
	}
	const ENetMode NetMode = GetWorld()->GetNetMode();
	if (NetMode != NM_DedicatedServer && NetMode != NM_Client)
	{
		return;
	}

	if (!GridCellLayout.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] GridCellLayout is not valid, skipping"));
		return;
	}

	// 동적 청크 분할 수 계산
	const int32 TotalCells = GridCellLayout.GetValidCellCount();
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

	for (int32 SparseIdx = 0; SparseIdx < GridCellLayout.GetValidCellCount(); ++SparseIdx)
	{
		const int32 CellId = GridCellLayout.SparseIndexToCellId[SparseIdx];
		const FVector LocalCenter = GridCellLayout.IdToLocalCenter(CellId);

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

	if (NetMode == NM_DedicatedServer)
	{
		// 서버: 전체 충돌 비활성화 (Cell Box가 모든 충돌 담당)
		SetCollisionEnabled(ECollisionEnabled::NoCollision);
		for (UDynamicMeshComponent* ChunkMesh : ChunkMeshComponents)
		{
			if (ChunkMesh)
			{
				ChunkMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
		}
	}
	else // NM_Client
	{
		// 클라이언트: Pawn 응답만 제거 (레이캐스트 충돌은 유지)
		SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		for (UDynamicMeshComponent* ChunkMesh : ChunkMeshComponents)
		{
			if (ChunkMesh)
			{
				ChunkMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] After NoCollision: Main CollisionEnabled=%d, HasPhysics=%d"),
		(int32)GetCollisionEnabled(), IsPhysicsStateCreated() ? 1 : 0);
	for (int32 i = 0; i < ChunkMeshComponents.Num(); ++i)
	{
		if (ChunkMeshComponents[i])
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] ChunkMesh[%d] CollisionEnabled=%d, HasPhysics=%d"),
				i, (int32)ChunkMeshComponents[i]->GetCollisionEnabled(),
				ChunkMeshComponents[i]->IsPhysicsStateCreated() ? 1 : 0);
		}
	}

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
		TotalChunks, NonEmptyChunks, GridCellLayout.GetValidCellCount(), TotalSurfaceCells);
}

void URealtimeDestructibleMeshComponent::BuildCollisionChunkBodySetup(int32 ChunkIndex)
{
	if (!CollisionChunks.IsValidIndex(ChunkIndex))
	{
		return;
	}

	// GridCellLayout 유효성 검사
	if (!GridCellLayout.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerCellCollision] GridCellLayout invalid, skipping chunk %d"), ChunkIndex);
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

		// 새 컴포넌트 생성 (고정 이름으로 서버/클라이언트 네트워크 경로 일치)
		const FName CompName = *FString::Printf(TEXT("CellBoxCollision_%d"), ChunkIndex);
		ChunkComp = NewObject<UStaticMeshComponent>(Owner, CompName, RF_Transient);
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
		ChunkComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		ChunkComp->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);  // Debris와 충돌
		ChunkComp->SetCanEverAffectNavigation(false);  // NavMesh 영향 없음
		ChunkComp->SetIsReplicated(true);  // 네트워크 MovementBase 참조 지원

		// BodyInstance 사전 설정
		FBodyInstance* BI = ChunkComp->GetBodyInstance();
		if (BI)
		{
			BI->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			BI->SetObjectType(ECC_WorldStatic);
			BI->SetResponseToAllChannels(ECR_Ignore);
			BI->SetResponseToChannel(ECC_Pawn, ECR_Block);
			BI->SetResponseToChannel(ECC_PhysicsBody, ECR_Block);  // Debris와 충돌
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

		const FVector LocalCenter = GridCellLayout.IdToLocalCenter(CellId);

		// 로컬 스페이스 셀 크기 사용 (GridCellSize는 월드 스페이스이므로 사용하면 안됨)
		const FVector& LocalCellSize = GridCellLayout.CellSize;

		FKBoxElem BoxElem;
		BoxElem.Center = LocalCenter;
		BoxElem.X = LocalCellSize.X;
		BoxElem.Y = LocalCellSize.Y;
		BoxElem.Z = LocalCellSize.Z;
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

	// 5. BoxElems는 Analytic Shape라 쿠킹 불필요 - CreatePhysicsMeshes() 스킵
	// BoxElems, SphereElems, CapsuleElems 등 기본 Shape는 Physics에서 직접 처리
	Chunk.BodySetup->bCreatedPhysicsMeshes = true;  // 쿠킹 완료 플래그 강제 설정

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
		ChunkBodyInstance->SetResponseToChannel(ECC_PhysicsBody, ECR_Block);  // Debris와 충돌

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

		// 물리 필터 데이터 강제 업데이트 (InitBody로 생성된 바디 유지)
		if (bHasPhysicsBody)
		{
			ChunkBodyInstance->UpdatePhysicsFilterData();
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
	const FIntArray& Neighbors = GridCellLayout.GetCellNeighbors(CellId);

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
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Collision_UpdateDirtyChunks);
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

bool URealtimeDestructibleMeshComponent::RemoveTrianglesForDetachedCells(const TArray<int32>& DetachedCellIds, ADebrisActor* TargetDebrisActor, TArray<int32>* OutToolMeshOverlappingCellIds)
{
		TRACE_CPUPROFILER_EVENT_SCOPE(Debris_RemoveTrianglesForDetachedCells);
	using namespace UE::Geometry;

	if (DetachedCellIds.Num() == 0)
	{
		return false;
	}
	if (ChunkMeshComponents.Num() == 0 && !OutToolMeshOverlappingCellIds)
	{
		return false;
	}
	UE_LOG(LogTemp, Warning, TEXT("=== RemoveTrianglesForDetachedCells START (TargetDebrisActor=%p) ==="), TargetDebrisActor);
	UE_LOG(LogTemp, Warning, TEXT("DetachedCellIds.Num()=%d, ChunkMeshComponents.Num()=%d"),
		DetachedCellIds.Num(), ChunkMeshComponents.Num());

	const FVector CellSizeVec = GridCellLayout.CellSize;

	// 파편 정리용 초기화
	LastOccupiedCells.Empty();
	LastCellSizeVec = CellSizeVec;

	// 1. 모든 분리된 셀들의 3D 점유 맵 생성
	TSet<FIntVector> BaseCells;
	for (int32 CellId : DetachedCellIds)
	{
		FIntVector GridPos = GridCellLayout.IdToCoord(CellId);   
		BaseCells.Add(GridPos);
	}
	

	TArray<TArray<FIntVector>> FinalPieces;

	// TargetDebrisActor가 있으면 분할 없이 단일 조각으로 처리 (서버에서 이미 분할 결정됨)
	if (TargetDebrisActor || DebrisSplitCount <= 1 || BaseCells.Num() <= 1)
	{
		FinalPieces.Add(BaseCells.Array());
	}
	else
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Split);
		struct FPieceRange
		{
			int32 Start;
			int32 End;
			int32 Num() const { return End - Start; }
		};
		// TSet -> TArray 변환 (분할 작업용)
		TArray<FIntVector> AllCells = BaseCells.Array();

		// 각 조각의 [Start, End) 범위를 저장

		TArray<FPieceRange> Ranges;
		Ranges.Add({ 0, AllCells.Num() });

		while (Ranges.Num() < DebrisSplitCount)
		{
			// 가장 큰 조각 찾기 
			int32 LargestIdx = 0;
			for (int32 i = 1; i < Ranges.Num(); i++)
			{
				if (Ranges[i].Num() > Ranges[LargestIdx].Num())
				{
					LargestIdx = i;
				}
			}

			if (Ranges[LargestIdx].Num() <= 1)
			{
				break;
			}

			FPieceRange& Range = Ranges[LargestIdx];

			// 바운딩박스 계산 + 가장 긴 축 찾기
			FIntVector MinBB(TNumericLimits<int32>::Max());
			FIntVector MaxBB(TNumericLimits<int32>::Lowest());
			for (int32 i = Range.Start; i < Range.End; i++)
			{
				MinBB.X = FMath::Min(MinBB.X, AllCells[i].X);
				MinBB.Y = FMath::Min(MinBB.Y, AllCells[i].Y);
				MinBB.Z = FMath::Min(MinBB.Z, AllCells[i].Z);
				MaxBB.X = FMath::Max(MaxBB.X, AllCells[i].X);
				MaxBB.Y = FMath::Max(MaxBB.Y, AllCells[i].Y);
				MaxBB.Z = FMath::Max(MaxBB.Z, AllCells[i].Z);
			}

			int32 ExtX = MaxBB.X - MinBB.X;
			int32 ExtY = MaxBB.Y - MinBB.Y;
			int32 ExtZ = MaxBB.Z - MinBB.Z;
			int32 SplitAxis = (ExtX >= ExtY && ExtX >= ExtZ) ? 0 : (ExtY >= ExtZ ? 1 : 2);


			int32 MidIdx = Range.Start + Range.Num() / 2;
			auto GetAxisValue = [SplitAxis](const FIntVector& V) -> int32
				{
					return SplitAxis == 0 ? V.X : (SplitAxis == 1 ? V.Y : V.Z);
				};

			// 가장 긴 축의 값들 기준으로 정렬 
			TArrayView<FIntVector> SortView(&AllCells[Range.Start], Range.Num());
			SortView.Sort([&](const FIntVector& A, const FIntVector& B)
				{    
						return GetAxisValue(A) < GetAxisValue(B);
				});




			// 한쪽이 비면 중단
			if (MidIdx == Range.Start || MidIdx == Range.End)
			{
				break;
			}

			// Range를 둘로 분할 (메모리 할당 없음, 인덱스만 변경)
			int32 OldEnd = Range.End;
			Range.End = MidIdx;
			Ranges.Add({ MidIdx, OldEnd });
		}

		// 각 범위를 TArray로 복사 (해시 비용 없음)
		for (const FPieceRange& Range : Ranges)
		{
			TArray<FIntVector> PieceArr(&AllCells[Range.Start], Range.Num());
			FinalPieces.Add(MoveTemp(PieceArr));
		}
	}


	// 3. 각 조각별로 ToolMesh 생성 + Enqueue
	UE_LOG(LogTemp, Warning, TEXT("Final Piceses : %d"), FinalPieces.Num());

	// 이진탐색용 비교 함수
	auto VoxelLess = [](const FIntVector& A, const FIntVector& B)
		{
			if (A.Z != B.Z) return A.Z < B.Z;
			if (A.Y != B.Y) return A.Y < B.Y;
			return A.X < B.X;
		};

	for (TArray<FIntVector>& Piece : FinalPieces)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Debris_FinalPieces);

		if (Piece.Num() == 0)
		{
			continue;
		}

		UE_LOG(LogTemp, Warning, TEXT("Piece Size: %d"), Piece.Num());

		// Piece를 정렬 (이진탐색용)
		Piece.Sort(VoxelLess);

		// ToolMesh 빌드 (GreedyMesh + FillHoles + Smoothing)
		FDynamicMesh3 ToolMesh = BuildSmoothedToolMesh(Piece);

		if (ToolMesh.TriangleCount() == 0)
		{
			continue;
		}

		FDynamicMesh3 DebrisToolMesh;
		DebrisToolMesh.EnableAttributes();
		DebrisToolMesh.EnableTriangleGroups();
		DebrisToolMesh = ToolMesh;


		// Subtract용만 Scaling 
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Scaling);

			FVector3d Centroid = FVector3d::Zero();
			int32 VertexCount = 0;
			for (int32 Vid : ToolMesh.VertexIndicesItr())
			{
				Centroid += ToolMesh.GetVertex(Vid);
				VertexCount++;
			}
			if (VertexCount > 0)
			{
				Centroid /= (double)VertexCount;
			}
			for (int32 Vid : ToolMesh.VertexIndicesItr())
			{
				FVector3d Pos = ToolMesh.GetVertex(Vid);

				ToolMesh.SetVertex(Vid, Centroid + (Pos - Centroid) * DebrisExpandRatio);
				DebrisToolMesh.SetVertex(Vid, Centroid + (Pos - Centroid) * DebrisScaleRatio);
			}
		}

		// ToolMesh(smoothed + DebrisExpandRatio) 삼각형과 겹치는 grid cell 수집
		if (OutToolMeshOverlappingCellIds)
		{
			CollectCellsOverlappingMesh(ToolMesh, *OutToolMeshOverlappingCellIds);
		}

		// Smoothing 
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Smooth);
			ApplyHCLaplacianSmoothing(DebrisToolMesh);
		}

		ToolMesh.ReverseOrientation();
		DebrisToolMesh.ReverseOrientation();

		// Debug 그리기
		if (bDebugMeshIslandRemoval)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Debris_DebugMeshIslandRemoval);
			if (UWorld* DebugWorld = GetWorld())
			{
				FTransform ComponentTransform = GetComponentTransform();
				FDynamicMesh3 DebugMesh = ToolMesh;
				DebugMesh.ReverseOrientation(); // 시각화용 원래 방향

				for (int32 TriId : DebugMesh.TriangleIndicesItr())
				{
					FIndex3i Tri = DebugMesh.GetTriangle(TriId);
					FVector V0 = ComponentTransform.TransformPosition(FVector(DebugMesh.GetVertex(Tri.A)));
					FVector V1 = ComponentTransform.TransformPosition(FVector(DebugMesh.GetVertex(Tri.B)));
					FVector V2 = ComponentTransform.TransformPosition(FVector(DebugMesh.GetVertex(Tri.C)));
					DrawDebugLine(DebugWorld, V0, V1, FColor::Yellow, false, 4.5f, 0, 1.0f);
					DrawDebugLine(DebugWorld, V1, V2, FColor::Yellow, false, 4.5f, 0, 1.0f);
					DrawDebugLine(DebugWorld, V2, V0, FColor::Yellow, false, 4.5f, 0, 1.0f);
				}
			}
		} 

		// Detect chunks to be subtracted
		TSharedPtr<FDynamicMesh3> SharedToolMesh = MakeShared<FDynamicMesh3>(MoveTemp(ToolMesh));
		TSharedPtr<FDynamicMesh3> SharedDebrisToolMesh = MakeShared<FDynamicMesh3>(MoveTemp(DebrisToolMesh));

		FAxisAlignedBox3d ToolBounds = SharedToolMesh->GetBounds();
		 
		TArray<int32> OverlappingChunks;

		for (int32 i = 0; i < GetChunkNum(); i++)
		{
			if (ChunkMeshComponents[i] && ChunkMeshComponents[i]->GetMesh())
			{ 
				if (ChunkMeshComponents[i]->GetMesh()->GetBounds().Intersects(ToolBounds))
	  			{
					OverlappingChunks.Add(i);
				} 
			}
		}
		 
		UE_LOG(LogTemp, Warning, TEXT("Piece %d/%d: CellCount=%d, OverlappingChunks=%d, ChunkIndices=[%s]"),
			FinalPieces.IndexOfByKey(Piece),
			FinalPieces.Num(),
			Piece.Num(),
			OverlappingChunks.Num(),
			*FString::JoinBy(OverlappingChunks, TEXT(","), [](int32 x) { return FString::FromInt(x); }));

		if (OverlappingChunks.Num() == 0)
		{
			continue;
		}

		TSharedPtr<FIslandRemovalContext> Context = nullptr;

		//if (Piece.Num() >= MinCellsForDebris)
		{
			Context = MakeShared<FIslandRemovalContext>();
			Context->Owner = this;
			Context->RemainingTaskCount = OverlappingChunks.Num();

			// TargetDebrisActor가 있으면 설정 (클라이언트에서 기존 DebrisActor에 메시 적용)
			if (TargetDebrisActor)
			{
				Context->TargetDebrisActor = TargetDebrisActor;
			}

			// Cleanup용 분리된 셀 저장 (모든 작업 완료 시 사용)
			Context->DisconnectedCellsForCleanup.Append(DetachedCellIds);

			// 활성 IslandRemoval 카운터 증가 (Boolean 배치 완료 시 Cleanup 스킵 판단용)
			IncrementIslandRemovalCount();
		}

		if (BooleanProcessor.IsValid())
		{
			for (int32 ChunkIndex : OverlappingChunks)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Debris_EnqueueIslandRemoval);
				//BooleanProcessor->EnqueueIslandRemoval(ChunkIndex, SharedToolMesh, Context);

				UE_LOG(LogTemp, Warning, TEXT("EnqueueIslandRemoval: Piece=%d, ChunkIndex=%d, ToolMesh Tris=%d, Context=%p"),
					FinalPieces.IndexOfByKey(Piece),
					ChunkIndex,
					SharedToolMesh->TriangleCount(),
					Context.Get());


				BooleanProcessor->EnqueueIslandRemoval(ChunkIndex, SharedToolMesh, SharedDebrisToolMesh, Context);
			}
		} 
	}

	return true;

	
}
FDynamicMesh3 URealtimeDestructibleMeshComponent::BuildSmoothedToolMesh(TArray<FIntVector>& SortedPiece)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_BuildSmoothedToolMesh);
	using namespace UE::Geometry;

	const FVector CellSizeVec = GridCellLayout.CellSize;
	const double BoxExpand = 1.0f;

	FDynamicMesh3 ToolMesh = GenerateGreedyMeshFromVoxels(SortedPiece, GridCellLayout.GridOrigin, CellSizeVec, BoxExpand);

	if (ToolMesh.TriangleCount() == 0)
	{
		return ToolMesh;
	}

	// FillHoles
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Debris_FillHoles);
		FMeshBoundaryLoops BoundaryLoops(&ToolMesh);
		for (const FEdgeLoop& Loop : BoundaryLoops.Loops)
		{
			FSimpleHoleFiller Filler(&ToolMesh, Loop);
			Filler.Fill();
		}
	}

	// HC Laplacian Smoothing
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Smooth);
		ApplyHCLaplacianSmoothing(ToolMesh);
	}

	return ToolMesh;
}

void URealtimeDestructibleMeshComponent::CollectCellsOverlappingMesh(const FDynamicMesh3& Mesh, TArray<int32>& OutCellIds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_CollectCellsOverlappingMesh);
	using namespace UE::Geometry;

	const FVector& Origin = GridCellLayout.GridOrigin;
	const FVector& CS = GridCellLayout.CellSize;
	const FVector InvCS(1.0 / CS.X, 1.0 / CS.Y, 1.0 / CS.Z);

	for (int32 TriId : Mesh.TriangleIndicesItr())
	{
		FIndex3i Tri = Mesh.GetTriangle(TriId);
		FVector3d V0 = Mesh.GetVertex(Tri.A);
		FVector3d V1 = Mesh.GetVertex(Tri.B);
		FVector3d V2 = Mesh.GetVertex(Tri.C);

		// 삼각형 AABB
		FVector3d TriMin = FVector3d::Min(FVector3d::Min(V0, V1), V2);
		FVector3d TriMax = FVector3d::Max(FVector3d::Max(V0, V1), V2);

		// grid 좌표 범위로 변환
		const int32 CMinX = FMath::Max(0, FMath::FloorToInt32((TriMin.X - Origin.X) * InvCS.X));
		const int32 CMinY = FMath::Max(0, FMath::FloorToInt32((TriMin.Y - Origin.Y) * InvCS.Y));
		const int32 CMinZ = FMath::Max(0, FMath::FloorToInt32((TriMin.Z - Origin.Z) * InvCS.Z));
		const int32 CMaxX = FMath::Min(GridCellLayout.GridSize.X - 1, FMath::FloorToInt32((TriMax.X - Origin.X) * InvCS.X));
		const int32 CMaxY = FMath::Min(GridCellLayout.GridSize.Y - 1, FMath::FloorToInt32((TriMax.Y - Origin.Y) * InvCS.Y));
		const int32 CMaxZ = FMath::Min(GridCellLayout.GridSize.Z - 1, FMath::FloorToInt32((TriMax.Z - Origin.Z) * InvCS.Z));

		for (int32 Z = CMinZ; Z <= CMaxZ; ++Z)
		{
			for (int32 Y = CMinY; Y <= CMaxY; ++Y)
			{
				for (int32 X = CMinX; X <= CMaxX; ++X)
				{
					const int32 CellId = GridCellLayout.CoordToId(X, Y, Z);
					if (GridCellLayout.GetCellExists(CellId) &&
						!CellState.DestroyedCells.Contains(CellId))
					{
						FVector CellMin(Origin.X + X * CS.X, Origin.Y + Y * CS.Y, Origin.Z + Z * CS.Z);
						FVector CellMax = CellMin + FVector(CS);

						if (FGridCellBuilder::TriangleIntersectsAABB(
							FVector(V0), FVector(V1), FVector(V2), CellMin, CellMax))
						{
							OutCellIds.AddUnique(CellId);
						}
					}
				}
			}
		}
	}
}

void URealtimeDestructibleMeshComponent::CollectToolMeshOverlappingCells(const TArray<int32>& CellIds, TArray<int32>& OutOverlappingCellIds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_CollectToolMeshOverlappingCells);
	using namespace UE::Geometry;

	if (CellIds.Num() == 0)
	{
		return;
	}

	// CellIds → 정렬된 voxel 좌표
	TSet<FIntVector> BaseCells;
	for (int32 CellId : CellIds)
	{
		BaseCells.Add(GridCellLayout.IdToCoord(CellId));
	}
	TArray<FIntVector> Piece = BaseCells.Array();
	if (Piece.Num() == 0)
	{
		return;
	}
	auto VoxelLess = [](const FIntVector& A, const FIntVector& B)
		{
			if (A.Z != B.Z) return A.Z < B.Z;
			if (A.Y != B.Y) return A.Y < B.Y;
			return A.X < B.X;
		};
	Piece.Sort(VoxelLess);

	// ToolMesh 빌드 (GreedyMesh + FillHoles + Smoothing)
	FDynamicMesh3 ToolMesh = BuildSmoothedToolMesh(Piece);
	if (ToolMesh.TriangleCount() == 0)
	{
		return;
	}

	// DebrisExpandRatio 스케일링
	{
		FVector3d Centroid = FVector3d::Zero();
		int32 VertexCount = 0;
		for (int32 Vid : ToolMesh.VertexIndicesItr())
		{
			Centroid += ToolMesh.GetVertex(Vid);
			VertexCount++;
		}
		if (VertexCount > 0)
		{
			Centroid /= (double)VertexCount;
		}
		for (int32 Vid : ToolMesh.VertexIndicesItr())
		{
			FVector3d Pos = ToolMesh.GetVertex(Vid);
			ToolMesh.SetVertex(Vid, Centroid + (Pos - Centroid) * DebrisExpandRatio);
		}
	}

	// SAT 교차 검사로 cell 수집
	CollectCellsOverlappingMesh(ToolMesh, OutOverlappingCellIds);
}

FDynamicMesh3 URealtimeDestructibleMeshComponent::GenerateGreedyMeshFromVoxels(const TArray<FIntVector>& InVoxels, FVector InCellOrigin, FVector InCellSize, double InBoxExpand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_GenerateGreedyMeshFromVoxels);

	using namespace UE::Geometry;

	FDynamicMesh3 ResultMesh;
	ResultMesh.EnableTriangleGroups();

	if (InVoxels.Num() == 0)
	{
		return ResultMesh;
	}

	// 정렬된 배열에서 이진탐색으로 Contains 대체
	auto VoxelLess = [](const FIntVector& A, const FIntVector& B)
	{
		if (A.Z != B.Z) return A.Z < B.Z;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.X < B.X;
	};
	auto SortedContains = [&](const FIntVector& Value) -> bool
	{
		const FIntVector* Start = InVoxels.GetData();
		const FIntVector* End = Start + InVoxels.Num();
		const FIntVector* It = std::lower_bound(Start, End, Value, VoxelLess);
		return It != End && *It == Value;
	};
	 
	// 외곽 경계 계산
	FIntVector GridMin(TNumericLimits<int32>::Max());
	FIntVector GridMax(TNumericLimits<int32>::Lowest());
	
	for (const FIntVector& Pos : InVoxels)
	{
		GridMin.X = FMath::Min(GridMin.X, Pos.X);
		GridMin.Y = FMath::Min(GridMin.Y, Pos.Y);
		GridMin.Z = FMath::Min(GridMin.Z, Pos.Z);

		GridMax.X = FMath::Max(GridMax.X, Pos.X + 1);
		GridMax.Y = FMath::Max(GridMax.Y, Pos.Y + 1);
		GridMax.Z = FMath::Max(GridMax.Z, Pos.Z + 1);
	}
	 
	// 정점 캐싱 및 생성 람다
	TMap<FIntVector, int32> CornerToVertexId;
	auto GetOrCreateVertex = [&](const FIntVector& Corner) -> int32 
		{
			if (int32* Existing = CornerToVertexId.Find(Corner))
			{
				return *Existing;
			}

			double ExpX = 0, ExpY = 0, ExpZ = 0;

			// 외곽에 있는 정점일 경우 BoxExpand 만큼 바깥으로 밀어냄
			// 한 칸씩 여유 있게 잡기 위함
			// 외곽에 있는 정점일 경우 BoxExpand만큼 바깥으로 밀어냄
			if (Corner.X == GridMin.X)
			{
				ExpX = -InBoxExpand;
			}
			else if (Corner.X == GridMax.X)
			{
				ExpX = InBoxExpand;
			}

			if (Corner.Y == GridMin.Y)
			{
				ExpY = -InBoxExpand;
			}
			else if (Corner.Y == GridMax.Y)
			{ 
				ExpY = InBoxExpand; 
			}

			if (Corner.Z == GridMin.Z)
			{
				ExpZ = -InBoxExpand;
			}
			else if (Corner.Z == GridMax.Z)
			{
				ExpZ = InBoxExpand;
			}

			FVector3d VertexPos(
				InCellOrigin.X + Corner.X * InCellSize.X + ExpX,
				InCellOrigin.Y + Corner.Y * InCellSize.Y + ExpY,
				InCellOrigin.Z + Corner.Z * InCellSize.Z + ExpZ
			);

			int32 NewId = ResultMesh.AppendVertex(VertexPos);
			CornerToVertexId.Add(Corner, NewId);
			return NewId;
		};

	for (int32 FaceDir = 0; FaceDir < 6; ++FaceDir)
	{
		TSet<FIntVector> ExposedFacesSet;
		FIntVector Normal;

		switch (FaceDir)
		{
		case 0:
			Normal = FIntVector(0, 0, 1);
			break;
		case 1:
			Normal = FIntVector(0, 0, -1);
			break;
		case 2:
			Normal = FIntVector(0, -1, 0);
			break;
		case 3:
			Normal = FIntVector(0, 1, 0);
			break;
		case 4:
			Normal = FIntVector(1, 0, 0);
			break;
		case 5:
			Normal = FIntVector(-1, 0, 0);
			break;
		default:
			break;
		}
		
		// 노출된 면 찾기 
		for (const FIntVector& Pos : InVoxels)
		{
			if (!SortedContains(Pos + Normal))
			{
				ExposedFacesSet.Add(Pos);
			}
		}


		if (ExposedFacesSet.Num() == 0)
		{
			continue;
		}
		
		TArray<FIntVector> SortedFaces = ExposedFacesSet.Array();
		SortedFaces.Sort([](const FIntVector& A, const FIntVector& B) {
			if (A.Z != B.Z) return A.Z < B.Z;
			if (A.Y != B.Y) return A.Y < B.Y;
			return A.X < B.X;
			});

		// Voxel 병합 시작  
		TSet<FIntVector> Processed; 
		for( const FIntVector& Start: SortedFaces)
		{
			if (Processed.Contains(Start))
			{
				continue;
			}

			int32 Width = 1, Height = 1;
		
			int32 WidthAxis, HeightAxis;

			// 0: X Axis
			// 1: Y Axis
			// 2: Z Axis
			if (FaceDir <= 1) // 상하
			{
				WidthAxis = 0;
				HeightAxis = 1;
			}
			else if (FaceDir <= 3) // 좌우
			{
				WidthAxis = 0;
				HeightAxis = 2; 
			}
			else // 전후
			{
				WidthAxis = 1;
				HeightAxis = 2;
			}

			auto GetCoord = [](const FIntVector& V, int32 Axis) 
				{
					return Axis == 0 ? V.X : (Axis == 1 ? V.Y : V.Z); 
				};

			auto SetCoord = [](FIntVector& V, int32 Axis, int Val) 
				{
					if (Axis == 0)
					{
						V.X = Val;
					}
					else if (Axis == 1)
					{
						V.Y = Val;
					}
					else
					{
						V.Z = Val;

					}
					
				};

			// Width 확장
			while (true)
			{
				FIntVector Check = Start;
				SetCoord(Check, WidthAxis, GetCoord(Start, WidthAxis) + Width);

				if (ExposedFacesSet.Contains(Check) && !Processed.Contains(Check))
				{
					Width++;
				}
				else
				{
					break;
				}
			}

			// Height 확장
			while (true)
			{
				bool CanExpand = true;
				for (int32 W = 0; W < Width; ++W)
				{
					FIntVector Check = Start;
					SetCoord( Check, WidthAxis, GetCoord(Start, WidthAxis) + W);
					SetCoord( Check, HeightAxis, GetCoord(Start, HeightAxis) + Height);
					
					// 이미 수집이 되어 있거나, 표면이 아니면 탈락
					if (!ExposedFacesSet.Contains(Check) || Processed.Contains(Check))
					{
						CanExpand = false;
						break;
					}
				}

				if (CanExpand)
				{
					Height++;
				}
				else
				{
					break;
				}
			}

			// 처리된 셀 등록 
			for (int32 H = 0; H < Height; ++H)
			{
				for (int32 W = 0; W < Width; ++W)
				{
					FIntVector Cell = Start;
					SetCoord(Cell, WidthAxis, GetCoord(Start, WidthAxis) + W);
					SetCoord(Cell, HeightAxis, GetCoord(Start, HeightAxis) + H);
					Processed.Add(Cell);
				}
			}

			// 사각형 코너 좌표 계산 
			FIntVector C0 = Start, C1 = Start, C2 = Start, C3 = Start;
			
			// C0: Start, C1: Start + Width, C2: Start + Width + Height, C3: Start+ Height
			SetCoord(C1, WidthAxis, GetCoord(Start, WidthAxis) + Width);
			SetCoord(C2, WidthAxis, GetCoord(Start, WidthAxis) + Width);
			SetCoord(C2, HeightAxis, GetCoord(Start, HeightAxis) + Height);
			SetCoord(C3, HeightAxis, GetCoord(Start, HeightAxis) + Height);
			
			// 양의 방향은 해당 축으로 +1만큼 이동시켜야지 큐브의 바깥면이 되고
			switch (FaceDir)
			{
			case 0: // Z+
				C0.Z++; C1.Z++; C2.Z++; C3.Z++;
				break;
			case 3: // Y+
				C0.Y++; C1.Y++; C2.Y++; C3.Y++;
				break;
			case 4: // X+
				C0.X++; C1.X++; C2.X++; C3.X++;
				break;
			}

			// 정점 생성 및 삼각형 추가 
			int32 I0 = GetOrCreateVertex(C0);
			int32 I1 = GetOrCreateVertex(C1);
			int32 I2 = GetOrCreateVertex(C2);
			int32 I3 = GetOrCreateVertex(C3);
	
			// 바깥면의 방향에 따라 normal 방향이 다르기 때문에 winding을 다르게 해줘야한다.
			bool bIsPositiveDir = (FaceDir == 0 || FaceDir == 2 || FaceDir == 4);
			if (bIsPositiveDir)
			{  
				ResultMesh.AppendTriangle(I0, I1, I2);
				ResultMesh.AppendTriangle(I0, I2, I3);
			}
			else
			{
				ResultMesh.AppendTriangle(I0, I2, I1);
				ResultMesh.AppendTriangle(I0, I3, I2);
			}
		}
	} 
	return ResultMesh; 
}

void URealtimeDestructibleMeshComponent::SpawnDebrisActor(FDynamicMesh3&& Source, const TArray<UMaterialInterface*>& Materials, ADebrisActor* TargetActor)
{
	using namespace UE::Geometry;

	// =========================================================================
	// SpawnDebrisActor: 분리된 메시 조각을 물리 시뮬레이션되는 Debris Actor로 스폰
	// =========================================================================
	// - RemoveTrianglesForDetachedCells에서 계산된 RemovedMeshIsland를 사용
	// - RemovedMeshIsland = OriginalMesh ∩ ToolMesh (원본에서 실제로 잘려나간 부분)
	// - 다중 머티리얼 지원: Triangle Group(MaterialID)별로 메시 섹션 분리
	// =========================================================================

	// -------------------------------------------------------------------------
	// 1. 유효성 검사
	// -------------------------------------------------------------------------

	// 데디케이티드 서버에서는 렌더링/물리 처리 스킵 (시각적 요소 불필요)
	//if (IsRunningDedicatedServer())
	//{
	//	return;
	//}

		// Dedicated Server 체크
	const bool bIsDedicatedServer = GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer;

	// 빈 메시 체크
	if (Source.TriangleCount() == 0 || Source.VertexCount() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnDebrisActor: Empty mesh, skipping"));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// -------------------------------------------------------------------------
	// 2. 메시 바운드 및 스폰 위치 계산
	// -------------------------------------------------------------------------
	// - 메시의 중심점(Centroid)을 기준으로 액터를 스폰
	// - 정점 좌표는 이 중심점 기준 상대 좌표로 변환하여 저장

	UE_LOG(LogTemp, Warning, TEXT("=== SpawnDebrisActor START === TriCount=%d, VertCount=%d"),
		Source.TriangleCount(),
		Source.VertexCount());

	FAxisAlignedBox3d MeshBounds = Source.GetBounds();
	FVector3d MeshCenter = MeshBounds.Center();

	// 컴포넌트의 월드 트랜스폼 (로컬 → 월드 변환용)
	FTransform ComponentTransform = GetComponentTransform();

	// 메시 중심점을 월드 좌표로 변환 → 이 위치에 액터 스폰
	FVector SpawnLocation = ComponentTransform.TransformPosition(FVector(MeshCenter));

	// -------------------------------------------------------------------------
	// 3. 물리 사용 가능 여부 판정
	// -------------------------------------------------------------------------
	// - 너무 작거나 납작한 메시는 Convex Collision 생성 시 문제 발생 가능
	// - 최소 크기 조건을 만족하지 않으면 물리 비활성화

	FVector BoundsSize = FVector(MeshBounds.Extents()) * 2.0f;
	float DebrisSize = BoundsSize.Size();
	float MinAxisSize = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);

	// 물리 활성화 조건:
	// - 정점 12개 이상 (안정적인 Convex Hull 생성)
	// - 전체 크기 5cm 이상
	// - 각 축 최소 2cm 이상 (납작한 형태 방지)
	bool bCanUsePhysics = Source.VertexCount() >= 12
		&& DebrisSize >= 5.0f
		&& MinAxisSize >= 2.0f;

	// -------------------------------------------------------------------------
	// 4. Triangle Group(MaterialID)별 삼각형 분류
	// -------------------------------------------------------------------------
	// - FDynamicMesh3의 Triangle Group = 머티리얼 슬롯 인덱스
	// - 같은 머티리얼을 사용하는 삼각형끼리 그룹화
	// - 각 그룹은 ProceduralMeshComponent의 별도 Section이 됨

	// MaterialID → 해당 머티리얼을 사용하는 삼각형 ID 목록
	TMap<int32, TArray<int32>> TrianglesByMaterial;

	// Triangle Group 속성이 활성화되어 있는지 확인
	bool bHasTriangleGroups = Source.HasTriangleGroups();

	const FDynamicMeshMaterialAttribute* MatAttr = nullptr;
	if (Source.HasAttributes())
	{
		MatAttr = Source.Attributes()->GetMaterialID();
	}

	for (int32 TriId : Source.TriangleIndicesItr())
	{
		// Triangle Group이 있으면 해당 그룹 ID 사용, 없으면 0번 머티리얼로 통일
		// int32 MaterialId = bHasTriangleGroups ? Source.GetTriangleGroup(TriId) : 0;
		// TrianglesByMaterial.FindOrAdd(MaterialId).Add(TriId);
		int32 MaterialId = 0;
		if (MatAttr)
		{
			MaterialId = MatAttr->GetValue(TriId);
		}
		else if (bHasTriangleGroups)
		{
			MaterialId = Source.GetTriangleGroup(TriId);
		}
		TrianglesByMaterial.FindOrAdd(MaterialId).Add(TriId);
	}

	// -------------------------------------------------------------------------
	// 5. 각 머티리얼 그룹별 정점/삼각형 데이터 추출
	// -------------------------------------------------------------------------
	// - ProceduralMeshComponent는 Section별로 정점 배열이 독립적
	// - 따라서 각 그룹별로 정점을 재매핑해야 함
	// - 정점 좌표는 스폰 위치(MeshCenter) 기준 상대 좌표로 변환
	
	TMap<int32, FMeshSectionData> SectionDataByMaterial;

	// 노말 속성 확인
	const FDynamicMeshNormalOverlay* NormalOverlay = nullptr;
	if (Source.HasAttributes())
	{
		NormalOverlay = Source.Attributes()->PrimaryNormals();
	}

	// UV 속성 확인
	const FDynamicMeshUVOverlay* UVOverlay = nullptr;
	if (Source.HasAttributes() && Source.Attributes()->NumUVLayers() > 0)
	{
		UVOverlay = Source.Attributes()->GetUVLayer(0);
	}

	for (const auto& Pair : TrianglesByMaterial)
	{
		int32 MaterialId = Pair.Key;
		const TArray<int32>& TriangleIds = Pair.Value;

		FMeshSectionData& SectionData = SectionDataByMaterial.FindOrAdd(MaterialId);

		for (int32 TriId : TriangleIds)
		{
			FIndex3i Triangle = Source.GetTriangle(TriId);
			
			FIndex3i NormalTri = NormalOverlay ? NormalOverlay->GetTriangle(TriId) : FIndex3i(-1, -1, -1);
			FIndex3i UVTri = UVOverlay ? UVOverlay->GetTriangle(TriId) : FIndex3i(-1, -1, -1);
			
			int32 NewTriIndices[3];

			for (int32 i = 0; i < 3; ++i)
			{
				int32 OrigVertId = Triangle[i];

				int32 NormalElem = NormalOverlay ? NormalTri[i] : -1;
				int32 UVElem = UVOverlay ? UVTri[i] : -1;

				FVertexKey Key{OrigVertId, NormalElem, UVElem};

				// 이미 매핑된 정점이면 재사용
				if (int32* ExistingIdx = SectionData.VertexRemap.Find(Key))
				{
					NewTriIndices[i] = *ExistingIdx;
				}
				else
				{
					// 새 정점 추가
					int32 NewIdx = SectionData.Vertices.Num();
					SectionData.VertexRemap.Add(Key, NewIdx);

					// 정점 위치: 메시 중심 기준 상대 좌표로 변환
					FVector3d LocalPos = Source.GetVertex(OrigVertId);
					SectionData.Vertices.Add(FVector(LocalPos - MeshCenter));

					// 노말 (없으면 기본값)
					if (NormalOverlay && NormalElem >= 0)
					{
						FVector3f Normal = NormalOverlay->GetElement(NormalElem);
						SectionData.Normals.Add(FVector(Normal));
					}
					else
					{
						SectionData.Normals.Add(FVector::UpVector);
					}

					if (UVOverlay && UVElem >= 0)
					{
						FVector2f UV = UVOverlay->GetElement(UVElem);
						SectionData.UVs.Add(FVector2D(UV));
					}
					else
					{
					// UV (없으면 0,0)
					SectionData.UVs.Add(FVector2D::ZeroVector);
					}					

					NewTriIndices[i] = NewIdx;
				}
			}

			// 삼각형 인덱스 추가 (와인딩 순서 유지)
			SectionData.Triangles.Add(NewTriIndices[0]);
			SectionData.Triangles.Add(NewTriIndices[1]);
			SectionData.Triangles.Add(NewTriIndices[2]);
		}
	}

	// 유효한 섹션이 없으면 리턴
	if (SectionDataByMaterial.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnDebrisActor: No valid mesh sections"));
		return;
	}

	// -------------------------------------------------------------------------
	// 6. Debris ID 부여 (서버/클라이언트 동일하게 증가 - Deterministic)
	// -------------------------------------------------------------------------
	// - 파괴 시스템이 Deterministic이므로 서버와 클라이언트의 호출 순서가 동일
	// - 따라서 NextDebrisId를 동일하게 증가시키면 같은 ID가 부여됨


	// 7. 서버/클라이언트 분기 처리
	const bool bIsServer = GetOwner() && GetOwner()->HasAuthority();

	// Bounding Box 먼저 계산 (동기화 여부 결정용)
	FBox DebrisBounds(ForceInit);
	for (const auto& Pair : SectionDataByMaterial)
	{
		for (const FVector& Vert : Pair.Value.Vertices)
		{
			DebrisBounds += Vert;
		}
	}
	FVector BoxExtent = DebrisBounds.GetExtent(); 
	//BoxExtent *= GetComponentTransform().GetScale3D();
	BoxExtent = BoxExtent.ComponentMax(FVector(1.0f, 1.0f, 1.0f));

	const bool bShouldSync = BoxExtent.GetMax() >= MinDebrisSyncSize; 

	// Target Actor가 있으면 Client 전용
	if (TargetActor)
	{
		TargetActor->SetActorLocation(SpawnLocation);
		CreateDebrisMeshSections(TargetActor->DebrisMesh, SectionDataByMaterial, Materials);
		TargetActor->SetCollisionBoxExtent(BoxExtent);

		// physics는 서버에서 딸려옴 
		return;
	}

	if (bIsServer && bShouldSync)
	{
		const int32 DebrisId = NextDebrisId++;

		// Server: Spawn Debris Actor ( 클라이언트로 자동 복제 ) 
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; // 위치가 뭐가 있어도 일단 생성하자

		ADebrisActor* DebrisActor = World->SpawnActor<ADebrisActor>(
		ADebrisActor::StaticClass(),
		SpawnLocation,
		ComponentTransform.GetRotation().Rotator(),
		SpawnParams
		);

		if (!DebrisActor)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] Failed To Spawn ADebrisActor"));
			return;
		}

		// 스케일 설정
		DebrisActor->SetActorScale3D(ComponentTransform.GetScale3D());

		// Material;
		UMaterialInterface* DebrisMaterial = Materials.Num() > 0 ? Materials[0] : nullptr;

		// Init Debris Actor
		DebrisActor->InitializeDebris(DebrisId, TArray<int32>() , INDEX_NONE, this, DebrisMaterial);


		if (!bIsDedicatedServer)
		{
			CreateDebrisMeshSections(DebrisActor->DebrisMesh, SectionDataByMaterial, Materials);
		}
		DebrisActor->SetCollisionBoxExtent(BoxExtent);
		DebrisActor->EnablePhysics();
		ApplyDebrisPhysics(DebrisActor->CollisionBox, SpawnLocation, BoxExtent);

		ActiveDebrisActors.Add(DebrisId, DebrisActor);

		if (bDebugDrawDebris)
		{
			DrawDebugBox(
				GetWorld(),
				SpawnLocation,
				BoxExtent,
				FColor::Green,
				false,
				DebugDrawDuration,
				0,
				2.0f
			);
		}
		UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] Server: SpawnDebrisActor: ADebrisActor spawned, DebrisId=%d"), DebrisId);
	}
	else if (!bIsServer && bShouldSync)
	{
		// ========================================================================
		// 클라이언트 + 큰 조각: 서버 타입에 따라 분기
		// ========================================================================

		if (bServerIsDedicatedServer)
		{
			// ----------------------------------------------------------------
			// 데디서버 클라이언트: 로컬 메시 생성 스킵
			// ----------------------------------------------------------------
			// 서버에서 복제된 ADebrisActor의 OnRep_DebrisParams에서
			// CellIds 기반으로 GenerateMeshFromCells() 호출하여 메시 생성
			// DebrisId도 생성하지 않음 (서버 Actor의 ID 사용)

			UE_LOG(LogTemp, Log, TEXT("[Client-Dedicated] SpawnDebrisActor: Skipping local mesh - will use CellIds from replicated ADebrisActor"));
		}
		else
		{
			// ----------------------------------------------------------------
			// Listen 서버 클라이언트: 기존 로컬 메시 생성 및 등록
			// ----------------------------------------------------------------
			// 서버와 동일한 SpawnDebrisActor 경로이므로 DebrisId 동기화됨

			const int32 DebrisId = NextDebrisId++;

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			// 임시 Actor 생성 (메시 보관용)
			AActor* TempActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator, SpawnParams);
			if (!TempActor)
			{
				return;
			}

			UProceduralMeshComponent* LocalMesh = NewObject<UProceduralMeshComponent>(
				TempActor, UProceduralMeshComponent::StaticClass(), TEXT("LocalDebrisMesh"));
			LocalMesh->SetMobility(EComponentMobility::Movable);

			CreateDebrisMeshSections(LocalMesh, SectionDataByMaterial, Materials);

			TempActor->SetRootComponent(LocalMesh);
			LocalMesh->RegisterComponent();
			TempActor->AddInstanceComponent(LocalMesh);

			// Transform 설정
			TempActor->SetActorLocation(SpawnLocation);
			TempActor->SetActorRotation(ComponentTransform.GetRotation());
			TempActor->SetActorScale3D(ComponentTransform.GetScale3D());

			// 로컬 메시 등록 (ADebrisActor 도착 시 매칭용)
			RegisterLocalDebris(DebrisId, LocalMesh);

			// 임시 Actor는 일단 숨김 (ADebrisActor 매칭 후 삭제 예정)
			TempActor->SetActorHiddenInGame(true);

			if (bDebugDrawDebris)
			{
				DrawDebugBox(
					GetWorld(),
					SpawnLocation,
					BoxExtent,
					FColor::Green,
					false,
					DebugDrawDuration,
					0,
					2.0f
				);
			}
			UE_LOG(LogTemp, Warning, TEXT("[Client-Listen] SpawnDebrisActor: Local mesh registered, DebrisId=%d"), DebrisId);
		}
	}
	else
	{
		// 로컬 전용 
		if (!bIsDedicatedServer)
		{
			CreateLocalOnlyDebrisActor(World, SpawnLocation, BoxExtent, SectionDataByMaterial, Materials);
			UE_LOG(LogTemp, Log, TEXT("[Debris] Local-only debris (no sync) - Size=%f"), BoxExtent.GetMax());

			if (bDebugDrawDebris)
			{
				FVector BoxCenter = SpawnLocation;

				DrawDebugBox(
					GetWorld(),
					BoxCenter,
					BoxExtent,
					FColor::Red,
					false,              // bPersistent
					DebugDrawDuration,  // LifeTime
					0,                  // DepthPriority
					2.0f                // Thickness
				);
			}
		}
	}
	
	// -------------------------------------------------------------------------
	// 11. 수명 설정 및 로그
	// -------------------------------------------------------------------------
	
	// 10초 후 자동 삭제 (메모리 관리)
	//DebrisActor->SetLifeSpan(10.0f); 
}

void URealtimeDestructibleMeshComponent::SpawnDebrisActorForDedicatedServer(const TArray<int32>& DetachedCellIds)
{
	if (DetachedCellIds.Num() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 서버 권한 체크
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	FTransform ComponentTransform = GetComponentTransform();
	const FVector CellSizeVec = GridCellLayout.CellSize;

	// =========================================================================
	// 1. CellIds를 Grid 좌표로 변환
	// =========================================================================
	TSet<FIntVector> BaseCells;
	for (int32 CellId : DetachedCellIds)
	{
		//const FVector LocalMin = GridCellLayout.IdToLocalMin(CellId);
		//const FVector Origin = GridCellLayout.GridOrigin;
		//
		//FIntVector GridPos(
		//	FMath::FloorToInt((LocalMin.X - Origin.X) / CellSizeVec.X),
		//	FMath::FloorToInt((LocalMin.Y - Origin.Y) / CellSizeVec.Y),
		//	FMath::FloorToInt((LocalMin.Z - Origin.Z) / CellSizeVec.Z)
		//);
		//BaseCells.Add(GridPos);
		//

		FIntVector GridPos = GridCellLayout.IdToCoord(CellId);
		BaseCells.Add(GridPos);
	}

	// =========================================================================
	// 2. Split 로직 (DebrisSplitCount 기반)
	// =========================================================================
	TArray<TArray<FIntVector>> FinalPieces;

	if (DebrisSplitCount <= 1 || BaseCells.Num() <= 1)
	{
		FinalPieces.Add(BaseCells.Array());
	}
	else
	{
		struct FPieceRange
		{
			int32 Start;
			int32 End;
			int32 Num() const { return End - Start; }
		};

		TArray<FIntVector> AllCells = BaseCells.Array();
		TArray<FPieceRange> Ranges;
		Ranges.Add({ 0, AllCells.Num() });

		while (Ranges.Num() < DebrisSplitCount)
		{
			// 가장 큰 조각 찾기
			int32 LargestIdx = 0;
			for (int32 i = 1; i < Ranges.Num(); i++)
			{
				if (Ranges[i].Num() > Ranges[LargestIdx].Num())
				{
					LargestIdx = i;
				}
			}

			if (Ranges[LargestIdx].Num() <= 1)
			{
				break;
			}

			FPieceRange& Range = Ranges[LargestIdx];

			// 바운딩박스 계산 + 가장 긴 축 찾기
			FIntVector MinBB(TNumericLimits<int32>::Max());
			FIntVector MaxBB(TNumericLimits<int32>::Lowest());
			for (int32 i = Range.Start; i < Range.End; i++)
			{
				MinBB.X = FMath::Min(MinBB.X, AllCells[i].X);
				MinBB.Y = FMath::Min(MinBB.Y, AllCells[i].Y);
				MinBB.Z = FMath::Min(MinBB.Z, AllCells[i].Z);
				MaxBB.X = FMath::Max(MaxBB.X, AllCells[i].X);
				MaxBB.Y = FMath::Max(MaxBB.Y, AllCells[i].Y);
				MaxBB.Z = FMath::Max(MaxBB.Z, AllCells[i].Z);
			}

			int32 ExtX = MaxBB.X - MinBB.X;
			int32 ExtY = MaxBB.Y - MinBB.Y;
			int32 ExtZ = MaxBB.Z - MinBB.Z;
			int32 SplitAxis = (ExtX >= ExtY && ExtX >= ExtZ) ? 0 : (ExtY >= ExtZ ? 1 : 2);

			int32 MidIdx = Range.Start + Range.Num() / 2;
			auto GetAxisValue = [SplitAxis](const FIntVector& V) -> int32
			{
				return SplitAxis == 0 ? V.X : (SplitAxis == 1 ? V.Y : V.Z);
			};

			// 가장 긴 축 기준 정렬
			TArrayView<FIntVector> SortView(&AllCells[Range.Start], Range.Num());
			SortView.Sort([&](const FIntVector& A, const FIntVector& B)
			{
				return GetAxisValue(A) < GetAxisValue(B);
			});

			if (MidIdx == Range.Start || MidIdx == Range.End)
			{
				break;
			}

			// Range를 둘로 분할
			int32 OldEnd = Range.End;
			Range.End = MidIdx;
			Ranges.Add({ MidIdx, OldEnd });
		}

		// 각 범위를 TArray로 복사
		for (const FPieceRange& Range : Ranges)
		{
			TArray<FIntVector> PieceArr(&AllCells[Range.Start], Range.Num());
			FinalPieces.Add(MoveTemp(PieceArr));
		}
	}

	// =========================================================================
	// 3. 각 조각별로 Debris Actor 스폰
	// =========================================================================
	UMaterialInterface* DebrisMaterial = GetMaterial(0);

	for (const TArray<FIntVector>& Piece : FinalPieces)
	{
		if (Piece.Num() == 0)
		{
			continue;
		}

		// Grid 좌표를 CellId로 변환
		TArray<int32> PieceCellIds;
		PieceCellIds.Reserve(Piece.Num());
		for (const FIntVector& GridPos : Piece)
		{
			if (GridCellLayout.IsValidCoord(GridPos))
			{
				PieceCellIds.Add(GridCellLayout.CoordToId(GridPos));
			}
		}

		if (PieceCellIds.Num() == 0)
		{
			continue;
		}

		// 바운딩박스 계산
		FBox CellBounds(ForceInit);
		for (int32 CellId : PieceCellIds)
		{
			FVector CellMin = GridCellLayout.IdToLocalMin(CellId);
			FVector CellMax = CellMin + GridCellLayout.CellSize;
			CellBounds += CellMin;
			CellBounds += CellMax;
		}

		FVector LocalCenter = CellBounds.GetCenter();
		FVector SpawnLocation = ComponentTransform.TransformPosition(LocalCenter);
		FVector BoxExtent = CellBounds.GetExtent();
		BoxExtent *= DebrisScaleRatio;
		//BoxExtent *= ComponentTransform.GetScale3D();
		BoxExtent = BoxExtent.ComponentMax(FVector(1.0f, 1.0f, 1.0f));
		//BoxExtent = BoxExtent.ComponentMax(FVector(1.0f, 1.0f, 1.0f));

		// 동기화 여부 판정 (작은 조각은 스킵)
		if (BoxExtent.GetMax() < MinDebrisSyncSize)
		{
			UE_LOG(LogTemp, Log, TEXT("[DediServer] Debris piece too small, skipping - Size=%f"), BoxExtent.GetMax());
			continue;
		}

		// Debris ID 생성
		const int32 DebrisId = NextDebrisId++;

		// Debris Actor 스폰
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ADebrisActor* DebrisActor = World->SpawnActor<ADebrisActor>(
			ADebrisActor::StaticClass(),
			SpawnLocation,
			ComponentTransform.GetRotation().Rotator(),
			SpawnParams
		);

		if (!DebrisActor)
		{
			UE_LOG(LogTemp, Warning, TEXT("[DediServer] Failed to spawn ADebrisActor"));
			continue;
		}

		DebrisActor->SetActorScale3D(ComponentTransform.GetScale3D());

		// CellIds 전달 - 클라이언트가 이걸로 메시 생성
		DebrisActor->InitializeDebris(DebrisId, PieceCellIds, INDEX_NONE, this, DebrisMaterial);

		// 콜리전 및 물리 설정
		DebrisActor->SetCollisionBoxExtent(BoxExtent);
		DebrisActor->EnablePhysics();
		ApplyDebrisPhysics(DebrisActor->CollisionBox, SpawnLocation, BoxExtent);

		// 추적 맵에 추가
		ActiveDebrisActors.Add(DebrisId, DebrisActor);

		UE_LOG(LogTemp, Warning, TEXT("[DediServer] SpawnDebrisActorForDedicatedServer: DebrisId=%d, CellCount=%d, Location=%s, Material=%s"),
			DebrisId, PieceCellIds.Num(), *SpawnLocation.ToString(), DebrisMaterial ? *DebrisMaterial->GetName() : TEXT("NULL"));
	}
}

bool URealtimeDestructibleMeshComponent::CanExtractDebrisForClient() const
{
	// BooleanProcessor가 유효한지 확인
	if (!BooleanProcessor.IsValid())
	{
		return false;
	}

	// ChunkMeshComponents에 유효한 메시가 있는지 확인
	for (int32 i = 0; i < ChunkMeshComponents.Num(); i++)
	{
		if (ChunkMeshComponents[i] && ChunkMeshComponents[i]->GetMesh() &&
			ChunkMeshComponents[i]->GetMesh()->TriangleCount() > 0)
		{
			return true;
		}
	}

	return false;
}

void URealtimeDestructibleMeshComponent::RegisterLocalDebris(int32 InDebrisId, UProceduralMeshComponent* Mesh)
{
	if (!Mesh || InDebrisId == INDEX_NONE)
	{
		return;
	}

	// 이미 대기 중인 Actor가 있는 지 확인
	if (TObjectPtr<ADebrisActor>* FoundActor = PendingDebrisActors.Find(InDebrisId))
	{
		// Actor가 먼저 도착해서 대기 중이였으면, 바로 매칭
		UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] Found pending actor for DebrisId=%d, applying mesh now"), InDebrisId);

		ADebrisActor* DebrisActor = *FoundActor;
		PendingDebrisActors.Remove(InDebrisId);

		if (DebrisActor)
		{
			DebrisActor->ApplyLocalMesh(Mesh); 
		}

		// TempActor 정리
		AActor* TempActor = Mesh->GetOwner();
		if (TempActor)
		{
			TempActor->Destroy();
		}
	}
	else
	{
		// 로컬 메쉬 먼저 등록 
		LocalDebrisMeshMap.Add(InDebrisId, Mesh);
		UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] RegisterLocalDebris - DebrisId=%d"), InDebrisId);
	} 
}

void URealtimeDestructibleMeshComponent::RegisterPendingDebrisActor(int32 InDebrisId, ADebrisActor* Actor)
{
	if (Actor && InDebrisId != INDEX_NONE)
	{
		PendingDebrisActors.Add(InDebrisId, Actor);
		UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] Actor registered as pending - DebrisId=%d"), InDebrisId);
	}
}

UProceduralMeshComponent* URealtimeDestructibleMeshComponent::FindAndRemoveLocalDebris(int32 InDebrisId)
{
	TObjectPtr<UProceduralMeshComponent> Found = nullptr;

	if (LocalDebrisMeshMap.RemoveAndCopyValue(InDebrisId, Found))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] Found DebridId=%d") , InDebrisId);
		return Found;
	}

	UE_LOG(LogTemp, Error, TEXT("[Debris Actor] Not Found DebridId=%d") , InDebrisId);
	return nullptr;
}

void URealtimeDestructibleMeshComponent::BroadcastDebrisPhysicsState()
{
	// =========================================================================
	// BroadcastDebrisPhysicsState: 서버에서 모든 활성 Debris의 물리 상태를 브로드캐스트
	// =========================================================================
	// - 주기적으로 호출됨 (DebrisPhysicsSyncInterval 간격)
	// - 각 Debris의 Transform과 Velocity를 클라이언트에 전송
	// - 만료된(삭제된) Debris는 자동으로 정리
	//
	// TODO [리팩토링 예정]: ADebrisActor 자체 Replication으로 대체 시 이 함수 삭제

	// 서버에서만 실행
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// 삭제된 Debris 정리용 목록
	TArray<int32> ExpiredDebrisIds;

	for (const auto& Pair : ActiveDebrisActors)
	{
		int32 DebrisId = Pair.Key;
		TWeakObjectPtr<AActor> WeakActor = Pair.Value;

		// 유효하지 않은 액터는 정리 목록에 추가
		if (!WeakActor.IsValid())
		{
			ExpiredDebrisIds.Add(DebrisId);
			continue;
		}

		AActor* DebrisActor = WeakActor.Get();

		// RootComponent에서 물리 상태 가져오기
		UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(DebrisActor->GetRootComponent());
		if (!RootPrimitive || !RootPrimitive->IsSimulatingPhysics())
		{
			continue;
		}

		// 현재 물리 상태 수집
		FVector Location = DebrisActor->GetActorLocation();
		FRotator Rotation = DebrisActor->GetActorRotation();
		FVector LinearVelocity = RootPrimitive->GetPhysicsLinearVelocity();
		FVector AngularVelocity = RootPrimitive->GetPhysicsAngularVelocityInDegrees();

		// 클라이언트에 브로드캐스트
		MulticastSyncDebrisPhysics(DebrisId, Location, Rotation, LinearVelocity, AngularVelocity);
	}

	// 만료된 Debris 정리
	for (int32 ExpiredId : ExpiredDebrisIds)
	{
		ActiveDebrisActors.Remove(ExpiredId);
	}

	// 모든 Debris가 삭제되면 타이머 중지
	if (ActiveDebrisActors.Num() == 0)
	{
		GetWorld()->GetTimerManager().ClearTimer(DebrisPhysicsSyncTimerHandle);
	}
}

void URealtimeDestructibleMeshComponent::MulticastSyncDebrisPhysics_Implementation(
	int32 DebrisId, FVector Location, FRotator Rotation, FVector LinearVelocity, FVector AngularVelocity)
{
	// =========================================================================
	// MulticastSyncDebrisPhysics: 클라이언트에서 Debris 물리 상태 적용
	// =========================================================================
	// - 서버에서 전송한 물리 상태를 로컬 Debris에 적용
	// - 서버 자신은 이미 올바른 상태이므로 스킵
	//
	// TODO [리팩토링 예정]: ADebrisActor 자체 Replication으로 대체 시 이 함수 삭제

	// 서버는 스킵 (자신이 Authority이므로 이미 올바른 상태)
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	// 해당 ID의 로컬 Debris 찾기
	TWeakObjectPtr<AActor>* WeakActorPtr = ActiveDebrisActors.Find(DebrisId);
	if (!WeakActorPtr || !WeakActorPtr->IsValid())
	{
		// 아직 스폰되지 않았거나 이미 삭제된 경우 스킵
		return;
	}

	AActor* DebrisActor = WeakActorPtr->Get();
	UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(DebrisActor->GetRootComponent());
	if (!RootPrimitive)
	{
		return;
	}

	// 물리 시뮬레이션 중일 때만 상태 적용
	if (RootPrimitive->IsSimulatingPhysics())
	{
		// Transform 설정 (물리 엔진에 직접 전달)
		RootPrimitive->SetWorldLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);

		// Velocity 설정
		RootPrimitive->SetPhysicsLinearVelocity(LinearVelocity);
		RootPrimitive->SetPhysicsAngularVelocityInDegrees(AngularVelocity);
	}
	else
	{
		// 물리 비활성화 상태면 Transform만 설정
		DebrisActor->SetActorLocationAndRotation(Location, Rotation);
	}
}

void URealtimeDestructibleMeshComponent::CleanupSmallFragments()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(CleanupSmallFragments_NoArg);

	// 데디케이티드 서버에서는 파편 처리 스킵
	if (IsRunningDedicatedServer())
	{
		return;
	}

	// GridCellLayout이 유효하지 않으면 빈 셋으로 처리
	if (!GridCellLayout.IsValid())
	{
		CleanupSmallFragments(TSet<int32>());
		return;
	}

	// BFS로 분리된 셀 계산
	const ENetMode NetMode = GetWorld() ? GetWorld()->GetNetMode() : NM_Standalone;
	TSet<int32> DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
		GridCellLayout,
		SupercellState,
		CellState,
		bEnableSupercell && SupercellState.IsValid(),
		bEnableSubcell && (NetMode == NM_Standalone),
		CellContext);

	UE_LOG(LogTemp, Log, TEXT("[CleanupSmallFragments] Computed %d disconnected cells"), DisconnectedCells.Num());

	// 원본 함수 호출
	CleanupSmallFragments(DisconnectedCells);
}

void URealtimeDestructibleMeshComponent::CleanupSmallFragments(const TSet<int32>& InDisconnectedCells)
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
	const TSet<int32>& DisconnectedCells = InDisconnectedCells;

	int32 TotalRemoved = 0;

	for (UDynamicMeshComponent* ChunkMesh : ChunkMeshComponents)
	{
		if (!ChunkMesh || !ChunkMesh->GetMesh()) continue;

		FDynamicMesh3* Mesh = ChunkMesh->GetMesh();
		if (Mesh->TriangleCount() == 0) continue;

		FMeshConnectedComponents ConnectedComponents(Mesh);
		ConnectedComponents.FindConnectedTriangles();

		if (ConnectedComponents.Num() == 0) continue;

		FTransform MeshTransform = ChunkMesh->GetComponentTransform();

		// 제거할 삼각형 ID 수집 (EditMesh에서 일괄 처리)
		TArray<int32> TrianglesToRemove;

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
				bool bConnectedToAnchor = false;
				int32 TotalCellCount = 0;
				int32 DestroyedCellCount = 0;

				if (GridCellLayout.IsValid())
				{
					// 컴포넌트가 속한 고유 셀 ID 수집
					TSet<int32> ComponentCellIds;

					// 헬퍼 람다: 위치 → 셀 ID 추가
					auto AddCellIdFromPosition = [&](const FVector& Position)
					{
						FVector RelativePos = Position - GridCellLayout.GridOrigin;
						FIntVector GridCoord(
							FMath::FloorToInt(RelativePos.X / GridCellLayout.CellSize.X),
							FMath::FloorToInt(RelativePos.Y / GridCellLayout.CellSize.Y),
							FMath::FloorToInt(RelativePos.Z / GridCellLayout.CellSize.Z)
						);

						if (GridCoord.X >= 0 && GridCoord.X < GridCellLayout.GridSize.X &&
							GridCoord.Y >= 0 && GridCoord.Y < GridCellLayout.GridSize.Y &&
							GridCoord.Z >= 0 && GridCoord.Z < GridCellLayout.GridSize.Z)
						{
							ComponentCellIds.Add(GridCellLayout.CoordToId(GridCoord));
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
					int32 DisconnectedCount = 0;
					int32 ConnectedCount = 0;
					int32 InvalidCount = 0;
					for (int32 CellId : ComponentCellIds)
					{
						// 유효하지 않은 셀은 스킵 (sparse grid)
						if (!GridCellLayout.GetCellExists(CellId))
						{
							InvalidCount++;
							continue;
						}

						if (CellState.DestroyedCells.Contains(CellId))
						{
							DestroyedCellCount++;
						}
						else if (InDisconnectedCells.Contains(CellId))
						{
							DisconnectedCount++;
						}
						// 파괴되지 않았고, Anchor에 연결된 셀이면 (DisconnectedCells에 없으면)
						else
						{
							ConnectedCount++;
							bConnectedToAnchor = true;
						}
					}

					// 분리 조건:
					// 1. 유효 셀이 하나도 없음 (Invalid=Total) → 무조건 떨어짐
					// 2. 파괴된 cell이 하나라도 있고 + anchor 연결 cell이 없음 → 떨어짐
					// 3. Disconnected 셀만 있고 Connected 없음 → 떨어짐
					int32 ValidCellCount = TotalCellCount - InvalidCount;
					bShouldRemove = (ValidCellCount == 0) ||
					                (DestroyedCellCount > 0 && !bConnectedToAnchor) ||
					                (DisconnectedCount > 0 && ConnectedCount == 0);
				}

				// 디버그 색상: 빨강 = 삭제 대상 (Anchor 분리), 초록 = 유지 (Anchor 연결)
				if (bShowCellSpawnPosition)
				{
					FColor PointColor = bShouldRemove ? FColor::Red : FColor::Green;

					// 큰 점으로 ConnectedComponent 위치 표시
					DrawDebugPoint(GetWorld(), WorldPos, 30.0f, PointColor, false, 10.0f);
					DrawDebugString(GetWorld(), WorldPos, FString::Printf(TEXT("%s [%s] (%d/%d destroyed)"),
						bShouldRemove ? TEXT("Detached") : TEXT("Anchored"),
						bConnectedToAnchor ? TEXT("AnchorOK") : TEXT("AnchorNone"),
						DestroyedCellCount, TotalCellCount), nullptr, PointColor, 10.0f);
				}

				// 분리된 파편은 스폰 없이 바로 삭제
				if (bShouldRemove)
				{
					// 삭제할 삼각형 수집 (유효한 삼각형만)
					for (int32 Tid : Comp.Indices)
					{
						if (Mesh->IsTriangle(Tid))
						{
							TrianglesToRemove.Add(Tid);
						}
					}
				}
			}
		}

		// EditMesh를 사용하여 삼각형 제거 및 Compact (렌더링 업데이트 보장)
		if (TrianglesToRemove.Num() > 0)
		{
			ChunkMesh->EditMesh([&TrianglesToRemove](FDynamicMesh3& EditMesh)
			{
				for (int32 Tid : TrianglesToRemove)
				{
					EditMesh.RemoveTriangle(Tid);
				}
				EditMesh.CompactInPlace();
			});
			TotalRemoved++;
		}
	}

	if (TotalRemoved > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CleanupSmallFragments: Removed %d chunk fragments (overlaps destroyed cells)"),
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
		// 데디케이티드 서버: 서버에서 파괴 로직 실행 (Cell Collision 업데이트용)
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() == NM_DedicatedServer)
		{
			for (const FRealtimeDestructionOp& Op : Ops)
			{
				DestructionLogic(Op.Request);
			}
		}

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
		if (bEnableSupercell && SupercellState.IsValid())
		{
			SupercellState.OnCellDestroyed(CellId);
	}
	}

	UE_LOG(LogTemp, Log, TEXT("[Client] MulticastDestroyedCells: +%d cells, Total=%d"),
		DestroyedCellIds.Num(), CellState.DestroyedCells.Num());

	// 클라이언트 Cell Box Collision: 파괴된 셀과 이웃 셀의 청크를 dirty 마킹
	if (bServerCellCollisionInitialized)
	{
		TSet<int32> DirtyChunkIndices;
		for (int32 CellId : DestroyedCellIds)
		{
			int32 ChunkIdx = GetCollisionChunkIndexForCell(CellId);
			if (ChunkIdx != INDEX_NONE)
			{
				DirtyChunkIndices.Add(ChunkIdx);
			}

			const FIntArray& Neighbors = GridCellLayout.GetCellNeighbors(CellId);
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

		UE_LOG(LogTemp, Log, TEXT("[ClientCellCollision] Marked %d chunks dirty from %d destroyed cells"),
			DirtyChunkIndices.Num(), DestroyedCellIds.Num());
	}
}

void URealtimeDestructibleMeshComponent::MulticastDetachSignal_Implementation()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Structural Integrity 비활성화 시 분리 셀 처리 스킵
	if (!bEnableStructuralIntegrity)
	{
		return;
	}

	const ENetMode NetMode = World->GetNetMode();

	// DedicatedServer는 스킵 (렌더링 없음)
	if (NetMode == NM_DedicatedServer)
	{
		return;
	}

	//// 서버는 이미 로컬에서 처리했으므로 스킵
	//if (GetOwner() && GetOwner()->HasAuthority())
	//{
	//	return;
	//}

	UE_LOG(LogTemp, Warning, TEXT("[Client] MulticastDetachSignal RECEIVED - Running local BFS"));

	// 클라이언트: 자체 BFS 실행하여 분리된 셀 찾기
	// 통합 API 사용: 서버와 동일한 알고리즘으로 일관성 유지
	// Multiplayer: SubCell 상태는 Client에 동기화되지 않으므로 Standalone에서만 사용
	TSet<int32> DisconnectedCells = FCellDestructionSystem::FindDisconnectedCells(
		GridCellLayout,
		SupercellState,
		CellState,
		bEnableSupercell && SupercellState.IsValid(),
		bEnableSubcell && (NetMode == NM_Standalone),
		CellContext); // subcell 동기화 안 하므로 subcell은 standalone에서만 허용 

	if (DisconnectedCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Client] BFS result: No disconnected cells"));
		CleanupSmallFragments(DisconnectedCells);
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Client] BFS result: %d disconnected cells"), DisconnectedCells.Num());

	// 분리된 셀 그룹화
	TArray<TArray<int32>> DetachedGroups = FCellDestructionSystem::GroupDetachedCells(
		GridCellLayout,
		DisconnectedCells,
		CellState.DestroyedCells);

	UE_LOG(LogTemp, Warning, TEXT("[Client] Grouped into %d debris groups"), DetachedGroups.Num());

	const bool bIsDedicatedServerClient = bServerIsDedicatedServer && !GetOwner()->HasAuthority(); 
	// 각 그룹에 대해 처리
	for (const TArray<int32>& Group : DetachedGroups)
	{
		// CellState에 Detached 그룹 추가
		CellState.AddDetachedGroup(Group);

		// 분리된 셀의 삼각형 삭제 (시각적 처리)
		if (!bIsDedicatedServerClient)
		{
			RemoveTrianglesForDetachedCells(Group);
		}
	}

	// 분리된 셀들을 파괴됨 상태로 이동
	CellState.MoveAllDetachedToDestroyed();

	// RemoveTriangles 후 작은 파편 정리는 IslandRemoval 완료 콜백에서 처리
	// (비동기 완료 시 FIslandRemovalContext::DisconnectedCellsForCleanup 사용)

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

	// === 배치 추적 시작 ===
	const int32 BatchId = NextBatchId++;
	int32 ActualEnqueuedCount = 0;

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

		// ToolOriginWorld는 Decompress()에서 이미 계산됨

		// DecalMaterial 조회 (네트워크로 전송된 ConfigID로 로컬에서 조회)
		// 1. 컴포넌트에 설정된 DecalDataAsset 사용
		// 2. 없으면 GameInstanceSubsystem에서 조회
		UImpactProfileDataAsset* DataAssetToUse = nullptr;
			if (UGameInstance* GI = GetWorld()->GetGameInstance())
			{
				if (UDestructionGameInstanceSubsystem* Subsystem = GI->GetSubsystem<UDestructionGameInstanceSubsystem>())
				{
				DataAssetToUse = Subsystem->FindDataAssetByConfigID(ModifiableRequest.DecalConfigID);
				}
			}


		if (DataAssetToUse && ModifiableRequest.bSpawnDecal)
		{
			FImpactProfileConfig FoundConfig;
			if (DataAssetToUse->GetConfigRandom(ModifiableRequest.SurfaceType, FoundConfig))
			{
				ModifiableRequest.DecalMaterial = FoundConfig.DecalMaterial;
				ModifiableRequest.DecalSize = FoundConfig.DecalSize;
				ModifiableRequest.DecalLocationOffset = FoundConfig.LocationOffset;
				ModifiableRequest.DecalRotationOffset = FoundConfig.RotationOffset;
			}
		}

		// 데칼 생성
		UDecalComponent* TempDecal = nullptr;
		if (ModifiableRequest.bSpawnDecal)
		{
			TempDecal = SpawnTemporaryDecal(ModifiableRequest);
		}

		// 비동기 경로로 처리 (워커 스레드 사용) - BatchId 전달
		if (ModifiableRequest.ChunkIndex != INDEX_NONE && BooleanProcessor.IsValid())
		{
			EnqueueRequestLocal(ModifiableRequest, Op.bIsPenetration, TempDecal, BatchId);
			ActualEnqueuedCount++;
		}
		else
		{
			// 큐잉 조건 불충족 시 기존처럼 호출 (BatchId 없이)
			EnqueueRequestLocal(ModifiableRequest, Op.bIsPenetration, TempDecal);
		}
	}

	// === 배치 트래커 등록 ===
	if (ActualEnqueuedCount > 0)
	{
		FBooleanBatchTracker Tracker;
		Tracker.TotalCount = ActualEnqueuedCount;
		Tracker.CompletedCount = 0;
		ActiveBatchTrackers.Add(BatchId, Tracker);

		UE_LOG(LogTemp, Log, TEXT("[BatchTracking] Started BatchId=%d, TotalCount=%d"), BatchId, ActualEnqueuedCount);
	}
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
			ShapeParams.Height + ShapeParams.SurfaceMargin,
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
			ShapeParams.Height + ShapeParams.SurfaceMargin,
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
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Collision_ApplyCollisionUpdate);

	// 서버 Cell Collision 활성 시: 서버는 NoCollision, 클라이언트는 Pawn만 Ignore
	if (bServerCellCollisionInitialized)
	{
		if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
		{
			TargetComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		else
		{
			TargetComp->UpdateCollision();
			TargetComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		}
		return;
	}
	TargetComp->UpdateCollision();
}

void URealtimeDestructibleMeshComponent::ApplyCollisionUpdateAsync(UDynamicMeshComponent* TargetComp)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Collision_ApplyCollisionUpdateAsync);

	// 서버 Cell Collision 활성 시: 서버는 NoCollision, 클라이언트는 Pawn만 Ignore
	if (bServerCellCollisionInitialized)
	{
		if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
		{
			TargetComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		else
		{
			TargetComp->UpdateCollision(true);
			TargetComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		}
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("Call Collision Update %f"), FPlatformTime::Seconds());
	TargetComp->UpdateCollision(true);
}

bool URealtimeDestructibleMeshComponent::IsChunkPenetrated(const FRealtimeDestructionRequest& Request) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(IsPenetratingThrough);

	if (!ChunkMeshComponents.IsValidIndex(Request.ChunkIndex))
	{
		return false;
	}

	UDynamicMeshComponent* ChunkComp = ChunkMeshComponents[Request.ChunkIndex];
	if (!ChunkComp)
	{
		return false;
	}

	const FVector ImpactEndPoint = Request.ImpactPoint + Request.ToolForwardVector * Request.Depth;

	FHitResult HitBackResult;
	FCollisionQueryParams Params;
	Params.bTraceComplex = true;

	// ImpactEndPoint에서 ImpactPoint 방향으로 Ray (대상 chunk만 트레이스)
	// tool이 벽을 관통하면 ImpactEndPoint는 메시 바깥 => back face 히트
	// tool이 벽을 관통 못하면 ImpactEndPoint는 메시 내부 => single-sided collision으로 히트 없음
	bool bHitBack = ChunkComp->LineTraceComponent(HitBackResult, ImpactEndPoint, Request.ImpactPoint, Params);

	return bHitBack && FVector::DotProduct(HitBackResult.ImpactNormal, Request.ToolForwardVector) > 0.f;
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

void URealtimeDestructibleMeshComponent::ApplyBooleanOperationResult(FDynamicMesh3&& NewMesh, const int32 ChunkIndex, bool bDelayedCollisionUpdate, int32 BatchId)
{
	if (ChunkIndex == INDEX_NONE)
	{
		NotifyBooleanSkipped(BatchId);
		return;
	}

	UDynamicMeshComponent* TargetComp = GetChunkMeshComponent(ChunkIndex);
	if (!TargetComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("TargetComp is invalid"));
		NotifyBooleanSkipped(BatchId);
		return;
	}

	TargetComp->EditMesh([&](FDynamicMesh3& InternalMesh)
		{
			InternalMesh = MoveTemp(NewMesh);
		});

	// 수정된 청크 추적
	ModifiedChunkIds.Add(ChunkIndex);
#if !UE_BUILD_SHIPPING
	// 디버그 텍스트 갱신 플래그는 기본적으로 구조적 무결성 갱신 후 업데이트되지만, 청크 없는 경우 여기에서 대신 갱신
		bShouldDebugUpdate = true;
#endif
	if (bDelayedCollisionUpdate)
	{
		RequestDelayedCollisionUpdate(TargetComp);
	}
	else
	{
		ApplyCollisionUpdate(TargetComp);
	}

	// Standalone: Boolean 완료 후 분리 셀 처리
	// TODO: 매 Boolean마다 호출하면 렉 발생 - 타이머 기반으로 변경 필요
	//UWorld* World = GetWorld();
	//if (World && World->GetNetMode() == NM_Standalone)
	//{
	//	DisconnectedCellStateLogic(PendingDestructionResults);
	//	PendingDestructionResults.Empty();
	//}

	// Boolean 완료 후 파편 정리 (스폰 제거로 가벼워짐)
	//CleanupSmallFragments();
	bPendingCleanup = true;

	// === 배치 완료 추적 ===
	if (BatchId != INDEX_NONE)
	{
		if (FBooleanBatchTracker* Tracker = ActiveBatchTrackers.Find(BatchId))
		{
			Tracker->CompletedCount++;
			UE_LOG(LogTemp, Log, TEXT("[BatchTracking] Completed BatchId=%d, Progress=%d/%d"),
				BatchId, Tracker->CompletedCount, Tracker->TotalCount);

			if (Tracker->IsComplete())
			{
				OnBooleanBatchCompleted(BatchId);
			}
		}
	}
}

void URealtimeDestructibleMeshComponent::NotifyBooleanSkipped(int32 BatchId)
{
	if (BatchId == INDEX_NONE)
	{
		return;
	}

	if (FBooleanBatchTracker* Tracker = ActiveBatchTrackers.Find(BatchId))
	{
		Tracker->CompletedCount++;
		UE_LOG(LogTemp, Log, TEXT("[BatchTracking] Skipped BatchId=%d, Progress=%d/%d"),
			BatchId, Tracker->CompletedCount, Tracker->TotalCount);

		if (Tracker->IsComplete())
		{
			OnBooleanBatchCompleted(BatchId);
		}
	}
}

void URealtimeDestructibleMeshComponent::NotifyBooleanCompleted(int32 BatchId)
{
	if (BatchId == INDEX_NONE)
	{
		return;
	}

	if (FBooleanBatchTracker* Tracker = ActiveBatchTrackers.Find(BatchId))
	{
		Tracker->CompletedCount++;
		UE_LOG(LogTemp, Log, TEXT("[BatchTracking] Completed BatchId=%d, Progress=%d/%d"),
			BatchId, Tracker->CompletedCount, Tracker->TotalCount);

		if (Tracker->IsComplete())
		{
			OnBooleanBatchCompleted(BatchId);
		}
	}
}

void URealtimeDestructibleMeshComponent::OnBooleanBatchCompleted(int32 BatchId)
{
	UE_LOG(LogTemp, Warning, TEXT("[BatchTracking] ★ Batch %d COMPLETED!"), BatchId);

	// 트래커 제거
	ActiveBatchTrackers.Remove(BatchId);

	// IslandRemoval이 진행 중이면 Cleanup 스킵
	// (IslandRemoval 완료 시점에 Cleanup이 호출되므로 중복 방지)
	if (ActiveIslandRemovalCount.load() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BatchTracking] Skipping CleanupSmallFragments - IslandRemoval in progress (Count: %d)"), ActiveIslandRemovalCount.load());
		return;
	}

	// 파편 정리 실행
	UE_LOG(LogTemp, Warning, TEXT("[BatchTracking] Calling CleanupSmallFragments"));
	CleanupSmallFragments();
}

void URealtimeDestructibleMeshComponent::RequestDelayedCollisionUpdate(UDynamicMeshComponent* TargetComp)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Debris_Collision_RequestDelayed);
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
	const int32 ChunkCount = ChunkMeshComponents.Num();

	if (ChunkCount > 0)
	{
		for (UDynamicMeshComponent* ChunkMesh : ChunkMeshComponents)
	{
			if (!ChunkMesh)
			{
				continue;
	}

			if (UDynamicMesh* ChunkDynMesh = ChunkMesh->GetDynamicMesh())
	{
				ChunkDynMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
						VertexCount += Mesh.VertexCount();
						TriangleCount += Mesh.TriangleCount();
					});
			}
		}
		}
	else if (UDynamicMesh* DynMesh = GetDynamicMesh())
	{
		DynMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			VertexCount = Mesh.VertexCount();
			TriangleCount = Mesh.TriangleCount();
		});
	}

	const int32 CellCount = GridCellLayout.GetValidCellCount();
	const int32 AnchorCount = GridCellLayout.GetAnchorCount();
	const int32 DestroyedCount = CellState.DestroyedCells.Num();

	// 디버그 텍스트 생성
	DebugText = FString::Printf(
		TEXT("[Basic Info]\nVertices: %d\nTriangles: %d\nInitialized: %s\n[Grid Cells]\nChunks: %d | Cells: %d | Anchors: %d | Destroyed: %d"),
		VertexCount,
		TriangleCount,
		bIsInitialized ? TEXT("Yes") : TEXT("No"),
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
	if (!GridCellLayout.IsValid() || !GridCellLayout.HasValidSparseData())
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
			GridCellLayout.GridSize.X, GridCellLayout.GridSize.Y, GridCellLayout.GridSize.Z,
			GridCellLayout.GetValidCellCount(), GridCellLayout.GetAnchorCount());
		bFirstGridDraw = false;
	}

	// // 파괴된 셀 개수 확인 로그 (1초마다)
	// static double LastLogTime = 0.0;
	// double CurrentTime = FPlatformTime::Seconds();
	// if (CurrentTime - LastLogTime > 1.0)
	// {
	// 	int32 DestroyedCount = 0;
	// 	int32 DetachedCount = 0;
	// 	for (int32 CellId : GridCellLayout.GetValidCellIds())
	// 	{
	// 		if (CellState.DestroyedCells.Contains(CellId)) DestroyedCount++;
	// 		if (CellState.IsCellDetached(CellId)) DetachedCount++;
	// 	}
	// 	UE_LOG(LogTemp, Warning, TEXT("[DrawGridCellDebug] DestroyedCells.Num=%d, ValidCells에서 Destroyed=%d, Detached=%d, bShowDestroyedCells=%d"),
	// 		CellState.DestroyedCells.Num(), DestroyedCount, DetachedCount, bShowDestroyedCells);
	// 	LastLogTime = CurrentTime;
	// }

	// 1. 유효 셀만 그리기 (희소 배열)
	for (int32 CellId : GridCellLayout.GetValidCellIds())
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
		else if (GridCellLayout.GetCellIsAnchor(CellId))
		{
			CellColor = FColor(0, 255, 0);  // 밝은 녹색
		}
		else
		{
			CellColor = FColor::Cyan;
		}

		// 셀 중심점 그리기 (점으로만 표시 - 성능 최적화)
		const FVector LocalCenter = GridCellLayout.IdToLocalCenter(CellId);
		const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);

		DrawDebugPoint(World, WorldCenter, 5.0f, CellColor, false, 0.0f, SDPG_Foreground);
	}
}

void URealtimeDestructibleMeshComponent::DrawSupercellDebug()
{
	if (!SupercellState.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& ComponentTransform = GetComponentTransform();
	const FVector& CellSize = GridCellLayout.CellSize;
	const FVector SupercellWorldSize = FVector(
		CellSize.X * SupercellState.SupercellSize.X,
		CellSize.Y * SupercellState.SupercellSize.Y,
		CellSize.Z * SupercellState.SupercellSize.Z
	);

	// 모든 SuperCell 순회
	for (int32 SCZ = 0; SCZ < SupercellState.SupercellCount.Z; ++SCZ)
	{
		for (int32 SCY = 0; SCY < SupercellState.SupercellCount.Y; ++SCY)
		{
			for (int32 SCX = 0; SCX < SupercellState.SupercellCount.X; ++SCX)
			{
				const int32 SupercellId = SupercellState.SupercellCoordToId(SCX, SCY, SCZ);
				const bool bIsIntact = SupercellState.IsSupercellIntact(SupercellId);

				//if (!bIsIntact)
				//{
				//	continue;
				//}
				//색상: Intact=녹색, Broken=빨강
				FColor BoxColor = bIsIntact ? FColor::Green : FColor::Red;
				//FColor BoxColor = FColor::Green;
				// SuperCell 로컬 Min 좌표
				FVector LocalMin = GridCellLayout.GridOrigin + FVector(
					SCX * SupercellWorldSize.X,
					SCY * SupercellWorldSize.Y,
					SCZ * SupercellWorldSize.Z
				);
				FVector LocalMax = LocalMin + SupercellWorldSize;
				FVector LocalCenter = (LocalMin + LocalMax) * 0.5f;

				// 월드 좌표로 변환
				FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);
				FVector WorldExtent = SupercellWorldSize * 0.5f * ComponentTransform.GetScale3D();

				// 박스 그리기
				DrawDebugBox(World, WorldCenter, WorldExtent, ComponentTransform.GetRotation(), BoxColor, false, -1.0f, 0, 2.0f); 
			}
		}
	}
}

void URealtimeDestructibleMeshComponent::DrawSubCellDebug()
{
#if !UE_BUILD_SHIPPING
	if (!GridCellLayout.IsValid() && !bEnableSubcell)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	} 

	const FTransform& ComponentTransform = GetComponentTransform();
	const FVector SubCellSize = GridCellLayout.GetSubCellSize(); 
	const FVector HalfExtent = SubCellSize * 0.5f * ComponentTransform.GetScale3D();
	 

	for (int32 CellId : GridCellLayout.GetValidCellIds())
	{
		for (int32 SubCellId = 0; SubCellId < SUBCELL_COUNT; ++SubCellId)
		{
			const bool bAlive = CellState.IsSubCellAlive(CellId, SubCellId);
			const FColor SubCellColor = bAlive ? FColor::Green : FColor::Red;

			const FVector LocalCenter = GridCellLayout.GetSubCellLocalCenter(CellId, SubCellId);
			const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);


			DrawDebugBox(World, WorldCenter, HalfExtent,
				ComponentTransform.GetRotation(),
				SubCellColor,
				false, 0.0f, SDPG_World, 1.0f);
		}
		  
	}

	static bool bFirstDraw = true;
#endif

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

			const FVector LocalCenter = GridCellLayout.IdToLocalCenter(CellId);
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
void URealtimeDestructibleMeshComponent::RegisterDecalToCells(UDecalComponent* Decal, const FRealtimeDestructionRequest& Request)
{
	if (!Decal)
	{
		UE_LOG(LogTemp, Warning, TEXT("RegisterDecalToSubCells : Decal Invalid"));
		return;
	}

	if (!GridCellLayout.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("RegisterDecalToSubCells : Grid Cell Invalid"));
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE("Decal_Register");

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

	FVector SearchCenter = Decal->GetComponentLocation();

	FCellDestructionShape DecalShape;
	DecalShape.Type = ECellDestructionShapeType::Box;
	DecalShape.Center = SearchCenter;
	DecalShape.BoxExtent = EffectiveExtent;
	DecalShape.Rotation = Decal->GetComponentRotation();

	FQuantizedDestructionInput QuantizedDecal = FQuantizedDestructionInput::FromDestructionShape(DecalShape);

	// 데칼 영역 내의 후보 cell 탐색
	FBox ThinLocalBox(-EffectiveExtent, EffectiveExtent);
	FTransform ThinBoxTransform(Decal->GetComponentQuat(), SearchCenter);
	FBox ThinWorldBox = ThinLocalBox.TransformBy(ThinBoxTransform);

	const FTransform& MeshTransform = GetComponentTransform();
	TArray<int32> CandidateCells = GridCellLayout.GetCellsInAABB(ThinWorldBox, MeshTransform);

	TSet<int32> ValidCells;
	ValidCells.Reserve(CandidateCells.Num());
	

	for (int32 CellID : CandidateCells)
	{
		if (CellState.DestroyedCells.Contains(CellID))
		{
			continue;
		}

		FCellOBB CellWorldOBB = GridCellLayout.GetCellWorldOBB(CellID, MeshTransform);

		if (QuantizedDecal.IntersectsOBB(CellWorldOBB))
		{
			ValidCells.Add(CellID);
		}
	}

	if (ValidCells.Num() > 0)
	{
		int32 NewHandle = ++NextDecalHandle;

		FManagedDecal NewDecal;
		NewDecal.Decal = Decal;
		NewDecal.RemainingCellCount = ValidCells.Num();

		ActiveDecals.Add(NewHandle, NewDecal);

		for (int32 CellId : ValidCells)
		{
			CellToDecalMap.FindOrAdd(CellId).Add(NewHandle);
	}
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

	TRACE_CPUPROFILER_EVENT_SCOPE("Decal_Removal")

	TSet<int32> DecalsToRemove;

	for (int32 DestroyedCellId : Result.NewlyDestroyedCells)
	{
		if (TArray<int32>* DecalHandle = CellToDecalMap.Find(DestroyedCellId))
		{
			for (int32 Handle : *DecalHandle)
			{
				if (FManagedDecal* Decal = ActiveDecals.Find(Handle))
				{
					Decal->RemainingCellCount--;

					if (Decal->RemainingCellCount <= 0)
					{
						DecalsToRemove.Add(Handle);
					}
				}
			CellToDecalMap.Remove(DestroyedCellId);
			}

			CellToDecalMap.Remove(DestroyedCellId);
		}
	}

	for (int32 Handle : DecalsToRemove)
	{
		if (FManagedDecal* Decal = ActiveDecals.Find(Handle))
		{
			if (auto DecalComp = Decal->Decal.Get())
			{
				DecalComp->DestroyComponent();
			}

			ActiveDecals.Remove(Handle);
		}
	}
}
 

void URealtimeDestructibleMeshComponent::OnRegister()
{
	Super::OnRegister();
#if WITH_EDITOR
	if (CachedGeometryCollection && ChunkMeshComponents.Num() == 0 && GetOwner() != nullptr)
	{
		BuildChunksFromGC(CachedGeometryCollection);
		return; 
	}

	if (!bAutoSetUpDone && !SourceStaticMesh && ChunkMeshComponents.Num() == 0)
	{
		TryAutoSetupFromParentStaticMesh();
	}
#endif

	// if (ChunkMeshComponents.Num() > 0)
	// {
	// 	return;  // Cell 모드에서 이미 셀이 있으면 스킵
	// }

	if (SourceStaticMesh && !bIsInitialized)
	{
		InitializeFromStaticMeshInternal(SourceStaticMesh, false);
	}

#if WITH_EDITOR
	// 에디터 로드 시 GridCellSize와 GridCellLayout.CellSize 불일치 검증
	// GridCellSize는 월드 스페이스, GridCellLayout.CellSize는 로컬 스페이스 (= GridCellSize / MeshScale)
	if (SourceStaticMesh && GridCellLayout.IsValid())
	{
		// 저장된 MeshScale 사용 (빌드 시점의 스케일)
		const FVector& SavedMeshScale = GridCellLayout.MeshScale;
		// 월드 셀 크기를 로컬로 변환
		const FVector ExpectedLocalCellSize = GridCellSize / SavedMeshScale;

		// 불일치 여부 확인 (오차 허용)
		const float Tolerance = 0.1f;
		const bool bCellSizeMismatch =
			!FMath::IsNearlyEqual(ExpectedLocalCellSize.X, GridCellLayout.CellSize.X, Tolerance) ||
			!FMath::IsNearlyEqual(ExpectedLocalCellSize.Y, GridCellLayout.CellSize.Y, Tolerance) ||
			!FMath::IsNearlyEqual(ExpectedLocalCellSize.Z, GridCellLayout.CellSize.Z, Tolerance);

		if (bCellSizeMismatch)
		{
			UE_LOG(LogTemp, Warning, TEXT("OnRegister: GridCellSize(%.1f,%.1f,%.1f)/MeshScale(%.2f,%.2f,%.2f) -> Expected(%.2f,%.2f,%.2f) != Saved(%.2f,%.2f,%.2f). Rebuilding..."),
				GridCellSize.X, GridCellSize.Y, GridCellSize.Z,
				SavedMeshScale.X, SavedMeshScale.Y, SavedMeshScale.Z,
				ExpectedLocalCellSize.X, ExpectedLocalCellSize.Y, ExpectedLocalCellSize.Z,
				GridCellLayout.CellSize.X, GridCellLayout.CellSize.Y, GridCellLayout.CellSize.Z);
			BuildGridCells();
		}
	}
#endif
}

void URealtimeDestructibleMeshComponent::InitializeComponent()
{
	Super::InitializeComponent(); 
}
void URealtimeDestructibleMeshComponent::BeginPlay()
{
	Super::BeginPlay();

	// 서버에서 데디케이티드 서버 여부 설정 (클라이언트로 복제됨)
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		bServerIsDedicatedServer = IsRunningDedicatedServer();
	}

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

	// 런타임 시작 시 GridCellLayout가 유효하지 않으면 구축
	if ( ( SourceStaticMesh && !GridCellLayout.IsValid()) || CachedRDMScale != GetComponentTransform().GetScale3D())
	{
		BuildGridCells();
	}
	// 런타임에서도 GridCellSize와 GridCellLayout.CellSize 불일치 검증 (서버/클라이언트 공통)
	else if (SourceStaticMesh && GridCellLayout.IsValid())
	{
		const FVector& SavedMeshScale = GridCellLayout.MeshScale;
		// 0으로 나누기 방지
		const FVector SafeScale(
			FMath::Max(SavedMeshScale.X, KINDA_SMALL_NUMBER),
			FMath::Max(SavedMeshScale.Y, KINDA_SMALL_NUMBER),
			FMath::Max(SavedMeshScale.Z, KINDA_SMALL_NUMBER)
		);
		const FVector ExpectedLocalCellSize = GridCellSize / SafeScale;

		const float Tolerance = 0.1f;
		const bool bCellSizeMismatch =
			!FMath::IsNearlyEqual(ExpectedLocalCellSize.X, GridCellLayout.CellSize.X, Tolerance) ||
			!FMath::IsNearlyEqual(ExpectedLocalCellSize.Y, GridCellLayout.CellSize.Y, Tolerance) ||
			!FMath::IsNearlyEqual(ExpectedLocalCellSize.Z, GridCellLayout.CellSize.Z, Tolerance);

		if (bCellSizeMismatch)
		{
			UE_LOG(LogTemp, Warning, TEXT("BeginPlay: GridCellSize/CellSize mismatch detected. Expected(%.2f,%.2f,%.2f) != Saved(%.2f,%.2f,%.2f). Rebuilding GridCellLayout..."),
				ExpectedLocalCellSize.X, ExpectedLocalCellSize.Y, ExpectedLocalCellSize.Z,
				GridCellLayout.CellSize.X, GridCellLayout.CellSize.Y, GridCellLayout.CellSize.Z);
			BuildGridCells();
		}
	}
	if (bIsInitialized && !BooleanProcessor.IsValid())
	{
		BooleanProcessor = MakeShared<FRealtimeBooleanProcessor>();
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
	UE_LOG(LogTemp, Log, TEXT("BeginPlay: bCellMeshesValid=%d, GridCellLayout.IsValid=%d, CellMeshComponents.Num=%d"),
		bChunkMeshesValid, GridCellLayout.IsValid(), ChunkMeshComponents.Num());

	/** Culstering 관련 초기화 */
	if (bEnableClustering && GetOwner()->HasAuthority())
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

	UWorld* World = GetWorld();
	if (bPendingCleanup && World && World->GetNetMode() == NM_Standalone )
	{
		StandaloneDetachTimer += DeltaTime;
			DisconnectedCellStateLogic(PendingDestructionResults, true);  // bForceRun = true
			PendingDestructionResults.Empty();
			StandaloneDetachTimer = 0.0f;

		bPendingCleanup = false;
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
	
	// Subcell Debug 표시 
	if (bShowSupercellDebug)
	{
		DrawSupercellDebug();
	}

	// Draw SubCell Debug
	if (bShowSubCellDebug)
	{
		DrawSubCellDebug();
	}

	// 서버/클라이언트 Cell Box Collision: 지연 초기화 (BeginPlay에서 GridCellLayout이 유효하지 않았던 경우)
	if (!bServerCellCollisionInitialized && bEnableServerCellCollision && GridCellLayout.IsValid()
		&& GetWorld() && (GetWorld()->GetNetMode() == NM_DedicatedServer || GetWorld()->GetNetMode() == NM_Client))
	{
		UE_LOG(LogTemp, Display, TEXT("[ServerCellCollision] Deferred init: GridCellLayout now valid, calling BuildServerCellCollision()"));
		BuildServerCellCollision();
	}

	// 서버/클라이언트 Cell Box Collision: Dirty 청크 업데이트
	if (bServerCellCollisionInitialized)
	{
		UpdateDirtyCollisionChunks();
	}

	// Late Join: 데이터 수신 후 조건 충족 시 적용
	if (!bLateJoinApplied && bLateJoinCellsReceived
		&& GetWorld() && GetWorld()->GetNetMode() == NM_Client
		&& GridCellLayout.IsValid() && BooleanProcessor.IsValid() && bChunkMeshesValid)
	{
		ApplyLateJoinData();
	}

	// 서버 배칭 처리
	if (!bUseServerBatching)
	{
		return;
	}

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
	DOREPLIFETIME_CONDITION(URealtimeDestructibleMeshComponent, AppliedOpHistory, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(URealtimeDestructibleMeshComponent, LateJoinDestroyedCells, COND_InitialOnly);

	// 서버 타입 복제 (클라이언트가 Listen/Dedicated 구분용)
	DOREPLIFETIME(URealtimeDestructibleMeshComponent, bServerIsDedicatedServer);
}

void URealtimeDestructibleMeshComponent::OnRep_LateJoinOpHistory()
{
	bLateJoinOpsReceived = true;
	UE_LOG(LogTemp, Log, TEXT("[LateJoin] Received %d ops from server"), AppliedOpHistory.Num());
}

void URealtimeDestructibleMeshComponent::OnRep_LateJoinDestroyedCells()
{
	bLateJoinCellsReceived = true;
	UE_LOG(LogTemp, Log, TEXT("[LateJoin] Received %d destroyed cells from server"), LateJoinDestroyedCells.Num());
}

void URealtimeDestructibleMeshComponent::ApplyLateJoinData()
{
	bLateJoinApplied = true;
	UE_LOG(LogTemp, Log, TEXT("[LateJoin] Applying: %d destroyed cells, %d ops"),
		LateJoinDestroyedCells.Num(), AppliedOpHistory.Num());

	// === Phase 1: CellState 즉시 적용 (충돌 정확성) ===
	for (int32 CellId : LateJoinDestroyedCells)
	{
		CellState.DestroyedCells.Add(CellId);

		// SuperCell 상태 업데이트
		if (bEnableSupercell && SupercellState.IsValid())
		{
			SupercellState.OnCellDestroyed(CellId);
		}
	}

	// Cell Box Collision 빌드 (DestroyedCells 기반으로 올바른 충돌 즉시 생성)
	if (bEnableServerCellCollision && !bServerCellCollisionInitialized)
	{
		BuildServerCellCollision();
	}
	else if (bServerCellCollisionInitialized)
	{
		// 이미 초기화됐으면 전체 청크 dirty 마킹
		for (int32 i = 0; i < CollisionChunks.Num(); ++i)
		{
			MarkCollisionChunkDirty(i);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[LateJoin] Phase 1 complete: CellState has %d destroyed cells"), CellState.DestroyedCells.Num());

	// === Phase 1.5: 분리 셀 삼각형 제거 + 파편 정리 (비주얼 즉시 반영) ===
	if (LateJoinDestroyedCells.Num() > 0)
	{
		// 파괴된 모든 셀의 삼각형을 청크 메시에서 제거
		RemoveTrianglesForDetachedCells(LateJoinDestroyedCells);

		// 잔여 소형 파편 정리
		TSet<int32> DestroyedCellSet(LateJoinDestroyedCells);
		CleanupSmallFragments(DestroyedCellSet);

		UE_LOG(LogTemp, Log, TEXT("[LateJoin] Phase 1.5 complete: Removed triangles for %d cells"), LateJoinDestroyedCells.Num());
	}

	// === Phase 2: Op History 리플레이 (Boolean으로 정밀 메시 복원) ===
	if (bLateJoinOpsReceived && AppliedOpHistory.Num() > 0)
	{
		// CompactOps → Ops 변환
		TArray<FRealtimeDestructionOp> Ops;
		Ops.Reserve(AppliedOpHistory.Num());
		for (const FCompactDestructionOp& CompactOp : AppliedOpHistory)
		{
			FRealtimeDestructionOp Op;
			Op.Request = CompactOp.Decompress();
			Ops.Add(Op);
		}

		// 기존 ApplyOpsDeterministic 파이프라인으로 리플레이
		// → EnqueueRequestLocal → BooleanProcessor (비동기)
		// → 메시가 점진적으로 업데이트됨
		ApplyOpsDeterministic(Ops);

		UE_LOG(LogTemp, Log, TEXT("[LateJoin] Phase 2: Enqueued %d ops for Boolean replay"), Ops.Num());
	}

	// Late Join 전용 데이터 메모리 해제 (클라이언트에서 더 이상 불필요)
	LateJoinDestroyedCells.Empty();
	LateJoinDestroyedCells.Shrink();

	UE_LOG(LogTemp, Log, TEXT("[LateJoin] Complete. CellState has %d destroyed cells"), CellState.DestroyedCells.Num());
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

		// 데디서버: Multicast는 자기 자신에게 실행 안 됨, BFS로 분리된 셀 찾기
		// DestructionLogic은 RequestDestruction에서 이미 호출되어 셀이 파괴됨
		// 여기서는 BFS만 실행하여 분리된 셀을 찾아 Cell Box 업데이트
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() == NM_DedicatedServer)
		{
			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH START] Ops=%d ##########"), PendingServerBatchOpsCompact.Num());

			TArray<FDestructionResult> AllResults;
			DisconnectedCellStateLogic(AllResults, true);

			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH END] ##########"));
		}

		// 압축된 데이터로 전파
		MulticastApplyOpsCompact(PendingServerBatchOpsCompact);

		// 클라이언트에게 분리 셀 처리 신호 전송
		MulticastDetachSignal();

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

		// 데디서버: Multicast는 자기 자신에게 실행 안 됨, BFS로 분리된 셀 찾기
		UWorld* WorldNonCompact = GetWorld();
		if (WorldNonCompact && WorldNonCompact->GetNetMode() == NM_DedicatedServer)
		{
			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH START] Ops=%d (non-compact) ##########"), PendingServerBatchOps.Num());

			TArray<FDestructionResult> AllResults;
			DisconnectedCellStateLogic(AllResults, true);

			UE_LOG(LogTemp, Warning, TEXT("########## [BATCH END] ##########"));
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
		const float Distance = FVector::Dist(Request.ToolOriginWorld, Request.ImpactPoint);
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

	RegisterDecalToCells(Decal, Request);

	return Decal;
}

//////////////////////////////////////////////////////////////////////////
// Chunk Mesh Parallel Processing
//////////////////////////////////////////////////////////////////////////

int32 URealtimeDestructibleMeshComponent::BuildChunksFromGC(UGeometryCollection* InGC)
{
	if (!InGC)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildChunksFromGC: GeometryCollection is not set."));
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
	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GeometryCollectionPtr = InGC->GetGeometryCollection();

	if (!GeometryCollectionPtr.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildChunksFromGC: Invalid GeometryCollection data."));
		return 0;
	}

	const FGeometryCollection& GC = *GeometryCollectionPtr;

	// 만들어진 조각이 없다면 return;
	const int32 NumTransforms = GC.NumElements(FGeometryCollection::TransformGroup);
	if (NumTransforms == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildChunksFromGC: No transforms in GeometryCollection."));
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
			*FString::Printf(TEXT("Chunk_%d"), TransformIdx),
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
		if (bServerCellCollisionInitialized)
		{
			if (GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
			{
				// 서버: Cell Box가 물리 담당 → 원본 메시 콜리전 비활성화
				CellComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			}
			else
			{
				// 클라이언트: Pawn만 Ignore, 나머지 충돌 유지
				CellComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				CellComp->SetCollisionProfileName(TEXT("BlockAll"));
				CellComp->SetComplexAsSimpleCollisionEnabled(true);
				CellComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			}
		}
		else
		{
			CellComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			CellComp->SetCollisionProfileName(TEXT("BlockAll"));
			CellComp->SetComplexAsSimpleCollisionEnabled(true);
		}


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
		const TArray<UMaterialInterface*>& GCMaterials = InGC->Materials;
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
	const TArray<UMaterialInterface*>& GCMaterials = InGC->Materials;
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

		UE_LOG(LogTemp, Log, TEXT("BuildChunksFromGC: Copied %d materials from GeometryCollection"), GCMaterials.Num());
	}

	bChunkMeshesValid = ExtractedCount > 0;

	UE_LOG(LogTemp, Log, TEXT("BuildChunksFromGC: Extracted %d meshes from %d transforms"),
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

		// GridCellLayout 초기화
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
	//const FVector WorldScale = GetComponentScale();
	const FVector WorldScale = GetComponentTransform().GetScale3D();

	// 3. GridCellBuilder를 사용하여 캐시 생성
	// - GridCellSize: 월드 좌표계 기준 (사용자 설정값)
	// - WorldScale: 컴포넌트 스케일 (빌더 내부에서 로컬 변환에 사용)
	// - FloorHeightThreshold: 앵커 판정용 (빌더 내부가 로컬 스페이스이므로 변환 필요)

	// 앵커 에디터에서 설정한 앵커 데이터 백업 (Reset 전에 저장)
	const FIntVector SavedGridSize = GridCellLayout.GridSize;
	const TArray<uint32> SavedAnchorBits = GridCellLayout.CellIsAnchorBits;
	const bool bHadSavedAnchors = SavedAnchorBits.Num() > 0
		&& SavedGridSize.X > 0 && SavedGridSize.Y > 0 && SavedGridSize.Z > 0;

	GridCellLayout.Reset();
	CellState.Reset();

	const float LocalFloorThreshold = FloorHeightThreshold / FMath::Max(WorldScale.Z, KINDA_SMALL_NUMBER);

	const bool bSuccess = FGridCellBuilder::BuildFromStaticMesh(
		SourceStaticMesh,
		WorldScale,           // MeshScale (새 파라미터)
		GridCellSize,         // 월드 스페이스 셀 크기 (빌더가 내부에서 로컬로 변환)
		LocalFloorThreshold,  // 앵커 높이 (빌더 내부가 로컬 스페이스이므로 변환 필요)
		GridCellLayout,
		&CellState.SubCellStates
	);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Warning, TEXT("BuildGridCells: Failed to build grid cells"));
		return false;
	}

	// 앵커 에디터에서 설정한 앵커 데이터 복원
	// 그리드 크기가 동일하고 비트필드 크기가 일치하면 에디터에서 설정한 앵커를 복원
	if (bHadSavedAnchors
		&& GridCellLayout.GridSize == SavedGridSize
		&& SavedAnchorBits.Num() == GridCellLayout.CellIsAnchorBits.Num())
	{
		GridCellLayout.CellIsAnchorBits = SavedAnchorBits;
		UE_LOG(LogTemp, Log, TEXT("BuildGridCells: Restored saved anchor data from Anchor Editor (Anchors: %d)"),
			GridCellLayout.GetAnchorCount());
	}

	// 4. 캐시된 정보 저장
	// CachedMeshBounds = SourceStaticMesh->GetBoundingBox();
	CachedCellSize = GridCellLayout.CellSize;  // 빌더가 저장한 로컬 스페이스 셀 크기
	CachedRDMScale = GetComponentTransform().GetScale3D();

	UE_LOG(LogTemp, Log, TEXT("BuildGridCells: WorldCellSize=(%.1f, %.1f, %.1f), Scale=(%.2f, %.2f, %.2f), LocalCellSize=(%.2f, %.2f, %.2f), Grid %dx%dx%d, Valid cells: %d, Anchors: %d"),
		GridCellSize.X, GridCellSize.Y, GridCellSize.Z,
		WorldScale.X, WorldScale.Y, WorldScale.Z,
		GridCellLayout.CellSize.X, GridCellLayout.CellSize.Y, GridCellLayout.CellSize.Z,
		GridCellLayout.GridSize.X, GridCellLayout.GridSize.Y, GridCellLayout.GridSize.Z,
		GridCellLayout.GetValidCellCount(),
		GridCellLayout.GetAnchorCount());
	
	// 5. SuperCell 상태 빌드 (BFS 최적화용)
	SupercellState.BuildFromGridLayout(GridCellLayout);

	return true;
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

	// GridCellSize가 변경되면 GridCellLayout 자동 재빌드
	if (PropertyName == GET_MEMBER_NAME_CHECKED(URealtimeDestructibleMeshComponent, GridCellSize))
	{
		if (SourceStaticMesh)
		{
			BuildGridCells();
			UE_LOG(LogTemp, Log, TEXT("PostEditChangeProperty: GridCellSize changed to (%.1f, %.1f, %.1f), GridCellLayout rebuilt"),
				GridCellSize.X, GridCellSize.Y, GridCellSize.Z);
		}
	}
}

void URealtimeDestructibleMeshComponent::TryAutoSetupFromParentStaticMesh()
{
	if (!(GIsEditor && GetWorld() && !GetWorld()->IsGameWorld()))
	{
		return;
	}
	
	if (SourceStaticMesh)
	{
		return;
	}

	if (HasAnyFlags(RF_ClassDefaultObject) || IsTemplate() || IsRunningCommandlet())
	{
		return;
	}

	UStaticMeshComponent* ParentStaticMeshComp = Cast<UStaticMeshComponent>(GetAttachParent());
	if (!ParentStaticMeshComp || !ParentStaticMeshComp->GetStaticMesh())
	{
		return;
	}
	SetRelativeTransform(FTransform::Identity);

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("RDM", "Auto Setup", "Setup From ParentSMC"));
	Owner->Modify();
	Modify();
	ParentStaticMeshComp->Modify();

	SourceStaticMesh = ParentStaticMeshComp->GetStaticMesh();
	
	const FVector LocalSize = SourceStaticMesh->GetBoundingBox().GetSize();
	const FVector ParentSizeAbs = ParentStaticMeshComp->GetComponentTransform().GetScale3D().GetAbs();
	const FVector ScaledSize = LocalSize * ParentSizeAbs;
	
	ParentStaticMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ParentStaticMeshComp->SetGenerateOverlapEvents(false);
	ParentStaticMeshComp->SetStaticMesh(nullptr);
	ParentStaticMeshComp->MarkRenderStateDirty();

	if (ChunkMeshComponents.Num() <= 0)
	{
		int32 SliceX = FMath::Clamp(FMath::FloorToInt(ScaledSize.X / 300), 2, 10);
		int32 SliceY = FMath::Clamp(FMath::FloorToInt(ScaledSize.Y / 300), 2, 10);
		int32 SliceZ = FMath::Clamp(FMath::FloorToInt(ScaledSize.Z / 300), 2, 10);
		SliceCount = FIntVector(SliceX, SliceY, SliceZ);
		UE_LOG(LogTemp, Display, TEXT("AutoSetup %s"), *SliceCount.ToString());
		GenerateDestructibleChunks();
	}

	Owner->MarkPackageDirty();
	MarkPackageDirty();

	bAutoSetUpDone = true;
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
void URealtimeDestructibleMeshComponent::CreateDebrisMeshSections(UProceduralMeshComponent* Mesh,
	const TMap<int32, FMeshSectionData>& SectionDataByMaterial,
	const TArray<UMaterialInterface*>& InMaterials)
{
	if (!Mesh)
	{
		return;
	}
	
	for (const auto& Pair : SectionDataByMaterial)
	{
		int32 MaterialId = Pair.Key;
		const FMeshSectionData& SectionData = Pair.Value;

		if (SectionData.Vertices.Num() < 3 || SectionData.Triangles.Num() < 3)
		{
			continue;
		}

		Mesh->CreateMeshSection_LinearColor(
			MaterialId,
			SectionData.Vertices,
			SectionData.Triangles,
			SectionData.Normals,
			SectionData.UVs,
			TArray<FLinearColor>(),
			TArray<FProcMeshTangent>(),
			false
		);

		if (InMaterials.IsValidIndex(MaterialId) && InMaterials[MaterialId])
		{
			Mesh->SetMaterial(MaterialId, InMaterials[MaterialId]);
		}
	}
}

AActor* URealtimeDestructibleMeshComponent::CreateLocalOnlyDebrisActor(UWorld* World, const FVector& SpawnLocation,
	const FVector& BoxExtent, const TMap<int32, FMeshSectionData>& SectionDataByMaterial,  const TArray<UMaterialInterface*>& InMaterials)
{
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* LocalActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator, SpawnParams);
	if (!LocalActor) return nullptr;

	// BoxComponent
	UBoxComponent* CollisionBox = NewObject<UBoxComponent>(LocalActor, TEXT("CollisionBox"));
	CollisionBox->SetBoxExtent(BoxExtent);
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionBox->SetCollisionObjectType(ECC_PhysicsBody);
	CollisionBox->SetCollisionResponseToAllChannels(ECR_Block);
	CollisionBox->SetHiddenInGame(true);
	CollisionBox->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	LocalActor->SetRootComponent(CollisionBox);
	CollisionBox->RegisterComponent();

	// ProceduralMesh
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(LocalActor, TEXT("DebrisMesh"));
	Mesh->SetupAttachment(CollisionBox);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetMobility(EComponentMobility::Movable);

	CreateDebrisMeshSections(Mesh, SectionDataByMaterial, InMaterials);
	
	Mesh->RegisterComponent();
	LocalActor->AddInstanceComponent(Mesh);

	// Transform
	FTransform ComponentTransform = GetComponentTransform();
	LocalActor->SetActorLocation(SpawnLocation);
	LocalActor->SetActorRotation(ComponentTransform.GetRotation());
	LocalActor->SetActorScale3D(ComponentTransform.GetScale3D());

	// Physics
	ApplyDebrisPhysics(CollisionBox, SpawnLocation, BoxExtent);

	// Lifespan
	LocalActor->SetLifeSpan(10.0f);

	return LocalActor;
}
  
void URealtimeDestructibleMeshComponent::ApplyDebrisPhysics(UBoxComponent* CollisionBox, const FVector& SpawnLocation, const FVector& BoxExtent)
{
	if (!CollisionBox)
	{
		return;
	}
	float Volume = 8.0f * BoxExtent.X * BoxExtent.Y * BoxExtent.Z;
	float CalcMassKg = 0.001f * Volume * DebrisDensity;
	float FinalMassKg = FMath::Clamp(CalcMassKg, 0.001, MaxDebrisMass); 
	float MassRatio = 1.0f - (FinalMassKg / MaxDebrisMass);
	MassRatio = std::max(MassRatio, 0.1f);
	
	// 물리 설정
	CollisionBox->SetEnableGravity(true);
	CollisionBox->SetMassOverrideInKg(NAME_None, FinalMassKg, true);
	CollisionBox->SetSimulatePhysics(true);

	// 초기 속도  
	FVector Impulse = -CachedToolForwardVector * 20.0f + FVector(0, 0, 10.0f); 
	CollisionBox->AddImpulse(Impulse);

	FVector RandomAngular(
		FMath::RandRange(-45.0f, 45.0f) * MassRatio,
		FMath::RandRange(-45.0f, 45.0f) * MassRatio,
		FMath::RandRange(-45.0f, 45.0f) * MassRatio );
	CollisionBox->SetPhysicsAngularVelocityInDegrees(RandomAngular);
}


#if WITH_EDITOR
void URealtimeDestructibleMeshComponent::GenerateDestructibleChunks()
{
	UStaticMesh* InStaticMesh = SourceStaticMesh.Get();
	if (!InStaticMesh)
	{
		return;
	}

	TObjectPtr<UGeometryCollection> GC = CreateFracturedGC(InStaticMesh);
	if (!GC)
	{
		return;
	}

	int32 CellCount = BuildChunksFromGC(GC);

	if (AActor* Owner = GetOwner())
	{
		Owner->Modify();
		Owner->RerunConstructionScripts();

		// Refresh detail panel to show newly created ChunkMeshComponents
		if (GUnrealEd)
		{
			GUnrealEd->UpdateFloatingPropertyWindows();
		}
	}
}

TObjectPtr<UGeometryCollection> URealtimeDestructibleMeshComponent::CreateFracturedGC(TObjectPtr<UStaticMesh> InSourceMesh)
{
	if (!InSourceMesh)
	{
		return nullptr;
	}

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
		return nullptr;
	}
	Package->FullyLoad();

	UGeometryCollection* GeometryCollection = NewObject<UGeometryCollection>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone
	);
	if (!GeometryCollection)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateFracturedGC: Failed to create GeometryCollection"));
		return nullptr;
	}

	TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GCPtr = GeometryCollection->GetGeometryCollection();
	if (!GCPtr.IsValid())
	{
		GCPtr = MakeShared<FGeometryCollection>();
		GeometryCollection->SetGeometryCollection(GCPtr);
	}

	// Source Static Mesh를 GC에 추가 (단일 조각으로 시작)
	TArray<UMaterialInterface*> Materials;
	for (const FStaticMaterial& StaticMat : InSourceMesh->GetStaticMaterials())
	{
		Materials.Add(StaticMat.MaterialInterface);
	}
	FGeometryCollectionConversion::AppendStaticMesh(
		InSourceMesh,
		Materials,
		FTransform::Identity,
		GeometryCollection,
		true
	);

	GCPtr = GeometryCollection->GetGeometryCollection();
	if (!GCPtr.IsValid())
	{
		return nullptr;
	}

	// SliceCutter로 GC 격자 형태로 자르기
	FDataflowTransformSelection TransformSelection;
	TransformSelection.InitializeFromCollection(*GCPtr, true);
	FBox BoundingBox = InSourceMesh->GetBoundingBox();

	// 주의: 깔끔한 육면체 절단을 위해 노이즈 관련 값은 0으로 둡니다.
	int32 NumCreated = FFractureEngineFracturing::SliceCutter(
		*GCPtr.Get(),           // 레퍼런스로 전달 (&InOutCollection)
		TransformSelection,     // 선택 영역
		BoundingBox,            // 자를 범위
		SliceCount.X - 1,       // X축 몇번 자를 지
		SliceCount.Y - 1,       // Y축 몇번 자를 지
		SliceCount.Z - 1,       // Z축 몇번 자를 지
		0.0f,                   // 0 이면 수직, 수평
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
		false,                  // AddSamplesForCollision
		0.0f
	);
	if (NumCreated <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("CreateFracturedGC: SliceCutter failed, returned %d"), NumCreated);
		return nullptr;
	}

	// 후처리로 데이터 무결성 갱신
	GeometryCollection->Materials = Materials;
	GeometryCollection->InvalidateCollection();

	GCPtr = GeometryCollection->GetGeometryCollection();
	GCPtr->UpdateBoundingBox();

	GeometryCollection->PostEditChange();

	// 에셋 저장
	FAssetRegistryModule::AssetCreated(GeometryCollection);
	GeometryCollection->MarkPackageDirty();
	Package->MarkPackageDirty();

	FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension()
	);

	FString DirectoryPath = FPaths::GetPath(PackageFileName);
	if (!IFileManager::Get().DirectoryExists(*DirectoryPath))
	{
		IFileManager::Get().MakeDirectory(*DirectoryPath, true);
	}

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
		UE_LOG(LogTemp, Warning, TEXT("CreateFracturedGC: Failed to save GeometryCollection: %s"), *PackageFileName);
	}

	return GeometryCollection;
}

void URealtimeDestructibleMeshComponent::RevertChunksToSourceMesh()
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
	CachedGeometryCollection = nullptr;

	bChunkMeshesValid = false;
	SetSourceMeshEnabled(true);

	ResetToSourceMesh();

	// Editor 강제 갱신
	if (AActor* Owner = GetOwner())
	{
		Owner->RerunConstructionScripts();

		// Refresh detail panel to remove destroyed ChunkMeshComponents
		if (GUnrealEd)
		{
			GUnrealEd->UpdateFloatingPropertyWindows();
		}
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

		// GridCellLayout 보존 (Blueprint 재구성 시 앵커 데이터 유실 방지)
		SavedGridCellLayout = SourceComponent->GridCellLayout;

		// CachedRDMScale 보존 (Blueprint 재구성 후 BeginPlay에서 불필요한 BuildGridCells 방지)
		SavedCachedRDMScale = SourceComponent->CachedRDMScale;

		UE_LOG(LogTemp, Warning, TEXT("InstanceData Constructor: bCellMeshesValid=%d, CellMeshComponents.Num=%d, SavedNames.Num=%d, GridValid=%d, Anchors=%d, CachedScale=(%.2f,%.2f,%.2f)"),
			bSavedChunkMeshesValid, SourceComponent->ChunkMeshComponents.Num(), SavedChunkComponentNames.Num(),
			SavedGridCellLayout.IsValid() ? 1 : 0, SavedGridCellLayout.GetAnchorCount(),
			SavedCachedRDMScale.X, SavedCachedRDMScale.Y, SavedCachedRDMScale.Z);
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
		DestructComp->bIsInitialized = bSavedIsInitialized;

		// GridCellLayout 복원 (Blueprint 재구성 시 앵커 데이터 유실 방지)
		if (SavedGridCellLayout.IsValid())
		{
			DestructComp->GridCellLayout = SavedGridCellLayout;
			UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: Restored GridCellLayout from InstanceData (ValidCells=%d, Anchors=%d)"),
				DestructComp->GridCellLayout.GetValidCellCount(), DestructComp->GridCellLayout.GetAnchorCount());
		}

		// CachedRDMScale 복원 (BeginPlay에서 스케일 불일치로 인한 불필요한 BuildGridCells 호출 방지)
		DestructComp->CachedRDMScale = SavedCachedRDMScale;

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

			// GridCellLayout는 저장되므로, 유효하지 않을 때만 재구축
			if (!DestructComp->GridCellLayout.IsValid())
			{
				UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: GridCellLayout is invalid, rebuilding..."));
			DestructComp->BuildGridCells();
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("ApplyToComponent: GridCellLayout loaded from saved data (ValidCells=%d)"),
					DestructComp->GridCellLayout.GetValidCellCount());
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

	// 사거리 체크
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

