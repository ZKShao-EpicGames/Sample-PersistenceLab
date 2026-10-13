// Copyright Epic Games, Inc. All Rights Reserved.

#include "References/PersistableActorReference.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "References/PersistableActorReferenceManager.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

AActor* FPersistableActorReference::TryResolve(UWorld* World) const
{
	if (!World)
	{
		return nullptr;
	}

	switch (Type)
	{
	case EPersistableActorReferenceType::LevelActor:
		if (UPersistableActorReferenceManager* Mgr = World->GetSubsystem<UPersistableActorReferenceManager>())
		{
			return Mgr->GetPersistedRuntimeActor(LevelPath, ActorName);
		}
		return nullptr;

	case EPersistableActorReferenceType::PlayerPawn:
		return UGameplayStatics::GetPlayerPawn(World, RuntimeIndex);

	case EPersistableActorReferenceType::PlayerController:
		return UGameplayStatics::GetPlayerController(World, RuntimeIndex);

	case EPersistableActorReferenceType::None:
	default:
		return nullptr;
	}
}

bool FPersistableActorReference::SetFromActor(AActor* Actor)
{
	Type = EPersistableActorReferenceType::None;
	LevelPath.Reset();
	ActorName = NAME_None;
	RuntimeIndex = INDEX_NONE;

	if (!Actor)
	{
		return false;
	}

	UWorld* World = Actor->GetWorld();
	if (!World)
	{
		return false;
	}

	// Player controllers and pawns prefer index-based identity, which is stable across sessions without needing component opt-in.
	if (APlayerController* PC = Cast<APlayerController>(Actor))
	{
		const int32 NumControllers = UGameplayStatics::GetNumPlayerControllers(World);
		for (int32 Index = 0; Index < NumControllers; ++Index)
		{
			if (UGameplayStatics::GetPlayerController(World, Index) == PC)
			{
				Type = EPersistableActorReferenceType::PlayerController;
				RuntimeIndex = Index;
				return true;
			}
		}
	}

	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		const int32 NumControllers = UGameplayStatics::GetNumPlayerControllers(World);
		for (int32 Index = 0; Index < NumControllers; ++Index)
		{
			if (UGameplayStatics::GetPlayerPawn(World, Index) == Pawn)
			{
				Type = EPersistableActorReferenceType::PlayerPawn;
				RuntimeIndex = Index;
				return true;
			}
		}
	}

	// Fall back to level-relative identity. Manager translates current FName -> last-session FName for
	// actors with UPersistableReferencedActorComponent; map-placed actors are resolved by scanning the level.
	if (ULevel* Level = Actor->GetLevel())
	{
		Type = EPersistableActorReferenceType::LevelActor;
		LevelPath = FSoftObjectPath(Level);
		ActorName = Actor->GetFName();
		return true;
	}

	return false;
}
