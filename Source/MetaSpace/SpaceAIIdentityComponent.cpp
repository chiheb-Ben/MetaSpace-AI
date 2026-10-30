// Copyright Epic Games, Inc. All Rights Reserved.

#include "SpaceAIIdentityComponent.h"
#include "MetaAIComponent.h"
#include "SpaceObjectComponent.h"
#include "SpaceFloatingComponent.h"
#include "SpaceGrabSnapComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Dom/JsonValue.h"

USpaceAIIdentityComponent::USpaceAIIdentityComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USpaceAIIdentityComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (EntityName.TrimStartAndEnd().IsEmpty())
	{
		EntityName = Owner->GetActorNameOrLabel();
	}

	if (EntityID.IsNone())
	{
		FString CleanId = EntityName.ToLower().Replace(TEXT(" "), TEXT("_"));
		EntityID = FName(*CleanId);
	}

	// Auto-detect AstronautPlayer role if attached to a Character with SpaceFloatingComponent
	if (EntityRole == ESpaceAIEntityRole::SpaceProp && Cast<ACharacter>(Owner))
	{
		EntityRole = ESpaceAIEntityRole::AstronautPlayer;
	}

	// Auto-detect Spaceship Repair Task / Snap Target if tagged "SnapPoint" or role is SnapTargetSlot / SpaceshipRepairZone
	if (EntityRole == ESpaceAIEntityRole::SnapTargetSlot ||
		EntityRole == ESpaceAIEntityRole::SpaceshipRepairZone ||
		Owner->ActorHasTag(TEXT("SnapPoint")))
	{
		bIsRepairTaskOrZone = true;
	}

	// Auto-enable [E] key interaction if tagged "Interactable" on Actor or any Component
	if (Owner->ActorHasTag(TEXT("Interactable")))
	{
		bCanInteractWithEKey = true;
	}
	else
	{
		for (const UActorComponent* SubComp : Owner->GetComponents())
		{
			if (SubComp && SubComp->ComponentHasTag(TEXT("Interactable")))
			{
				bCanInteractWithEKey = true;
				break;
			}
		}
	}
}

void USpaceAIIdentityComponent::SetRepairTaskCompleted(bool bCompleted, float NewIntegrityPercent)
{
	bIsRepairTaskOrZone = true;
	bIsRepairTaskCompleted = bCompleted;
	ZoneIntegrityPercent = bCompleted ? FMath::Clamp(NewIntegrityPercent, 1.0f, 100.0f) : FMath::Clamp(NewIntegrityPercent, 0.0f, 99.0f);
}

void USpaceAIIdentityComponent::TriggerPlayerInteractionWithE(AActor* InteractingPlayer, UPrimitiveComponent* HitComponent)
{
	if (bMarkRepairCompletedOnInteract)
	{
		SetRepairTaskCompleted(true, 100.0f);
	}

	const FName HitCompName = HitComponent ? HitComponent->GetFName() : NAME_None;

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			-1,
			5.0f,
			FColor::Green,
			FString::Printf(TEXT("[Interactive System (E Key)] Interacted with '%s' (Component: %s)"),
				*GetEffectiveEntityName(),
				*HitCompName.ToString())
		);
	}

	// 1. Fire the Blueprint event on this Actor so the user's custom Blueprint logic runs immediately!
	OnInteractedWithEKey.Broadcast(InteractingPlayer, HitComponent, HitCompName);

	// 2. Automatically inform Meta AI and the MySQL Database if enabled
	if (bAutoNotifyAIOnInteract && GetOwner())
	{
		const FString DetailedWhatHappened = FString::Printf(
			TEXT("Player pressed [E] on '%s' (Component: %s): %s"),
			*GetEffectiveEntityName(),
			*HitCompName.ToString(),
			*InteractionDescriptionForAI
		);
		UMetaAIComponent::ReportShipScenarioEventToAI(
			GetOwner(),
			TEXT("Player_E_Key_Interact"),
			GetEffectiveEntityName(),
			DetailedWhatHappened,
			bMarkRepairCompletedOnInteract
		);
	}
}

FString USpaceAIIdentityComponent::GetEffectiveEntityName() const
{
	if (!EntityName.TrimStartAndEnd().IsEmpty())
	{
		return EntityName.TrimStartAndEnd();
	}
	return GetOwner() ? GetOwner()->GetActorNameOrLabel() : TEXT("UnknownEntity");
}

FString USpaceAIIdentityComponent::GetEffectiveEntityID() const
{
	if (!EntityID.IsNone())
	{
		return EntityID.ToString();
	}
	return GetEffectiveEntityName().ToLower().Replace(TEXT(" "), TEXT("_"));
}

bool USpaceAIIdentityComponent::MatchesAITargetName(const FString& TargetIdentifier) const
{
	const FString TrimmedTarget = TargetIdentifier.TrimStartAndEnd();
	if (TrimmedTarget.IsEmpty())
	{
		return false;
	}

	// Wildcards for all objects or player
	if (TrimmedTarget.Equals(TEXT("ALL"), ESearchCase::IgnoreCase) ||
		TrimmedTarget.Equals(TEXT("ALL_ENTITIES"), ESearchCase::IgnoreCase))
	{
		return true;
	}

	if ((TrimmedTarget.Equals(TEXT("ALL_OBJECTS"), ESearchCase::IgnoreCase) ||
		 TrimmedTarget.Equals(TEXT("ALL_PROPS"), ESearchCase::IgnoreCase)) &&
		EntityRole != ESpaceAIEntityRole::AstronautPlayer)
	{
		return true;
	}

	if ((TrimmedTarget.Equals(TEXT("PLAYER"), ESearchCase::IgnoreCase) ||
		 TrimmedTarget.Equals(TEXT("TRAINEE"), ESearchCase::IgnoreCase) ||
		 TrimmedTarget.Equals(TEXT("ASTRONAUT"), ESearchCase::IgnoreCase) ||
		 TrimmedTarget.Equals(TEXT("CHARACTER"), ESearchCase::IgnoreCase)) &&
		(EntityRole == ESpaceAIEntityRole::AstronautPlayer || Cast<ACharacter>(GetOwner()) != nullptr))
	{
		return true;
	}

	const FString MyName = GetEffectiveEntityName();
	const FString MyID = GetEffectiveEntityID();
	const FString ActorInternalName = GetOwner() ? GetOwner()->GetName() : TEXT("");

	if (MyName.Equals(TrimmedTarget, ESearchCase::IgnoreCase) ||
		MyID.Equals(TrimmedTarget, ESearchCase::IgnoreCase) ||
		ActorInternalName.Equals(TrimmedTarget, ESearchCase::IgnoreCase))
	{
		return true;
	}

	// Also allow clean partial/substring matching (e.g., AI says "Chair" and EntityName is "Office Chair 1")
	if (MyName.Contains(TrimmedTarget, ESearchCase::IgnoreCase) ||
		MyID.Contains(TrimmedTarget, ESearchCase::IgnoreCase))
	{
		return true;
	}

	return false;
}

TSharedPtr<FJsonObject> USpaceAIIdentityComponent::BuildTelemetryJsonObject() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return Json;
	}

	Json->SetStringField(TEXT("id"), GetEffectiveEntityID());
	Json->SetStringField(TEXT("name"), GetEffectiveEntityName());

	FString RoleString = TEXT("SpaceProp");
	switch (EntityRole)
	{
	case ESpaceAIEntityRole::SpaceProp:           RoleString = TEXT("SpaceProp"); break;
	case ESpaceAIEntityRole::MissionToolOrItem:   RoleString = TEXT("MissionItem"); break;
	case ESpaceAIEntityRole::AstronautPlayer:     RoleString = TEXT("AstronautPlayer"); break;
	case ESpaceAIEntityRole::SnapTargetSlot:      RoleString = TEXT("SnapTargetSlot"); break;
	case ESpaceAIEntityRole::SpaceshipRepairZone: RoleString = TEXT("SpaceshipRepairZone"); break;
	case ESpaceAIEntityRole::SpaceHazardObstacle: RoleString = TEXT("SpaceHazardObstacle"); break;
	case ESpaceAIEntityRole::StationEnvironment:  RoleString = TEXT("StationEnvironment"); break;
	}
	Json->SetStringField(TEXT("role"), RoleString);
	Json->SetStringField(TEXT("description"), DescriptionAndPurpose);

	if (bIsRepairTaskOrZone)
	{
		Json->SetBoolField(TEXT("is_repair_zone_or_task"), true);
		Json->SetStringField(TEXT("spaceship_zone_name"), SpaceshipZoneName.IsEmpty() ? GetEffectiveEntityName() : SpaceshipZoneName);
		Json->SetBoolField(TEXT("is_repair_completed"), bIsRepairTaskCompleted);
		Json->SetNumberField(TEXT("zone_integrity_percent"), ZoneIntegrityPercent);
	}

	if (!AIPhysicsControlHint.IsEmpty())
	{
		Json->SetStringField(TEXT("ai_hint"), AIPhysicsControlHint);
	}

	const FVector Loc = Owner->GetActorLocation();
	TArray<TSharedPtr<FJsonValue>> LocArray;
	LocArray.Add(MakeShareable(new FJsonValueNumber(FMath::RoundToInt(Loc.X))));
	LocArray.Add(MakeShareable(new FJsonValueNumber(FMath::RoundToInt(Loc.Y))));
	LocArray.Add(MakeShareable(new FJsonValueNumber(FMath::RoundToInt(Loc.Z))));
	Json->SetArrayField(TEXT("location"), LocArray);

	// 1. If this Actor has a USpaceObjectComponent (Floating Prop / Mission Item), attach its live physics & size stats!
	if (const USpaceObjectComponent* SpaceObj = Owner->FindComponentByClass<USpaceObjectComponent>())
	{
		TSharedPtr<FJsonObject> PhysJson = MakeShareable(new FJsonObject());
		const FVector ActorScale = Owner->GetActorScale3D();
		const FVector BoundsSizeCm = Owner->GetComponentsBoundingBox(true).GetSize();

		PhysJson->SetNumberField(TEXT("scale"), FMath::RoundToFloat(ActorScale.X * 100.0f) / 100.0f);
		PhysJson->SetStringField(
			TEXT("size_cm"),
			FString::Printf(TEXT("%.0fx%.0fx%.0f cm"), BoundsSizeCm.X, BoundsSizeCm.Y, BoundsSizeCm.Z)
		);
		PhysJson->SetNumberField(TEXT("mass_kg"), SpaceObj->MassInKg);
		PhysJson->SetBoolField(TEXT("zero_g"), SpaceObj->IsZeroGEnabled());
		PhysJson->SetNumberField(TEXT("spin_speed_deg"), SpaceObj->SelfRotationSpeed);
		PhysJson->SetNumberField(TEXT("max_speed"), SpaceObj->MaxAllowedSpeed);
		PhysJson->SetNumberField(TEXT("linear_damping"), SpaceObj->ZeroGLinearDamping);
		PhysJson->SetNumberField(TEXT("angular_damping"), SpaceObj->ZeroGAngularDamping);
		PhysJson->SetNumberField(TEXT("bounce"), SpaceObj->SurfaceBounceCoefficient);

		if (UPrimitiveComponent* Prim = SpaceObj->GetPrimaryPrimitive())
		{
			PhysJson->SetNumberField(TEXT("current_speed"), FMath::RoundToInt(Prim->GetPhysicsLinearVelocity().Size()));
		}

		Json->SetObjectField(TEXT("object_physics"), PhysJson);
	}

	// 2. If this Actor has a USpaceFloatingComponent (Player / Astronaut Character), attach character physics stats!
	if (const USpaceFloatingComponent* FloatComp = Owner->FindComponentByClass<USpaceFloatingComponent>())
	{
		TSharedPtr<FJsonObject> CharPhysJson = MakeShareable(new FJsonObject());
		CharPhysJson->SetNumberField(TEXT("body_mass_kg"), FloatComp->TraineeBodyMassKg);
		CharPhysJson->SetNumberField(TEXT("max_fly_speed"), FloatComp->ZeroGMaxFlySpeed);
		CharPhysJson->SetNumberField(TEXT("hand_reach_distance"), FloatComp->HandReachDistance);
		CharPhysJson->SetNumberField(TEXT("wall_launch_strength"), FloatComp->PushImpulseStrength);
		CharPhysJson->SetNumberField(TEXT("wall_pull_strength"), FloatComp->PullImpulseStrength);
		CharPhysJson->SetBoolField(TEXT("is_grabbing_wall"), FloatComp->IsGrabbingSurface());
		CharPhysJson->SetNumberField(TEXT("current_speed"), FMath::RoundToInt(Owner->GetVelocity().Size()));

		if (const USpaceGrabSnapComponent* GrabComp = Owner->FindComponentByClass<USpaceGrabSnapComponent>())
		{
			CharPhysJson->SetBoolField(TEXT("is_holding_item"), GrabComp->IsHoldingObject());
			CharPhysJson->SetNumberField(TEXT("throw_speed"), GrabComp->ThrowLaunchSpeed);
			if (AActor* Held = GrabComp->GetHeldActor())
			{
				if (const USpaceAIIdentityComponent* HeldId = Held->FindComponentByClass<USpaceAIIdentityComponent>())
				{
					CharPhysJson->SetStringField(TEXT("held_item"), HeldId->GetEffectiveEntityName());
				}
				else
				{
					CharPhysJson->SetStringField(TEXT("held_item"), Held->GetActorNameOrLabel());
				}
			}
		}

		Json->SetObjectField(TEXT("player_physics"), CharPhysJson);
	}

	return Json;
}

FString USpaceAIIdentityComponent::GetLiveTelemetrySummaryForAI() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return TEXT("");
	}

	FString Summary = FString::Printf(
		TEXT("[%s (ID: %s) | Description: %s"),
		*GetEffectiveEntityName(),
		*GetEffectiveEntityID(),
		*DescriptionAndPurpose
	);

	if (const USpaceObjectComponent* SpaceObj = Owner->FindComponentByClass<USpaceObjectComponent>())
	{
		const FVector ActorScale = Owner->GetActorScale3D();
		const FVector BoundsSizeCm = Owner->GetComponentsBoundingBox(true).GetSize();
		Summary += FString::Printf(
			TEXT(" | Size: %.2fx (%.0fx%.0fx%.0f cm) | Physics: Mass=%.2fkg, Spin=%.1fdeg/s, MaxSpeed=%.0f, Damping=%.2f, Bounce=%.2f, ZeroG=%s"),
			ActorScale.X,
			BoundsSizeCm.X,
			BoundsSizeCm.Y,
			BoundsSizeCm.Z,
			SpaceObj->MassInKg,
			SpaceObj->SelfRotationSpeed,
			SpaceObj->MaxAllowedSpeed,
			SpaceObj->ZeroGLinearDamping,
			SpaceObj->SurfaceBounceCoefficient,
			SpaceObj->IsZeroGEnabled() ? TEXT("ON") : TEXT("OFF")
		);
	}

	if (const USpaceFloatingComponent* FloatComp = Owner->FindComponentByClass<USpaceFloatingComponent>())
	{
		Summary += FString::Printf(
			TEXT(" | Astronaut: BodyMass=%.1fkg, MaxSpeed=%.0f, Reach=%.0f, LaunchPower=%.0f, PullPower=%.0f"),
			FloatComp->TraineeBodyMassKg,
			FloatComp->ZeroGMaxFlySpeed,
			FloatComp->HandReachDistance,
			FloatComp->PushImpulseStrength,
			FloatComp->PullImpulseStrength
		);
	}

	if (bIsRepairTaskOrZone)
	{
		Summary += FString::Printf(
			TEXT(" | RepairZone='%s' [%s, Integrity=%.0f%%]"),
			*(SpaceshipZoneName.IsEmpty() ? GetEffectiveEntityName() : SpaceshipZoneName),
			bIsRepairTaskCompleted ? TEXT("COMPLETED") : TEXT("INCOMPLETE_DAMAGED"),
			ZoneIntegrityPercent
		);
	}

	Summary += TEXT("]");
	return Summary;
}
