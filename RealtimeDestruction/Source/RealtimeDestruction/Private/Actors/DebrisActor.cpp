// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "Actors/DebrisActor.h"
#include "Components/RealtimeDestructibleMeshComponent.h"
#include "Components/BoxComponent.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "TimerManager.h" 

#include "BoxTypes.h"
#include "IndexTypes.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"

ADebrisActor::ADebrisActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// Network Settings
	bReplicates = true;
	SetReplicateMovement(true); // Transform 자동 동기화

	// BoxComponent가 Root - 물리 시뮬레이션 담당
	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	RootComponent = CollisionBox;

	CollisionBox->SetBoxExtent(FVector(1.0f, 1.0f, 1.0f)); // 기본 크기
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionBox->SetCollisionObjectType(ECC_PhysicsBody);
	CollisionBox->SetCollisionResponseToAllChannels(ECR_Block);
	
	CollisionBox->SetSimulatePhysics(false); // 나중에 EnablePhysics에서 활성화
	CollisionBox->SetEnableGravity(true);
	CollisionBox->SetHiddenInGame(true); // Box는 보이지 않게

	// ProceduralMesh - 시각적 표현만 담당
	DebrisMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("DebrisMesh"));
	DebrisMesh->SetupAttachment(CollisionBox);
	DebrisMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); // Collision은 Box가 담당

	// Default Values
	DebrisId = INDEX_NONE;
	SourceChunkIndex = INDEX_NONE;
	SourceMeshOwner = nullptr;
	DebrisMaterial = nullptr;
	DebrisLifetime = 10.0f;
	bMeshReady = false;
}

void ADebrisActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Init Only
	DOREPLIFETIME_CONDITION(ADebrisActor, DebrisId, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(ADebrisActor, SourceMeshOwner, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(ADebrisActor, SourceChunkIndex, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(ADebrisActor, DebrisMaterial, COND_InitialOnly);

	// 비트맵 압축 데이터 (CellIds 대신)
	DOREPLIFETIME_CONDITION(ADebrisActor, CellBoundsMin, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(ADebrisActor, CellBoundsMax, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(ADebrisActor, CellBitmap, COND_InitialOnly);
}

void ADebrisActor::OnRep_DebrisParams()
{
	if (bMeshReady)
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] OnRep_DebrisParams: DebrisId=%d, BitmapSize=%d, Material=%s"),
		DebrisId, CellBitmap.Num(), DebrisMaterial ? *DebrisMaterial->GetName() : TEXT("NULL"));

	// 1. 로컬 메시 매칭 시도 (Listen 서버 클라이언트)
	UProceduralMeshComponent* LocalMesh = FindLocalDebrisMesh(DebrisId);

	if (LocalMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] Matching Success! DebrisId=%d"), DebrisId);

		ApplyLocalMesh(LocalMesh);

		// 로컬 메시의 Owner(TempActor) 삭제
		AActor* TempActor = LocalMesh->GetOwner();
		if (TempActor && TempActor != this)
		{
			TempActor->Destroy();
		}

		bMeshReady = true;
	}
	// 2. 비트맵이 있으면 → CellIds 디코딩 → 메시 생성 (데디서버 클라이언트)
	else if (CellBitmap.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] Decoding bitmap and generating mesh - DebrisId=%d, BitmapBytes=%d"),
			DebrisId, CellBitmap.Num());

		URealtimeDestructibleMeshComponent* SourceMesh = GetSourceMeshComponent();
		if (SourceMesh)
		{
			// 비트맵 → CellIds 디코딩
			DecodeBitmapToCells(SourceMesh->GetGridCellLayout());

			if (CellIds.Num() > 0)
			{
				// Boolean Intersection 시도 (Standalone 품질)
				// BooleanProcessor가 유효하고 ChunkMesh가 있을 때만 사용
				bool bUseBooleanExtraction = SourceMesh->CanExtractDebrisForClient();

				if (bUseBooleanExtraction)
				{
					// RemoveTrianglesForDetachedCells에 TargetDebrisActor를 전달하여
					// Subtract + Intersection 모두 수행 (Standalone과 동일)
					UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] Using RemoveTrianglesForDetachedCells for mesh extraction (Standalone quality)"));
					SourceMesh->RemoveTrianglesForDetachedCells(CellIds, this);
				}
				else
				{
					// Fallback: Greedy Mesh 사용 (기존 방식)
					UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] Fallback to GenerateMeshFromCells (BooleanProcessor unavailable)"));
					GenerateMeshFromCells();
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] DecodeBitmapToCells resulted in 0 cells"));
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[DebrisActor] SourceMeshComponent is null! Cannot decode bitmap - DebrisId=%d"), DebrisId);
		}
	}
	// 3. 둘 다 없으면 대기열에 등록
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] No local mesh or bitmap, registering as pending - DebrisId=%d"), DebrisId);

		URealtimeDestructibleMeshComponent* SourceMesh = GetSourceMeshComponent();
		if (SourceMesh)
		{
			SourceMesh->RegisterPendingDebrisActor(DebrisId, this);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[DebrisActor] SourceMeshComponent is null! Cannot register pending - DebrisId=%d"), DebrisId);
		}
	}
}

UProceduralMeshComponent* ADebrisActor::FindLocalDebrisMesh(int32 InDebrisId)
{
	URealtimeDestructibleMeshComponent* SourceMesh = GetSourceMeshComponent();
	if (SourceMesh)
	{
		return SourceMesh->FindAndRemoveLocalDebris(InDebrisId);
	}
	return nullptr;
}

void ADebrisActor::OnLifetimeExpired()
{
	Destroy();
}

void ADebrisActor::BeginPlay()
{
	Super::BeginPlay();

	// 수명 타이머 (서버에서만)
	if (HasAuthority() && DebrisLifetime > 0.0f)
	{
		FTimerHandle TimerHandle;
		GetWorld()->GetTimerManager().SetTimer(
			TimerHandle,
			this,
			&ADebrisActor::OnLifetimeExpired,
			DebrisLifetime,
			false);
	}
}

// 서버 전용 Methods 
void ADebrisActor::SetMeshDirectly(const TArray<FVector>& Vertices, const TArray<int32>& Triangles,
	const TArray<FVector>& Normals, const TArray<FVector2D>& UVs)
{
	if (!DebrisMesh)
	{
		return;
	}

	TArray<FLinearColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;

	DebrisMesh->CreateMeshSection_LinearColor(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		VertexColors,
		Tangents,
		true  // bCreateCollision
	);

	if (DebrisMaterial)
	{
		DebrisMesh->SetMaterial(0, DebrisMaterial);
	}

	bMeshReady = true;
}

void ADebrisActor::GenerateMeshFromCells()
{
	if (CellIds.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] GenerateMeshFromCells: No CellIds"));
		return;
	}

	URealtimeDestructibleMeshComponent* SourceMesh = GetSourceMeshComponent();
	if (!SourceMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] GenerateMeshFromCells: SourceMesh is null"));
		return;
	}

	if (!DebrisMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] GenerateMeshFromCells: DebrisMesh is null"));
		return;
	}
	
	const FGridCellLayout& GridLayout = SourceMesh->GetGridCellLayout();
	if (!GridLayout.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] GenerateMeshFromCells: GridLayout is invalid"));
		return;
	}

	using namespace UE::Geometry;
	// CellIds를 IntVector(Grid 좌표)로 변환
	TArray<FIntVector> Voxels;
	Voxels.Reserve(CellIds.Num());

	const FVector& CellSize = GridLayout.CellSize;
	const FVector& Origin = GridLayout.GridOrigin;

	for (int32 CellId : CellIds)
	{
		FVector LocalMin = GridLayout.IdToLocalMin(CellId);
		FIntVector GridPos(
			FMath::FloorToInt((LocalMin.X - Origin.X) / CellSize.X),
			FMath::FloorToInt((LocalMin.Y - Origin.Y) / CellSize.Y),
			FMath::FloorToInt((LocalMin.Z - Origin.Z) / CellSize.Z)
		);
		Voxels.Add(GridPos);
	}

	float BoxExpand = 1.0f;
	// GreedyMesh 생성 (SourceMesh의 함수 사용)
	FDynamicMesh3 GeneratedMesh = SourceMesh->GenerateGreedyMeshFromVoxels(
		Voxels,
		Origin,
		CellSize,
		BoxExpand
	);

	if (GeneratedMesh.TriangleCount() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DebrisActor] GenerateMeshFromCells: Generated mesh is empty"));
		return;
	} 

	// Winding order 뒤집기 (GenerateGreedyMeshFromVoxels는 내부를 향하는 방향으로 생성)
	GeneratedMesh.ReverseOrientation();

	
	// FDynamicMesh3 -> ProceduralMeshComponent로 변환
	FAxisAlignedBox3d MeshBounds = GeneratedMesh.GetBounds();
	FVector3d MeshCenter = MeshBounds.Center();

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;

	// Flat shading: 삼각형마다 별도의 버텍스를 생성하여 면 노말 사용
	for (int32 TriId : GeneratedMesh.TriangleIndicesItr())
	{
		FIndex3i Tri = GeneratedMesh.GetTriangle(TriId);

		// 삼각형의 세 꼭짓점 위치
		FVector3d P0 = GeneratedMesh.GetVertex(Tri.A) - MeshCenter;
		FVector3d P1 = GeneratedMesh.GetVertex(Tri.B) - MeshCenter;
		FVector3d P2 = GeneratedMesh.GetVertex(Tri.C) - MeshCenter;

		// 면 노말 계산
		FVector3d Edge1 = P1 - P0;
		FVector3d Edge2 = P2 - P0;
		FVector FaceNormal = FVector(Edge1 ^ Edge2).GetSafeNormal();

		// 세 버텍스 추가 (각 삼각형마다 고유 버텍스)
		int32 BaseIdx = Vertices.Num();

		Vertices.Add(FVector(P0));
		Vertices.Add(FVector(P1));
		Vertices.Add(FVector(P2));

		Normals.Add(FaceNormal);
		Normals.Add(FaceNormal);
		Normals.Add(FaceNormal);

		UVs.Add(FVector2D::ZeroVector);
		UVs.Add(FVector2D::ZeroVector);
		UVs.Add(FVector2D::ZeroVector);

		Triangles.Add(BaseIdx);
		Triangles.Add(BaseIdx + 1);
		Triangles.Add(BaseIdx + 2);
	}

	// ProceduralMesh에 적용
	DebrisMesh->CreateMeshSection_LinearColor(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		VertexColors,
		Tangents,
		false  // bCreateCollision - 클라이언트는 불필요
	);

	// 머티리얼 적용
	if (DebrisMaterial)
	{
		DebrisMesh->SetMaterial(0, DebrisMaterial);
	}

	bMeshReady = true;

}

URealtimeDestructibleMeshComponent* ADebrisActor::GetSourceMeshComponent() const
{
	if (!SourceMeshOwner)
	{
		return nullptr;
	}

	return SourceMeshOwner->FindComponentByClass<URealtimeDestructibleMeshComponent>();
}

void ADebrisActor::InitializeDebris(int32 InDebrisId, const TArray<int32>& InCellIds, int32 InChunkIndex,
                                    URealtimeDestructibleMeshComponent* InSourceMesh, UMaterialInterface* InMaterial)
{
	if (!HasAuthority())
	{
		return;
	}

	DebrisId = InDebrisId;
	SourceChunkIndex = InChunkIndex;
	SourceMeshOwner = InSourceMesh ? InSourceMesh->GetOwner() : nullptr;
	DebrisMaterial = InMaterial;

	// CellIds가 있으면 비트맵으로 인코딩 (복제용)
	if (InCellIds.Num() > 0 && InSourceMesh)
	{
		CellIds = InCellIds;  // 서버 로컬용
		EncodeCellsToBitmap(InCellIds, InSourceMesh->GetGridCellLayout());

		UE_LOG(LogTemp, Log, TEXT("[DebrisActor] InitializeDebris: DebrisId=%d, CellIds=%d -> Bitmap=%d bytes"),
			DebrisId, InCellIds.Num(), CellBitmap.Num());
	}
}

void ADebrisActor::SetCollisionBoxExtent(const FVector& Extent)
{
	if (CollisionBox)
	{
		CollisionBox->SetBoxExtent(Extent);
	}
}

void ADebrisActor::EnablePhysics()
{
	if (!CollisionBox)
	{
		return;
	}

	// 디버그: BodyInstance 상태 확인
	FBodyInstance* BodyInst = CollisionBox->GetBodyInstance();
	UE_LOG(LogTemp, Warning, TEXT("[Debris] EnablePhysics - BoxComponent BodyInstance: %s"),
		BodyInst ? TEXT("EXISTS") : TEXT("NULL"));

	if (BodyInst)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Debris] EnablePhysics - IsValidBodyInstance: %s"),
			BodyInst->IsValidBodyInstance() ? TEXT("YES") : TEXT("NO"));
	}

	// 중력 활성화
	CollisionBox->SetEnableGravity(true);

	// Mass 설정
	CollisionBox->SetMassOverrideInKg(NAME_None, 10.0f, true);

	// 물리 시뮬레이션 활성화
	CollisionBox->SetSimulatePhysics(true);

	// 디버그: 활성화 후 상태 확인
	UE_LOG(LogTemp, Warning, TEXT("[Debris] EnablePhysics - After SetSimulatePhysics: IsSimulating=%s, Gravity=%s"),
		CollisionBox->IsSimulatingPhysics() ? TEXT("YES") : TEXT("NO"),
		CollisionBox->IsGravityEnabled() ? TEXT("YES") : TEXT("NO"));
}

void ADebrisActor::ApplyLocalMesh(UProceduralMeshComponent* LocalMesh)
{
	if (!LocalMesh || !DebrisMesh)
	{
		return;
	}
	
	// 로컬 메시의 섹션 데이터를 DebrisMesh로 복사
	int32 NumSections = LocalMesh->GetNumSections();
	for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
	{
		FProcMeshSection* Section = LocalMesh->GetProcMeshSection(SectionIndex);
		if (Section && Section->ProcVertexBuffer.Num() > 0)
		{
			// 버텍스 데이터 추출
			TArray<FVector> Vertices;
			TArray<FVector> Normals;
			TArray<FVector2D> UVs;
			TArray<FLinearColor> VertexColors;
			TArray<FProcMeshTangent> Tangents;

			Vertices.Reserve(Section->ProcVertexBuffer.Num());
			Normals.Reserve(Section->ProcVertexBuffer.Num());
			UVs.Reserve(Section->ProcVertexBuffer.Num());

			for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
			{
				Vertices.Add(Vertex.Position);
				Normals.Add(Vertex.Normal);
				UVs.Add(Vertex.UV0);
			}
				
			TArray<int32> Triangles;
			Triangles.Reserve(Section->ProcIndexBuffer.Num());
			for (uint32 Index : Section->ProcIndexBuffer)
			{
				Triangles.Add(static_cast<int32>(Index));
			}
				
			// 메시 섹션 생성
			DebrisMesh->CreateMeshSection_LinearColor(
				SectionIndex,
				Vertices,
				Triangles,
				Normals,
				UVs,
				VertexColors,
				Tangents,
				false  // bCreateCollision - 클라이언트는 collision 불필요
			);

			// 머티리얼 복사
			UMaterialInterface* SectionMaterial = LocalMesh->GetMaterial(SectionIndex);
			if (SectionMaterial)
			{
				DebrisMesh->SetMaterial(SectionIndex, SectionMaterial);
			}
		}
	}

	bMeshReady = true;
    UE_LOG(LogTemp, Warning, TEXT("[Debris Actor] ApplyLocalMesh completed - DebrisId=%d"), DebrisId);
}

void ADebrisActor::EncodeCellsToBitmap(const TArray<int32>& InCellIds, const FGridCellLayout& GridLayout)
{
	if (InCellIds.Num() == 0)
	{
		return;
	}

	const FVector& CellSize = GridLayout.CellSize;
	const FVector& Origin = GridLayout.GridOrigin;

	// 1. 모든 CellId를 그리드 좌표로 변환하고 바운딩 박스 계산
	TArray<FIntVector> GridPositions;
	GridPositions.Reserve(InCellIds.Num());

	FIntVector MinBounds(TNumericLimits<int32>::Max());
	FIntVector MaxBounds(TNumericLimits<int32>::Lowest());

	for (int32 CellId : InCellIds)
	{
		FVector LocalMin = GridLayout.IdToLocalMin(CellId);
		FIntVector GridPos(
			FMath::FloorToInt((LocalMin.X - Origin.X) / CellSize.X),
			FMath::FloorToInt((LocalMin.Y - Origin.Y) / CellSize.Y),
			FMath::FloorToInt((LocalMin.Z - Origin.Z) / CellSize.Z)
		);
		GridPositions.Add(GridPos);

		MinBounds.X = FMath::Min(MinBounds.X, GridPos.X);
		MinBounds.Y = FMath::Min(MinBounds.Y, GridPos.Y);
		MinBounds.Z = FMath::Min(MinBounds.Z, GridPos.Z);
		MaxBounds.X = FMath::Max(MaxBounds.X, GridPos.X);
		MaxBounds.Y = FMath::Max(MaxBounds.Y, GridPos.Y);
		MaxBounds.Z = FMath::Max(MaxBounds.Z, GridPos.Z);
	}

	CellBoundsMin = MinBounds;
	CellBoundsMax = MaxBounds;

	// 2. 바운딩 박스 크기 계산
	int32 SizeX = MaxBounds.X - MinBounds.X + 1;
	int32 SizeY = MaxBounds.Y - MinBounds.Y + 1;
	int32 SizeZ = MaxBounds.Z - MinBounds.Z + 1;
	int32 TotalBits = SizeX * SizeY * SizeZ;
	int32 TotalBytes = (TotalBits + 7) / 8;  // 올림

	// 3. 비트맵 초기화 (모두 0)
	CellBitmap.SetNumZeroed(TotalBytes);

	// 4. 각 셀 위치에 비트 설정
	for (const FIntVector& GridPos : GridPositions)
	{
		// 로컬 좌표로 변환 (바운딩 박스 기준)
		int32 LocalX = GridPos.X - MinBounds.X;
		int32 LocalY = GridPos.Y - MinBounds.Y;
		int32 LocalZ = GridPos.Z - MinBounds.Z;

		// 1D 인덱스 계산 (X-major ordering)
		int32 BitIndex = LocalX + LocalY * SizeX + LocalZ * SizeX * SizeY;

		// 비트 설정
		int32 ByteIndex = BitIndex / 8;
		int32 BitOffset = BitIndex % 8;
		if (ByteIndex < CellBitmap.Num())
		{
			CellBitmap[ByteIndex] |= (1 << BitOffset);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[DebrisActor] EncodeCellsToBitmap: %d cells -> %d bytes (Bounds: %s to %s)"),
		InCellIds.Num(), CellBitmap.Num(), *MinBounds.ToString(), *MaxBounds.ToString());
}

void ADebrisActor::DecodeBitmapToCells(const FGridCellLayout& GridLayout)
{
	if (CellBitmap.Num() == 0)
	{
		return;
	}

	// 바운딩 박스 크기
	int32 SizeX = CellBoundsMax.X - CellBoundsMin.X + 1;
	int32 SizeY = CellBoundsMax.Y - CellBoundsMin.Y + 1;
	int32 SizeZ = CellBoundsMax.Z - CellBoundsMin.Z + 1;
	int32 TotalBits = SizeX * SizeY * SizeZ;

	CellIds.Empty();
	CellIds.Reserve(TotalBits / 4);  // 대략적인 예상 크기

	// 비트맵 순회하며 CellIds 복원
	for (int32 BitIndex = 0; BitIndex < TotalBits; ++BitIndex)
	{
		int32 ByteIndex = BitIndex / 8;
		int32 BitOffset = BitIndex % 8;

		if (ByteIndex >= CellBitmap.Num())
		{
			break;
		}

		// 비트가 설정되어 있으면 해당 셀 존재
		if (CellBitmap[ByteIndex] & (1 << BitOffset))
		{
			// 1D 인덱스 → 3D 로컬 좌표
			int32 LocalX = BitIndex % SizeX;
			int32 LocalY = (BitIndex / SizeX) % SizeY;
			int32 LocalZ = BitIndex / (SizeX * SizeY);

			// 글로벌 그리드 좌표
			FIntVector GridPos(
				CellBoundsMin.X + LocalX,
				CellBoundsMin.Y + LocalY,
				CellBoundsMin.Z + LocalZ
			);

			// 그리드 좌표 → CellId (CoordToId 사용)
			if (GridLayout.IsValidCoord(GridPos))
			{
				int32 CellId = GridLayout.CoordToId(GridPos);
				CellIds.Add(CellId);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[DebrisActor] DecodeBitmapToCells: %d bytes -> %d cells"),
		CellBitmap.Num(), CellIds.Num());
}
