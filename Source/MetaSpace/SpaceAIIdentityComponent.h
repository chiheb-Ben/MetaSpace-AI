// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "SpaceAIIdentityComponent.generated.h"

class UPrimitiveComponent;

/**
 * Fired in Blueprint when the player looks at this Actor (or a component inside it) and presses the [E] key!
 * Connect any custom function or logic to this event in your Actor Blueprint!
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnSpaceObjectInteractedWithE, AActor*, InteractingPlayer, UPrimitiveComponent*, HitComponent, FName, HitComponentName);

/**
 * Defines the semantic role of this Actor in the Space Station so Meta AI understands
 * whether it is the Player/Trainee, a floating prop, a mission tool, a snap slot, or station environment.
 */
UENUM(BlueprintType)
enum class ESpaceAIEntityRole : uint8
{
	/** Standard floating Zero-G room prop (Chair, Crate, Paper, Monitor, Debris, etc.) */
	SpaceProp           UMETA(DisplayName = "Space Object / Floating Prop"),

	/** Key interactive mission item that the Trainee must Grab, Carry, or Snap (Battery, Wrench, Data Drive, Oxygen Canister) */
	MissionToolOrItem   UMETA(DisplayName = "Mission Item / Grabbable Tool"),

	/** The Astronaut Trainee / Player Character */
	AstronautPlayer     UMETA(DisplayName = "Astronaut Trainee (Player Character)"),

	/** Target Docking / Snap Slot where a Mission Item belongs */
	SnapTargetSlot      UMETA(DisplayName = "Snap Point / Target Slot"),

	/** Spaceship Repair Zone / Damaged System Task that the Astronaut must fix */
	SpaceshipRepairZone UMETA(DisplayName = "Spaceship Repair Zone / Task"),

	/** Space Hazard / Floating Obstacle spawned or controlled by AI to challenge the player */
	SpaceHazardObstacle UMETA(DisplayName = "Space Obstacle / Hazard"),

	/** Station structure, terminal, hazard, or environment controller */
	StationEnvironment  UMETA(DisplayName = "Station Environment / System")
};

/**
 * Semantic Identity & Description Component for Objects, Characters, and Station Elements.
 * Attach this component to ANY Actor (Props, Grabbable Items, Player Character, Snap Slots)
 * to give it a Name, Role, Purpose Description, and AI Hints so Meta AI understands what each
 * object is for and can intelligently control its physics (mass, speed, spin, push, zero-G, etc.)!
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Space AI Identity & Description Component"))
class METASPACE_API USpaceAIIdentityComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	USpaceAIIdentityComponent();

protected:
	virtual void BeginPlay() override;

public:
	// =========================================================================
	// 1. Object / Character Identity & Semantic Description for Meta AI
	// =========================================================================

	/** Clear name of this Object or Character that Meta AI will read and use to target it (e.g., "Oxygen Tank", "Office Chair", "Astronaut Trainee", "Generator Slot") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|1. Name & Description")
	FString EntityName = TEXT("");

	/** Short unique ID tag for AI targeting (e.g., "chair_01", "battery_alpha", "player"). Auto-filled from EntityName if left None. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|1. Name & Description")
	FName EntityID = NAME_None;

	/** What kind of entity is this in the Space Station? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|1. Name & Description")
	ESpaceAIEntityRole EntityRole = ESpaceAIEntityRole::SpaceProp;

	/**
	 * Detailed description of what this object/character is, what it is used for in the training scenario,
	 * and its physical nature so Meta AI understands its purpose!
	 * Example: "An 8kg metal maintenance chair floating in the cabin. Can get in the trainee's way or be pushed aside."
	 * Example: "Emergency Fusion Cell needed to restore station power. Must be grabbed and snapped into the Reactor Slot."
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|1. Name & Description", meta = (MultiLine = "true"))
	FString DescriptionAndPurpose = TEXT("A zero-gravity object inside the space station.");

	/**
	 * Optional instructions or hints telling Meta AI how it can modify this object's physics during gameplay.
	 * Example: "Make it heavier (50kg) during hard training, or push it gently toward the player if they need help finding it."
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|1. Name & Description", meta = (MultiLine = "true"))
	FString AIPhysicsControlHint = TEXT("");

	// =========================================================================
	// 2. AI Physics Control Permissions
	// =========================================================================

	/** Include this Actor's name, description, and live physics state when sending world context to Meta AI */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|2. AI Permissions")
	bool bExposeToMetaAI = true;

	/** Allow Meta AI to dynamically modify this Actor's physics (Mass/Weight, Speed, Spin, Damping, Bounce, Zero-G, Push/Pull) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|2. AI Permissions")
	bool bAllowAIToModifyPhysics = true;

	/** Allow Meta AI to apply 3D physical pushes/impulses to move or attract/repel this object in space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|2. AI Permissions")
	bool bAllowAIToPushOrMoveObject = true;

	// =========================================================================
	// 3. Spaceship Zone & Repair Task Status (For AI Spaceship Repair Logic)
	// =========================================================================

	/** Enable if this Actor represents a Spaceship Zone, Damaged System, or Repair Task that the player must complete! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|3. Spaceship Zone & Repair Task")
	bool bIsRepairTaskOrZone = false;

	/** Name of the Spaceship Zone or Sector (e.g., "Engine Room", "Oxygen Generator", "Hull Breach Sector B", "Navigation Console") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|3. Spaceship Zone & Repair Task")
	FString SpaceshipZoneName = TEXT("Main Hull Sector");

	/** Is this repair task or zone currently COMPLETED (Fixed) or INCOMPLETE (Damaged/Broken)? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|3. Spaceship Zone & Repair Task")
	bool bIsRepairTaskCompleted = false;

	/** Current health/repair integrity percentage of this spaceship zone (0% = Damaged/Broken, 100% = Fully Repaired) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|3. Spaceship Zone & Repair Task", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float ZoneIntegrityPercent = 0.0f;

	/** Updates whether this repair task/zone is completed or incomplete (can be called from Blueprint when a player finishes a zone!) */
	UFUNCTION(BlueprintCallable, Category = "Space AI Identity|Spaceship Repair")
	void SetRepairTaskCompleted(bool bCompleted, float NewIntegrityPercent = 100.0f);

	// =========================================================================
	// 4. Interactive System (Press [E] Key to Execute Custom Logic & Notify AI)
	// =========================================================================

	/** Enable this if the player can look at this Actor (or its components like ValveWheel, Switch, Lever) and press [E] to execute a function! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|4. Interactive System (Press E)")
	bool bCanInteractWithEKey = false;

	/** Prompt message displayed when looking at this interactive object */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|4. Interactive System (Press E)", meta = (EditCondition = "bCanInteractWithEKey"))
	FString InteractionPromptText = TEXT("Press [E] to Interact");

	/** Description sent to Meta AI when the player presses [E] on this object so AI understands what just happened in the ship! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|4. Interactive System (Press E)", meta = (EditCondition = "bCanInteractWithEKey", MultiLine = "true"))
	FString InteractionDescriptionForAI = TEXT("Player interacted with this spaceship mechanism using the E key.");

	/** Automatically notify Meta AI and the MySQL Database when the player presses [E] on this object */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|4. Interactive System (Press E)", meta = (EditCondition = "bCanInteractWithEKey"))
	bool bAutoNotifyAIOnInteract = true;

	/** Automatically mark this Repair Task / Zone as Completed (100%) when the player presses [E] on it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Space AI Identity|4. Interactive System (Press E)", meta = (EditCondition = "bCanInteractWithEKey"))
	bool bMarkRepairCompletedOnInteract = false;

	/**
	 * Event fired in Blueprint whenever the player looks at this Actor (or any component inside it) and presses [E]!
	 * Click '+' in the Details panel to hook up your custom Blueprint logic (e.g., rotate valve wheel, toggle system, etc.).
	 */
	UPROPERTY(BlueprintAssignable, Category = "Space AI Identity|Events")
	FOnSpaceObjectInteractedWithE OnInteractedWithEKey;

	/** Executes the [E] key interaction on this Actor, firing OnInteractedWithEKey in Blueprint and notifying Meta AI! */
	UFUNCTION(BlueprintCallable, Category = "Space AI Identity|Interactive System")
	void TriggerPlayerInteractionWithE(AActor* InteractingPlayer, UPrimitiveComponent* HitComponent = nullptr);

	// =========================================================================
	// Blueprint Callable Helpers
	// =========================================================================

	/** Returns the effective display name for AI (falls back to Actor label/name if EntityName is empty) */
	UFUNCTION(BlueprintPure, Category = "Space AI Identity")
	FString GetEffectiveEntityName() const;

	/** Returns the effective ID string for AI targeting */
	UFUNCTION(BlueprintPure, Category = "Space AI Identity")
	FString GetEffectiveEntityID() const;

	/** Returns true if a target string from Meta AI (e.g., "chair_01", "Office Chair", "ALL", "PLAYER") matches this entity */
	UFUNCTION(BlueprintPure, Category = "Space AI Identity")
	bool MatchesAITargetName(const FString& TargetIdentifier) const;

	/** Returns a human- and AI-readable one-line summary of this Actor's identity and live physics telemetry */
	UFUNCTION(BlueprintPure, Category = "Space AI Identity")
	FString GetLiveTelemetrySummaryForAI() const;

	/** Builds a structured JSON object containing this Actor's identity, description, and current physics state */
	TSharedPtr<FJsonObject> BuildTelemetryJsonObject() const;
};
