// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassPersistence/MassPersistenceUtils.h"
#include "PersistenceUtils.h"
#include "PersistenceUtilsSettings.h"
#include "MassPersistence/PersistableEntityConfigTrait.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTemplate.h"
#include "MassEntityManager.h"
#include "MassEntityQuery.h"
#include "MassRequirements.h"
#include "MassExecutionContext.h"
#include "MassSimulationSubsystem.h"
#include "MassSpawnerSubsystem.h"
#include "Engine/World.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"

void UMassPersistenceUtils::SnapshotEntities(const UObject* WorldContextObject, FOnMassPreSnapshot PreSnapshot, FOnMassSnapshotComplete OnComplete)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World)
	{
		OnComplete.ExecuteIfBound({});
		return;
	}

	// Teardown path: snapshot synchronously. The OnWorldBeginTearDown broadcast that triggers this fires from
	// between-frame engine cleanup paths (UWorld::EndPlay, seamless travel, engine shutdown), so we are already
	// outside any active phase. Waiting for the next phase boundary would deadlock: no further phase ticks
	// occur before UMassSimulationSubsystem::Deinitialize stops the phase manager.
	if (World->bIsTearingDown)
	{
		PreSnapshot.ExecuteIfBound();
		TArray<FMassEntityConfigGroupSnapshot> Snapshots;
		DoSnapshotWork(*World, Snapshots);
		OnComplete.ExecuteIfBound(MoveTemp(Snapshots));
		return;
	}

	UMassSimulationSubsystem* SimSub = World->GetSubsystem<UMassSimulationSubsystem>();
	if (!SimSub)
	{
		// No Mass simulation in this world, so nothing to snapshot. Fire empty so callers can proceed.
		PreSnapshot.ExecuteIfBound();
		OnComplete.ExecuteIfBound({});
		return;
	}

	// Shared state for the async snapshot operation. TSharedRef'd so both delegate bindings can refer to it;
	// bFired ensures only the first-firing delegate does the work.
	struct FAsyncSnapshotState
	{
		TWeakObjectPtr<UWorld> WeakWorld;
		TWeakObjectPtr<UMassSimulationSubsystem> WeakSimSub;
		FOnMassPreSnapshot PreSnapshot;
		FOnMassSnapshotComplete OnComplete;
		FDelegateHandle PhaseHandle;
		FDelegateHandle TeardownHandle;
		bool bFired = false;

		void Fire()
		{
			// Ensure this only fires once (since it's bound to multiple delegates).
			if (bFired)
			{
				return;
			}
			bFired = true;

			// Perform snapshot work
			UWorld* PinnedWorld = WeakWorld.Get();
			if (PinnedWorld)
			{
				PreSnapshot.ExecuteIfBound();
				TArray<FMassEntityConfigGroupSnapshot> Snapshots;
				DoSnapshotWork(*PinnedWorld, Snapshots);
				OnComplete.ExecuteIfBound(MoveTemp(Snapshots));
			}
			else
			{
				OnComplete.ExecuteIfBound({});
			}

			// Cleanup callbacks
			FWorldDelegates::OnWorldBeginTearDown.Remove(TeardownHandle);
			if (UMassSimulationSubsystem* PinnedSim = WeakSimSub.Get())
			{
				PinnedSim->GetOnProcessingPhaseFinished(EMassProcessingPhase::FrameEnd).Remove(PhaseHandle);
			}
		}
	};

	TSharedRef<FAsyncSnapshotState> State = MakeShared<FAsyncSnapshotState>();
	State->WeakWorld = World;
	State->WeakSimSub = SimSub;
	State->PreSnapshot = MoveTemp(PreSnapshot);
	State->OnComplete = MoveTemp(OnComplete);

	// Enqueue the save work to the GameThread after Mass's last processing phase for this tick.
	// This ensures all processors have finished for the frame and we're not reading fragments
	// while they might be operated on. The Presnapshot delegate will be called then, before 
	// actual saving. The OnComplete callback fires after the save is done, resuming the remaining
	// save SaveGame to file work.
	State->PhaseHandle = SimSub->GetOnProcessingPhaseFinished(EMassProcessingPhase::FrameEnd).AddLambda([State](float /*DeltaSeconds*/) 
		{ 
			State->Fire(); 
		});

	// Fail-safe: if the world begins tearing down before the next phase end, snapshot synchronously from
	// the teardown broadcast (between-frames, phase-safe) rather than losing the save.
	State->TeardownHandle = FWorldDelegates::OnWorldBeginTearDown.AddLambda(
		[State](UWorld* TornDownWorld)
		{
			if (TornDownWorld == State->WeakWorld.Get())
			{
				State->Fire();
			}
		});
}

void UMassPersistenceUtils::RestoreEntities(const UObject* WorldContextObject, TArray<FMassEntityConfigGroupSnapshot> Snapshots, FOnMassRestoreComplete OnComplete)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World || World->bIsTearingDown || Snapshots.IsEmpty())
	{
		// No world, world is going away, or nothing to do.
		OnComplete.ExecuteIfBound();
		return;
	}

	UMassSimulationSubsystem* SimSub = World->GetSubsystem<UMassSimulationSubsystem>();
	if (!SimSub)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: No UMassSimulationSubsystem, restore skipped."));
		OnComplete.ExecuteIfBound();
		return;
	}

	// Same shape as snapshot's async state, minus PreSnapshot: restore has no flush-state hook.
	struct FAsyncRestoreState
	{
		TWeakObjectPtr<UWorld> WeakWorld;
		TWeakObjectPtr<UMassSimulationSubsystem> WeakSimSub;
		TArray<FMassEntityConfigGroupSnapshot> Snapshots;
		FOnMassRestoreComplete OnComplete;
		FDelegateHandle PhaseHandle;
		FDelegateHandle TeardownHandle;
		bool bFired = false;

		void Fire(bool bDoWork)
		{
			if (bFired)
			{
				return;
			}
			bFired = true;

			// Restore data from Mass Entity snapshots onto the entities
			UWorld* PinnedWorld = WeakWorld.Get();
			if (bDoWork && PinnedWorld)
			{
				DoRestoreWork(*PinnedWorld, Snapshots);
			}
			OnComplete.ExecuteIfBound();

			// Cleanup callbacks
			if (UMassSimulationSubsystem* PinnedSim = WeakSimSub.Get())
			{
				PinnedSim->GetOnProcessingPhaseFinished(EMassProcessingPhase::FrameEnd).Remove(PhaseHandle);
			}
			FWorldDelegates::OnWorldBeginTearDown.Remove(TeardownHandle);
		}
	};

	TSharedRef<FAsyncRestoreState> State = MakeShared<FAsyncRestoreState>();
	State->WeakWorld = World;
	State->WeakSimSub = SimSub;
	State->Snapshots = MoveTemp(Snapshots);
	State->OnComplete = MoveTemp(OnComplete);

	State->PhaseHandle = SimSub->GetOnProcessingPhaseFinished(EMassProcessingPhase::FrameEnd).AddLambda([State](float /*DeltaSeconds*/) { State->Fire(/*bDoWork=*/true); });

	// If the world tears down before we get a phase tick, abandon the restore; there's no one to read it.
	State->TeardownHandle = FWorldDelegates::OnWorldBeginTearDown.AddLambda(
		[State](UWorld* TornDownWorld)
		{
			if (TornDownWorld == State->WeakWorld.Get())
			{
				State->Fire(/*bDoWork=*/false);
			}
		});
}

void UMassPersistenceUtils::DoSnapshotWork(UWorld& World, TArray<FMassEntityConfigGroupSnapshot>& OutSnapshots)
{
	UMassSpawnerSubsystem* SpawnerSubsystem = World.GetSubsystem<UMassSpawnerSubsystem>();
	if (!SpawnerSubsystem)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("SnapshotEntities: UMassSpawnerSubsystem not found."));
		return;
	}
	FMassEntityManager& EntityManager = SpawnerSubsystem->GetEntityManagerChecked();

	const UPersistenceUtilsSettings* Settings = GetDefault<UPersistenceUtilsSettings>();

	// Resolve the fragment allow list once. Each setting entry is the path name of a UScriptStruct.
	// Only fragments whose UScriptStruct is in this set will be considered for serialization.
	TSet<const UScriptStruct*> FragmentAllowList;
	for (const FName& PathName : Settings->MassFragmentsToSerialize)
	{
		if (const UScriptStruct* FragType = FindObject<UScriptStruct>(nullptr, *PathName.ToString()))
		{
			FragmentAllowList.Add(FragType);
		}
		else
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("SnapshotEntities: Could not resolve fragment type '%s', skipping."), *PathName.ToString());
		}
	}

	if (FragmentAllowList.IsEmpty())
	{
		return;
	}

	// First pass: query every entity tagged with FPersistableEntityConfigFragment (i.e., opted into
	// persistence via UPersistableEntityConfigTrait), then group those entities by source config.
	TMap<UMassEntityConfigAsset*, TArray<FMassEntityHandle>> EntitiesByConfig;
	{
		FMassEntityQuery DiscoveryQuery;
		DiscoveryQuery.Initialize(EntityManager.AsShared());
		DiscoveryQuery.AddRequirement<FPersistableEntityConfigFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::All);

		FMassExecutionContext DiscoveryContext(EntityManager);
		DiscoveryQuery.ForEachEntityChunk(DiscoveryContext, [&](FMassExecutionContext& Ctx)
		{
			for (int32 i = 0; i < Ctx.GetNumEntities(); ++i)
			{
				const FMassEntityHandle EntityHandle = Ctx.GetEntity(i);
				const FPersistableEntityConfigFragment& Origin =
					EntityManager.GetFragmentDataChecked<FPersistableEntityConfigFragment>(EntityHandle);
				if (UMassEntityConfigAsset* Config = Origin.EntityConfig)
				{
					EntitiesByConfig.FindOrAdd(Config).Add(EntityHandle);
				}
			}
		});
	}

	// Second pass: per config, derive the serializable fragment set from its template archetype,
	// then write each entity's fragment bytes in archetype-layout order.
	const UScriptStruct* OriginFragmentType = FPersistableEntityConfigFragment::StaticStruct();
	for (const TPair<UMassEntityConfigAsset*, TArray<FMassEntityHandle>>& Pair : EntitiesByConfig)
	{
		UMassEntityConfigAsset* ConfigAsset = Pair.Key;
		const TArray<FMassEntityHandle>& Entities = Pair.Value;
		if (Entities.IsEmpty())
		{
			continue;
		}

		const FMassEntityTemplate& Template = ConfigAsset->GetOrCreateEntityTemplate(World);
		const FMassArchetypeHandle& ArchetypeHandle = Template.GetArchetype();
		if (!ArchetypeHandle.IsValid())
		{
			continue;
		}

		// Collect fragment types for this archetype that are in the allow list, excluding the origin
		// fragment itself (its TObjectPtr would be meaningless if byte-serialized; the value is
		// restored separately from Snapshot.SourceConfigAsset).
		TArray<const UScriptStruct*> FragmentTypes;
		FMassEntityManager::ForEachArchetypeFragmentType(ArchetypeHandle, [&FragmentTypes, &FragmentAllowList, OriginFragmentType](const UScriptStruct* FragType)
		{
			if (FragType == OriginFragmentType)
			{
				return;
			}
			if (FragmentAllowList.Contains(FragType))
			{
				FragmentTypes.Add(FragType);
			}
		});

		if (FragmentTypes.IsEmpty())
		{
			continue;
		}

		TArray<uint8> Data;
		FMemoryWriter MemWriter(Data);

		for (const FMassEntityHandle& Entity : Entities)
		{
			for (const UScriptStruct* FragType : FragmentTypes)
			{
				FStructView FragView = EntityManager.GetFragmentDataStruct(Entity, FragType);
				MemWriter.Serialize(FragView.GetMemory(), FragType->GetStructureSize());
			}
		}

		FMassEntityConfigGroupSnapshot& Snapshot = OutSnapshots.AddDefaulted_GetRef();
		for (const UScriptStruct* FragType : FragmentTypes)
		{
			FMassFragmentLayout& Layout = Snapshot.FragmentLayout.AddDefaulted_GetRef();
			Layout.Type = FSoftObjectPath(FragType);
			Layout.SizeInBytes = FragType->GetStructureSize();
		}
		Snapshot.EntityCount = Entities.Num();
		Snapshot.Data = MoveTemp(Data);
		Snapshot.SourceConfigAsset = FSoftObjectPath(ConfigAsset);
	}
}

void UMassPersistenceUtils::DoRestoreWork(UWorld& World, TConstArrayView<FMassEntityConfigGroupSnapshot> Snapshots)
{
	UMassSpawnerSubsystem* SpawnerSubsystem = World.GetSubsystem<UMassSpawnerSubsystem>();
	if (!SpawnerSubsystem)
	{
		UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: UMassSpawnerSubsystem not found."));
		return;
	}
	FMassEntityManager& EntityManager = SpawnerSubsystem->GetEntityManagerChecked();

	const UPersistenceUtilsSettings* Settings = GetDefault<UPersistenceUtilsSettings>();

	// Resolve the fragment allow list once. Any saved fragment type that is no longer in the
	// allow list is skipped at restore: its bytes are consumed but never written, so the layout
	// stays in sync per entity. Symmetric with the SnapshotEntities filter.
	TSet<const UScriptStruct*> FragmentAllowList;
	for (const FName& PathName : Settings->MassFragmentsToSerialize)
	{
		if (const UScriptStruct* FragType = FindObject<UScriptStruct>(nullptr, *PathName.ToString()))
		{
			FragmentAllowList.Add(FragType);
		}
	}

	if (FragmentAllowList.IsEmpty())
	{
		return;
	}

	for (const FMassEntityConfigGroupSnapshot& Snapshot : Snapshots)
	{
		if (Snapshot.EntityCount <= 0 || Snapshot.Data.IsEmpty())
		{
			continue;
		}

		// Source config must be resolvable; the recreate-from-fragment-layout fallback was removed.
		if (!Snapshot.SourceConfigAsset.IsValid())
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: Snapshot has no SourceConfigAsset, skipping."));
			continue;
		}
		UMassEntityConfigAsset* RestoredConfig = Cast<UMassEntityConfigAsset>(Snapshot.SourceConfigAsset.TryLoad());
		if (!RestoredConfig)
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: Could not load config asset '%s', snapshot skipped."), *Snapshot.SourceConfigAsset.ToString());
			continue;
		}

		// Resolve saved fragment types. Entries that can no longer be loaded, whose size has changed,
		// or that are no longer in the allow list are marked null: their bytes will be skipped per
		// entity rather than aborting the snapshot.
		struct FResolvedFragment
		{
			const UScriptStruct* Struct = nullptr; // null = skip
			int32 SavedSize = 0;
		};
		TArray<FResolvedFragment> Resolved;
		TArray<const UScriptStruct*> ActiveFragmentTypes; // non-null entries only, for archetype construction

		for (const FMassFragmentLayout& Layout : Snapshot.FragmentLayout)
		{
			FResolvedFragment& R = Resolved.AddDefaulted_GetRef();
			R.SavedSize = Layout.SizeInBytes;
			R.Struct = Cast<UScriptStruct>(Layout.Type.TryLoad());

			if (!R.Struct)
			{
				UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: Fragment '%s' could not be loaded, skipping its %d bytes per entity."), *Layout.Type.ToString(), Layout.SizeInBytes);
			}
			else if (R.Struct->GetStructureSize() != Layout.SizeInBytes)
			{
				UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: Fragment '%s' size changed (%d → %d bytes), skipping saved data, fragment will be default-initialized."), *Layout.Type.GetAssetName(), Layout.SizeInBytes, R.Struct->GetStructureSize());
				R.Struct = nullptr;
			}
			else if (!FragmentAllowList.Contains(R.Struct))
			{
				UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: Fragment '%s' is no longer in MassFragmentsToSerialize, skipping its %d bytes per entity."), *Layout.Type.GetAssetName(), Layout.SizeInBytes);
				R.Struct = nullptr;
			}
			else
			{
				ActiveFragmentTypes.Add(R.Struct);
			}
		}

		if (ActiveFragmentTypes.IsEmpty())
		{
			UE_LOG(LogPersistenceUtils, Warning, TEXT("RestoreEntities: No resolvable fragments, snapshot skipped."));
			continue;
		}

		// Spawn the entities via the config asset's template.
		TArray<FMassEntityHandle> NewEntities;
		{
			const FMassEntityTemplate& Template = RestoredConfig->GetOrCreateEntityTemplate(World);
			TSharedPtr<FMassEntityManager::FEntityCreationContext> CreationContext =
				SpawnerSubsystem->SpawnEntities(Template, static_cast<uint32>(Snapshot.EntityCount), NewEntities);

			// Re-stamp the origin fragment with the snapshot's source config. Direct set assumes the trait
			// (UPersistableEntityConfigTrait) is on the config so the fragment is in the template archetype.
			// If the trait was removed since save, the fragment isn't present and we skip silently; the
			// restored entity is no longer persistable but is otherwise functional.
			for (FMassEntityHandle Entity : NewEntities)
			{
				if (FPersistableEntityConfigFragment* Origin = EntityManager.GetFragmentDataPtr<FPersistableEntityConfigFragment>(Entity))
				{
					Origin->EntityConfig = RestoredConfig;
				}
			}

			// Write saved fragment data into the newly spawned entities. Fragments that were removed from
			// the EntityConfig template, or resized, are skipped: their bytes are consumed but not written.
			FMemoryReader MemReader(Snapshot.Data);
			for (const FMassEntityHandle& Entity : NewEntities)
			{
				for (const FResolvedFragment& R : Resolved)
				{
					if (R.Struct)
					{
						FStructView FragView = EntityManager.GetFragmentDataStruct(Entity, R.Struct);
						MemReader.Serialize(FragView.GetMemory(), R.SavedSize);
					}
					else
					{
						MemReader.Seek(MemReader.Tell() + R.SavedSize);
					}
				}
			}
		} // CreationContext goes out of scope, observers can now start processing the entities

		UE_LOG(LogPersistenceUtils, Log, TEXT("RestoreEntities: Restored %d entities (%s)."), NewEntities.Num(), *Snapshot.SourceConfigAsset.GetAssetName());
	}
}
