// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "References/PersistableActorReference.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "ResolvePersistableActorReferenceAction.generated.h"

class AActor;

/**
 * Latent Blueprint node that resolves a FPersistableActorReference into a live AActor*. The standard "Then" exec
 * output fires immediately after the node starts (use it to do unrelated work in parallel). OnSuccess fires once
 * with the resolved actor: synchronously from the node if already resolvable, deferred until the owning level's
 * LSP PostRestoreLevel otherwise. OnFailure fires if resolution is definitively impossible (level finished
 * restoring without a match, world torn down, or the reference is None).
 */
UCLASS()
class PERSISTENCEUTILS_API UResolvePersistableActorReferenceAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FResolveOutputPin, AActor*, ResolvedActor);

	UPROPERTY(BlueprintAssignable)
	FResolveOutputPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FResolveOutputPin OnFailure;

	UFUNCTION(BlueprintCallable, Category="Persistence",
		meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject",
		      DisplayName="Resolve Persistable Actor Reference"))
	static UResolvePersistableActorReferenceAction* ResolvePersistableActorReference(
		UObject* WorldContextObject,
		FPersistableActorReference Reference);

	virtual void Activate() override;

private:
	// Queues a broadcast for next tick so the standard "Then" exec pin always fires before OnSuccess/OnFailure,
	// even when resolution is synchronous. K2 async-task nodes execute Then after Activate() returns; firing the
	// delegate inside Activate would invert that order.
	void DeferBroadcast(AActor* ResolvedActor, bool bSuccess);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FPersistableActorReference Reference;
	FDelegateHandle PendingHandle;
};
