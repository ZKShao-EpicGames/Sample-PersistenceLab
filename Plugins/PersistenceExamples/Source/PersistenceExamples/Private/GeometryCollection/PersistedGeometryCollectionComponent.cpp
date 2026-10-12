// Copyright Epic Games, Inc. All Rights Reserved.

#include "GeometryCollection/PersistedGeometryCollectionComponent.h"
#include "GeometryCollectionProxyData.h"
#include "PersistenceExamples.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"

void UPersistedGeometryCollectionComponent::PrePersistObject_Implementation()
{
	PersistedState.PieceTransforms.Reset();
	PersistedState.BrokenIndices.Reset();

	const FGeometryDynamicCollection* DC = GetDynamicCollection();
	if (!DC || !RestCollection)
	{
		UE_LOG(LogPersistenceExamples, Log, TEXT("[%s] PrePersistObject: no DC/RestCollection, nothing to save"),
			*GetNameSafe(GetOwner()));
		return;
	}

	const int32 NumTransforms = DC->GetNumTransforms();
	PersistedState.PieceTransforms.Reserve(NumTransforms);
	for (int32 i = 0; i < NumTransforms; ++i)
	{
		PersistedState.PieceTransforms.Emplace(FTransform(DC->GetTransform(i)));
	}

	const TManagedArray<int32>& RestParents = GetParentArrayRest();
	const int32 NumRestParents = RestParents.Num();
	for (int32 i = 0; i < NumRestParents; ++i)
	{
		if (GetParent(i) != RestParents[i])
		{
			PersistedState.BrokenIndices.Add(i);
		}
	}

	UE_LOG(LogPersistenceExamples, Log, TEXT("[%s] PrePersistObject: captured %d piece transforms, %d broken indices"),
		*GetNameSafe(GetOwner()), PersistedState.PieceTransforms.Num(), PersistedState.BrokenIndices.Num());
}

void UPersistedGeometryCollectionComponent::PostRestoreObject_Implementation(const TArray<FName>& RestoredPropertyNames)
{
	if (PersistedState.PieceTransforms.Num() == 0)
	{
		return;
	}

	const FGeometryDynamicCollection* DC = GetDynamicCollection();
	const int32 ExpectedCount = DC ? DC->GetNumTransforms() : 0;
	if (PersistedState.PieceTransforms.Num() != ExpectedCount)
	{
		UE_LOG(LogPersistenceExamples, Warning,
			TEXT("[%s] PostRestoreObject: saved transform count (%d) != DC count (%d); skipping restore (asset re-fractured?)"),
			*GetNameSafe(GetOwner()), PersistedState.PieceTransforms.Num(), ExpectedCount);
		return;
	}

	// Restore per-piece transforms and the per-piece broken state into the GT dynamic collection,
	// then rebuild the physics proxy from it.
	TArray<FTransform> Transforms = PersistedState.PieceTransforms;
	SetRestState(MoveTemp(Transforms));

	if (PersistedState.BrokenIndices.Num() > 0)
	{
		SetInitialClusterBreaks(PersistedState.BrokenIndices);
	}

	RecreatePhysicsState();

	// Push the same Transform + HasParent state into the proxy's PhysicsThreadCollection. The
	// engine's CopyMatchingAttributesFrom won't carry Transform across to PT (PT never registers
	// the attribute), so without this PT particles spawn at asset positions and clobber DC on
	// first sync.
	if (FGeometryCollectionPhysicsProxy* Proxy = GetPhysicsProxy())
	{
		FGeometryDynamicCollection& PT = Proxy->GetPhysicsCollection();
		const int32 NumTransforms = FMath::Min(PT.GetNumTransforms(), PersistedState.PieceTransforms.Num());
		for (int32 i = 0; i < NumTransforms; ++i)
		{
			PT.SetTransform(i, FTransform3f(PersistedState.PieceTransforms[i]));
		}
		for (int32 BrokenIdx : PersistedState.BrokenIndices)
		{
			if (BrokenIdx >= 0 && BrokenIdx < PT.GetNumTransforms())
			{
				PT.SetHasParent(BrokenIdx, false);
			}
		}
	}

	// Force the custom renderer into the broken branch. Without this it falls back to
	// RestCollection->RootProxyData (the intact-crate static mesh) because IsRootBroken() is
	// derived from !Active[RootIndex], and proxy init sets Active[root]=true (root has no
	// authored parent, so HasParent[root] is always false). Also stomp Active[root]=false so
	// any other consumer of IsRootBroken() sees a consistent broken state.
	if (FGeometryDynamicCollection* MutableDC = GetDynamicCollection())
	{
		const int32 RootIndex = GetRootIndex();
		if (MutableDC->Active.IsValidIndex(RootIndex))
		{
			MutableDC->Active[RootIndex] = false;

			// Also disable the root's game thread particle, so the intact shape doesn't remain in the game thread
			// scene query structure, where it would block traces and character movement.
			FGeometryCollectionPhysicsProxy* Proxy = GetPhysicsProxy();
			if (FGeometryCollectionPhysicsProxy::FParticle* RootParticle = Proxy ? Proxy->GetParticleByIndex_External(RootIndex) : nullptr)
			{
				RootParticle->SetDisabled(true);
			}
		}
	}
	ForceBrokenForCustomRenderer(true);

	UE_LOG(LogPersistenceExamples, Log,
		TEXT("[%s] PostRestoreObject: applied %d piece transforms, restored %d broken indices"),
		*GetNameSafe(GetOwner()), PersistedState.PieceTransforms.Num(), PersistedState.BrokenIndices.Num());
}
