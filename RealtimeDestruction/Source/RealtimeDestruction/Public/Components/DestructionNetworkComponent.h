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
#include "Components/ActorComponent.h"
#include "RealtimeDestructibleMeshComponent.h"
#include "DestructionNetworkComponent.generated.h"

/**
 * Network component that forwards destruction requests to the server.
 *
 * Add this component to a PlayerController and
 * DestructionProjectileComponent will automatically find and use it.
 *
 * Usage Example:
 * 1. Open BP_PlayerController
 * 2. Add Component -> DestructionNetworkComponent
 * 3. Done!
 */
UCLASS(ClassGroup=(RealtimeDestruction), meta=(BlueprintSpawnableComponent, DisplayName="Destruction Network"))
class REALTIMEDESTRUCTION_API UDestructionNetworkComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDestructionNetworkComponent();

	/**
	 * Forwards destruction request to server.
	 * Automatically called by DestructionProjectileComponent.
	 *
	 * @param DestructComp - The mesh component to destroy
	 * @param Request - Destruction request info (location, normal, radius)
	 */
	UFUNCTION(BlueprintCallable, Category="Destruction")
	void RequestDestruction(URealtimeDestructibleMeshComponent* DestructComp, const FRealtimeDestructionRequest& Request);

protected:
	virtual void BeginPlay() override;

	/**
	 * Process destruction on server (Server RPC) - Legacy method
	 * Called from client, executed on server
	 */
	UFUNCTION(Server, Reliable)
	void ServerApplyDestruction(URealtimeDestructibleMeshComponent* DestructComp, const FRealtimeDestructionRequest& Request);

	/**
	 * Process destruction on server (Server RPC) - Compact method
	 * Reduces network bandwidth by ~65%
	 */
	UFUNCTION(Server, Reliable)
	void ServerApplyDestructionCompact(URealtimeDestructibleMeshComponent* DestructComp, const FCompactDestructionOp& CompactOp);

	/**
	 * Validate destruction request (called on server)
	 * Calls RealtimeDestructibleMeshComponent's ValidateDestructionRequest
	 */
	bool ValidateDestructionRequest(
		URealtimeDestructibleMeshComponent* DestructComp,
		const FRealtimeDestructionRequest& Request,
		EDestructionRejectReason& OutReason) const;

protected:
	/** Maximum allowed destruction radius (anti-cheat) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Validation")
	float MaxAllowedRadius = 100.0f;

	/** Enable request validation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Validation")
	bool bEnableValidation = true;

	/**
	 * Use compressed network data
	 * true: Use FCompactDestructionOp (11 bytes)
	 * false: Use FRealtimeDestructionRequest (32+ bytes)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Destruction|Network")
	bool bUseCompactData = true;

private:
	/** Sequence counter (for compact data) */
	int32 LocalSequence = 0;
};
