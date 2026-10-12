// Copyright Epic Games, Inc. All Rights Reserved.

#include "Framework/PersistenceGameSubsystem.h"
#include "Framework/PersistenceUtilsDelegates.h"
#include "Framework/PersistenceWorldSubsystem.h"
#include "PersistenceUtilsSettings.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "PersistenceUtils.h"

void UPersistenceGameSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

#if WITH_EDITOR
	if (GetWorld() && GetWorld()->IsPlayInEditor())
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("Initialize: PIE detected — attempting to load existing PIE save file."));
		if (!LoadFromFile(TEXT("PIETestFile"), 0, /*bStartTravel=*/false))
		{
			StartNewSaveFile(TEXT("PIETestFile"), 0, UPersistenceSaveGame::StaticClass());
		}
	}
#endif
}

UPersistenceSaveGame* UPersistenceGameSubsystem::StartNewSaveFile(const FString& SlotName, int32 UserIndex, TSubclassOf<UPersistenceSaveGame> SpecificClass)
{
	if (!SpecificClass)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("StartNewSaveFile: SpecificClass is null. A specific UPersistenceSaveGame subclass must be provided."));
		return nullptr;
	}

	UPersistenceSaveGame* NewSaveGame = Cast<UPersistenceSaveGame>(UGameplayStatics::CreateSaveGameObject(SpecificClass));
	if (!NewSaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("StartNewSaveFile: Failed to create SaveGame object of class '%s'."), *SpecificClass->GetName());
		return nullptr;
	}

	NewSaveGame->SlotName = SlotName.IsEmpty() ? TEXT("DefaultSlot") : SlotName;
	NewSaveGame->UserIndex = UserIndex;

	SaveGame = NewSaveGame;
	UE_LOG(LogPersistenceUtils, Log, TEXT("StartNewSaveFile: Created new SaveGame of class '%s' in slot '%s' (UserIndex: %d)."), *SpecificClass->GetName(), *SaveGame->SlotName, SaveGame->UserIndex);
	return SaveGame;
}

UPersistenceSaveGame* UPersistenceGameSubsystem::LoadFromFile(const FString& SlotName, int32 UserIndex, bool bStartTravel)
{
	SaveGame = Cast<UPersistenceSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex));
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("LoadFromFile: Failed to load SaveGame from slot '%s'."), *SlotName);
		return nullptr;
	}

	// Resume the saved game time once the world we travel to (which starts at time 0) begins.
	GameTimeBeforeCurrentWorld = SaveGame->GameTime;

	if (SaveGame->TravelData.Map.IsNull())
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("LoadFromFile: Loaded SaveGame from slot '%s' — no travel data, staying on current map."), *SlotName);
	}
	else if (bStartTravel)
	{
		TravelFromSaveFile();
	}
	else
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("LoadFromFile: Loaded SaveGame from slot '%s' — travel deferred (bStartTravel=false)."), *SlotName);
	}

	return SaveGame;
}

void UPersistenceGameSubsystem::TravelFromSaveFile()
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("TravelFromSaveFile: No active SaveGame. Call LoadFromFile or StartNewSaveFile first."));
		return;
	}

	if (SaveGame->TravelData.Map.IsNull())
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("TravelFromSaveFile: SaveGame has no travel data (Map is null)."));
		return;
	}

	UE_LOG(LogPersistenceUtils, Log, TEXT("TravelFromSaveFile: Travelling to map '%s'."), *SaveGame->TravelData.Map.GetAssetPath().GetPackageName().ToString());
	bTravelingFromSaveFile = true;

	// UPersistenceWorldSubsystem on the destination world handles PlayerStartPIE placement at the saved pawn transform.

	const FName MapPackageName = SaveGame->TravelData.Map.GetAssetPath().GetPackageName();
	UGameplayStatics::OpenLevel(this, MapPackageName);
}

void UPersistenceGameSubsystem::ReportTravelFromSaveFileComplete(UWorld* World)
{
	const FName ExpectedMap = SaveGame ? SaveGame->TravelData.Map.GetAssetPath().GetPackageName() : NAME_None;
	const FName LoadedMap = World ? World->GetOutermost()->GetFName() : NAME_None;

	if (LoadedMap == ExpectedMap)
	{
		UE_LOG(LogPersistenceUtils, Log, TEXT("ReportTravelFromSaveFileComplete: Travel complete on '%s'."), *LoadedMap.ToString());
	}
	else
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("ReportTravelFromSaveFileComplete: Expected map '%s' but reported from '%s'. Save data may not have been fully restored."), *ExpectedMap.ToString(), *LoadedMap.ToString());
	}

	bTravelingFromSaveFile = false;
}

UPersistenceSaveGame* UPersistenceGameSubsystem::ReloadFromFile(bool bStartTravel)
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("ReloadFromFile: No active SaveGame. Call LoadFromFile or StartNewSaveFile first."));
		return nullptr;
	}

	return LoadFromFile(SaveGame->SlotName, SaveGame->UserIndex, bStartTravel);
}

void UPersistenceGameSubsystem::SaveToFile()
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("SaveToFile: No active SaveGame found. Call StartNewSaveFile first."));
		return;
	}

	SaveStartTime = FPlatformTime::Seconds();

	// External extension point: game code can subscribe to OnPreSave to push state into the SaveGame
	// synchronously before the persistence plugin's own flushes run.
	FPersistenceUtilsDelegates::OnPreSave.Broadcast(this, SaveGame);

	UPersistenceWorldSubsystem* WorldSubsystem = nullptr;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UWorld* World = GameInstance->GetWorld())
		{
			WorldSubsystem = World->GetSubsystem<UPersistenceWorldSubsystem>();
		}
	}

	if (WorldSubsystem)
	{
		// Plugin-internal flushes (sync work inline + Mass async). Disk write is deferred to the completion.
		WorldSubsystem->RequestSaveFlush(
			UPersistenceWorldSubsystem::FOnSaveFlushComplete::FDelegate::CreateUObject(this, &UPersistenceGameSubsystem::ContinueSaveToFileToDisk));
	}
	else
	{
		// No world subsystem (e.g., transition world). Nothing to flush — write directly.
		ContinueSaveToFileToDisk();
	}
}

void UPersistenceGameSubsystem::ContinueSaveToFileToDisk()
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("ContinueSaveToFileToDisk: SaveGame was cleared during async flush — write aborted."));
		return;
	}

	SaveGame->GameTime = GetGameTime();
	UGameplayStatics::SaveGameToSlot(SaveGame, SaveGame->SlotName, SaveGame->UserIndex);

	const double Elapsed = FPlatformTime::Seconds() - SaveStartTime;
	UE_LOG(LogPersistenceUtils, Display, TEXT("Saved game to slot '%s' in %.1f seconds."), *SaveGame->SlotName, Elapsed);
}

double UPersistenceGameSubsystem::GetGameTime() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	return GameTimeBeforeCurrentWorld + (World ? World->GetTimeSeconds() : 0.0);
}

void UPersistenceGameSubsystem::AccumulateWorldGameTime(const UWorld& World)
{
	GameTimeBeforeCurrentWorld += World.GetTimeSeconds();
}

void UPersistenceGameSubsystem::SetPersistenceTravelData(FPersistenceTravelData InTravelData)
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("SetPersistenceTravelData: No active SaveGame. Call StartNewSaveFile first."));
		return;
	}

	SaveGame->TravelData = MoveTemp(InTravelData);
	UE_LOG(LogPersistenceUtils, Log, TEXT("SetPersistenceTravelData: Travel data set — Map: '%s', HasPawnTransform: %s."), *SaveGame->TravelData.Map.GetAssetPath().GetPackageName().ToString(), SaveGame->TravelData.bHasPawnTransform ? TEXT("true") : TEXT("false"));
}

void UPersistenceGameSubsystem::SetInstancedActorManagerDataForLevel(FName MapName, FName LevelName, FName ManagerName, TArray<uint8> InData)
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("SetInstancedActorManagerDataForLevel: No active SaveGame. Call StartNewSaveFile first."));
		return;
	}

	FInstancedActorManagerState& State = SaveGame->SavedStatePerMap.FindOrAdd(MapName)
		.SavedStatePerStreamingLevel.FindOrAdd(LevelName)
		.InstancedActorManagerDeltas.FindOrAdd(ManagerName);
	State.Data = MoveTemp(InData);
	UE_LOG(LogPersistenceUtils, Log, TEXT("SetInstancedActorManagerDataForLevel: Stored %d bytes for manager '%s' in level '%s' on map '%s'."),
		State.Data.Num(), *ManagerName.ToString(), *LevelName.ToString(), *MapName.ToString());
}

void UPersistenceGameSubsystem::SetMassEntityDataForMap(FName MapName, TArray<FMassEntityConfigGroupSnapshot> InSnapshots)
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("SetMassEntityDataForMap: No active SaveGame. Call StartNewSaveFile first."));
		return;
	}

	FWorldPersistenceEntry& Entry = SaveGame->SavedStatePerMap.FindOrAdd(MapName);
	Entry.MassEntitySnapshots = MoveTemp(InSnapshots);
	UE_LOG(LogPersistenceUtils, Log, TEXT("SetMassEntityDataForMap: Stored %d snapshots for map '%s'."), Entry.MassEntitySnapshots.Num(), *MapName.ToString());
}

void UPersistenceGameSubsystem::SetStreamingLevelDataForMap(FName MapName, TArray<uint8> InData)
{
	if (!SaveGame)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("SetStreamingLevelDataForMap: No active SaveGame. Call StartNewSaveFile first."));
		return;
	}

	FWorldPersistenceEntry& Entry = SaveGame->SavedStatePerMap.FindOrAdd(MapName);
	Entry.StreamingLevelData = MoveTemp(InData);
	UE_LOG(LogPersistenceUtils, Log, TEXT("SetStreamingLevelDataForMap: Updated streaming level data for map '%s' (%d bytes)."), *MapName.ToString(), Entry.StreamingLevelData.Num());
}
