// Copyright Epic Games, Inc. All Rights Reserved.

#include "InstancedActors/ExampleInstancedActorStages.h"
#include "GameFramework/GameStateBase.h"
#include "InstancedActorsData.h"
#include "InstancedActorsManager.h"
#include "InstancedActorsRepresentationSubsystem.h"
#include "InstancedActorsTypes.h"
#include "InstancedActorsVisualizationSwitcherProcessor.h"
#include "MassCommonFragments.h"
#include "MassEntityManager.h"
#include "MassEntityTemplateRegistry.h"
#include "MassEntityUtils.h"
#include "MassRepresentationFragments.h"

void UExampleStagesTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const
{
	FMassEntityManager& EntityManager = UE::Mass::Utils::GetEntityManagerChecked(World);

	BuildContext.AddFragment<FExampleStageFragment>();

	FExampleStageConfigSharedFragment Config;
	Config.StageDurations = StageDurations;
	BuildContext.AddConstSharedFragment(EntityManager.GetOrCreateConstSharedFragment(Config));
}

namespace ExampleIAStages
{
	float GetStageDuration(TConstArrayView<float> StageDurations, int32 Stage)
	{
		return StageDurations.IsValidIndex(Stage) ? FMath::Max(StageDurations[Stage], 0.f) : 0.f;
	}

	bool AdvanceStages(FExampleStageFragment& InOutStage, TConstArrayView<float> StageDurations, double Now)
	{
		bool bChanged = false;
		while (InOutStage.Stage > 0 && Now >= InOutStage.StageEndTime)
		{
			--InOutStage.Stage;
			InOutStage.StageEndTime += GetStageDuration(StageDurations, InOutStage.Stage);
			bChanged = true;
		}
		return bChanged;
	}

	FQuat SpinRotation(double TimeSeconds, float DegreesPerSecond)
	{
		const double Degrees = FMath::Fmod(TimeSeconds * DegreesPerSecond, 360.0);
		return FQuat(FVector::UpVector, FMath::DegreesToRadians(Degrees));
	}

	double GetSpinTime(const UWorld* World)
	{
		if (!World)
		{
			return 0.0;
		}
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return GameState->GetServerWorldTimeSeconds();
		}
		return World->GetTimeSeconds();
	}

	void SwitchInstanceVisualization(UInstancedActorsData& InstanceData, FMassEntityHandle EntityHandle, uint8 VisualizationIndex,
		FMassRepresentationFragment& Representation, const FMassRepresentationLODFragment& RepresentationLOD, const FTransformFragment& Transform)
	{
		const FInstancedActorsVisualizationInfo* Visualization = InstanceData.GetVisualization(VisualizationIndex);
		UWorld* World = InstanceData.GetWorld();
		UInstancedActorsRepresentationSubsystem* RepresentationSubsystem = World ? World->GetSubsystem<UInstancedActorsRepresentationSubsystem>() : nullptr;
		if (!Visualization || Visualization->IsAsyncLoading() || !RepresentationSubsystem || Representation.bIsPendingDestruction)
		{
			return;
		}

		const FStaticMeshInstanceVisualizationDescHandle NewHandle = Visualization->MassStaticMeshDescHandle;
		FMassInstancedStaticMeshInfoArrayView ISMInfos = RepresentationSubsystem->GetMutableInstancedStaticMeshInfos();
		if (NewHandle == Representation.StaticMeshDescHandle || !ISMInfos.IsValidIndex(NewHandle.ToIndex()))
		{
			return;
		}

		// Only re-add when the entity is settled as an ISM instance. In any other state (no representation, hydrated actor,
		// or a representation transition in progress), swapping the handle is enough: the engine's next add uses NewHandle.
		const bool bIsSettledISMInstance = Representation.PrevRepresentation == EMassRepresentationType::StaticMeshInstance
			&& Representation.CurrentRepresentation == EMassRepresentationType::StaticMeshInstance;

		// Removes the instance from the old ISM (if any) and sets PrevRepresentation = None.
		UInstancedActorsVisualizationSwitcherProcessor::SwitchEntityMeshDesc(ISMInfos, Representation, EntityHandle, NewHandle);

		if (bIsSettledISMInstance)
		{
			// Mirrors the add in UMassStationaryISMSwitcherProcessor, which won't run for this entity outside the Detailed bulk LOD.
			FMassInstancedStaticMeshInfo& ISMInfo = ISMInfos[NewHandle.ToIndex()];
			if (FMassLODSignificanceRange* Range = ISMInfo.GetLODSignificanceRange(RepresentationLOD.LODSignificance))
			{
				const FTransform& EntityTransform = Transform.GetTransform();
				Range->AddInstance(EntityHandle, ISMInfo.ShouldUseTransformOffset() ? ISMInfo.GetTransformOffset() * EntityTransform : EntityTransform);
				if (ISMInfo.GetDesc().CustomDataFloats.Num() > 0)
				{
					Range->AddBatchedCustomDataFloats(ISMInfo.GetDesc().CustomDataFloats, {});
				}
			}

			// Back to a consistent "remain ISM" state so no processor adds the instance a second time.
			Representation.PrevLODSignificance = RepresentationLOD.LODSignificance;
			Representation.PrevRepresentation = EMassRepresentationType::StaticMeshInstance;
		}
	}

	void SwitchInstanceVisualization(UInstancedActorsData& InstanceData, FMassEntityManager& EntityManager, FMassEntityHandle EntityHandle, uint8 VisualizationIndex)
	{
		if (!EntityManager.IsEntityValid(EntityHandle))
		{
			return;
		}

		FMassRepresentationFragment* Representation = EntityManager.GetFragmentDataPtr<FMassRepresentationFragment>(EntityHandle);
		const FMassRepresentationLODFragment* RepresentationLOD = EntityManager.GetFragmentDataPtr<FMassRepresentationLODFragment>(EntityHandle);
		const FTransformFragment* Transform = EntityManager.GetFragmentDataPtr<FTransformFragment>(EntityHandle);
		if (Representation && RepresentationLOD && Transform)
		{
			SwitchInstanceVisualization(InstanceData, EntityHandle, VisualizationIndex, *Representation, *RepresentationLOD, *Transform);
		}
	}

	void ReplicateStage(UInstancedActorsData& InstanceData, FInstancedActorsInstanceIndex InstanceIndex, uint8 Stage)
	{
		// SetInstanceCurrentLifecyclePhase asserts on authority.
		const AInstancedActorsManager* Manager = InstanceData.GetManager();
		if (Manager && Manager->HasAuthority())
		{
			InstanceData.SetInstanceCurrentLifecyclePhase(InstanceIndex, Stage);
		}
	}
}
