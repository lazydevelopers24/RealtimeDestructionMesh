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
#include "Components/DecalComponent.h"
#include "DestructionTypes.generated.h"

UENUM(BlueprintType)
enum class EDestructionToolShape : uint8
{
    Sphere      UMETA(DisplayName = "Sphere"),
    Cylinder    UMETA(DisplayName = "Cylinder")
};
/**
 * Server destruction request rejection reason
 */
UENUM(BlueprintType)
enum class EDestructionRejectReason : uint8
{
    None               UMETA(DisplayName = "None"),
    OutOfRange         UMETA(DisplayName = "Out of Range"),          // Range exceeded
    LineOfSightBlocked UMETA(DisplayName = "Line of Sight Blocked"), // Line of sight blocked
    RateLimited        UMETA(DisplayName = "Rate Limited"),          // Rate limited
    InvalidPosition    UMETA(DisplayName = "Invalid Position"),      // Invalid position
    Indestructible     UMETA(DisplayName = "Indestructible"),        // Indestructible
    MaxHoleReached     UMETA(DisplayName = "Max Hole Reached")       // Max hole count reached
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

    // Added UPROPERTY for network serialization
    UPROPERTY()
    float SurfaceMargin = 0.0f;
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
    TArray<FVector> MemberPoints = {};

    UPROPERTY()
    TArray<FVector> MemberNormals = {};

    UPROPERTY()
    TArray<float> MemberRadius = {};

    UPROPERTY()
    TArray<int32> ChunkIndices = {};

    UPROPERTY()
    FVector AverageForwardVector = FVector::ForwardVector;

    UPROPERTY()
    FVector ForwardSum = FVector::ZeroVector;

    UPROPERTY()
    FVector ToolOriginWorld = FVector::ZeroVector;

    UPROPERTY()
    float Depth = 10.0f;

    FBulletCluster() = default;

    FBulletCluster(const FVector& InCenter, const FVector& InNormal,
                   float InRadius, const FVector& InForward,
                   const FVector& InToolCenter, int32 ChunkIndex, float InDepth)
        : Center(InCenter), Normal(InNormal), Radius(InRadius), ToolOriginWorld(InToolCenter), Depth(InDepth)
    {
        ForwardSum = InForward.GetSafeNormal();
        AverageForwardVector = ForwardSum.IsNearlyZero() ? FVector::ForwardVector : ForwardSum;

        MemberPoints.Empty();
        MemberNormals.Empty();
        MemberRadius.Empty();
        ChunkIndices.Empty();

        MemberPoints.Add(InCenter);
        MemberNormals.Add(InNormal);
        MemberRadius.Add(InRadius);
        ChunkIndices.Add(ChunkIndex);
    }

    void Init(FBulletCluster&& Other)
    {
        Center = Other.Center;
        Normal = Other.Normal;
        Radius = Other.Radius;
        Depth = Other.Depth;
        
        AverageForwardVector = Other.AverageForwardVector;
        ForwardSum = Other.ForwardSum;
        ToolOriginWorld = Other.ToolOriginWorld;

        MemberPoints = MoveTemp(Other.MemberPoints);
        MemberNormals = MoveTemp(Other.MemberNormals);
        MemberRadius = MoveTemp(Other.MemberRadius);
        ChunkIndices = MoveTemp(Other.ChunkIndices);
    }

    void Init(const FVector& Point, const FVector& InNormal, const FVector& Forward, const FVector& InToolOriginWorld,
        float InRadius, int ChunkIndex, float InDepth)
    {
        Center = Point;
        Normal = InNormal;
        Radius = InRadius;
        Depth = InDepth;

        MemberPoints.Empty();
        MemberNormals.Empty();
        MemberRadius.Empty();
        ChunkIndices.Empty();


        MemberPoints.Add(Point);
        MemberNormals.Add(InNormal);
        MemberRadius.Add(InRadius);
        ChunkIndices.Add(ChunkIndex);

        ForwardSum = Forward.GetSafeNormal();
        AverageForwardVector = ForwardSum.IsNearlyZero() ? FVector::ForwardVector : ForwardSum;
        ToolOriginWorld = InToolOriginWorld;
    }
    void AddMember(const FVector& Point, const FVector& InNormal, const FVector& InForward, float InRadius, int ChunkIndex )
    {
        MemberPoints.Add(Point);
        MemberNormals.Add(InNormal);
        MemberRadius.Add(InRadius);
        ChunkIndices.Add(ChunkIndex);

        // Normal
        Normal += InNormal;
        Normal = Normal.GetSafeNormal();

        const FVector SafeForward = InForward.GetSafeNormal();
        if (!SafeForward.IsNearlyZero())
        {
            ForwardSum += SafeForward;
            AverageForwardVector += ForwardSum.GetSafeNormal();
        }

        // Expand Center, Radius
        float Dist = FVector::Dist(Center, Point);

        if (Dist + InRadius <= Radius)
        {
            return;
        }

        float NewRadius = (Radius + Dist + InRadius) * 0.5f;

        Radius = NewRadius; 
    }

    void Shutdown()
        {
        Center = FVector::ZeroVector;

        Normal = FVector::UpVector;

        Radius = 0.0f;

        MemberPoints.Empty();
        MemberNormals.Empty();
        MemberRadius.Empty();

        ForwardSum = FVector::ZeroVector;
        AverageForwardVector = FVector::ForwardVector;
    }


    float PredictRadius(const FVector& Point, float InRadius) const
    {
        float Dist = FVector::Dist(Center, Point);

        if (Dist + InRadius <= Radius)
        {
            return Radius;
        }

        return (Radius + Dist + InRadius) * 0.5f;
    }

};

USTRUCT()
struct FManagedDecal
{
    GENERATED_BODY()

    TWeakObjectPtr<UDecalComponent> Decal;
    int32 RemainingCellCount = 0;

    bool IsValid() const
    {
        return Decal.IsValid() && RemainingCellCount > 0;
    }
};
struct FVertexKey
{
    int32 VertexId = -1;
    int32 NormalElem = -1;
    int32 UVElem = -1;
    bool operator==(const FVertexKey& Other) const
    {
        return VertexId == Other.VertexId && NormalElem == Other.NormalElem && UVElem == Other.UVElem;
    }
    friend  uint32 GetTypeHash(const FVertexKey& Key)
    {
        const uint32 Hash0 = ::GetTypeHash(Key.VertexId);
        const uint32 Hash1 = ::GetTypeHash(Key.NormalElem);
        const uint32 Hash2 = ::GetTypeHash(Key.UVElem);
        return HashCombine(HashCombine(Hash0, Hash1), Hash2);
    }
};	