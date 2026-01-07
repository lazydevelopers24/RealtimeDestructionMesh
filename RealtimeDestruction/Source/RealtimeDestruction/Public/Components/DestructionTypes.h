#pragma once

#include "CoreMinimal.h"
#include "DestructionTypes.generated.h"

UENUM(BlueprintType)
enum class EDestructionToolShape : uint8
{
    Sphere      UMETA(DisplayName = "Sphere"),
    Cylinder    UMETA(DisplayName = "Cylinder")
};
/**
 * 서버 파괴 요청 거부 사유
 */
UENUM(BlueprintType)
enum class EDestructionRejectReason : uint8
{
    None               UMETA(DisplayName = "None"),
    OutOfRange         UMETA(DisplayName = "Out of Range"),          // 사거리 초과
    LineOfSightBlocked UMETA(DisplayName = "Line of Sight Blocked"), // 시야 차단
    RateLimited        UMETA(DisplayName = "Rate Limited"),          // 연사 제한
    InvalidPosition    UMETA(DisplayName = "Invalid Position"),      // 유효하지 않은 위치
    Indestructible     UMETA(DisplayName = "Indestructible"),        // 파괴 불가
    MaxHoleReached     UMETA(DisplayName = "Max Hole Reached")       // 최대 구멍 수 도달
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

USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FUnionFind
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<int32> Parent;

    UPROPERTY()
    TArray<int32> Rank;

    void Init(int32 Count)
    {
        Parent.SetNumUninitialized(Count);
        Rank.SetNumZeroed(Count);

        for (int i = 0; i < Count; ++i)
        {
            Parent[i] = i;
        }
    }

    int32 Find(int32 X)
    {
        if (!Parent.IsValidIndex(X))
        {
            return -1;
        }

        if (Parent[X] != X)
        {
            Parent[X] = Find(Parent[X]);
        }

        return Parent[X];
    }

    void Union(int32 A, int32 B)
    {
        int32 RootA = Find(A);
        int32 RootB = Find(B);

        if (RootA == -1 || RootB == -1)
        {
            return;
        }

        if (RootA != RootB)
        {
            if (Rank[RootA] < Rank[RootB])
            {
                Parent[RootA] = RootB;
            }
            else if (Rank[RootA] > Rank[RootB])
            {
                Parent[RootB] = RootA;
            }
            else
            {
                Parent[RootB] = RootA;
                Rank[RootA]++;
            }
        }
    }
};


USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FBulletCluster
{
    GENERATED_BODY()

    UPROPERTY()
    FVector Center = FVector::ZeroVector;

    UPROPERTY()
    FVector Normal = FVector::UpVector;

    UPROPERTY()
    float Radius = 0.0f;

    UPROPERTY()
    TArray<FVector> MemberPoints;

    UPROPERTY()
    TArray<FVector> MemberNormals;

    UPROPERTY()
    TArray<float> MemberRadius;


    void Init(FVector Point, FVector InNormal, float InRadius)
    {
        Center = Point;
        Normal = InNormal;
        Radius = InRadius;

        MemberPoints.Empty();
        MemberNormals.Empty();
        MemberRadius.Empty();

        MemberPoints.Add(Point);
        MemberNormals.Add(InNormal);
        MemberRadius.Add(InRadius);
    }
    void AddMember(FVector Point, FVector InNormal, float InRadius)
    {
        MemberPoints.Add(Point);
        MemberNormals.Add(InNormal);
        MemberRadius.Add(InRadius);

        // Normal
        Normal += InNormal;
        Normal = Normal.GetSafeNormal();

        // Center, Radius 확장 
        float Dist = FVector::Dist(Center, Point);

        if (Dist + InRadius <= Radius)
        {
            return;
        }

        float NewRadius = (Radius + Dist + InRadius) * 0.5f;

        if (Dist > KINDA_SMALL_NUMBER)
        {
            FVector Dir = (Point - Center).GetSafeNormal();

            Center += Dir * (NewRadius - Radius);
        }

        Radius = NewRadius;

    }

    float PredictRadius(const FVector& Point, float InRadius) const
    {
        float Dist = FVector::Dist(Center, Point);

        if (Dist + InRadius <= Radius)
        {
            return Radius;
        }

        return (Radius + Dist + InRadius);
    }

};

