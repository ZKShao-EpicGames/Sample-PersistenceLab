// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InstancedActorsData.h"
#include "ExampleInstancedActorsData.generated.h"

/**
 * Example UInstancedActorsData that replicates Visual State to dehydrated instances on late-joining clients.
 *
 * The problem: when an instanced actor is dehydrated there's no actor to replicate Visual State through, so a
 * late-joining client has no way to learn which visualization (ISM mesh) an instance should be showing.
 *
 * The insight: FInstancedActorsDelta already replicates CurrentLifecyclePhaseIndex to ALL clients (including
 * late-joiners) via the FastArraySerializer on UInstancedActorsData::InstanceDeltas - for free, out of the box.
 * The base ApplyInstanceDelta receives that phase index but only ever acts on destruction, silently ignoring it.
 *
 * This subclass closes that gap: on the client it maps the replicated lifecycle phase to a visualization and
 * applies the ISM mesh switch synchronously - no custom replication required. Instances in their default state
 * (visualization 0) carry no phase delta; when the server removes one, clients revert to visualization 0.
 *
 * Convention: visualization indices must be ordered to match lifecycle phase indices (phase N -> visualization N).
 * This is enforced at IAD setup time by the order visualizations are registered (see
 * UExampleInstancedActorComponent::ModifyMassEntityTemplate).
 *
 * To use: set AInstancedActorsManager::InstancedActorsDataClass to this class on the manager.
 */
UCLASS()
class PERSISTENCEEXAMPLES_API UExampleInstancedActorsData : public UInstancedActorsData
{
	GENERATED_BODY()

public:
	//~ Begin UInstancedActorsData Overrides
	virtual void ApplyInstanceDelta(FMassEntityManager& EntityManager, const FInstancedActorsDelta& InstanceDelta, TArray<FInstancedActorsInstanceIndex>& OutEntitiesToRemove) override;
	virtual void RollbackInstanceDelta(FMassEntityManager& EntityManager, const FInstancedActorsDelta& InstanceDelta) override;
	virtual void OnSpawnEntities() override;
	//~ End UInstancedActorsData Overrides

	// Phase 1 staging (server-only). Filled from UExampleInstancedActorComponent::SerializeInstancePersistenceData
	// on load - before Mass entities are spawned - and consumed in OnSpawnEntities once entities exist.
	// Keyed by instance index. See the persistence restore design: deserialize-before-spawn, apply on spawn.
	void StagePersistedVisualization(int32 InstanceIndex, uint8 VisualizationIndex);
	void StagePersistedHealth(int32 InstanceIndex, float Current, float Max);

	// RemainingSeconds is the saved time left in Stage, minus the game time elapsed since saving. It may be negative, in
	// which case the excess carries over into subsequent stages when applied in OnSpawnEntities.
	void StagePersistedStage(int32 InstanceIndex, uint8 Stage, double RemainingSeconds);

private:
	// Applies a restored stage to a freshly spawned entity: fragment, visualization, replication, timer tag.
	void ApplyPersistedStage(FMassEntityManager& EntityManager, FInstancedActorsInstanceIndex InstanceIndex, uint8 Stage, double RemainingSeconds, double Now);

	struct FPendingStage
	{
		uint8 Stage = 0;
		double RemainingSeconds = 0.0;
	};

	// Not a UPROPERTY (FPendingStage isn't reflected); transient staging only.
	TMap<int32, FPendingStage> PendingStages;

	UPROPERTY(Transient)
	TMap<int32, uint8> PendingVisualization;

	// Value: X = Current health, Y = Max health.
	UPROPERTY(Transient)
	TMap<int32, FVector2f> PendingHealth;
};
