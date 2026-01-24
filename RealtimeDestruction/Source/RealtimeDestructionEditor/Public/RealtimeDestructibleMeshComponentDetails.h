// Copyright (c) 2026 LazyDevelopers <lazydeveloper24@gmail.com>. All rights reserved.
// This plugin is distributed under the Fab Standard License.
//
// This product was independently developed by us while participating in the Epic Project, a developer-support
// program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively
// vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations
// and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from
// the use of this product.

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
