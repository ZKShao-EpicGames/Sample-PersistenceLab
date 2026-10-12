// Copyright Epic Games, Inc. All Rights Reserved.

#include "GAS/HealthAttributeSet.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"
#include "PersistenceExamples.h"

UHealthAttributeSet::UHealthAttributeSet()
{
	InitHealth(100.0f);
	InitMaxHealth(100.0f);
}

void UHealthAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHealthAttributeSet, Health);
	DOREPLIFETIME(UHealthAttributeSet, MaxHealth);
}

bool UHealthAttributeSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
	UE_LOG(LogPersistenceExamples, Verbose, TEXT("PreApply: Gameplay Effect '%s'"), *Data.EffectSpec.Def->GetClass()->GetName());
	return Super::PreGameplayEffectExecute(Data);
}

void UHealthAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		// Convert the meta Damage into a clamped Health delta.
		const float DamageValue = GetDamage();
		const float OldHealthValue = GetHealth();
		const float MaxHealthValue = GetMaxHealth();
		const float NewHealthValue = FMath::Clamp(OldHealthValue - DamageValue, 0.0f, MaxHealthValue);

		if (OldHealthValue != NewHealthValue)
		{
			SetHealth(NewHealthValue);

			// Lenient cue tag lookup so authoring is optional.
			const FGameplayTag DamageCueTag = FGameplayTag::RequestGameplayTag(FName("GameplayCue.DamageNumber"), /*ErrorIfNotFound=*/false);
			if (DamageCueTag.IsValid())
			{
				if (UAbilitySystemComponent* OwningASC = GetValid(GetOwningAbilitySystemComponent()))
				{
					FGameplayCueParameters DamageCueParams;
					DamageCueParams.NormalizedMagnitude = 1.f;
					DamageCueParams.RawMagnitude = OldHealthValue - NewHealthValue;
					OwningASC->ExecuteGameplayCue(DamageCueTag, DamageCueParams);
				}
			}
		}

		SetDamage(0.0f);
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		// Direct Health modifiers (e.g. heals) only get their current value clamped by PreAttributeChange. Clamp the base
		// value too, so it can't exceed MaxHealth.
		SetHealth(FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
	}
}

void UHealthAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	Super::PreAttributeChange(Attribute, NewValue);
}

void UHealthAttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);

	if (Attribute == GetHealthAttribute())
	{
		OnHealthChanged.Broadcast(this, OldValue, NewValue);
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		// Rescale: broadcast a no-op Health change so listeners (e.g. health bars) recompute fractions.
		const float CurrentHealth = GetHealth();
		OnHealthChanged.Broadcast(this, CurrentHealth, CurrentHealth);
	}
}

void UHealthAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHealthAttributeSet, Health, OldValue);
	OnHealthChanged.Broadcast(this, OldValue.GetCurrentValue(), GetHealth());
}

void UHealthAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHealthAttributeSet, MaxHealth, OldValue);
	const float CurrentHealth = GetHealth();
	OnHealthChanged.Broadcast(this, CurrentHealth, CurrentHealth);
}

void UHealthAttributeSet::PostRestoreObject_Implementation(const TArray<FName>& RestoredPropertyNames)
{
	UAbilitySystemComponent* OwningASC = GetOwningAbilitySystemComponent();
	if (!OwningASC || !OwningASC->HasBegunPlay())
	{
		return;
	}

	// Although attribute values can be restored directly by LevelStreamingPersistence plugin,
	// call the setters here to broadcast change delegates so that listeners observe the new value.
	const bool bRestoredMax = RestoredPropertyNames.Contains(GET_MEMBER_NAME_CHECKED(UHealthAttributeSet, MaxHealth));
	if (bRestoredMax)
	{
		SetMaxHealth(GetMaxHealth());
	}

	const bool bRestoredHealth = RestoredPropertyNames.Contains(GET_MEMBER_NAME_CHECKED(UHealthAttributeSet, Health));
	if (bRestoredHealth)
	{
		SetHealth(GetHealth());
	}

	UE_LOG(LogPersistenceExamples, Log, TEXT("[%s] PostRestoreObject: Health=%.2f / MaxHealth=%.2f (restored Health=%d, MaxHealth=%d)"), *GetNameSafe(GetOwningActor()), GetHealth(), GetMaxHealth(), bRestoredHealth ? 1 : 0, bRestoredMax ? 1 : 0);
}
