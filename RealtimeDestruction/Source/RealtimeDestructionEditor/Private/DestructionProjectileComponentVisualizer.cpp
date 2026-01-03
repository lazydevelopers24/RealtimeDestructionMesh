#include "DestructionProjectileComponentVisualizer.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "Components/DestructionProjectileComponent.h"

void FDestructionProjectileComponentVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const UDestructionProjectileComponent* ProjectileComp =
		Cast<const UDestructionProjectileComponent>(Component);

	if (!ProjectileComp || !ProjectileComp->GetOwner())
	{
		return;
	}
	 
	
	FLinearColor DrawColor = FLinearColor(1.0f, 1.0f,0.0f, 1.0f);
	switch (ProjectileComp->ToolShape)
	{
	case EDestructionToolShape::Sphere:
		DrawSphere(ProjectileComp, PDI, DrawColor);
		break;

	case EDestructionToolShape::Cylinder:
		DrawCylinder(ProjectileComp, PDI, DrawColor);
		break;

	default:
		break; 
	}

}

void FDestructionProjectileComponentVisualizer::DrawSphere(const UDestructionProjectileComponent* Component, FPrimitiveDrawInterface* PDI, const FLinearColor& Color)
{
	if (!Component || !Component->GetOwner())
	{
		return;
	}

	FVector Location = Component->GetOwner()->GetActorLocation();
	float Radius = Component->SphereRadius;

	float Thickness = 2.0f;
	int32 Segments = Component->SphereStepsTheta;

	DrawWireSphere(
		PDI,
		Location,
		Color,
		Radius,
		Segments,
		SDPG_World,
		0.0f,
		true
	);

}

void FDestructionProjectileComponentVisualizer::DrawCylinder(const UDestructionProjectileComponent* Component, FPrimitiveDrawInterface* PDI, const FLinearColor& Color)
{
	if (!Component || !Component->GetOwner())
	{
		return;
	} 

	FVector Location = Component->GetComponentLocation();  
	FRotator Rotation = Component->GetComponentRotation(); 

	float Radius = Component->CylinderRadius;
	float HalfHeight = Component->CylinderHeight * 0.5f;
	int32 Segments = FMath::Max(4, Component->RadialSteps);
	float Thickness = 2.0f;

	//FVector Base = Location - FVector(0, 0, HalfHeight);
	FVector Base = Location - Rotation.RotateVector(FVector(0, 0, HalfHeight));

	FTransform Transform(Rotation, Location);
	FVector XAxis = Transform.GetUnitAxis(EAxis::X);
	FVector YAxis = Transform.GetUnitAxis(EAxis::Y);
	FVector ZAxis = Transform.GetUnitAxis(EAxis::Z);

	// Cylinder 와이어프레임 그리기
	DrawWireCylinder(
		PDI,
		Base,
		XAxis,
		YAxis,
		ZAxis,
		Color,
		Radius,
		HalfHeight,
		Segments,
		SDPG_World,
		Thickness,
		0.0f,
		true
	);
} 
