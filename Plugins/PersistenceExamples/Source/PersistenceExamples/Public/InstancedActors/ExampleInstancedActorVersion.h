// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Custom version for UExampleInstancedActorComponent::SerializeInstancePersistenceData.
 *
 * Evolving the schema by hand:
 *   1. Add the next enum entry above VersionPlusOne (LatestVersion auto-tracks the last one).
 *   2. Add/remove the matching data in SerializeInstancePersistenceData.
 *   3. Gate the new layout on Ar.CustomVer(GUID) >= <YourNewEntry>.
 */
struct FExampleInstancedActorVersion
{
	enum Type
	{
		// Before any custom version was assigned to this GUID.
		BeforeCustomVersionWasAdded = 0,

		// Initial layout: { VisualizationOverrides[], HealthOverrides[] }
		// (matches the prior manual PersistenceFormatVersion == 1).
		InitialVersion,

		// Appends StageOverrides[] { InstanceIndex, Stage, RemainingSeconds } for stage timers (respawn / regrow).
		// Instances with a stage record have no VisualizationOverrides entry: their visualization follows the stage.
		StageTimers,

		// ---- new versions go above this line ----
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	/** Unique, stable GUID identifying this custom version across sessions. */
	PERSISTENCEEXAMPLES_API const static FGuid GUID;

	FExampleInstancedActorVersion() = delete;
};
