// Copyright Epic Games, Inc. All Rights Reserved.

#include "References/PersistableActorReferenceManager.h"

#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PersistenceUtils.h"

bool UPersistableActorReferenceManager::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Mirrors UPersistenceWorldSubsystem: only game worlds, server only.
	UWorld* World = Cast<UWorld>(Outer);
	if (!World || !World->IsGameWorld() || World->IsNetMode(NM_Client))
	{
		return false;
	}
	return true;
}

void UPersistableActorReferenceManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UPersistableActorReferenceManager::Deinitialize()
{
	// Flush remaining pending callbacks as failures so callers can clean up.
	TArray<FRefKey> Keys;
	PendingCallbacks.GetKeys(Keys);
	for (const FRefKey& Key : Keys)
	{
		FirePending(Key, nullptr, false);
	}

	// Same for level-scoped callbacks: fire with a null level so callers can clean up.
	TArray<FSoftObjectPath> LevelKeys;
	PendingLevelCallbacks.GetKeys(LevelKeys);
	for (const FSoftObjectPath& LevelPath : LevelKeys)
	{
		FireLevelPending(LevelPath, nullptr);
	}

	RegisteredActors.Empty();
	PendingCallbacks.Empty();
	HandleToKey.Empty();
	PendingLevelCallbacks.Empty();
	LevelHandleToPath.Empty();
	PostRestoredLevelInstances.Empty();

	Super::Deinitialize();
}

bool UPersistableActorReferenceManager::IsLevelCurrentlyPostRestored(const FSoftObjectPath& LevelPath) const
{
	const TWeakObjectPtr<const ULevel>* Tracked = PostRestoredLevelInstances.Find(LevelPath);
	if (!Tracked)
	{
		return false;
	}
	// Stale entry if the level was streamed out and GC'd, or re-streamed (new instance at same path).
	const ULevel* TrackedLevel = Tracked->Get();
	if (!TrackedLevel)
	{
		return false;
	}
	return TrackedLevel == LevelPath.ResolveObject();
}

FPersistableActorReference UPersistableActorReferenceManager::MakePersistableActorReference(AActor* Actor)
{
	FPersistableActorReference Ref;
	Ref.SetFromActor(Actor);
	return Ref;
}

AActor* UPersistableActorReferenceManager::ResolvePersistableActorReferenceImmediate(UObject* WorldContextObject, const FPersistableActorReference& Reference)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	return Reference.TryResolve(World);
}

bool UPersistableActorReferenceManager::IsPersistableActorReferenceNotNull(const FPersistableActorReference& Reference)
{
	return Reference.Type != EPersistableActorReferenceType::None;
}

void UPersistableActorReferenceManager::RegisterActor(const FSoftObjectPath& LevelPath, FName LastSessionName, AActor* Actor)
{
	if (!Actor || LevelPath.IsNull() || LastSessionName.IsNone())
	{
		return;
	}

	const FRefKey Key{ LevelPath, LastSessionName };
	RegisteredActors.Add(Key, Actor);

	UE_LOG(LogPersistenceUtils, Log, TEXT("PersistableActorReferenceManager: Registered %s (last-session %s @ %s)"),
		*GetPathNameSafe(Actor), *LastSessionName.ToString(), *LevelPath.ToString());
}

void UPersistableActorReferenceManager::UnregisterActor(const FSoftObjectPath& LevelPath, FName LastSessionName)
{
	if (LevelPath.IsNull() || LastSessionName.IsNone())
	{
		return;
	}
	RegisteredActors.Remove(FRefKey{ LevelPath, LastSessionName });
}

AActor* UPersistableActorReferenceManager::GetPersistedRuntimeActor(const FSoftObjectPath& LevelPath, FName ActorName) const
{
	if (LevelPath.IsNull() || ActorName.IsNone())
	{
		return nullptr;
	}

	// Registry first: it handles runtime actors whose current FName differs from the saved one.
	if (const TWeakObjectPtr<AActor>* Found = RegisteredActors.Find(FRefKey{ LevelPath, ActorName }))
	{
		if (AActor* Actor = Found->Get())
		{
			return Actor;
		}
	}

	// Fallback: scan the level by FName. Covers map-placed actors (stable name from asset).
	ULevel* Level = Cast<ULevel>(LevelPath.ResolveObject());
	if (!Level)
	{
		return nullptr;
	}

	for (AActor* Actor : Level->Actors)
	{
		if (Actor && Actor->GetFName() == ActorName)
		{
			return Actor;
		}
	}
	return nullptr;
}

FDelegateHandle UPersistableActorReferenceManager::ResolveOrRegister(const FSoftObjectPath& LevelPath, FName ActorName,
	FOnRuntimeActorResolved Callback, UObject* Lifetime)
{
	// Reject malformed keys outright; there's no point deferring something that can never resolve.
	if (LevelPath.IsNull() || ActorName.IsNone())
	{
		Callback.ExecuteIfBound(nullptr, false);
		return FDelegateHandle();
	}

	if (AActor* Resolved = GetPersistedRuntimeActor(LevelPath, ActorName))
	{
		Callback.ExecuteIfBound(Resolved, true);
		return FDelegateHandle();
	}

	// The level is loaded and its LSP PostRestoreLevel has already fired, so the answer is definitively "not here."
	// (Cross-level case where the level isn't loaded falls through to deferral.)
	if (IsLevelCurrentlyPostRestored(LevelPath))
	{
		Callback.ExecuteIfBound(nullptr, false);
		return FDelegateHandle();
	}

	FPendingCallback Pending;
	Pending.Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	Pending.Delegate = MoveTemp(Callback);
	Pending.Lifetime = Lifetime;
	Pending.bHasLifetime = Lifetime != nullptr;

	const FRefKey Key{ LevelPath, ActorName };
	PendingCallbacks.FindOrAdd(Key).Add(Pending);
	HandleToKey.Add(Pending.Handle, Key);
	return Pending.Handle;
}

FDelegateHandle UPersistableActorReferenceManager::AddOnLevelPostRestoreCallback(const FSoftObjectPath& LevelPath,
	FOnLevelPostRestored Callback, UObject* Lifetime)
{
	if (LevelPath.IsNull())
	{
		Callback.ExecuteIfBound(nullptr);
		return FDelegateHandle();
	}

	// Level already finalized and still loaded: fire now with the live level.
	if (IsLevelCurrentlyPostRestored(LevelPath))
	{
		Callback.ExecuteIfBound(Cast<ULevel>(LevelPath.ResolveObject()));
		return FDelegateHandle();
	}

	FPendingLevelCallback Pending;
	Pending.Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	Pending.Delegate = MoveTemp(Callback);
	Pending.Lifetime = Lifetime;
	Pending.bHasLifetime = Lifetime != nullptr;

	PendingLevelCallbacks.FindOrAdd(LevelPath).Add(Pending);
	LevelHandleToPath.Add(Pending.Handle, LevelPath);
	return Pending.Handle;
}

void UPersistableActorReferenceManager::UnregisterResolveCallback(FDelegateHandle Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	// Level-scoped callbacks live in a separate registry.
	if (const FSoftObjectPath* LevelPathPtr = LevelHandleToPath.Find(Handle))
	{
		const FSoftObjectPath LevelPath = *LevelPathPtr;
		LevelHandleToPath.Remove(Handle);
		if (TArray<FPendingLevelCallback>* List = PendingLevelCallbacks.Find(LevelPath))
		{
			List->RemoveAll([Handle](const FPendingLevelCallback& C) { return C.Handle == Handle; });
			if (List->IsEmpty())
			{
				PendingLevelCallbacks.Remove(LevelPath);
			}
		}
		return;
	}

	const FRefKey* KeyPtr = HandleToKey.Find(Handle);
	if (!KeyPtr)
	{
		return;
	}
	const FRefKey Key = *KeyPtr;
	HandleToKey.Remove(Handle);

	if (TArray<FPendingCallback>* List = PendingCallbacks.Find(Key))
	{
		List->RemoveAll([Handle](const FPendingCallback& C) { return C.Handle == Handle; });
		if (List->IsEmpty())
		{
			PendingCallbacks.Remove(Key);
		}
	}
}

void UPersistableActorReferenceManager::OnLevelPostRestore(const ULevel* Level)
{
	if (!Level)
	{
		return;
	}

	const FSoftObjectPath LevelPath(Level);
	PostRestoredLevelInstances.Add(LevelPath, Level);

	// Snapshot keys-to-fire, since FirePending mutates PendingCallbacks.
	TArray<FRefKey> KeysToFire;
	for (const TPair<FRefKey, TArray<FPendingCallback>>& Pair : PendingCallbacks)
	{
		if (Pair.Key.LevelPath == LevelPath)
		{
			KeysToFire.Add(Pair.Key);
		}
	}

	for (const FRefKey& Key : KeysToFire)
	{
		AActor* Resolved = GetPersistedRuntimeActor(Key.LevelPath, Key.ActorName);
		FirePending(Key, Resolved, Resolved != nullptr);
	}

	FireLevelPending(LevelPath, const_cast<ULevel*>(Level));
}

void UPersistableActorReferenceManager::FireLevelPending(const FSoftObjectPath& LevelPath, ULevel* Level)
{
	TArray<FPendingLevelCallback> Callbacks;
	if (!PendingLevelCallbacks.RemoveAndCopyValue(LevelPath, Callbacks))
	{
		return;
	}

	for (FPendingLevelCallback& Cb : Callbacks)
	{
		LevelHandleToPath.Remove(Cb.Handle);
		if (Cb.bHasLifetime && !Cb.Lifetime.IsValid())
		{
			continue;
		}
		Cb.Delegate.ExecuteIfBound(Level);
	}
}

void UPersistableActorReferenceManager::FirePending(const FRefKey& Key, AActor* ResolvedActor, bool bSuccess)
{
	TArray<FPendingCallback> Callbacks;
	if (!PendingCallbacks.RemoveAndCopyValue(Key, Callbacks))
	{
		return;
	}

	for (FPendingCallback& Cb : Callbacks)
	{
		HandleToKey.Remove(Cb.Handle);
		if (Cb.bHasLifetime && !Cb.Lifetime.IsValid())
		{
			continue;
		}
		Cb.Delegate.ExecuteIfBound(ResolvedActor, bSuccess);
	}
}
