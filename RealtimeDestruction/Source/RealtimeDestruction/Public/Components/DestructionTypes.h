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
    FVector Normal= FVector::UpVector;
    
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
         
        // Normal?  

        // Center, Radius 확장 
        float Dist = FVector::Dist(Center, Point);

        if (Dist + InRadius <= Radius)
        {
            return;
        }

        float NewRaidus = (Radius + Dist + InRadius) * 0.5f;

        if (Dist > KINDA_SMALL_NUMBER)
        {
            FVector Dir = (Point - Center).GetSafeNormal();

            Center += Dir * (NewRaidus - Radius);
        }

        Radius = NewRaidus;

    }
     
};
