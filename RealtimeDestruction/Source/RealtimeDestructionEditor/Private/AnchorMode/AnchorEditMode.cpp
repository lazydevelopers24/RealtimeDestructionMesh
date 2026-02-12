// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "AnchorMode/AnchorEditMode.h"

#include "DynamicMeshBuilder.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "EngineUtils.h"
#include "RealtimeDestructibleMeshComponent.h"
#include "Selection.h"
#include "Actors/AnchorPlaneActor.h"
#include "Actors/AnchorVolumeActor.h"
#include "AnchorMode/AnchorActionObejct.h"
#include "AnchorMode/AnchorEditModeToolkit.h"
#include "Toolkits/ToolkitManager.h"
#include "DrawDebugHelpers.h"

class FDynamicColoredMaterialRenderProxy;
const FEditorModeID UAnchorEditMode::EM_AnchorEditModeId = FName("AnchorEditMode");

void FCellDebugSnapshot::Initialize(URealtimeDestructibleMeshComponent* InComponent)
{
	Reset();

	if (!IsValid(InComponent))
	{
		return;
	}
	
	Component = InComponent;
	Owner = InComponent ?  InComponent->GetOwner() : nullptr;

	const FGridCellLayout& Layout = Component->GetGridCellLayout();

	Scale = InComponent->GetComponentTransform().GetScale3D();
	GridSize = Layout.GridSize;
	CellSize = Layout.CellSize;

	CellBits = Layout.CellExistsBits;
	AnchorBits = Layout.CellIsAnchorBits;

	TotalCells = Layout.GetTotalCellCount();
	TotalAnchors = Layout.GetAnchorCount();
}

void FCellDebugSnapshot::Reset()
{
	Owner.Reset();
	Component.Reset();
	Scale = FVector::OneVector;
	GridSize = FIntVector::ZeroValue;
	CellSize = FVector::ZeroVector;
	CellBits.Empty();
	AnchorBits.Empty();
	TotalCells = 0;
	TotalAnchors = 0;
}

bool FCellDebugSnapshot::IsRedraw(const FGridCellLayout& Layout)
{
	if (!Layout.IsValid() || Layout.GetTotalCellCount() <= 0)
	{
		return false;
	}

	if (TotalCells != Layout.GetTotalCellCount())
	{
		return true;
	}

	if (TotalAnchors != Layout.GetAnchorCount())
	{
		return true;
	}

	if (CellBits.Num() != Layout.CellExistsBits.Num())
	{
		return true;
	}

	if (AnchorBits.Num() != Layout.CellIsAnchorBits.Num())
	{
		return true;
	}

	for (int32 i = 0; i < CellBits.Num(); i++)
	{
		if (CellBits[i] != Layout.CellExistsBits[i])
		{
			return true;
		}
	}

	for (int32 i = 0; i < AnchorBits.Num(); i++)
	{
		if (AnchorBits[i] != Layout.CellIsAnchorBits[i])
		{
			return true;
		}
	}
	
	return false;
}

bool FCellDebugSnapshot::IsFlush(URealtimeDestructibleMeshComponent* InComponent, const FGridCellLayout& Layout)
{
	if (!IsValid(InComponent))
	{
		return true;
	}

	if (!Component.IsValid() || Component.Get() != InComponent)
	{
		return true;
	}

	const FVector CurrentScale = InComponent->GetComponentTransform().GetScale3D();
	if (!Scale.Equals(CurrentScale, 1.e-4f))
	{
		return true;
	}

	return (GridSize != Layout.GridSize)
			|| !CellSize.Equals(Layout.CellSize, 1.e-4f);
}

UAnchorEditMode::UAnchorEditMode()
{
	Info = FEditorModeInfo(EM_AnchorEditModeId, FText::FromString("Anchor Editor"), FSlateIcon(), true);
}

void UAnchorEditMode::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	Super::Tick(ViewportClient, DeltaTime);

	if (ActionObject && ActionObject->bShowGridCell)
	{
		DrawSelectedGridCells();
	}
}

void UAnchorEditMode::Enter()
{
	Super::Enter();
	
	if (!ActionObject)
	{
		ActionObject = NewObject<UAnchorActionObejct>(this);
	}

	if (ActionObject)
	{
		ActionObject->EnsureEditorDelegatesBound();
		ActionObject->CollectionExistingAnchorActors(GetWorld());
	}
	
	GLevelEditorModeTools().SetShowWidget(true);

	OnEditorSelectionChanged(nullptr);
}

void UAnchorEditMode::Exit()
{
	SelectedComp = nullptr;
	
	if (ActionObject)
	{
		ActionObject->RemoveAllAnchorPlanes();
		ActionObject->RemoveAllAnchorVolumes();
		ActionObject->UnBindEditorDelgates();
	}
	
	if(Toolkit.IsValid())
	{
		FToolkitManager::Get().CloseToolkit(Toolkit.ToSharedRef());
		Toolkit.Reset();
	}
	
	Super::Exit();
}

void UAnchorEditMode::CreateToolkit()
{
	if (Toolkit.IsValid())
	{
		return;
	}
	Toolkit = MakeShareable(new FAnchorEditModeToolkit);	
}

bool UAnchorEditMode::IsSelectionAllowed(AActor* InActor, bool bInSelection) const
{
	if (!InActor)
	{
		return false;
	}
	
	bool bIsAnchorPlaneActor = InActor->IsA(AAnchorPlaneActor::StaticClass());
	
	bool bIsAnchorVolumeActor = InActor->IsA(AAnchorVolumeActor::StaticClass());

	bool bHasRTDM = InActor->FindComponentByClass<URealtimeDestructibleMeshComponent>() != nullptr;
	
	return bIsAnchorPlaneActor || bIsAnchorVolumeActor || bHasRTDM;
}

void UAnchorEditMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	Super::Render(View, Viewport, PDI);

	if (!View || !Viewport || !PDI || !GEditor)
	{
		return;
	}

	if (View->Family && View->Family->EngineShowFlags.HitProxies)
	{
		return;
	}
	
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	
	DrawPlaneEdge(View, PDI);
}

void UAnchorEditMode::OnEditorSelectionChanged(UObject* NewSelection)
{
	SelectedComp = nullptr;

	if (ActionObject)
	{
		ActionObject->UpdateSelectionFromEditor(GetWorld());
		SelectedComp = ActionObject->TargetComp;

	}

	if (Toolkit.IsValid())
	{
		TSharedPtr<FAnchorEditModeToolkit> AnchorToolkit = StaticCastSharedPtr<FAnchorEditModeToolkit>(Toolkit);
		if (AnchorToolkit.IsValid())
		{
			AnchorToolkit->ForceRefreshDetails();
		}
	}
}

void UAnchorEditMode::ActorSelectionChangeNotify()
{
	Super::ActorSelectionChangeNotify();
	OnEditorSelectionChanged(nullptr);
}

TSharedRef<FLegacyEdModeWidgetHelper> UAnchorEditMode::CreateWidgetHelper()
{
	return MakeShared<FLegacyEdModeWidgetHelper>();
}

void UAnchorEditMode::DrawPlaneEdge(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	
	const float InflateWorldCm = 2.0f;
	const float LineThickness = 2.5f;

	for (TActorIterator<AAnchorPlaneActor> It(World); It; ++It)
	{
		AAnchorPlaneActor* Plane = *It;
		if (!IsValid(Plane))
		{
			continue;
		}

#if WITH_EDITORONLY_DATA
		UStaticMeshComponent* PlaneMeshComp = Plane->PlaneMesh;
#else
		UStaticMeshComponent* PlaneMeshComp = Plane->FindComponentByClass<UStaticMeshComponent>();
#endif
		if (!IsValid(PlaneMeshComp))
		{
			continue;
		}

		const UStaticMesh* Mesh = PlaneMeshComp->GetStaticMesh();
		if (!Mesh)
		{
			continue;
		}

		const FLinearColor Color = Plane->bIsEraser ? FLinearColor::Red : FLinearColor::Green;

		const FBoxSphereBounds LocalBounds = PlaneMeshComp->GetStaticMesh()->GetBounds();
		const FVector O = LocalBounds.Origin;
		const FVector E = LocalBounds.BoxExtent;

		const FTransform CT = PlaneMeshComp->GetComponentTransform();
		
		const FVector Normal = CT.GetUnitAxis(EAxis::X);
		const FVector CenterWorld = PlaneMeshComp->Bounds.Origin;
		const FVector ToCamera = (View->ViewLocation - CenterWorld).GetSafeNormal();
		const float Sign = (FVector::DotProduct(Normal, ToCamera) >= 0.0f) ? 1.0f : -1.0f;

		const float FaceX = (Sign > 0.0f) ? (O.X + E.X) : (O.X - E.X);

		const FVector Scale = PlaneMeshComp->GetComponentScale().GetAbs();
		const float ScaleY = FMath::Max(0.0001f, Scale.Y);
		const float ScaleZ = FMath::Max(0.0001f, Scale.Z);

		const float InflateLocalY = InflateWorldCm / ScaleY;
		const float InflateLocalZ = InflateWorldCm / ScaleZ;

		const float Y = E.Y + InflateLocalY;
		const float Z = E.Z + InflateLocalZ;

		const FVector ALocal(FaceX, O.Y - Y, O.Z - Z);
		const FVector BLocal(FaceX, O.Y + Y, O.Z - Z);
		const FVector CLocal(FaceX, O.Y + Y, O.Z + Z);
		const FVector DLocal(FaceX, O.Y - Y, O.Z + Z);

		const FVector A = CT.TransformPosition(ALocal);
		const FVector B = CT.TransformPosition(BLocal);
		const FVector C = CT.TransformPosition(CLocal);
		const FVector D = CT.TransformPosition(DLocal);

		PDI->DrawLine(A, B, Color, SDPG_Foreground, LineThickness);
		PDI->DrawLine(B, C, Color, SDPG_Foreground, LineThickness);
		PDI->DrawLine(C, D, Color, SDPG_Foreground, LineThickness);
		PDI->DrawLine(D, A, Color, SDPG_Foreground, LineThickness);
	}
}

void UAnchorEditMode::DrawSelectedGridCells()
{
	if (!IsValid(SelectedComp))
	{
		return;
	}
	
	UWorld* World = GetWorld();
	if (!World || World->IsGameWorld() || SelectedComp->GetWorld() != World)
	{
		return;
	}

	const FGridCellLayout& Layout = SelectedComp->GetGridCellLayout();
	if (!Layout.IsValid() ||
		!Layout.MeshScale.Equals(SelectedComp->GetComponentTransform().GetScale3D(), 1.e-4f) ||
		!Layout.HasValidSparseData())
	{
		return;
	}
	
	const FTransform& ComponentTransform = SelectedComp->GetComponentTransform();
	const float PointSize = 5.0f;

	for (int32 CellId : Layout.GetValidCellIds())
	{
		const bool bIsAnchor = Layout.GetCellIsAnchor(CellId);
		// const FLinearColor CellColor = bIsAnchor ? FLinearColor::Green : FLinearColor(FColor::Cyan);
		const FColor CellColor = bIsAnchor ? FColor::Green : FColor::Cyan;

		const FVector LocalCenter = Layout.IdToLocalCenter(CellId);
		const FVector WorldCenter = ComponentTransform.TransformPosition(LocalCenter);	

		DrawDebugPoint(World, WorldCenter, PointSize, CellColor, false, 0.0f, SDPG_Foreground);
	}
}
