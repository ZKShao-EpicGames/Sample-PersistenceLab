// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "References/PersistableActorReference.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/WeakObjectPtr.h"
#include "PersistableActorReferenceManager.generated.h"

class AActor;
class ULevel;

/**
 * Per-world registry that translates a (LevelPath, previous-session actor FName) pair into a live actor pointer.
 *
 * Runtime-spawned actors that opt in via UPersistableReferencedActorComponent self-register here on PostRestoreObject.
 * Map-placed actors don't need the component; the manager falls back to scanning the owning level's Actors array
 * by FName when the registry doesn't have a match.
 *
 * Resolution is the authoritative answer once the owning level's LSP PostRestoreLevel has fired; pending callbacks
 * registered before that point are finalized at that moment with either the resolved actor (success) or nullptr (fail).
 */
UCLASS()
class PERSISTENCEUTILS_API UPersistableActorReferenceManager : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	DECLARE_DELEGATE_TwoParams(FOnRuntimeActorResolved, AActor* /*ResolvedActor*/, bool /*bSuccess*/);
	DECLARE_DELEGATE_OneParam(FOnLevelPostRestored, ULevel* /*Level*/);

	//~ USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Blueprint helper: classify Actor into a serializable FPersistableActorReference. Wraps FPersistableActorReference::SetFromActor. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Persistence", meta=(DefaultToSelf="Actor"))
	static FPersistableActorReference MakePersistableActorReference(AActor* Actor);

	/** Blueprint helper: synchronous resolve. Returns nullptr if not (yet) resolvable. Use the latent ResolvePersistableActorReference action when you want to wait. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Persistence", meta=(WorldContext="WorldContextObject"))
	static AActor* ResolvePersistableActorReferenceImmediate(UObject* WorldContextObject, const FPersistableActorReference& Reference);

	/** Blueprint helper: true if the reference is set to something (Type != None). Says nothing about whether the actor currently resolves. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Persistence", meta=(DisplayName="Is Not Null (Persistable Actor Reference)", CompactNodeTitle="Is Not Null"))
	static bool IsPersistableActorReferenceNotNull(const FPersistableActorReference& Reference);

	/** Called by the component on PostRestoreObject. Records the current live actor under its previous-session identity. */
	void RegisterActor(const FSoftObjectPath& LevelPath, FName LastSessionName, AActor* Actor);

	/** Called by the component on EndPlay to release the registry slot. */
	void UnregisterActor(const FSoftObjectPath& LevelPath, FName LastSessionName);

	/** Registry lookup with fallback to scanning the owning level's Actors by FName. Returns nullptr if not yet resolvable. */
	AActor* GetPersistedRuntimeActor(const FSoftObjectPath& LevelPath, FName ActorName) const;

	/**
	 * Fires Callback synchronously if the actor is already resolvable; otherwise defers until the owning level's
	 * LSP PostRestoreLevel completes. Synchronous fire returns an invalid FDelegateHandle; deferred fire returns
	 * a valid handle that can be passed to UnregisterResolveCallback.
	 * Lifetime, if provided, is captured as a weak ptr; the callback is skipped at fire time if it has gone stale.
	 */
	FDelegateHandle ResolveOrRegister(const FSoftObjectPath& LevelPath, FName ActorName,
		FOnRuntimeActorResolved Callback, UObject* Lifetime = nullptr);

	/**
	 * Level-scoped sibling of ResolveOrRegister: fires Callback with the live ULevel now if that level's LSP
	 * PostRestoreLevel has already fired, otherwise defers until it does (nullptr if the subsystem tears down first).
	 * Lifetime is weak-captured; an invalid handle means it fired synchronously.
	 */
	FDelegateHandle AddOnLevelPostRestoreCallback(const FSoftObjectPath& LevelPath,
		FOnLevelPostRestored Callback, UObject* Lifetime = nullptr);

	void UnregisterResolveCallback(FDelegateHandle Handle);

	/** Module-level LSP PostRestoreLevel hook calls this once for the level being finalized. */
	void OnLevelPostRestore(const ULevel* Level);

private:
	struct FRefKey
	{
		FSoftObjectPath LevelPath;
		FName ActorName;

		bool operator==(const FRefKey& Other) const
		{
			return LevelPath == Other.LevelPath && ActorName == Other.ActorName;
		}

		friend uint32 GetTypeHash(const FRefKey& Key)
		{
			return HashCombine(GetTypeHash(Key.LevelPath), GetTypeHash(Key.ActorName));
		}
	};

	struct FPendingCallback
	{
		FDelegateHandle Handle;
		FOnRuntimeActorResolved Delegate;
		TWeakObjectPtr<UObject> Lifetime;
		bool bHasLifetime = false;
	};

	struct FPendingLevelCallback
	{
		FDelegateHandle Handle;
		FOnLevelPostRestored Delegate;
		TWeakObjectPtr<UObject> Lifetime;
		bool bHasLifetime = false;
	};

	void FirePending(const FRefKey& Key, AActor* ResolvedActor, bool bSuccess);
	void FireLevelPending(const FSoftObjectPath& LevelPath, ULevel* Level);

	// Tracks the level instance for each LevelPath that has had PostRestoreLevel fire. Matching the weak ptr
	// against the current ResolveObject() distinguishes "post-restored and still loaded" from "was post-restored
	// in a previous load cycle but is now streamed out". Re-stream needs to wait for a new PostRestoreLevel.
	TMap<FSoftObjectPath, TWeakObjectPtr<const ULevel>> PostRestoredLevelInstances;

	bool IsLevelCurrentlyPostRestored(const FSoftObjectPath& LevelPath) const;

	TMap<FRefKey, TWeakObjectPtr<AActor>> RegisteredActors;
	TMap<FRefKey, TArray<FPendingCallback>> PendingCallbacks;
	TMap<FDelegateHandle, FRefKey> HandleToKey;

	TMap<FSoftObjectPath, TArray<FPendingLevelCallback>> PendingLevelCallbacks;
	TMap<FDelegateHandle, FSoftObjectPath> LevelHandleToPath;
};
