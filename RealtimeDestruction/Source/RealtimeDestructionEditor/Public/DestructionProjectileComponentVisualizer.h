#pragma once

#include "CoreMinimal.h"
#include "ComponentVisualizer.h"

/**
 * DestructProjectileComponent용 Visualizer
 * 에디터 뷰포트에서 ToolShape을 wireframe으로 그려준다.
 */

class FDestructionProjectileComponentVisualizer : public FComponentVisualizer
{
public:
	/**
	 * 컴포넌트 시각화
	 */
	virtual void DrawVisualization(
		const UActorComponent* Component,
		const FSceneView* View,
		FPrimitiveDrawInterface* PDI
	) override; 


private:
	/** Sphere 시각화 */
	void DrawSphere(
		const class UDestructionProjectileComponent* Component,
		FPrimitiveDrawInterface* PDI,
		const FLinearColor& Color
		);

	/** Cylinder 시각화 */
	void DrawCylinder(
		const class UDestructionProjectileComponent* Component,
		FPrimitiveDrawInterface* PDI,
		const FLinearColor& Color
	);

};





