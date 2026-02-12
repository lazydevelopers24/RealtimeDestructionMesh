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

void UAnchorActionObejct::BeginDestroy()
{
	UnBindEditorDelgates();
	Super::BeginDestroy();
}

void UAnchorActionObejct::SpawnAnchorPlane()
{
	EnsureEditorDelegatesBound();
	
	if (!GEditor || !GEditor->GetActiveViewport())
	{
		return;
	}

	FEditorViewportClient* ViewportClient = (FEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	FVector SpawnLocation = ViewportClient->GetViewLocation() + (ViewportClient->GetViewRotation().Vector() * 300.0f);
	FRotator SpawnRotation = FRotator::ZeroRotator;
	
	if (IsValid(TargetComp))
	{
		const FBoxSphereBounds WorldBounds = TargetComp->Bounds;
		const FVector LocalHalfExtent = TargetComp->GetLocalBounds().GetBox().GetExtent();
		const FVector ScaleAbs = TargetComp->GetComponentTransform().GetScale3D().GetAbs();
		const FVector ScaledHalfExtent = LocalHalfExtent * ScaleAbs;
		const FVector BoundsCenter = WorldBounds.Origin;
		
		const FVector Forward = TargetComp->GetForwardVector();
		const FVector Right = TargetComp->GetRightVector();
		
		const FVector ToCamera = (ViewportClient->GetViewLocation() - BoundsCenter).GetSafeNormal();

		const bool bUseForwardAxis = ScaledHalfExtent.X <= ScaledHalfExtent.Y;
		const FVector Axis = bUseForwardAxis ? Forward : Right;
		const float HalfExtent = bUseForwardAxis ? ScaledHalfExtent.X : ScaledHalfExtent.Y;

		const float Sign = (FVector::DotProduct(Axis, ToCamera) >= 0.0f) ? 1.0f : -1.0f;
		
		constexpr float Distance = 100.0f;
		SpawnLocation = BoundsCenter +(Axis * Sign * (HalfExtent + Distance));
		SpawnRotation = TargetComp->GetComponentRotation();
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "SpawnAnchorPlane", "Spawn Plane"));

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World)
	{		
		AAnchorPlaneActor* NewPlane = World->SpawnActor<AAnchorPlaneActor>(SpawnLocation, SpawnRotation);
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
	EnsureEditorDelegatesBound();
	
	if (!GEditor || !GEditor->GetActiveViewport())
	{
		return;
	}

	FEditorViewportClient* ViewportClient = (FEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	FVector SpawnLocation = ViewportClient->GetViewLocation() + (ViewportClient->GetViewRotation().Vector() * 300.0f);
	FRotator SpawnRotation = FRotator::ZeroRotator;

	if (IsValid(TargetComp))
	{
		const FBoxSphereBounds WorldBounds = TargetComp->Bounds;
		const FVector BoundsCenter = WorldBounds.Origin;
		const FVector ToCamera = (ViewportClient->GetViewLocation() - BoundsCenter).GetSafeNormal();
		const FVector LocalHalfExtent = TargetComp->GetLocalBounds().GetBox().GetExtent();
		const FVector ScaleAbs = TargetComp->GetComponentTransform().GetScale3D().GetAbs();
		const FVector ScaledHalfExtent = LocalHalfExtent * ScaleAbs;

		const FVector Forward = TargetComp->GetForwardVector();
		const FVector Right = TargetComp->GetRightVector();

		const bool bUseForwardAxis = ScaledHalfExtent.X <= ScaledHalfExtent.Y;
		const FVector Axis = bUseForwardAxis ? Forward : Right;
		const float HalfExtent = bUseForwardAxis ? ScaledHalfExtent.X : ScaledHalfExtent.Y;

		const float Sign = (FVector::DotProduct(Axis, ToCamera) >= 0.0f) ? 1.0f : -1.0f;
		
		constexpr float Distance = 100.0f;
		SpawnLocation = BoundsCenter +(Axis * Sign * (HalfExtent + Distance));
		SpawnRotation = TargetComp->GetComponentRotation();
	}	

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "SpawnAnchorVolume", "Spawn Volume"));

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World)
	{
		AAnchorVolumeActor* NewVolume = World->SpawnActor<AAnchorVolumeActor>(SpawnLocation, SpawnRotation);
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
	EnsureEditorDelegatesBound();
	
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
	EnsureEditorDelegatesBound();
	
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
	EnsureEditorDelegatesBound();
	
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
	
	EnsureEditorDelegatesBound();
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

void UAnchorActionObejct::ApplyAnchors()
{
	EnsureEditorDelegatesBound();
	
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

	TargetComp->Modify();
	
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
	EnsureEditorDelegatesBound();
	
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

	TargetComp->Modify();
	
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
	EnsureEditorDelegatesBound();

	if (!GEditor)
	{
		return;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	if (!ResolveTargetComponent(World))
	{
		UE_LOG(LogTemp, Display, TEXT("AnchorEdit: TargetComp unresolved (reinistanced?)"));
		return;
	}

	if (TargetComp->GetWorld() != World)
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "BuildGridCellsForSelection", "Build Grid Cells (Selected)"));

	TargetComp->Modify();

	FGridCellLayout& Cache = TargetComp->GetGridCellLayout();
	if (!Cache.IsValid() || !Cache.MeshScale.Equals(TargetComp->GetComponentTransform().GetScale3D(), 1.e-4f) || Cache.GetTotalCellCount() == 0)
	{
		UE_LOG(LogTemp, Display, TEXT("BuildCell/BuildGridCellsForSelection %s"), *TargetComp->GetOwner()->GetName());
		TargetComp->BuildGridCells();
	}

	UpdateCellCounts();

	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::ClearAllCells()
{
	EnsureEditorDelegatesBound();

	if (!GEditor)
	{
		return;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return;
	}

	if (!ResolveTargetComponent(World))
	{
		UE_LOG(LogTemp, Display, TEXT("AnchorEdit: TargetComp unresolved (reinistanced?)"));
		return;
	}

	if (TargetComp->GetWorld() != World)
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Anchor", "ClearAllCells", "Clear Grid Cells (Selected)"));

	TargetComp->Modify();
	
	FGridCellLayout& Cache = TargetComp->GetGridCellLayout();
	FCellState& CellState = TargetComp->GetCellState();
	if (Cache.IsValid())
	{
		Cache.Reset();
		CellState.Reset();
	}

	UpdateCellCounts();

	GEditor->RedrawLevelEditingViewports(true);
}

void UAnchorActionObejct::UpdateSelectionFromEditor(UWorld* InWorld)
{
	EnsureEditorDelegatesBound();
	
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
		TargetOwner = TargetComp->GetOwner();
		TargetCompName = TargetComp->GetFName();
	}
	else
	{
		SelectedComponentName = TEXT("None");
		TargetOwner.Reset();
		TargetCompName = NAME_None;
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

	if (bAnchorActorsDirty)
	{
		if (GEditor)
		{
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				CollectionExistingAnchorActors(World);
			}
			bAnchorActorsDirty = false;
		}
	}
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

	bAnchorActorsDirty = false;
	UpdateCellCounts();
}

void UAnchorActionObejct::EnsureEditorDelegatesBound()
{
	if (bEditorDelegatesBound)
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}

	if (!OnObjectsReplacedHandle.IsValid())
	{
		OnObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda(
			[this](const TMap<UObject*, UObject*>& OldToNew)
			{
				if (IsValid(TargetComp))
				{
					if (UObject* const* NewObj = OldToNew.Find(TargetComp))
					{
						if (URealtimeDestructibleMeshComponent* NewComp = Cast<URealtimeDestructibleMeshComponent>(*NewObj))
						{
							TargetComp = NewComp;
							SelectedComponentName = TargetComp->GetName();
							TargetCompName = TargetComp->GetFName();
							UpdateCellCounts();
							return;
						}
					}
				}

				if (!IsValid(TargetComp) && TargetOwner.IsValid() && !TargetCompName.IsNone())
				{
					AActor* Owner = TargetOwner.Get();
					if (Owner)
					{
						TArray<URealtimeDestructibleMeshComponent*> Comps;
						Owner->GetComponents<URealtimeDestructibleMeshComponent>(Comps);
						for (auto Comp : Comps)
						{
							if (IsValid(Comp) && Comp->GetFName() == TargetCompName)
							{
								TargetComp = Cast<URealtimeDestructibleMeshComponent>(Comp);
								UpdateCellCounts();
								return;
							}
						}
					}
				}
			});
	}

	if (!OnLevelActorAddedHandle.IsValid())
	{
		OnLevelActorAddedHandle = GEditor->OnLevelActorAdded().AddUObject(this, &UAnchorActionObejct::OnLevelActorAdded);
	}

	if (!OnLevelActorDeletedHandle.IsValid())
	{
		OnLevelActorDeletedHandle = GEditor->OnLevelActorDeleted().AddUObject(this, &UAnchorActionObejct::OnLevelActorDeleted);
	}

	if (USelection* SelectedActors = GEditor->GetSelectedActors())
	{
		if (!OnSelectionChangedHandle_Actors.IsValid())
		{
			OnSelectionChangedHandle_Actors = SelectedActors->SelectionChangedEvent.AddUObject(this, &UAnchorActionObejct::OnEditorSelectionChanged);
		}

		if (!OnSelectObjectHandle_Actors.IsValid())
		{
			OnSelectObjectHandle_Actors = SelectedActors->SelectObjectEvent.AddUObject(this, &UAnchorActionObejct::OnEditorSelectObject);
		}
	}

	if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
	{
		if (!OnSelectionChangedHandle_Components.IsValid())
		{
			OnSelectionChangedHandle_Components = SelectedComponents->SelectionChangedEvent.AddUObject(this, &UAnchorActionObejct::OnEditorSelectionChanged);
		}

		if (!OnSelectObjectHandle_Components.IsValid())
		{
			OnSelectObjectHandle_Components = SelectedComponents->SelectObjectEvent.AddUObject(
				this, &UAnchorActionObejct::OnEditorSelectObject);
		}
	}

	bEditorDelegatesBound = true;
}

void UAnchorActionObejct::UnBindEditorDelgates()
{
	if (!bEditorDelegatesBound)
	{
		return;
	}

	if (OnObjectsReplacedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(OnObjectsReplacedHandle);
		OnObjectsReplacedHandle.Reset();
	}

	if (GEditor)
	{
		if (OnLevelActorAddedHandle.IsValid())
		{
			GEditor->OnLevelActorAdded().Remove(OnLevelActorAddedHandle);
			OnLevelActorAddedHandle.Reset();
		}

		if (OnLevelActorDeletedHandle.IsValid())
		{
			GEditor->OnLevelActorDeleted().Remove(OnLevelActorDeletedHandle);
			OnLevelActorDeletedHandle.Reset();
		}
		
		if (USelection* SelectedActors = GEditor->GetSelectedActors())
		{
			if (OnSelectionChangedHandle_Actors.IsValid())
			{
				SelectedActors->SelectionChangedEvent.Remove(OnSelectionChangedHandle_Actors);
				OnSelectionChangedHandle_Actors.Reset();
			}
			if (OnSelectObjectHandle_Actors.IsValid())
			{
				SelectedActors->SelectObjectEvent.Remove(OnSelectObjectHandle_Actors);
				OnSelectObjectHandle_Actors.Reset();
			}
		}
	}

	if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
	{
		if (OnSelectionChangedHandle_Components.IsValid())
		{
			SelectedComponents->SelectionChangedEvent.Remove(OnSelectionChangedHandle_Components);
			OnSelectionChangedHandle_Components.Reset();
		}
		if (OnSelectObjectHandle_Components.IsValid())
		{
			SelectedComponents->SelectObjectEvent.Remove(OnSelectObjectHandle_Components);
			OnSelectObjectHandle_Components.Reset();
		}
	}
	
	bEditorDelegatesBound = false;
}

bool UAnchorActionObejct::ResolveTargetComponent(UWorld* World)
{
	if (IsValid(TargetComp))
	{
		return true;
	}

	RefreshTargetFromEditorSelection(World);
	if (IsValid(TargetComp))
	{
		return true;
	}

	AActor* Owner = TargetOwner.Get();
	if (!Owner || TargetCompName.IsNone())
	{
		return false;
	}

	TArray<URealtimeDestructibleMeshComponent*> Components;
	Owner->GetComponents<URealtimeDestructibleMeshComponent>(Components);
	for (auto Comp : Components)
	{
		if (IsValid(Comp) && Comp->GetFName() == TargetCompName && Comp->GetWorld() == World && !Comp->IsTemplate())
		{
			TargetComp = Comp;
			return true;
		}
	}

	return false;
}

void UAnchorActionObejct::RefreshTargetFromEditorSelection(UWorld* World)
{
	if (!GEditor || !World)
	{
		return;
	}

	if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
	{
		for (FSelectionIterator It(*SelectedComponents); It; ++It)
		{
			if (URealtimeDestructibleMeshComponent* Comp = Cast<URealtimeDestructibleMeshComponent>(*It))
			{
				if (IsValid(Comp) && Comp->GetWorld() == World && !Comp->IsTemplate())
				{
					TargetComp = Comp;
					TargetOwner = Comp->GetOwner();
					TargetCompName = Comp->GetFName();
					SelectedComponentName = TargetComp->GetName();
					UpdateCellCounts();
					return;
				}
			}
		}
	}

	if (USelection* SelectedActors = GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*SelectedActors); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (!IsValid(Actor) || Actor->GetWorld() != World)
			{
				continue;
			}

			URealtimeDestructibleMeshComponent* Comp = Actor->FindComponentByClass<
				URealtimeDestructibleMeshComponent>();
			if (IsValid(Comp) && !Comp->IsTemplate())
			{
				TargetComp = Comp;
				TargetOwner = Actor;
				TargetCompName = Comp->GetFName();
				SelectedComponentName = TargetComp->GetName();
				UpdateCellCounts();
				return;
			}
		}
	}
}

void UAnchorActionObejct::OnEditorSelectionChanged(UObject* NewSeletion)
{
	if (!GEditor)
	{
		return;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	RefreshTargetFromEditorSelection(World);
}

void UAnchorActionObejct::OnEditorSelectObject(UObject* Object)
{
	if (!GEditor)
	{
		return;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	RefreshTargetFromEditorSelection(World);
}

void UAnchorActionObejct::OnLevelActorAdded(AActor* InActor)
{
	if (!GEditor || !IsValid(InActor))
	{
		return;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World || InActor->GetWorld() != World)
	{
		return;
	}

	AAnchorActor* AnchorActor = Cast<AAnchorActor>(InActor);
	if (!IsValid(AnchorActor))
	{
		return;
	}

	AnchorActors.AddUnique(TWeakObjectPtr<AAnchorActor>(AnchorActor));
}

void UAnchorActionObejct::OnLevelActorDeleted(AActor* InActor)
{
	if (!IsValid(InActor))
	{
		return;
	}

	AAnchorActor* AnchorActor = Cast<AAnchorActor>(InActor);
	if (!IsValid(AnchorActor))
	{
		return;
	}

	AnchorActors.RemoveAll([AnchorActor](const TWeakObjectPtr<AAnchorActor>& Ptr)
	{
		return Ptr.Get() == AnchorActor;
	});
}
