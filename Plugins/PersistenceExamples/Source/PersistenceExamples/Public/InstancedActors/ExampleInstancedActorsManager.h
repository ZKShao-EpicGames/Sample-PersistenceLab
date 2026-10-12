// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InstancedActorsManager.h"
#include "ExampleInstancedActorsManager.generated.h"

/**
 * Example manager that spawns UExampleInstancedActorsData instead of the base UInstancedActorsData,
 * so dehydrated instances recover their Visual State on late-joining clients (see UExampleInstancedActorsData).
 *
 * Managers are spawned at EDITOR conversion time (Convert Actors to IAs) by UInstancedActorsSubsystem, which
 * reads its InstancedActorsManagerClass. The manager and its IADs are then serialized into the map
 * (AInstancedActorsManager::PerActorClassInstanceData is an Instanced, non-transient UPROPERTY), so the IAD
 * class is baked in at conversion time. UExampleInstancedActorsSubsystem points InstancedActorsManagerClass at
 * this class; the project settings point the active subsystem at it (see Config/DefaultInstancedActors.ini).
 *
 * NOTE: this only affects NEW conversions. JumpBlocks already converted/saved as base classes must be
 * re-converted in-editor (with the config active) for the override to take effect.
 *
 * Also spins the ISM instances of spinning items (e.g. health packs) while they're dehydrated. Their rotation is a pure
 * function of time (ExampleIAStages::SpinRotation), shared by all instances and by the hydrated actor's spinning
 * component, so the manager simply overwrites the rotation of every instance each frame. Mass only ever writes an IA
 * instance's transform when adding it, so it never undoes the spin.
 */
UCLASS()
class PERSISTENCEEXAMPLES_API AExampleInstancedActorsManager : public AInstancedActorsManager
{
	GENERATED_BODY()

public:
	AExampleInstancedActorsManager();

	/**
	 * Spins all instances of ISMComponent around their up axis from now on. Called by UExampleInstancedActorComponent
	 * when its IAD creates visualizations. Also makes the ISM component Movable (it's animated every frame) and
	 * disables its collision and navigation relevance, which would otherwise be updated for every instance, every frame.
	 */
	void RegisterSpinningISMComponent(UInstancedStaticMeshComponent* ISMComponent, float DegreesPerSecond);

	//~ Begin AActor Overrides
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor Overrides

private:
	struct FSpinningISMComponent
	{
		TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
		float DegreesPerSecond = 0.f;
	};

	// ISM components are destroyed and recreated whenever the IAD despawns / respawns its entities (e.g. streaming),
	// so entries are weak and pruned in Tick.
	TArray<FSpinningISMComponent> SpinningISMComponents;

	// Reused per tick to avoid reallocations.
	TArray<FTransform> SpinTransformsScratch;
};
