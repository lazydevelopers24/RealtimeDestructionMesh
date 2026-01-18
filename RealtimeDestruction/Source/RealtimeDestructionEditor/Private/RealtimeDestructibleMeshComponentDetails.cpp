#include "RealtimeDestructibleMeshComponentDetails.h"

#include "Components/RealtimeDestructibleMeshComponent.h"
#include "DetailLayoutBuilder.h"

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
	DetailBuilder.EditCategory("RealtimeDestructibleMesh", FText::GetEmpty(), ECategoryPriority::Important);
}
