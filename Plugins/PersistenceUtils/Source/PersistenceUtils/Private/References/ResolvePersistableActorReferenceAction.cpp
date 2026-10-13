// Copyright Epic Games, Inc. All Rights Reserved.

#include "References/ResolvePersistableActorReferenceAction.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "References/PersistableActorReferenceManager.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

UResolvePersistableActorReferenceAction* UResolvePersistableActorReferenceAction::ResolvePersistableActorReference(
	UObject* WorldContextObject, FPersistableActorReference Reference)
{
	UResolvePersistableActorReferenceAction* Action = NewObject<UResolvePersistableActorReferenceAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Reference = Reference;
	return Action;
}

void UResolvePersistableActorReferenceAction::Activate()
{
	// Anchor lifetime to the game instance so the action is released on world teardown even if it never resolved.
	RegisterWithGameInstance(WorldContextObject);

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World)
	{
		DeferBroadcast(nullptr, false);
		return;
	}

	switch (Reference.Type)
	{
	case EPersistableActorReferenceType::None:
		DeferBroadcast(nullptr, false);
		return;

	case EPersistableActorReferenceType::PlayerPawn:
	case EPersistableActorReferenceType::PlayerController:
	{
		AActor* Resolved = Reference.TryResolve(World);
		DeferBroadcast(Resolved, Resolved != nullptr);
		return;
	}

	case EPersistableActorReferenceType::LevelActor:
	{
		UPersistableActorReferenceManager* Mgr = World->GetSubsystem<UPersistableActorReferenceManager>();
		if (!Mgr)
		{
			DeferBroadcast(nullptr, false);
			return;
		}

		TWeakObjectPtr<UResolvePersistableActorReferenceAction> WeakThis(this);
		UPersistableActorReferenceManager::FOnRuntimeActorResolved Callback;
		Callback.BindLambda(
			[WeakThis](AActor* ResolvedActor, bool bSuccess)
			{
				if (UResolvePersistableActorReferenceAction* Strong = WeakThis.Get())
				{
					Strong->DeferBroadcast(ResolvedActor, bSuccess);
				}
			});

		// Manager may fire synchronously (invalid key, already resolvable, or level already post-restored)
		// or deferred (waits for the owning level's PostRestoreLevel). Either way, DeferBroadcast normalizes
		// ordering against the "Then" exec pin.
		PendingHandle = Mgr->ResolveOrRegister(Reference.LevelPath, Reference.ActorName, Callback, this);
		return;
	}
	}
}

void UResolvePersistableActorReferenceAction::DeferBroadcast(AActor* ResolvedActor, bool bSuccess)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	TWeakObjectPtr<UResolvePersistableActorReferenceAction> WeakThis(this);

	auto Fire = [WeakThis, ResolvedActor, bSuccess]()
	{
		if (UResolvePersistableActorReferenceAction* Strong = WeakThis.Get())
		{
			if (bSuccess)
			{
				Strong->OnSuccess.Broadcast(ResolvedActor);
			}
			else
			{
				Strong->OnFailure.Broadcast(nullptr);
			}
			Strong->SetReadyToDestroy();
		}
	};

	if (World)
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda(Fire));
	}
	else
	{
		// No world to schedule on: fire immediately rather than leak the action.
		Fire();
	}
}
