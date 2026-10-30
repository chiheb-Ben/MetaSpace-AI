// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
#include "SpaceObjectComponent.generated.h"

class UPrimitiveComponent;
class ACharacter;

/**
 * Preset weight classes to quickly configure realistic Zero-G objects (Papers, Tools, Chairs, Heavy Crates)
 * or choose Custom to tweak every physics parameter manually.
 */
UENUM(BlueprintType)
enum class ESpaceObjectWeightClass : uint8
{
	/** Custom manual mass, damping, spin, and drift settings */
	Custom          UMETA(DisplayName = "Custom (Manual Settings)"),

	/** Feather-light items (e.g., Paper sheets, tablets, pens, small tools: ~0.1 - 0.5 kg) */
	FeatherLight    UMETA(DisplayName = "Feather Light (Paper / Small Tool)"),

	/** Medium cabin items (e.g., Chairs, laptops, food trays, helmets: ~5 - 15 kg) */
	MediumProp      UMETA(DisplayName = "Medium Object (Chair / Monitor / Bag)"),

	/** Heavy cargo items (e.g., Storage crates, oxygen tanks, lockers: ~40 - 120 kg) */
	HeavyCargo      UMETA(DisplayName = "Heavy Cargo (Crate / Equipment Rack)")
};

/**
 * Dedicated Zero-Gravity Physics Object Component for Room Props (Chairs, Crates, Papers, Tools, etc.).
 * Completely separate from the Character movement system so every object in the space station
 * can have its own realistic mass, autonomous 3D rotation, air-current drift, bounciness, and collision response!
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Space Object Component (Zero-G Prop)"))
class METASPACE_API USpaceObjectComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	USpaceObjectComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:	
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =========================================================================
	// 1. Preset & Mass / Weight Settings
	// =========================================================================

	/** Select a preset (Paper, Chair, Heavy Crate) or leave on Custom to control all values manually */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|1. Mass & Preset")
	ESpaceObjectWeightClass WeightPreset = ESpaceObjectWeightClass::Custom;

	/** Override the object's physical mass in Kilograms (kg). Heavy objects require much more force to push and can knock light objects away! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|1. Mass & Preset")
	bool bOverrideMassInKg = true;

	/** Mass of this object in Kilograms (e.g., 0.15 for Paper, 8.0 for Chair, 60.0 for Heavy Crate) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|1. Mass & Preset", meta = (EditCondition = "bOverrideMassInKg", ClampMin = "0.01", ClampMax = "5000.0"))
	float MassInKg = 8.0f;

	/** Automatically set the Actor and its StaticMesh/Primitive components to Movable & SimulatePhysics on BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|1. Mass & Preset")
	bool bAutoEnableZeroGPhysics = true;

	// =========================================================================
	// 2. Autonomous Zero-G Rotation (Self-Spin & Tumbling)
	// =========================================================================

	/** Should this object slowly rotate/tumble on its own in space? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|2. Autonomous Rotation")
	bool bEnableAutonomousRotation = true;

	/** Pick a random 3D rotation axis on BeginPlay so every chair/box/paper rotates differently */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|2. Autonomous Rotation")
	bool bUseRandomRotationAxis = true;

	/** Custom 3D rotation axis (used if bUseRandomRotationAxis is false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|2. Autonomous Rotation", meta = (EditCondition = "!bUseRandomRotationAxis"))
	FVector CustomRotationAxis = FVector(0.25f, 0.45f, 0.85f);

	/** Base rotation speed in degrees per second (e.g., 15 deg/s for slow cinematic space spin) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|2. Autonomous Rotation", meta = (ClampMin = "0.0", ClampMax = "360.0"))
	float SelfRotationSpeed = 16.0f;

	/** Random variation (+/- degrees/sec) added to SelfRotationSpeed so identical props spin at slightly different speeds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|2. Autonomous Rotation", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float RotationSpeedRandomVariance = 5.0f;

	/** If true, after a hard collision spins the object fast, it gradually settles back to its gentle SelfRotationSpeed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|2. Autonomous Rotation")
	bool bMaintainBaseRotationSpeed = true;

	// =========================================================================
	// 3. Zero-G Floating, Lift & Cabin Air Currents
	// =========================================================================

	/** Initial upward height lift (cm) on BeginPlay so objects resting on floors/tables immediately unstick and float */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement", meta = (ClampMin = "0.0"))
	float InitialLiftHeight = 14.0f;

	/** Initial gentle upward velocity (cm/s) on BeginPlay */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement")
	float InitialUpwardVelocity = 6.0f;

	/** Give the object a gentle initial 3D drift velocity when the simulation starts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement")
	bool bApplyInitialDrift = false;

	/** Initial 3D drift speed (cm/s) if bApplyInitialDrift is enabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement", meta = (EditCondition = "bApplyInitialDrift", ClampMin = "0.0"))
	float InitialDriftSpeed = 12.0f;

	/** Enable subtle zero-G floating wave (simulates gentle cabin ventilation / micro-gravity bobbing without runaway speed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement")
	bool bEnableMicroGravityWave = true;

	/** Distance amplitude (cm) of the gentle zero-G wave motion (e.g., 3-8 cm for heavy/medium items, 12 cm for papers) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement", meta = (EditCondition = "bEnableMicroGravityWave", ClampMin = "0.0"))
	FVector WaveAmplitude = FVector(3.0f, 3.0f, 6.5f);

	/** Frequency/speed of the gentle zero-G wave motion */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement", meta = (EditCondition = "bEnableMicroGravityWave", ClampMin = "0.0"))
	float WaveFrequency = 1.15f;

	/** Maximum linear velocity (cm/s) allowed for this object so physics glitches never shoot it through walls */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|3. Floating & Movement", meta = (ClampMin = "10.0"))
	float MaxAllowedSpeed = 650.0f;

	// =========================================================================
	// 4. Damping, Collisions & Player Push Response
	// =========================================================================

	/** Linear resistance in space (0.05 = floats across the whole room when pushed; 1.0 = slows down after a short distance) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|4. Physics & Push Response", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ZeroGLinearDamping = 0.2f;

	/** Rotational resistance in space (lower = spins longer when hit off-center) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|4. Physics & Push Response", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float ZeroGAngularDamping = 0.15f;

	/** Multiplier for how strongly this object reacts when the Trainee / Player bumps into it or pushes off it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|4. Physics & Push Response", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float PlayerCollisionPushMultiplier = 1.0f;

	/** How easily off-center impacts cause this object to tumble and spin in 3D space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|4. Physics & Push Response", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float ImpactTorqueMultiplier = 1.0f;

	/** Elastic bounce multiplier when this object collides with walls, floors, or other floating objects (0.0 = no extra bounce, 0.5 = realistic space ricochet) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space Object|4. Physics & Push Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SurfaceBounceCoefficient = 0.35f;

	// =========================================================================
	// Blueprint Callable Functions
	// =========================================================================

	/** Enable or disable zero-gravity on this object at runtime (e.g., when artificial gravity is turned on/off) */
	UFUNCTION(BlueprintCallable, Category = "Space Object")
	void SetZeroGEnabled(bool bEnable);

	/** Returns true if this object is currently in zero-gravity mode */
	UFUNCTION(BlueprintPure, Category = "Space Object")
	bool IsZeroGEnabled() const { return bIsZeroGActive; }

	/** Push this object in 3D space with a force/impulse (automatically respects the object's Mass unless bVelocityChange is true) */
	UFUNCTION(BlueprintCallable, Category = "Space Object")
	void ApplySpacePush(FVector PushVector, bool bVelocityChange = false);

	/** Apply a rotational torque impulse at a specific world impact point (causes realistic 3D tumbling) */
	UFUNCTION(BlueprintCallable, Category = "Space Object")
	void ApplySpacePushAtLocation(FVector PushForceOrImpulse, FVector WorldLocation);

	/** Change the object's mass in kg at runtime */
	UFUNCTION(BlueprintCallable, Category = "Space Object")
	void SetObjectMassInKg(float NewMassInKg);

	/** Returns the unified master physics primitive for this Actor */
	UFUNCTION(BlueprintPure, Category = "Space Object")
	UPrimitiveComponent* GetPrimaryPrimitive() const { return PrimaryPrimitive; }

	/** Unifies all child components (Meshes, BoxCollisions, etc.) under PrimaryPrimitive so the entire Actor moves as one piece */
	void UnifyActorComponents();

	/** Called by USpaceGrabSnapComponent when the player grabs or releases this object */
	void NotifyGrabStateChanged(bool bIsHeld);

protected:
	UFUNCTION()
	void OnObjectHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

private:
	bool bIsZeroGActive = false;
	bool bIsHeldByPlayer = false;
	float RunningTime = 0.0f;
	float EffectiveRotationSpeed = 16.0f;
	float LastHitTime = -10.0f;

	FVector RotationAxis = FVector::UpVector;
	FVector DriftDirection = FVector::ForwardVector;
	FVector PreviousWaveVelocity = FVector::ZeroVector;

	UPROPERTY(Transient)
	UPrimitiveComponent* PrimaryPrimitive = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPrimitiveComponent>> ManagedPrimitives;

	/** Stores initial relative transforms of child components (BoxCollision, extra meshes, etc.) relative to PrimaryPrimitive so they never separate */
	TMap<TWeakObjectPtr<USceneComponent>, FTransform> ChildRelativeTransforms;

	float SavedLinearDamping = 0.01f;
	float SavedAngularDamping = 0.0f;

	void ApplyWeightPresetValues();
	void DiscoverPrimitives();
	void SyncAttachedChildComponents();
	void ActivateObjectZeroG();
	void DeactivateObjectZeroG();
};
