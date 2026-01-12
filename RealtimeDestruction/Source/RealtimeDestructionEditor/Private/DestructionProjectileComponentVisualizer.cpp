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

	FLinearColor DecalColor = FLinearColor(0.0f, 1.0f, 0.5f, 1.0f);
	DrawDecalPreview(ProjectileComp, PDI, DecalColor);
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

void FDestructionProjectileComponentVisualizer::DrawDecalPreview(const class UDestructionProjectileComponent* Component,
	FPrimitiveDrawInterface* PDI, const FLinearColor& Color)
{
	if (!Component || !Component->GetOwner())
	{
		return;
	}

	FVector DecalSize; 
	FVector LocationOffset;
	FRotator RotationOffset;
	Component->GetCalculateDecalSize(LocationOffset, RotationOffset, DecalSize);

	
	FVector Location = Component->GetComponentLocation();
	FRotator Rotation = Component->GetComponentRotation();

	Location += LocationOffset;
	Rotation += RotationOffset;
	
	FTransform Transform(Rotation, Location);
	FVector YAxis = Transform.GetUnitAxis(EAxis::Y);
	FVector ZAxis = Transform.GetUnitAxis(EAxis::Z);

	float HalfY = DecalSize.Y * 0.5f;
	float HalfZ = DecalSize.Z * 0.5f;

	// 사각형 4개 꼭지점
	FVector TopLeft = Location + YAxis * (-HalfY) + ZAxis * HalfZ;
	FVector TopRight = Location + YAxis * HalfY + ZAxis * HalfZ;
	FVector BottomRight = Location + YAxis * HalfY + ZAxis * (-HalfZ);
	FVector BottomLeft = Location + YAxis * (-HalfY) + ZAxis * (-HalfZ);

	float Thickness = 1.5f;
	
	// 사각형 그리기
	PDI->DrawLine(TopLeft, TopRight, Color, SDPG_World, Thickness);
	PDI->DrawLine(TopRight, BottomRight, Color, SDPG_World, Thickness);
	PDI->DrawLine(BottomRight, BottomLeft, Color, SDPG_World, Thickness);
	PDI->DrawLine(BottomLeft, TopLeft, Color, SDPG_World, Thickness);

	// 대각선 (X 표시로 데칼임을 표시)
	PDI->DrawLine(TopLeft, BottomRight, Color, SDPG_World, Thickness * 0.5f);
	PDI->DrawLine(TopRight, BottomLeft, Color, SDPG_World, Thickness * 0.5f);
} 
