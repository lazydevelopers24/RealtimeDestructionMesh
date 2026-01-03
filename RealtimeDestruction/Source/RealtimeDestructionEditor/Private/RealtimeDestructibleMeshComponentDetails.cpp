#include "RealtimeDestructibleMeshComponentDetails.h"

#include "Components/RealtimeDestructibleMeshComponent.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"

#define LOCTEXT_NAMESPACE "RealtimeDestructibleMeshComponentDetails"

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

	// "Cell Mesh" 카테고리 - 안내 텍스트
	IDetailCategoryBuilder& CellMeshCategory = DetailBuilder.EditCategory(
		"CellMesh",
		LOCTEXT("CellMeshCategoryName", "Cell Mesh"),
		ECategoryPriority::Important
	);

	// 안내 텍스트
	CellMeshCategory.AddCustomRow(LOCTEXT("CellMeshInfoRow", "Info"))
		[
			SNew(SBox)
			.Padding(FMargin(0.f, 4.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CellMeshInfo", "GeometryCollection을 사용하여 메시를 분리합니다.\n\n1. Fracture Mode에서 GeometryCollection 에셋 생성\n2. FracturedGeometryCollection에 할당\n3. BuildCellMeshesFromGeometryCollection 호출"))
				.Font(IDetailLayoutBuilder::GetDetailFontItalic())
				.AutoWrapText(true)
			]
		];
}

#undef LOCTEXT_NAMESPACE
