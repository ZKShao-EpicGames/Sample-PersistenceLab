// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstancedActors/ExampleInstancedActorComponent.h"
#include "InstancedActors/ExampleInstancedActorsData.h"
#include "InstancedActors/ExampleInstancedActorsManager.h"
#include "InstancedActors/ExampleInstancedActorStages.h"
#include "InstancedActors/ExampleInstancedActorVersion.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Algo/AllOf.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GAS/HealthAttributeSet.h"
#include "Mass/HealthFragment.h"
#include "PersistenceExamples.h"
#include "InstancedActorsData.h"
#include "InstancedActorsManager.h"
#include "InstancedActorsTypes.h"
#include "ISMPartition/ISMComponentDescriptor.h"
#include "MassCommandBuffer.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassRepresentationFragments.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Framework/PersistenceWorldSubsystem.h"

namespace ExampleIAC
{
	constexpr uint32 PersistenceDataID = 0x45494143; // 'EIAC'

	static UHealthAttributeSet* FindOwnerHealthSet(AActor* Owner)
	{
		if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Owner))
		{
			if (const UAbilitySystemComponent* ASC = ASI->GetAbilitySystemComponent())
			{
				return const_cast<UHealthAttributeSet*>(ASC->GetSet<UHealthAttributeSet>());
			}
		}
		return nullptr;
	}
}

UExampleInstancedActorComponent::UExampleInstancedActorComponent()
{
	// Replicates CurrentStage for hydrated actors. (IA's server spawner also forces replication on this component.)
	SetIsReplicatedByDefault(true);

	// Only ticks to spin SpinningComponent, enabled in BeginPlay.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UExampleInstancedActorComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UExampleInstancedActorComponent, CurrentStage);
}

void UExampleInstancedActorComponent::ModifyMassEntityConfig(FMassEntityManager& InMassEntityManager, UInstancedActorsData* InstancedActorData, FMassEntityConfig& InOutMassEntityConfig) const
{
	Super::ModifyMassEntityConfig(InMassEntityManager, InstancedActorData, InOutMassEntityConfig);

	if (bSupportsHealth && InstancedActorData)
	{
		UHealthTrait* HealthTrait = NewObject<UHealthTrait>(InstancedActorData);
		InOutMassEntityConfig.AddTrait(*HealthTrait);
	}

	if (bUseStageTimers && InstancedActorData && GetNumStages() > 1)
	{
		UExampleStagesTrait* StagesTrait = NewObject<UExampleStagesTrait>(InstancedActorData);
		StagesTrait->StageDurations = GetStageDurations();
		InOutMassEntityConfig.AddTrait(*StagesTrait);
	}
}

void UExampleInstancedActorComponent::ModifyMassEntityTemplate(FMassEntityManager& InMassEntityManager, UInstancedActorsData* InstancedActorData, FMassEntityTemplateData& InOutMassEntityTemplateData) const
{
	Super::ModifyMassEntityTemplate(InMassEntityManager, InstancedActorData, InOutMassEntityTemplateData);

	if (!InstancedActorData)
	{
		return;
	}

	const FInstancedActorsVisualizationInfo* DefaultViz = InstancedActorData->GetVisualization(0);
	if (VisualDescriptionGenerationType == EVisualDescriptionGenerationType::PropertyConfigured && DefaultViz)
	{
		// For each additional visual state, copy state 0's descriptors and apply the per-SMC-name overrides.
		// AdditionalVisualStates[K] configures visual state K+1; state 0 is engine-built and needs no entry.
		// Descriptors whose source SMC name isn't in the override map pass through unchanged. Descriptors
		// whose override has bVisible=false are dropped (no ISM contribution at this state).
		for (int32 EntryIndex = 0; EntryIndex < AdditionalVisualStates.Num(); ++EntryIndex)
		{
			const TMap<FName, FVisualStateMeshOverride>& Overrides = AdditionalVisualStates[EntryIndex].Overrides;
			const auto IsHiddenByOverride = [&Overrides](const FISMComponentDescriptor& Descriptor)
			{
				const UStaticMeshComponent* SourceSMC = Descriptor.GetStaticMeshComponent().Get();
				const FVisualStateMeshOverride* Override = SourceSMC ? Overrides.Find(SourceSMC->GetFName()) : nullptr;
				return Override && !Override->bVisible;
			};

			FInstancedActorsVisualizationDesc Desc = DefaultViz->VisualizationDesc;
			if (!Desc.ISMComponentDescriptors.IsEmpty() && Algo::AllOf(Desc.ISMComponentDescriptors, IsHiddenByOverride))
			{
				// Dropping every descriptor would leave a visualization without ISMs, which has no valid Mass mesh handle
				// (the ISM switcher processors ensure on that). Keep them as hidden ISMs instead, e.g. for a picked up health pack.
				for (FISMComponentDescriptor& Descriptor : Desc.ISMComponentDescriptors)
				{
					Descriptor.bVisible = false;
				}
			}
			else
			{
				Desc.ISMComponentDescriptors.RemoveAll(IsHiddenByOverride);
			}

			for (FISMComponentDescriptor& Descriptor : Desc.ISMComponentDescriptors)
			{
				const UStaticMeshComponent* SourceSMC = Descriptor.GetStaticMeshComponent().Get();
				const FVisualStateMeshOverride* Override = SourceSMC ? Overrides.Find(SourceSMC->GetFName()) : nullptr;
				if (Override && Override->bVisible)
				{
					if (Override->StaticMeshOverride)
					{
						Descriptor.StaticMesh = Override->StaticMeshOverride;
					}
					switch (Override->TransformModification)
					{
					case EVisualStateTransformModification::Override:
						Descriptor.LocalTransform = Override->TransformToApply;
						break;
					case EVisualStateTransformModification::ApplyRelative:
						Descriptor.LocalTransform = Override->TransformToApply * Descriptor.LocalTransform;
						break;
					case EVisualStateTransformModification::None:
					default:
						break;
					}
				}

				// The copied descriptor cached state 0's hash.
				Descriptor.ComputeHash();
			}

			InstancedActorData->AddVisualization(Desc);
		}
	}

	// All visualizations (including state 0) now have their ISM components.
	InstancedActorData->ForEachVisualization([this, InstancedActorData](uint8 VizIndex, const FInstancedActorsVisualizationInfo& VizInfo)
	{
		ConfigureVisualizationISMComponents(*InstancedActorData, VizInfo);
		return true;
	});
}

void UExampleInstancedActorComponent::ConfigureVisualizationISMComponents(UInstancedActorsData& InstanceData, const FInstancedActorsVisualizationInfo& Visualization) const
{
	AExampleInstancedActorsManager* Manager = Cast<AExampleInstancedActorsManager>(InstanceData.GetManager());
	if (bSpin && !Manager)
	{
		UE_LOG(LogPersistenceExamples, Warning, TEXT("%s: bSpin requires the IAD's manager to be an AExampleInstancedActorsManager. Dehydrated instances won't spin."), *InstanceData.GetName());
	}

	const TArray<FISMComponentDescriptor>& Descriptors = Visualization.VisualizationDesc.ISMComponentDescriptors;
	for (int32 Index = 0; Index < Visualization.ISMComponents.Num() && Index < Descriptors.Num(); ++Index)
	{
		UInstancedStaticMeshComponent* ISMComponent = Visualization.ISMComponents[Index];
		if (!ISMComponent)
		{
			continue;
		}

		// ISM components are created in descriptor order. The descriptor's source SMC lives on the exemplar actor, which
		// is alive while the entity template is created.
		const UStaticMeshComponent* SourceSMC = Descriptors[Index].GetStaticMeshComponent().Get();
		if (bSpin && Manager && SourceSMC && SourceSMC->GetFName() == SpinningComponentName)
		{
			Manager->RegisterSpinningISMComponent(ISMComponent, SpinDegreesPerSecond);
		}
		else if (!Descriptors[Index].bVisible)
		{
			// IA forces collision onto every ISM component; hidden instances shouldn't block anything.
			ISMComponent->bDisableCollision = true;
			ISMComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
}

void UExampleInstancedActorComponent::InitializeComponentForInstance(FInstancedActorsInstanceHandle InInstanceHandle)
{
	Super::InitializeComponentForInstance(InInstanceHandle);

	AActor* Owner = GetOwner();
	UInstancedActorsData* InstanceData = InInstanceHandle.GetInstanceActorData();
	if (!Owner || !InstanceData)
	{
		return;
	}

	UMassEntitySubsystem* EntitySubsystem = GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	if (!EntitySubsystem)
	{
		return;
	}

	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();
	const FMassEntityHandle EntityHandle = GetMassEntityHandle();
	if (!EntityManager.IsEntityValid(EntityHandle))
	{
		return;
	}

	// Pick up the stage (and its countdown) from the entity. The timer is (re)started in BeginPlay.
	if (const FExampleStageFragment* StageFragment = EntityManager.GetFragmentDataPtr<FExampleStageFragment>(EntityHandle))
	{
		CurrentStage = StageFragment->Stage;
		StageEndTime = StageFragment->StageEndTime;
	}

	// Restore visual state by mapping the entity's current StaticMeshDescHandle back to a state index.
	const FMassRepresentationFragment& RepFrag = EntityManager.GetFragmentDataChecked<FMassRepresentationFragment>(EntityHandle);
	const FStaticMeshInstanceVisualizationDescHandle CurrentHandle = RepFrag.StaticMeshDescHandle;

	int32 VisualStateIndex = 0;
	InstanceData->ForEachVisualization([&](uint8 VizIndex, const FInstancedActorsVisualizationInfo& VizInfo)
	{
		if (VizInfo.MassStaticMeshDescHandle == CurrentHandle)
		{
			VisualStateIndex = static_cast<int32>(VizIndex);
			return false;
		}
		return true;
	});

	if (bApplyVisualStatesToOwner)
	{
		ApplyVisualStateToOwner(VisualStateIndex);
	}

	if (Owner->Implements<UExampleVisualStateRestorable>())
	{
		IExampleVisualStateRestorable::Execute_OnVisualStateRestored(Owner, VisualStateIndex);
	}

	if (bSupportsHealth)
	{
		if (UHealthAttributeSet* HealthSet = ExampleIAC::FindOwnerHealthSet(Owner))
		{
			if (const FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
			{
				HealthSet->SetMaxHealth(HealthFrag->Max);
				HealthSet->SetHealth(HealthFrag->Current);
			}
		}

		// When the persistence world subsystem is about to save, ensure that a hydrated actor's health is flushed back to the Mass entity fragment.
		// Then, SerializeInstancePersistenceData will write those values into a binary blob for the owning AInstancedActorsManager which manages
		// this instance. For dehydrated actors, the Mass fragment is already updated on EndPlay of this component. Alternatively, you could sync
		// the health values on every change.
		if (UPersistenceWorldSubsystem* PersistenceSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UPersistenceWorldSubsystem>() : nullptr)
		{
			PreIAMFlushHandle = PersistenceSubsystem->OnPreFlushInstancedActorsData.AddLambda([this]()
			{
				if (UHealthAttributeSet* HealthSet = ExampleIAC::FindOwnerHealthSet(GetOwner()))
				{
					NotifyHealthChanged(HealthSet->GetHealth(), HealthSet->GetMaxHealth());
				}
			});
		}
	}
}

void UExampleInstancedActorComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bSpin && !IsNetMode(NM_DedicatedServer))
	{
		TArray<USceneComponent*> SceneComponents;
		GetOwner()->GetComponents<USceneComponent>(SceneComponents);
		USceneComponent* const* Found = SceneComponents.FindByPredicate([this](const USceneComponent* Component) { return Component->GetFName() == SpinningComponentName; });
		if (Found && *Found != GetOwner()->GetRootComponent())
		{
			SpinningComponent = *Found;
			SpinningComponent->SetMobility(EComponentMobility::Movable);
			SetComponentTickEnabled(true);
		}
		else
		{
			UE_LOG(LogPersistenceExamples, Warning, TEXT("%s: SpinningComponentName '%s' doesn't name a non-root component of %s."),
				*GetName(), *SpinningComponentName.ToString(), *GetNameSafe(GetOwner()));
		}
	}

	if (bUseStageTimers)
	{
		if (GetOwner()->HasAuthority())
		{
			ScheduleStageTimer();
		}

		// Deferred so that bindings made in the owner's BeginPlay (which runs after component BeginPlay) receive the initial stage.
		GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			OnStageChanged.Broadcast(CurrentStage);
		}));
	}
}

void UExampleInstancedActorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The stage needs no flush: every change is written through to the entity's FExampleStageFragment immediately, and
	// UExampleStageTimerProcessor takes over the countdown once the actor is gone.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}

	// Last chance to flush actor-authoritative health back to the entity fragment before despawn.
	if (bSupportsHealth && HasMassEntity())
	{
		if (UHealthAttributeSet* HealthSet = ExampleIAC::FindOwnerHealthSet(GetOwner()))
		{
			UMassEntitySubsystem* EntitySubsystem = GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
			if (EntitySubsystem)
			{
				FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();
				const FMassEntityHandle EntityHandle = GetMassEntityHandle();
				if (EntityManager.IsEntityValid(EntityHandle))
				{
					if (FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
					{
						HealthFrag->Current = HealthSet->GetHealth();
						HealthFrag->Max = HealthSet->GetMaxHealth();
					}
				}
			}
		}
	}

	if (PreIAMFlushHandle.IsValid())
	{
		if (UPersistenceWorldSubsystem* PersistenceSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UPersistenceWorldSubsystem>() : nullptr)
		{
			PersistenceSubsystem->OnPreFlushInstancedActorsData.Remove(PreIAMFlushHandle);
		}
	}

	Super::EndPlay(EndPlayReason);
}

void UExampleInstancedActorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Same function of time as the dehydrated ISM instances (AExampleInstancedActorsManager::Tick), so hydration and
	// dehydration are seamless. World rotation, so it matches regardless of how the actor was placed.
	if (SpinningComponent)
	{
		SpinningComponent->SetWorldRotation(ExampleIAStages::SpinRotation(ExampleIAStages::GetSpinTime(GetWorld()), SpinDegreesPerSecond));
	}
}

uint32 UExampleInstancedActorComponent::GetInstancePersistenceDataID() const
{
	return ExampleIAC::PersistenceDataID;
}

bool UExampleInstancedActorComponent::ShouldSerializeInstancePersistenceData(const FArchive& Archive, UInstancedActorsData* InstanceData, int64 TimeDelta) const
{
	return InstanceData != nullptr;
}

void UExampleInstancedActorComponent::SerializeInstancePersistenceData(FStructuredArchive::FRecord Record, UInstancedActorsData* InstanceData, int64 TimeDelta) const
{
	FArchive& Ar = Record.GetUnderlyingArchive();

	// Versioning via the archive's custom-version container instead of a hand-rolled byte. On a saving
	// archive UsingCustomVersion copies LatestVersion from the process-global registry into the container;
	// on a loading archive it's a no-op and CustomVer returns the version that was saved (the transport,
	// UPersistenceWorldSubsystem, persists the FCustomVersionContainer alongside this blob and restores it
	// onto the reader before Serialize runs). See FExampleInstancedActorVersion.
	Ar.UsingCustomVersion(FExampleInstancedActorVersion::GUID);
	const int32 Version = Ar.CustomVer(FExampleInstancedActorVersion::GUID);

	// On load, a record older than our first versioned layout can't be trusted; warn (the archive seek
	// past this record's body is handled by the IAM caller via the IAC size header).
	if (Ar.IsLoading() && Version < FExampleInstancedActorVersion::InitialVersion)
	{
		UE_LOG(LogPersistenceExamples, Warning,
			TEXT("ExampleInstancedActorComponent: unsupported persistence version %d (expected >= %d) — skipping record."),
			Version, static_cast<int32>(FExampleInstancedActorVersion::InitialVersion));
	}

	// Save-time: gather sparse overrides vs. the IAD's defaults (viz 0, health 100/100, stage 0).
	TArray<TPair<FInstancedActorsInstanceIndex, uint8>> VizOverrides;
	TArray<TTuple<FInstancedActorsInstanceIndex, float, float>> HealthOverrides;
	TArray<TTuple<FInstancedActorsInstanceIndex, uint8, double>> StageOverrides;

	if (Ar.IsSaving())
	{
		AInstancedActorsManager* Manager = InstanceData ? InstanceData->GetTypedOuter<AInstancedActorsManager>() : nullptr;
		TSharedPtr<FMassEntityManager> EntityManagerPtr = Manager ? Manager->GetMassEntityManager() : nullptr;
		if (EntityManagerPtr && InstanceData->HasSpawnedEntities())
		{
			const FMassEntityManager& EntityManager = *EntityManagerPtr;
			const double Now = InstanceData->GetWorld()->GetTimeSeconds();
			for (int32 i = 0; i < static_cast<int32>(InstanceData->NumValidInstances); ++i)
			{
				const FInstancedActorsInstanceIndex InstanceIndex(i);
				const FMassEntityHandle EntityHandle = InstanceData->GetEntity(InstanceIndex);
				if (!EntityManager.IsEntityValid(EntityHandle))
				{
					continue;
				}

				// Stage timer classes save their stage + remaining time instead; their visualization follows the stage.
				if (const FExampleStageFragment* StageFrag = EntityManager.GetFragmentDataPtr<FExampleStageFragment>(EntityHandle))
				{
					if (StageFrag->Stage != 0)
					{
						StageOverrides.Emplace(InstanceIndex, StageFrag->Stage, FMath::Max(StageFrag->StageEndTime - Now, 0.0));
					}
				}
				else
				{
					const FMassRepresentationFragment& RepFrag = EntityManager.GetFragmentDataChecked<FMassRepresentationFragment>(EntityHandle);
					uint8 VizIndex = 0;
					InstanceData->ForEachVisualization([&](uint8 Idx, const FInstancedActorsVisualizationInfo& VizInfo)
					{
						if (VizInfo.MassStaticMeshDescHandle == RepFrag.StaticMeshDescHandle)
						{
							VizIndex = Idx;
							return false;
						}
						return true;
					});

					if (VizIndex != 0)
					{
						VizOverrides.Emplace(InstanceIndex, VizIndex);
					}
				}

				if (bSupportsHealth)
				{
					if (const FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
					{
						if (HealthFrag->Current != 100.f || HealthFrag->Max != 100.f)
						{
							HealthOverrides.Emplace(InstanceIndex, HealthFrag->Current, HealthFrag->Max);
						}
					}
				}
			}
		}
	}

	int32 NumVizOverrides = VizOverrides.Num();
	FStructuredArchive::FArray VizArray = Record.EnterArray(TEXT("VisualizationOverrides"), NumVizOverrides);
	if (Ar.IsSaving())
	{
		for (TPair<FInstancedActorsInstanceIndex, uint8>& Override : VizOverrides)
		{
			FStructuredArchive::FRecord Entry = VizArray.EnterElement().EnterRecord();
			Entry << SA_VALUE(TEXT("InstanceIndex"), Override.Key);
			Entry << SA_VALUE(TEXT("VizIndex"), Override.Value);
		}
	}
	else
	{
		UExampleInstancedActorsData* ExampleData = Cast<UExampleInstancedActorsData>(InstanceData);
		for (int32 j = 0; j < NumVizOverrides; ++j)
		{
			FStructuredArchive::FRecord Entry = VizArray.EnterElement().EnterRecord();
			FInstancedActorsInstanceIndex InstanceIndex;
			uint8 VizIndex = 0;
			Entry << SA_VALUE(TEXT("InstanceIndex"), InstanceIndex);
			Entry << SA_VALUE(TEXT("VizIndex"), VizIndex);
			// Phase 1: stage into the IAD rather than touching Mass - entities are not yet spawned at
			// restore time. Applied in UExampleInstancedActorsData::OnSpawnEntities.
			if (ExampleData)
			{
				ExampleData->StagePersistedVisualization(InstanceIndex.GetIndex(), VizIndex);
			}
		}
	}

	int32 NumHealthOverrides = HealthOverrides.Num();
	FStructuredArchive::FArray HealthArray = Record.EnterArray(TEXT("HealthOverrides"), NumHealthOverrides);
	if (Ar.IsSaving())
	{
		for (TTuple<FInstancedActorsInstanceIndex, float, float>& Override : HealthOverrides)
		{
			FStructuredArchive::FRecord Entry = HealthArray.EnterElement().EnterRecord();
			Entry << SA_VALUE(TEXT("InstanceIndex"), Override.Get<0>());
			Entry << SA_VALUE(TEXT("Health"), Override.Get<1>());
			Entry << SA_VALUE(TEXT("MaxHealth"), Override.Get<2>());
		}
	}
	else
	{
		UExampleInstancedActorsData* ExampleData = Cast<UExampleInstancedActorsData>(InstanceData);
		for (int32 j = 0; j < NumHealthOverrides; ++j)
		{
			FStructuredArchive::FRecord Entry = HealthArray.EnterElement().EnterRecord();
			FInstancedActorsInstanceIndex InstanceIndex;
			float Health = 100.f;
			float MaxHealth = 100.f;
			Entry << SA_VALUE(TEXT("InstanceIndex"), InstanceIndex);
			Entry << SA_VALUE(TEXT("Health"), Health);
			Entry << SA_VALUE(TEXT("MaxHealth"), MaxHealth);
			// Phase 1: stage into the IAD; applied to FHealthFragment in OnSpawnEntities once entities exist.
			if (ExampleData)
			{
				ExampleData->StagePersistedHealth(InstanceIndex.GetIndex(), Health, MaxHealth);
			}
		}
	}

	int32 NumStageOverrides = StageOverrides.Num();
	if (Version >= FExampleInstancedActorVersion::StageTimers)
	{
		FStructuredArchive::FArray StageArray = Record.EnterArray(TEXT("StageOverrides"), NumStageOverrides);
		if (Ar.IsSaving())
		{
			for (TTuple<FInstancedActorsInstanceIndex, uint8, double>& Override : StageOverrides)
			{
				FStructuredArchive::FRecord Entry = StageArray.EnterElement().EnterRecord();
				Entry << SA_VALUE(TEXT("InstanceIndex"), Override.Get<0>());
				Entry << SA_VALUE(TEXT("Stage"), Override.Get<1>());
				Entry << SA_VALUE(TEXT("RemainingSeconds"), Override.Get<2>());
			}
		}
		else
		{
			UExampleInstancedActorsData* ExampleData = Cast<UExampleInstancedActorsData>(InstanceData);
			for (int32 j = 0; j < NumStageOverrides; ++j)
			{
				FStructuredArchive::FRecord Entry = StageArray.EnterElement().EnterRecord();
				FInstancedActorsInstanceIndex InstanceIndex;
				uint8 Stage = 0;
				double RemainingSeconds = 0.0;
				Entry << SA_VALUE(TEXT("InstanceIndex"), InstanceIndex);
				Entry << SA_VALUE(TEXT("Stage"), Stage);
				Entry << SA_VALUE(TEXT("RemainingSeconds"), RemainingSeconds);
				// TimeDelta is the real time in seconds since this data was saved (AInstancedActorsManager::Serialize stores a
				// UTC timestamp), both for loading a save file and for a level streaming back in. Subtracting it lets the stage
				// countdown progress while the player was away; any excess carries over into further stages once applied in
				// UExampleInstancedActorsData::OnSpawnEntities.
				if (ExampleData)
				{
					ExampleData->StagePersistedStage(InstanceIndex.GetIndex(), Stage, RemainingSeconds - static_cast<double>(TimeDelta));
				}
			}
		}
	}

	UE_LOG(LogPersistenceExamples, Log,
		TEXT("ExampleIAC SerializeInstancePersistenceData [%s] Viz: %d override(s), Health: %d override(s), Stage: %d override(s)"),
		Ar.IsSaving() ? TEXT("Save") : TEXT("Load"), NumVizOverrides, NumHealthOverrides, NumStageOverrides);
}

void UExampleInstancedActorComponent::NotifyVisualStateChanged(int32 NewVisualState)
{
	if (!HasMassEntity())
	{
		return;
	}

	UInstancedActorsData* InstanceData = InstanceHandle.GetInstanceActorData();
	if (!InstanceData)
	{
		return;
	}

	// Switch the entity's visualization synchronously on server and clients alike. While this actor is hydrated, only the
	// entity's mesh handle changes, so the right ISM instance appears once the actor dehydrates. The engine's deferred
	// UInstancedActorsData::SwitchInstanceVisualization isn't used: its consumer processor is server-only, and it loses the
	// instance outside the Detailed bulk LOD. See ExampleIAStages::SwitchInstanceVisualization.
	const FInstancedActorsInstanceIndex InstanceIndex = InstanceHandle.GetInstanceIndex();
	ExampleIAStages::SwitchInstanceVisualization(*InstanceData, GetMassEntityManagerChecked(), GetMassEntityHandle(), static_cast<uint8>(NewVisualState));

	// Server: record the new visualization index as the instance's lifecycle phase. This is the producer
	// for late-joining-client replication: SetInstanceCurrentLifecyclePhase flushes net dormancy and writes
	// into the replicated InstanceDeltas FastArray, so the phase reaches ALL clients (incl. late joiners)
	// and survives this actor dehydrating. Convention: viz index == lifecycle phase index. The base
	// ApplyInstanceDelta ignores the phase on the server (it only acts on destruction), so this has no
	// server-side gameplay effect; UExampleInstancedActorsData::ApplyInstanceDelta consumes it client-side.
	ExampleIAStages::ReplicateStage(*InstanceData, InstanceIndex, static_cast<uint8>(NewVisualState));
}

void UExampleInstancedActorComponent::ApplyVisualStateToOwner(int32 VisualState)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// AdditionalVisualStates[K] configures visual state K+1; state 0 has no overrides (engine-built from the exemplar).
	const TMap<FName, FVisualStateMeshOverride>* Overrides = nullptr;
	const int32 EntryIndex = VisualState - 1;
	if (VisualDescriptionGenerationType == EVisualDescriptionGenerationType::PropertyConfigured && AdditionalVisualStates.IsValidIndex(EntryIndex))
	{
		Overrides = &AdditionalVisualStates[EntryIndex].Overrides;
	}

	// Initial values are read from each SMC's archetype (the CDO's same-named default subobject for native components,
	// the SCS template for BP-added components) so ApplyRelative always composes against the base, and no-override /
	// state 0 restores cleanly.
	TArray<UStaticMeshComponent*> SMCs;
	Owner->GetComponents<UStaticMeshComponent>(SMCs);
	for (UStaticMeshComponent* SMC : SMCs)
	{
		if (!SMC)
		{
			continue;
		}
		// For the root component RelativeTransform is the actor's world transform — touching it would
		// teleport the actor. Apply mesh/visibility to the root, but skip transform changes.
		const bool bIsRoot = (SMC == Owner->GetRootComponent());
		const UStaticMeshComponent* Archetype = Cast<UStaticMeshComponent>(SMC->GetArchetype());
		const FTransform InitialTransform = Archetype ? Archetype->GetRelativeTransform() : FTransform::Identity;
		const bool bArchetypeVisible = Archetype ? Archetype->IsVisible() : true;
		const FVisualStateMeshOverride* Override = Overrides ? Overrides->Find(SMC->GetFName()) : nullptr;

		if (Override)
		{
			SMC->SetVisibility(Override->bVisible);
			if (!Override->bVisible)
			{
				continue;
			}
			if (Override->StaticMeshOverride)
			{
				SMC->SetStaticMesh(Override->StaticMeshOverride);
			}
			if (bIsRoot)
			{
				continue;
			}
			switch (Override->TransformModification)
			{
			case EVisualStateTransformModification::Override:
				SMC->SetRelativeTransform(Override->TransformToApply);
				break;
			case EVisualStateTransformModification::ApplyRelative:
				SMC->SetRelativeTransform(Override->TransformToApply * InitialTransform);
				break;
			case EVisualStateTransformModification::None:
			default:
				SMC->SetRelativeTransform(InitialTransform);
				break;
			}
		}
		else
		{
			// No override at this state — restore archetype mesh + visibility + transform in case a prior
			// state modified them. Skip transform on the root for the same reason as above.
			if (Archetype)
			{
				SMC->SetStaticMesh(Archetype->GetStaticMesh());
			}
			SMC->SetVisibility(bArchetypeVisible);
			if (!bIsRoot)
			{
				SMC->SetRelativeTransform(InitialTransform);
			}
		}
	}
}

void UExampleInstancedActorComponent::NotifyHealthChanged(float NewHealth, float NewMaxHealth)
{
	if (!HasMassEntity())
	{
		return;
	}

	UMassEntitySubsystem* EntitySubsystem = GetWorld() ? GetWorld()->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	if (!EntitySubsystem)
	{
		return;
	}

	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();
	const FMassEntityHandle EntityHandle = GetMassEntityHandle();
	if (!EntityManager.IsEntityValid(EntityHandle))
	{
		return;
	}

	if (FHealthFragment* HealthFrag = EntityManager.GetFragmentDataPtr<FHealthFragment>(EntityHandle))
	{
		HealthFrag->Current = NewHealth;
		HealthFrag->Max = NewMaxHealth;
	}
}

void UExampleInstancedActorComponent::DestroyInstance()
{
	if (HasMassEntity())
	{
		InstanceHandle.GetInstanceActorDataChecked().DestroyInstance(InstanceHandle.GetInstanceIndex());
	}
}

void UExampleInstancedActorComponent::EjectInstance()
{
	AActor* Owner = GetOwner();
	if (Owner && HasMassEntity())
	{
		InstanceHandle.GetInstanceActorDataChecked().EjectInstanceActor(InstanceHandle.GetInstanceIndex(), *Owner);
	}
}

int32 UExampleInstancedActorComponent::GetNumStages() const
{
	return VisualDescriptionGenerationType == EVisualDescriptionGenerationType::PropertyConfigured ? AdditionalVisualStates.Num() + 1 : 1;
}

TArray<float> UExampleInstancedActorComponent::GetStageDurations() const
{
	// Index 0 (the resting stage) has no duration.
	TArray<float> Durations;
	Durations.Add(0.f);
	if (VisualDescriptionGenerationType == EVisualDescriptionGenerationType::PropertyConfigured)
	{
		for (const FVisualStateSMCOverrides& State : AdditionalVisualStates)
		{
			Durations.Add(State.StageDuration);
		}
	}
	return Durations;
}

FExampleStageFragment* UExampleInstancedActorComponent::GetStageFragment() const
{
	if (!HasMassEntity())
	{
		return nullptr;
	}
	FMassEntityManager& EntityManager = GetMassEntityManagerChecked();
	const FMassEntityHandle EntityHandle = GetMassEntityHandle();
	return EntityManager.IsEntityValid(EntityHandle) ? EntityManager.GetFragmentDataPtr<FExampleStageFragment>(EntityHandle) : nullptr;
}

void UExampleInstancedActorComponent::SetStage(int32 NewStage)
{
	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		UE_LOG(LogPersistenceExamples, Warning, TEXT("%s: SetStage must be called on the server (e.g. from a server RPC)."), *GetPathName());
		return;
	}
	if (!bUseStageTimers)
	{
		UE_LOG(LogPersistenceExamples, Warning, TEXT("%s: SetStage requires bUseStageTimers."), *GetPathName());
		return;
	}

	const uint8 ClampedStage = static_cast<uint8>(FMath::Clamp(NewStage, 0, GetNumStages() - 1));
	StageEndTime = GetWorld()->GetTimeSeconds() + ExampleIAStages::GetStageDuration(GetStageDurations(), ClampedStage);
	CommitStage(ClampedStage);
}

void UExampleInstancedActorComponent::StartStageCycle()
{
	SetStage(GetNumStages() - 1);
}

void UExampleInstancedActorComponent::CommitStage(uint8 NewStage)
{
	const bool bChanged = NewStage != CurrentStage;
	CurrentStage = NewStage;

	// Write through to the entity, the source of truth that persistence reads and that UExampleStageTimerProcessor
	// continues from once this actor dehydrates.
	if (FExampleStageFragment* StageFragment = GetStageFragment())
	{
		StageFragment->Stage = NewStage;
		StageFragment->StageEndTime = StageEndTime;

		FMassCommandBuffer& CommandBuffer = GetMassEntityManagerChecked().Defer();
		if (NewStage > 0)
		{
			CommandBuffer.AddTag<FExampleStageTimerActiveTag>(GetMassEntityHandle());
		}
		else
		{
			CommandBuffer.RemoveTag<FExampleStageTimerActiveTag>(GetMassEntityHandle());
		}
	}

	// ISM visualization for when this actor dehydrates, and lifecycle phase replication for dehydrated instances on clients.
	NotifyVisualStateChanged(NewStage);

	if (bChanged)
	{
		NotifyOwnerOfStage();
	}

	ScheduleStageTimer();
}

void UExampleInstancedActorComponent::ScheduleStageTimer()
{
	FTimerManager& TimerManager = GetWorld()->GetTimerManager();
	TimerManager.ClearTimer(StageTimerHandle);
	if (CurrentStage > 0)
	{
		const double Delay = FMath::Max(StageEndTime - GetWorld()->GetTimeSeconds(), 0.01);
		TimerManager.SetTimer(StageTimerHandle, this, &UExampleInstancedActorComponent::OnStageTimerElapsed, static_cast<float>(Delay), /*bLoop*/ false);
	}
}

void UExampleInstancedActorComponent::OnStageTimerElapsed()
{
	// Same countdown as UExampleStageTimerProcessor uses for dehydrated instances.
	FExampleStageFragment Stage;
	Stage.Stage = CurrentStage;
	Stage.StageEndTime = StageEndTime;
	if (ExampleIAStages::AdvanceStages(Stage, GetStageDurations(), GetWorld()->GetTimeSeconds()))
	{
		StageEndTime = Stage.StageEndTime;
		CommitStage(Stage.Stage);
	}
	else
	{
		// Timer precision: fired marginally early.
		ScheduleStageTimer();
	}
}

void UExampleInstancedActorComponent::OnRep_CurrentStage()
{
	NotifyOwnerOfStage();
}

void UExampleInstancedActorComponent::NotifyOwnerOfStage()
{
	// Applied first, so owner callbacks see the stage's meshes already in place.
	if (bApplyVisualStatesToOwner)
	{
		ApplyVisualStateToOwner(CurrentStage);
	}

	if (AActor* Owner = GetOwner(); Owner && Owner->Implements<UExampleVisualStateRestorable>())
	{
		IExampleVisualStateRestorable::Execute_OnVisualStateRestored(Owner, CurrentStage);
	}
	OnStageChanged.Broadcast(CurrentStage);
}
