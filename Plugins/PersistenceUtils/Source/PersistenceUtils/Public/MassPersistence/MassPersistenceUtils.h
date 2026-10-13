// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Data/PersistenceDataTypes.h"
#include "MassEntityTypes.h"     // FMassEntityHandle
#include "MassPersistenceUtils.generated.h"

// Invoked synchronously in the phase-quiescent slot immediately before the snapshot is captured. Use this
// to push actor-authoritative state into Mass fragments so the snapshot reflects it.
DECLARE_DELEGATE(FOnMassPreSnapshot);

// Invoked with the captured snapshots once SnapshotEntities is complete.
DECLARE_DELEGATE_OneParam(FOnMassSnapshotComplete, TArray<FMassEntityConfigGroupSnapshot> /*Snapshots*/);

// Invoked once RestoreEntities has spawned all entities and written saved fragment bytes.
DECLARE_DELEGATE(FOnMassRestoreComplete);

UCLASS()
class PERSISTENCEUTILS_API UMassPersistenceUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Snapshots all live entities carrying FPersistableEntityConfigFragment (i.e., spawned from a config that uses
	 * UPersistableEntityConfigTrait), grouped by their source UMassEntityConfigAsset. One snapshot is produced per config.
	 *
	 * Phase-safety: snapshot work always runs in a phase-quiescent slot.
	 *   - Mid-play: deferred to the next EMassProcessingPhase::FrameEnd. PreSnapshot runs first, then the snapshot.
	 *   - Tear-down (World->bIsTearingDown): runs synchronously. Safe because OnWorldBeginTearDown itself is
	 *     dispatched between frames, outside any active phase, and no further phase ticks will occur before
	 *     UMassSimulationSubsystem::Deinitialize stops the phase manager.
	 *
	 * Fail-safe: if the world begins tearing down between scheduling and the next phase end, the snapshot runs
	 * synchronously from the teardown broadcast instead of being lost.
	 *
	 * OnComplete always fires (with an empty array if no entities or no Mass simulation present).
	 */
	static void SnapshotEntities(
		const UObject* WorldContextObject,
		FOnMassPreSnapshot PreSnapshot,
		FOnMassSnapshotComplete OnComplete);

	/**
	 * Restores entities from the given snapshots. SpawnEntities + fragment writes are always deferred to the next
	 * EMassProcessingPhase::FrameEnd so they don't collide with phase-executing processors. OnComplete fires after the
	 * work is done. Snapshots whose SourceConfigAsset cannot be resolved are skipped with a warning.
	 *
	 * Fail-safe: if the world begins tearing down before the next phase end, the restore is abandoned and OnComplete
	 * still fires (restoring during teardown serves no purpose).
	 *
	 * NOTE: Fragment structs are written raw. If a struct's layout has changed since the snapshot was saved, the
	 * deserialized data will be incorrect.
	 */
	static void RestoreEntities(
		const UObject* WorldContextObject,
		TArray<FMassEntityConfigGroupSnapshot> Snapshots,
		FOnMassRestoreComplete OnComplete);

private:
	// Synchronous snapshot worker. Caller is responsible for invoking this in a phase-quiescent slot.
	static void DoSnapshotWork(UWorld& World, TArray<FMassEntityConfigGroupSnapshot>& OutSnapshots);

	// Synchronous restore worker. Caller is responsible for invoking this in a phase-quiescent slot.
	static void DoRestoreWork(UWorld& World, TConstArrayView<FMassEntityConfigGroupSnapshot> Snapshots);
};
