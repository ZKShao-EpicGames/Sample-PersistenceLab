// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/PersistenceSaveGame.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PersistenceGameSubsystem.generated.h"

/**
 * This GameInstance subsystem is automatically created. It manages the SaveGame data in memory and persists across map loads.
 */
UCLASS()
class PERSISTENCEUTILS_API UPersistenceGameSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	UPersistenceSaveGame* GetSaveGame() const { return SaveGame; }
	bool IsTravelingFromSaveFile() const { return bTravelingFromSaveFile; }

	UPersistenceSaveGame* StartNewSaveFile(const FString& SlotName, int32 UserIndex, TSubclassOf<UPersistenceSaveGame> SpecificClass);
	UPersistenceSaveGame* LoadFromFile(const FString& SlotName, int32 UserIndex, bool bStartTravel = true);
	UPersistenceSaveGame* ReloadFromFile(bool bStartTravel = true);
	void TravelFromSaveFile();

	// Initiates a save. Broadcasts OnPreSave so external game code can push state into the SaveGame, then calls
	// UPersistenceWorldSubsystem::RequestSaveFlush which runs the plugin's synchronous flushes (IAM, level
	// streaming, optionally travel data) inline and schedules the Mass entity snapshot at the next FrameEnd
	// phase boundary (or runs it synchronously on world teardown). The on-disk write happens once that flush
	// completes. Returns immediately; observers waiting for the disk write should listen for the completion log
	// from ContinueSaveToFileToDisk, or extend the API with an explicit completion delegate.
	void SaveToFile();

	// Ensures an entry exists in the SaveGame for the given map and sets its streaming level data.
	void SetStreamingLevelDataForMap(FName MapName, TArray<uint8> InData);

	// Stores serialized IAM delta data into the SaveGame, creating map/level entries as needed.
	void SetInstancedActorManagerDataForLevel(FName MapName, FName LevelName, FName ManagerName, TArray<uint8> InData);

	// Replaces the Mass entity snapshots for the given map in the SaveGame.
	void SetMassEntityDataForMap(FName MapName, TArray<FMassEntityConfigGroupSnapshot> InSnapshots);

	void SetPersistenceTravelData(FPersistenceTravelData InTravelData);

	// Game time in seconds played with the active save, across all maps and sessions: the time of previously played worlds
	// plus the current world's time. Advances only while a world runs (respecting pause and time dilation), and resumes
	// from the saved value when loading a save, so time while the game wasn't running never counts. Unlike world
	// timestamps, values are comparable across map travel and save/load, e.g. to measure game time elapsed since some
	// data was saved.
	double GetGameTime() const;

	// Called by UPersistenceWorldSubsystem when a world is torn down, to carry its time over into GetGameTime.
	void AccumulateWorldGameTime(const UWorld& World);

	// Called by UPersistenceWorldSubsystem::OnWorldBeginPlay to mark travel-from-save-file as complete and trigger any post-arrival restoration.
	void ReportTravelFromSaveFileComplete(UWorld* World);

private:
	// Writes SaveGame to its slot. Called either directly from SaveToFile (no world subsystem) or as the
	// completion callback of UPersistenceWorldSubsystem::RequestMassEntityFlush.
	void ContinueSaveToFileToDisk();

	UPROPERTY()
	TObjectPtr<UPersistenceSaveGame> SaveGame;

	bool bTravelingFromSaveFile = false;

	// Game time of all worlds before the current one. See GetGameTime.
	double GameTimeBeforeCurrentWorld = 0.0;

	// Wall-clock start of the in-flight SaveToFile, captured before any flush work so the completion log
	// can report total elapsed time including the async wait.
	double SaveStartTime = 0.0;
};
