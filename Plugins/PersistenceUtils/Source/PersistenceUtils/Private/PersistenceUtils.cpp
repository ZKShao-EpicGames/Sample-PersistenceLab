// Copyright Epic Games, Inc. All Rights Reserved.

#include "PersistenceUtils.h"

#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "References/PersistableActorReferenceManager.h"
#include "Framework/PersistedObjectInterface.h"
#include "GameFramework/Actor.h"
#include "LevelStreamingPersistenceModule.h"
#include "MassActorSubsystem.h"
#include "UObject/UnrealType.h"

// Pull in the macro-defining headers unconditionally so the guard below evaluates correctly.
// Mirrors the include order at the top of GameplayDebuggerCategory_InstancedActors.h.
#include "InstancedActorsDebug.h"  // WITH_INSTANCEDACTORS_DEBUG
#include "MassEntityTypes.h"       // WITH_MASSENTITY_DEBUG

#if WITH_GAMEPLAY_DEBUGGER && WITH_INSTANCEDACTORS_DEBUG && WITH_MASSENTITY_DEBUG
#include "GameplayDebugger.h"
#include "GameplayDebuggerCategory_InstancedActors.h"
#endif

DEFINE_LOG_CATEGORY(LogPersistenceUtils);

#define LOCTEXT_NAMESPACE "FPersistenceUtilsModule"

namespace
{
	const FName Name_RelativeLocation(TEXT("RelativeLocation"));
	const FName Name_RelativeRotation(TEXT("RelativeRotation"));
	const FName Name_GDTCategory_InstancedActors(TEXT("InstancedActors"));
}

UE_DISABLE_OPTIMIZATION
void FPersistenceUtilsModule::StartupModule()
{
	if (!ILevelStreamingPersistenceModule::IsAvailable())
	{
		return;
	}

	ILevelStreamingPersistenceModule& LSP = ILevelStreamingPersistenceModule::Get();

	// Detect objects that implement IPersistedObject and call PrePersistObject before persisting
	LSP.OnPrePersistObject(UObject::StaticClass()).BindLambda(
		[](const UObject* InObject)
		{
			if (InObject && InObject->GetClass()->ImplementsInterface(UPersistedObject::StaticClass()))
			{
				IPersistedObject::Execute_PrePersistObject(const_cast<UObject*>(InObject));
			}
		});

	// Detect objects that implement IPersistedObject and call PostRestoreObject after restoring
	LSP.OnPostRestoreObject(UObject::StaticClass()).BindLambda(
		[](const UObject* InObject, const TArray<const FProperty*>& RestoredProperties)
		{
			if (!InObject || !InObject->GetClass()->ImplementsInterface(UPersistedObject::StaticClass()))
			{
				return;
			}

			TArray<FName> RestoredPropertyNames;
			RestoredPropertyNames.Reserve(RestoredProperties.Num());
			for (const FProperty* Property : RestoredProperties)
			{
				if (Property)
				{
					RestoredPropertyNames.Add(Property->GetFName());
				}
			}

			IPersistedObject::Execute_PostRestoreObject(const_cast<UObject*>(InObject), RestoredPropertyNames);
		});

	// Selectively persist RelativeLocation and RelativeRotation: only movable, root USceneComponents on NetStartup actors.
	LSP.OnShouldPersistProperty(USceneComponent::StaticClass()).BindLambda(
		[](const UObject* Object, const FProperty* Property) -> bool
		{
			if (!Property)
			{
				return false;
			}

			if (const AActor* Actor = Cast<AActor>(Object))
			{
				// Only persist properties on actors not controlled by Mass
				const UWorld* World = Actor->GetWorld();
				if (!World)
				{
					return false;
				}
				UMassActorSubsystem* ActorSubsystem = World ? World->GetSubsystem<UMassActorSubsystem>() : nullptr;
				if (ActorSubsystem && ActorSubsystem->GetEntityHandleFromActor(Actor).IsValid())
				{
					return false;
				}
			}
			else if (const UActorComponent* Comp = Cast<UActorComponent>(Object))
			{
				const AActor* Owner = Comp->GetOwner();
				if (!Owner)
				{
					return false;
				}
				const UWorld* World = Owner->GetWorld();
				if (!World)
				{
					return false;
				}

				// Only persist component properties on actors not controlled by Mass
				UMassActorSubsystem* ActorSubsystem = World ? World->GetSubsystem<UMassActorSubsystem>() : nullptr;
				if (ActorSubsystem && ActorSubsystem->GetEntityHandleFromActor(Owner).IsValid())
				{
					return false;
				}

				// Transform values only persist on map actor's movable root scene components
				const FName PropName = Property->GetFName();
				if (PropName == Name_RelativeLocation || PropName == Name_RelativeRotation)
				{
					return Owner->IsNetStartupActor() && Owner->GetRootComponent() == Comp && Owner->GetRootComponent()->Mobility == EComponentMobility::Movable;
				}
			}

			// No blocking conditions reached, just persist
			return true;
		});

	// After RelativeLocation/RelativeRotation are restored, update ComponentToWorld.
	LSP.OnPostRestoreObject(USceneComponent::StaticClass()).BindLambda(
		[](const UObject* InObject, const TArray<const FProperty*>& RestoredProperties)
		{
			USceneComponent* Comp = const_cast<USceneComponent*>(Cast<const USceneComponent>(InObject));
			if (!Comp)
			{
				return;
			}

			for (const FProperty* Property : RestoredProperties)
			{
				if (Property && (Property->GetFName() == Name_RelativeLocation || Property->GetFName() == Name_RelativeRotation))
				{
					Comp->UpdateComponentToWorld();
					return;
				}
			}
		});

	// Actors currently linked to a Mass entity are owned by Mass for persistence, so skip them in the
	// runtime-actor pathway. Covers both InstancedActors-spawned actors and Mass-Visualization high-LOD
	// representations. Once the actor is unlinked from its entity (e.g. EjectInstance on the IAC, or
	// Mass dehydrating the visualization), it falls through and gets persisted normally.
	LSP.OnShouldPersistRuntimeActor(AActor::StaticClass()).BindLambda(
		[](const AActor* InActor) -> bool
		{
			// Not controlled by Mass or IA, anymore?
			const UWorld* World = InActor->GetWorld();
			UMassActorSubsystem* ActorSubsystem = World ? World->GetSubsystem<UMassActorSubsystem>() : nullptr;
			return !ActorSubsystem || !ActorSubsystem->GetEntityHandleFromActor(InActor).IsValid();
		});

#if WITH_GAMEPLAY_DEBUGGER && WITH_INSTANCEDACTORS_DEBUG && WITH_MASSENTITY_DEBUG
	// The IA plugin ships FGameplayDebuggerCategory_InstancedActors but doesn't register it itself;
	// downstream projects wire it in. Enable it here so the GDT picks up the "InstancedActors" row.
	if (IGameplayDebugger::IsAvailable())
	{
		IGameplayDebugger& GDT = IGameplayDebugger::Get();
		GDT.RegisterCategory(
			Name_GDTCategory_InstancedActors,
			IGameplayDebugger::FOnGetCategory::CreateStatic(&FGameplayDebuggerCategory_InstancedActors::MakeInstance),
			EGameplayDebuggerCategoryState::EnabledInGameAndSimulate);
		GDT.NotifyCategoriesChanged();
	}
#endif

	// Route LSP's per-level post-restore signal to the per-world UPersistableActorReferenceManager so it
	// can finalize pending actor resolution callbacks for that level.
	LSP.PostRestoreLevel.BindLambda(
		[](const ULevel* Level)
		{
			if (!Level)
			{
				return;
			}
			if (UWorld* World = Level->GetWorld())
			{
				if (UPersistableActorReferenceManager* Mgr = World->GetSubsystem<UPersistableActorReferenceManager>())
				{
					Mgr->OnLevelPostRestore(Level);
				}
			}
		});
}
UE_ENABLE_OPTIMIZATION

void FPersistenceUtilsModule::ShutdownModule()
{
	// Both modules and classes may have begun destruction on app shutdown
	if (!ILevelStreamingPersistenceModule::IsAvailable() || AActor::StaticClass()->HasAnyFlags(RF_BeginDestroyed))
	{
		return;
	}

	ILevelStreamingPersistenceModule& LSP = ILevelStreamingPersistenceModule::Get();
	LSP.OnPrePersistObject(AActor::StaticClass()).Unbind();
	LSP.OnPostRestoreObject(AActor::StaticClass()).Unbind();
	LSP.OnShouldPersistProperty(USceneComponent::StaticClass()).Unbind();
	LSP.OnPostRestoreObject(USceneComponent::StaticClass()).Unbind();
	LSP.OnShouldPersistRuntimeActor(AActor::StaticClass()).Unbind();
	LSP.PostRestoreLevel.Unbind();

#if WITH_GAMEPLAY_DEBUGGER && WITH_INSTANCEDACTORS_DEBUG && WITH_MASSENTITY_DEBUG
	if (IGameplayDebugger::IsAvailable())
	{
		IGameplayDebugger& GDT = IGameplayDebugger::Get();
		GDT.UnregisterCategory(Name_GDTCategory_InstancedActors);
		GDT.NotifyCategoriesChanged();
	}
#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPersistenceUtilsModule, PersistenceUtils)
