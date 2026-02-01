// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

#include "RealtimeDestructibleMeshComponentDetails.h"

#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h" 
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

TSharedRef<IDetailCustomization> FRealtimeDestructibleMeshComponentDetails::MakeInstance()
{
	return MakeShareable(new FRealtimeDestructibleMeshComponentDetails);
}

void FRealtimeDestructibleMeshComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// 선택된 오브젝트 가져오기
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);

	SelectedComponents.Empty();
	for (TWeakObjectPtr<UObject>& Obj : ObjectsBeingCustomized)
	{
		if (URealtimeDestructibleMeshComponent* Comp = Cast<URealtimeDestructibleMeshComponent>(Obj.Get()))
		{
			SelectedComponents.Add(Comp);
		}
	}


	//////////////////////////////////////////////////////////////////////////
	// 카테고리 순서 지정
	// Transform 다음에 RealtimeDestructibleMesh 카테고리가 오도록 설정
	//////////////////////////////////////////////////////////////////////////

	// RealtimeDestructibleMesh 최상위 카테고리를 Important로 설정하여 상단 배치
	// 하위 카테고리는 기본 순서 유지
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
		"RealtimeDestructibleMesh",
		FText::GetEmpty(),
		ECategoryPriority::Important
	);

	Category.AddCustomRow(FText::FromString("Editor Actions"))
		.NameContent()
		[
			SNew(STextBlock)
				.Text(FText::FromString("Editor Actions"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(400.f)
		[
			SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.f)
				[
					SNew(SButton)
						.Text(FText::FromString("Generate Chunks"))
						.ToolTipText(FText::FromString("Creates a GeometryCollection from SourceStaticMesh and builds chunk meshes."))
						.OnClicked(this, &FRealtimeDestructibleMeshComponentDetails::OnGenerateChunksClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.f)
				[
					SNew(SButton)
						.Text(FText::FromString("Revert Chunks"))
						.ToolTipText(FText::FromString("Destroys all ChunkMeshComponents and reverts to the state before chunk meshes were generated."))
						.OnClicked(this, &FRealtimeDestructibleMeshComponentDetails::OnRevertChunksClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(2.f)
				[
					SNew(SButton)
						.Text(FText::FromString("Build Grid Cells"))
						.ToolTipText(FText::FromString(
							"Generates grid cells from SourceStaticMesh.\n\n"
							"WARNING: The Grid Cell system is generated based on world coordinates. "
							"If you change the world scale of this component at runtime, there will be "
							"a mismatch between grid cells and the actual mesh, causing inaccurate destruction detection. "
							"If you need to change the scale, you must call BuildGridCells() again."))
						.OnClicked(this, &FRealtimeDestructibleMeshComponentDetails::OnBuildGridCellsClicked)
				]
		]; 
}

FReply FRealtimeDestructibleMeshComponentDetails::OnGenerateChunksClicked()
{
	for (TWeakObjectPtr<URealtimeDestructibleMeshComponent>& WeakComp : SelectedComponents)
	{
		if (URealtimeDestructibleMeshComponent * Comp = WeakComp.Get())
		{
			Comp->GenerateDestructibleChunks();

			if (UBlueprint* Blueprint = GetBlueprintFromComponent(Comp))
			{
				ForceCompileBlueprint(Blueprint);
			}
		}
	}

	return FReply::Handled();
}

FReply FRealtimeDestructibleMeshComponentDetails::OnRevertChunksClicked()
{
	for (TWeakObjectPtr<URealtimeDestructibleMeshComponent>& WeakComp : SelectedComponents)
	{
		if (URealtimeDestructibleMeshComponent* Comp = WeakComp.Get())
		{
			Comp->CachedGeometryCollection = nullptr;

			// 현재 작업 공간이 blueprint라면 CDO 값 복구 
			if (UBlueprint* Blueprint = GetBlueprintFromComponent(Comp))
			{
				if (Comp->SourceStaticMesh)
				{
					Comp->InitializeFromStaticMeshInternal(Comp->SourceStaticMesh, true);
				}

				if (AActor* PreviewActor = Blueprint->SimpleConstructionScript ?
					Blueprint->SimpleConstructionScript->GetComponentEditorActorInstance() : nullptr)
				{
					TArray<URealtimeDestructibleMeshComponent*> PreviewComps;

					PreviewActor->GetComponents<URealtimeDestructibleMeshComponent>(PreviewComps);

					for (URealtimeDestructibleMeshComponent* PreviewComp : PreviewComps)
					{
						PreviewComp->CachedGeometryCollection = nullptr;

						for (UDynamicMeshComponent* ChunkComp : PreviewComp->ChunkMeshComponents)
						{
							if (ChunkComp)
							{
								ChunkComp->DestroyComponent();
							}
						}
						PreviewComp->ChunkMeshComponents.Empty();

						if (PreviewComp->SourceStaticMesh)
						{
							PreviewComp->InitializeFromStaticMeshInternal(PreviewComp->SourceStaticMesh, true);
						}
					}
				}

				ForceCompileBlueprint(Blueprint);
			}

			else
			{
				Comp->RevertChunksToSourceMesh();
			}
		}
	}
	return FReply::Handled();
}

FReply FRealtimeDestructibleMeshComponentDetails::OnBuildGridCellsClicked()
{
	for (TWeakObjectPtr<URealtimeDestructibleMeshComponent>& WeakComp : SelectedComponents)
	{
		if (URealtimeDestructibleMeshComponent* Comp = WeakComp.Get())
		{
			Comp->BuildGridCells();
		}
	}
	return FReply::Handled();
}

UBlueprint* FRealtimeDestructibleMeshComponentDetails::GetBlueprintFromComponent(URealtimeDestructibleMeshComponent* Component)
{
	if (!Component)
	{
		return nullptr;
	}
	UObject* Outer = Component->GetOuter();
	while (Outer)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Outer))
		{
			return Blueprint;
		}
		if (UBlueprintGeneratedClass* BPGC = Cast< UBlueprintGeneratedClass>(Outer))
		{
			return Cast<UBlueprint>(BPGC->ClassGeneratedBy);
		}
		Outer = Outer->GetOuter();
	}
	return nullptr;
}

void FRealtimeDestructibleMeshComponentDetails::ForceCompileBlueprint(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}

	Blueprint->Modify();
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
}

