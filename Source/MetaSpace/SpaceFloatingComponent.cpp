// Copyright Epic Games, Inc. All Rights Reserved.

#include "SpaceFloatingComponent.h"
#include "SpaceGrabSnapComponent.h"
#include "SpaceObjectComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "InputCoreTypes.h"
#include "Math/UnrealMathUtility.h"

USpaceFloatingComponent::USpaceFloatingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// Tick BEFORE CharacterMovementComponent and Physics so our mode changes and 3D impulses are processed immediately each frame
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USpaceFloatingComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (bRandomizePhase)
	{
		PhaseOffset = FMath::FRandRange(0.0f, 100.0f);
		RunningTime = PhaseOffset;
	}

	RandomDriftDirection = FMath::VRand();
	RandomRotationAxis = FMath::VRand();
	if (RandomRotationAxis.IsNearlyZero())
	{
		RandomRotationAxis = FVector(0.3f, 0.5f, 0.8f).GetSafeNormal();
	}

	OwnerCharacter = Cast<ACharacter>(Owner);
	if (OwnerCharacter)
	{
		OwnerMovementComp = OwnerCharacter->GetCharacterMovement();
		VisualTargetComponent = OwnerCharacter->GetMesh();
		OwnerCamera = OwnerCharacter->FindComponentByClass<UCameraComponent>();
		OwnerSpringArm = OwnerCharacter->FindComponentByClass<USpringArmComponent>();

		if (VisualTargetComponent)
		{
			BaseRelativeLocation = VisualTargetComponent->GetRelativeLocation();
			BaseRelativeRotation = VisualTargetComponent->GetRelativeRotation();
		}

		if (OwnerMovementComp)
		{
			OwnerMovementComp->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
		}
	}
	else
	{
		DiscoverPropPrimitives(Owner);
		BaseActorLocation = Owner->GetActorLocation();
		BaseActorRotation = Owner->GetActorRotation();
	}

	if (bStartFloatingOnBeginPlay)
	{
		SetFloatingEnabled(true);
	}
}

FVector USpaceFloatingComponent::GetCameraAimDirection() const
{
	if (OwnerCamera)
	{
		return OwnerCamera->GetForwardVector();
	}
	if (OwnerCharacter)
	{
		if (AController* Controller = OwnerCharacter->GetController())
		{
			return Controller->GetControlRotation().Vector();
		}
		return OwnerCharacter->GetViewRotation().Vector();
	}
	return FVector::ForwardVector;
}

void USpaceFloatingComponent::ConfigureFirstPersonCamera()
{
	if (!OwnerCharacter || !bAutoConfigureFirstPersonView)
	{
		return;
	}

	if (OwnerSpringArm)
	{
		SavedTargetArmLength = OwnerSpringArm->TargetArmLength;
		SavedSpringArmLocation = OwnerSpringArm->GetRelativeLocation();

		OwnerSpringArm->TargetArmLength = 0.0f;
		OwnerSpringArm->SetRelativeLocation(FirstPersonCameraRelativeOffset);
		OwnerSpringArm->bUsePawnControlRotation = true;
		OwnerSpringArm->bInheritPitch = true;
		OwnerSpringArm->bInheritYaw = true;
		OwnerSpringArm->bInheritRoll = true;
		OwnerSpringArm->bDoCollisionTest = false;
	}
	else if (OwnerCamera)
	{
		OwnerCamera->SetRelativeLocation(FirstPersonCameraRelativeOffset);
		OwnerCamera->bUsePawnControlRotation = true;
	}

	if (bHideThirdPersonBodyInFirstPerson && OwnerCharacter->GetMesh())
	{
		OwnerCharacter->GetMesh()->SetOwnerNoSee(true);
	}
}

void USpaceFloatingComponent::RestoreThirdPersonCamera()
{
	if (!OwnerCharacter || !bAutoConfigureFirstPersonView)
	{
		return;
	}

	if (OwnerSpringArm)
	{
		OwnerSpringArm->TargetArmLength = SavedTargetArmLength;
		OwnerSpringArm->SetRelativeLocation(SavedSpringArmLocation);
		OwnerSpringArm->bDoCollisionTest = true;
	}

	if (bHideThirdPersonBodyInFirstPerson && OwnerCharacter->GetMesh())
	{
		OwnerCharacter->GetMesh()->SetOwnerNoSee(false);
	}
}

void USpaceFloatingComponent::DiscoverPropPrimitives(AActor* Owner)
{
	PropPrimitives.Reset();
	OwnerRootPrimitive = nullptr;

	if (!Owner)
	{
		return;
	}

	if (bForceMovableOnProps && Owner->GetRootComponent())
	{
		Owner->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	}

	TArray<UPrimitiveComponent*> FoundPrimitives;
	Owner->GetComponents<UPrimitiveComponent>(FoundPrimitives);

	for (UPrimitiveComponent* Prim : FoundPrimitives)
	{
		if (!Prim || !Prim->IsRegistered())
		{
			continue;
		}

		if (bForceMovableOnProps)
		{
			Prim->SetMobility(EComponentMobility::Movable);
		}

		PropPrimitives.Add(Prim);

		Prim->SetNotifyRigidBodyCollision(true);
		Prim->OnComponentHit.RemoveDynamic(this, &USpaceFloatingComponent::OnPropComponentHit);
		Prim->OnComponentHit.AddDynamic(this, &USpaceFloatingComponent::OnPropComponentHit);

		if (!OwnerRootPrimitive)
		{
			OwnerRootPrimitive = Prim;
		}
		else if (Prim->IsSimulatingPhysics() && !OwnerRootPrimitive->IsSimulatingPhysics())
		{
			OwnerRootPrimitive = Prim;
		}
	}
}

void USpaceFloatingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bIsFloatingActive)
	{
		DeactivateZeroG();
	}

	Super::EndPlay(EndPlayReason);
}

void USpaceFloatingComponent::SetFloatingEnabled(bool bEnable)
{
	if (bIsFloatingActive == bEnable)
	{
		return;
	}

	bIsFloatingActive = bEnable;

	if (bIsFloatingActive)
	{
		ActivateZeroG();
	}
	else
	{
		DeactivateZeroG();
	}
}

void USpaceFloatingComponent::ActivateZeroG()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (OwnerCharacter && OwnerMovementComp)
	{
		SavedGravityScale = OwnerMovementComp->GravityScale;
		SavedGroundFriction = OwnerMovementComp->GroundFriction;
		SavedBrakingDecelerationFlying = OwnerMovementComp->BrakingDecelerationFlying;
		SavedMaxFlySpeed = OwnerMovementComp->MaxFlySpeed;
		SavedMaxAcceleration = OwnerMovementComp->MaxAcceleration;
		SavedBrakingFrictionFactor = OwnerMovementComp->BrakingFrictionFactor;
		SavedWalkableFloorAngle = OwnerMovementComp->GetWalkableFloorAngle();
		SavedWalkableFloorZ = OwnerMovementComp->GetWalkableFloorZ();
		bSavedOrientRotationToMovement = OwnerMovementComp->bOrientRotationToMovement;

		// Requirement 1: Zero-Gravity CharacterMovementComponent Setup
		OwnerMovementComp->GravityScale = 0.0f;
		OwnerMovementComp->GroundFriction = ZeroGGroundFriction;
		OwnerMovementComp->BrakingDecelerationFlying = ZeroGBrakingDeceleration;
		OwnerMovementComp->BrakingDecelerationWalking = 0.0f;
		OwnerMovementComp->BrakingDecelerationFalling = 0.0f;
		OwnerMovementComp->BrakingFrictionFactor = ZeroGFluidFriction;
		OwnerMovementComp->BrakingFriction = 0.0f;
		OwnerMovementComp->bUseSeparateBrakingFriction = false;
		OwnerMovementComp->MaxFlySpeed = ZeroGMaxFlySpeed;

		OwnerMovementComp->bEnablePhysicsInteraction = true;
		OwnerMovementComp->TouchForceFactor = 1.0f;
		OwnerMovementComp->PushForceFactor = 500.0f;

		OwnerMovementComp->SetWalkableFloorAngle(0.0f);
		OwnerMovementComp->SetWalkableFloorZ(1.01f);
		OwnerMovementComp->CurrentFloor.Clear();

		if (bAlignCharacterBodyToCamera)
		{
			OwnerMovementComp->bOrientRotationToMovement = false;
		}

		ConfigureFirstPersonCamera();

		if (InitialCharacterLiftHeight > 0.0f)
		{
			OwnerCharacter->AddActorWorldOffset(FVector(0.0f, 0.0f, InitialCharacterLiftHeight), true);
		}

		OwnerMovementComp->SetMovementMode(MOVE_Flying);

		if (!FMath::IsNearlyZero(InitialCharacterLiftVelocity))
		{
			OwnerMovementComp->Velocity += FVector(0.0f, 0.0f, InitialCharacterLiftVelocity);
		}
	}
	else
	{
		if (PropPrimitives.IsEmpty())
		{
			DiscoverPropPrimitives(Owner);
		}

		if (InitialPropLiftHeight > 0.0f)
		{
			Owner->AddActorWorldOffset(FVector(0.0f, 0.0f, InitialPropLiftHeight), false);
		}

		BaseActorLocation = Owner->GetActorLocation();
		BaseActorRotation = Owner->GetActorRotation();
		PreviousWaveVelocity = FVector::ZeroVector;

		for (UPrimitiveComponent* Prim : PropPrimitives)
		{
			if (!Prim)
			{
				continue;
			}

			if (bForceMovableOnProps)
			{
				Prim->SetMobility(EComponentMobility::Movable);
			}

			Prim->SetEnableGravity(false);

			if (Prim == OwnerRootPrimitive && bAutoEnableSimulatePhysicsOnProps && !Prim->IsSimulatingPhysics())
			{
				if (Prim->GetCollisionEnabled() == ECollisionEnabled::NoCollision ||
					Prim->GetCollisionEnabled() == ECollisionEnabled::QueryOnly)
				{
					Prim->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					Prim->SetCollisionResponseToAllChannels(ECR_Block);
				}
				Prim->SetSimulatePhysics(true);
			}

			if (Prim->IsSimulatingPhysics())
			{
				OwnerRootPrimitive = Prim;
				BaseActorLocation = Prim->GetComponentLocation();
				BaseActorRotation = Prim->GetComponentRotation();

				SavedLinearDamping = Prim->GetLinearDamping();
				SavedAngularDamping = Prim->GetAngularDamping();

				Prim->SetEnableGravity(false);
				Prim->SetLinearDamping(ZeroGLinearDamping);
				Prim->SetAngularDamping(ZeroGAngularDamping);
				Prim->SetNotifyRigidBodyCollision(true);
				Prim->WakeAllRigidBodies();

				FVector InitialVel = FVector::ZeroVector;
				if (bApplyInitialRandomDrift)
				{
					InitialVel = RandomDriftDirection * InitialDriftSpeed;
				}
				Prim->SetPhysicsLinearVelocity(InitialVel, false);

				if (InPlaceRotationSpeed > 0.0f)
				{
					Prim->SetPhysicsAngularVelocityInDegrees(RandomRotationAxis * InPlaceRotationSpeed, false);
				}
			}
		}
	}
}

void USpaceFloatingComponent::DeactivateZeroG()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	bIsGrabbingWall = false;

	if (OwnerCharacter && OwnerMovementComp)
	{
		OwnerMovementComp->GravityScale = SavedGravityScale;
		OwnerMovementComp->GroundFriction = SavedGroundFriction;
		OwnerMovementComp->BrakingDecelerationFlying = SavedBrakingDecelerationFlying;
		OwnerMovementComp->MaxFlySpeed = SavedMaxFlySpeed;
		OwnerMovementComp->MaxAcceleration = SavedMaxAcceleration;
		OwnerMovementComp->BrakingFrictionFactor = SavedBrakingFrictionFactor;
		OwnerMovementComp->SetWalkableFloorAngle(SavedWalkableFloorAngle);
		OwnerMovementComp->SetWalkableFloorZ(SavedWalkableFloorZ);
		OwnerMovementComp->bOrientRotationToMovement = bSavedOrientRotationToMovement;
		OwnerMovementComp->SetMovementMode(MOVE_Falling);

		RestoreThirdPersonCamera();

		FRotator UprightRot = OwnerCharacter->GetActorRotation();
		UprightRot.Pitch = 0.0f;
		UprightRot.Roll = 0.0f;
		OwnerCharacter->SetActorRotation(UprightRot);

		CurrentRollAngle = 0.0f;

		if (VisualTargetComponent)
		{
			VisualTargetComponent->SetRelativeLocation(BaseRelativeLocation);
			VisualTargetComponent->SetRelativeRotation(BaseRelativeRotation);
		}
	}
	else
	{
		bool bWasPhysics = false;
		for (UPrimitiveComponent* Prim : PropPrimitives)
		{
			if (Prim)
			{
				Prim->SetEnableGravity(true);
				if (Prim->IsSimulatingPhysics())
				{
					bWasPhysics = true;
					Prim->SetLinearDamping(SavedLinearDamping);
					Prim->SetAngularDamping(SavedAngularDamping);
					Prim->WakeAllRigidBodies();
				}
			}
		}

		if (!bWasPhysics)
		{
			Owner->SetActorLocationAndRotation(BaseActorLocation, BaseActorRotation);
		}
	}
}

// =============================================================================
// Requirement 2: Detection (Line Trace from First-Person Camera Center Forward)
// =============================================================================
bool USpaceFloatingComponent::PerformHandReachTrace(FHitResult& OutHitResult) const
{
	if (!OwnerCharacter)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FVector TraceStart = OwnerCharacter->GetPawnViewLocation();
	FVector TraceDir = GetCameraAimDirection();

	if (OwnerCamera)
	{
		TraceStart = OwnerCamera->GetComponentLocation();
		TraceDir = OwnerCamera->GetForwardVector();
	}
	else if (AController* Controller = OwnerCharacter->GetController())
	{
		FRotator ViewRot;
		Controller->GetPlayerViewPoint(TraceStart, ViewRot);
		TraceDir = ViewRot.Vector();
	}

	const FVector TraceEnd = TraceStart + (TraceDir * HandReachDistance);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ZeroGHandReachTrace), false, OwnerCharacter);
	QueryParams.bReturnPhysicalMaterial = false;

	// Ignore held object so the hand reach trace passes cleanly through the carried tool to reach walls!
	if (const USpaceGrabSnapComponent* GrabSnapComp = OwnerCharacter->FindComponentByClass<USpaceGrabSnapComponent>())
	{
		if (AActor* Held = GrabSnapComp->GetHeldActor())
		{
			QueryParams.AddIgnoredActor(Held);
		}
	}

	bool bHit = false;
	if (HandTraceSphereRadius > 0.0f)
	{
		bHit = World->SweepSingleByChannel(
			OutHitResult,
			TraceStart,
			TraceEnd,
			FQuat::Identity,
			HandTraceChannel,
			FCollisionShape::MakeSphere(HandTraceSphereRadius),
			QueryParams
		);
	}
	else
	{
		bHit = World->LineTraceSingleByChannel(
			OutHitResult,
			TraceStart,
			TraceEnd,
			HandTraceChannel,
			QueryParams
		);
	}

	if (bDrawHandReachDebug && !bIsGrabbingWall)
	{
		const FColor LineColor = bHit ? FColor::Green : FColor::Red;
		DrawDebugLine(World, TraceStart, bHit ? OutHitResult.ImpactPoint : TraceEnd, LineColor, false, -1.0f, 0, 1.2f);
		if (bHit)
		{
			DrawDebugSphere(World, OutHitResult.ImpactPoint, 6.0f, 12, FColor::Cyan, false, -1.0f, 0, 1.5f);
		}
	}

	return bHit && OutHitResult.bBlockingHit;
}

// =============================================================================
// Grab, Hold & Aimed Launch Mechanics (Grab wall -> Aim/Drag any direction -> Launch!)
// =============================================================================
bool USpaceFloatingComponent::StartWallGrab()
{
	if (!bIsFloatingActive || !OwnerCharacter || !OwnerMovementComp)
	{
		return false;
	}

	// If the player is NOT currently holding an item, check if they are looking at an ungrabbed Grabbable item within reach:
	// A single LMB click grabs the item, and once held, LMB returns 100% to wall-grab & launch controls!
	if (USpaceGrabSnapComponent* GrabSnapComp = OwnerCharacter->FindComponentByClass<USpaceGrabSnapComponent>())
	{
		if (!GrabSnapComp->IsHoldingObject())
		{
			AActor* GrabbableInSight = nullptr;
			if (GrabSnapComp->IsGrabbableInReach(GrabbableInSight))
			{
				if (GrabSnapComp->TryGrabObject())
				{
					// Successfully grabbed item with this single LMB click!
					// Keep player un-anchored and free to move!
					bIsGrabbingWall = false;
					return false;
				}
			}
		}
	}

	if (!bEnableWallGrabAndAimLaunch)
	{
		return TryHandPull();
	}

	FHitResult Hit;
	if (!PerformHandReachTrace(Hit))
	{
		return false;
	}

	// Never wall-grab an Actor or Component tagged "Grabbable" (including child BoxCollision components!)
	if (AActor* HitActor = Hit.GetActor())
	{
		if (const USpaceGrabSnapComponent* GrabSnapComp = OwnerCharacter->FindComponentByClass<USpaceGrabSnapComponent>())
		{
			if (HitActor == GrabSnapComp->GetHeldActor())
			{
				return false;
			}
		}
		if (HitActor->ActorHasTag(FName("Grabbable")) ||
			(Hit.GetComponent() && Hit.GetComponent()->ComponentHasTag(FName("Grabbable"))))
		{
			return false;
		}
		for (const UActorComponent* SubComp : HitActor->GetComponents())
		{
			if (SubComp && SubComp->ComponentHasTag(FName("Grabbable")))
			{
				return false;
			}
		}

		// Also check if the Actor has a USpaceObjectComponent with mass < MinObjectMassToAnchorPlayer
		if (const USpaceObjectComponent* SpaceObj = HitActor->FindComponentByClass<USpaceObjectComponent>())
		{
			if (SpaceObj->MassInKg < MinObjectMassToAnchorPlayer)
			{
				return false;
			}
		}
	}

	// Check if what we hit (or its root primitive) is a light/medium movable physics object (< MinObjectMassToAnchorPlayer, e.g., 8kg chair)
	UPrimitiveComponent* PhysicsPrim = Hit.GetComponent();
	if (PhysicsPrim && !PhysicsPrim->IsSimulatingPhysics() && Hit.GetActor())
	{
		if (UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Hit.GetActor()->GetRootComponent()))
		{
			if (RootPrim->IsSimulatingPhysics())
			{
				PhysicsPrim = RootPrim;
			}
		}
	}

	if (PhysicsPrim && PhysicsPrim->IsSimulatingPhysics())
	{
		const float ObjectMass = PhysicsPrim->GetMass();
		if (ObjectMass < MinObjectMassToAnchorPlayer)
		{
			// Light/Medium movable props CANNOT be used as wall anchors to pull/drag or launch the astronaut!
			return false;
		}
	}

	UWorld* World = GetWorld();
	GrabStartTime = World ? World->GetTimeSeconds() : 0.0f;
	bIsGrabbingWall = true;
	GrabbedWorldPoint = Hit.ImpactPoint;
	GrabbedSurfaceNormal = Hit.ImpactNormal.GetSafeNormal();
	GrabInitialLookDir = GetCameraAimDirection();
	GrabbedComponent = Hit.GetComponent();

	// Store the grab point and surface normal in the grabbed component's LOCAL space!
	if (GrabbedComponent.IsValid())
	{
		const FTransform& CompTransform = GrabbedComponent->GetComponentTransform();
		GrabbedLocalPoint = CompTransform.InverseTransformPosition(GrabbedWorldPoint);
		GrabbedLocalNormal = CompTransform.InverseTransformVectorNoScale(GrabbedSurfaceNormal).GetSafeNormal();
	}

	float AstronautMassShare = 1.0f;
	if (GrabbedComponent.IsValid() && GrabbedComponent->IsSimulatingPhysics())
	{
		const float ObjectMass = FMath::Max(0.05f, GrabbedComponent->GetMass());
		const float TotalMass = TraineeBodyMassKg + ObjectMass;
		AstronautMassShare = FMath::Clamp(ObjectMass / TotalMass, 0.0f, 1.0f);
	}

	if (bDampenExistingVelocityOnGrab)
	{
		OwnerMovementComp->Velocity *= (1.0f - FMath::Clamp(GrabVelocityDampingFactor * AstronautMassShare, 0.0f, 1.0f));
	}

	OnZeroGWallInteraction.Broadcast(TEXT("wall_grab_start"), Hit.GetActor(), GrabbedWorldPoint, 0.0f);
	return true;
}

AActor* USpaceFloatingComponent::GetGrabbedSurfaceActor() const
{
	return (bIsGrabbingWall && GrabbedComponent.IsValid()) ? GrabbedComponent->GetOwner() : nullptr;
}

FVector USpaceFloatingComponent::ComputeGrabLaunchDirection() const
{
	const FVector CurrentLookDir = GetCameraAimDirection().GetSafeNormal();

	if (!bLaunchTowardCameraLookOnRelease)
	{
		const FVector DragDelta = CurrentLookDir - GrabInitialLookDir;
		if (DragDelta.SizeSquared() > 0.04f)
		{
			FVector SlingshotDir = (-DragDelta + (GrabbedSurfaceNormal * 0.35f)).GetSafeNormal();
			if (FVector::DotProduct(SlingshotDir, GrabbedSurfaceNormal) < 0.05f)
			{
				SlingshotDir = FVector::VectorPlaneProject(SlingshotDir, GrabbedSurfaceNormal) + (GrabbedSurfaceNormal * 0.25f);
			}
			return SlingshotDir.GetSafeNormal();
		}
	}

	FVector AimDir = CurrentLookDir;

	const float DotWithWallNormal = FVector::DotProduct(AimDir, GrabbedSurfaceNormal);
	if (DotWithWallNormal < -0.25f)
	{
		FVector TangentialDir = FVector::VectorPlaneProject(AimDir, GrabbedSurfaceNormal);
		if (TangentialDir.SizeSquared() > 0.05f)
		{
			AimDir = (TangentialDir.GetSafeNormal() * 0.85f + GrabbedSurfaceNormal * 0.35f).GetSafeNormal();
		}
		else
		{
			AimDir = GrabbedSurfaceNormal;
		}
	}

	return AimDir.GetSafeNormal();
}

void USpaceFloatingComponent::UpdateGrabHoldState(float DeltaTime)
{
	if (!bIsGrabbingWall || !OwnerCharacter || !OwnerMovementComp)
	{
		return;
	}

	// If the grabbed component was destroyed, release grab immediately!
	if (!GrabbedComponent.IsValid())
	{
		bIsGrabbingWall = false;
		return;
	}

	UWorld* World = GetWorld();
	const float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;
	const float HoldDuration = CurrentTime - GrabStartTime;

	// 1. Update live World Position & Normal of the grabbed spot from the component's current Transform!
	// So if the object moves or rotates, the grab point moves with the object!
	const FTransform& CompTransform = GrabbedComponent->GetComponentTransform();
	GrabbedWorldPoint = CompTransform.TransformPosition(GrabbedLocalPoint);
	GrabbedSurfaceNormal = CompTransform.TransformVectorNoScale(GrabbedLocalNormal).GetSafeNormal();

	const FVector HandOrigin = OwnerCamera ? OwnerCamera->GetComponentLocation() : OwnerCharacter->GetActorLocation();
	const float CurrentDistToGrabPoint = FVector::Dist(HandOrigin, GrabbedWorldPoint);

	// 2. Break grab automatically if the moving object gets knocked too far beyond arm's reach!
	if (CurrentDistToGrabPoint > MaxGrabStretchDistance)
	{
		bIsGrabbingWall = false;
		return;
	}

	const bool bIsMovablePhysicsObject = GrabbedComponent->IsSimulatingPhysics();

	// 3. Physics-Accurate Mass Interaction (Only Static Walls or Heavy Anchors >= MinObjectMassToAnchorPlayer can reach here)
	if (bIsMovablePhysicsObject)
	{
		const float ObjectMass = FMath::Max(0.05f, GrabbedComponent->GetMass());
		if (ObjectMass < MinObjectMassToAnchorPlayer)
		{
			bIsGrabbingWall = false;
			return;
		}

		const float TotalMass = TraineeBodyMassKg + ObjectMass;
		const float AstronautPullRatio = FMath::Clamp(ObjectMass / TotalMass, 0.0f, 1.0f);

		const FVector TargetAnchorLocation = GrabbedWorldPoint + (GrabbedSurfaceNormal * GrabHoldAnchorDistance);
		const FVector ToAnchor = TargetAnchorLocation - OwnerCharacter->GetActorLocation();
		const FVector CurrentObjVel = GrabbedComponent->GetPhysicsLinearVelocity();
		const FVector DesiredAstronautVel = CurrentObjVel + (ToAnchor * GrabAnchorPullSpeed);

		OwnerMovementComp->Velocity = FMath::VInterpTo(
			OwnerMovementComp->Velocity,
			DesiredAstronautVel.GetClampedToMaxSize(PullImpulseStrength),
			DeltaTime,
			6.0f * AstronautPullRatio
		);
	}
	else
	{
		// Static Immovable Station Wall / Handrail: Anchors the astronaut 100% firmly!
		const FVector TargetAnchorLocation = GrabbedWorldPoint + (GrabbedSurfaceNormal * GrabHoldAnchorDistance);
		const FVector ToAnchor = TargetAnchorLocation - OwnerCharacter->GetActorLocation();

		FVector DesiredHoldVelocity = ToAnchor * GrabAnchorPullSpeed;
		if (DesiredHoldVelocity.SizeSquared() > FMath::Square(PullImpulseStrength))
		{
			DesiredHoldVelocity = DesiredHoldVelocity.GetClampedToMaxSize(PullImpulseStrength);
		}

		if (bAllowDirectionalSteerWhileGrabbing)
		{
			if (APlayerController* PC = Cast<APlayerController>(OwnerCharacter->GetController()))
			{
				float ForwardKey = 0.0f;
				if (PC->IsInputKeyDown(EKeys::W) || PC->IsInputKeyDown(EKeys::Up)) ForwardKey += 1.0f;
				if (PC->IsInputKeyDown(EKeys::S) || PC->IsInputKeyDown(EKeys::Down)) ForwardKey -= 1.0f;

				float RightKey = 0.0f;
				if (PC->IsInputKeyDown(EKeys::D) || PC->IsInputKeyDown(EKeys::Right)) RightKey += 1.0f;
				if (PC->IsInputKeyDown(EKeys::A) || PC->IsInputKeyDown(EKeys::Left)) RightKey -= 1.0f;

				if (!FMath::IsNearlyZero(ForwardKey) || !FMath::IsNearlyZero(RightKey))
				{
					const FRotator AimRot = PC->GetControlRotation();
					const FRotationMatrix AimMat(AimRot);
					const FVector SteerVec = (AimMat.GetUnitAxis(EAxis::X) * ForwardKey + AimMat.GetUnitAxis(EAxis::Y) * RightKey).GetSafeNormal();
					const FVector WallSlideVec = FVector::VectorPlaneProject(SteerVec, GrabbedSurfaceNormal);
					const FVector StepDelta = WallSlideVec * (PullImpulseStrength * 0.5f) * DeltaTime;
					GrabbedWorldPoint += StepDelta;
					GrabbedLocalPoint = CompTransform.InverseTransformPosition(GrabbedWorldPoint);

					TotalWallWalkDistanceCm += StepDelta.Size();
					if (CurrentTime - LastWallWalkBroadcastTime >= 1.2f)
					{
						LastWallWalkBroadcastTime = CurrentTime;
						OnZeroGWallInteraction.Broadcast(
							TEXT("wall_walk"),
							GrabbedComponent->GetOwner(),
							GrabbedWorldPoint,
							TotalWallWalkDistanceCm / 100.0f
						);
					}
				}
			}
		}

		OwnerMovementComp->Velocity = FMath::VInterpTo(OwnerMovementComp->Velocity, DesiredHoldVelocity, DeltaTime, 10.0f);
	}

	if (bDrawHandReachDebug && World)
	{
		DrawDebugLine(World, OwnerCharacter->GetActorLocation(), GrabbedWorldPoint, FColor::Yellow, false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, GrabbedWorldPoint, 8.0f, 12, FColor::Yellow, false, -1.0f, 0, 2.0f);

		if (HoldDuration >= GrabHoldThresholdSeconds)
		{
			const FVector LaunchDir = ComputeGrabLaunchDirection();
			const FVector ArrowStart = OwnerCharacter->GetActorLocation() + (LaunchDir * 30.0f);
			const FVector ArrowEnd = ArrowStart + (LaunchDir * 140.0f);
			DrawDebugDirectionalArrow(World, ArrowStart, ArrowEnd, 30.0f, FColor::Emerald, false, -1.0f, 0, 3.0f);
		}
	}
}

void USpaceFloatingComponent::ReleaseWallGrabAndLaunch()
{
	if (!bIsGrabbingWall || !OwnerCharacter || !OwnerMovementComp)
	{
		return;
	}

	UWorld* World = GetWorld();
	const float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;
	const float HoldDuration = CurrentTime - GrabStartTime;

	bIsGrabbingWall = false;
	LastHandActionTime = CurrentTime;

	// Update final world grab point
	if (GrabbedComponent.IsValid())
	{
		GrabbedWorldPoint = GrabbedComponent->GetComponentTransform().TransformPosition(GrabbedLocalPoint);
	}

	// Calculate Newton's 3rd Law Mass Split between Astronaut (80kg) and Grabbed Object:
	// - Static Wall: Astronaut gets 100% of launch velocity.
	// - Small/Light Prop (e.g., 0.25kg paper or 8kg chair): Pushing/throwing it throws the prop fast, and gives the astronaut a proportional recoil!
	// - Heavy Cargo Crate (65kg): Gives the astronaut a strong launch and pushes the crate back proportionally!
	float AstronautMassShare = 1.0f;
	float ObjectRecoilShare = 0.0f;

	const bool bIsPhysicsProp = GrabbedComponent.IsValid() && GrabbedComponent->IsSimulatingPhysics();
	if (bIsPhysicsProp)
	{
		const float ObjectMass = FMath::Max(0.05f, GrabbedComponent->GetMass());
		const float TotalMass = TraineeBodyMassKg + ObjectMass;
		AstronautMassShare = FMath::Clamp(ObjectMass / TotalMass, 0.05f, 1.0f);
		ObjectRecoilShare = FMath::Clamp(TraineeBodyMassKg / TotalMass, 0.1f, 1.6f);
	}

	// Case A: Quick Tap on LMB (< GrabHoldThresholdSeconds) -> Pull!
	if (HoldDuration < GrabHoldThresholdSeconds)
	{
		const FVector PullDirection = (GrabbedWorldPoint - OwnerCharacter->GetActorLocation()).GetSafeNormal();
		if (!PullDirection.IsNearlyZero())
		{
			// Astronaut pulls themselves toward the object (scaled by how heavy the object is)
			const FVector AstronautPullVel = PullDirection * (PullImpulseStrength * AstronautMassShare);
			OwnerMovementComp->AddImpulse(AstronautPullVel, true);

			// And if it's a movable prop, it gets pulled toward the astronaut (light objects come flying right to your hand!)
			if (bIsPhysicsProp)
			{
				GrabbedComponent->WakeAllRigidBodies();
				const FVector PropPullVel = -PullDirection * (PullImpulseStrength * ObjectRecoilShare * 0.75f);
				GrabbedComponent->SetPhysicsLinearVelocity(
					GrabbedComponent->GetPhysicsLinearVelocity() + PropPullVel,
					false
				);
			}

			OnZeroGHandAction.Broadcast(true, GrabbedWorldPoint, AstronautPullVel);
			OnZeroGWallInteraction.Broadcast(
				TEXT("wall_pull"),
				GrabbedComponent.IsValid() ? GrabbedComponent->GetOwner() : nullptr,
				GrabbedWorldPoint,
				AstronautPullVel.Size()
			);
		}
		return;
	}

	// Case B: Held LMB & Released -> Aimed Launch (or Throw Object + Recoil if holding a small physics prop!)
	const FVector LaunchDirection = ComputeGrabLaunchDirection();
	if (!LaunchDirection.IsNearlyZero())
	{
		if (!bIsPhysicsProp)
		{
			// Static Wall: Full clean launch in the aimed direction
			OwnerMovementComp->Velocity = FVector::ZeroVector;
			const FVector LaunchImpulse = LaunchDirection * PushImpulseStrength;
			OwnerMovementComp->AddImpulse(LaunchImpulse, true);
			OnZeroGHandAction.Broadcast(false, GrabbedWorldPoint, LaunchImpulse);
			OnZeroGWallInteraction.Broadcast(
				TEXT("wall_launch"),
				GrabbedComponent.IsValid() ? GrabbedComponent->GetOwner() : nullptr,
				GrabbedWorldPoint,
				LaunchImpulse.Size()
			);
		}
		else
		{
			// Movable Physics Object:
			// If it's a small/medium object (like a chair/box/paper), aiming and releasing throws/pushes the object
			// AND pushes the astronaut according to Newton's 3rd Law (m1 * dv1 = -m2 * dv2)!
			const FVector AstronautLaunchVel = LaunchDirection * (PushImpulseStrength * AstronautMassShare);
			OwnerMovementComp->AddImpulse(AstronautLaunchVel, true);

			GrabbedComponent->WakeAllRigidBodies();
			const FVector ObjectThrowVel = -LaunchDirection * (PushImpulseStrength * ObjectRecoilShare * 0.85f);
			GrabbedComponent->SetPhysicsLinearVelocity(
				GrabbedComponent->GetPhysicsLinearVelocity() + ObjectThrowVel,
				false
			);

			OnZeroGHandAction.Broadcast(false, GrabbedWorldPoint, AstronautLaunchVel);
			OnZeroGWallInteraction.Broadcast(
				TEXT("wall_launch"),
				GrabbedComponent.IsValid() ? GrabbedComponent->GetOwner() : nullptr,
				GrabbedWorldPoint,
				AstronautLaunchVel.Size()
			);
		}

		if (bDrawHandReachDebug && World)
		{
			DrawDebugDirectionalArrow(
				World,
				OwnerCharacter->GetActorLocation(),
				OwnerCharacter->GetActorLocation() + (LaunchDirection * 160.0f * AstronautMassShare),
				30.0f,
				FColor::Cyan,
				false,
				1.2f,
				0,
				3.0f
			);
		}
	}
}

void USpaceFloatingComponent::CancelGrabAndStopInPlace()
{
	if (!bIsFloatingActive || !OwnerCharacter || !OwnerMovementComp)
	{
		return;
	}

	// If holding a Grabbable object in hand, RMB is used to Throw that object — do not brake the player!
	if (USpaceGrabSnapComponent* GrabSnapComp = OwnerCharacter->FindComponentByClass<USpaceGrabSnapComponent>())
	{
		if (GrabSnapComp->IsHoldingObject())
		{
			return;
		}
	}

	// Only a STATIC surface (or very heavy object) can stop the astronaut completely in place!
	// Holding a 0.2kg piece of paper in mid-air cannot magically brake an 80kg astronaut!
	if (bIsGrabbingWall)
	{
		bIsGrabbingWall = false;
		if (!GrabbedComponent.IsValid() || !GrabbedComponent->IsSimulatingPhysics())
		{
			// Grabbing a static wall -> full stop in place
			OwnerMovementComp->StopMovementImmediately();
			OwnerMovementComp->Velocity = FVector::ZeroVector;
			OnZeroGWallInteraction.Broadcast(
				TEXT("wall_stop_brake"),
				GrabbedComponent.IsValid() ? GrabbedComponent->GetOwner() : nullptr,
				GrabbedWorldPoint,
				0.0f
			);
		}
		else
		{
			// Grabbing a movable physics object -> just let go of the object smoothly without stopping in mid-air!
			const float ObjectMass = FMath::Max(0.05f, GrabbedComponent->GetMass());
			const float BrakeRatio = FMath::Clamp(ObjectMass / (TraineeBodyMassKg + ObjectMass), 0.0f, 0.7f);
			OwnerMovementComp->Velocity *= (1.0f - BrakeRatio);
		}
		return;
	}

	FHitResult Hit;
	if (PerformHandReachTrace(Hit))
	{
		AActor* HitActor = Hit.GetActor();
		bool bIsGrabbableOrFloatingProp = false;
		if (HitActor)
		{
			if (HitActor->ActorHasTag(FName("Grabbable")) || HitActor->FindComponentByClass<USpaceObjectComponent>() != nullptr)
			{
				bIsGrabbableOrFloatingProp = true;
			}
			else
			{
				for (const UActorComponent* SubComp : HitActor->GetComponents())
				{
					if (SubComp && SubComp->ComponentHasTag(FName("Grabbable")))
					{
						bIsGrabbableOrFloatingProp = true;
						break;
					}
				}
			}
		}

		if (!bIsGrabbableOrFloatingProp && (!Hit.GetComponent() || !Hit.GetComponent()->IsSimulatingPhysics()))
		{
			OwnerMovementComp->StopMovementImmediately();
			OwnerMovementComp->Velocity = FVector::ZeroVector;
			OnZeroGWallInteraction.Broadcast(TEXT("wall_stop_brake"), HitActor, Hit.ImpactPoint, 0.0f);
		}
	}
}

// =============================================================================
// Requirement 3: Pull Mechanic (Direct Callable)
// =============================================================================
bool USpaceFloatingComponent::TryHandPull()
{
	if (!bIsFloatingActive || !OwnerCharacter || !OwnerMovementComp)
	{
		return false;
	}

	UWorld* World = GetWorld();
	const float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;
	if (CurrentTime - LastHandActionTime < HandActionCooldown)
	{
		return false;
	}

	FHitResult Hit;
	if (!PerformHandReachTrace(Hit))
	{
		return false;
	}

	LastHandActionTime = CurrentTime;

	const FVector PullDirection = (Hit.ImpactPoint - OwnerCharacter->GetActorLocation()).GetSafeNormal();
	if (PullDirection.IsNearlyZero())
	{
		return false;
	}

	if (bDampenExistingVelocityOnGrab)
	{
		OwnerMovementComp->Velocity *= (1.0f - FMath::Clamp(GrabVelocityDampingFactor, 0.0f, 1.0f));
	}

	const FVector PullImpulse = PullDirection * PullImpulseStrength;
	OwnerMovementComp->AddImpulse(PullImpulse, true);

	if (bApplyNewtonianReactionToPhysicsProps && Hit.GetComponent() && Hit.GetComponent()->IsSimulatingPhysics())
	{
		Hit.GetComponent()->WakeAllRigidBodies();
		Hit.GetComponent()->AddImpulseAtLocation(-PullImpulse * 0.5f, Hit.ImpactPoint);
	}

	if (bDrawHandReachDebug && World)
	{
		DrawDebugDirectionalArrow(World, OwnerCharacter->GetActorLocation(), Hit.ImpactPoint, 25.0f, FColor::Green, false, 1.0f, 0, 2.5f);
	}

	OnZeroGHandAction.Broadcast(true, Hit.ImpactPoint, PullImpulse);
	return true;
}

// =============================================================================
// Requirement 4: Push Mechanic (Right Mouse Button)
// - If currently holding a wall with LMB, pressing RMB immediately launches you in your aimed direction!
// - Otherwise, if looking at a wall within reach, pushes off the wall (-ImpactNormal blended with camera aim).
// =============================================================================
bool USpaceFloatingComponent::TryHandPush()
{
	if (!bIsFloatingActive || !OwnerCharacter || !OwnerMovementComp)
	{
		return false;
	}

	// If the trainee is currently holding onto a wall with LMB and clicks RMB, launch them immediately toward where they are looking!
	if (bIsGrabbingWall)
	{
		UWorld* World = GetWorld();
		GrabStartTime = (World ? World->GetTimeSeconds() : 0.0f) - (GrabHoldThresholdSeconds + 1.0f);
		ReleaseWallGrabAndLaunch();
		return true;
	}

	UWorld* World = GetWorld();
	const float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;
	if (CurrentTime - LastHandActionTime < HandActionCooldown)
	{
		return false;
	}

	FHitResult Hit;
	if (!PerformHandReachTrace(Hit))
	{
		return false;
	}

	LastHandActionTime = CurrentTime;

	FVector SurfaceNormal = Hit.ImpactNormal.GetSafeNormal();
	const FVector ViewForward = GetCameraAimDirection();
	if (FVector::DotProduct(SurfaceNormal, ViewForward) > 0.0f)
	{
		SurfaceNormal = -SurfaceNormal;
	}

	// Reflect/blend the camera look vector off the wall normal so angled pushes let you push off diagonally in any direction!
	const FVector ReflectedLook = FMath::GetReflectionVector(ViewForward, SurfaceNormal).GetSafeNormal();
	const FVector PushDirection = (SurfaceNormal * 0.55f + ReflectedLook * 0.45f).GetSafeNormal();

	if (bDampenExistingVelocityOnGrab)
	{
		OwnerMovementComp->Velocity *= (1.0f - FMath::Clamp(GrabVelocityDampingFactor, 0.0f, 1.0f));
	}

	const FVector PushImpulse = PushDirection * PushImpulseStrength;
	OwnerMovementComp->AddImpulse(PushImpulse, true);

	if (bApplyNewtonianReactionToPhysicsProps && Hit.GetComponent() && Hit.GetComponent()->IsSimulatingPhysics())
	{
		Hit.GetComponent()->WakeAllRigidBodies();
		Hit.GetComponent()->AddImpulseAtLocation(-PushImpulse * 0.75f, Hit.ImpactPoint);
	}

	if (bDrawHandReachDebug && World)
	{
		DrawDebugDirectionalArrow(
			World,
			Hit.ImpactPoint,
			Hit.ImpactPoint + (PushDirection * 120.0f),
			25.0f,
			FColor::Orange,
			false,
			1.0f,
			0,
			2.5f
		);
	}

	OnZeroGHandAction.Broadcast(false, Hit.ImpactPoint, PushImpulse);
	return true;
}

void USpaceFloatingComponent::OnPropComponentHit(
	UPrimitiveComponent* HitComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	if (!bIsFloatingActive || !OwnerRootPrimitive || !OwnerRootPrimitive->IsSimulatingPhysics())
	{
		return;
	}

	if (!OtherActor || OtherActor == GetOwner())
	{
		return;
	}

	if (ACharacter* BumpingCharacter = Cast<ACharacter>(OtherActor))
	{
		UWorld* World = GetWorld();
		const float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;
		if (CurrentTime - LastBumpTime < 0.08f)
		{
			return;
		}
		LastBumpTime = CurrentTime;

		const FVector CharVel = BumpingCharacter->GetVelocity();
		const FVector PushDir = (OwnerRootPrimitive->GetComponentLocation() - BumpingCharacter->GetActorLocation()).GetSafeNormal();

		const float PushSpeed = FMath::Max(CharVel.Size() * CharacterBumpPushStrength, MinCharacterBumpSpeed);
		const FVector ImpulseVel = (CharVel * CharacterBumpPushStrength * 0.65f) + (PushDir * (PushSpeed * 0.55f));

		OwnerRootPrimitive->WakeAllRigidBodies();
		OwnerRootPrimitive->AddImpulse(ImpulseVel, NAME_None, true);

		if (CharacterBumpSpinStrength > 0.0f)
		{
			const FVector OffsetFromCenter = Hit.ImpactPoint - OwnerRootPrimitive->GetCenterOfMass();
			FVector SpinAxis = FVector::CrossProduct(OffsetFromCenter, ImpulseVel).GetSafeNormal();
			if (SpinAxis.IsNearlyZero())
			{
				SpinAxis = FMath::VRand();
			}
			const FVector CurrentAngVel = OwnerRootPrimitive->GetPhysicsAngularVelocityInDegrees();
			OwnerRootPrimitive->SetPhysicsAngularVelocityInDegrees(
				CurrentAngVel + (SpinAxis * CharacterBumpSpinStrength),
				false
			);
		}
	}
}

void USpaceFloatingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsFloatingActive)
	{
		return;
	}

	RunningTime += DeltaTime;

	if (OwnerCharacter && OwnerMovementComp)
	{
		EnforceZeroGOffGround(DeltaTime);

		bHasSurfaceInReach = PerformHandReachTrace(CurrentHandReachHit);

		Process6DoFCharacterControls(DeltaTime);

		if (bIsGrabbingWall)
		{
			UpdateGrabHoldState(DeltaTime);
		}
		else if (bEnableStrandedAntiStuckDrift)
		{
			EnforceStrandedMinimumDrift(DeltaTime);
		}
	}

	UpdateProceduralFloating(DeltaTime);

	FrameRollInput = 0.0f;
}

void USpaceFloatingComponent::EnforceZeroGOffGround(float DeltaTime)
{
	if (!OwnerCharacter || !OwnerMovementComp)
	{
		return;
	}

	OwnerMovementComp->GravityScale = 0.0f;
	OwnerMovementComp->GroundFriction = ZeroGGroundFriction;
	OwnerMovementComp->BrakingDecelerationFlying = ZeroGBrakingDeceleration;
	OwnerMovementComp->BrakingFrictionFactor = ZeroGFluidFriction;
	OwnerMovementComp->SetWalkableFloorZ(1.01f);
	OwnerMovementComp->CurrentFloor.Clear();

	if (OwnerMovementComp->MovementMode != MOVE_Flying)
	{
		OwnerMovementComp->SetMovementMode(MOVE_Flying);
	}

	OwnerCharacter->StopJumping();

	if (bPreventGroundContact && MinFloorClearance > 0.0f && !bIsGrabbingWall)
	{
		UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
		UWorld* World = GetWorld();
		if (Capsule && World)
		{
			const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
			const FVector ActorLoc = OwnerCharacter->GetActorLocation();
			const FVector TraceStart = ActorLoc;
			const FVector TraceEnd = ActorLoc - FVector(0.0f, 0.0f, CapsuleHalfHeight + MinFloorClearance);

			FHitResult Hit;
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SpaceFloorCheck), false, OwnerCharacter);

			if (World->SweepSingleByChannel(
				Hit,
				TraceStart,
				TraceEnd,
				FQuat::Identity,
				ECC_Visibility,
				FCollisionShape::MakeSphere(CapsuleRadius * 0.65f),
				QueryParams))
			{
				const float DistanceToFloor = Hit.Distance;
				if (DistanceToFloor < MinFloorClearance)
				{
					const float PenetrationRatio = FMath::Clamp(1.0f - (DistanceToFloor / MinFloorClearance), 0.0f, 1.0f);
					if (OwnerMovementComp->Velocity.Z < 0.0f)
					{
						OwnerMovementComp->Velocity.Z = FMath::FInterpTo(OwnerMovementComp->Velocity.Z, 10.0f, DeltaTime, 10.0f);
					}
					OwnerMovementComp->Velocity.Z += FloorRepulsionStrength * PenetrationRatio * DeltaTime;
				}
			}
		}
	}
}

bool USpaceFloatingComponent::IsStrandedInOpenSpace(float CheckRadius) const
{
	if (!OwnerCharacter || !bIsFloatingActive)
	{
		return false;
	}

	if (bIsGrabbingWall)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// If the hand forward trace already found a reachable surface right in front of the astronaut, not stranded!
	if (bHasSurfaceInReach)
	{
		return false;
	}

	const float Radius = (CheckRadius > 0.0f) ? CheckRadius : HandReachDistance;
	const FVector PlayerCenter = OwnerCharacter->GetPawnViewLocation();

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ZeroGStrandedCheck), false, OwnerCharacter);
	if (const USpaceGrabSnapComponent* GrabSnapComp = OwnerCharacter->FindComponentByClass<USpaceGrabSnapComponent>())
	{
		if (AActor* Held = GrabSnapComp->GetHeldActor())
		{
			QueryParams.AddIgnoredActor(Held);
		}
	}

	FHitResult HitResult;
	const bool bHitNearbySurface = World->SweepSingleByChannel(
		HitResult,
		PlayerCenter,
		PlayerCenter + FVector(0.0f, 0.0f, 1.0f),
		FQuat::Identity,
		HandTraceChannel,
		FCollisionShape::MakeSphere(Radius),
		QueryParams
	);

	return !bHitNearbySurface;
}

void USpaceFloatingComponent::EnforceStrandedMinimumDrift(float DeltaTime)
{
	if (!OwnerCharacter || !OwnerMovementComp || !bIsFloatingActive || bIsGrabbingWall)
	{
		return;
	}

	if (!IsStrandedInOpenSpace())
	{
		return;
	}

	const float EffectiveMinSpeed = FMath::Max(0.01f, MinStrandedDriftSpeed);
	const float CurrentSpeed = OwnerMovementComp->Velocity.Size();

	// If the astronaut is stationary or drifting slower than the minimum required speed
	if (CurrentSpeed < EffectiveMinSpeed)
	{
		FVector DriftDirection = OwnerMovementComp->Velocity.GetSafeNormal();

		// If stopped in place (velocity ~ 0), smoothly drift toward the camera aim direction!
		if (DriftDirection.IsNearlyZero(0.001f))
		{
			DriftDirection = GetCameraAimDirection();
			if (DriftDirection.IsNearlyZero(0.001f))
			{
				DriftDirection = OwnerCharacter->GetActorForwardVector();
			}
		}

		const FVector TargetVelocity = DriftDirection * EffectiveMinSpeed;
		OwnerMovementComp->Velocity = FMath::VInterpTo(OwnerMovementComp->Velocity, TargetVelocity, DeltaTime, 4.0f);

		// Guarantee minimum speed never drops below 0.01 / EffectiveMinSpeed
		if (OwnerMovementComp->Velocity.Size() < EffectiveMinSpeed * 0.75f)
		{
			OwnerMovementComp->Velocity = DriftDirection * EffectiveMinSpeed;
		}
	}
}

void USpaceFloatingComponent::Process6DoFCharacterControls(float DeltaTime)
{
	if (!OwnerCharacter || !OwnerMovementComp)
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(OwnerCharacter->GetController());
	if (!PC)
	{
		return;
	}

	if (PC->IsLocalController())
	{
		OwnerCharacter->ConsumeMovementInputVector();

		if (bUseDirectKeyBindings)
		{
			// Left Mouse Button (LMB):
			// - Press: Grab onto the surface within arm reach
			// - Hold: Stay anchored to the wall while you look/aim in any 3D direction (or climb with WASD)
			// - Release:
			//     * Quick tap (< 0.18s): Pulls you directly toward the grabbed point
			//     * Hold & Release: Launches you in the exact 3D direction you aimed!
			if (PC->WasInputKeyJustPressed(EKeys::LeftMouseButton) || PC->WasInputKeyJustPressed(EKeys::Gamepad_LeftTrigger))
			{
				StartWallGrab();
			}

			if (bIsGrabbingWall && (PC->WasInputKeyJustReleased(EKeys::LeftMouseButton) || PC->WasInputKeyJustReleased(EKeys::Gamepad_LeftTrigger)))
			{
				ReleaseWallGrabAndLaunch();
			}

			// Cancel Grab / Brake & Stay Stationary in Place: Use 'X' key, Gamepad, or RMB (if not holding an item)
			if (PC->WasInputKeyJustPressed(EKeys::X) || PC->WasInputKeyJustPressed(EKeys::Gamepad_RightTrigger))
			{
				CancelGrabAndStopInPlace();
			}
			else if (PC->WasInputKeyJustPressed(EKeys::RightMouseButton))
			{
				bool bHoldingItem = false;
				if (const USpaceGrabSnapComponent* GrabComp = OwnerCharacter->FindComponentByClass<USpaceGrabSnapComponent>())
				{
					bHoldingItem = GrabComp->IsHoldingObject();
				}
				if (!bHoldingItem && bIsGrabbingWall)
				{
					CancelGrabAndStopInPlace();
				}
			}

			const bool bCanUseThrusters = (!bDisableWASDWalking) || (bAllowEmergencyThrustersWhenStranded && IsStrandedInOpenSpace());
			if (bCanUseThrusters && bEnable6DoFControls && !bIsGrabbingWall)
			{
				float ForwardKey = 0.0f;
				if (PC->IsInputKeyDown(EKeys::W) || PC->IsInputKeyDown(EKeys::Up)) ForwardKey += 1.0f;
				if (PC->IsInputKeyDown(EKeys::S) || PC->IsInputKeyDown(EKeys::Down)) ForwardKey -= 1.0f;

				float RightKey = 0.0f;
				if (PC->IsInputKeyDown(EKeys::D) || PC->IsInputKeyDown(EKeys::Right)) RightKey += 1.0f;
				if (PC->IsInputKeyDown(EKeys::A) || PC->IsInputKeyDown(EKeys::Left)) RightKey -= 1.0f;

				if (!FMath::IsNearlyZero(ForwardKey) || !FMath::IsNearlyZero(RightKey))
				{
					MoveInSpace(RightKey, ForwardKey);
				}

				float UpKey = 0.0f;
				if (PC->IsInputKeyDown(EKeys::SpaceBar)) UpKey += 1.0f;
				if (PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::C)) UpKey -= 1.0f;
				if (!FMath::IsNearlyZero(UpKey))
				{
					AddVerticalSpaceInput(UpKey);
				}
			}
		}
	}

	if (!FMath::IsNearlyZero(FrameRollInput))
	{
		CurrentRollAngle = FMath::UnwindDegrees(CurrentRollAngle + FrameRollInput * RollRotationSpeed * DeltaTime);
		FRotator CtrlRot = PC->GetControlRotation();
		CtrlRot.Roll = CurrentRollAngle;
		PC->SetControlRotation(CtrlRot);
	}
}

void USpaceFloatingComponent::MoveInSpace(float RightInput, float ForwardInput)
{
	if (!OwnerCharacter)
	{
		return;
	}

	const bool bCanMove = (!bDisableWASDWalking) || (bAllowEmergencyThrustersWhenStranded && IsStrandedInOpenSpace());
	if (!bCanMove)
	{
		return;
	}

	AController* Controller = OwnerCharacter->GetController();
	if (!Controller)
	{
		return;
	}

	FRotator SpaceAimRotation = Controller->GetControlRotation();
	SpaceAimRotation.Roll = CurrentRollAngle;

	const FRotationMatrix AimMatrix(SpaceAimRotation);

	if (!FMath::IsNearlyZero(ForwardInput))
	{
		OwnerCharacter->AddMovementInput(AimMatrix.GetUnitAxis(EAxis::X), ForwardInput);
	}

	if (!FMath::IsNearlyZero(RightInput))
	{
		OwnerCharacter->AddMovementInput(AimMatrix.GetUnitAxis(EAxis::Y), RightInput);
	}
}

void USpaceFloatingComponent::AddVerticalSpaceInput(float UpInput)
{
	if (!OwnerCharacter || FMath::IsNearlyZero(UpInput))
	{
		return;
	}

	const bool bCanMove = (!bDisableWASDWalking) || (bAllowEmergencyThrustersWhenStranded && IsStrandedInOpenSpace());
	if (!bCanMove)
	{
		return;
	}

	if (AController* Controller = OwnerCharacter->GetController())
	{
		FRotator SpaceAimRotation = Controller->GetControlRotation();
		SpaceAimRotation.Roll = CurrentRollAngle;
		const FVector LocalUpDir = FRotationMatrix(SpaceAimRotation).GetUnitAxis(EAxis::Z);
		OwnerCharacter->AddMovementInput(LocalUpDir, UpInput);
	}
}

void USpaceFloatingComponent::AddRollSpaceInput(float RollInput)
{
	FrameRollInput = FMath::Clamp(FrameRollInput + RollInput, -1.0f, 1.0f);
}

void USpaceFloatingComponent::ResetRollOrientation()
{
	CurrentRollAngle = 0.0f;
	if (OwnerCharacter)
	{
		if (APlayerController* PC = Cast<APlayerController>(OwnerCharacter->GetController()))
		{
			FRotator CtrlRot = PC->GetControlRotation();
			CtrlRot.Roll = 0.0f;
			PC->SetControlRotation(CtrlRot);
		}
	}
}

void USpaceFloatingComponent::UpdateCharacter3DOrientation(float DeltaTime, const FRotator& ProceduralRotationOffset)
{
	if (!OwnerCharacter)
	{
		return;
	}

	AController* Controller = OwnerCharacter->GetController();
	if (bAlignCharacterBodyToCamera && Controller)
	{
		const FRotator ControlRot = Controller->GetControlRotation();
		const float NormalizedPitch = FRotator::NormalizeAxis(ControlRot.Pitch);

		FRotator TargetActorRot;
		TargetActorRot.Yaw = ControlRot.Yaw;
		TargetActorRot.Pitch = NormalizedPitch * PitchAlignmentFactor;
		TargetActorRot.Roll = FRotator::NormalizeAxis(CurrentRollAngle);

		const FQuat CurrentQuat = OwnerCharacter->GetActorQuat();
		const FQuat TargetQuat = TargetActorRot.Quaternion();
		const FQuat SmoothQuat = FMath::QInterpTo(CurrentQuat, TargetQuat, DeltaTime, BodyAlignmentInterpSpeed);

		OwnerCharacter->SetActorRotation(SmoothQuat);
	}

	if (VisualTargetComponent)
	{
		if (bEnableProceduralBobbing)
		{
			VisualTargetComponent->SetRelativeRotation(BaseRelativeRotation + ProceduralRotationOffset);
		}
		else
		{
			VisualTargetComponent->SetRelativeRotation(BaseRelativeRotation);
		}
	}
}

void USpaceFloatingComponent::UpdateProceduralFloating(float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const float WaveX = FMath::Sin(RunningTime * BobbingSpeed * 0.7f);
	const float WaveY = FMath::Cos(RunningTime * BobbingSpeed * 0.85f);
	const float WaveZ = FMath::Sin(RunningTime * BobbingSpeed);

	const FVector LocationOffset(
		WaveX * BobbingAmplitude.X,
		WaveY * BobbingAmplitude.Y,
		WaveZ * BobbingAmplitude.Z
	);

	const float PitchWave = FMath::Sin(RunningTime * RotationSwaySpeed * 0.9f);
	const float YawWave = FMath::Cos(RunningTime * RotationSwaySpeed * 0.6f);
	const float RollWave = FMath::Sin(RunningTime * RotationSwaySpeed * 1.1f);

	const FRotator RotationOffset(
		PitchWave * RotationSwayAmplitude.Pitch,
		YawWave * RotationSwayAmplitude.Yaw,
		RollWave * RotationSwayAmplitude.Roll
	);

	if (OwnerCharacter)
	{
		if (VisualTargetComponent)
		{
			if (bEnableProceduralBobbing)
			{
				VisualTargetComponent->SetRelativeLocation(BaseRelativeLocation + LocationOffset);
			}
			else
			{
				VisualTargetComponent->SetRelativeLocation(BaseRelativeLocation);
			}
		}

		UpdateCharacter3DOrientation(DeltaTime, RotationOffset);
		return;
	}

	if (OwnerRootPrimitive && OwnerRootPrimitive->IsSimulatingPhysics())
	{
		if (OwnerRootPrimitive->IsGravityEnabled())
		{
			OwnerRootPrimitive->SetEnableGravity(false);
		}
		OwnerRootPrimitive->WakeAllRigidBodies();

		FVector CurrentWaveVelocity = FVector::ZeroVector;
		if (bEnableProceduralBobbing)
		{
			CurrentWaveVelocity = FVector(
				FMath::Cos(RunningTime * BobbingSpeed * 0.7f) * (BobbingSpeed * 0.7f) * BobbingAmplitude.X,
				-FMath::Sin(RunningTime * BobbingSpeed * 0.85f) * (BobbingSpeed * 0.85f) * BobbingAmplitude.Y,
				FMath::Cos(RunningTime * BobbingSpeed) * BobbingSpeed * BobbingAmplitude.Z
			);
		}

		const FVector WaveDeltaVel = CurrentWaveVelocity - PreviousWaveVelocity;
		PreviousWaveVelocity = CurrentWaveVelocity;

		const FVector RealPhysicsVel = OwnerRootPrimitive->GetPhysicsLinearVelocity();
		OwnerRootPrimitive->SetPhysicsLinearVelocity(RealPhysicsVel + WaveDeltaVel, false);

		const FVector CurrentAngVel = OwnerRootPrimitive->GetPhysicsAngularVelocityInDegrees();
		if (InPlaceRotationSpeed > 0.0f && CurrentAngVel.SizeSquared() < FMath::Square(InPlaceRotationSpeed * 0.8f))
		{
			const FVector TargetGentleSpin = RandomRotationAxis * InPlaceRotationSpeed;
			OwnerRootPrimitive->SetPhysicsAngularVelocityInDegrees(
				FMath::VInterpTo(CurrentAngVel, TargetGentleSpin, DeltaTime, 2.0f),
				false
			);
		}

		return;
	}

	if (bEnableProceduralBobbing || InPlaceRotationSpeed > 0.0f)
	{
		if (InPlaceRotationSpeed > 0.0f)
		{
			BaseActorRotation += FRotator(
				RandomRotationAxis.X * InPlaceRotationSpeed * DeltaTime,
				RandomRotationAxis.Y * InPlaceRotationSpeed * DeltaTime,
				RandomRotationAxis.Z * InPlaceRotationSpeed * DeltaTime
			);
		}

		Owner->SetActorLocationAndRotation(
			BaseActorLocation + (bEnableProceduralBobbing ? LocationOffset : FVector::ZeroVector),
			BaseActorRotation + (bEnableProceduralBobbing ? RotationOffset : FRotator::ZeroRotator)
		);
	}
}

void USpaceFloatingComponent::AddSpaceImpulse(FVector ImpulseVelocity, bool bVelocityChange)
{
	if (OwnerCharacter && OwnerMovementComp)
	{
		OwnerMovementComp->AddImpulse(ImpulseVelocity, bVelocityChange);
	}
	else if (OwnerRootPrimitive && OwnerRootPrimitive->IsSimulatingPhysics())
	{
		OwnerRootPrimitive->WakeAllRigidBodies();
		OwnerRootPrimitive->AddImpulse(ImpulseVelocity, NAME_None, bVelocityChange);
	}
}
