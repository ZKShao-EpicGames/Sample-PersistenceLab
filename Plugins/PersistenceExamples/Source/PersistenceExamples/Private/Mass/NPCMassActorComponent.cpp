// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mass/NPCMassActorComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GAS/HealthAttributeSet.h"
#include "Mass/HealthFragment.h"
#include "PersistenceExamples.h"
#include "MassActorSubsystem.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassSignalSubsystem.h"
#include "MassStateTreeTypes.h"
#include "Framework/PersistenceWorldSubsystem.h"
#include "TimerManager.h"

UNPCMassActorComponent::UNPCMassActorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UNPCMassActorComponent::HasValidEntity() const
{
	if (!EntityHandle.IsSet())
	{
		return false;
	}

	const UMassEntitySubsystem* EntitySubsystem = GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	return EntitySubsystem && EntitySubsystem->GetEntityManager().IsEntityValid(EntityHandle);
}

void UNPCMassActorComponent::BeginPlay()
{
	Super::BeginPlay();

	AttemptHydration(MaxHydrationRetries);

	if (UPersistenceWorldSubsystem* WorldSubsystem = GetWorld()->GetSubsystem<UPersistenceWorldSubsystem>())
	{
		PreFlushDelegateHandle = WorldSubsystem->OnPreFlushMassEntityData.AddWeakLambda(this, [this]()
		{
			// Save-time path: write pending state but keep FNPCHydratedTag; the actor isn't going away.
			// bDehydrating=false keeps the Mass StateTree's path-follow gate closed while the actor is alive.
			WritePendingToFragments(/*bDehydrating=*/ false);
		});
	}
}

void UNPCMassActorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UPersistenceWorldSubsystem* WorldSubsystem = GetWorld()->GetSubsystem<UPersistenceWorldSubsystem>())
	{
		WorldSubsystem->OnPreFlushMassEntityData.Remove(PreFlushDelegateHandle);
	}

	// Dehydration path: actor is leaving the world. bDehydrating=true enables the Mass StateTree's
	// path-follow gate so the entity continues toward PendingSnapshot.NavTarget after the actor is gone.
	WritePendingToFragments(/*bDehydrating=*/ true);

	if (HasValidEntity())
	{
		FMassEntityManager& EntityManager = GetWorld()->GetSubsystem<UMassEntitySubsystem>()->GetMutableEntityManager();
		EntityManager.RemoveTagFromEntity(EntityHandle, FNPCHydratedTag::StaticStruct());

		// Force a Mass StateTree tick so the eval sees the new bValid=true + Target this frame.
		// Without this, the StateTree only ticks on subscribed signals (MassStateTreeProcessors.cpp:279-289),
		// so the Stand→PathFollow transition wouldn't fire and the task wouldn't re-RequestPath with the
		// fresh target until some other signal happens to arrive.
		if (UMassSignalSubsystem* SignalSubsystem = GetWorld()->GetSubsystem<UMassSignalSubsystem>())
		{
			SignalSubsystem->SignalEntity(UE::Mass::Signals::NewStateTreeTaskRequired, EntityHandle);
		}
	}

	//UE_LOG(LogPersistenceExamples, Log, TEXT("[%s] NPCMassActorComponent EndPlay (Reason=%d), flushed to entity %s"), *GetNameSafe(GetOwner()), (int32)EndPlayReason, *EntityHandle.DebugGetDescription());

	Super::EndPlay(EndPlayReason);
}

void UNPCMassActorComponent::AttemptHydration(int32 RetriesRemaining)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	UMassActorSubsystem* ActorSubsystem = GetWorld()->GetSubsystem<UMassActorSubsystem>();
	UMassEntitySubsystem* EntitySubsystem = GetWorld()->GetSubsystem<UMassEntitySubsystem>();
	if (!ActorSubsystem || !EntitySubsystem)
	{
		UE_LOG(LogPersistenceExamples, Warning, TEXT("[%s] NPCMassActorComponent: Mass subsystems unavailable; cannot hydrate."), *GetNameSafe(Owner));
		return;
	}

	EntityHandle = ActorSubsystem->GetEntityHandleFromActor(Owner);
	if (EntitySubsystem->GetEntityManager().IsEntityValid(EntityHandle))
	{
		HydrateFromEntity();
		return;
	}

	if (RetriesRemaining > 0)
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateUObject(this, &UNPCMassActorComponent::AttemptHydration, RetriesRemaining - 1));
		return;
	}

	UE_LOG(LogPersistenceExamples, Warning, TEXT("[%s] NPCMassActorComponent: no entity mapped after %d retries; giving up."), *GetNameSafe(Owner), MaxHydrationRetries);
}

void UNPCMassActorComponent::HydrateFromEntity()
{
	UMassEntitySubsystem* EntitySubsystem = GetWorld()->GetSubsystem<UMassEntitySubsystem>();
	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();

	FNPCMassSnapshot Snap;
	if (const FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
	{
		Snap.Health = HealthFrag->Current;
		Snap.MaxHealth = HealthFrag->Max;
	}
	if (const FNPCNavTargetFragment* NavFrag = EntityManager.GetFragmentDataPtr<FNPCNavTargetFragment>(EntityHandle))
	{
		Snap.NavTarget = NavFrag->Target;
		Snap.bNavTargetValid = NavFrag->bValid;
	}
	if (const FNPCStateFragment* StateFrag = EntityManager.GetFragmentDataPtr<FNPCStateFragment>(EntityHandle))
	{
		Snap.ActiveStateId = StateFrag->ActiveStateId;
	}

	PendingSnapshot = Snap;

	EntityManager.AddTagToEntity(EntityHandle, FNPCHydratedTag::StaticStruct());

	// Actor is in charge now: close the Mass StateTree's path-follow gate so the dehydrated nav
	// stack doesn't fight CharacterMovementComponent.
	if (FNPCNavTargetFragment* NavFrag = EntityManager.GetFragmentDataPtr<FNPCNavTargetFragment>(EntityHandle))
	{
		NavFrag->bValid = false;
	}

	// Force a Mass StateTree tick so the eval observes bValid=false and PathFollow exits to Stand
	// during the hydrated window.
	if (UMassSignalSubsystem* SignalSubsystem = GetWorld()->GetSubsystem<UMassSignalSubsystem>())
	{
		SignalSubsystem->SignalEntity(UE::Mass::Signals::NewStateTreeTaskRequired, EntityHandle);
	}

	// Seed the owner's HealthSet from the snapshot before BP listeners run, so OnHydrated handlers
	// observe a fully-hydrated actor. MaxHealth first so SetHealth's PreAttributeChange clamp uses
	// the correct ceiling.
	if (UHealthAttributeSet* HealthSet = FindOwnerHealthSet())
	{
		HealthSet->SetMaxHealth(Snap.MaxHealth);
		HealthSet->SetHealth(Snap.Health);
	}

	//UE_LOG(LogPersistenceExamples, Log, TEXT("[%s] NPCMassActorComponent hydrated from entity %s (Health=%.1f, NavValid=%d, StateId=%s)"), *GetNameSafe(GetOwner()), *EntityHandle.DebugGetDescription(), Snap.Health, Snap.bNavTargetValid ? 1 : 0, *Snap.ActiveStateId.ToString());

	// Defer the broadcast to next tick so the owning actor's BP BeginPlay has time to bind to OnHydrated.
	// Quick workaround for timing issue in this example. Didn't want to pollute pawn class.
	const FNPCMassSnapshot SnapCopy = Snap;
	GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, SnapCopy]() 
		{
			OnHydrated.Broadcast(SnapCopy);
		}
	));
}

void UNPCMassActorComponent::WritePendingToFragments(bool bDehydrating)
{
	if (!HasValidEntity())
	{
		return;
	}

	FMassEntityManager& EntityManager = GetWorld()->GetSubsystem<UMassEntitySubsystem>()->GetMutableEntityManager();

	// Pull current GAS health into PendingSnapshot as the default. The OnFlushPending broadcast
	// fires next, giving BP listeners a chance to overwrite if they need to.
	if (UHealthAttributeSet* HealthSet = FindOwnerHealthSet())
	{
		PendingSnapshot.Health = HealthSet->GetHealth();
		PendingSnapshot.MaxHealth = HealthSet->GetMaxHealth();
	}

	OnFlushPending.Broadcast();

	// Gate the Mass StateTree's path-follow against the hydrate context: AND BP's intent (set via
	// SetPendingNavTarget / ClearPendingNavTarget) with bDehydrating. Hydrated always means false
	// (actor drives); on dehydrate, BP decides whether the entity continues moving.
	PendingSnapshot.bNavTargetValid = bDehydrating && PendingSnapshot.bNavTargetValid;

	if (FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
	{
		HealthFrag->Current = PendingSnapshot.Health;
		HealthFrag->Max = PendingSnapshot.MaxHealth;
	}
	if (FNPCNavTargetFragment* NavFrag = EntityManager.GetFragmentDataPtr<FNPCNavTargetFragment>(EntityHandle))
	{
		NavFrag->Target = PendingSnapshot.NavTarget;
		NavFrag->bValid = PendingSnapshot.bNavTargetValid;
	}
	if (FNPCStateFragment* StateFrag = EntityManager.GetFragmentDataPtr<FNPCStateFragment>(EntityHandle))
	{
		StateFrag->ActiveStateId = PendingSnapshot.ActiveStateId;
	}
}

UHealthAttributeSet* UNPCMassActorComponent::FindOwnerHealthSet() const
{
	if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(GetOwner()))
	{
		if (const UAbilitySystemComponent* ASC = ASI->GetAbilitySystemComponent())
		{
			return const_cast<UHealthAttributeSet*>(ASC->GetSet<UHealthAttributeSet>());
		}
	}
	return nullptr;
}
