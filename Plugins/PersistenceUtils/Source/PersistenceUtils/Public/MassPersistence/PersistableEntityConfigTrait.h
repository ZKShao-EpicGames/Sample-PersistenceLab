// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"
#include "Mass/ExternalSubsystemTraits.h"
#include "MassEntityTraitBase.h"
#include "PersistableEntityConfigTrait.generated.h"

class UMassEntityConfigAsset;

/**
 * Records which UMassEntityConfigAsset spawned an entity, and acts as the runtime opt-in marker for
 * persistence. Entities whose archetype contains this fragment are considered persistable by
 * UMassPersistenceUtils::SnapshotEntities; the fragment's EntityConfig value attributes each entity
 * to its source config, avoiding cross-matching between configs that share fragment composition.
 *
 * Added to an entity's archetype by UPersistableEntityConfigTrait. The value is stamped after spawn
 * by APersistedMassSpawner (using GetFragmentDataChecked, no archetype migration), and re-stamped
 * by RestoreEntities after re-creating entities from a saved snapshot. The fragment is NOT
 * byte-serialized into snapshots: the raw TObjectPtr would be meaningless across sessions.
 */
USTRUCT()
struct PERSISTENCEUTILS_API FPersistableEntityConfigFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UMassEntityConfigAsset> EntityConfig = nullptr;
};

// TObjectPtr<T> isn't trivially copyable in editor builds (carries debug state). Opt out of Mass's
// trivial-copy fragment check. The GC barrier is the only "non-trivial" cost and it's a wash for
// a single-pointer fragment that lives one-per-entity.
template<>
struct TMassFragmentTraits<FPersistableEntityConfigFragment> final
{
	enum
	{
		AuthorAcceptsItsNotTriviallyCopyable = true
	};
};

/**
 * Mass entity trait that opts an entity config into the persistence flow by adding
 * FPersistableEntityConfigFragment to the resulting archetype. The fragment's value is stamped by
 * APersistedMassSpawner after spawn (or by UMassPersistenceUtils::RestoreEntities after restore).
 * SnapshotEntities then queries for entities containing this fragment, groups them by EntityConfig,
 * and produces one FMassEntityConfigGroupSnapshot per config.
 */
UCLASS(meta=(DisplayName="Persistable Entity Config"))
class PERSISTENCEUTILS_API UPersistableEntityConfigTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

protected:
	virtual void BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, const UWorld& World) const override;
};
