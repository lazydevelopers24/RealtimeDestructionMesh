#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class URealtimeDestructibleMeshComponent;

/**
 * RealtimeDestructibleMeshComponent의 디테일 패널 커스터마이징
 * GeometryCollection 기반 Cell Mesh 안내 표시
 */
class FRealtimeDestructibleMeshComponentDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	// IDetailCustomization
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** 선택된 컴포넌트들 */
	TArray<TWeakObjectPtr<URealtimeDestructibleMeshComponent>> SelectedComponents;
};
