// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InstancedActorsIndex.h"
#include "Mass/EntityHandle.h"
#include "MassEntityTraitBase.h"
#include "MassEntityTypes.h"
#include "ExampleInstancedActorStages.generated.h"

class UInstancedActorsData;
struct FMassEntityManager;
struct FMassRepresentationFragment;
struct FMassRepresentationLODFragment;
struct FTransformFragment;

/**
 * Persistent stage of a single instanced actor, e.g. a regrowing pumpkin or a respawning health pack.
 *
 * Stage == visual state index (see UExampleInstancedActorComponent::AdditionalVisualStates). Stage 0 is the resting
 * state and has no timer. Gameplay sets a higher stage (usually the last one), after which the stage counts down
 * towards 0, spending StageDurations[Stage] seconds in each stage.
 *
 * This fragment is the single source of truth for the stage, whether the instance is hydrated (actor) or not:
 *  - Dehydrated: UExampleStageTimerProcessor advances it and swaps the ISM visualization.
 *  - Hydrated: UExampleInstancedActorComponent runs a timer and writes every change through to this fragment.
 * Since StageEndTime is absolute world time, no state needs to be handed over on hydration / dehydration.
 */
USTRUCT()
struct PERSISTENCEEXAMPLES_API FExampleStageFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "InstancedActors|Stages")
	uint8 Stage = 0;

	// World time (UWorld::GetTimeSeconds) at which Stage advances to Stage - 1. Ignored when Stage == 0.
	UPROPERTY(EditAnywhere, Category = "InstancedActors|Stages")
	double StageEndTime = 0.0;
};

// Present only while an instance's stage is counting down (Stage > 0), so idle instances cost nothing to process.
USTRUCT()
struct PERSISTENCEEXAMPLES_API FExampleStageTimerActiveTag : public FMassTag
{
	GENERATED_BODY()
};

// Per actor class stage configuration, shared by all instances of that class.
USTRUCT()
struct PERSISTENCEEXAMPLES_API FExampleStageConfigSharedFragment : public FMassConstSharedFragment
{
	GENERATED_BODY()

	// StageDurations[K] = seconds spent in stage K before advancing to stage K - 1. Index 0 (resting) is unused.
	UPROPERTY(EditAnywhere, Category = "InstancedActors|Stages")
	TArray<float> StageDurations;
};

// Adds FExampleStageFragment + FExampleStageConfigSharedFragment. Added by UExampleInstancedActorComponent::ModifyMassEntityConfig.
UCLASS(meta = (DisplayName = "Example Stages"))
class PERSISTENCEEXAMPLES_API UExampleStagesTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "InstancedActors|Stages")
	TArray<float> StageDurations;

protected:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override;
};

namespace ExampleIAStages
{
	// Returns how long Stage lasts, or 0 if Stage has no configured duration.
	PERSISTENCEEXAMPLES_API float GetStageDuration(TConstArrayView<float> StageDurations, int32 Stage);

	/**
	 * Advances InOutStage towards 0 for as long as Now has passed its StageEndTime. Each next stage's end time is
	 * accumulated from the previous end time rather than from Now, so excess time carries over into subsequent stages
	 * (hitches, or game time elapsed while a level was unloaded). Returns true if the stage changed.
	 */
	PERSISTENCEEXAMPLES_API bool AdvanceStages(FExampleStageFragment& InOutStage, TConstArrayView<float> StageDurations, double Now);

	// Rotation of spinning items as a pure function of time, identical for all instances and for the hydrated actor.
	PERSISTENCEEXAMPLES_API FQuat SpinRotation(double TimeSeconds, float DegreesPerSecond);

	// Time used for spinning. Uses the server's world time so all clients agree.
	PERSISTENCEEXAMPLES_API double GetSpinTime(const UWorld* World);

	/**
	 * Switches an instance's ISM visualization immediately, in every IA bulk LOD.
	 *
	 * UInstancedActorsData::SwitchInstanceVisualization isn't used because its deferred path loses the instance when the
	 * IAD's bulk LOD is Medium / Low: UInstancedActorsVisualizationSwitcherProcessor removes the old ISM instance, but the
	 * UMassStationaryISMSwitcherProcessor that would add the new one requires a detailed-LOD tag, so the instance stays
	 * invisible until it hydrates again. It also races UInstancedActorsVisualizationProcessor in the Detailed bulk LOD.
	 * Instead, this removes the old instance and adds the new one itself, leaving the representation fragment in a
	 * consistent state for all subsequent bulk LOD changes and hydration / dehydration.
	 *
	 * Must be called on the game thread. Safe both inside game thread Mass processors and outside Mass processing.
	 */
	PERSISTENCEEXAMPLES_API void SwitchInstanceVisualization(UInstancedActorsData& InstanceData, FMassEntityHandle EntityHandle, uint8 VisualizationIndex,
		FMassRepresentationFragment& Representation, const FMassRepresentationLODFragment& RepresentationLOD, const FTransformFragment& Transform);

	// Convenience overload that looks up the entity's fragments.
	PERSISTENCEEXAMPLES_API void SwitchInstanceVisualization(UInstancedActorsData& InstanceData, FMassEntityManager& EntityManager, FMassEntityHandle EntityHandle, uint8 VisualizationIndex);

	// Server: replicates the stage to clients through the IAD's lifecycle phase delta. Stage 0 (the default) removes the
	// phase delta instead, keeping the replicated delta list sparse. Clients then revert to visualization 0 in
	// UExampleInstancedActorsData::RollbackInstanceDelta / ApplyInstanceDelta.
	PERSISTENCEEXAMPLES_API void ReplicateStage(UInstancedActorsData& InstanceData, FInstancedActorsInstanceIndex InstanceIndex, uint8 Stage);
}
