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
#include "StructuralIntegrity/StructuralIntegrityTypes.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "ProceduralMeshComponent.h"
#include "DebrisTypes.generated.h"

//////////////////////////////////////////////////////////////////////////
// Debris Enums
//////////////////////////////////////////////////////////////////////////

/**
 * Debris type classification
 */
UENUM(BlueprintType)
enum class EDebrisType : uint8
{
	Cosmetic,    // Local only, short lifespan, no gameplay impact
	Gameplay     // Server authoritative, replicated, physics interaction enabled
};

/**
 * Debris size tier
 */
UENUM(BlueprintType)
enum class EDebrisTier : uint8
{
	Tiny,      // < 100 cm³ - Replaced with particles
	Small,     // 100-500 cm³ - Sphere collision
	Medium,    // 500-2000 cm³ - Box collision
	Large,     // 2000-10000 cm³ - Convex Hull
	Massive    // > 10000 cm³ - Complex collision
};

//////////////////////////////////////////////////////////////////////////
// Debris Structs
//////////////////////////////////////////////////////////////////////////

/**
 * Per-tier debris settings
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDebrisTierConfig
{
	GENERATED_BODY()

	// Volume upper limit for this tier (cm³)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier")
	float VolumeThreshold = 0.0f;

	// Debris lifespan (seconds, 0 for permanent)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier")
	float Lifespan = 3.0f;

	// Maximum debris count
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier", meta = (ClampMin = "1"))
	int32 MaxCount = 50;

	// Whether cosmetic (false = Gameplay - network replicated)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebrisTier")
	bool bIsCosmetic = true;

	FDebrisTierConfig() = default;

	FDebrisTierConfig(float InVolumeThreshold, float InLifespan, int32 InMaxCount, bool bInIsCosmetic)
		: VolumeThreshold(InVolumeThreshold)
		, Lifespan(InLifespan)
		, MaxCount(InMaxCount)
		, bIsCosmetic(bInIsCosmetic)
	{
	}
};

/**
 * Debris spawn settings
 */
USTRUCT(BlueprintType)
struct REALTIMEDESTRUCTION_API FDebrisSpawnSettings
{
	GENERATED_BODY()

	// Enable debris spawn
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris")
	bool bEnableDebrisSpawn = true;

	// Gameplay debris volume threshold (cm³) - Gameplay type if exceeded
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris")
	float GameplayVolumeThreshold = 2000.0f;

	// Cosmetic debris default lifespan (seconds)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Cosmetic")
	float CosmeticLifespan = 3.0f;

	// Maximum cosmetic debris count
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Cosmetic", meta = (ClampMin = "1"))
	int32 MaxCosmeticDebris = 50;

	// Gameplay debris default lifespan (seconds, 0 for permanent)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Gameplay")
	float GameplayLifespan = 0.0f;

	// Maximum gameplay debris count
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Gameplay", meta = (ClampMin = "1"))
	int32 MaxGameplayDebris = 20;

	// Debris initial impulse horizontal strength
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Physics")
	float InitialImpulseHorizontal = 100.0f;

	// Debris initial impulse vertical strength
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debris|Physics")
	float InitialImpulseVertical = 150.0f;

	// Determine debris type by volume
	EDebrisType GetDebrisType(float Volume) const
	{
		return (Volume <= GameplayVolumeThreshold) ? EDebrisType::Cosmetic : EDebrisType::Gameplay;
	}

	// Return lifespan by type
	float GetLifespanForType(EDebrisType Type) const
	{
		return (Type == EDebrisType::Cosmetic) ? CosmeticLifespan : GameplayLifespan;
	}

	// Return max count by type
	int32 GetMaxCountForType(EDebrisType Type) const
	{
		return (Type == EDebrisType::Cosmetic) ? MaxCosmeticDebris : MaxGameplayDebris;
	}
};

/**
 * Compressed debris sync Op (for network transmission)
 * Follows existing FCompactDestructionOp pattern
 */
USTRUCT()
struct REALTIMEDESTRUCTION_API FCompactDebrisOp
{
	GENERATED_BODY()

	// Packed Cell Key array: (ChunkId << 16) | CellId
	UPROPERTY()
	TArray<int32> PackedCellKeys;

	// Group ID
	UPROPERTY()
	int32 GroupId = INDEX_NONE;

	// Center of mass (1cm precision)
	UPROPERTY()
	FVector_NetQuantize CenterOfMass;

	// Approximate volume (cm³, compressed)
	UPROPERTY()
	float ApproximateVolume = 0.0f;

	// Sequence number
	UPROPERTY()
	uint16 Sequence = 0;

	FCompactDebrisOp() = default;

	// Pack Cell Keys
	static void PackCellKeys(const TArray<FCellKey>& Keys, TArray<int32>& OutPacked)
	{
		OutPacked.Reset();
		OutPacked.Reserve(Keys.Num());
		for (const FCellKey& Key : Keys)
		{
			OutPacked.Add((Key.ChunkId << 16) | (Key.CellId & 0xFFFF));
		}
	}

	// Unpack Cell Keys
	static void UnpackCellKeys(const TArray<int32>& Packed, TArray<FCellKey>& OutKeys)
	{
		OutKeys.Reset();
		OutKeys.Reserve(Packed.Num());
		for (int32 P : Packed)
		{
			OutKeys.Add(FCellKey(P >> 16, P & 0xFFFF));
		}
	}

	// Create from FDetachedCellGroup
	static FCompactDebrisOp FromDetachedGroup(const FDetachedCellGroup& Group, uint16 InSequence)
	{
		FCompactDebrisOp Op;
		PackCellKeys(Group.CellKeys, Op.PackedCellKeys);
		Op.GroupId = Group.GroupId;
		Op.CenterOfMass = Group.CenterOfMass;
		Op.ApproximateVolume = Group.ApproximateMass;
		Op.Sequence = InSequence;
		return Op;
	}

	// Restore to FDetachedCellGroup
	FDetachedCellGroup ToDetachedGroup() const
	{
		FDetachedCellGroup Group;
		Group.GroupId = GroupId;
		UnpackCellKeys(PackedCellKeys, Group.CellKeys);
		Group.CenterOfMass = CenterOfMass;
		Group.ApproximateMass = ApproximateVolume;
		return Group;
	}
};

//////////////////////////////////////////////////////////////////////////
// PMC Debris Tracking (Phase 1.5)
//////////////////////////////////////////////////////////////////////////

/**
 * Gameplay debris tracking data (for PMC → DMC conversion)
 *
 * UDynamicMeshComponent only supports TriMesh(Complex) collision, making dynamic physics simulation impossible.
 * Therefore, UProceduralMeshComponent(PMC) is used during physics simulation,
 * then converted to UDynamicMeshComponent(DMC) after stabilization to support secondary destruction.
 */
struct REALTIMEDESTRUCTION_API FGameplayDebrisTracker
{
	/** PMC component (for physics simulation) */
	TWeakObjectPtr<UProceduralMeshComponent> PMC;

	/** Original mesh data (preserved for DMC conversion) */
	TSharedPtr<UE::Geometry::FDynamicMesh3> OriginalMesh;

	/** Stable state duration (seconds) */
	float StableTime = 0.0f;

	/** Debris type */
	EDebrisType DebrisType = EDebrisType::Gameplay;

	/** Stabilization velocity threshold (cm/s) */
	static constexpr float StableVelocityThreshold = 5.0f;

	/** Required stabilization duration (seconds) */
	static constexpr float StableTimeRequired = 0.5f;

	FGameplayDebrisTracker() = default;

	FGameplayDebrisTracker(UProceduralMeshComponent* InPMC, TSharedPtr<UE::Geometry::FDynamicMesh3> InMesh, EDebrisType InType = EDebrisType::Gameplay)
		: PMC(InPMC)
		, OriginalMesh(InMesh)
		, StableTime(0.0f)
		, DebrisType(InType)
	{
	}

	/** Check if PMC is valid */
	bool IsValid() const { return PMC.IsValid(); }

	/** Whether stabilization is complete */
	bool IsStabilized() const { return StableTime >= StableTimeRequired; }

	/** Reset stabilization timer */
	void ResetStableTime() { StableTime = 0.0f; }

	/** Accumulate stabilization time */
	void AccumulateStableTime(float DeltaTime) { StableTime += DeltaTime; }
};
