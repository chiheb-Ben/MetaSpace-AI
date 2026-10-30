// Copyright Epic Games, Inc. All Rights Reserved.

#include "SpaceObjectComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/MeshComponent.h"
#include "GameFramework/Character.h"
#include "Engine/World.h"
#include "Math/UnrealMathUtility.h"

USpaceObjectComponent::USpaceObjectComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USpaceObjectComponent::BeginPlay()
{
	Super::BeginPlay();

	// Randomize starting phase so objects don't bob in identical sync
	RunningTime = FMath::FRandRange(0.0f, 100.0f);

	ApplyWeightPresetValues();

	// Set up 3D rotation axis
	if (bUseRandomRotationAxis)
	{
		RotationAxis = FMath::VRand();
		if (RotationAxis.IsNearlyZero())
		{
			RotationAxis = FVector(0.3f, 0.5f, 0.8f).GetSafeNormal();
		}
	}
	else
	{
		RotationAxis = CustomRotationAxis.GetSafeNormal();
		if (RotationAxis.IsNearlyZero())
		{
			RotationAxis = FVector::UpVector;
		}
	}

	EffectiveRotationSpeed = FMath::Max(0.0f, SelfRotationSpeed + FMath::FRandRange(-RotationSpeedRandomVariance, RotationSpeedRandomVariance));
	DriftDirection = FMath::VRand();

	DiscoverPrimitives();

	if (bAutoEnableZeroGPhysics)
	{
		SetZeroGEnabled(true);
	}
}

void USpaceObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bIsZeroGActive)
	{
		DeactivateObjectZeroG();
	}

	Super::EndPlay(EndPlayReason);
}

void USpaceObjectComponent::ApplyWeightPresetValues()
{
	switch (WeightPreset)
	{
	case ESpaceObjectWeightClass::FeatherLight:
		// Papers, pens, small tablets: ultra-light, lively rotation, high air wave, very easy to push
		bOverrideMassInKg = true;
		MassInKg = 0.25f;
		SelfRotationSpeed = 28.0f;
		WaveAmplitude = FVector(6.0f, 6.0f, 11.0f);
		WaveFrequency = 1.4f;
		ZeroGLinearDamping = 0.35f;
		ZeroGAngularDamping = 0.2f;
		PlayerCollisionPushMultiplier = 1.6f;
		ImpactTorqueMultiplier = 1.8f;
		SurfaceBounceCoefficient = 0.45f;
		break;

	case ESpaceObjectWeightClass::MediumProp:
		// Chairs, monitors, bags: balanced mass, steady rotation, realistic momentum
		bOverrideMassInKg = true;
		MassInKg = 8.0f;
		SelfRotationSpeed = 15.0f;
		WaveAmplitude = FVector(3.0f, 3.0f, 6.0f);
		WaveFrequency = 1.1f;
		ZeroGLinearDamping = 0.2f;
		ZeroGAngularDamping = 0.15f;
		PlayerCollisionPushMultiplier = 1.0f;
		ImpactTorqueMultiplier = 1.1f;
		SurfaceBounceCoefficient = 0.35f;
		break;

	case ESpaceObjectWeightClass::HeavyCargo:
		// Heavy storage crates, racks, tanks: heavy inertia, very slow majestic rotation, requires strong push
		bOverrideMassInKg = true;
		MassInKg = 65.0f;
		SelfRotationSpeed = 6.0f;
		WaveAmplitude = FVector(1.2f, 1.2f, 2.5f);
		WaveFrequency = 0.75f;
		ZeroGLinearDamping = 0.12f;
		ZeroGAngularDamping = 0.25f;
		PlayerCollisionPushMultiplier = 0.45f;
		ImpactTorqueMultiplier = 0.5f;
		SurfaceBounceCoefficient = 0.25f;
		break;

	case ESpaceObjectWeightClass::Custom:
	default:
		// Keep user's exact manual Inspector settings
		break;
	}
}

void USpaceObjectComponent::DiscoverPrimitives()
{
	ManagedPrimitives.Reset();
	PrimaryPrimitive = nullptr;

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (Owner->GetRootComponent())
	{
		Owner->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	}

	TArray<UPrimitiveComponent*> FoundPrims;
	Owner->GetComponents<UPrimitiveComponent>(FoundPrims);

	UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	UPrimitiveComponent* TaggedBoxOrPrim = nullptr;
	UPrimitiveComponent* AnyShapeBoxPrim = nullptr;
	UPrimitiveComponent* SimulatingPrim = nullptr;

	for (UPrimitiveComponent* Prim : FoundPrims)
	{
		if (!Prim || !Prim->IsRegistered())
		{
			continue;
		}

		Prim->SetMobility(EComponentMobility::Movable);
		ManagedPrimitives.Add(Prim);

		Prim->SetNotifyRigidBodyCollision(true);
		Prim->OnComponentHit.RemoveDynamic(this, &USpaceObjectComponent::OnObjectHit);
		Prim->OnComponentHit.AddDynamic(this, &USpaceObjectComponent::OnObjectHit);

		if (Prim->ComponentHasTag(FName("Grabbable")) && !TaggedBoxOrPrim)
		{
			TaggedBoxOrPrim = Prim;
		}
		if (Prim->IsA<UShapeComponent>() && !AnyShapeBoxPrim)
		{
			AnyShapeBoxPrim = Prim;
		}
		if (Prim->IsSimulatingPhysics() && !SimulatingPrim)
		{
			SimulatingPrim = Prim;
		}
	}

	// Priority:
	// 1. If the RootComponent is already a Box/Shape (or Primitive), or there is a tagged/shape BoxCollision,
	//    make the Box the Base/Root and attach everything else (StaticMesh, etc.) inside it!
	if (RootPrim && (RootPrim->IsA<UShapeComponent>() || RootPrim->ComponentHasTag(FName("Grabbable"))))
	{
		PrimaryPrimitive = RootPrim;
	}
	else if (TaggedBoxOrPrim)
	{
		PrimaryPrimitive = TaggedBoxOrPrim;
	}
	else if (AnyShapeBoxPrim)
	{
		PrimaryPrimitive = AnyShapeBoxPrim;
	}
	else if (RootPrim)
	{
		PrimaryPrimitive = RootPrim;
	}
	else if (SimulatingPrim)
	{
		PrimaryPrimitive = SimulatingPrim;
	}
	else if (ManagedPrimitives.Num() > 0)
	{
		PrimaryPrimitive = ManagedPrimitives[0];
	}

	UnifyActorComponents();
}

void USpaceObjectComponent::UnifyActorComponents()
{
	AActor* Owner = GetOwner();
	if (!Owner || !PrimaryPrimitive)
	{
		return;
	}

	PrimaryPrimitive->SetMobility(EComponentMobility::Movable);

	// Cache current world transforms of all scene components before reorganizing root
	TArray<USceneComponent*> AllSceneComps;
	Owner->GetComponents<USceneComponent>(AllSceneComps);

	TMap<USceneComponent*, FTransform> SavedWorldTransforms;
	for (USceneComponent* Comp : AllSceneComps)
	{
		if (Comp && Comp->IsRegistered())
		{
			Comp->SetMobility(EComponentMobility::Movable);
			SavedWorldTransforms.Add(Comp, Comp->GetComponentTransform());
		}
	}

	// Promote PrimaryPrimitive to RootComponent if the Actor had a DefaultSceneRoot or different root
	if (Owner->GetRootComponent() != PrimaryPrimitive)
	{
		if (PrimaryPrimitive->GetAttachParent() != nullptr)
		{
			PrimaryPrimitive->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		}
		Owner->SetRootComponent(PrimaryPrimitive);
	}

	const FTransform MasterWorldTransform = PrimaryPrimitive->GetComponentTransform();
	ChildRelativeTransforms.Reset();

	// Attach all other components (BoxCollisions, extra Meshes, Lights, etc.) to PrimaryPrimitive
	// and disable independent SimulatePhysics on child primitives so they NEVER split or fly off alone!
	for (USceneComponent* Comp : AllSceneComps)
	{
		if (!Comp || !Comp->IsRegistered() || Comp == PrimaryPrimitive)
		{
			continue;
		}

		if (UPrimitiveComponent* ChildPrim = Cast<UPrimitiveComponent>(Comp))
		{
			if (ChildPrim->IsSimulatingPhysics())
			{
				ChildPrim->SetSimulatePhysics(false);
			}
			ChildPrim->SetEnableGravity(false);
			PrimaryPrimitive->IgnoreComponentWhenMoving(ChildPrim, true);
			ChildPrim->IgnoreComponentWhenMoving(PrimaryPrimitive, true);
		}

		if (const FTransform* SavedWorld = SavedWorldTransforms.Find(Comp))
		{
			Comp->SetWorldTransform(*SavedWorld, false, nullptr, ETeleportType::TeleportPhysics);
		}

		if (!Comp->IsAttachedTo(PrimaryPrimitive))
		{
			Comp->AttachToComponent(PrimaryPrimitive, FAttachmentTransformRules::KeepWorldTransform);
		}

		const FTransform RelTransform = Comp->GetComponentTransform().GetRelativeTransform(MasterWorldTransform);
		ChildRelativeTransforms.Add(Comp, RelTransform);
	}
}

void USpaceObjectComponent::SyncAttachedChildComponents()
{
	if (!PrimaryPrimitive)
	{
		return;
	}

	const FTransform MasterWorld = PrimaryPrimitive->GetComponentTransform();
	for (auto& Pair : ChildRelativeTransforms)
	{
		USceneComponent* ChildComp = Pair.Key.Get();
		if (!ChildComp || ChildComp == PrimaryPrimitive)
		{
			continue;
		}

		if (UPrimitiveComponent* ChildPrim = Cast<UPrimitiveComponent>(ChildComp))
		{
			if (ChildPrim->IsSimulatingPhysics())
			{
				ChildPrim->SetSimulatePhysics(false);
			}
		}

		if (!ChildComp->IsAttachedTo(PrimaryPrimitive))
		{
			ChildComp->AttachToComponent(PrimaryPrimitive, FAttachmentTransformRules::KeepWorldTransform);
			ChildComp->SetWorldTransform(Pair.Value * MasterWorld, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
}

void USpaceObjectComponent::NotifyGrabStateChanged(bool bIsHeld)
{
	bIsHeldByPlayer = bIsHeld;
	PreviousWaveVelocity = FVector::ZeroVector;

	if (!bIsHeldByPlayer)
	{
		UnifyActorComponents();
		if (bIsZeroGActive && PrimaryPrimitive && PrimaryPrimitive->IsSimulatingPhysics())
		{
			if (bOverrideMassInKg && MassInKg > 0.0f)
			{
				PrimaryPrimitive->SetMassOverrideInKg(NAME_None, MassInKg, true);
			}
			PrimaryPrimitive->SetEnableGravity(false);
			PrimaryPrimitive->SetLinearDamping(ZeroGLinearDamping);
			PrimaryPrimitive->SetAngularDamping(ZeroGAngularDamping);
			PrimaryPrimitive->WakeAllRigidBodies();
		}
	}
}

void USpaceObjectComponent::SetZeroGEnabled(bool bEnable)
{
	if (bIsZeroGActive == bEnable)
	{
		return;
	}

	bIsZeroGActive = bEnable;

	if (bIsZeroGActive)
	{
		ActivateObjectZeroG();
	}
	else
	{
		DeactivateObjectZeroG();
	}
}

void USpaceObjectComponent::ActivateObjectZeroG()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	if (ManagedPrimitives.IsEmpty() || !PrimaryPrimitive)
	{
		DiscoverPrimitives();
	}

	if (!PrimaryPrimitive)
	{
		return;
	}

	// Lift slightly off tables/floors so resting contact friction doesn't hold the object down
	if (InitialLiftHeight > 0.0f)
	{
		Owner->AddActorWorldOffset(FVector(0.0f, 0.0f, InitialLiftHeight), false);
	}

	PreviousWaveVelocity = FVector::ZeroVector;

	// Ensure PrimaryPrimitive can simulate physics; if it's a StaticMesh without simple collision geometry,
	// fall back to a collision shape primitive (e.g. BoxComponent) on the Actor and unify everything under it!
	PrimaryPrimitive->SetMobility(EComponentMobility::Movable);
	PrimaryPrimitive->SetEnableGravity(false);
	if (!PrimaryPrimitive->IsSimulatingPhysics())
	{
		if (PrimaryPrimitive->GetCollisionEnabled() == ECollisionEnabled::NoCollision ||
			PrimaryPrimitive->GetCollisionEnabled() == ECollisionEnabled::QueryOnly)
		{
			PrimaryPrimitive->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			PrimaryPrimitive->SetCollisionResponseToAllChannels(ECR_Block);
		}
		PrimaryPrimitive->SetSimulatePhysics(true);
	}

	if (!PrimaryPrimitive->IsSimulatingPhysics())
	{
		for (UPrimitiveComponent* Candidate : ManagedPrimitives)
		{
			if (!Candidate || Candidate == PrimaryPrimitive)
			{
				continue;
			}
			if (Candidate->GetCollisionEnabled() == ECollisionEnabled::NoCollision ||
				Candidate->GetCollisionEnabled() == ECollisionEnabled::QueryOnly)
			{
				Candidate->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Candidate->SetCollisionResponseToAllChannels(ECR_Block);
			}
			Candidate->SetSimulatePhysics(true);
			if (Candidate->IsSimulatingPhysics())
			{
				PrimaryPrimitive = Candidate;
				break;
			}
		}
	}

	// Unify all other components (Mesh, Box, etc.) as non-simulating children of PrimaryPrimitive
	UnifyActorComponents();

	if (PrimaryPrimitive && PrimaryPrimitive->IsSimulatingPhysics())
	{
		SavedLinearDamping = PrimaryPrimitive->GetLinearDamping();
		SavedAngularDamping = PrimaryPrimitive->GetAngularDamping();

		// Apply custom mass in Kilograms
		if (bOverrideMassInKg && MassInKg > 0.0f)
		{
			PrimaryPrimitive->SetMassOverrideInKg(NAME_None, MassInKg, true);
		}

		PrimaryPrimitive->SetEnableGravity(false);
		PrimaryPrimitive->SetLinearDamping(ZeroGLinearDamping);
		PrimaryPrimitive->SetAngularDamping(ZeroGAngularDamping);
		PrimaryPrimitive->SetNotifyRigidBodyCollision(true);
		PrimaryPrimitive->WakeAllRigidBodies();

		// Apply initial lift & optional drift velocity
		FVector StartVel(0.0f, 0.0f, InitialUpwardVelocity);
		if (bApplyInitialDrift)
		{
			StartVel += DriftDirection * InitialDriftSpeed;
		}
		PrimaryPrimitive->SetPhysicsLinearVelocity(StartVel, false);

		// Apply autonomous 3D space rotation
		if (bEnableAutonomousRotation && EffectiveRotationSpeed > 0.0f)
		{
			PrimaryPrimitive->SetPhysicsAngularVelocityInDegrees(RotationAxis * EffectiveRotationSpeed, false);
		}
	}
}

void USpaceObjectComponent::DeactivateObjectZeroG()
{
	for (UPrimitiveComponent* Prim : ManagedPrimitives)
	{
		if (Prim)
		{
			Prim->SetEnableGravity(true);
			if (Prim->IsSimulatingPhysics())
			{
				Prim->SetLinearDamping(SavedLinearDamping);
				Prim->SetAngularDamping(SavedAngularDamping);
				Prim->WakeAllRigidBodies();
			}
		}
	}
}

void USpaceObjectComponent::SetObjectMassInKg(float NewMassInKg)
{
	MassInKg = FMath::Max(0.01f, NewMassInKg);
	bOverrideMassInKg = true;

	if (PrimaryPrimitive && PrimaryPrimitive->IsSimulatingPhysics())
	{
		PrimaryPrimitive->SetMassOverrideInKg(NAME_None, MassInKg, true);
	}
}

void USpaceObjectComponent::ApplySpacePush(FVector PushVector, bool bVelocityChange)
{
	if (PrimaryPrimitive && PrimaryPrimitive->IsSimulatingPhysics())
	{
		PrimaryPrimitive->WakeAllRigidBodies();
		PrimaryPrimitive->AddImpulse(PushVector, NAME_None, bVelocityChange);
	}
}

void USpaceObjectComponent::ApplySpacePushAtLocation(FVector PushForceOrImpulse, FVector WorldLocation)
{
	if (PrimaryPrimitive && PrimaryPrimitive->IsSimulatingPhysics())
	{
		PrimaryPrimitive->WakeAllRigidBodies();
		PrimaryPrimitive->AddImpulseAtLocation(PushForceOrImpulse, WorldLocation);
	}
}

void USpaceObjectComponent::OnObjectHit(
	UPrimitiveComponent* HitComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	if (!bIsZeroGActive || !PrimaryPrimitive || !PrimaryPrimitive->IsSimulatingPhysics())
	{
		return;
	}

	if (!OtherActor || OtherActor == GetOwner())
	{
		return;
	}

	UWorld* World = GetWorld();
	const float CurrentTime = World ? World->GetTimeSeconds() : 0.0f;
	if (CurrentTime - LastHitTime < 0.06f)
	{
		return;
	}
	LastHitTime = CurrentTime;

	// 1. If a Character (Trainee) collides with this object, transfer realistic mass-scaled momentum & 3D spin!
	if (ACharacter* Character = Cast<ACharacter>(OtherActor))
	{
		const FVector CharVel = Character->GetVelocity();
		const FVector OutwardDir = (PrimaryPrimitive->GetComponentLocation() - Character->GetActorLocation()).GetSafeNormal();

		// Scale push response inversely by object mass (assuming ~75kg astronaut vs object MassInKg)
		// Light objects (0.2kg paper/tool) fly off easily; Heavy crates (65kg) move slowly and heavily!
		const float EffectiveMass = FMath::Max(0.1f, PrimaryPrimitive->GetMass());
		const float MassRatioFactor = FMath::Clamp(35.0f / (EffectiveMass + 25.0f), 0.15f, 1.8f);

		const float CharSpeed = FMath::Max(CharVel.Size(), 95.0f);
		const FVector PushVelocityDelta =
			((CharVel * 0.65f) + (OutwardDir * CharSpeed * 0.55f)) * MassRatioFactor * PlayerCollisionPushMultiplier;

		PrimaryPrimitive->WakeAllRigidBodies();
		PrimaryPrimitive->AddImpulse(PushVelocityDelta, NAME_None, true);

		// Compute off-center 3D torque so hitting the edge of a chair or box makes it realistically spin!
		if (ImpactTorqueMultiplier > 0.0f)
		{
			const FVector LeverArm = Hit.ImpactPoint - PrimaryPrimitive->GetCenterOfMass();
			FVector TorqueAxis = FVector::CrossProduct(LeverArm, PushVelocityDelta).GetSafeNormal();
			if (TorqueAxis.IsNearlyZero())
			{
				TorqueAxis = FMath::VRand();
			}

			const float SpinBoostDeg = FMath::Clamp(PushVelocityDelta.Size() * 0.22f * ImpactTorqueMultiplier, 8.0f, 140.0f);
			const FVector CurrentAngVel = PrimaryPrimitive->GetPhysicsAngularVelocityInDegrees();
			PrimaryPrimitive->SetPhysicsAngularVelocityInDegrees(CurrentAngVel + (TorqueAxis * SpinBoostDeg), false);
		}
		return;
	}

	// 2. If hitting a static wall/floor/ceiling in Zero-G, apply a clean elastic space ricochet so objects don't just stick to walls
	if (SurfaceBounceCoefficient > 0.0f && (!OtherComp || !OtherComp->IsSimulatingPhysics()))
	{
		const FVector CurrentVel = PrimaryPrimitive->GetPhysicsLinearVelocity();
		const float SpeedIntoSurface = -FVector::DotProduct(CurrentVel, Hit.ImpactNormal);
		if (SpeedIntoSurface > 15.0f)
		{
			const FVector ReflectedVel = FMath::GetReflectionVector(CurrentVel, Hit.ImpactNormal) * SurfaceBounceCoefficient;
			PrimaryPrimitive->SetPhysicsLinearVelocity(ReflectedVel, false);
		}
	}
}

void USpaceObjectComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	SyncAttachedChildComponents();

	if (bIsHeldByPlayer || !bIsZeroGActive || !PrimaryPrimitive || !PrimaryPrimitive->IsSimulatingPhysics())
	{
		return;
	}

	RunningTime += DeltaTime;

	// Ensure gravity remains off and body stays awake in zero-G
	if (PrimaryPrimitive->IsGravityEnabled())
	{
		PrimaryPrimitive->SetEnableGravity(false);
	}
	PrimaryPrimitive->WakeAllRigidBodies();

	// 1. Apply Zero-Sum Micro-Gravity Wave (cabin air currents) without overwriting real collision velocities!
	FVector CurrentWaveVel = FVector::ZeroVector;
	if (bEnableMicroGravityWave)
	{
		CurrentWaveVel = FVector(
			FMath::Cos(RunningTime * WaveFrequency * 0.7f) * (WaveFrequency * 0.7f) * WaveAmplitude.X,
			-FMath::Sin(RunningTime * WaveFrequency * 0.85f) * (WaveFrequency * 0.85f) * WaveAmplitude.Y,
			FMath::Cos(RunningTime * WaveFrequency) * WaveFrequency * WaveAmplitude.Z
		);
	}

	const FVector WaveDelta = CurrentWaveVel - PreviousWaveVelocity;
	PreviousWaveVelocity = CurrentWaveVel;

	FVector NewLinearVel = PrimaryPrimitive->GetPhysicsLinearVelocity() + WaveDelta;
	if (NewLinearVel.SizeSquared() > FMath::Square(MaxAllowedSpeed))
	{
		NewLinearVel = NewLinearVel.GetClampedToMaxSize(MaxAllowedSpeed);
	}
	PrimaryPrimitive->SetPhysicsLinearVelocity(NewLinearVel, false);

	// 2. Maintain Autonomous 3D Space Rotation
	if (bEnableAutonomousRotation && EffectiveRotationSpeed > 0.0f)
	{
		const FVector CurrentAngVel = PrimaryPrimitive->GetPhysicsAngularVelocityInDegrees();
		const float CurrentSpinSpeed = CurrentAngVel.Size();

		if (CurrentSpinSpeed < EffectiveRotationSpeed * 0.85f)
		{
			// Gently bring rotation back up to its natural autonomous spin speed
			const FVector TargetSpin = RotationAxis * EffectiveRotationSpeed;
			PrimaryPrimitive->SetPhysicsAngularVelocityInDegrees(
				FMath::VInterpTo(CurrentAngVel, TargetSpin, DeltaTime, 2.0f),
				false
			);
		}
		else if (bMaintainBaseRotationSpeed && CurrentSpinSpeed > EffectiveRotationSpeed * 2.5f)
		{
			// After a strong collision makes it spin fast, gradually settle back toward its calm space spin
			const FVector SettledSpin = CurrentAngVel.GetSafeNormal() * EffectiveRotationSpeed;
			PrimaryPrimitive->SetPhysicsAngularVelocityInDegrees(
				FMath::VInterpTo(CurrentAngVel, SettledSpin, DeltaTime, 0.6f),
				false
			);
		}
	}
}
