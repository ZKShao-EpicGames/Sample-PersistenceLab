// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassPersistence/PersistedMassSpawner.h"
#include "MassPersistence/PersistableEntityConfigTrait.h"
#include "PersistenceUtils.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassSpawnerTypes.h"

APersistedMassSpawner::APersistedMassSpawner()
{
	bAutoSpawnOnBeginPlay = false;
}

void APersistedMassSpawner::BeginPlay()
{
	Super::BeginPlay();

	if (ShouldSpawnEntities())
	{
		OnSpawningFinishedEvent.AddDynamic(this, &APersistedMassSpawner::OnSpawningFinished);
		DoSpawning();
	}
}

void APersistedMassSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Clear tracked entities so AMassSpawner::EndPlay's DoDespawning has nothing to act on.
	// Spawned entities are managed externally (serialized and restored via save game).
	AllSpawnedEntities.Empty();
	Super::EndPlay(EndPlayReason);
}

bool APersistedMassSpawner::ShouldSpawnEntities() const
{
	return !bHasEverSpawned;
}

void APersistedMassSpawner::OnSpawningFinished()
{
	OnSpawningFinishedEvent.RemoveDynamic(this, &APersistedMassSpawner::OnSpawningFinished);
	bHasEverSpawned = true;

	StampOriginFragmentOnSpawnedEntities();
}

void APersistedMassSpawner::StampOriginFragmentOnSpawnedEntities()
{
	UMassEntitySubsystem* EntitySubsystem = GetWorld()->GetSubsystem<UMassEntitySubsystem>();
	if (!EntitySubsystem)
	{
		return;
	}
	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();

	for (const FSpawnedEntities& Spawned : AllSpawnedEntities)
	{
		// Match each batch's template ID against EntityTypes to recover the source config asset.
		UMassEntityConfigAsset* ResolvedConfig = nullptr;
		for (const FMassSpawnedEntityType& EntityType : EntityTypes)
		{
			UMassEntityConfigAsset* Config = const_cast<UMassEntityConfigAsset*>(EntityType.GetEntityConfig());
			if (!Config)
			{
				continue;
			}
			const FMassEntityTemplate& Template = Config->GetOrCreateEntityTemplate(*GetWorld());
			if (Template.GetTemplateID() == Spawned.TemplateID)
			{
				ResolvedConfig = Config;
				break;
			}
		}

		if (!ResolvedConfig)
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("%s: Could not resolve config asset for template %s, origin fragment not stamped on %d entities."),
				*GetName(), *Spawned.TemplateID.ToString(), Spawned.Entities.Num());
			continue;
		}

		// Direct set on the existing fragment, no archetype migration. The trait UPersistableEntityConfigTrait
		// is responsible for placing the fragment into the archetype at template build time. Entities whose
		// config doesn't include the trait won't have the fragment; they're simply not persistable, no warning.
		for (FMassEntityHandle Entity : Spawned.Entities)
		{
			if (FPersistableEntityConfigFragment* Origin = EntityManager.GetFragmentDataPtr<FPersistableEntityConfigFragment>(Entity))
			{
				Origin->EntityConfig = ResolvedConfig;
			}
		}
	}
}
