// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Mass/EntityHandle.h"
#include "Mass/NPCMassFragments.h"
#include "NPCMassActorComponent.generated.h"

class UHealthAttributeSet;
class UPersistenceWorldSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNPCMassOnHydrated, const FNPCMassSnapshot&, Snapshot);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FNPCMassOnFlushPending);

/**
 * Bridges a hydrated ACharacter to its backing Mass entity. Add to a BP descendant of APersistenceLabCharacter
 * that is selected as the HighResSpawnedActor in the UMassRepresentationTrait of the entity config.
 *
 * Authority model: actor is authoritative while hydrated. Fragments are written on EndPlay (the actor's
 * dehydration point) and on UPersistenceWorldSubsystem::OnPreFlushMassEntityData (save-time flush).
 *
 * GAS health auto-sync: if the owning actor implements IAbilitySystemInterface and the ASC carries a
 * UHealthAttributeSet, this component automatically pushes Snapshot.Health/MaxHealth into the AS on
 * hydrate and pulls them back into PendingSnapshot before each flush. BP listeners can still override
 * (the OnFlushPending broadcast fires after the C++ pull).
 *
 * Owning actor still wires the non-health fields:
 *  - BP-bind OnHydrated and use Snapshot.ActiveStateId / Snapshot.NavTarget to seed StateTree / nav.
 *  - Continuously update PendingSnapshot via SetPendingSnapshot, OR bind OnFlushPending and push then.
 */
UCLASS(ClassGroup = (NPC), meta = (BlueprintSpawnableComponent), Blueprintable)
class PERSISTENCEEXAMPLES_API UNPCMassActorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UNPCMassActorComponent();

	/** Broadcast after hydration with the values just read from fragments. Bind in BP to seed actor state. */
	UPROPERTY(BlueprintAssignable, Category = "NPC|Mass")
	FNPCMassOnHydrated OnHydrated;

	/** Broadcast at the top of FlushToEntity so the owning actor can call SetPendingSnapshot with fresh data. */
	UPROPERTY(BlueprintAssignable, Category = "NPC|Mass")
	FNPCMassOnFlushPending OnFlushPending;

	/**
	 * Latest actor-authored state. Written to fragments on flush. Update either continuously or in
	 * OnFlushPending. Note: this writes EVERY field, including bNavTargetValid. Prefer the narrow
	 * SetPendingNavTarget / ClearPendingNavTarget helpers below when only the nav target is changing.
	 */
	UFUNCTION(BlueprintCallable, Category = "NPC|Mass")
	void SetPendingSnapshot(const FNPCMassSnapshot& InSnapshot) { PendingSnapshot = InSnapshot; }

	/**
	 * Set the navigation target the entity should move toward when dehydrated. Writes only the
	 * NavTarget location and marks intent-to-move via bNavTargetValid=true; other PendingSnapshot
	 * fields (health, state id) are untouched. C++ AND's this intent with the dehydrate context at
	 * flush time, so while hydrated the actor still drives the body regardless.
	 */
	UFUNCTION(BlueprintCallable, Category = "NPC|Mass")
	void SetPendingNavTarget(const FVector& InTarget)
	{
		PendingSnapshot.NavTarget = InTarget;
		PendingSnapshot.bNavTargetValid = true;
	}

	/**
	 * Clear the navigation target: the entity stands at its last position when dehydrated instead of
	 * path-following. Writes only the NavTarget / bNavTargetValid fields; health and state id are
	 * untouched.
	 */
	UFUNCTION(BlueprintCallable, Category = "NPC|Mass")
	void ClearPendingNavTarget()
	{
		PendingSnapshot.NavTarget = FVector::ZeroVector;
		PendingSnapshot.bNavTargetValid = false;
	}

	UFUNCTION(BlueprintPure, Category = "NPC|Mass")
	const FNPCMassSnapshot& GetPendingSnapshot() const { return PendingSnapshot; }

	UFUNCTION(BlueprintPure, Category = "NPC|Mass")
	bool HasValidEntity() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/**
	 * Attempts hydration. Returns true on success. On failure (entity↔actor mapping not yet set
	 * in UMassActorSubsystem), schedules a retry next tick up to MaxHydrationRetries.
	 *
	 * Needed because UMassRepresentationActorManagement::OnPostActorSpawn registers the mapping
	 * AFTER World->SpawnActor returns, i.e. after the actor's BeginPlay already fired.
	 */
	void AttemptHydration(int32 RetriesRemaining);

	/** Reads fragments into a snapshot, adds FNPCHydratedTag, broadcasts OnHydrated. Assumes EntityHandle is valid. */
	void HydrateFromEntity();

	/**
	 * Broadcasts OnFlushPending then writes PendingSnapshot to fragments.
	 * bDehydrating gates the Mass StateTree's path-follow: true = enable (actor is leaving, hand over
	 * to Mass), false = disable (actor still alive, save-time flush only). C++ owns this bit so
	 * BP doesn't have to distinguish flush vs. dehydrate contexts.
	 */
	void WritePendingToFragments(bool bDehydrating);

	/** Returns the owner's UHealthAttributeSet if it has one (via IAbilitySystemInterface), nullptr otherwise. */
	UHealthAttributeSet* FindOwnerHealthSet() const;

	static constexpr int32 MaxHydrationRetries = 5;

	FMassEntityHandle EntityHandle;
	FNPCMassSnapshot PendingSnapshot;
	FDelegateHandle PreFlushDelegateHandle;
};
