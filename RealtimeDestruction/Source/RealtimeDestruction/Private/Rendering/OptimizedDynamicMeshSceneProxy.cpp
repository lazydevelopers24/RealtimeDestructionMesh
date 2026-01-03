#include "OptimizedDynamicMeshSceneProxy.h"

FOptimizedDynamicMeshSceneProxy::FOptimizedDynamicMeshSceneProxy(UDynamicMeshComponent* Component)
    : FMyDynamicMeshSceneProxy(Component)
{
    // 여기에 초기화 코드 추가
    // 예: UE_LOG(LogTemp, Warning, TEXT("FOptimizedDynamicMeshSceneProxy Created"));

	CurrentValidVertexCount = 0;
	CurrentValidIndexCount = 0;
} 

void FOptimizedDynamicMeshSceneProxy::Initialize()
{
    FDynamicMesh3* Mesh = ParentComponent->GetRenderMesh();
    if (!Mesh)
    {
        return;
    }

    // 버퍼 세트 생성
    // TODO: 현재 Material을 하나만 사용한다는 가정으로 작성 중 (MVP)
    RenderBufferSets.SetNum(1);
    RenderBufferSets[0] = AllocateNewRenderBufferSet();
    FMeshRenderBufferSet* Buffers = RenderBufferSets[0];

    // Material 설정
    Buffers->Material = (ParentComponent->GetNumMaterials() > 0)
        ? ParentComponent->GetMaterial(0)
        : UMaterial::GetDefaultMaterial(MD_Surface);


    // Overlay 정보 가져오기 
    TArray<const FDynamicMeshUVOverlay*, TInlineAllocator<8>> UVOverlays;
    const FDynamicMeshNormalOverlay* NormalOverlay = nullptr;
    const FDynamicMeshColorOverlay* ColorOverlay = nullptr;

    if (Mesh->HasAttributes())
    {
        const FDynamicMeshAttributeSet* Attributes = Mesh->Attributes();
        NormalOverlay = Attributes->PrimaryNormals();
        ColorOverlay = Attributes->PrimaryColors();

        UVOverlays.SetNum(Attributes->NumUVLayers());
        for (int32 k = 0; k < UVOverlays.Num(); ++k)
        {
            UVOverlays[k] = Attributes->GetUVLayer(k);
        }
    }

    // Tangent 생성 함수
    TUniqueFunction<void(int, int, int, const FVector3f&, FVector3f&, FVector3f&)>
        TangentsFunc = MakeTangentsFunc();


    // InitializeBuffersFromOverlays 호출 
    // 이 함수가 Position, Normal, Index 모두 처리
    const bool bTrackTriangles = false;
    const bool bParallel = true;

    InitializeBuffersFromOverlays(
        Buffers,
        Mesh,
        Mesh->TriangleCount(),          // 현재 Triangle 개수
        Mesh->TriangleIndicesItr(),     // Triangle Iterator
        UVOverlays,
        NormalOverlay,
        ColorOverlay,
        TangentsFunc,
        bTrackTriangles,
        bParallel
    );

    // ========== 이 시점에서 Buffers에 정확한 크기의 데이터가 모두 채워져 있음
    // (Position, Normal, UV, Color, Index) 

    // 현재 크기 저장
    int32 CurrentVertexCount = Buffers->PositionVertexBuffer.GetNumVertices();
    int32 CurrentIndexCount = Buffers->IndexBuffer.Indices.Num();

    CurrentValidVertexCount = CurrentVertexCount;
    CurrentValidIndexCount = CurrentIndexCount;


    // 버퍼 확장 (크기만 키우고 데이터는 유지)
    // TODO: ExtraCapacity를 조정할 수 있도록 인자로 빼면 좋을 듯
    MaxVertexCapacity = CurrentVertexCount + ExtraVertexCapacity;
    MaxIndexCapacity = CurrentIndexCount + ExtraIndexCapacity;

    // Position 버퍼 확장
    {
        // 기존 데이터 백업
        TArray<FVector3f> BackupPositions;
        BackupPositions.SetNum(CurrentVertexCount);

        for (int32 i = 0; i < CurrentVertexCount; ++i)
        {
            BackupPositions[i] = Buffers->PositionVertexBuffer.VertexPosition(i);
        }

        // 큰 크기로 재생성
        Buffers->PositionVertexBuffer.Init(MaxVertexCapacity);

        // 데이터 복원
        for (int32 i = 0; i < CurrentVertexCount; ++i)
        {
            Buffers->PositionVertexBuffer.VertexPosition(i) = BackupPositions[i];
        }

        // 나머지 공간을 더미 데이터로 채우기 ( GPU 버퍼 크기 확보용 ) 
        for (int32 i = CurrentVertexCount; i < MaxVertexCapacity; ++i)
        {
            Buffers->PositionVertexBuffer.VertexPosition(i) = FVector3f::ZeroVector;
        }
    }

    // StaticMeshVertexBuffer 확장 (Normal, Tangent, UV 포함)
    {
        int32 NumTexCoords = Buffers->StaticMeshVertexBuffer.GetNumTexCoords();

        // 기존 데이터 백업
        struct FVertexData
        {
            FVector3f TangentX;
            FVector3f TangentY;
            FVector3f TangentZ;  // Normal
            TArray<FVector2f> UVs;
        };

        TArray<FVertexData> BackupData;
        BackupData.SetNum(CurrentVertexCount);

        for (int32 i = 0; i < CurrentVertexCount; ++i)
        {
            BackupData[i].TangentX = Buffers->StaticMeshVertexBuffer.VertexTangentX(i);
            BackupData[i].TangentY = Buffers->StaticMeshVertexBuffer.VertexTangentY(i);
            BackupData[i].TangentZ = Buffers->StaticMeshVertexBuffer.VertexTangentZ(i);

            BackupData[i].UVs.SetNum(NumTexCoords);
            for (int32 UVIdx = 0; UVIdx < NumTexCoords; ++UVIdx)
            {
                BackupData[i].UVs[UVIdx] = Buffers->StaticMeshVertexBuffer.GetVertexUV(i, UVIdx);
            }
        }

        // 큰 크기로 재생성
        Buffers->StaticMeshVertexBuffer.Init(MaxVertexCapacity, NumTexCoords);

        // 데이터 복원
        for (int32 i = 0; i < CurrentVertexCount; ++i)
        {
            Buffers->StaticMeshVertexBuffer.SetVertexTangents(
                i,
                BackupData[i].TangentX,
                BackupData[i].TangentY,
                BackupData[i].TangentZ  // Normal
            );

            for (int32 UVIdx = 0; UVIdx < NumTexCoords; ++UVIdx)
            {
                Buffers->StaticMeshVertexBuffer.SetVertexUV(i, UVIdx, BackupData[i].UVs[UVIdx]);
            }
        }

         // 나머지 공간을 더미 데이터로 채움 => GPU 공간 확보용
        for (int32 i = CurrentVertexCount; i < MaxVertexCapacity; ++i)
        {
            Buffers->StaticMeshVertexBuffer.SetVertexTangents(
                i,
                FVector3f::ForwardVector,  // TangentX
                FVector3f::RightVector,    // TangentY
                FVector3f::UpVector        // Normal
            );

            for (int32 UVIdx = 0; UVIdx < NumTexCoords; ++UVIdx)
            {
                Buffers->StaticMeshVertexBuffer.SetVertexUV(i, UVIdx, FVector2f::ZeroVector);
            }
        }
    }

    // ColorVertexBuffer 확장
    {
        TArray<FColor> BackupColors;
        BackupColors.SetNum(CurrentVertexCount);
        for (int32 i = 0; i < CurrentVertexCount; ++i)
        {
            BackupColors[i] = Buffers->ColorVertexBuffer.VertexColor(i);
        }

        Buffers->ColorVertexBuffer.Init(MaxVertexCapacity);

        for (int32 i = 0; i < CurrentVertexCount; ++i)
        {
            Buffers->ColorVertexBuffer.VertexColor(i) = BackupColors[i];
        }

        // 나머지를 기본 색상으로 채움 => GPU 공간 확보용
        for (int32 i = CurrentVertexCount; i < MaxVertexCapacity; ++i)
        {
            Buffers->ColorVertexBuffer.VertexColor(i) = FColor::White;
        }

    }

    // Index 버퍼 확장 (Reserve만)
    {
        // Index는 TArray이므로 Reserve만 하면 됨
        //Buffers->IndexBuffer.Indices.Reserve(MaxIndexCapacity);
        Buffers->IndexBuffer.Indices.SetNum(MaxIndexCapacity);
    }


    // GPU 업로드
   // ENQUEUE_RENDER_COMMAND(InitOptimizedBuffers)(
   //     [Buffers](FRHICommandListImmediate& RHICmdList)
   //     {
   //         // Upload()가 모든 버퍼를 GPU로 전송
   //         Buffers->Upload();
   //
   //     } 
   //    );

    ENQUEUE_RENDER_COMMAND(InitOptimizedBuffers)(
        [Buffers, MaxVtx = MaxVertexCapacity, MaxIdx = MaxIndexCapacity](FRHICommandListImmediate& RHICmdList)
        {
            Buffers->Upload();

            // ✅ 실제 GPU 버퍼 크기 확인
            if (Buffers->PositionVertexBuffer.VertexBufferRHI.IsValid())
            {
                uint32 GPUBufferSize = Buffers->PositionVertexBuffer.VertexBufferRHI->GetSize();
                uint32 ExpectedSize = MaxVtx * sizeof(FVector3f);

                UE_LOG(LogTemp, Error, TEXT("=== GPU Buffer Size Verification ==="));
                UE_LOG(LogTemp, Error, TEXT("Position Buffer - GPU: %u bytes (%d vertices)"),
                    GPUBufferSize, GPUBufferSize / sizeof(FVector3f));
                UE_LOG(LogTemp, Error, TEXT("Position Buffer - Expected: %u bytes (%d vertices)"),
                    ExpectedSize, MaxVtx);
                UE_LOG(LogTemp, Error, TEXT("Match: %s"),
                    (GPUBufferSize == ExpectedSize) ? TEXT("✅ YES") : TEXT("❌ NO"));
            }

            if (Buffers->IndexBuffer.IndexBufferRHI.IsValid())
            {
                uint32 GPUIndexSize = Buffers->IndexBuffer.IndexBufferRHI->GetSize();
                uint32 ExpectedIndexSize = MaxIdx * sizeof(uint32);

                UE_LOG(LogTemp, Error, TEXT("Index Buffer - GPU: %u bytes (%d indices)"),
                    GPUIndexSize, GPUIndexSize / sizeof(uint32));
                UE_LOG(LogTemp, Error, TEXT("Index Buffer - Expected: %u bytes (%d indices)"),
                    ExpectedIndexSize, MaxIdx);
                UE_LOG(LogTemp, Error, TEXT("Match: %s"),
                    (GPUIndexSize == ExpectedIndexSize) ? TEXT("✅ YES") : TEXT("❌ NO"));
            }
        }
        );



    UE_LOG(LogTemp, Log, TEXT("OptimizedProxy: Initialized with %d/%d vertices, %d/%d indices"),
        CurrentValidVertexCount, MaxVertexCapacity,
        CurrentValidIndexCount, MaxIndexCapacity);     
}

void FOptimizedDynamicMeshSceneProxy::FastUpdate_IncreamentalVertices(
	const TArray<FVector3f>& NewPositions,
	const TArray<FVector3f>& NewNormals,
	const TArray<uint32>& NewIndices,
	int32 VertexOffset,
	int32 IndexOffset
)
{
    // 유효성 검사
    if (VertexOffset + NewPositions.Num() > MaxVertexCapacity || IndexOffset + NewIndices.Num() > MaxIndexCapacity)
    {
        UE_LOG(LogTemp, Error, TEXT("Buffer Overflow!!"));
        return;
    }

    // TODO: 지금은 Material이 하나라고 가정
    FMeshRenderBufferSet* Buffers = RenderBufferSets[0];

    // CPU측 데이터 갱신
    {
        // Position 
        for (int32 i = 0; i < NewPositions.Num(); ++i)
        {
            Buffers->PositionVertexBuffer.VertexPosition(VertexOffset + i) = NewPositions[i];
        }

        // Color 
        // TODO: 일단은 흰색으로 두고, 나중에 인자로 빼자 
        for (int32 i = 0; i < NewNormals.Num(); ++i)
        {
            Buffers->ColorVertexBuffer.VertexColor(VertexOffset + i) = FColor::White; 
        }

        // Normal / Tangent / UV
        for(int32 i = 0;  i < NewNormals.Num(); ++i)
        {
            Buffers->StaticMeshVertexBuffer.SetVertexTangents(
                VertexOffset + i,
                FVector3f::ZeroVector,
                FVector3f::ZeroVector,
                NewNormals[i]
            );

            Buffers->StaticMeshVertexBuffer.SetVertexUV(VertexOffset + i, 0, FVector2f::ZeroVector);
        }

        // Index
        if (Buffers->IndexBuffer.Indices.Num() < IndexOffset + NewIndices.Num())
        {
            Buffers->IndexBuffer.Indices.SetNum(IndexOffset + NewIndices.Num());
        }
        FMemory::Memcpy(&Buffers->IndexBuffer.Indices[IndexOffset], NewIndices.GetData(), NewIndices.Num() * sizeof(uint32)); 
    }

    // 유효 카운트 업데이트
    CurrentValidVertexCount = VertexOffset + NewPositions.Num();
    CurrentValidIndexCount = IndexOffset + NewIndices.Num();

    // GPU에 부분 업데이트
    // 기존에 있던 buffer는 유지
    ENQUEUE_RENDER_COMMAND(FastUpdatePartial)(
        [Buffers, VertexOffset, IndexOffset, NewPositions, NewNormals, NewIndices](FRHICommandListImmediate& RHICmdList)
        {
            // Position Buffer 업데이트
            if (NewPositions.Num() > 0)
            { 
                void* BufferData = RHICmdList.LockBuffer(
                    Buffers->PositionVertexBuffer.VertexBufferRHI,
                    VertexOffset * sizeof(FVector3f),
                    NewPositions.Num() * sizeof(FVector3f),
                    RLM_WriteOnly
                );

                FMemory::Memcpy(BufferData, NewPositions.GetData(), NewPositions.Num() * sizeof(FVector3f));
                RHICmdList.UnlockBuffer(Buffers->PositionVertexBuffer.VertexBufferRHI);
            }

            // Static Mesh Buffer 업데이트 ( Normal/Tangent/UV ) 
            if (NewNormals.Num() > 0)
            {
                // Tangent Buffer Update 
                {
                    uint32 TangentStride = Buffers->StaticMeshVertexBuffer.GetUseHighPrecisionTangentBasis() ? 8 : 4;

                    void* BufferData = RHICmdList.LockBuffer(
                        Buffers->StaticMeshVertexBuffer.TangentsVertexBuffer.VertexBufferRHI,
                        VertexOffset * TangentStride,
                        NewNormals.Num() * TangentStride,
                        RLM_WriteOnly
                    );

                    // CPU에서 데이터 가져오기
                    // GetTangentData()는 버퍼의 시작 주소(void*)를 반환합니다.
                    const uint8* CPUDataStart = (const uint8*)Buffers->StaticMeshVertexBuffer.GetTangentData();

                    // 복사할 시작 위치 계산
                    const uint8* CopyStart = CPUDataStart + (VertexOffset * TangentStride);

                    // 메모리 복사
                    FMemory::Memcpy(BufferData, CopyStart, NewNormals.Num() * TangentStride);
                    RHICmdList.UnlockBuffer(Buffers->StaticMeshVertexBuffer.TangentsVertexBuffer.VertexBufferRHI);
                }

                // UV Buffer Update (TexCoord)
                // 위와 동일한 로직
                { 
                    // UV 정밀도에 따라 float(4byte) 혹은 half(2byte) * XY(2개) = 8 or 4 바이트
                    uint32 SingleUVStride = Buffers->StaticMeshVertexBuffer.GetUseFullPrecisionUVs() ? 8 : 4;

                    // 중요: 전체 UV Stride는 (UV 하나 크기 * 텍스처 좌표 채널 수) 입니다.
                    uint32 TotalUVStride = SingleUVStride * Buffers->StaticMeshVertexBuffer.GetNumTexCoords();

                    void* UVBufferData = RHICmdList.LockBuffer(
                        Buffers->StaticMeshVertexBuffer.TexCoordVertexBuffer.VertexBufferRHI,
                        VertexOffset * TotalUVStride,
                        NewNormals.Num() * TotalUVStride, // 정점 개수만큼 복사
                        RLM_WriteOnly
                    );

                    // CPU에서 데이터 가져오기
                    const uint8* CPUUVDataStart = (const uint8*)Buffers->StaticMeshVertexBuffer.GetTexCoordData();
                    const uint8* UVCopyStart = CPUUVDataStart + (VertexOffset * TotalUVStride);

                    FMemory::Memcpy(UVBufferData, UVCopyStart, NewNormals.Num() * TotalUVStride);
                    RHICmdList.UnlockBuffer(Buffers->StaticMeshVertexBuffer.TexCoordVertexBuffer.VertexBufferRHI);
                }
            }

            if (NewIndices.Num() > 0)
            {
                void* BufferData = RHICmdList.LockBuffer(
                    Buffers->IndexBuffer.IndexBufferRHI,
                    IndexOffset * sizeof(uint32),
                    NewIndices.Num() * sizeof(uint32),
                    RLM_WriteOnly
                ); 
                
                FMemory::Memcpy(BufferData, NewIndices.GetData(), NewIndices.Num() * sizeof(uint32));
                RHICmdList.UnlockBuffer(Buffers->IndexBuffer.IndexBufferRHI);
            }

        }
        );
}

void FOptimizedDynamicMeshSceneProxy::FastUpdate_EntireMesh(const TArray<FVector3f>& AllPositions, const TArray<FVector3f>& AllNormals, const TArray<uint32>& AllIndices)
{
    // 미리 잡아둔 Max보다 크면 어쩔 수 없이 리턴하거나 재할당
    if (AllPositions.Num() > MaxVertexCapacity || AllIndices.Num() > MaxIndexCapacity)
    {
        // TODO: ResizeBuffers(AllPositions.Num() * 1.5) 같은 걸 호출
        // 일단은 리턴
        UE_LOG(LogTemp, Warning, TEXT("Buffer Overflow! Need Resize."));
        return;
    }
    
    // 렌더 스레드 명령 ( 전체 덮어쓰기 ) 
    FMeshRenderBufferSet* Buffers = RenderBufferSets[0];

    static FRHIBuffer* LastPositionBufferRHI = nullptr;
    static FRHIBuffer* LastIndexBufferRHI = nullptr;

    ENQUEUE_RENDER_COMMAND(FastUpdateEntire)
    ([Buffers, AllPositions, AllNormals, AllIndices, this] (FRHICommandListImmediate& RHICmdList)
    {
        FRHIBuffer* CurrentPosBuffer = Buffers->PositionVertexBuffer.VertexBufferRHI;
        FRHIBuffer* CurrentIdxBuffer = Buffers->IndexBuffer.IndexBufferRHI;

        if (LastPositionBufferRHI != nullptr && LastPositionBufferRHI != CurrentPosBuffer)
        {
            UE_LOG(LogTemp, Error, TEXT("❌ GPU BUFFER REALLOCATED! (Position)"));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("✅Success: GPU BUFFER NOT REALLOCATED! (Position)"));
        }

        if (LastIndexBufferRHI != nullptr && LastIndexBufferRHI != CurrentIdxBuffer)
        {
            UE_LOG(LogTemp, Error, TEXT("❌ GPU BUFFER REALLOCATED! (Index)"));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("✅Success: GPU BUFFER NOT REALLOCATED! (Index)"));
        }
        LastPositionBufferRHI = CurrentPosBuffer;
        LastIndexBufferRHI = CurrentIdxBuffer;

        // Position 전체 덮어쓰기 
        if (AllPositions.Num() > 0)
        {
            void* BufferData = RHICmdList.LockBuffer(
                Buffers->PositionVertexBuffer.VertexBufferRHI,
                0, 
                AllPositions.Num() * sizeof(FVector3f),
                RLM_WriteOnly
            );
        
            FMemory::Memcpy(BufferData, AllPositions.GetData(), AllPositions.Num() * sizeof(FVector3f));
            RHICmdList.UnlockBuffer(Buffers->PositionVertexBuffer.VertexBufferRHI); 
        }

        // Normal/Tangent 덮어쓰기
        if (AllNormals.Num() > 0)
        {
            // Tangent
            uint32 TanStride = Buffers->StaticMeshVertexBuffer.GetUseHighPrecisionTangentBasis() ? 8 : 4;
            void* TanData = RHICmdList.LockBuffer(
                Buffers->StaticMeshVertexBuffer.TangentsVertexBuffer.VertexBufferRHI,
                0,
                AllNormals.Num() * TanStride,
                RLM_WriteOnly
            ); 

            RHICmdList.UnlockBuffer(Buffers->StaticMeshVertexBuffer.TangentsVertexBuffer.VertexBufferRHI);

            //TODO: UV
             
        }

        // Index 전체 덮어쓰기
        if (AllIndices.Num() > 0)
        {
            void* IdxData = RHICmdList.LockBuffer(
                Buffers->IndexBuffer.IndexBufferRHI,
                0,
                AllIndices.Num() * sizeof(uint32),
                RLM_WriteOnly
            );

            FMemory::Memcpy(IdxData, AllIndices.GetData(), AllIndices.Num() * sizeof(uint32));
            RHICmdList.UnlockBuffer(Buffers->IndexBuffer.IndexBufferRHI);
        }
    }
    );

    // 유효 개수 업데이트
}

void FOptimizedDynamicMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
    // 기본 체크
    if (CurrentValidIndexCount == 0 || RenderBufferSets.Num() == 0)
    {
        return;
    }

    FMeshRenderBufferSet* Buffers = RenderBufferSets[0];
    if (!Buffers) return;

    // Material 가져오기
    FMaterialRenderProxy* MaterialProxy = Buffers->Material->GetRenderProxy();

    // Mesh Batch 생성 (Draw 명령)
    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
    {
        if (VisibilityMap & (1 << ViewIndex))
        {
            const FSceneView* View = Views[ViewIndex];

            // Dynamic Mesh를 그리기 위한 Batch 생성
            FMeshBatch& Mesh = Collector.AllocateMesh();

            // FMeshBatch 설정
            Mesh.bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe; 
            Mesh.VertexFactory = &Buffers->VertexFactory;
            Mesh.MaterialRenderProxy = MaterialProxy;

            //Reverse Culling
            Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();

            Mesh.Type = PT_TriangleList;
            Mesh.DepthPriorityGroup = SDPG_World;
            Mesh.bCanApplyViewModeOverrides = true;

            // 유효한 개수만큼만 그리기
            FMeshBatchElement& BatchElement = Mesh.Elements[0];
            BatchElement.IndexBuffer = &Buffers->IndexBuffer;

            // 전체 Index Buffer 크기가 아니라, 유효한 Index 개수 / 3 (삼각형 수) 
            BatchElement.NumPrimitives = CurrentValidIndexCount / 3;
            
            BatchElement.FirstIndex = 0;
            BatchElement.MinVertexIndex = 0;
            BatchElement.MaxVertexIndex = CurrentValidVertexCount - 1; 

            // Drawcall
            Collector.AddMesh(ViewIndex, Mesh);
        }
    }


}