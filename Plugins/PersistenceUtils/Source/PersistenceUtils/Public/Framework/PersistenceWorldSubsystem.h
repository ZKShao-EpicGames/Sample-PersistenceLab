// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "Data/PersistenceDataTypes.h"
#include "MassPersistence/MassPersistenceUtils.h"
#include "PersistenceWorldSubsystem.generated.h"

class ULevelStreaming;

class UPersistenceGameSubsystem;
class UPersistenceSaveGame;
class AInstancedActorsManager;

/**
 *
 */
UCLASS()
class PERSISTENCEUTILS_API UPersistenceWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// Broadcast just before InstancedActorManagers are serialized into the SaveGame.
	// Subscribers like InstancedActorsComponents can update Mass fragments just in time,
	// before serializing them.
	DECLARE_MULTICAST_DELEGATE(FPreFlushInstancedActorsData);
	FPreFlushInstancedActorsData OnPreFlushInstancedActorsData;

	// Broadcast just before Mass entity fragments are serialized into the SaveGame, inside the phase-quiescent
	// slot (FrameEnd phase boundary, or synchronously on world teardown). Subscribers (typically
	// actor-authoritative gameplay code) should push their live state into the relevant Mass fragments
	// synchronously during the broadcast.
	DECLARE_MULTICAST_DELEGATE(FPreFlushMassEntityData);
	FPreFlushMassEntityData OnPreFlushMassEntityData;

	// Broadcast once a requested save flush has completed and its snapshots/state have been stored on the
	// SaveGame. Used internally to fan out completion to multiple callers waiting on the same in-flight flush.
	DECLARE_MULTICAST_DELEGATE(FOnSaveFlushComplete);

	// Flushes all live state into the SaveGame in preparation for a disk write or world teardown.
	// Synchronous work that doesn't touch Mass fragments runs inline: map travel data (skipped on teardown,
	// since the game code triggering the map change owns the next-session start spot) and level-streaming
	// persistence data. Work that DOES touch Mass fragments (the OnPreFlushMassEntityData broadcast, IAM
	// serialization, and the Mass entity snapshot itself) is deferred to the next FrameEnd phase boundary
	// via UMassPersistenceUtils::SnapshotEntities, or runs synchronously on world teardown.
	// OnComplete fires once that phase-quiescent work completes, possibly before this function returns when
	// the flush is synchronous (teardown, or no Mass simulation present).
	// Concurrent callers piggy-back on the in-flight broadcast.
	void RequestSaveFlush(FOnSaveFlushComplete::FDelegate OnComplete);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	static bool IsTransitionWorld(const UWorld* World);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void OnWorldEndPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	void FlushLevelStreamingPersistenceData();
	TArray<ULevel*> GetVisibleLevels() const;
	void FlushMapTravelData();

	// Serialize all AInstancedActorsManagers across all currently visible levels and push to UPersistenceGameSubsystem.
	void FlushInstancedActorManagers();

	// Serialize all AInstancedActorsManagers in Level to bytes and push to UPersistenceGameSubsystem.
	void FlushInstancedActorManagerDataForLevel(UWorld* World, const ULevelStreaming* LevelStreaming, ULevel* Level);

	// Bound to FLevelStreamingDelegates::OnLevelBeginMakingVisible. Fires before a streamed level's
	// actors begin play; records its IAMs so the OnWorldPreActorTick poll can restore each one in the
	// window after it registers with the IA subsystem and before its deferred entity spawn.
	void OnLevelBeginMakingVisible(UWorld* World, const ULevelStreaming* LevelStreaming, ULevel* Level);

	// Bound to FWorldDelegates::OnWorldPreActorTick (runs before FTickableGameObject ticks, i.e. before
	// UInstancedActorsSubsystem's deferred-spawn tick). Restores any pending IAM that has registered
	// (post-BeginPlay) but not yet spawned its entities.
	void OnWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);

	// Called by FWorldDelegates::OnWorldBeginTearDown before actors and subsystems have ended play.
	// This is where IAM/Mass state is flushed; by OnWorldEndPlay the IAMs have already deinitialized.
	void OnWorldBeginTearDown(UWorld* World);

	// Phase 1: deserialize a single AInstancedActorsManager's saved bytes into its IADs. Must run after
	// the manager has registered with the IA subsystem but before its entities spawn. Destroyed deltas
	// are applied by the engine post-spawn; custom viz/health staged into the IADs are applied in
	// UInstancedActorsData::OnSpawnEntities. Touches no Mass fragments.
	void RestoreManager(AInstancedActorsManager* Manager);

	// Bound as the pre-snapshot hook of UMassPersistenceUtils::SnapshotEntities. Runs in the phase-quiescent slot
	// immediately before the Mass snapshot reads fragment bytes. Does the work that itself reads or writes Mass
	// fragments: broadcasts OnPreFlushMassEntityData (actor-authoritative → Mass push), then serializes IAMs
	// (which read Mass fragments via UExampleInstancedActorComponent::SerializeInstancePersistenceData).
	void PerformPreSaveMassTasks();

	// Bound as the completion handler of UMassPersistenceUtils::SnapshotEntities. Stamps the result onto the
	// active SaveGame, clears the in-progress flag, and broadcasts OnSaveFlushBroadcast to fan out
	// completion to all callers of RequestSaveFlush.
	void OnSaveFlushMassPartFinished(TArray<FMassEntityConfigGroupSnapshot> Snapshots);

	// Spawn and populate entities from saved FMassEntityConfigGroupSnapshot data. Called once Mass simulation is confirmed started.
	void RestoreMassEntityData();

	// Bound to UMassSimulationSubsystem::GetOnSimulationStarted() when simulation isn't ready at BeginPlay.
	void OnMassSimulationStarted(UWorld* World);

	// Bound to FWorldDelegates::OnWorldInitializedActors when traveling from a save file with a saved pawn transform.
	// Places a PlayerStartPIE at the saved transform so the game mode spawns the player there.
	void OnWorldActorsInitialized(const UWorld::FActorsInitializedParams& Params);

	FDelegateHandle LevelMakingVisibleHandle;
	FDelegateHandle LevelMakingInvisibleHandle;
	FDelegateHandle PreActorTickHandle;
	FDelegateHandle SimulationStartedHandle;
	FDelegateHandle WorldBeginTearDownHandle;
	FDelegateHandle ActorsInitializedHandle;

	bool bBegunPlay = false;
	bool bPendingMassRestore = false;
	bool bSaveFlushInProgress = false;

	// Pending completion callbacks registered via RequestSaveFlush. Broadcast (and cleared) when the
	// current flush finishes; a fresh multicast accepts any re-entries from inside the broadcast.
	FOnSaveFlushComplete OnSaveFlushBroadcast;

	// IAMs discovered (persistent level at OnWorldBeginPlay, streamed levels at OnLevelBeginMakingVisible)
	// and awaiting restore. OnWorldPreActorTick restores each once it has registered with the IA subsystem
	// (post-BeginPlay) and before its deferred entity spawn, then removes it.
	UPROPERTY()
	TArray<TObjectPtr<AInstancedActorsManager>> ManagersPendingRestore;
};
