// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Framework/PersistedObjectInterface.h"
#include "UObject/SoftObjectPath.h"
#include "PersistableReferencedActorComponent.generated.h"

/**
 * Opt-in component that lets an actor be re-resolved across save/load by its previous-session identity.
 *
 * On PrePersistObject the owner's (level path, actor FName) is captured and serialized by LSP.
 * On PostRestoreObject the component registers itself with UPersistableActorReferenceManager keyed by
 * that previous-session identity, even when the owner's runtime FName has changed (e.g., respawned runtime actors).
 *
 * Map-placed actors don't need this component; their FName is stable from the asset and resolves via level scan.
 * Use it when (a) the actor is runtime-spawned (name not stable across sessions), or (b) the actor may be ejected
 * and respawned at runtime by LSP and other code holds references to it.
 */
UCLASS(ClassGroup=(Persistence), meta=(BlueprintSpawnableComponent))
class PERSISTENCEUTILS_API UPersistableReferencedActorComponent : public UActorComponent, public IPersistedObject
{
	GENERATED_BODY()

public:
	UPersistableReferencedActorComponent();

	/** Captured from the owner on PrePersistObject; restored by LSP. Together with LastSessionActorName forms the cross-session key. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Persistence", SaveGame)
	FSoftObjectPath LastSessionLevelPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Persistence", SaveGame)
	FName LastSessionActorName;

	//~ IPersistedObject
	virtual void PrePersistObject_Implementation() override;
	virtual void PostRestoreObject_Implementation(const TArray<FName>& RestoredPropertyNames) override;

	//~ UActorComponent
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool bRegisteredWithManager = false;
};
