#pragma once

#include "CoreMinimal.h"
#include "DestructionTypes.generated.h"

UENUM(BlueprintType)
enum class EDestructionToolShape : uint8
{
    Sphere      UMETA(DisplayName = "Sphere"),
    Cylinder    UMETA(DisplayName = "Cylinder")
};

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDestructionToolShapeParams
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
    float Radius = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Cylinder")
    float Height = 400.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Cylinder", meta = (ClampMin = 3, ClampMax = 64))
    int32 RadiusSteps = 12;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Cylinder", meta = (ClampMin = 0, ClampMax = 32))
    int32 HeightSubdivisions = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Cylinder")
    bool bCapped = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Sphere", meta = (ClampMin = 3, ClampMax = 64))
    int32 StepsPhi = 8;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Sphere", meta = (ClampMin = 3, ClampMax = 128))
    int32 StepsTheta = 16;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape|Box")
    FVector BoxSize = FVector(20.0f, 20.0f, 20.0f);
};
