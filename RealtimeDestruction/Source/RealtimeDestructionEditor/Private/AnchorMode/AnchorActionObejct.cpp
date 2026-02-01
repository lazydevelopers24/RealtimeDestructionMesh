// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

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
			AnchorActors.Add(TWeakObjectPtr<AAnchorActor>(NewPlane));
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
			AnchorActors.Add(TWeakObjectPtr<AAnchorActor>(NewVolume));
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

	ValidateAnchorArray();
	
	TArray<AAnchorPlaneActor*> Planes;
	for (auto AnchorActor : AnchorActors)
	{
		if (AAnchorPlaneActor* Plane = Cast<AAnchorPlaneActor>(AnchorActor.Get()))
		{
		if (IsValid(Plane))
		{
			Planes.Add(Plane);
		}
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
			Plane->ApplyToAnchors(Comp->GetComponentTransform(), GridCellCache);
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

	ValidateAnchorArray();

	TArray<AAnchorVolumeActor*> Volumes;
	for (auto AnchorActor : AnchorActors)
	{
		if (AAnchorVolumeActor* Volume = Cast<AAnchorVolumeActor>(AnchorActor.Get()))
		{
		if (IsValid(Volume))
		{
			Volumes.Add(Volume);
		}
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
			Volume->ApplyToAnchors(Comp->GetComponentTransform(), GridCellCache);
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

	ValidateAnchorArray();
	
	GEditor->SelectNone(false, true, false);

	UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();

	for (auto AnchorActor : AnchorActors)
	{
		if (AAnchorPlaneActor* Plane = Cast<AAnchorPlaneActor>(AnchorActor.Get()))
		{
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

	ValidateAnchorArray();
	
	GEditor->SelectNone(false, true, false);

	UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();

	for (auto AnchorActor : AnchorActors)
	{
		if(AAnchorVolumeActor* Volume = Cast<AAnchorVolumeActor>(AnchorActor.Get()))
		{
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

	ValidateAnchorArray();

	FGridCellLayout& GridCellCache = TargetComp->GetGridCellLayout();
	if (!GridCellCache.IsValid())
	{
		TargetComp->BuildGridCells();
	}

	const FTransform& MeshTransform = TargetComp->GetComponentTransform();
	for (auto AnchorActor : AnchorActors)
	{
		if (!IsValid(AnchorActor.Get()))
		{
			continue;
		}

		if (!IsValid(TargetComp))
		{
			return;
		}

		AnchorActor->ApplyToAnchors(MeshTransform, GridCellCache);
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

void UAnchorActionObejct::ValidateAnchorArray()
{
	AnchorActors.RemoveAll([](const TWeakObjectPtr<AAnchorActor>& Ptr)
	{
		return !Ptr.IsValid();
	});
}

void UAnchorActionObejct::CollectionExistingAnchorActors(UWorld* World)
{
	AnchorActors.Reset();

	if (!World)
	{
		return;
	}

	for (TActorIterator<AAnchorActor> It(World); It; ++It)
	{
		AAnchorActor* AnchorActor = Cast<AAnchorActor>(*It);
		if (IsValid(AnchorActor))
		{
			AnchorActors.Add(AnchorActor);
		}
	}

	UpdateCellCounts();
}
