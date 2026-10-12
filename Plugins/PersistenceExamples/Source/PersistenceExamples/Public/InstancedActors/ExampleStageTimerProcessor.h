// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MassProcessor.h"
#include "ExampleStageTimerProcessor.generated.h"

/**
 * Server-side countdown of FExampleStageFragment for dehydrated instanced actors (respawning health packs, regrowing
 * pumpkins and grass). Only processes entities with FExampleStageTimerActiveTag, i.e. instances that are counting down.
 *
 * When a stage elapses, it swaps the instance's ISM visualization and replicates the new stage through the IAD's lifecycle
 * phase delta, so clients swap their ISM too (see UExampleInstancedActorsData::ApplyInstanceDelta).
 *
 * Hydrated instances are skipped: their UExampleInstancedActorComponent runs the same countdown (ExampleIAStages::AdvanceStages)
 * and also updates the actor. Runs on the game thread since it touches UObjects (IAD, ISM components, net dormancy).
 */
UCLASS()
class PERSISTENCEEXAMPLES_API UExampleStageTimerProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UExampleStageTimerProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;
};
