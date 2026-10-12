// Copyright Epic Games, Inc. All Rights Reserved.

#include "Framework/PersistenceWorldSubsystem.h"
#include "Framework/PersistenceGameSubsystem.h"
#include "MassPersistence/MassPersistenceUtils.h"
#include "PersistenceUtils.h"
#include "PersistenceUtilsSettings.h"
#include "Engine/PlayerStartPIE.h"
#include "EngineUtils.h"
#include "GameMapsSettings.h"
#include "GameFramework/PlayerController.h"
#include "LevelStreamingPersistenceManager.h"
#include "Streaming/LevelStreamingDelegates.h"
#include "WorldPartition/WorldPartition.h"
#include "InstancedActorsManager.h"
#include "InstancedActorsData.h"
#include "InstancedActorsSubsystem.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/CustomVersion.h"
#include "UObject/ObjectVersion.h"
#include "MassSimulationSubsystem.h"

bool UPersistenceWorldSubsystem::IsTransitionWorld(const UWorld* World)
{
	if (!World)
	{
		return false;
	}
	const FSoftObjectPath& TransitionMap = GetDefault<UGameMapsSettings>()->TransitionMap;
	return !TransitionMap.IsNull() && World->GetOutermost()->GetFName() == TransitionMap.GetAssetPath().GetPackageName();
}

bool UPersistenceWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Only game worlds are saveable. Clients are excluded: we assume all saving happens on the authority.
	// TODO: Investigate — GetNetMode() may not be reliable at ShouldCreateSubsystem time (net driver not yet assigned),
	// meaning the NM_Client check may not correctly exclude client worlds in PIE with a dedicated server.
	UWorld* World = Cast<UWorld>(Outer);
	if (!World || !World->IsGameWorld() || (World->IsNetMode(NM_Client)))
	{
		return false;
	}

	return true;
}

void UPersistenceWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	USubsystem* Dependency = Collection.InitializeDependency<ULevelStreamingPersistenceManager>();
	Super::Initialize(Collection);

	if (!Dependency)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("UPersistenceWorldSubsystem::Initialize: Missing an expected world subsystem"));
		return;
	}

	if (!GetWorld()->GetGameInstance())
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("UPersistenceWorldSubsystem::Initialize: Skipping init on world %s due to GameInstance missing"), *GetWorld()->GetName());
		return;
	}

	LevelMakingVisibleHandle = FLevelStreamingDelegates::OnLevelBeginMakingVisible.AddUObject(this, &UPersistenceWorldSubsystem::OnLevelBeginMakingVisible);
	LevelMakingInvisibleHandle = FLevelStreamingDelegates::OnLevelBeginMakingInvisible.AddUObject(this, &UPersistenceWorldSubsystem::FlushInstancedActorManagerDataForLevel);
	PreActorTickHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &UPersistenceWorldSubsystem::OnWorldPreActorTick);

	WorldBeginTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &UPersistenceWorldSubsystem::OnWorldBeginTearDown);

	const UPersistenceUtilsSettings* Settings = GetDefault<UPersistenceUtilsSettings>();

	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	const UPersistenceSaveGame* SaveGame = PersistenceSubsystem ? PersistenceSubsystem->GetSaveGame() : nullptr;

	// If we're arriving from a save-file travel and have a saved pawn transform, hook OnWorldInitializedActors
	// to place a PlayerStartPIE at the saved location before actors begin play.
	if (PersistenceSubsystem && PersistenceSubsystem->IsTravelingFromSaveFile()
		&& Settings && Settings->bRestorePawnTransform
		&& SaveGame && SaveGame->TravelData.bHasPawnTransform
		&& !UPersistenceWorldSubsystem::IsTransitionWorld(GetWorld()))
	{
		ActorsInitializedHandle = FWorldDelegates::OnWorldInitializedActors.AddUObject(this, &UPersistenceWorldSubsystem::OnWorldActorsInitialized);
	}

	// Restore any previously saved streaming level data for this map so that property state is applied
	// as streaming levels become visible, regardless of whether we are traveling from a save file.
	if (SaveGame)
	{
		const FName MapKey = GetWorld()->GetOutermost()->GetFName();
		if (const FWorldPersistenceEntry* Entry = SaveGame->SavedStatePerMap.Find(MapKey))
		{
			ULevelStreamingPersistenceManager* PersistenceManager = GetWorld()->GetSubsystem<ULevelStreamingPersistenceManager>();
			if (PersistenceManager && PersistenceManager->InitializeFrom(Entry->StreamingLevelData))
			{
				UE_LOG(LogPersistenceUtils, Log, TEXT("Initialize: Restored streaming level data for '%s' (%d bytes)."), *MapKey.ToString(), Entry->StreamingLevelData.Num());
			}
			else
			{
				UE_LOG(LogPersistenceUtils, Warning, TEXT("Initialize: Failed to restore streaming level data for '%s'."), *MapKey.ToString());
			}
		}
	}

	UE_LOG(LogPersistenceUtils, Log, TEXT("Hello: %s. NetMode: %d"), *GetPathName(), GetWorld()->GetNetMode());
}

void UPersistenceWorldSubsystem::PostInitialize()
{
	Super::PostInitialize();
}

void UPersistenceWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bBegunPlay = true;

	if (UPersistenceWorldSubsystem::IsTransitionWorld(&InWorld))
	{
		return;
	}

	// Attempt to restore Mass entities. Defer if simulation hasn't started yet.
	UMassSimulationSubsystem* SimSubsystem = GetWorld()->GetSubsystem<UMassSimulationSubsystem>();
	if (SimSubsystem && SimSubsystem->IsSimulationStarted())
	{
		RestoreMassEntityData();
	}
	else if (SimSubsystem)
	{
		bPendingMassRestore = true;
		SimulationStartedHandle = SimSubsystem->GetOnSimulationStarted().AddUObject(this, &UPersistenceWorldSubsystem::OnMassSimulationStarted);
	}

	// Record persistent-level IAMs for restore. They begin play during this UWorld::BeginPlay (after this
	// subsystem callback); the OnWorldPreActorTick poll restores each once it has registered with the IA
	// subsystem and before its deferred entity spawn.
	if (InWorld.PersistentLevel)
	{
		for (AActor* Actor : InWorld.PersistentLevel->Actors)
		{
			if (AInstancedActorsManager* Manager = Cast<AInstancedActorsManager>(Actor))
			{
				ManagersPendingRestore.AddUnique(Manager);
			}
		}
	}

	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	if (PersistenceSubsystem && PersistenceSubsystem->IsTravelingFromSaveFile())
	{
		// Restore the player's camera orientation if it was saved.
		const UPersistenceUtilsSettings* Settings = GetDefault<UPersistenceUtilsSettings>();
		const UPersistenceSaveGame* SaveGame = PersistenceSubsystem->GetSaveGame();
		if (Settings->bRestoreControlRotation && SaveGame && SaveGame->TravelData.bHasControlRotation)
		{
			if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
			{
				PC->SetControlRotation(SaveGame->TravelData.ControlRotation);
				const FRotator& R = SaveGame->TravelData.ControlRotation;
				UE_LOG(LogPersistenceUtils, Log, TEXT("OnWorldBeginPlay: Restored control rotation (P=%.1f, Y=%.1f, R=%.1f) on '%s'."), R.Pitch, R.Yaw, R.Roll, *GetWorld()->GetMapName());
			}
			else
			{
				UE_LOG(LogPersistenceUtils, Warning, TEXT("OnWorldBeginPlay: No PlayerController found to restore control rotation on '%s'."), *GetWorld()->GetMapName());
			}
		}

		// Report that we're done traveling from save file
		PersistenceSubsystem->ReportTravelFromSaveFileComplete(GetWorld());
	}
}

void UPersistenceWorldSubsystem::OnWorldEndPlay(UWorld& InWorld)
{
	Super::OnWorldEndPlay(InWorld);
}

void UPersistenceWorldSubsystem::OnWorldBeginTearDown(UWorld* World)
{
	// Fires for every world; only act on the one this subsystem belongs to.
	if (World != GetWorld())
	{
		return;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	const UPersistenceUtilsSettings* Settings = GetDefault<UPersistenceUtilsSettings>();
	UPersistenceGameSubsystem* PersistenceSubsystem = GameInstance ? GameInstance->GetSubsystem<UPersistenceGameSubsystem>() : nullptr;
	// When traveling from a save file, the loaded save's game time replaces this world's.
	if (!PersistenceSubsystem || PersistenceSubsystem->IsTravelingFromSaveFile())
	{
		return;
	}

	if (Settings && Settings->bAutoSaveWhenLeavingMap)
	{
		// Synchronous flushes + (synchronously-completing) Mass flush. RequestSaveFlush detects World->bIsTearingDown
		// internally and skips map travel data (game code triggering map travel owns the next-session start spot)
		// and runs the Mass snapshot synchronously inside this broadcast. No completion callback needed.
		RequestSaveFlush(FOnSaveFlushComplete::FDelegate{});
	}

	// After the flush, so data flushed above is stamped with this world's final game time.
	PersistenceSubsystem->AccumulateWorldGameTime(*World);
}

void UPersistenceWorldSubsystem::Deinitialize()
{
	FLevelStreamingDelegates::OnLevelBeginMakingVisible.Remove(LevelMakingVisibleHandle);
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.Remove(LevelMakingInvisibleHandle);
	FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(WorldBeginTearDownHandle);
	FWorldDelegates::OnWorldInitializedActors.Remove(ActorsInitializedHandle);

	if (UMassSimulationSubsystem* SimSubsystem = GetWorld()->GetSubsystem<UMassSimulationSubsystem>())
	{
		SimSubsystem->GetOnSimulationStarted().Remove(SimulationStartedHandle);
	}

	Super::Deinitialize();
}

void UPersistenceWorldSubsystem::OnWorldActorsInitialized(const UWorld::FActorsInitializedParams& Params)
{
	// Only act on our own world — FWorldDelegates is global and fires for every world that initializes.
	if (Params.World != GetWorld())
	{
		return;
	}

	FWorldDelegates::OnWorldInitializedActors.Remove(ActorsInitializedHandle);
	ActorsInitializedHandle.Reset();

	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	const UPersistenceSaveGame* SaveGame = PersistenceSubsystem ? PersistenceSubsystem->GetSaveGame() : nullptr;
	if (!SaveGame)
	{
		return;
	}

	// Destroy existing PlayerStartPIE if any, because we will spawn a new one at the saved transform.
	for (TActorIterator<APlayerStartPIE> It(Params.World); It; ++It)
	{
		It->Destroy();
	}

	// Spawn a PlayerStartPIE at the saved transform so the game mode spawns the player there.
	// This relies on the convention that PlayerStartPIE actors are preferred over PlayerStart when spawning players in PIE,
	// which is true in current engine code but not guaranteed by any hard rule. If you override your GameMode's player spawning
	// to consider PawnTransform, that would be most reliable.
	const FName LoadedMap = Params.World->GetOutermost()->GetFName();
	if (Params.World->SpawnActor<APlayerStartPIE>(APlayerStartPIE::StaticClass(), SaveGame->TravelData.PawnTransform))
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("OnWorldActorsInitialized: Placed PlayerStartPIE at saved transform on '%s'."), *LoadedMap.ToString());
	}
	else
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("OnWorldActorsInitialized: Failed to find or spawn PlayerStartPIE on '%s'."), *LoadedMap.ToString());
	}
}

void UPersistenceWorldSubsystem::FlushLevelStreamingPersistenceData()
{
	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	if (!PersistenceSubsystem)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("UPersistenceWorldSubsystem: Could not resolve UPersistenceGameSubsystem. Persistent data will not be updated."));
		return;
	}

	TArray<uint8> WorldSerializedSaveData;
	ULevelStreamingPersistenceManager* PersistenceManager = GetWorld()->GetSubsystem<ULevelStreamingPersistenceManager>();
	if (PersistenceManager && PersistenceManager->SerializeTo(WorldSerializedSaveData, true))
	{
		const FName MapKey = GetWorld()->GetOutermost()->GetFName();
		UE_LOG(LogPersistenceUtils, Log, TEXT("UpdateAndFlushStreamingLevelData: Serialized streaming level data for '%s' — %d bytes."), *MapKey.ToString(), WorldSerializedSaveData.Num());
		PersistenceSubsystem->SetStreamingLevelDataForMap(MapKey, MoveTemp(WorldSerializedSaveData));
	}
	else
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("UpdateAndFlushStreamingLevelData: Failed to serialize streaming level data for '%s'. Existing saved data preserved."), *GetWorld()->GetOutermost()->GetName());
	}
}

TArray<ULevel*> UPersistenceWorldSubsystem::GetVisibleLevels() const
{
	TArray<ULevel*> VisibleLevels;
	if (UWorld* World = GetWorld())
	{
		VisibleLevels.Add(World->PersistentLevel);

		for (ULevelStreaming* StreamingLevel : GetWorld()->GetStreamingLevels())
		{
			if (StreamingLevel)
			{
				if (ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel())
				{
					if (LoadedLevel->bIsVisible)
					{
						VisibleLevels.Add(LoadedLevel);
					}
				}
			}
		}
	}
	return VisibleLevels;
}

void UPersistenceWorldSubsystem::FlushMapTravelData()
{
	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	if (!PersistenceSubsystem)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("UpdateAndFlushTravelData: Could not resolve UPersistenceGameSubsystem. Travel data will not be updated."));
		return;
	}

	// Capture the current map path, player's position, and camera orientation for use when travelling back to this map.
	const UPersistenceUtilsSettings* Settings = GetDefault<UPersistenceUtilsSettings>();
	FPersistenceTravelData TravelData;
	TravelData.Map = FSoftObjectPath(GetWorld()->GetOutermost());
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (Settings->bRestorePawnTransform)
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				TravelData.PawnTransform = Pawn->GetActorTransform();
				TravelData.bHasPawnTransform = true;
			}
		}
		if (Settings->bRestoreControlRotation)
		{
			TravelData.ControlRotation = PC->GetControlRotation();
			TravelData.bHasControlRotation = true;
		}
	}
	const bool bHasPawnTransform = TravelData.bHasPawnTransform;
	const FVector PawnLoc = TravelData.PawnTransform.GetLocation();
	const bool bHasControlRotation = TravelData.bHasControlRotation;
	const FRotator ControlRot = TravelData.ControlRotation;
	PersistenceSubsystem->SetPersistenceTravelData(MoveTemp(TravelData));

	UE_LOG(LogPersistenceUtils, Log, TEXT("UpdateAndFlushTravelData: Travel data set for '%s' — Pawn transform: %s, Control rotation: %s."),
		*GetWorld()->GetMapName(),
		bHasPawnTransform ? *FString::Printf(TEXT("(%.1f, %.1f, %.1f)"), PawnLoc.X, PawnLoc.Y, PawnLoc.Z) : TEXT("not captured"),
		bHasControlRotation ? *FString::Printf(TEXT("(P=%.1f, Y=%.1f, R=%.1f)"), ControlRot.Pitch, ControlRot.Yaw, ControlRot.Roll) : TEXT("not captured"));
}

void UPersistenceWorldSubsystem::FlushInstancedActorManagers()
{
	OnPreFlushInstancedActorsData.Broadcast();

	for (ULevel* Level : GetVisibleLevels())
	{
		ULevelStreaming* LS = GetWorld()->GetLevelStreamingForPackageName(Level->GetPackage()->GetFName());
		FlushInstancedActorManagerDataForLevel(GetWorld(), LS, Level);
	}
}

void UPersistenceWorldSubsystem::FlushInstancedActorManagerDataForLevel(UWorld* World, const ULevelStreaming* LevelStreaming, ULevel* Level)
{
	if (!Level || World != GetWorld())
	{
		return;
	}

	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	if (!PersistenceSubsystem)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("FlushInstancedActorManagerDataForLevel: Could not resolve UPersistenceGameSubsystem."));
		return;
	}
	else if (PersistenceSubsystem->IsTravelingFromSaveFile())
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("FlushInstancedActorManagerDataForLevel: Not flushing save data because we're leaving this map due to loading from a save file."));
		return;
	}

	const FName MapKey   = GetWorld()->GetOutermost()->GetFName();
	const FName LevelKey = LevelStreaming ? LevelStreaming->GetWorldAssetPackageFName() : Level->GetPackage()->GetFName();

	for (AActor* Actor : Level->Actors)
	{
		AInstancedActorsManager* Manager = Cast<AInstancedActorsManager>(Actor);
		if (!IsValid(Manager))
		{
			continue;
		}

		// Pass 1: serialize the manager body into a temp buffer. The set of custom versions touched
		// (e.g. FInstancedActorsCustomVersion, and any UsingCustomVersion calls inside an IAC's
		// SerializeInstancePersistenceData) isn't known until the body has been written, since each
		// UsingCustomVersion call populates the archive's container during Serialize. So we collect the
		// container afterwards and write it as a leading header in pass 2.
		TArray<uint8> BodyData;
		FMemoryWriter BodyWriter(BodyData);
		BodyWriter.ArIsSaveGame = true;
		{
			FStructuredArchiveFromArchive StructuredAr(BodyWriter);
			Manager->Serialize(StructuredAr.GetSlot().EnterRecord());
		}

		if (BodyWriter.IsError() || BodyData.Num() == 0)
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("FlushInstancedActorManagerDataForLevel: Skipping manager '%s' — serialize produced no data or errored."), *Manager->GetName());
			continue;
		}

		// Pass 2: write [UEVersion][CustomVersions][body] into the stored blob. UEVersion is the
		// FPackageFileVersion axis (drives Ar.UEVer()-gated migrations like LWC); the custom-version
		// container carries the GUID-keyed versions that Ar.CustomVer() reads on load. Without this header
		// a bare FMemoryReader resets its container to the live registry, so CustomVer() would always
		// report the current build's version rather than the saved one.
		FPackageFileVersion UEVersion = GPackageFileUEVersion;
		FCustomVersionContainer CustomVersions = BodyWriter.GetCustomVersions();

		TArray<uint8> Data;
		FMemoryWriter MemWriter(Data);
		MemWriter.ArIsSaveGame = true;
		MemWriter << UEVersion;
		CustomVersions.Serialize(MemWriter);
		MemWriter.Serialize(BodyData.GetData(), BodyData.Num());

		if (!MemWriter.IsError() && Data.Num() > 0)
		{
			PersistenceSubsystem->SetInstancedActorManagerDataForLevel(MapKey, LevelKey, Manager->GetFName(), MoveTemp(Data));
		}
		else
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("FlushInstancedActorManagerDataForLevel: Skipping manager '%s' — failed to assemble versioned blob."), *Manager->GetName());
		}
	}
}

void UPersistenceWorldSubsystem::OnLevelBeginMakingVisible(UWorld* World, const ULevelStreaming* LevelStreaming, ULevel* Level)
{
	if (World != GetWorld() || !Level)
	{
		return;
	}

	// Record IAMs before their actors begin play. The OnWorldPreActorTick poll restores each one in the
	// window after it registers with the IA subsystem (post-BeginPlay) and before its deferred spawn.
	for (AActor* Actor : Level->Actors)
	{
		if (AInstancedActorsManager* Manager = Cast<AInstancedActorsManager>(Actor))
		{
			ManagersPendingRestore.AddUnique(Manager);
		}
	}
}

void UPersistenceWorldSubsystem::OnWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || ManagersPendingRestore.IsEmpty())
	{
		return;
	}

	// OnWorldPreActorTick runs before FTickableGameObject::TickObjects, i.e. before
	// UInstancedActorsSubsystem's deferred-spawn tick. Restore each registered-but-unspawned IAM here so
	// its IADs hold the staged data before SpawnEntities -> OnSpawnEntities applies it.
	for (int32 i = ManagersPendingRestore.Num() - 1; i >= 0; --i)
	{
		AInstancedActorsManager* Manager = ManagersPendingRestore[i];
		if (!IsValid(Manager))
		{
			ManagersPendingRestore.RemoveAt(i);
			continue;
		}

		// Not registered yet: SerializeInstancePersistenceData needs the manager's subsystem pointer,
		// which is set during BeginPlay. Try again on a later frame.
		if (Manager->GetInstancedActorSubsystem() == nullptr)
		{
			continue;
		}

		// A registered manager must not have spawned yet at this point. This holds iff entity spawning is
		// deferred (IA.DeferSpawnEntities, on by default) - which this restore path requires, since
		// OnSpawnEntities is the sole apply path for the staged data.
		checkf(!Manager->HasSpawnedEntities(),
			TEXT("%s spawned entities before persistence restore. IA.DeferSpawnEntities must be enabled."),
			*Manager->GetName());

		RestoreManager(Manager);
		ManagersPendingRestore.RemoveAt(i);
	}
}

void UPersistenceWorldSubsystem::RestoreManager(AInstancedActorsManager* Manager)
{
	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	const UPersistenceSaveGame* SaveGame = PersistenceSubsystem ? PersistenceSubsystem->GetSaveGame() : nullptr;
	if (!SaveGame)
	{
		return;
	}

	const FName MapKey = GetWorld()->GetOutermost()->GetFName();
	ULevel* Level = Manager->GetLevel();
	ULevelStreaming* LevelStreaming = GetWorld()->GetLevelStreamingForPackageName(Level->GetPackage()->GetFName());
	const FName LevelKey = LevelStreaming ? LevelStreaming->GetWorldAssetPackageFName() : Level->GetPackage()->GetFName();

	const FWorldPersistenceEntry* WorldEntry = SaveGame->SavedStatePerMap.Find(MapKey);
	if (!WorldEntry)
	{
		return;
	}

	const FStreamingLevelPersistenceEntry* LevelEntry = WorldEntry->SavedStatePerStreamingLevel.Find(LevelKey);
	if (!LevelEntry || LevelEntry->InstancedActorManagerDeltas.IsEmpty())
	{
		return;
	}

	const FInstancedActorManagerState* State = LevelEntry->InstancedActorManagerDeltas.Find(Manager->GetFName());
	if (!State || State->Data.IsEmpty())
	{
		return;
	}

	// Phase 1: deserialize the saved data into the IADs. Destroyed-instance deltas land in the engine's
	// InstanceDeltas (applied automatically by AInstancedActorsManager after the deferred SpawnEntities);
	// custom viz/health are staged into UExampleInstancedActorsData and applied in its OnSpawnEntities.
	// Entities are intentionally not spawned yet - this path touches no Mass fragments.
	FMemoryReader MemReader(State->Data);
	MemReader.ArIsSaveGame = true;

	// Read the [UEVersion][CustomVersions] header written by FlushInstancedActorManagerDataForLevel and
	// apply it to the reader BEFORE deserializing the body. SetCustomVersions clears the reader's
	// reset-to-live-registry flag, so Ar.CustomVer(GUID) inside IAM::Serialize / an IAC's
	// SerializeInstancePersistenceData returns the version that was saved rather than the current build's.
	FPackageFileVersion UEVersion;
	FCustomVersionContainer CustomVersions;
	MemReader << UEVersion;
	CustomVersions.Serialize(MemReader);
	MemReader.SetUEVer(UEVersion);
	MemReader.SetCustomVersions(CustomVersions);

	FStructuredArchiveFromArchive StructuredAr(MemReader);
	Manager->Serialize(StructuredAr.GetSlot().EnterRecord());

	UE_LOG(LogPersistenceUtils, Log, TEXT("RestoreManager: Deserialized %d bytes for manager '%s' in level '%s' on map '%s'."),
		State->Data.Num(), *Manager->GetName(), *LevelKey.ToString(), *MapKey.ToString());
}

void UPersistenceWorldSubsystem::RequestSaveFlush(FOnSaveFlushComplete::FDelegate OnComplete)
{
	// Register the completion callback BEFORE the sync work, so a synchronous completion path (teardown,
	// no Mass simulation) still invokes it via the broadcast at the end.
	if (OnComplete.IsBound())
	{
		OnSaveFlushBroadcast.Add(OnComplete);
	}

	if (bSaveFlushInProgress)
	{
		// In-flight flush will broadcast to all registered callbacks when it finishes.
		return;
	}
	bSaveFlushInProgress = true;

	// Synchronous work that doesn't touch Mass fragments. Map travel data is captured only on explicit saves
	// — on teardown, the game code that triggered the map change owns the next-session start spot.
	// Level-streaming persistence serializes actor UPROPERTYs and is independent of Mass.
	UWorld* World = GetWorld();
	if (World && !World->bIsTearingDown)
	{
		FlushMapTravelData();
	}
	FlushLevelStreamingPersistenceData();

	// IAM serialization and the Mass snapshot both read Mass fragment data, so both must run in the
	// phase-quiescent slot. SnapshotEntities defers to the next FrameEnd boundary (or runs synchronously
	UMassPersistenceUtils::SnapshotEntities(
		this,
		FOnMassPreSnapshot::CreateUObject(this, &UPersistenceWorldSubsystem::PerformPreSaveMassTasks),
		FOnMassSnapshotComplete::CreateUObject(this, &UPersistenceWorldSubsystem::OnSaveFlushMassPartFinished));
}

void UPersistenceWorldSubsystem::PerformPreSaveMassTasks()
{
	// On the GameThread now, outside of Mass phases. We can now safely let Instanced Actor components read Mass Fragments
	// in order to serialize their data in a custom way.
	FlushInstancedActorManagers();

	// Then, any game code that wishes to write to Mass Fragments before Mass Entity snapshots are created can do so,
	// in order to ensure that the latest gameplay state is represented in those entities.
	OnPreFlushMassEntityData.Broadcast();

	// Return execution to UMassPersistenceUtils::SnapshotEntities
}

void UPersistenceWorldSubsystem::OnSaveFlushMassPartFinished(TArray<FMassEntityConfigGroupSnapshot> Snapshots)
{
	const int32 NumSnapshots = Snapshots.Num();

	// Stamp result onto the active SaveGame so subsequent disk writes capture it.
	if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UPersistenceGameSubsystem* PersistenceSubsystem = GameInstance->GetSubsystem<UPersistenceGameSubsystem>())
		{
			const FName MapKey = GetWorld()->GetOutermost()->GetFName();
			PersistenceSubsystem->SetMassEntityDataForMap(MapKey, MoveTemp(Snapshots));
		}
	}

	UE_LOG(LogPersistenceUtils, Log, TEXT("RequestSaveFlush: Stored %d Mass snapshots for '%s'."), NumSnapshots, *GetWorld()->GetOutermost()->GetName());

	// Snapshot a copy of the bindings before broadcasting, so any re-entry that calls RequestSaveFlush
	// from inside a completion callback lands on a fresh multicast and isn't dropped by the Clear below.
	FOnSaveFlushComplete LocalBroadcast = OnSaveFlushBroadcast;
	OnSaveFlushBroadcast.Clear();
	bSaveFlushInProgress = false;
	LocalBroadcast.Broadcast();
}

void UPersistenceWorldSubsystem::RestoreMassEntityData()
{
	UPersistenceGameSubsystem* PersistenceSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<UPersistenceGameSubsystem>();
	const UPersistenceSaveGame* SaveGame = PersistenceSubsystem ? PersistenceSubsystem->GetSaveGame() : nullptr;
	if (!SaveGame)
	{
		return;
	}

	const FName MapKey = GetWorld()->GetOutermost()->GetFName();
	const FWorldPersistenceEntry* WorldEntry = SaveGame->SavedStatePerMap.Find(MapKey);
	if (!WorldEntry || WorldEntry->MassEntitySnapshots.IsEmpty())
	{
		return;
	}

	const int32 NumSnapshots = WorldEntry->MassEntitySnapshots.Num();
	UMassPersistenceUtils::RestoreEntities(this, WorldEntry->MassEntitySnapshots, FOnMassRestoreComplete::CreateLambda([NumSnapshots, MapKey]()
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("RestoreMassEntityData: Deserialized %d snapshots for map '%s'."), NumSnapshots, *MapKey.ToString());
	}));
}

void UPersistenceWorldSubsystem::OnMassSimulationStarted(UWorld* World)
{
	if (World != GetWorld() || !bPendingMassRestore)
	{
		return;
	}

	bPendingMassRestore = false;

	if (UMassSimulationSubsystem* SimSubsystem = GetWorld()->GetSubsystem<UMassSimulationSubsystem>())
	{
		SimSubsystem->GetOnSimulationStarted().Remove(SimulationStartedHandle);
	}

	RestoreMassEntityData();
}
