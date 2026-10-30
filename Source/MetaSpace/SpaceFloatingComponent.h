// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
#include "SpaceFloatingComponent.generated.h"

class ACharacter;
class UCharacterMovementComponent;
class UPrimitiveComponent;
class USceneComponent;
class UCameraComponent;
class USpringArmComponent;

/**
 * Delegate fired whenever the trainee performs a Zero-G Hand Pull (LMB), Aimed Launch, or Push-Off (RMB).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnZeroGHandAction, bool, bIsPullAction, const FVector&, TargetPoint, const FVector&, AppliedImpulse);

/**
 * Detailed delegate fired whenever the trainee sticks/grabs a wall ("wall_grab_start"),
 * walks/crawls along a wall ("wall_walk"), brakes/stops on a wall ("wall_stop_brake"),
 * or launches off a wall ("wall_launch" / "wall_pull").
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnZeroGWallInteraction, const FString&, InteractionType, AActor*, SurfaceActor, const FVector&, WorldPoint, float, MetricValue);

/**
 * Zero-Gravity First-Person 6DoF Space Training Simulator Component.
 *
 * Technical Features:
 * 1. Character Setup: GravityScale = 0, GroundFriction = 0, BrakingDecelerationFlying = 0, FluidFriction = 0, MovementMode = MOVE_Flying.
 * 2. Detection (Line Trace): Casts a reach ray from the First-Person Camera center forward.
 * 3. Grab, Hold & Aimed Slingshot Launch (Hold & Release LMB or RMB):
 *    - Tap LMB: Pulls the trainee directly toward the grabbed surface point.
 *    - Hold LMB on a wall/bar: Anchors/holds onto the wall in zero-G (stopping drift), lets the trainee freely look around in ANY direction, and upon releasing LMB (or pressing RMB while holding), launches the trainee in that exact chosen direction!
 *    - Also supports flick/throw movement (dragging the mouse while grabbing flings the trainee in the opposite direction of the hand drag, like Echo VR / ISS training!).
 * 4. Push Mechanic (Right Mouse Button):
 *    - If holding onto a wall (with LMB) or looking away from a wall within reach: Launches the trainee towards the camera's aimed direction (or away from wall).
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Space Floating Component"))
class METASPACE_API USpaceFloatingComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	USpaceFloatingComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:	
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =========================================================================
	// Settings - General
	// =========================================================================

	/** Whether zero-gravity floating is active immediately on BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|General")
	bool bStartFloatingOnBeginPlay = true;

	/** Randomize phase offset on BeginPlay so multiple objects don't float in identical sync */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|General")
	bool bRandomizePhase = true;

	// =========================================================================
	// Settings - 1. First-Person 6DoF Camera & Zero-G Character Setup
	// =========================================================================

	/** Automatically convert the Character's camera into a true First-Person 6DoF helmet view on BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	bool bAutoConfigureFirstPersonView = true;

	/** Relative location of the First-Person Camera (head/eye height inside the capsule) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	FVector FirstPersonCameraRelativeOffset = FVector(15.0f, 0.0f, 64.0f);

	/** Hide the character's third-person body mesh from the first-person camera view so it doesn't clip the screen */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	bool bHideThirdPersonBodyInFirstPerson = false;

	/** Disable traditional WASD thruster walking so the trainee MUST use the realistic Hand Push/Pull/Grab mechanics */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	bool bDisableWASDWalking = true;

	/** Optional: Allow emergency suit micro-thrusters with WASD only if bDisableWASDWalking is set to false */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	bool bEnable6DoFControls = true;

	/** Automatically read LMB (Grab/Pull/Launch), RMB (Push/Launch), and Q/E (Roll) directly from PlayerController */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	bool bUseDirectKeyBindings = true;

	/** Ground Friction in Zero-G (0.0 for frictionless space simulation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup", meta = (ClampMin = "0.0"))
	float ZeroGGroundFriction = 0.0f;

	/** Braking Deceleration in Flying mode (0.0 so the character floats indefinitely) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup", meta = (ClampMin = "0.0"))
	float ZeroGBrakingDeceleration = 0.0f;

	/** Fluid Friction in Zero-G (0.0 = pure vacuum infinite drift) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup", meta = (ClampMin = "0.0"))
	float ZeroGFluidFriction = 0.0f;

	/** Maximum 3D drifting/flying speed in Zero-G (cm/s) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup", meta = (ClampMin = "0.0"))
	float ZeroGMaxFlySpeed = 850.0f;

	/** Initial upward height offset (cm) on BeginPlay so the character immediately unsticks from any floor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup", meta = (ClampMin = "0.0"))
	float InitialCharacterLiftHeight = 35.0f;

	/** Small upward velocity on BeginPlay to lift the character off the floor into a floating state */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|1. First-Person & Zero-G Setup")
	float InitialCharacterLiftVelocity = 15.0f;

	// =========================================================================
	// Settings - 2. Arm Reach Detection (First-Person Line Trace)
	// =========================================================================

	/** Maximum reach distance of the trainee's arms from the First-Person Camera center (in cm, e.g., 220-260 units for comfortable grabbing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|2. Detection (Line Trace)", meta = (ClampMin = "50.0", ClampMax = "1000.0"))
	float HandReachDistance = 240.0f;

	/** Small sphere sweep radius (0 = pure LineTraceSingleByChannel, >0 = slightly forgiving hand grab trace) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|2. Detection (Line Trace)", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float HandTraceSphereRadius = 0.0f;

	/** Collision channel used for the Hand Line Trace */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|2. Detection (Line Trace)")
	TEnumAsByte<ECollisionChannel> HandTraceChannel = ECC_Visibility;

	/** Draw visual debug line, grab anchor, and launch trajectory arrow in the viewport */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|2. Detection (Line Trace)")
	bool bDrawHandReachDebug = true;

	// =========================================================================
	// Settings - 3 & 4. Grab, Hold, Aimed Launch & Push/Pull Mechanics
	// =========================================================================

	/** Impulse strength applied towards the Hit Location on a quick Left Mouse Click (Pull in) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "10.0"))
	float PullImpulseStrength = 380.0f;

	/** Strong impulse strength applied when pushing off (RMB) or launching from a grabbed wall towards your aimed direction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "10.0"))
	float PushImpulseStrength = 580.0f;

	/**
	 * Enable "Grab, Hold & Aim Launch":
	 * Hold LMB on a wall to grab & stabilize yourself on the wall, look/aim in ANY direction (or drag), and release LMB (or click RMB) to launch yourself in that direction!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics")
	bool bEnableWallGrabAndAimLaunch = true;

	/** How long (in seconds) LMB must be held to enter "Wall Grab & Aim Launch" mode instead of a simple quick Pull tap */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float GrabHoldThresholdSeconds = 0.18f;

	/** Optimal distance (cm) from the grabbed point on the wall where the trainee's body is held while grabbing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "40.0", ClampMax = "300.0"))
	float GrabHoldAnchorDistance = 95.0f;

	/** How fast the trainee is pulled into the wall anchor when holding LMB */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "1.0"))
	float GrabAnchorPullSpeed = 6.0f;

	/**
	 * Direction Mode when Releasing a Grab (LMB Release):
	 * - If true: Launches the trainee directly toward where the camera is currently looking/aiming (super intuitive!).
	 * - If false: Slingshot/Drag mode — launches in the opposite direction of how the player dragged their camera/hand away from the grab point.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics")
	bool bLaunchTowardCameraLookOnRelease = true;

	/** If true, dragging WASD while holding onto a wall also steers your launch direction or crawls along the wall! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics")
	bool bAllowDirectionalSteerWhileGrabbing = true;

	/** How much existing drift velocity is cancelled when grabbing a wall to pull/push */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics")
	bool bDampenExistingVelocityOnGrab = true;

	/** Velocity damping factor on quick grab */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GrabVelocityDampingFactor = 0.55f;

	/** Cooldown time (in seconds) between consecutive hand pulls/pushes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "0.05"))
	float HandActionCooldown = 0.12f;

	/** Also apply an equal-and-opposite Newton's 3rd Law impulse if the trainee pushes/pulls on a movable physics prop */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics")
	bool bApplyNewtonianReactionToPhysicsProps = true;

	/** Approximate mass of the Astronaut Trainee in kg (used to compute realistic Newton's 3rd Law mass ratio when grabbing/throwing movable objects!) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "10.0"))
	float TraineeBodyMassKg = 80.0f;

	/** Minimum mass (in kg) required for a movable physics object to act as a wall anchor that can pull/launch the astronaut! Objects lighter than this (e.g., 8kg chairs, boxes, tools) CANNOT pull or launch the astronaut! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "1.0"))
	float MinObjectMassToAnchorPlayer = 50.0f;

	/** Maximum distance (in cm) before a grabbed moving object slips out of the trainee's hand reach */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|3. Grab, Pull & Launch Mechanics", meta = (ClampMin = "100.0"))
	float MaxGrabStretchDistance = 320.0f;

	// =========================================================================
	// Settings - 4. Zero-G Stranded Anti-Stuck & Micro-Drift Protocol
	// =========================================================================

	/**
	 * Automatically apply a gentle continuous micro-drift whenever the astronaut is stranded in the middle
	 * of the room with no walls/objects in arm's reach and stops moving (velocity < MinStrandedDriftSpeed),
	 * ensuring the player can NEVER get permanently stuck frozen at 0 velocity in mid-air!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. Stranded Anti-Stuck & Micro-Drift")
	bool bEnableStrandedAntiStuckDrift = true;

	/**
	 * Minimum speed (in cm/s) maintained when stranded in mid-air with no reachable walls (minimum 0.01 cm/s).
	 * If the astronaut stops moving, suit micro-ventilation smoothly drifts them towards where they look!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. Stranded Anti-Stuck & Micro-Drift", meta = (ClampMin = "0.01", ClampMax = "250.0"))
	float MinStrandedDriftSpeed = 25.0f;

	/**
	 * Allow emergency micro-thruster navigation (WASD, Space, Ctrl) whenever the astronaut is stranded in the middle
	 * with no walls within reach, even if bDisableWASDWalking is set to true!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. Stranded Anti-Stuck & Micro-Drift")
	bool bAllowEmergencyThrustersWhenStranded = true;

	/** Thrust acceleration speed when using emergency micro-thrusters in open space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. Stranded Anti-Stuck & Micro-Drift", meta = (ClampMin = "10.0", ClampMax = "600.0"))
	float EmergencyStrandedThrusterSpeed = 160.0f;

	// =========================================================================
	// Settings - 5. 6DoF Body Orientation & Roll (Q / E)
	// =========================================================================

	/** Speed of manual 6DoF Roll rotation when pressing Q or E (degrees per second) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation", meta = (ClampMin = "0.0"))
	float RollRotationSpeed = 90.0f;

	/** Smoothly align the character's 3D body rotation (Pitch, Yaw, and Roll) with the First-Person Camera */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation")
	bool bAlignCharacterBodyToCamera = true;

	/** Interpolation speed for aligning character body & camera roll in 3D space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation", meta = (ClampMin = "0.1"))
	float BodyAlignmentInterpSpeed = 10.0f;

	/** How much the character's capsule pitch follows the camera's pitch (1.0 = full 6DoF orientation) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PitchAlignmentFactor = 1.0f;

	/** Keep a soft cushion above floors so the character never snaps to ground walking mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation")
	bool bPreventGroundContact = true;

	/** Minimum clearance distance (cm) from the bottom of the capsule to the floor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation", meta = (ClampMin = "5.0"))
	float MinFloorClearance = 15.0f;

	/** Strength of the upward cushion push when getting too close to the floor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|4. 6DoF Roll & Orientation", meta = (ClampMin = "0.0"))
	float FloorRepulsionStrength = 250.0f;

	// =========================================================================
	// Settings - Space Props & Room Actors (100% Physics Zero-G & Pushable)
	// =========================================================================

	/** Enable subtle zero-G sine-wave bobbing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Procedural Motion")
	bool bEnableProceduralBobbing = true;

	/** Amplitude of 3D bobbing (in cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Procedural Motion", meta = (ClampMin = "0.0"))
	FVector BobbingAmplitude = FVector(1.5f, 1.5f, 3.5f);

	/** Speed/Frequency of the bobbing oscillation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Procedural Motion", meta = (ClampMin = "0.0"))
	float BobbingSpeed = 1.1f;

	/** Amplitude of gentle rotational sway (Pitch, Yaw, Roll in degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Procedural Motion")
	FRotator RotationSwayAmplitude = FRotator(2.0f, 2.0f, 2.0f);

	/** Speed/Frequency of the rotational sway */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Procedural Motion", meta = (ClampMin = "0.0"))
	float RotationSwaySpeed = 0.8f;

	/** Automatically force Actor and its Mesh components to Movable so they can float even if placed as Static */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props")
	bool bForceMovableOnProps = true;

	/** Automatically enable SimulatePhysics on the prop mesh so it is 100% physical and reacts to collisions/pushes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props")
	bool bAutoEnableSimulatePhysicsOnProps = true;

	/** Initial upward height lift (cm) on BeginPlay for props so they hover slightly above tables/floors */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float InitialPropLiftHeight = 15.0f;

	/** Gentle initial rotation speed in place (degrees per second) on BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float InPlaceRotationSpeed = 14.0f;

	/** Multiplier for how strongly a Character or Pawn pushes this floating object when colliding with it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float CharacterBumpPushStrength = 0.85f;

	/** Minimum push speed (cm/s) imparted when a Character touches/bumps this prop in zero-G */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float MinCharacterBumpSpeed = 120.0f;

	/** Spin/tumble torque imparted when a Character bumps this prop off-center */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float CharacterBumpSpinStrength = 35.0f;

	/** Linear damping in zero-G for floating props */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float ZeroGLinearDamping = 0.25f;

	/** Angular damping in zero-G for floating props */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float ZeroGAngularDamping = 0.2f;

	/** Optional: apply a small random drift velocity at BeginPlay to props */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props")
	bool bApplyInitialRandomDrift = false;

	/** Initial drift speed if bApplyInitialRandomDrift is true */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Simulator|Physics Props", meta = (ClampMin = "0.0"))
	float InitialDriftSpeed = 10.0f;

	// =========================================================================
	// Blueprint Events & Callable Functions (Push/Pull/Grab & 6DoF API)
	// =========================================================================

	/** Broadcasts whenever the trainee successfully pulls (LMB), launches, or pushes off (RMB) a surface */
	UPROPERTY(BlueprintAssignable, Category = "Space Simulator|Events")
	FOnZeroGHandAction OnZeroGHandAction;

	/** Broadcasts detailed wall interactions: "wall_grab_start", "wall_walk", "wall_stop_brake", "wall_launch", "wall_pull" */
	UPROPERTY(BlueprintAssignable, Category = "Space Simulator|Events")
	FOnZeroGWallInteraction OnZeroGWallInteraction;

	/** Enable or disable zero-gravity floating at runtime */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator")
	void SetFloatingEnabled(bool bEnable);

	/** Returns true if zero-gravity floating is currently active */
	UFUNCTION(BlueprintPure, Category = "Space Simulator")
	bool IsFloatingEnabled() const { return bIsFloatingActive; }

	/** True if player 6DoF thrusters and wall grabbing are locked for UI / Widget interaction */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Space Simulator|UI")
	bool bInputLockedForUI = false;

	UFUNCTION(BlueprintCallable, Category = "Space Simulator|UI")
	void SetInputLockedForUI(bool bLock);

	/** Performs the First-Person Camera Line Trace to detect grab/push surfaces */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|Hand Push & Pull")
	bool PerformHandReachTrace(FHitResult& OutHitResult) const;

	/** Pull Mechanic (Left Mouse Button) */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|Hand Push & Pull")
	bool TryHandPull();

	/** Push Mechanic (Right Mouse Button) — or aimed launch if currently holding a wall! */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|Hand Push & Pull")
	bool TryHandPush();

	/** Start grabbing/holding onto a surface within reach (called on LMB Press) */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|Hand Push & Pull")
	bool StartWallGrab();

	/** Release the grabbed surface and launch in the aimed/dragged direction (called on LMB Release) */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|Hand Push & Pull")
	void ReleaseWallGrabAndLaunch();

	/** Release the grabbed surface (or stop movement if near a surface) and stay stationary in place without launching (called on RMB Press) */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|Hand Push & Pull")
	void CancelGrabAndStopInPlace();

	/** Returns true if the trainee is currently holding onto a wall/surface */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|Hand Push & Pull")
	bool IsGrabbingSurface() const { return bIsGrabbingWall; }

	/** Returns the Actor of the wall/surface currently grabbed by the trainee (or nullptr if not grabbing) */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|Hand Push & Pull")
	AActor* GetGrabbedSurfaceActor() const;

	/** Returns total distance (in meters) the trainee has walked/crawled along walls while grabbing */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|Hand Push & Pull")
	float GetTotalWallWalkDistanceMeters() const { return TotalWallWalkDistanceCm / 100.0f; }

	/** Returns true if the trainee is currently looking at a surface within arm's reach */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|Hand Push & Pull")
	bool IsSurfaceWithinHandReach() const { return bHasSurfaceInReach; }

	/** Returns the latest Line Trace HitResult from the trainee's hand reach check */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|Hand Push & Pull")
	FHitResult GetCurrentHandReachHit() const { return CurrentHandReachHit; }

	/** Returns true if the astronaut is stranded in open space with no walls or grab surfaces within reach */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|Hand Push & Pull")
	bool IsStrandedInOpenSpace(float CheckRadius = 0.0f) const;

	/** 6DoF Roll Input (Q = -1 Left Roll, E = +1 Right Roll) */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|6DoF Input")
	void AddRollSpaceInput(float RollInput);

	/** Optional 6DoF Thruster Move */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|6DoF Input")
	void MoveInSpace(float RightInput, float ForwardInput);

	/** Optional 6DoF Vertical Thrust */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|6DoF Input")
	void AddVerticalSpaceInput(float UpInput);

	/** Reset accumulated manual Roll angle smoothly back to zero */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator|6DoF Input")
	void ResetRollOrientation();

	/** Add a directional push/impulse in 3D space */
	UFUNCTION(BlueprintCallable, Category = "Space Simulator")
	void AddSpaceImpulse(FVector ImpulseVelocity, bool bVelocityChange = true);

	/** Returns the current accumulated Roll angle in degrees */
	UFUNCTION(BlueprintPure, Category = "Space Simulator|6DoF Input")
	float GetCurrentRollAngle() const { return CurrentRollAngle; }

protected:
	UFUNCTION()
	void OnPropComponentHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

private:
	bool bIsFloatingActive = false;
	float RunningTime = 0.0f;
	float PhaseOffset = 0.0f;
	float LastBumpTime = -10.0f;
	float LastHandActionTime = -10.0f;

	// Hand Trace & Grab-Hold-Launch State
	bool bHasSurfaceInReach = false;
	FHitResult CurrentHandReachHit;

	bool bIsGrabbingWall = false;
	float GrabStartTime = 0.0f;
	float LastWallWalkBroadcastTime = -10.0f;
	float TotalWallWalkDistanceCm = 0.0f;
	FVector GrabbedWorldPoint = FVector::ZeroVector;
	FVector GrabbedLocalPoint = FVector::ZeroVector;
	FVector GrabbedSurfaceNormal = FVector::UpVector;
	FVector GrabbedLocalNormal = FVector::UpVector;
	FVector GrabInitialLookDir = FVector::ForwardVector;
	TWeakObjectPtr<UPrimitiveComponent> GrabbedComponent = nullptr;

	// Random per-object axes
	FVector RandomDriftDirection = FVector::UpVector;
	FVector RandomRotationAxis = FVector(0.3f, 0.5f, 0.8f);

	// 6DoF Runtime State
	float CurrentRollAngle = 0.0f;
	float FrameRollInput = 0.0f;

	UPROPERTY(Transient)
	ACharacter* OwnerCharacter = nullptr;

	UPROPERTY(Transient)
	UCharacterMovementComponent* OwnerMovementComp = nullptr;

	UPROPERTY(Transient)
	UCameraComponent* OwnerCamera = nullptr;

	UPROPERTY(Transient)
	USpringArmComponent* OwnerSpringArm = nullptr;

	UPROPERTY(Transient)
	UPrimitiveComponent* OwnerRootPrimitive = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPrimitiveComponent>> PropPrimitives;

	UPROPERTY(Transient)
	USceneComponent* VisualTargetComponent = nullptr;

	// Cached initial transforms & settings
	FVector BaseRelativeLocation = FVector::ZeroVector;
	FRotator BaseRelativeRotation = FRotator::ZeroRotator;
	FVector BaseActorLocation = FVector::ZeroVector;
	FRotator BaseActorRotation = FRotator::ZeroRotator;
	FVector PreviousWaveVelocity = FVector::ZeroVector;

	float SavedGravityScale = 1.0f;
	float SavedGroundFriction = 8.0f;
	float SavedBrakingDecelerationFlying = 0.0f;
	float SavedMaxFlySpeed = 600.0f;
	float SavedMaxAcceleration = 2048.0f;
	float SavedBrakingFrictionFactor = 2.0f;
	float SavedWalkableFloorAngle = 44.0f;
	float SavedWalkableFloorZ = 0.71f;
	bool bSavedOrientRotationToMovement = true;
	float SavedTargetArmLength = 400.0f;
	FVector SavedSpringArmLocation = FVector::ZeroVector;
	float SavedLinearDamping = 0.01f;
	float SavedAngularDamping = 0.0f;

	FVector GetCameraAimDirection() const;
	FVector ComputeGrabLaunchDirection() const;
	void UpdateGrabHoldState(float DeltaTime);
	void ConfigureFirstPersonCamera();
	void RestoreThirdPersonCamera();
	void DiscoverPropPrimitives(AActor* Owner);
	void ActivateZeroG();
	void DeactivateZeroG();
	void EnforceZeroGOffGround(float DeltaTime);
	void EnforceStrandedMinimumDrift(float DeltaTime);
	void Process6DoFCharacterControls(float DeltaTime);
	void UpdateCharacter3DOrientation(float DeltaTime, const FRotator& ProceduralRotationOffset);
	void UpdateProceduralFloating(float DeltaTime);
};
