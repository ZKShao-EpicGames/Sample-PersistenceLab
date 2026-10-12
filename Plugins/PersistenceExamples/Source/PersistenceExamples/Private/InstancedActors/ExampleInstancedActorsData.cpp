// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstancedActors/ExampleInstancedActorsData.h"
#include "InstancedActors/ExampleInstancedActorStages.h"
#include "InstancedActorsManager.h"
#include "InstancedActorsTypes.h"
#include "Mass/HealthFragment.h"
#include "MassCommandBuffer.h"
#include "MassEntityManager.h"

void UExampleInstancedActorsData::ApplyInstanceDelta(FMassEntityManager& EntityManager, const FInstancedActorsDelta& InstanceDelta, TArray<FInstancedActorsInstanceIndex>& OutEntitiesToRemove)
{
	// Let the engine handle destruction (the only thing the base implementation acts on).
	Super::ApplyInstanceDelta(EntityManager, InstanceDelta, OutEntitiesToRemove);

	if (!HasSpawnedEntities())
	{
		return;
	}

	// A destroyed instance is on its way out; the lifecycle phase is irrelevant.
	if (InstanceDelta.IsDestroyed())
	{
		return;
	}

	// The only piece of replicated state we can use to recover Visual State is the lifecycle phase.
	if (!InstanceDelta.HasCurrentLifecyclePhase())
	{
		return;
	}

	// On the server/standalone the gameplay-driven path (UExampleInstancedActorComponent, UExampleStageTimerProcessor)
	// already swaps the mesh, so only clients need to act on the replicated phase.
	AInstancedActorsManager* Manager = GetManager();
	if (!Manager || Manager->GetNetMode() != NM_Client)
	{
		return;
	}

	// Convention: visualization index == lifecycle phase index (see class comment). Applied synchronously, since the
	// engine's deferred visualization switch is consumed by a server-only processor. See ExampleIAStages::SwitchInstanceVisualization.
	const uint8 PhaseIndex = InstanceDelta.GetCurrentLifecyclePhaseIndex();
	ExampleIAStages::SwitchInstanceVisualization(*this, EntityManager, GetEntity(InstanceDelta.GetInstanceIndex()), PhaseIndex);
}

void UExampleInstancedActorsData::OnSpawnEntities()
{
	Super::OnSpawnEntities();

	if (PendingVisualization.IsEmpty() && PendingHealth.IsEmpty() && PendingStages.IsEmpty())
	{
		return;
	}

	// Staged persistence state is server-authoritative. On clients (where these maps are never filled,
	// since restore only runs on the authority) there is nothing to do; clear and bail defensively.
	const AInstancedActorsManager* Manager = GetManager();
	if (!Manager || !Manager->HasAuthority())
	{
		PendingVisualization.Empty();
		PendingHealth.Empty();
		PendingStages.Empty();
		return;
	}

	// Phase 2: entities now exist (OnSpawnEntities runs at the end of SpawnEntities). Apply the data
	// staged during deserialization onto the freshly spawned Mass entities - once, on the game thread,
	// before any simulation processor ticks.
	FMassEntityManager& EntityManager = GetMassEntityManagerChecked();
	for (const TPair<int32, uint8>& VizPair : PendingVisualization)
	{
		const FInstancedActorsInstanceIndex InstanceIndex(VizPair.Key);
		ExampleIAStages::SwitchInstanceVisualization(*this, EntityManager, GetEntity(InstanceIndex), VizPair.Value);

		// Re-seed the replicated lifecycle-phase delta from restored viz. Persistence serializes only
		// destruction (AInstancedActorsManager::SerializeInstancePersistenceData), not the phase, so after a
		// server save/load InstanceDeltas carries no phase entry for instances whose viz changed pre-save.
		// Without this, a client joining a restored session sees the default mesh on dehydrated instances.
		// Mirrors the live producer in UExampleInstancedActorComponent::NotifyVisualStateChanged. Authority
		// is guaranteed here (early-out above); SetInstanceCurrentLifecyclePhase asserts on it. Convention:
		// viz index == lifecycle phase index. Server-side this is a no-op for gameplay (base ApplyInstanceDelta
		// ignores the phase), so there is no double-apply with the visualization switch above.
		SetInstanceCurrentLifecyclePhase(InstanceIndex, VizPair.Value);
	}

	for (const TPair<int32, FVector2f>& HealthPair : PendingHealth)
	{
		const FMassEntityHandle EntityHandle = GetEntity(FInstancedActorsInstanceIndex(HealthPair.Key));
		if (EntityManager.IsEntityValid(EntityHandle))
		{
			if (FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
			{
				HealthFrag->Current = HealthPair.Value.X;
				HealthFrag->Max = HealthPair.Value.Y;
			}
		}
	}

	const double Now = GetWorld()->GetTimeSeconds();
	for (const TPair<int32, FPendingStage>& StagePair : PendingStages)
	{
		ApplyPersistedStage(EntityManager, FInstancedActorsInstanceIndex(StagePair.Key), StagePair.Value.Stage, StagePair.Value.RemainingSeconds, Now);
	}

	PendingVisualization.Empty();
	PendingHealth.Empty();
	PendingStages.Empty();
}

void UExampleInstancedActorsData::ApplyPersistedStage(FMassEntityManager& EntityManager, FInstancedActorsInstanceIndex InstanceIndex, uint8 Stage, double RemainingSeconds, double Now)
{
	const FMassEntityHandle EntityHandle = GetEntity(InstanceIndex);
	if (!EntityManager.IsEntityValid(EntityHandle))
	{
		return;
	}

	FExampleStageFragment* StageFragment = EntityManager.GetFragmentDataPtr<FExampleStageFragment>(EntityHandle);
	const FExampleStageConfigSharedFragment* Config = EntityManager.GetConstSharedFragmentDataPtr<FExampleStageConfigSharedFragment>(EntityHandle);
	if (!StageFragment || !Config)
	{
		// The actor class no longer uses stage timers.
		return;
	}

	// Clamp in case the actor class now has fewer stages than when the data was saved.
	StageFragment->Stage = static_cast<uint8>(FMath::Min<int32>(Stage, FMath::Max(Config->StageDurations.Num() - 1, 0)));
	StageFragment->StageEndTime = Now + RemainingSeconds;

	// Consume the real time that passed between saving and loading, possibly across multiple stages.
	ExampleIAStages::AdvanceStages(*StageFragment, Config->StageDurations, Now);

	if (StageFragment->Stage == 0)
	{
		return;
	}

	ExampleIAStages::SwitchInstanceVisualization(*this, EntityManager, EntityHandle, StageFragment->Stage);
	ExampleIAStages::ReplicateStage(*this, InstanceIndex, StageFragment->Stage);
	EntityManager.Defer().AddTag<FExampleStageTimerActiveTag>(EntityHandle);
}

void UExampleInstancedActorsData::StagePersistedVisualization(int32 InstanceIndex, uint8 VisualizationIndex)
{
	PendingVisualization.Add(InstanceIndex, VisualizationIndex);
}

void UExampleInstancedActorsData::StagePersistedHealth(int32 InstanceIndex, float Current, float Max)
{
	PendingHealth.Add(InstanceIndex, FVector2f(Current, Max));
}

void UExampleInstancedActorsData::StagePersistedStage(int32 InstanceIndex, uint8 Stage, double RemainingSeconds)
{
	PendingStages.Add(InstanceIndex, { Stage, RemainingSeconds });
}
