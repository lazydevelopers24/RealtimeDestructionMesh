// Fill out your copyright notice in the Description page of Project Settings.


#include "AnchorMode/AnchorActionObejct.h"

#include "EditorModeManager.h"
#include "EngineUtils.h"
#include "GridCellBuilder.h"
#include "RealtimeDestructibleMeshComponent.h"
#include "Selection.h"
#include "Actors/AnchorPlaneActor.h"
#include "Actors/AnchorVolumeActor.h"
#include "Subsystems/EditorActorSubsystem.h"


class AAnchorVolumeActor;

void UAnchorActionObejct::SpawnAnchorPlane()
{
	if (!GEditor || !GEditor->GetActiveViewport())
	{
		return;
	}

	FEditorViewportClient* ViewportClient = (FEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	FVector SpawnLocation = ViewportClient->GetViewLocation() + (ViewportClient->GetViewRotation().Vector() * 300.0f);

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "SpawnAnchorPlane", "Spawn Plane"));

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World)
	{
		AAnchorPlaneActor* NewPlane = World->SpawnActor<AAnchorPlaneActor>(SpawnLocation, FRotator::ZeroRotator);
		if (NewPlane)
		{
			GEditor->SelectNone(true, true);
			GEditor->SelectActor(NewPlane, true, true);
			GLevelEditorModeTools().SetWidgetMode(UE::Widget::WM_Translate);
		}
	}

	UpdateCellCounts();
}

void UAnchorActionObejct::SpawnAnchorVolume()
{
	if (!GEditor || !GEditor->GetActiveViewport())
	{
		return;
	}

	FEditorViewportClient* ViewportClient = (FEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	FVector SpawnLocation = ViewportClient->GetViewLocation() + (ViewportClient->GetViewRotation().Vector() * 300.0f);

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "SpawnAnchorVolume", "Spawn Volume"));

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World)
	{
		AAnchorVolumeActor* NewVolume = World->SpawnActor<AAnchorVolumeActor>(SpawnLocation, FRotator::ZeroRotator);
		if (NewVolume)
		{
			GEditor->SelectNone(true, true);
			GEditor->SelectActor(NewVolume, true, true);
			GLevelEditorModeTools().SetWidgetMode(UE::Widget::WM_Translate);
		}
	}

	UpdateCellCounts();
}

void UAnchorActionObejct::ApplyAllAnchorPlanes()
{
	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ApplyAnchorPlanes", "Apply Anchor Planes"));

	TArray<AAnchorPlaneActor*> Planes;
	for (TActorIterator<AAnchorPlaneActor> PlaneIt(World); PlaneIt; ++PlaneIt)
	{
		AAnchorPlaneActor* Plane = *PlaneIt;
		if (IsValid(Plane))
		{
			Planes.Add(Plane);
		}
	}

	if (Planes.Num() == 0)
	{
		return;
	}

	for (TObjectIterator<URealtimeDestructibleMeshComponent> It; It; ++It)
	{
		URealtimeDestructibleMeshComponent* Comp = *It;

		if (!IsValid(Comp) || Comp->GetWorld() != World || Comp->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}

		if (Comp->IsTemplate())
		{
			continue;
		}

		Comp->Modify();

		FGridCellLayout& GridCellCache = Comp->GetGridCellLayout();
		if (GridCellCache.GetTotalCellCount() == 0)
		{
			Comp->BuildGridCells();
		}

		for (auto Plane : Planes)
		{
			FGridCellBuilder::SetAnchorsByFinitePlane(
				Plane->GetActorTransform(),
				Comp->GetComponentTransform(),
				GridCellCache,
				Plane->bIsEraser);
		}

		Comp->MarkRenderStateDirty();
	}

	UpdateCellCounts();

	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::ApplyAllAnchorVolumes()
{
	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ApplyAnchorVolumes", "Apply Anchor Volumes"));

	TArray<AAnchorVolumeActor*> Volumes;
	for (TActorIterator<AAnchorVolumeActor> VolumeIt(World); VolumeIt; ++VolumeIt)
	{
		AAnchorVolumeActor* Volume = *VolumeIt;
		if (IsValid(Volume))
		{
			Volumes.Add(Volume);
		}
	}

	if (Volumes.Num() == 0)
	{
		return;
	}

	for (TObjectIterator<URealtimeDestructibleMeshComponent> It; It; ++It)
	{
		URealtimeDestructibleMeshComponent* Comp = *It;

		if (!IsValid(Comp) || Comp->GetWorld() != World || Comp->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}

		if (Comp->IsTemplate())
		{
			continue;
		}

		Comp->Modify();

		FGridCellLayout& GridCellCache = Comp->GetGridCellLayout();
		if (GridCellCache.GetTotalCellCount() == 0)
		{
			Comp->BuildGridCells();
		}

		for (auto Volume : Volumes)
		{
			if (Volume->Shape == EAnchorVolumeShape::Box)
			{
				FGridCellBuilder::SetAnchorsByFiniteBox(
				   Volume->GetActorTransform(),
				   Volume->BoxExtent,
				   Comp->GetComponentTransform(),
				   GridCellCache,
				   Volume->bIsEraser);
			}

			if (Volume->Shape == EAnchorVolumeShape::Sphere)
			{
				FGridCellBuilder::SetAnchorsByFiniteSphere(
				   Volume->GetActorTransform(),
				   Volume->SphereRadius,
				   Comp->GetComponentTransform(),
				   GridCellCache,
				   Volume->bIsEraser);
			}
		}

		Comp->MarkRenderStateDirty();
	}

	UpdateCellCounts();
	
	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::RemoveAllAnchorPlanes()
{
	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ClearAnchorPlanes", "Clear Anchor Planes"));

	GEditor->SelectNone(false, true, false);

	UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();

	for (TActorIterator<AAnchorPlaneActor> PlaneIt(World); PlaneIt; ++PlaneIt)
	{
		AAnchorPlaneActor* Plane = *PlaneIt;
		if (!IsValid(Plane))
		{
			continue;
		}
		Plane->Modify();

		GEditor->SelectActor(Plane, false, false);
		
		if (ActorSubsystem)
		{
			ActorSubsystem->DestroyActor(Plane);
		}
		else
		{
			World->EditorDestroyActor(Plane, true);
		}
	}

	UpdateCellCounts();
	GEditor->NoteSelectionChange();
	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::RemoveAllAnchorVolumes()
{
	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ClearAnchorVolumes", "Clear Anchor Volumes"));

	GEditor->SelectNone(false, true, false);

	UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();

	for (TActorIterator<AAnchorVolumeActor> VolumeIt(World); VolumeIt; ++VolumeIt)
	{
		AAnchorVolumeActor* Volume = *VolumeIt;
		if (!IsValid(Volume))
		{
			continue;
		}
		Volume->Modify();

		GEditor->SelectActor(Volume, false, false);
		
		if (ActorSubsystem)
		{
			ActorSubsystem->DestroyActor(Volume);
		}
		else
		{
			World->EditorDestroyActor(Volume, true);
		}
	}

	UpdateCellCounts();
	GEditor->NoteSelectionChange();
	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::RemoveAllAnchors()
{
	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ClearAnchors", "Clear Anchors"));
	
	for (TObjectIterator<URealtimeDestructibleMeshComponent> It; It; ++It)
	{
		URealtimeDestructibleMeshComponent* Comp = *It;

		if (!IsValid(Comp) || Comp->GetWorld() != World || Comp->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}

		if (Comp->IsTemplate())
		{
			continue;
		}

		Comp->Modify();

		FGridCellLayout& GridCellCache = Comp->GetGridCellLayout();
		if (GridCellCache.IsValid())
		{
			FGridCellBuilder::ClearAllAnchors(GridCellCache);;
			Comp->MarkRenderStateDirty();
		}
	}

	UpdateCellCounts();
	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::ApplyAnchors()
{
	if (!TargetComp)
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ApplyAnchorsToSelectedComp", "Apply Anchors To Selected"));

	FGridCellLayout& GridCellCache = TargetComp->GetGridCellLayout();
	if (!GridCellCache.IsValid())
	{
		TargetComp->BuildGridCells();
	}

	TArray<AAnchorVolumeActor*> Volumes;
	for (TActorIterator<AAnchorVolumeActor> VolumeIt(World); VolumeIt; ++VolumeIt)
	{
		AAnchorVolumeActor* Volume = *VolumeIt;
		if (IsValid(Volume))
		{
			Volumes.Add(Volume);
		}
	}

	TArray<AAnchorPlaneActor*> Planes;
	for (TActorIterator<AAnchorPlaneActor> PlaneIt(World); PlaneIt; ++PlaneIt)
	{
		AAnchorPlaneActor* Plane = *PlaneIt;
		if (IsValid(Plane))
		{
			Planes.Add(Plane);
		}
	}

	for (auto Plane : Planes)
	{
		if (!IsValid(Plane))
		{
			continue;
		}

		if (!IsValid(TargetComp))
		{
			return;
		}

		FGridCellBuilder::SetAnchorsByFinitePlane(
			Plane->GetActorTransform(),
			TargetComp->GetComponentTransform(),
			GridCellCache,
			Plane->bIsEraser);
	}

	for (auto Volume : Volumes)
	{
		if (!IsValid(Volume))
		{
			continue;
		}

		if (!IsValid(TargetComp))
		{
			return;
		}

		if (Volume->Shape == EAnchorVolumeShape::Box)
		{
			FGridCellBuilder::SetAnchorsByFiniteBox(
				Volume->GetActorTransform(),
				Volume->BoxExtent,
				TargetComp->GetComponentTransform(),
				GridCellCache,
				Volume->bIsEraser);
		}

		if (Volume->Shape == EAnchorVolumeShape::Sphere)
		{
			FGridCellBuilder::SetAnchorsByFiniteSphere(
				Volume->GetActorTransform(),
				Volume->SphereRadius,
				TargetComp->GetComponentTransform(),
				GridCellCache,
				Volume->bIsEraser);
		}
	}

	UpdateCellCounts();
}

void UAnchorActionObejct::RemoveAnchors()
{
	if (!TargetComp)
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}
	
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ApplyAnchorsToSelectedComp", "Apply Anchors To Selected"));

	FGridCellLayout& GridCellCache = TargetComp->GetGridCellLayout();
	if (!GridCellCache.IsValid())
	{
		return;
	}

	FGridCellBuilder::ClearAllAnchors(GridCellCache);

	UpdateCellCounts();
}

void UAnchorActionObejct::BuildGridCellsForSelection()
{
	if (!GEditor || !IsValid(TargetComp))
	{
		return;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World || TargetComp->GetWorld() != World)
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "BuildGridCellsForSelection", "Build Grid Cells (Selected)"));

	TargetComp->Modify();

	FGridCellLayout& Cache = TargetComp->GetGridCellLayout();

	if (!Cache.IsValid() || Cache.GetTotalCellCount() == 0)
	{
		TargetComp->BuildGridCells();
	}

	UpdateCellCounts();

	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::UpdateSelectionFromEditor(UWorld* InWorld)
{
	TargetComp = nullptr;

	TotalCellCount = 0;
	ValidCellCount = 0;
	AnchorCellCount = 0;

	UWorld* World = InWorld;
	if (!GEditor || !World)
	{
		return;
	}

	if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
	{
		for (FSelectionIterator It(*SelectedComponents); It; ++It)
		{
			URealtimeDestructibleMeshComponent* Comp = Cast<URealtimeDestructibleMeshComponent>(*It);
			if (IsValid(Comp) && Comp->GetWorld() == World && !Comp->IsTemplate())
			{
				TargetComp = Comp;
				break;
			}
		}
	}

	if (!TargetComp)
	{
		if (USelection* SelectedActors = GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*SelectedActors); It; ++It)
			{
				AActor* Actor = Cast<AActor>(*It);
				if (!IsValid(Actor) || Actor->GetWorld() != World)
				{
					continue;
				}

				URealtimeDestructibleMeshComponent* Comp = Actor->FindComponentByClass<URealtimeDestructibleMeshComponent>();
				if (IsValid(Comp) && !Comp->IsTemplate())
				{
					TargetComp = Comp;
					break;
				}
			}
		}
	}

	if (TargetComp)
	{
		// 이름 업데이트
		SelectedComponentName = TargetComp->GetName();
	}
	else
	{
		SelectedComponentName = TEXT("None");
	}

	UpdateCellCounts();
}

void UAnchorActionObejct::UpdateCellCounts()
{
	if (!IsValid(TargetComp))
	{
		return;
	}
	
	TotalCellCount = 0;
	ValidCellCount = 0;
	AnchorCellCount = 0;	

	FGridCellLayout& Cache = TargetComp->GetGridCellLayout();
	if (!Cache.IsValid())
	{
		return;
	}

	TotalCellCount = Cache.GetTotalCellCount();
	ValidCellCount = Cache.GetValidCellCount();
	AnchorCellCount = Cache.GetAnchorCount();
	SelectedComponentName = TargetComp->GetOwner()->GetActorLabel();
}
