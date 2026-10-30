// Copyright Epic Games, Inc. All Rights Reserved.

#include "SpaceGrabSnapComponent.h"
#include "SpaceObjectComponent.h"
#include "SpaceAIIdentityComponent.h"
#include "SpaceFloatingComponent.h"
#include "MetaAIComponent.h"
#include "SpaceTabletQuizComponent.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "InputCoreTypes.h"

USpaceGrabSnapComponent::USpaceGrabSnapComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USpaceGrabSnapComponent::BeginPlay()
{
	Super::BeginPlay();

	OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (OwnerCharacter)
	{
		OwnerCamera = OwnerCharacter->FindComponentByClass<UCameraComponent>();
	}
}

void USpaceGrabSnapComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HeldActor.IsValid())
	{
		DropHeldObject();
	}

	if (bWaitingToRestoreCollision)
	{
		RestoreRecentlyDroppedCollision();
	}

	Super::EndPlay(EndPlayReason);
}

void USpaceGrabSnapComponent::RestoreRecentlyDroppedCollision()
{
	for (auto& Pair : RecentlyDroppedPawnResponses)
	{
		UPrimitiveComponent* Prim = Pair.Key.Get();
		if (Prim && (!HeldActor.IsValid() || Prim->GetOwner() != HeldActor.Get()))
		{
			Prim->SetCollisionResponseToChannel(ECC_Pawn, Pair.Value);
		}
	}
	RecentlyDroppedPawnResponses.Reset();
	RecentlyDroppedActor = nullptr;
	bWaitingToRestoreCollision = false;
}

void USpaceGrabSnapComponent::GetCameraView(FVector& OutLocation, FRotator& OutRotation) const
{
	if (OwnerCamera)
	{
		OutLocation = OwnerCamera->GetComponentLocation();
		OutRotation = OwnerCamera->GetComponentRotation();
		return;
	}

	if (OwnerCharacter)
	{
		if (AController* Controller = OwnerCharacter->GetController())
		{
			Controller->GetPlayerViewPoint(OutLocation, OutRotation);
			return;
		}
		OutLocation = OwnerCharacter->GetPawnViewLocation();
		OutRotation = OwnerCharacter->GetViewRotation();
		return;
	}

	if (const AActor* Owner = GetOwner())
	{
		OutLocation = Owner->GetActorLocation();
		OutRotation = Owner->GetActorRotation();
	}
}

bool USpaceGrabSnapComponent::HasRequiredTag(const AActor* Actor, const UPrimitiveComponent* Comp, const FName& RequiredTag) const
{
	if (RequiredTag.IsNone())
	{
		return false;
	}

	if (Actor && Actor->ActorHasTag(RequiredTag))
	{
		return true;
	}

	if (Comp && Comp->ComponentHasTag(RequiredTag))
	{
		return true;
	}

	// Also check if ANY component inside the Actor (e.g., a child BoxCollision or StaticMesh) has the tag!
	if (Actor)
	{
		for (const UActorComponent* SubComp : Actor->GetComponents())
		{
			if (SubComp && SubComp->ComponentHasTag(RequiredTag))
			{
				return true;
			}
		}
	}

	return false;
}

UPrimitiveComponent* USpaceGrabSnapComponent::ResolveAndUnifyActorMasterPrimitive(AActor* Actor) const
{
	if (!Actor)
	{
		return nullptr;
	}

	// 1. If the Actor has a USpaceObjectComponent, let it unify all components and return its PrimaryPrimitive
	if (USpaceObjectComponent* SpaceObj = Actor->FindComponentByClass<USpaceObjectComponent>())
	{
		SpaceObj->UnifyActorComponents();
		if (UPrimitiveComponent* SpacePrimary = SpaceObj->GetPrimaryPrimitive())
		{
			return SpacePrimary;
		}
	}

	// 2. Otherwise, make the Box / RootComponent the Master Base and attach everything else (StaticMesh, etc.) inside it!
	TArray<UPrimitiveComponent*> FoundPrims;
	Actor->GetComponents<UPrimitiveComponent>(FoundPrims);

	UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
	UPrimitiveComponent* TaggedBoxOrPrim = nullptr;
	UPrimitiveComponent* AnyShapeBoxPrim = nullptr;
	UPrimitiveComponent* SimulatingPrim = nullptr;
	UPrimitiveComponent* FirstPrim = nullptr;

	for (UPrimitiveComponent* Prim : FoundPrims)
	{
		if (!Prim || !Prim->IsRegistered())
		{
			continue;
		}

		Prim->SetMobility(EComponentMobility::Movable);
		if (!FirstPrim)
		{
			FirstPrim = Prim;
		}

		if (Prim->ComponentHasTag(GrabbableTag) && !TaggedBoxOrPrim)
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

	UPrimitiveComponent* MasterPrim = nullptr;
	if (RootPrim && (RootPrim->IsA<UShapeComponent>() || RootPrim->ComponentHasTag(GrabbableTag)))
	{
		MasterPrim = RootPrim;
	}
	else if (TaggedBoxOrPrim)
	{
		MasterPrim = TaggedBoxOrPrim;
	}
	else if (AnyShapeBoxPrim)
	{
		MasterPrim = AnyShapeBoxPrim;
	}
	else if (RootPrim)
	{
		MasterPrim = RootPrim;
	}
	else if (SimulatingPrim)
	{
		MasterPrim = SimulatingPrim;
	}
	else
	{
		MasterPrim = FirstPrim;
	}

	if (!MasterPrim)
	{
		return nullptr;
	}

	// 3. Unify all other scene/primitive components (BoxCollision, extra Meshes, etc.) under MasterPrim
	TArray<USceneComponent*> AllSceneComps;
	Actor->GetComponents<USceneComponent>(AllSceneComps);

	TMap<USceneComponent*, FTransform> SavedWorldTransforms;
	for (USceneComponent* Comp : AllSceneComps)
	{
		if (Comp && Comp->IsRegistered())
		{
			Comp->SetMobility(EComponentMobility::Movable);
			SavedWorldTransforms.Add(Comp, Comp->GetComponentTransform());
		}
	}

	if (Actor->GetRootComponent() != MasterPrim)
	{
		if (MasterPrim->GetAttachParent() != nullptr)
		{
			MasterPrim->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		}
		Actor->SetRootComponent(MasterPrim);
	}

	for (USceneComponent* Comp : AllSceneComps)
	{
		if (!Comp || !Comp->IsRegistered() || Comp == MasterPrim)
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
			MasterPrim->IgnoreComponentWhenMoving(ChildPrim, true);
			ChildPrim->IgnoreComponentWhenMoving(MasterPrim, true);
		}

		if (const FTransform* SavedWorld = SavedWorldTransforms.Find(Comp))
		{
			Comp->SetWorldTransform(*SavedWorld, false, nullptr, ETeleportType::TeleportPhysics);
		}

		if (!Comp->IsAttachedTo(MasterPrim))
		{
			Comp->AttachToComponent(MasterPrim, FAttachmentTransformRules::KeepWorldTransform);
		}
	}

	return MasterPrim;
}

void USpaceGrabSnapComponent::SyncActorChildComponents(
	AActor* Actor,
	UPrimitiveComponent* MasterPrim,
	const TMap<TWeakObjectPtr<USceneComponent>, FTransform>& RelativeTransforms) const
{
	if (!Actor || !MasterPrim)
	{
		return;
	}

	const FTransform MasterWorld = MasterPrim->GetComponentTransform();
	for (const auto& Pair : RelativeTransforms)
	{
		USceneComponent* ChildComp = Pair.Key.Get();
		if (!ChildComp || ChildComp == MasterPrim)
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

		if (!ChildComp->IsAttachedTo(MasterPrim))
		{
			ChildComp->AttachToComponent(MasterPrim, FAttachmentTransformRules::KeepWorldTransform);
		}

		ChildComp->SetWorldTransform(Pair.Value * MasterWorld, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

bool USpaceGrabSnapComponent::TraceForGrabbable(FHitResult& OutHit, AActor*& OutActor, UPrimitiveComponent*& OutPrimitive) const
{
	OutActor = nullptr;
	OutPrimitive = nullptr;

	UWorld* World = GetWorld();
	if (!World || !GetOwner())
	{
		return false;
	}

	FVector ViewLoc;
	FRotator ViewRot;
	GetCameraView(ViewLoc, ViewRot);

	const FVector ViewDir = ViewRot.Vector().GetSafeNormal();
	const FVector TraceEnd = ViewLoc + (ViewDir * GrabReachDistance);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SpaceGrabTrace), false, GetOwner());
	QueryParams.bIgnoreTouches = false; // Also detect Overlap-only BoxCollision components tagged "Grabbable"!

	// 1. Multi-Sweep against ALL object types along the camera ray so whether the user tagged a StaticMesh
	// or an Overlap/Trigger BoxCollision component, we always detect the Actor and unify its root!
	TArray<FHitResult> Hits;
	FCollisionObjectQueryParams AllObjectsParams(FCollisionObjectQueryParams::InitType::AllObjects);

	if (GrabSweepRadius > 0.0f)
	{
		World->SweepMultiByObjectType(
			Hits,
			ViewLoc,
			TraceEnd,
			FQuat::Identity,
			AllObjectsParams,
			FCollisionShape::MakeSphere(GrabSweepRadius),
			QueryParams
		);
	}
	else
	{
		World->LineTraceMultiByObjectType(
			Hits,
			ViewLoc,
			TraceEnd,
			AllObjectsParams,
			QueryParams
		);
	}

	for (const FHitResult& Hit : Hits)
	{
		AActor* HitActor = Hit.GetActor();
		UPrimitiveComponent* HitComp = Hit.GetComponent();
		if (HitActor && HitActor != GetOwner() && HasRequiredTag(HitActor, HitComp, GrabbableTag))
		{
			UPrimitiveComponent* MasterPrim = ResolveAndUnifyActorMasterPrimitive(HitActor);
			if (MasterPrim)
			{
				OutHit = Hit;
				OutActor = HitActor;
				OutPrimitive = MasterPrim;
				return true;
			}
		}
	}

	// 2. Fallback Cone / Proximity Search: If a fast-moving or thin object is in front of the camera within reach,
	// find the closest Actor that has GrabbableTag (on the Actor or on any of its components, like BoxCollision)!
	float BestScore = MAX_FLT;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Candidate = *It;
		if (!Candidate || Candidate == GetOwner())
		{
			continue;
		}

		if (!HasRequiredTag(Candidate, nullptr, GrabbableTag))
		{
			continue;
		}

		UPrimitiveComponent* MasterPrim = ResolveAndUnifyActorMasterPrimitive(Candidate);
		if (!MasterPrim)
		{
			continue;
		}

		const FVector CenterLoc = MasterPrim->GetComponentLocation();
		const FVector ToCandidate = CenterLoc - ViewLoc;
		const float Dist = ToCandidate.Size();
		if (Dist > GrabReachDistance || Dist < 1.0f)
		{
			continue;
		}

		const float ForwardDot = FVector::DotProduct(ToCandidate / Dist, ViewDir);
		if (ForwardDot > 0.84f)
		{
			const float Score = Dist * (2.0f - ForwardDot);
			if (Score < BestScore)
			{
				BestScore = Score;
				OutActor = Candidate;
				OutPrimitive = MasterPrim;
				OutHit.ImpactPoint = CenterLoc;
				OutHit.Location = CenterLoc;
			}
		}
	}

	return OutActor != nullptr && OutPrimitive != nullptr;
}

bool USpaceGrabSnapComponent::IsGrabbableInReach(AActor*& OutGrabbableActor) const
{
	FHitResult Hit;
	UPrimitiveComponent* Prim = nullptr;
	return TraceForGrabbable(Hit, OutGrabbableActor, Prim);
}

bool USpaceGrabSnapComponent::TryGrabObject()
{
	if (HeldActor.IsValid())
	{
		return false;
	}

	FHitResult Hit;
	AActor* TargetActor = nullptr;
	UPrimitiveComponent* TargetPrim = nullptr;

	if (!TraceForGrabbable(Hit, TargetActor, TargetPrim) || !TargetActor || !TargetPrim)
	{
		return false;
	}

	// Ensure all components of TargetActor (StaticMesh, BoxCollision, etc.) are unified under TargetPrim!
	TargetPrim = ResolveAndUnifyActorMasterPrimitive(TargetActor);
	if (!TargetPrim)
	{
		return false;
	}

	// If we re-grabbed the same Actor while its post-drop collision grace timer was active, reuse its saved responses
	HeldActorSavedPawnResponses.Reset();
	if (bWaitingToRestoreCollision && RecentlyDroppedActor.Get() == TargetActor)
	{
		HeldActorSavedPawnResponses = RecentlyDroppedPawnResponses;
		RecentlyDroppedPawnResponses.Reset();
		RecentlyDroppedActor = nullptr;
		bWaitingToRestoreCollision = false;
	}
	else if (bWaitingToRestoreCollision)
	{
		RestoreRecentlyDroppedCollision();
	}

	// Cancel any ongoing snap animation on this actor if the player re-grabs it
	if (SnappingActor.Get() == TargetActor)
	{
		bIsAnimatingSnap = false;
		SnappingActor = nullptr;
		SnappingPrimitive = nullptr;
	}

	HeldActor = TargetActor;
	HeldPrimitive = TargetPrim;

	// Check if this object has a SpaceObjectComponent or was simulating physics on any primitive
	USpaceObjectComponent* SpaceObj = TargetActor->FindComponentByClass<USpaceObjectComponent>();
	bHeldObjectWasSimulatingPhysics = TargetPrim->IsSimulatingPhysics() || (SpaceObj != nullptr);

	// 1. Temporarily pause SpaceObjectComponent while carried in the player's hand
	if (SpaceObj)
	{
		SpaceObj->NotifyGrabStateChanged(true);
		SpaceObj->SetComponentTickEnabled(false);
	}

	// 2. Disable SimulatePhysics and ignore ECC_Pawn across ALL primitive components of TargetActor!
	// And cache the exact relative transform of every scene component relative to TargetPrim so the entire Actor
	// (Mesh + Box + Child Components) moves 100% together as one unified object!
	if (OwnerCharacter)
	{
		OwnerCharacter->MoveIgnoreActorAdd(TargetActor);
		if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
		{
			Capsule->IgnoreActorWhenMoving(TargetActor, true);
		}
		if (USpaceFloatingComponent* FloatComp = OwnerCharacter->FindComponentByClass<USpaceFloatingComponent>())
		{
			FloatComp->CancelGrabAndStopInPlace();
		}
	}

	TArray<UPrimitiveComponent*> AllActorPrims;
	TargetActor->GetComponents<UPrimitiveComponent>(AllActorPrims);
	for (UPrimitiveComponent* Prim : AllActorPrims)
	{
		if (!Prim || !Prim->IsRegistered())
		{
			continue;
		}

		Prim->SetMobility(EComponentMobility::Movable);
		if (Prim->IsSimulatingPhysics())
		{
			bHeldObjectWasSimulatingPhysics = true;
			Prim->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
			Prim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);
			Prim->SetSimulatePhysics(false);
		}

		if (!HeldActorSavedPawnResponses.Contains(Prim))
		{
			HeldActorSavedPawnResponses.Add(Prim, Prim->GetCollisionResponseToChannel(ECC_Pawn));
		}
		Prim->IgnoreActorWhenMoving(OwnerCharacter, true);
		Prim->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Prim->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		Prim->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	}

	HeldChildRelativeTransforms.Reset();
	const FTransform MasterWorldTransform = TargetPrim->GetComponentTransform();
	TArray<USceneComponent*> AllSceneComps;
	TargetActor->GetComponents<USceneComponent>(AllSceneComps);
	for (USceneComponent* Comp : AllSceneComps)
	{
		if (!Comp || !Comp->IsRegistered() || Comp == TargetPrim)
		{
			continue;
		}

		Comp->SetMobility(EComponentMobility::Movable);
		if (!Comp->IsAttachedTo(TargetPrim))
		{
			Comp->AttachToComponent(TargetPrim, FAttachmentTransformRules::KeepWorldTransform);
		}
		HeldChildRelativeTransforms.Add(Comp, Comp->GetComponentTransform().GetRelativeTransform(MasterWorldTransform));
	}

	FVector ViewLoc;
	FRotator ViewRot;
	GetCameraView(ViewLoc, ViewRot);
	HeldRelativeRotationOffset = (TargetPrim->GetComponentQuat() * ViewRot.Quaternion().Inverse()).Rotator();

	OnItemGrabStateChanged.Broadcast(TargetActor, nullptr, false);
	return true;
}

bool USpaceGrabSnapComponent::FindBestSnapTarget(FTransform& OutSnapTransform, AActor*& OutSnapActor, USceneComponent*& OutSnapComp) const
{
	OutSnapActor = nullptr;
	OutSnapComp = nullptr;

	if (!HeldActor.IsValid())
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector HeldLocation = HeldPrimitive.IsValid() ? HeldPrimitive->GetComponentLocation() : HeldActor->GetActorLocation();
	float BestDistSq = FMath::Square(SnapDetectionRadius);

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* CandidateActor = *It;
		if (!CandidateActor || CandidateActor == HeldActor.Get() || CandidateActor == GetOwner())
		{
			continue;
		}

		TArray<USceneComponent*> SceneComps;
		CandidateActor->GetComponents<USceneComponent>(SceneComps);
		for (USceneComponent* Comp : SceneComps)
		{
			if (Comp && Comp->ComponentHasTag(SnapTargetTag))
			{
				const float DistSq = FVector::DistSquared(HeldLocation, Comp->GetComponentLocation());
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					OutSnapActor = CandidateActor;
					OutSnapComp = Comp;
					OutSnapTransform = Comp->GetComponentTransform();
				}
			}
		}

		if (CandidateActor->ActorHasTag(SnapTargetTag) && CandidateActor->GetRootComponent())
		{
			const float DistSq = FVector::DistSquared(HeldLocation, CandidateActor->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				OutSnapActor = CandidateActor;
				OutSnapComp = CandidateActor->GetRootComponent();
				OutSnapTransform = CandidateActor->GetActorTransform();
			}
		}
	}

	return OutSnapActor != nullptr;
}

void USpaceGrabSnapComponent::SnapHeldObjectToTarget(const FTransform& SnapTransform, AActor* SnapTargetActor)
{
	AActor* Item = HeldActor.Get();
	UPrimitiveComponent* Prim = HeldPrimitive.Get();
	if (!Item || !Prim)
	{
		return;
	}

	SyncActorChildComponents(Item, Prim, HeldChildRelativeTransforms);

	if (OwnerCharacter)
	{
		OwnerCharacter->MoveIgnoreActorRemove(Item);
		if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
		{
			Capsule->IgnoreActorWhenMoving(Item, false);
		}
	}

	for (auto& Pair : HeldActorSavedPawnResponses)
	{
		if (UPrimitiveComponent* Comp = Pair.Key.Get())
		{
			Comp->SetCollisionResponseToChannel(ECC_Pawn, Pair.Value);
			Comp->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
			Comp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		}
	}
	HeldActorSavedPawnResponses.Reset();
	HeldChildRelativeTransforms.Reset();

	if (bLockPhysicsWhenSnapped)
	{
		Prim->SetSimulatePhysics(false);
	}

	SnappingActor = Item;
	SnappingPrimitive = Prim;
	TargetSnapTransform = SnapTransform;
	bIsAnimatingSnap = true;

	HeldActor = nullptr;
	HeldPrimitive = nullptr;
	CandidateSnapActor = nullptr;
	CandidateSnapComponent = nullptr;

	OnItemGrabStateChanged.Broadcast(Item, SnapTargetActor, true);
}

void USpaceGrabSnapComponent::DropHeldObject()
{
	if (!HeldActor.IsValid() || !HeldPrimitive.IsValid())
	{
		HeldActor = nullptr;
		HeldPrimitive = nullptr;
		HeldChildRelativeTransforms.Reset();
		HeldActorSavedPawnResponses.Reset();
		return;
	}

	// 1. Check if we are dropping the object within range of a valid SnapPoint!
	FTransform SnapTransform;
	AActor* SnapActor = nullptr;
	USceneComponent* SnapComp = nullptr;
	if (FindBestSnapTarget(SnapTransform, SnapActor, SnapComp))
	{
		SnapHeldObjectToTarget(SnapTransform, SnapActor);
		return;
	}

	// 2. Otherwise, perform a calm Zero-G release right in place (with all Actor components unified!)
	AActor* Item = HeldActor.Get();
	UPrimitiveComponent* Prim = HeldPrimitive.Get();

	SyncActorChildComponents(Item, Prim, HeldChildRelativeTransforms);

	if (bHeldObjectWasSimulatingPhysics)
	{
		Prim->SetSimulatePhysics(true);
		// If Prim is a StaticMesh without simple collision geometry, fallback to a child shape primitive that CAN simulate
		if (!Prim->IsSimulatingPhysics())
		{
			TArray<UPrimitiveComponent*> AllPrims;
			Item->GetComponents<UPrimitiveComponent>(AllPrims);
			for (UPrimitiveComponent* Candidate : AllPrims)
			{
				if (Candidate && Candidate != Prim)
				{
					Candidate->SetSimulatePhysics(true);
					if (Candidate->IsSimulatingPhysics())
					{
						Prim = Candidate;
						if (Item->GetRootComponent() != Prim)
						{
							Prim->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
							Item->SetRootComponent(Prim);
						}
						for (UPrimitiveComponent* OtherPrim : AllPrims)
						{
							if (OtherPrim && OtherPrim != Prim)
							{
								OtherPrim->SetSimulatePhysics(false);
								OtherPrim->AttachToComponent(Prim, FAttachmentTransformRules::KeepWorldTransform);
							}
						}
						break;
					}
				}
			}
		}

		Prim->SetEnableGravity(false);
		Prim->WakeAllRigidBodies();

		// Set linear velocity to ZERO (or tiny fraction of player drift) so it stays floating calmly right in front of you!
		const FVector CalmVel = OwnerCharacter ? (OwnerCharacter->GetVelocity() * 0.15f).GetClampedToMaxSize(40.0f) : FVector::ZeroVector;
		Prim->SetPhysicsLinearVelocity(CalmVel, false);
		Prim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);
	}

	if (OwnerCharacter)
	{
		OwnerCharacter->MoveIgnoreActorRemove(Item);
		if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
		{
			Capsule->IgnoreActorWhenMoving(Item, false);
		}
	}

	// Keep ECC_Pawn ignored on ALL primitives of the Actor for a brief grace window (0.45s) after dropping
	if (UWorld* World = GetWorld())
	{
		RecentlyDroppedActor = Item;
		RecentlyDroppedPawnResponses = HeldActorSavedPawnResponses;
		RestoreCollisionTimestamp = World->GetTimeSeconds() + PostDropPawnIgnoreDuration;
		bWaitingToRestoreCollision = true;
	}
	else
	{
		for (auto& Pair : HeldActorSavedPawnResponses)
		{
			if (UPrimitiveComponent* Comp = Pair.Key.Get())
			{
				Comp->SetCollisionResponseToChannel(ECC_Pawn, Pair.Value);
			}
		}
	}

	if (USpaceObjectComponent* SpaceObj = Item->FindComponentByClass<USpaceObjectComponent>())
	{
		SpaceObj->NotifyGrabStateChanged(false);
		SpaceObj->SetComponentTickEnabled(true);
	}

	HeldActor = nullptr;
	HeldPrimitive = nullptr;
	HeldChildRelativeTransforms.Reset();
	HeldActorSavedPawnResponses.Reset();
	CandidateSnapActor = nullptr;
	CandidateSnapComponent = nullptr;

	OnItemGrabStateChanged.Broadcast(Item, nullptr, false);
}

void USpaceGrabSnapComponent::ThrowHeldObject()
{
	if (!HeldActor.IsValid() || !HeldPrimitive.IsValid())
	{
		return;
	}

	FTransform SnapTransform;
	AActor* SnapActor = nullptr;
	USceneComponent* SnapComp = nullptr;
	if (FindBestSnapTarget(SnapTransform, SnapActor, SnapComp))
	{
		SnapHeldObjectToTarget(SnapTransform, SnapActor);
		return;
	}

	AActor* Item = HeldActor.Get();
	UPrimitiveComponent* Prim = HeldPrimitive.Get();

	SyncActorChildComponents(Item, Prim, HeldChildRelativeTransforms);

	FVector ViewLoc;
	FRotator ViewRot;
	GetCameraView(ViewLoc, ViewRot);
	const FVector LaunchDir = ViewRot.Vector().GetSafeNormal();

	Prim->SetSimulatePhysics(true);
	if (!Prim->IsSimulatingPhysics())
	{
		TArray<UPrimitiveComponent*> AllPrims;
		Item->GetComponents<UPrimitiveComponent>(AllPrims);
		for (UPrimitiveComponent* Candidate : AllPrims)
		{
			if (Candidate && Candidate != Prim)
			{
				Candidate->SetSimulatePhysics(true);
				if (Candidate->IsSimulatingPhysics())
				{
					Prim = Candidate;
					if (Item->GetRootComponent() != Prim)
					{
						Prim->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
						Item->SetRootComponent(Prim);
					}
					for (UPrimitiveComponent* OtherPrim : AllPrims)
					{
						if (OtherPrim && OtherPrim != Prim)
						{
							OtherPrim->SetSimulatePhysics(false);
							OtherPrim->AttachToComponent(Prim, FAttachmentTransformRules::KeepWorldTransform);
						}
					}
					break;
				}
			}
		}
	}

	Prim->SetEnableGravity(false);
	Prim->WakeAllRigidBodies();

	// Launch the unified Actor smoothly forward at ThrowLaunchSpeed
	Prim->SetPhysicsLinearVelocity(LaunchDir * ThrowLaunchSpeed, false);
	Prim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);

	if (OwnerCharacter)
	{
		OwnerCharacter->MoveIgnoreActorRemove(Item);
		if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
		{
			Capsule->IgnoreActorWhenMoving(Item, false);
		}
	}

	if (UWorld* World = GetWorld())
	{
		RecentlyDroppedActor = Item;
		RecentlyDroppedPawnResponses = HeldActorSavedPawnResponses;
		RestoreCollisionTimestamp = World->GetTimeSeconds() + PostDropPawnIgnoreDuration;
		bWaitingToRestoreCollision = true;
	}
	else
	{
		for (auto& Pair : HeldActorSavedPawnResponses)
		{
			if (UPrimitiveComponent* Comp = Pair.Key.Get())
			{
				Comp->SetCollisionResponseToChannel(ECC_Pawn, Pair.Value);
			}
		}
	}

	if (USpaceObjectComponent* SpaceObj = Item->FindComponentByClass<USpaceObjectComponent>())
	{
		SpaceObj->NotifyGrabStateChanged(false);
		SpaceObj->SetComponentTickEnabled(true);
	}

	HeldActor = nullptr;
	HeldPrimitive = nullptr;
	HeldChildRelativeTransforms.Reset();
	HeldActorSavedPawnResponses.Reset();
	CandidateSnapActor = nullptr;
	CandidateSnapComponent = nullptr;

	OnItemGrabStateChanged.Broadcast(Item, nullptr, false);
}

void USpaceGrabSnapComponent::ToggleGrabOrDrop()
{
	if (HeldActor.IsValid())
	{
		DropHeldObject();
	}
	else
	{
		TryGrabObject();
	}
}

bool USpaceGrabSnapComponent::TryInteractWithObjectInFront()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FVector CamLoc;
	FRotator CamRot;
	GetCameraView(CamLoc, CamRot);

	const FVector TraceEnd = CamLoc + (CamRot.Vector() * GrabReachDistance);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SpaceInteractTrace), false, GetOwner());
	if (HeldActor.IsValid())
	{
		QueryParams.AddIgnoredActor(HeldActor.Get());
	}

	TArray<FHitResult> Hits;
	FCollisionObjectQueryParams ObjParams = FCollisionObjectQueryParams::AllObjects;
	World->SweepMultiByObjectType(
		Hits,
		CamLoc,
		TraceEnd,
		FQuat::Identity,
		ObjParams,
		FCollisionShape::MakeSphere(FMath::Max(16.0f, GrabSweepRadius)),
		QueryParams
	);

	for (const FHitResult& Hit : Hits)
	{
		AActor* HitActor = Hit.GetActor();
		UPrimitiveComponent* HitComp = Hit.GetComponent();
		if (!HitActor || HitActor == GetOwner())
		{
			continue;
		}

		// Ignore hidden components (e.g., if ValveWheel is hidden until the player attaches the wheel!)
		if (HitComp && HitComp->bHiddenInGame)
		{
			continue;
		}

		USpaceAIIdentityComponent* IdComp = HitActor->FindComponentByClass<USpaceAIIdentityComponent>();
		USpaceTabletQuizComponent* QuizComp = HitActor->FindComponentByClass<USpaceTabletQuizComponent>();
		const FString HitActorName = HitActor->GetActorNameOrLabel().ToLower();
		const bool bIsTablet = (QuizComp != nullptr) ||
		                       HitActorName.Contains(TEXT("tablet")) ||
		                       HitActorName.Contains(TEXT("scanner")) ||
		                       HitActorName.Contains(TEXT("quiz")) ||
		                       HitActorName.Contains(TEXT("quest_main"));

		const bool bHasIdentityInteract = (IdComp && IdComp->bCanInteractWithEKey);
		const bool bHasInteractTag = (!InteractableTag.IsNone() && (HitActor->ActorHasTag(InteractableTag) || (HitComp && HitComp->ComponentHasTag(InteractableTag))));

		bool bIsQuestTargetOrMarker = false;
		if (UMetaAIComponent* MetaAI = UMetaAIComponent::GetMetaAIComponentGlobal(this))
		{
			bIsQuestTargetOrMarker = (HitActor == MetaAI->GetActiveQuestMarkerActor() ||
			                          HitActor == MetaAI->GetActiveQuestTargetActor() ||
			                          HitActor->ActorHasTag(TEXT("QuestTarget")) ||
			                          HitActor->ActorHasTag(TEXT("Interactable")));
		}

		if (bHasIdentityInteract || bHasInteractTag || bIsTablet || bIsQuestTargetOrMarker)
		{
			const FName CompName = HitComp ? HitComp->GetFName() : NAME_None;

			if (UMetaAIComponent* MetaAI = UMetaAIComponent::GetMetaAIComponentGlobal(this))
			{
				MetaAI->HandleTrackedPlayerInteractWithE(HitActor, HitComp, CompName);
			}

			if (bIsTablet)
			{
				if (!QuizComp)
				{
					QuizComp = USpaceTabletQuizComponent::GetSpaceTabletQuizComponent(HitActor);
				}

				if (QuizComp)
				{
					QuizComp->EnsureDefaultWidgetClass();
					QuizComp->ToggleTabletUI();
				}
			}

			if (IdComp)
			{
				IdComp->TriggerPlayerInteractionWithE(OwnerCharacter, HitComp);
			}
			else
			{
				UMetaAIComponent::ReportShipScenarioEventToAI(
					HitActor,
					TEXT("Player_E_Key_Interact"),
					HitActor->GetActorNameOrLabel(),
					FString::Printf(TEXT("Player pressed [E] to interact with '%s' (Component: %s)"), *HitActor->GetActorNameOrLabel(), *CompName.ToString()),
					false
				);
			}

			OnPlayerInteractedWithActor.Broadcast(HitActor, HitComp, CompName);
			return true;
		}
	}

	return false;
}

void USpaceGrabSnapComponent::UpdateHeldObjectTransform(float DeltaTime)
{
	if (!HeldActor.IsValid() || !HeldPrimitive.IsValid())
	{
		return;
	}

	FVector ViewLoc;
	FRotator ViewRot;
	GetCameraView(ViewLoc, ViewRot);

	// Position held item in the astronaut's right hand area (slightly lower and to the right, comfortably in view without blocking center crosshair)
	const FVector ForwardOffset = ViewRot.Vector() * HoldDistanceFromCamera;
	const FVector RightOffset   = FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Y) * 24.0f;
	const FVector DownOffset    = FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Z) * -16.0f;
	const FVector TargetHoldLocation = ViewLoc + ForwardOffset + RightOffset + DownOffset;
	const FQuat TargetHoldRotation = ViewRot.Quaternion() * HeldRelativeRotationOffset.Quaternion();

	UPrimitiveComponent* Prim = HeldPrimitive.Get();
	const FVector CurrentLoc = Prim->GetComponentLocation();
	const FQuat CurrentQuat = Prim->GetComponentQuat();

	const FVector SmoothLoc = FMath::VInterpTo(CurrentLoc, TargetHoldLocation, DeltaTime, HoldInterpSpeed);
	const FQuat SmoothQuat = FMath::QInterpTo(CurrentQuat, TargetHoldRotation, DeltaTime, HoldInterpSpeed);

	// Move the unified Root Primitive AND sync all child components (Box, Meshes, etc.) so the whole Actor moves as one piece!
	Prim->SetWorldLocationAndRotation(SmoothLoc, SmoothQuat, false, nullptr, ETeleportType::TeleportPhysics);
	if (HeldActor->GetRootComponent() && HeldActor->GetRootComponent() != Prim)
	{
		HeldActor->SetActorLocationAndRotation(SmoothLoc, SmoothQuat, false, nullptr, ETeleportType::TeleportPhysics);
	}
	SyncActorChildComponents(HeldActor.Get(), Prim, HeldChildRelativeTransforms);

	// Check if there is a SnapPoint nearby while carrying the object
	FTransform SnapTransform;
	AActor* SnapActor = nullptr;
	USceneComponent* SnapComp = nullptr;
	if (FindBestSnapTarget(SnapTransform, SnapActor, SnapComp))
	{
		CandidateSnapActor = SnapActor;
		CandidateSnapComponent = SnapComp;

		if (bDrawDebugHelpers && GetWorld())
		{
			DrawDebugSphere(GetWorld(), SnapTransform.GetLocation(), 22.0f, 16, FColor::Green, false, -1.0f, 0, 2.5f);
			DrawDebugLine(GetWorld(), Prim->GetComponentLocation(), SnapTransform.GetLocation(), FColor::Green, false, -1.0f, 0, 2.0f);
		}

		if (bAutoSnapWhenBroughtClose)
		{
			SnapHeldObjectToTarget(SnapTransform, SnapActor);
		}
	}
	else
	{
		CandidateSnapActor = nullptr;
		CandidateSnapComponent = nullptr;
	}
}

void USpaceGrabSnapComponent::UpdateSnapAnimation(float DeltaTime)
{
	if (!bIsAnimatingSnap || !SnappingActor.IsValid())
	{
		bIsAnimatingSnap = false;
		return;
	}

	AActor* Item = SnappingActor.Get();
	UPrimitiveComponent* Prim = SnappingPrimitive.Get();

	const FVector CurrentLoc = Prim ? Prim->GetComponentLocation() : Item->GetActorLocation();
	const FQuat CurrentQuat = Prim ? Prim->GetComponentQuat() : Item->GetActorQuat();

	const FVector TargetLoc = TargetSnapTransform.GetLocation();
	const FQuat TargetQuat = TargetSnapTransform.GetRotation();

	const FVector NewLoc = FMath::VInterpTo(CurrentLoc, TargetLoc, DeltaTime, SnapTransformInterpSpeed);
	const FQuat NewQuat = FMath::QInterpTo(CurrentQuat, TargetQuat, DeltaTime, SnapTransformInterpSpeed);

	if (Prim)
	{
		Prim->SetWorldLocationAndRotation(NewLoc, NewQuat, false, nullptr, ETeleportType::TeleportPhysics);
	}
	Item->SetActorLocationAndRotation(NewLoc, NewQuat, false, nullptr, ETeleportType::TeleportPhysics);

	if (FVector::DistSquared(NewLoc, TargetLoc) < 0.25f && CurrentQuat.AngularDistance(TargetQuat) < 0.01f)
	{
		if (Prim)
		{
			Prim->SetWorldLocationAndRotation(TargetLoc, TargetQuat, false, nullptr, ETeleportType::TeleportPhysics);
		}
		Item->SetActorLocationAndRotation(TargetLoc, TargetQuat, false, nullptr, ETeleportType::TeleportPhysics);
		bIsAnimatingSnap = false;
	}
}

void USpaceGrabSnapComponent::ProcessInputBindings()
{
	if (bInputLockedForUI || !bUseDirectKeyBindings || !OwnerCharacter)
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(OwnerCharacter->GetController());
	if (!PC || !PC->IsLocalController())
	{
		return;
	}

	// Dedicated [T] key: Toggles the Space Tablet & AI Technical Quiz anytime from anywhere!
	if (PC->WasInputKeyJustPressed(EKeys::T))
	{
		if (USpaceTabletQuizComponent* TabletQuizComp = USpaceTabletQuizComponent::GetSpaceTabletQuizComponent(this))
		{
			TabletQuizComp->EnsureDefaultWidgetClass();
			TabletQuizComp->ToggleTabletUI();
			return;
		}
	}

	// 1. E Key: INTERACT ONLY! NEVER drops held objects!
	// Used for: Terminals, Quest targets, Buttons, Valves, Oxygen tanks, Diagnostic Tablet, etc.
	if (PC->WasInputKeyJustPressed(EKeys::E))
	{
		// Always attempt to interact with whatever object/mechanism/tablet is in front
		if (TryInteractWithObjectInFront())
		{
			return;
		}

		// Proximity check for Diagnostic Tablet vs Active Quest Target
		if (UMetaAIComponent* MetaAI = UMetaAIComponent::GetMetaAIComponentGlobal(this))
		{
			AActor* TabletAct = MetaAI->FindDiagnosticTabletActor();
			const float DistToTablet = TabletAct ? FVector::Dist(OwnerCharacter->GetActorLocation(), TabletAct->GetActorLocation()) : 99999.0f;

			AActor* TargetAct = MetaAI->GetActiveQuestTargetActor();
			const float DistToTarget = (TargetAct && MetaAI->HasActiveQuest()) ? FVector::Dist(OwnerCharacter->GetActorLocation(), TargetAct->GetActorLocation()) : 99999.0f;

			// If player is close to the Diagnostic Tablet (<= 260cm), or closer to tablet than quest target, tablet opens!
			if (TabletAct && (DistToTablet <= 260.0f || (DistToTablet <= 380.0f && DistToTablet <= DistToTarget)))
			{
				MetaAI->HandleTrackedPlayerInteractWithE(TabletAct, nullptr, NAME_None);
				return;
			}

			if (MetaAI->HasActiveQuest() && TargetAct && DistToTarget <= 380.0f)
			{
				MetaAI->HandleTrackedPlayerInteractWithE(TargetAct, nullptr, NAME_None);
				return;
			}

			// Proximity fallback to tablet if within 380cm
			if (TabletAct && DistToTablet <= 380.0f)
			{
				MetaAI->HandleTrackedPlayerInteractWithE(TabletAct, nullptr, NAME_None);
				return;
			}
		}

		// If NOT holding an object, player can use [E] to grab a grabbable object in reach
		if (!HeldActor.IsValid())
		{
			TryGrabObject();
		}
		// NOTE: If already holding an object, [E] NEVER drops it! The object remains firmly held in hand!
		return;
	}

	if (PC->WasInputKeyJustPressed(EKeys::F))
	{
		ToggleGrabOrDrop();
		return;
	}

	// 2. Left Mouse Button (LMB):
	// A single click on a Grabbable object within reach grabs it!
	// Once grabbed, the object remains held in hand and LMB NEVER drops it — LMB returns 100% to normal wall-grab & launch controls!
	if (bAllowLMBToGrabTaggedObjects && PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		if (!HeldActor.IsValid())
		{
			AActor* Candidate = nullptr;
			if (IsGrabbableInReach(Candidate))
			{
				TryGrabObject();
				return;
			}
		}
		// Notice: If already holding an object, we DO NOT drop it on LMB!
		// We leave LMB completely free for SpaceFloatingComponent to perform Wall Grab, Crawl & Launch!
	}

	// 3. Right Mouse Button (RMB):
	// - If holding an object: RMB drops it (or Shift+RMB throws it forward).
	// - If not holding an object: RMB can also grab a grabbable object in reach!
	if (bUseRMBToGrabAndHold && PC->WasInputKeyJustPressed(EKeys::RightMouseButton))
	{
		if (HeldActor.IsValid())
		{
			if (PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift))
			{
				ThrowHeldObject();
			}
			else
			{
				DropHeldObject();
			}
			return;
		}
		else
		{
			AActor* Candidate = nullptr;
			if (IsGrabbableInReach(Candidate))
			{
				TryGrabObject();
				return;
			}
		}
	}

	// 4. Throw / Launch Held Object into a target spot using G or Middle Mouse Button
	if (HeldActor.IsValid())
	{
		if (PC->WasInputKeyJustPressed(EKeys::G) || PC->WasInputKeyJustPressed(EKeys::MiddleMouseButton))
		{
			ThrowHeldObject();
		}
	}
}

void USpaceGrabSnapComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Restore Pawn collision on recently dropped object after the grace period expires
	if (bWaitingToRestoreCollision && GetWorld())
	{
		if (GetWorld()->GetTimeSeconds() >= RestoreCollisionTimestamp)
		{
			RestoreRecentlyDroppedCollision();
		}
	}

	ProcessInputBindings();

	if (HeldActor.IsValid())
	{
		UpdateHeldObjectTransform(DeltaTime);
	}
	else if (bDrawDebugHelpers && GetWorld())
	{
		AActor* LookAtGrabbable = nullptr;
		if (IsGrabbableInReach(LookAtGrabbable) && LookAtGrabbable)
		{
			DrawDebugBox(
				GetWorld(),
				LookAtGrabbable->GetActorLocation(),
				LookAtGrabbable->GetComponentsBoundingBox().GetExtent() + FVector(2.0f),
				FColor::Cyan,
				false,
				-1.0f,
				0,
				1.5f
			);
		}
	}

	if (bIsAnimatingSnap)
	{
		UpdateSnapAnimation(DeltaTime);
	}
}
