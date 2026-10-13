// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MassTranslator.h"
#include "MassEntityQuery.h"
#include "NPCActorTransformToMassTranslator.generated.h"

/**
 * Copies actor transform into FTransformFragment for entities tagged FNPCHydratedTag.
 *
 * Drop-in alternative to UMassCharacterMovementToMassTranslator for the actor-authoritative
 * hybrid flow that uses UMassActorSubsystem mapping (FMassActorFragment) rather than
 * UMassAgentComponent. The engine translator depends on FCharacterMovementComponentWrapperFragment
 * which is populated by UMassAgentComponent's initializer; this one reads the actor directly.
 *
 * Inherits auto-register=true from UMassTranslator, runs in SyncWorldToMass group every tick.
 */
UCLASS()
class PERSISTENCEEXAMPLES_API UNPCActorTransformToMassTranslator : public UMassTranslator
{
	GENERATED_BODY()

public:
	UNPCActorTransformToMassTranslator();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;
};
