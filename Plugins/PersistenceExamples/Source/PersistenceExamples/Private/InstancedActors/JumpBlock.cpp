// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstancedActors/JumpBlock.h"
#include "Net/UnrealNetwork.h"

AJumpBlock::AJumpBlock()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
	MeshComponent->SetNotifyRigidBodyCollision(true);
	SetRootComponent(MeshComponent);

	InstancedActorsComp = CreateDefaultSubobject<UExampleInstancedActorComponent>(TEXT("ExampleIAComp"));
}

void AJumpBlock::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AJumpBlock, VisualState);
}

void AJumpBlock::BeginPlay()
{
	Super::BeginPlay();
	ApplyVisualState();
}

// Game code callable function that changes the actor's visual state and notifies this actor's instanced actor component,
// so that the visual state value on the Mass entity will be updates as well.
void AJumpBlock::SetVisualState(int32 NewVisualState)
{
	VisualState = NewVisualState;
	ApplyVisualState();
	InstancedActorsComp->NotifyVisualStateChanged(NewVisualState);
}

// Update server or client-side visual after receiving the state from this actor's instanced actor component.
// For example, on hydration from Mass entity into this actor.
void AJumpBlock::OnVisualStateRestored_Implementation(int32 VisualStateIndex)
{
	VisualState = VisualStateIndex;
	ApplyVisualState();
}

void AJumpBlock::ApplyVisualState()
{
	// Mirrors the IAC's visual state overrides (mesh, visibility, transform per SMC name) onto this actor's SMCs.
	InstancedActorsComp->ApplyVisualStateToOwner(VisualState);
}

// Update client-side visual after receiving the state via replication from the server-side actor.
void AJumpBlock::OnRep_VisualState()
{
	SetVisualState(VisualState);
}
