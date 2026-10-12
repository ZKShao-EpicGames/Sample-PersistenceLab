// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/PersistenceDataTypes.h"
#include "GameFramework/SaveGame.h"
#include "PersistenceSaveGame.generated.h"

UCLASS()
class PERSISTENCEUTILS_API UPersistenceSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	// The slot name and user index used to identify this SaveGame when calling UGameplayStatics::SaveGameToSlot / LoadGameFromSlot.
	UPROPERTY()
	FString SlotName;

	UPROPERTY()
	int32 UserIndex = 0;

	// Game time in seconds played with this save across all maps and sessions, see UPersistenceGameSubsystem::GetGameTime.
	UPROPERTY()
	double GameTime = 0.0;

	// Data required to restore the player's position after travelling to a map.
	UPROPERTY()
	FPersistenceTravelData TravelData;

	// Saved state per World Partitioned map asset.
	// Internally, stores persistent data for the World Partition streaming levels, instanced actors, and non-partitioned Mass Entities.
	UPROPERTY()
	TMap<FName, FWorldPersistenceEntry> SavedStatePerMap;
};
