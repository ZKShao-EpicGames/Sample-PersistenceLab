// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstancedActors/ExampleStageTimerProcessor.h"
#include "InstancedActors/ExampleInstancedActorStages.h"
#include "InstancedActorsData.h"
#include "InstancedActorsTypes.h"
#include "MassActorSubsystem.h"
#include "MassCommonFragments.h"
#include "MassExecutionContext.h"
#include "MassRepresentationFragments.h"

UExampleStageTimerProcessor::UExampleStageTimerProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ProcessingPhase = EMassProcessingPhase::PrePhysics;
	// Stages are server-authoritative. Clients learn about stage changes through replication.
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Server | EProcessorExecutionFlags::Standalone);
	bRequiresGameThreadExecution = true;
}

void UExampleStageTimerProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.Initialize(EntityManager);
	EntityQuery.AddRequirement<FExampleStageFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FInstancedActorsFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassActorFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassRepresentationFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FMassRepresentationLODFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddConstSharedRequirement<FExampleStageConfigSharedFragment>();
	EntityQuery.AddTagRequirement<FExampleStageTimerActiveTag>(EMassFragmentPresence::All);
	EntityQuery.RegisterWithProcessor(*this);
}

void UExampleStageTimerProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	const UWorld* World = EntityManager.GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	EntityQuery.ForEachEntityChunk(Context, [Now](FMassExecutionContext& Ctx)
	{
		const TArrayView<FExampleStageFragment> StageList = Ctx.GetMutableFragmentView<FExampleStageFragment>();
		const TConstArrayView<FInstancedActorsFragment> InstanceList = Ctx.GetFragmentView<FInstancedActorsFragment>();
		const TConstArrayView<FMassActorFragment> ActorList = Ctx.GetFragmentView<FMassActorFragment>();
		const TArrayView<FMassRepresentationFragment> RepresentationList = Ctx.GetMutableFragmentView<FMassRepresentationFragment>();
		const TConstArrayView<FMassRepresentationLODFragment> RepresentationLODList = Ctx.GetFragmentView<FMassRepresentationLODFragment>();
		const TConstArrayView<FTransformFragment> TransformList = Ctx.GetFragmentView<FTransformFragment>();
		const FExampleStageConfigSharedFragment& Config = Ctx.GetConstSharedFragment<FExampleStageConfigSharedFragment>();

		for (FMassExecutionContext::FEntityIterator EntityIt = Ctx.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			// While hydrated, the actor's UExampleInstancedActorComponent drives the countdown.
			if (ActorList[EntityIt].IsValid())
			{
				continue;
			}

			FExampleStageFragment& Stage = StageList[EntityIt];
			const FMassEntityHandle EntityHandle = Ctx.GetEntity(EntityIt);
			if (Stage.Stage == 0)
			{
				Ctx.Defer().RemoveTag<FExampleStageTimerActiveTag>(EntityHandle);
				continue;
			}

			if (!ExampleIAStages::AdvanceStages(Stage, Config.StageDurations, Now))
			{
				continue;
			}

			UInstancedActorsData* InstanceData = InstanceList[EntityIt].InstanceData.Get();
			if (!InstanceData)
			{
				continue;
			}

			ExampleIAStages::SwitchInstanceVisualization(*InstanceData, EntityHandle, Stage.Stage,
				RepresentationList[EntityIt], RepresentationLODList[EntityIt], TransformList[EntityIt]);
			ExampleIAStages::ReplicateStage(*InstanceData, InstanceList[EntityIt].InstanceIndex, Stage.Stage);

			if (Stage.Stage == 0)
			{
				Ctx.Defer().RemoveTag<FExampleStageTimerActiveTag>(EntityHandle);
			}
		}
	});
}
