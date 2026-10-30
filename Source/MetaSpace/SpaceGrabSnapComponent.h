// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
#include "SpaceGrabSnapComponent.generated.h"

class ACharacter;
class UCameraComponent;
class UPrimitiveComponent;
class USceneComponent;

/**
 * Delegate fired when an object is Grabbed, Dropped, or Snapped into a target slot.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnSpaceItemGrabEvent, AActor*, ItemActor, AActor*, SnapTargetActor, bool, bWasSnapped);

/**
 * Delegate fired when the player looks at an Interactive Actor/Component (e.g., Valve, Switch, Panel) and presses [E].
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnSpacePlayerInteractEvent, AActor*, InteractedActor, UPrimitiveComponent*, InteractedComponent, FName, ComponentName);

/**
 * Tag-based Grabbing, Carrying, Dropping, Throwing & Snapping Component for the Space Training Simulator.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Space Grab & Snap Component"))
class METASPACE_API USpaceGrabSnapComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	USpaceGrabSnapComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:	
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =========================================================================
	// 1. Tag Settings (Grabbable & Snap Point Tags)
	// =========================================================================

	/** Actor or Component Tag required on an object so the player can Grab and carry it (default: "Grabbable") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|1. Tags")
	FName GrabbableTag = FName("Grabbable");

	/** Actor or Component Tag on a target slot/zone where held objects can Snap into place (default: "SnapPoint") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|1. Tags")
	FName SnapTargetTag = FName("SnapPoint");

	/** Actor or Component Tag on an interactive mechanism (Valve, Button, Lever, Panel) that triggers when the player presses [E] (default: "Interactable") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|1. Tags")
	FName InteractableTag = FName("Interactable");

	// =========================================================================
	// 2. Controls & Reach Settings
	// =========================================================================

	/** Automatically bind Keyboard/Mouse keys (E / F to interact/grab, RMB to Grab/Drop items, LMB for Wall Grab) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach")
	bool bUseDirectKeyBindings = true;

	/**
	 * Use Right Mouse Button (RMB) to Grab, Carry, and Drop items/tools!
	 * This frees Left Mouse Button (LMB) completely so you can grab walls, crawl, push, and launch off walls while holding items in hand!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach")
	bool bUseRMBToGrabAndHold = true;

	/** Allow Left Mouse Button (LMB) to Grab tagged objects with a single click. Once grabbed, the object remains held in hand and LMB returns 100% to normal wall-grab & launch controls! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach")
	bool bAllowLMBToGrabTaggedObjects = true;

	/** Maximum distance (in cm) from the First-Person Camera to grab a tagged object */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach", meta = (ClampMin = "50.0", ClampMax = "1000.0"))
	float GrabReachDistance = 300.0f;

	/** Sphere sweep radius (cm) for the grab trace so moving/spinning objects are very easy to grab again! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach", meta = (ClampMin = "0.0", ClampMax = "80.0"))
	float GrabSweepRadius = 28.0f;

	/** Distance (in cm) in front of the First-Person Camera where the grabbed object is held while carrying */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach", meta = (ClampMin = "40.0", ClampMax = "400.0"))
	float HoldDistanceFromCamera = 135.0f;

	/** Smoothness speed for carrying and rotating the held object in front of the camera */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float HoldInterpSpeed = 16.0f;

	/** Speed (cm/s) applied when throwing/launching the held object into a spot (G or RMB) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach", meta = (ClampMin = "0.0"))
	float ThrowLaunchSpeed = 380.0f;

	/** Grace period (seconds) after dropping/throwing an object before the player's body collision can bump it again */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|2. Controls & Reach", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float PostDropPawnIgnoreDuration = 0.45f;

	// =========================================================================
	// 3. Snapping Settings
	// =========================================================================

	/** Radius (in cm) around the held object to search for a valid "SnapPoint" Actor/Component */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|3. Snapping", meta = (ClampMin = "10.0", ClampMax = "500.0"))
	float SnapDetectionRadius = 120.0f;

	/** Automatically snap the object as soon as it gets close to a SnapPoint while held */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|3. Snapping")
	bool bAutoSnapWhenBroughtClose = false;

	/** Smoothly interpolate the snapped object into the exact SnapPoint transform */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|3. Snapping", meta = (ClampMin = "1.0", ClampMax = "50.0"))
	float SnapTransformInterpSpeed = 16.0f;

	/** Lock physics on the object once it is snapped into place (until the player grabs it again) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|3. Snapping")
	bool bLockPhysicsWhenSnapped = true;

	/** Draw debug indicators for Grabbable objects and Snap Points in the viewport */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Grab & Snap|3. Snapping")
	bool bDrawDebugHelpers = true;

	// =========================================================================
	// Blueprint Events & Callable Functions
	// =========================================================================

	/** Fired when an object is grabbed, dropped, or snapped */
	UPROPERTY(BlueprintAssignable, Category = "Space Grab & Snap|Events")
	FOnSpaceItemGrabEvent OnItemGrabStateChanged;

	/** Fired when the player looks at an Interactive Actor/Component (tagged "Interactable" or with SpaceAIIdentityComponent bCanInteractWithEKey=true) and presses [E] */
	UPROPERTY(BlueprintAssignable, Category = "Space Grab & Snap|Events")
	FOnSpacePlayerInteractEvent OnPlayerInteractedWithActor;

	/** Traces in front of the camera for an Interactive Actor/Component and triggers its [E] key interaction! Returns true if an interactive object was triggered. */
	UFUNCTION(BlueprintCallable, Category = "Space Grab & Snap|Interactive System (Press E)")
	bool TryInteractWithObjectInFront();

	/** Try to grab a tagged object in front of the camera. Returns true if an object was grabbed. */
	UFUNCTION(BlueprintCallable, Category = "Space Grab & Snap")
	bool TryGrabObject();

	/** Drop the currently held object gently in place (or snap it if within range of a SnapPoint). */
	UFUNCTION(BlueprintCallable, Category = "Space Grab & Snap")
	void DropHeldObject();

	/** Launch/throw the currently held object in the camera's aim direction (or into a targeted SnapPoint). */
	UFUNCTION(BlueprintCallable, Category = "Space Grab & Snap")
	void ThrowHeldObject();

	/** Toggle: If holding an object, drop/snap it; otherwise try to grab a tagged object. */
	UFUNCTION(BlueprintCallable, Category = "Space Grab & Snap")
	void ToggleGrabOrDrop();

	/** Returns true if the player is currently holding a grabbable object */
	UFUNCTION(BlueprintPure, Category = "Space Grab & Snap")
	bool IsHoldingObject() const { return HeldActor.IsValid(); }

	/** Returns the currently held Actor (or nullptr if none) */
	UFUNCTION(BlueprintPure, Category = "Space Grab & Snap")
	AActor* GetHeldActor() const { return HeldActor.Get(); }

	/** Returns true if a valid Grabbable Actor is currently in front of the camera within reach */
	UFUNCTION(BlueprintPure, Category = "Space Grab & Snap")
	bool IsGrabbableInReach(AActor*& OutGrabbableActor) const;

	/** True if player input is temporarily locked for UI / Widget interaction */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Space Grab & Snap")
	bool bInputLockedForUI = false;

	UFUNCTION(BlueprintCallable, Category = "Space Grab & Snap")
	void SetInputLockedForUI(bool bLock) { bInputLockedForUI = bLock; }

private:
	UPROPERTY(Transient)
	ACharacter* OwnerCharacter = nullptr;

	UPROPERTY(Transient)
	UCameraComponent* OwnerCamera = nullptr;

	TWeakObjectPtr<AActor> HeldActor = nullptr;
	TWeakObjectPtr<UPrimitiveComponent> HeldPrimitive = nullptr;
	TWeakObjectPtr<USceneComponent> CandidateSnapComponent = nullptr;
	TWeakObjectPtr<AActor> CandidateSnapActor = nullptr;

	bool bHeldObjectWasSimulatingPhysics = false;
	FRotator HeldRelativeRotationOffset = FRotator::ZeroRotator;

	/** Saved relative transforms of all child components (BoxCollision, extra meshes, etc.) relative to HeldPrimitive so the whole Actor moves as one unit */
	TMap<TWeakObjectPtr<USceneComponent>, FTransform> HeldChildRelativeTransforms;

	/** Saved ECC_Pawn collision responses for ALL primitive components of HeldActor */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, ECollisionResponse> HeldActorSavedPawnResponses;

	// Post-drop grace period so the player capsule doesn't immediately kick the dropped item away
	TWeakObjectPtr<AActor> RecentlyDroppedActor = nullptr;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, ECollisionResponse> RecentlyDroppedPawnResponses;
	float RestoreCollisionTimestamp = 0.0f;
	bool bWaitingToRestoreCollision = false;

	// Active smooth snap animation state
	TWeakObjectPtr<AActor> SnappingActor = nullptr;
	TWeakObjectPtr<UPrimitiveComponent> SnappingPrimitive = nullptr;
	FTransform TargetSnapTransform = FTransform::Identity;
	bool bIsAnimatingSnap = false;

	void GetCameraView(FVector& OutLocation, FRotator& OutRotation) const;
	bool HasRequiredTag(const AActor* Actor, const UPrimitiveComponent* Comp, const FName& RequiredTag) const;
	UPrimitiveComponent* ResolveAndUnifyActorMasterPrimitive(AActor* Actor) const;
	void SyncActorChildComponents(AActor* Actor, UPrimitiveComponent* MasterPrim, const TMap<TWeakObjectPtr<USceneComponent>, FTransform>& RelativeTransforms) const;
	void RestoreRecentlyDroppedCollision();
	bool TraceForGrabbable(FHitResult& OutHit, AActor*& OutActor, UPrimitiveComponent*& OutPrimitive) const;
	bool FindBestSnapTarget(FTransform& OutSnapTransform, AActor*& OutSnapActor, USceneComponent*& OutSnapComp) const;
	void SnapHeldObjectToTarget(const FTransform& SnapTransform, AActor* SnapTargetActor);
	void UpdateHeldObjectTransform(float DeltaTime);
	void UpdateSnapAnimation(float DeltaTime);
	void ProcessInputBindings();
};
