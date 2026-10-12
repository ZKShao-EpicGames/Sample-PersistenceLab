// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstancedActors/ExampleInstancedActorsManager.h"
#include "InstancedActors/ExampleInstancedActorsData.h"
#include "InstancedActors/ExampleInstancedActorStages.h"
#include "Components/InstancedStaticMeshComponent.h"

AExampleInstancedActorsManager::AExampleInstancedActorsManager()
{
	// AInstancedActorsManager::CreateNextInstanceActorData NewObject's this class for every per-actor-class instance group.
	InstancedActorsDataClass = UExampleInstancedActorsData::StaticClass();

	// Only ticks while there are spinning ISM components (see RegisterSpinningISMComponent). TG_PostUpdateWork runs after
	// Mass applies this frame's ISM instance adds (end of PostPhysics), so newly added instances render spun right away.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void AExampleInstancedActorsManager::RegisterSpinningISMComponent(UInstancedStaticMeshComponent* ISMComponent, float DegreesPerSecond)
{
	if (!ISMComponent || SpinningISMComponents.ContainsByPredicate([ISMComponent](const FSpinningISMComponent& Entry) { return Entry.Component == ISMComponent; }))
	{
		return;
	}

	// AInstancedActorsManager::CreateISMComponents forces Stationary mobility, collision and navigation relevance onto all ISM
	// components. None of those suit an ISM whose instances move every frame:
	//  - Movable lets the renderer treat it as dynamic (e.g. dynamic VSM page caching instead of invalidating cached static pages).
	//  - With collision, every transform update would also move a physics body. Pickups are handled by the hydrated actor anyway.
	//  - With navigation relevance, every transform update would dirty the navmesh.
	ISMComponent->SetMobility(EComponentMobility::Movable);
	ISMComponent->bDisableCollision = true;
	ISMComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ISMComponent->SetCanEverAffectNavigation(false);

	// Spinning is purely cosmetic.
	if (IsNetMode(NM_DedicatedServer))
	{
		return;
	}

	SpinningISMComponents.Add({ ISMComponent, DegreesPerSecond });
	SetActorTickEnabled(true);
}

void AExampleInstancedActorsManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const double Time = ExampleIAStages::GetSpinTime(GetWorld());

	for (int32 Index = SpinningISMComponents.Num() - 1; Index >= 0; --Index)
	{
		UInstancedStaticMeshComponent* ISMComponent = SpinningISMComponents[Index].Component.Get();
		if (!IsValid(ISMComponent))
		{
			SpinningISMComponents.RemoveAtSwap(Index);
			continue;
		}

		// Invisible: IA hides ISMs in the Off bulk LOD, and "empty" stages (e.g. a picked up health pack) use hidden ISMs.
		const int32 NumInstances = ISMComponent->GetInstanceCount();
		if (!ISMComponent->IsVisible() || NumInstances == 0)
		{
			continue;
		}

		// All instances share the same time-based rotation, so there's no need to track per-instance base transforms:
		// keep each instance's location and scale, and replace its rotation.
		const FQuat Rotation = ExampleIAStages::SpinRotation(Time, SpinningISMComponents[Index].DegreesPerSecond);
		SpinTransformsScratch.SetNum(NumInstances, EAllowShrinking::No);
		for (int32 InstanceIndex = 0; InstanceIndex < NumInstances; ++InstanceIndex)
		{
			ISMComponent->GetInstanceTransform(InstanceIndex, SpinTransformsScratch[InstanceIndex], /*bWorldSpace*/ true);
			SpinTransformsScratch[InstanceIndex].SetRotation(Rotation);
		}
		ISMComponent->BatchUpdateInstancesTransforms(0, SpinTransformsScratch, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
	}

	if (SpinningISMComponents.IsEmpty())
	{
		SetActorTickEnabled(false);
	}
}
