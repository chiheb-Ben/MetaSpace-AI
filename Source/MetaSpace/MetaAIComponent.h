// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/IHttpRequest.h"
#include "MetaAIComponent.generated.h"

class USpaceAIIdentityComponent;
class ULightComponent;
class UPointLightComponent;
class UBoxComponent;
class UTextRenderComponent;
class UArrowComponent;
class UStaticMeshComponent;
class UPrimitiveComponent;
class UStaticMesh;
class USpaceGrabSnapComponent;
class USoundBase;

/**
 * Runtime state for each Spaceship Light (e.g., RectLight, RectLight2) controlled by Meta AI.
 */
struct FTrackedShipLightState
{
	TWeakObjectPtr<ULightComponent> LightComp = nullptr;
	FString LightName = TEXT("RectLight");
	float OriginalIntensity = 5000.0f;
	FLinearColor OriginalColor = FLinearColor::White;
	FLinearColor TargetColor = FLinearColor::White;
	float TargetIntensityMultiplier = 1.0f;
	FString ActiveMode = TEXT("solid"); // "solid", "pulse_alarm", "flicker", "strobe", "alternating", "off"
	float AnimationSpeed = 2.0f;
	int32 LightIndex = 0;
};

/**
 * Defines a Manual Designer Quest or a Saved Database Quest that merges seamlessly with the AI Game Master!
 */
USTRUCT(BlueprintType)
struct FSpaceQuestDefinition
{
	GENERATED_BODY()

	/** Unique Quest ID / Key (auto-generated from Target + Tool if left empty, e.g. "pipe__valvewheel") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	FString QuestKey = TEXT("");

	/** Title of the Quest shown on the 3D Box & Website (e.g., "Seal Main Coolant Pipe Valve") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	FString QuestTitle = TEXT("Attach Valve Wheel to Leaking Pipe");

	/** Clear instruction of what the player must do (e.g., "Grab the Valve Wheel, attach it to the Pipe, and press [E] to close!") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	FString QuestInstruction = TEXT("Bring the required part/tool to the target and complete the repair!");

	/** Optional: Pick the Target Actor directly from the Level (e.g., Pipe, Terminal_elctric) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	TObjectPtr<AActor> TargetActorInLevel = nullptr;

	/** Name or ID of the Target Actor (used if TargetActorInLevel is not set or when loaded from MySQL Database) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	FString TargetActorName = TEXT("");

	/** Optional: Pick the Required Tool / Item Actor directly from the Level (e.g., ValveWheel, Digi_Wrench) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	TObjectPtr<AActor> RequiredToolActorInLevel = nullptr;

	/** Name or ID of the Required Tool / Item (used if RequiredToolActorInLevel is not set or when loaded from MySQL Database) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	FString RequiredToolName = TEXT("");

	/** Should the AI Game Master also spawn the 3D Quest Box + Floating Arrow for this quest? (Disable if your custom Blueprint already handles the visuals) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest Definition")
	bool bShow3DQuestBoxAndArrow = true;

	/** Source of this quest ("MANUAL", "AI_GENERATED", or "DATABASE_SAVED") */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	FString QuestSource = TEXT("MANUAL");

	/** Fastest time (in seconds) this quest was completed (fetched from MySQL Database) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	float BestCompletionTimeSeconds = 0.0f;

	/** How many times this quest has been completed (fetched from MySQL Database) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	int32 TimesCompleted = 0;

	/** Precomputed Meta AI JSON response (physics commands, light commands) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	FString PrecomputedRawAIJson = TEXT("");

	/** Precomputed Flight Director radio advice */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	FString PrecomputedFlightDirectorAdvice = TEXT("");

	/** Precomputed Risk Level */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	FString PrecomputedRiskLevel = TEXT("CRITICAL");

	/** Precomputed Risk Score (0-100) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Quest Definition")
	float PrecomputedRiskScore = 75.0f;
};

/**
 * Current Phase of the Astronaut Pre-Flight Orientation & Competency Evaluation.
 * The AI Flight Director tests movement, wall-grab, and equipment retrieval before activating ship emergencies!
 */
UENUM(BlueprintType)
enum class EAstronautTrainingPhase : uint8
{
	NotStarted              UMETA(DisplayName = "Not Started"),
	Step1_ThrusterMovement  UMETA(DisplayName = "Step 1: Thrusters & 3D Flight"),
	Step2_WallGrabAndLaunch UMETA(DisplayName = "Step 2: Wall Tether & Aim Launch"),
	Step3_GrabEquipment     UMETA(DisplayName = "Step 3: Tool Acquisition & Grip"),
	Step4_CarryAndCalibrate UMETA(DisplayName = "Step 4: Tool Navigation & Terminal Sync"),
	CertifiedReady          UMETA(DisplayName = "Certified 100% Ready (Threats Approaching)"),
	ThreatOperationsActive  UMETA(DisplayName = "Live Threat Operations Active")
};

/**
 * Complete real-time Astronaut HUD and AI telemetry snapshot.
 * Allows binding all UMG progress bars, text labels, and tutorial hints with a single Blueprint node!
 */
USTRUCT(BlueprintType)
struct FAstronautHUDState
{
	GENERATED_BODY()

	// =========================================================================
	// 0. Pre-Emergency Orientation Protocol (Readiness Training)
	// =========================================================================

	/** True if the astronaut is currently undergoing pre-emergency orientation & readiness training */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Training")
	bool bIsInOrientationTraining = false;

	/** Current Training Phase */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Training")
	EAstronautTrainingPhase TrainingPhase = EAstronautTrainingPhase::NotStarted;

	/** Human-readable progress text for the training badge (e.g. "Orientation: Step 2 of 4") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Training")
	FText TrainingStepText;

	/** Normalized progress through orientation (0.0 to 1.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Training")
	float TrainingProgressPercent = 0.0f;

	// =========================================================================
	// 1. Oxygen Telemetry
	// =========================================================================

	/** Normalized Player Suit Oxygen (0.0 to 1.0) directly bindable to UMG ProgressBar 'SetPercent' */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	float PlayerOxygenPercent = 1.0f;

	/** Player Suit Current Oxygen (0 to 100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	float PlayerOxygenScore = 100.0f;

	/** Estimated seconds of suit oxygen remaining */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	float PlayerOxygenSecondsRemaining = 300.0f;

	/** Formatted Player Oxygen text (e.g., "85%") for UMG TextBlock */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	FText PlayerOxygenText;

	/** Dynamic Color Tint (Cyan -> Warning Orange -> Flashing Red) for UMG ProgressBar Tint */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	FLinearColor PlayerOxygenColor = FLinearColor(0.2f, 0.85f, 1.0f, 1.0f);

	/** Normalized Spaceship Room/Cabin Atmospheric Oxygen (0.0 to 1.0) directly bindable to UMG ProgressBar */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	float RoomOxygenPercent = 1.0f;

	/** Spaceship Room Atmospheric Oxygen (0 to 100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	float RoomOxygenScore = 100.0f;

	/** Formatted Room Oxygen text (e.g., "94%") for UMG TextBlock */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	FText RoomOxygenText;

	/** Dynamic Room Oxygen Color Tint for UMG ProgressBar Tint */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	FLinearColor RoomOxygenColor = FLinearColor(0.25f, 1.0f, 0.45f, 1.0f);

	/** True if player oxygen is under 25% */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	bool bIsPlayerOxygenLow = false;

	/** True if room atmosphere is actively leaking due to hull emergency */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Oxygen")
	bool bIsRoomOxygenDepleting = false;

	// =========================================================================
	// 2. Autonomous Danger & Risk Progress Bar
	// =========================================================================

	/** Normalized Autonomous AI Danger/Risk (0.0 to 1.0) directly bindable to UMG ProgressBar 'SetPercent' */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Risk")
	float AIRiskPercent = 0.35f;

	/** Current Risk Score (0.0 to 100.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Risk")
	float AIRiskScore = 35.0f;

	/** Risk Level String: "LOW", "MODERATE", "HIGH", "CRITICAL", or "OPTIMAL" */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Risk")
	FString AIRiskLevel = TEXT("MODERATE");

	/** Formatted Risk text (e.g., "AI RISK: HIGH (78%)") for UMG TextBlock */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Risk")
	FText AIRiskText;

	/** Dynamic Risk Color (Green -> Orange -> Critical Red) for UMG ProgressBar Tint */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Risk")
	FLinearColor AIRiskColor = FLinearColor(0.95f, 0.6f, 0.1f, 1.0f);

	// =========================================================================
	// 3. Active Real-Time Quest (What To Do)
	// =========================================================================

	/** True if there is an active quest */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Quest")
	bool bHasActiveQuest = false;

	/** Current Quest Title (e.g., "Calibrate Digi_Wrench at Terminal_elctric") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Quest")
	FText QuestTitleText;

	/** Clear instruction of what to do (e.g., "Take Digi_Wrench (Cyan Box) and go to Terminal_elctric Box & Press [E]!") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Quest")
	FText QuestInstructionText;

	/** Name of the broken target system (e.g., "Terminal_elctric") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Quest")
	FText QuestTargetNameText;

	/** Name of the required tool (e.g., "Digi_Wrench") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Quest")
	FText QuestRequiredToolText;

	// =========================================================================
	// 4. Problem-Solving Mastery Progress
	// =========================================================================

	/** Normalized Mastery Score (0.0 to 1.0) for UMG ProgressBar 'SetPercent' */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Mastery")
	float ProblemSolvingMasteryPercent = 0.0f;

	/** Current Mastery Score (0 to 100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Mastery")
	float ProblemSolvingMasteryScore = 0.0f;

	/** Formatted Mastery Text (e.g., "Mastery: 72 / 100") for UMG TextBlock */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Mastery")
	FText ProblemSolvingMasteryText;

	/** True if 90/100 mastery has been reached and random breakdowns have stopped */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Mastery")
	bool bMasteryThresholdReached = false;

	// =========================================================================
	// 5. In-Game Advice & Controls Tutorial (Teaching the Player)
	// =========================================================================

	/** Real-time dynamic contextual hint teaching the exact mouse/keyboard keys needed right now! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Advice & Controls")
	FText ContextualControlHintText;

	/** Authentic radio transmission from the Space Station AI Flight Director */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Astronaut HUD|Advice & Controls")
	FText FlightDirectorAdviceText;
};

/**
 * Delegate fired when Meta AI responds (or when an error/fallback occurs).
 * @param bSuccess True if the HTTP request succeeded with a 200 OK response from Meta API.
 * @param AIResponseText The extracted message from the AI model (or error/fallback message).
 * @param RawJsonResponse The full raw JSON string returned by the server.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnMetaAIResponseReceived, bool, bSuccess, const FString&, AIResponseText, const FString&, RawJsonResponse);

/**
 * Delegate fired whenever Meta AI executes a physics modification command on an Actor in the world.
 * @param TargetActor The Actor whose physics were modified by Meta AI.
 * @param EntityName The SpaceAIIdentity name of the Actor.
 * @param CommandSummary Human-readable summary of the physics changes applied (e.g., "Mass->2.5kg, Spin->40deg/s").
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnMetaAIPhysicsCommandExecuted, AActor*, TargetActor, const FString&, EntityName, const FString&, CommandSummary);

/**
 * Delegate fired whenever Meta AI triggers a Spaceship Event (Hull Shake, Camera Shake, Obstacle Spawned, Hazard Launched, or Zone Repaired).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnMetaAIShipEventTriggered, const FString&, EventType, const FString&, EventDetails, float, IntensityOrCount);

/**
 * Component that connects Unreal Engine C++ to Meta's Model API (https://api.meta.ai/v1/chat/completions)
 * AND gives Meta AI full real-time control over the Zero-G Physics of all Objects & Characters
 * described by USpaceAIIdentityComponent!
 * Includes strict multi-layer Credit & Rate-Limit Protection so accidental loops or ticks NEVER drain API credits!
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Meta AI Component (Physics Director)"))
class METASPACE_API UMetaAIComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	UMetaAIComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	// =========================================================================
	// 1. Settings - Meta API Configuration
	// =========================================================================

	/** Meta API Endpoint URL (OpenAI-compatible chat completions endpoint) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|1. API Configuration")
	FString ApiEndpointUrl = TEXT("https://api.meta.ai/v1/chat/completions");

	/** Meta API Key (Bearer Token starting with LLM_...) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|1. API Configuration")
	FString ApiKey = TEXT("YOUR_META_MODEL_API_KEY_HERE");

	/** Meta Model ID (e.g., muse-spark-1.3, muse-spark-1.2, muse-wonder-1.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|1. API Configuration")
	FString ModelName = TEXT("muse-spark-1.3");

	/** Base System Role instructing Meta AI how to act as the Space Station AI & Physics Director */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|1. API Configuration", meta = (MultiLine = "true"))
	FString SystemPrompt = TEXT("You are the onboard Space Station AI and Physics Director for 'MetaSpace', a realistic zero-gravity astronaut training simulator. You monitor every object and trainee in the station via their Name, Role, and Description, and you dynamically control their physical properties (mass in kg, speed, rotation spin, damping, bounce, zero-G state, and 3D forces) to create an immersive, intelligent space simulation.");

	/** Sampling temperature for the AI response */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|1. API Configuration", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Temperature = 0.5f;

	// =========================================================================
	// 2. Settings - AI Full Physics Control Engine
	// =========================================================================

	/** Allow Meta AI to read all USpaceAIIdentityComponent objects/characters in the level and modify their physics in real time! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|2. Physics Control Engine")
	bool bEnableAIPhysicsControl = true;

	/** Automatically attach the live list of all Objects & Characters (Name, Description, Mass, Speed, Spin) to every event/prompt sent to Meta AI */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|2. Physics Control Engine", meta = (EditCondition = "bEnableAIPhysicsControl"))
	bool bAutoIncludeWorldObjectsContext = true;

	/** Also discover Actors with USpaceObjectComponent or USpaceFloatingComponent even if you forgot to add USpaceAIIdentityComponent to them */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|2. Physics Control Engine", meta = (EditCondition = "bEnableAIPhysicsControl"))
	bool bAutoDiscoverUnlabeledPhysicsActors = true;

	/** Default scenario directive sent to Meta AI when the game starts (if bSendHelloWorldOnBeginPlay is true) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|2. Physics Control Engine", meta = (MultiLine = "true"))
	FString InitialAIPhysicsDirective = TEXT("player_started_game: Analyze all objects and the trainee in the station based on their Name and Description, and adjust their zero-G physics (mass_kg, spin_speed, max_speed, or gentle push) to match their real-world purpose.");

	// =========================================================================
	// 3. Settings - Credit & Anti-Loop Protection (Safety Guardrails)
	// =========================================================================

	/** Master Kill-Switch: If false, blocks all outgoing HTTP calls immediately */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection")
	bool bEnableApiCalls = true;

	/** Maximum total API requests allowed per Play session (Hard cap so a bug/loop can NEVER drain your credits!) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection", meta = (ClampMin = "1", ClampMax = "500"))
	int32 MaxRequestsPerSession = 15;

	/** Minimum cooldown time (in seconds) between any two API requests (Prevents Tick/Loop spam!) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection", meta = (ClampMin = "0.5", ClampMax = "300.0"))
	float MinCooldownBetweenRequests = 5.0f;

	/** Maximum tokens the AI is allowed to generate per reply (3500 tokens covers muse-spark-1.3 reasoning_tokens + full JSON output!) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection", meta = (ClampMin = "500", ClampMax = "8192"))
	int32 MaxCompletionTokens = 3500;

	/** Block sending a new request while another request is still waiting for a response */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection")
	bool bBlockConcurrentRequests = true;

	/** Prevent sending the exact same prompt/event twice in a row within DuplicateBlockWindowSeconds */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection")
	bool bBlockDuplicatePrompts = true;

	/** Time window (in seconds) during which an identical prompt will be ignored or served from cache */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection", meta = (ClampMin = "1.0"))
	float DuplicateBlockWindowSeconds = 30.0f;

	/** If an identical prompt was already answered during this session, return the cached reply for free (0 credits used!) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|3. Credit Protection")
	bool bUseSessionResponseCache = true;

	// =========================================================================
	// 4. Settings - Startup Test & Debugging
	// =========================================================================

	/** Automatically send InitialAIPhysicsDirective once on BeginPlay (after a short 0.2s delay so all actors initialize first) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug")
	bool bSendHelloWorldOnBeginPlay = true;

	/** Also check /v1/models on BeginPlay (Default FALSE to save requests; enable only if debugging auth) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug")
	bool bCheckModelsListOnBeginPlay = false;

	/** Default event name sent on BeginPlay if bEnableAIPhysicsControl is disabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug")
	FString InitialTestEventName = TEXT("player_started_game");

	/** Print request status, AI message, and executed physics changes directly on the Viewport */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug")
	bool bPrintToScreen = true;

	/** Print a detailed report on screen listing every single Object, Tool, and Character that Meta AI discovered in the world */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug")
	bool bPrintDiscoveredObjectsReport = true;

	/** Duration (in seconds) that the AI message stays on screen */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug", meta = (ClampMin = "1.0"))
	float ScreenMessageDuration = 15.0f;

	/** If the API returns a 402 Billing Error or network timeout during testing, generate & execute a smart physics response based on your scene's USpaceAIIdentityComponent descriptions! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|4. Startup & Debug")
	bool bUseSimulatedFallbackOnBillingError = false;

	// =========================================================================
	// 5. Settings - PHP & MySQL Training Database (Telemetry, Timer & AI Coach)
	// =========================================================================

	/** Enable syncing training sessions, mission timer, telemetry events, and AI coaching advice with your PHP/MySQL server (api.php) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|5. PHP & MySQL Training Database")
	bool bEnablePhpDatabaseSync = true;

	/** Full URL to your uploaded api.php endpoint on your server (e.g., https://naenco.tech/system/api.php) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|5. PHP & MySQL Training Database", meta = (EditCondition = "bEnablePhpDatabaseSync"))
	FString PhpApiUrl = TEXT("https://naenco.tech/system/api.php");

	/** Name or Callsign of the Astronaut Trainee saved in the MySQL database */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|5. PHP & MySQL Training Database")
	FString TraineeName = TEXT("Astronaut_Trainee");

	/** Automatically bind to USpaceGrabSnapComponent & USpaceFloatingComponent events to track grabs, snaps, wall launches, and mission completion */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|5. PHP & MySQL Training Database")
	bool bAutoTrackPlayerGrabSnapAndLaunchEvents = true;

	/** Automatically ask Meta AI to evaluate the trainee's elapsed time & give coaching advice when an item is snapped or when the mission finishes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|5. PHP & MySQL Training Database")
	bool bAutoAskAICoachOnSnapAndFinish = true;

	// =========================================================================
	// 6. Settings - AI Obstacle Spawner, Level Meshes, Risk Level & Ship Shake
	// =========================================================================

	/** Allow Meta AI to dynamically spawn Zero-G obstacles/debris/hazards in front of or around the player during the repair mission! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	bool bAllowAIToSpawnObstacles = true;

	/**
	 * Pick existing Actors / StaticMeshActors ALREADY placed in the Level so Meta AI can directly control their Zero-G physics,
	 * hurl them as obstacles, or clone their meshes!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	TArray<TObjectPtr<AActor>> LevelObstacleActors;

	/**
	 * Pick any 3D StaticMesh assets (e.g., Pipe, Crate, Debris, Wrench, Cylinder) so Meta AI can spawn Zero-G physical obstacles using these exact meshes!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	TArray<TObjectPtr<UStaticMesh>> SpawnableObstacleMeshes;

	/**
	 * Actor Tags or Actor Names of existing meshes in the Level that Meta AI should automatically take control of as Zero-G obstacles (default tag: "AIObstacle").
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	TArray<FName> LevelObstacleTagsOrNames = { FName("AIObstacle") };

	/**
	 * Optional Blueprint Actor classes that Meta AI can spawn as obstacles or repair items.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair", meta = (EditCondition = "bAllowAIToSpawnObstacles"))
	TArray<TSubclassOf<AActor>> SpawnableObstacleClasses;

	/** Allow Meta AI to autonomously determine the Spaceship Risk Level ("LOW", "MODERATE", "HIGH", "CRITICAL", 0-100%) and automatically adjust lights, shake, and obstacles! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	bool bLetAIDetermineRiskLevelAutomatically = true;

	/** Current Spaceship Risk Level determined by Meta AI ("LOW", "MODERATE", "HIGH", "CRITICAL") */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	FString CurrentAIRiskLevel = TEXT("MODERATE");

	/** Current Spaceship Risk Score (0% = Safe, 100% = Extreme Critical Emergency) determined by Meta AI */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	float CurrentAIRiskScorePercent = 35.0f;

	/** Current Dynamic Realistic Mission / Objective generated by Meta AI */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	FString MissionName = TEXT("Awaiting AI Game Master Quest...");

	/** Current Astronaut Trainee Grade determined autonomously by Meta AI ("S", "A", "B", "C", "D") */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|7. Trainee Performance & Autonomous Study")
	FString CurrentAIGrade = TEXT("PENDING");

	/** Current Astronaut Competency Score determined autonomously by Meta AI (0.0 to 100.0) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|7. Trainee Performance & Autonomous Study")
	float CurrentAIFinalScore = 0.0f;

	/** Trainee Flight Readiness status determined autonomously by Meta AI */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|7. Trainee Performance & Autonomous Study")
	FString CurrentAIFlightReadiness = TEXT("IN_EVALUATION");

	/** Detailed scientific evaluation study written autonomously by Meta AI analyzing zero-G physics mastery */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|7. Trainee Performance & Autonomous Study")
	FString CurrentAIEvaluationSummary = TEXT("Awaiting Meta AI flight director performance debrief...");

	/** Maximum number of obstacles Meta AI can spawn per session (prevents clutter/lag) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair", meta = (ClampMin = "1", ClampMax = "50"))
	int32 MaxAISpawnedObstaclesPerSession = 15;

	/** Distance (in cm) ahead/around the player where AI-spawned obstacles appear before flying toward the player */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair", meta = (ClampMin = "120.0", ClampMax = "2000.0"))
	float ObstacleSpawnDistanceFromPlayer = 360.0f;

	/** Default speed (cm/s) at which AI-spawned or AI-hurled obstacles fly toward the player */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair", meta = (ClampMin = "50.0", ClampMax = "2000.0"))
	float ObstacleLaunchSpeedTowardPlayer = 320.0f;

	/** Allow Meta AI to trigger Spaceship Turbulence, Player Camera Shake, and hurl floating objects toward the player! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair")
	bool bAllowAIToTriggerShipCameraShake = true;

	/** Default intensity (0.1 to 2.0) of Spaceship Hull & Player Camera Shake when triggered by AI */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair", meta = (ClampMin = "0.1", ClampMax = "2.5"))
	float DefaultShipShakeIntensity = 0.8f;

	/** Default duration (in seconds) of Spaceship Hull & Player Camera Shake when triggered by AI */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|6. AI Spawner, Ship Shake & Repair", meta = (ClampMin = "0.2", ClampMax = "15.0"))
	float DefaultShipShakeDurationSeconds = 3.0f;

	/** Allow Meta AI to dynamically control Spaceship Lights (RectLight, RectLight2, etc.) — changing color (e.g., Red for Critical), turning off/on, flickering, or pulsing! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|7. AI Lighting Control (RectLight & RectLight2)")
	bool bAllowAIToControlShipLights = true;

	// =========================================================================
	// 8. Settings - AI Game Master (Endless Real-Time Quests + 3D Box & Arrow)
	// =========================================================================

	/** Enable the Endless Real-Time AI Game Master: every time something breaks or a quest is finished, AI creates a new live quest in the ship! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bEnableEndlessAIGameMaster = true;

	/** Automatically spawn a glowing 3D Quest Box + Floating Bouncing Arrow + 3D Objective Text at the broken object/zone so the player sees where to go and what to do! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bSpawn3DQuestBoxAndArrow = true;

	/** Automatically scan the entire Level for ALL Tools (e.g. Digi_Wrench, ValveWheel, Battery) and ALL Terminals/Systems (e.g. Terminal_elctric, Pipe, Console) so the AI Game Master uses everything in the level! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bAutoDiscoverAllLevelToolsAndTerminals = true;

	/** When a quest requires a Tool (e.g. 'Take Digi_Wrench to Terminal_elctric'), require the player to bring/hold that Tool at the 3D Quest Box to complete the repair! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bRequireToolForToolQuests = true;

	/**
	 * Merge your custom Hand-Crafted / Manual Quests (ManualDesignerQuests) with AI-Generated Quests!
	 * The AI Game Master will include your manual quests in the rotation and never repeat a quest the player already completed quickly!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bMergeManualAndAIQuests = true;

	/**
	 * Save every AI-generated and Manual quest into the MySQL 'quest_catalog' database table, fetch existing quests directly from the database if available,
	 * and prevent repeating quests that the player already finished quickly!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bSaveAndFetchQuestsFromDatabase = true;

	/**
	 * If a player completes a quest in less than or equal to this time (in seconds), the quest is marked as 'Mastered Quickly' in the database
	 * so the AI Game Master skips repeating it and generates/fetches a new quest instead!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)", meta = (ClampMin = "5.0", ClampMax = "300.0"))
	float FastQuestMasteryThresholdSeconds = 60.0f;

	/**
	 * Add your own Manual Quests here (e.g., Pipe + Valve Wheel, Oxygen + Burning Pipe, etc.)!
	 * They will be merged with the AI Game Master quests, synced to the MySQL Quest Database, and tracked for completion time!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)", meta = (EditCondition = "bMergeManualAndAIQuests"))
	TArray<FSpaceQuestDefinition> ManualDesignerQuests;

	/** Source of the currently active quest ("MANUAL", "DATABASE_SAVED", or "AI_GENERATED") */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	FString CurrentQuestSource = TEXT("AI_GENERATED");

	/**
	 * Optional: Pick specific Tool / Item Actors in the Level (e.g., Digi_Wrench, ValveWheel, Battery, Extinguisher) for the AI Game Master to use in quests!
	 * (If left empty, automatically discovers all tools/items in the level!)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	TArray<TObjectPtr<AActor>> QuestToolActorsInLevel;

	/**
	 * Optional: Pick specific Terminal / Station Actors in the Level (e.g., Terminal_elctric, Pipe, Reactor, Console) that can break down and receive the 3D Quest Box + Arrow!
	 * (If left empty, the AI Game Master automatically discovers and picks from real Terminals/Objects in your level!)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	TArray<TObjectPtr<AActor>> QuestTargetActorsInLevel;

	/** Extent (half-size in cm) of the 3D Quest Marker Box spawned around the target broken object */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	FVector QuestMarkerBoxExtent = FVector(95.0f, 95.0f, 95.0f);

	/** Delay (in seconds) after the player completes an AI Quest before the AI Game Master breaks a new system and spawns the next quest */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)", meta = (ClampMin = "1.0", ClampMax = "60.0"))
	float NextQuestDelayAfterCompletionSeconds = 4.0f;

	/** If the player hasn't triggered a breakdown recently, how often (in seconds) the AI Game Master triggers a new ship breakdown & quest */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)", meta = (ClampMin = "15.0", ClampMax = "300.0"))
	float EndlessBreakdownIntervalSeconds = 55.0f;

	/** Current active Quest Title created by the AI Game Master */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	FString CurrentQuestTitle = TEXT("None");

	/** Current active Quest Instruction (what the player must do at the 3D Quest Box) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	FString CurrentQuestWhatToDo = TEXT("");

	/** Name of the broken Actor/Terminal where the 3D Quest Box + Arrow is currently placed (e.g., "Terminal_elctric") */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	FString CurrentQuestTargetName = TEXT("");

	/** Name of the Tool required for the current quest (e.g., "Digi_Wrench"), if any */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	FString CurrentQuestRequiredToolName = TEXT("");

	/** Total number of real-time AI Game Master quests completed during this session */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	int32 CompletedAIQuestsCount = 0;

	/**
	 * Current Problem-Solving Proficiency Score (0 to 100).
	 * Increases as the player repairs systems and disposes of hazards.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	float ProblemSolvingProficiencyScore = 0.0f;

	/**
	 * Target Problem-Solving Mastery Threshold (defaults to 90.0 / 100).
	 * Upon reaching this score, the AI Game Master permanently stops generating random breakdowns!
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)", meta = (ClampMin = "50.0", ClampMax = "100.0"))
	float ProblemSolvingMasteryThreshold = 90.0f;

	/** Base percentage increment awarded per completed quest (defaults to 7.0%, allowing ~10 missions to reach 90% mastery) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)", meta = (ClampMin = "2.0", ClampMax = "25.0"))
	float BaseQuestMasteryIncrement = 7.0f;

	/** Set to true when ProblemSolvingProficiencyScore reaches 90/100 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|8. AI Game Master (Endless Quests & 3D Box/Arrow)")
	bool bMasteryThresholdReached = false;

	/** Returns current problem-solving proficiency (0 - 100) */
	UFUNCTION(BlueprintPure, Category = "Meta AI|AI Game Master")
	float GetProblemSolvingProficiencyScore() const { return ProblemSolvingProficiencyScore; }

	/** Returns whether the astronaut has stabilized the ship at 90/100 mastery */
	UFUNCTION(BlueprintPure, Category = "Meta AI|AI Game Master")
	bool HasReachedMasteryThreshold() const { return bMasteryThresholdReached; }

	// =========================================================================
	// 9. Settings - Spaceship Room Bounds (locaition_siez TriggerBox)
	// =========================================================================

	/**
	 * Strictly enforce that all Tools, level objects, and AI-spawned obstacles stay INSIDE the TriggerBox named 'locaition_siez'!
	 * If any object reaches or crosses the room boundary, it is automatically kept inside and bounced back toward the center.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|9. Spaceship Room Bounds (locaition_siez)")
	bool bEnforceSpaceshipRoomBounds = true;

	/**
	 * Optional direct reference to the TriggerBox Actor in the Level representing the spaceship room boundary.
	 * (If left unassigned, automatically finds the TriggerBox named 'locaition_siez' in the Level!)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|9. Spaceship Room Bounds (locaition_siez)")
	TObjectPtr<AActor> ShipRoomBoundsTriggerBox = nullptr;

	/**
	 * Name of the TriggerBox in the level defining the room boundary (defaults to 'locaition_siez' created by user).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|9. Spaceship Room Bounds (locaition_siez)")
	FString ShipRoomBoundsActorName = TEXT("locaition_siez");

	/**
	 * Safety padding (in cm) inside the TriggerBox walls so tools and obstacles never touch or clip through the walls.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|9. Spaceship Room Bounds (locaition_siez)", meta = (ClampMin = "5.0", ClampMax = "200.0"))
	float RoomBoundaryInnerPaddingCm = 35.0f;

	/**
	 * Draw glowing boundary wireframes and corner markers for the 3D room bounds in Viewport
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|9. Spaceship Room Bounds (locaition_siez)")
	bool bDrawRoomBoundaryDebug = false;

	/** The cached 3D World-Space Bounding Box of 'locaition_siez' */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|9. Spaceship Room Bounds (locaition_siez)")
	FBox CachedSpaceshipRoomBoundsBox = FBox(ForceInit);

	/** Returns true if a valid room boundary TriggerBox ('locaition_siez') is active */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Spaceship Room Bounds")
	bool HasValidSpaceshipRoomBounds() const;

	/** Clamps any World Location so it stays strictly inside the 'locaition_siez' TriggerBox volume */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Spaceship Room Bounds")
	FVector ClampLocationToRoomBounds(const FVector& InLocation, float PaddingCm = 35.0f) const;

	/** Checks whether a World Location is inside the 'locaition_siez' TriggerBox volume */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Spaceship Room Bounds")
	bool IsLocationInsideRoomBounds(const FVector& InLocation, float PaddingCm = 0.0f) const;

	// =========================================================================
	// Blueprint Events & Callable Functions
	// =========================================================================

	/** Fired in Blueprint whenever Meta AI replies to a request */
	UPROPERTY(BlueprintAssignable, Category = "Meta AI|Events")
	FOnMetaAIResponseReceived OnAIResponseReceived;

	/** Fired in Blueprint whenever Meta AI modifies the physics of an Object or the Player Character */
	UPROPERTY(BlueprintAssignable, Category = "Meta AI|Events")
	FOnMetaAIPhysicsCommandExecuted OnAIPhysicsCommandExecuted;

	/** Fired in Blueprint whenever Meta AI triggers a Spaceship Shake, Obstacle Spawn, Hazard Launch, or Zone Repair update */
	UPROPERTY(BlueprintAssignable, Category = "Meta AI|Events")
	FOnMetaAIShipEventTriggered OnAIShipEventTriggered;

	/**
	 * Spawns a Zero-G Space Obstacle / Debris Actor near the player (using SpawnableObstacleClasses or cloning an existing Space Object)
	 * and optionally launches it directly toward the player!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	AActor* SpawnSpaceObstacleForPlayer(const FString& ObstacleNameOrType = TEXT("SpaceDebris"), bool bLaunchTowardPlayer = true, float LaunchSpeed = 320.0f, float CustomMassKg = 12.0f, float CustomScale = 1.0f);

	/** Maximum allowed concurrent floating debris/obstacles in the spaceship to protect frame rate */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|AI Spawner, Ship Shake & Repair", meta = (ClampMin = "1", ClampMax = "12"))
	int32 MaxConcurrentSpawnedObstacles = 4;

	/** Whether to automatically destroy debris/scrap when a Dispose/Clear quest completes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	bool bAutoDestroyDebrisOnDisposalQuest = true;

	/**
	 * Safely destroys and removes an obstacle, loose scrap, or disposed debris actor from the level,
	 * unregistering physics and cleaning up references to maintain maximum frame rate.
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	void DestroyOrDisposeDebrisActor(AActor* DebrisActor);

	/**
	 * Destroys all spawned obstacles and floating debris actors in the ship to return the environment to pristine condition.
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	void CleanupAllExcessDebris();

	/**
	 * Triggers a realistic Spaceship Hull Vibration + First-Person Player Camera Shake,
	 * optionally jolting all floating objects in the ship and hurling nearby obstacles toward the player!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	void TriggerShipTurbulenceAndCameraShake(float Intensity = 0.8f, float DurationSeconds = 3.0f, bool bAlsoShakeFloatingObjects = true, int32 ObjectsToHurlAtPlayerCount = 1);

	/**
	 * Directs up to MaxObjectsCount floating objects in the spaceship to fly toward the player's current position!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	int32 LaunchNearbyObjectsTowardPlayer(int32 MaxObjectsCount = 2, float LaunchSpeed = 300.0f);

	/**
	 * Call this from Blueprint whenever a Spaceship Repair Zone or Task is completed (or damaged/incomplete)
	 * so Meta AI and the MySQL Database immediately know the ship's repair state and react to it!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	void NotifySpaceshipZoneOrTaskUpdated(const FString& ZoneOrTaskName, bool bIsCompleted, float IntegrityPercent = 100.0f, bool bRequestAIReaction = true);

	/**
	 * Scans all Spaceship Repair Zones / Tasks in the level and returns the overall Spaceship Repair Progress (0% to 100%).
	 */
	UFUNCTION(BlueprintPure, Category = "Meta AI|AI Spawner, Ship Shake & Repair")
	float GetSpaceshipRepairProgressPercent(int32& OutCompletedTasks, int32& OutTotalTasks) const;

	/**
	 * Controls Spaceship Lights ("RectLight", "RectLight2", or "ALL_LIGHTS"):
	 * Sets their Color, Brightness Multiplier, and Animation Mode ("solid", "pulse_alarm", "flicker", "strobe", "alternating", "off")!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Lighting Control (RectLight & RectLight2)")
	int32 ControlShipLights(const FString& TargetLightName, FLinearColor NewColor, float IntensityMultiplier = 1.0f, const FString& LightMode = TEXT("solid"), float Speed = 2.0f);

	/**
	 * Applies a ready-made Spaceship Lighting Mood Preset ("critical_red", "flicker_damage", "warning_orange", "alternating_alarm", "blackout", "repaired_green", "normal_white")
	 * to "RectLight", "RectLight2", or "ALL_LIGHTS"!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Lighting Control (RectLight & RectLight2)")
	int32 ApplyShipLightingPreset(const FString& PresetName, const FString& TargetLightName = TEXT("ALL_LIGHTS"));

	/**
	 * GLOBAL BLUEPRINT NODE (Callable from ANY Actor Blueprint, e.g., your Pipe + Wheel Overlap or Oxygen + Burning Pipe Overlap!):
	 * Reports any custom gameplay event to Meta AI & MySQL so Meta AI immediately decides the consequence
	 * (e.g., if Wheel placed on Pipe -> Green/Normal Lights & Task Complete; if Oxygen touches Burning Pipe -> Explosion Shake, Critical Red Flashing Lights & Flying Debris!).
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Spaceship Custom Logic & Events", meta = (WorldContext = "WorldContextObject", DisplayName = "MetaAI: Report Custom Ship Action / Overlap (AI Decides Consequence)"))
	static void ReportShipScenarioEventToAI(const UObject* WorldContextObject, const FString& SourceItemName, const FString& TargetObjectOrZoneName, const FString& WhatHappenedDescription, bool bIsTaskCompleted = false);

	/** Member version of ReportShipScenarioEventToAI */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Spaceship Custom Logic & Events")
	void HandleCustomShipScenarioEvent(const FString& SourceItemName, const FString& TargetObjectOrZoneName, const FString& WhatHappenedDescription, bool bIsTaskCompleted = false);

	/**
	 * Spawns or moves the 3D AI Quest Marker (Glowing Trigger Box + Floating Bouncing Arrow + 3D Objective Text + Beacon Light)
	 * to TargetActor (or WorldLocation) and displays what the player needs to do there!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	AActor* SpawnOrUpdate3DQuestMarker(AActor* TargetActor, FVector WorldLocation, const FString& QuestTitle, const FString& WhatToDoInstruction, const FString& RequiredItemName = TEXT(""));

	/**
	 * Triggers a new real-time Spaceship Breakdown on an Actor in the level, spawns the 3D Quest Box + Arrow on it,
	 * increases Risk Level, and asks the AI Game Master to generate a real-time quest!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	void TriggerNewShipBreakdownAndAIQuest(const FString& SpecificSystemOrReason = TEXT(""));

	/**
	 * Marks the current active AI Game Master Quest as Completed, flashes the 3D Quest Box Green,
	 * lowers ship Risk Level, logs to the PHP/MySQL website, and schedules the next real-time breakdown quest!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	void CompleteCurrentAIQuest(const FString& CompletionDetails = TEXT("Astronaut repaired broken system"));

	/**
	 * Immediately deletes/cancels the current active AI Quest and hides/destroys the 3D Quest Marker Box & Arrow!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	void DeleteCurrentAIQuest();

	/**
	 * Registers and optionally activates a Manual Designer Quest from Blueprint (merging it with the AI Game Master & saving it to the MySQL Quest Database)!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	void AddAndActivateManualQuest(const FString& QuestTitle, const FString& QuestInstruction, AActor* TargetActor, AActor* RequiredToolActor = nullptr, bool bShow3DBoxAndArrow = true);

	/**
	 * Starts a new Training Mission timer and registers the session in the PHP/MySQL Database (action="start_session").
	 * Called automatically on BeginPlay if bEnablePhpDatabaseSync is true.
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|PHP Database & Mission Timer")
	void StartTrainingMissionSession();

	/**
	 * Logs a training telemetry event (e.g., "item_grabbed", "item_snapped", "wall_launch", "checkpoint")
	 * with the exact elapsed mission time to the PHP/MySQL Database (action="log_event").
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|PHP Database & Mission Timer")
	void LogTrainingEventToDatabase(const FString& EventType, const FString& ActorName = TEXT(""), const FString& TargetName = TEXT(""));

	/**
	 * Asks Meta AI to inspect the trainee's current elapsed time, progress, and MySQL history,
	 * provide coaching advice, and optionally adjust station physics to help or challenge the trainee!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|PHP Database & Mission Timer")
	void RequestAICoachingAndTimeEvaluation(const FString& CheckpointOrEventReason);

	/**
	 * Completes (or fails) the current training mission, records the final duration_seconds in MySQL (action="finish_session"),
	 * and asks Meta AI for a final mission debrief & time evaluation!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|PHP Database & Mission Timer")
	void CompleteTrainingMission(bool bMissionSuccess = true);

	/** Returns how many seconds have elapsed since the training mission started */
	UFUNCTION(BlueprintPure, Category = "Meta AI|PHP Database & Mission Timer")
	float GetMissionElapsedTimeSeconds() const;

	/** Returns the current Session UUID synced with MySQL */
	UFUNCTION(BlueprintPure, Category = "Meta AI|PHP Database & Mission Timer")
	FString GetCurrentSessionUUID() const { return CurrentSessionUUID; }

	/** Returns how many items the trainee has snapped into place during this session */
	UFUNCTION(BlueprintPure, Category = "Meta AI|PHP Database & Mission Timer")
	int32 GetItemsSnappedCount() const { return SessionItemsSnappedCount; }

	/** Returns how many items the trainee has grabbed during this session */
	UFUNCTION(BlueprintPure, Category = "Meta AI|PHP Database & Mission Timer")
	int32 GetItemsGrabbedCount() const { return SessionItemsGrabbedCount; }

	/** Returns how many wall launches/pulls the trainee has performed during this session */
	UFUNCTION(BlueprintPure, Category = "Meta AI|PHP Database & Mission Timer")
	int32 GetWallLaunchesCount() const { return SessionWallLaunchesCount; }

	/**
	 * Sends a game event or scenario instruction to Meta AI along with the live snapshot of all
	 * Objects & Characters (their Name, Description, Mass, Speed, Spin) so Meta AI can adjust their physics!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Physics Director")
	void SendWorldDirectiveToAI(const FString& DirectiveOrEventText);

	/**
	 * Sends a simple JSON game event to Meta AI, e.g. {"event": "player_started_game"}
	 * (Automatically attaches world objects context if bAutoIncludeWorldObjectsContext is enabled).
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI")
	void SendGameEvent(const FString& EventName);

	/**
	 * Sends any custom prompt or custom JSON string to Meta AI via HTTP POST.
	 * Protected by Cooldown, Session Quota, In-Flight Lock, and Response Cache.
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI")
	void SendCustomPrompt(const FString& UserMessageJsonOrText);

	/**
	 * Builds and returns the full JSON string of all Objects & Characters in the level
	 * (their Name, ID, Role, Description, and current Physics values) that gets sent to Meta AI.
	 */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Physics Director")
	FString BuildWorldContextJsonString(const FString& EventOrDirective = TEXT("")) const;

	/**
	 * Scans the entire level and prints on screen (and returns) every single Object, Tool, and Character
	 * that Meta AI can see and control in the world!
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Physics Director")
	int32 ScanAndPrintDiscoveredWorldObjects(TArray<FString>& OutDiscoveredItemsList) const;

	/**
	 * Directly parses and executes a Meta AI JSON response containing "physics_commands" on the world's Actors.
	 * Returns how many Actors had their physics updated.
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Physics Director")
	int32 ExecuteAIPhysicsResponseJson(const FString& AIJsonOrText, FString& OutExtractedMessage);

	/**
	 * Ping / List available models from https://api.meta.ai/v1/models to verify API Key authentication.
	 */
	UFUNCTION(BlueprintCallable, Category = "Meta AI")
	void VerifyApiKeyAndListModels();

	/** Returns how many API requests have been sent in this Play session */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Credit Protection")
	int32 GetTotalRequestsSentThisSession() const { return SessionRequestCount; }

	/** Returns how many API requests remain in your session budget */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Credit Protection")
	int32 GetRemainingSessionRequests() const { return FMath::Max(0, MaxRequestsPerSession - SessionRequestCount); }

	/** Returns true if a request can currently be sent (not on cooldown, not in-flight, and under quota) */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Credit Protection")
	bool CanSendRequestNow(FString& OutBlockReason) const;

	/** Manually resets the session request counter if needed during testing */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Credit Protection")
	void ResetSessionQuota();

	// =========================================================================
	// 10. Settings - Spaceship Oxygen & Life Support System
	// =========================================================================

	/** Enable real-time Oxygen consumption and hull decompression depletion */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support")
	bool bEnableOxygenSystem = true;

	/** Player Suit Current Oxygen in Liters/Units (0.0 to 100.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support", meta = (ClampMin = "0.0", ClampMax = "500.0"))
	float PlayerOxygenCurrent = 100.0f;

	/** Player Suit Maximum Oxygen Capacity (defaults to 100.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support", meta = (ClampMin = "10.0", ClampMax = "500.0"))
	float PlayerOxygenMax = 100.0f;

	/** Rate at which the astronaut consumes suit oxygen per second (default: 0.35 -> ~5 minutes per full tank) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float PlayerOxygenConsumptionPerSec = 0.35f;

	/** Spaceship Room / Cabin Current Atmospheric Oxygen in Liters/Units (0.0 to 100.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support", meta = (ClampMin = "0.0", ClampMax = "500.0"))
	float RoomOxygenCurrent = 100.0f;

	/** Spaceship Room / Cabin Maximum Oxygen Capacity (defaults to 100.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support", meta = (ClampMin = "10.0", ClampMax = "500.0"))
	float RoomOxygenMax = 100.0f;

	/** Rate at which Room Oxygen leaks during high-risk breakdowns or hull breaches per second (default: 0.85) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|10. Spaceship Oxygen & Life Support", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float RoomOxygenBreachLeakRatePerSec = 0.85f;

	/** Refills the player's suit oxygen by Amount (or sets to Max if Amount <= 0) */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Oxygen & Life Support")
	void RefillPlayerOxygen(float Amount = 100.0f);

	/** Refills the room / station atmospheric oxygen by Amount (or sets to Max if Amount <= 0) */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Oxygen & Life Support")
	void RefillRoomOxygen(float Amount = 100.0f);

	// =========================================================================
	// 11. UMG UI Binding Getters (Progress Bars, Text Blocks & Colors)
	// =========================================================================

	/** Returns normalized Player Oxygen (0.0 to 1.0) directly bindable to UMG ProgressBar 'SetPercent' */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Oxygen")
	float GetPlayerOxygenPercent() const;

	/** Returns Player Oxygen as clean formatted text (e.g., "85%") for UMG TextBlock */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Oxygen")
	FText GetPlayerOxygenText() const;

	/** Returns Player Oxygen color tint (Cyan -> Orange -> Flashing Red) for UMG ProgressBar Tint */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Oxygen")
	FLinearColor GetPlayerOxygenColor() const;

	/** Returns normalized Room/Station Oxygen (0.0 to 1.0) directly bindable to UMG ProgressBar 'SetPercent' */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Oxygen")
	float GetRoomOxygenPercent() const;

	/** Returns Room/Station Oxygen as clean formatted text (e.g., "94%") for UMG TextBlock */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Oxygen")
	FText GetRoomOxygenText() const;

	/** Returns Room/Station Oxygen color tint for UMG ProgressBar Tint */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Oxygen")
	FLinearColor GetRoomOxygenColor() const;

	/** Returns normalized Autonomous AI Danger/Risk (0.0 to 1.0) directly bindable to UMG ProgressBar 'SetPercent' */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Risk")
	float GetAIRiskPercent() const;

	/** Returns Current Risk Score (0.0 to 100.0) */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Risk")
	float GetAIRiskScore() const { return CurrentAIRiskScorePercent; }

	/** Returns Current Risk Level String ("LOW", "MODERATE", "HIGH", "CRITICAL", "OPTIMAL") */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Risk")
	FString GetAIRiskLevelString() const { return CurrentAIRiskLevel; }

	/** Returns AI Risk formatted text (e.g., "AI RISK: HIGH (78%)") for UMG TextBlock */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Risk")
	FText GetAIRiskText() const;

	/** Returns dynamic Risk Color (Green -> Orange -> Critical Red) for UMG ProgressBar Tint */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Risk")
	FLinearColor GetAIRiskColor() const;

	/** Returns whether an active AI Game Master quest is currently active */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Quest")
	bool HasActiveQuest() const { return bHasActiveAIQuest; }

	/** Returns the current active quest target actor */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Quest")
	AActor* GetActiveQuestTargetActor() const { return ActiveQuestTargetActor.Get(); }

	/** Returns the 3D marker actor */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Quest")
	AActor* GetActiveQuestMarkerActor() const { return ActiveQuestMarkerActor; }

	/** Locates the stationary Space Diagnostic Tablet in the ship */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Station")
	AActor* FindDiagnosticTabletActor() const;

	/** Transitions the space station to 100% nominal state and activates the Diagnostic Tablet for final certification */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Station")
	void ActivateDiagnosticTabletEvaluationState();

	// =========================================================================
	// 9. Pipelined Background AI Quest Engine (Instant Continuous Breakdown Cycle)
	// =========================================================================

	/** True if the background pipeline has precomputed the next AI Quest */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|AI Game Master (Endless Quests)")
	bool bHasPrecomputedNextQuest = false;

	/** True if an asynchronous HTTP call to Meta AI is currently prefetching the next quest */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|AI Game Master (Endless Quests)")
	bool bIsPrecomputingNextQuest = false;

	/** Staged next quest precomputed by Meta AI in the background, ready to activate in 0.00 milliseconds */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|AI Game Master (Endless Quests)")
	FSpaceQuestDefinition PrecomputedNextAIQuest;

	/** Asynchronously sends a background directive to Meta AI to prepare the next upcoming emergency */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	void StartBackgroundPrecomputeNextQuest();

	/** Instantly activates the precomputed background quest, applying its AI physics, lighting, and markers in 0ms */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|AI Game Master (Endless Quests)")
	bool ActivatePrecomputedNextQuest();

	// =========================================================================
	// Settings - Flight Director Radio Voice Audio (Windows SAPI & Chime)
	// =========================================================================

	/** Enable real-time spoken voice audio for Flight Director transmissions via native Windows Speech API */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|Flight Director Audio")
	bool bEnableSpokenVoiceAudio = true;

	/** Optional radio click / beep / chime sound effect played when a transmission starts */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|Flight Director Audio")
	TObjectPtr<USoundBase> RadioTransmissionSound = nullptr;

	/** Speech rate for Flight Director voice (-10 to +10, default 1 for crisp mission control cadence) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|Flight Director Audio", meta = (ClampMin = "-10", ClampMax = "10"))
	int32 VoiceSpeechRate = 1;

	/** Voice volume for Flight Director transmissions (0 to 100, default 100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|Flight Director Audio", meta = (ClampMin = "0", ClampMax = "100"))
	int32 VoiceVolume = 100;

	/** Immediately stops any active Flight Director spoken voice and clears the audio queue */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Flight Director Audio")
	void StopFlightDirectorVoice();

	/** Transmits real-time Flight Director radio advice to the astronaut HUD and audio log */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Flight Director")
	void TransmitFlightDirectorRadioComms(const FString& Message, float DisplayDurationSeconds = 7.0f, bool bLogToDatabase = true);

	/** Initializes the permanent Space Tablet & Quiz Terminal in the room on BeginPlay */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Diagnostic Tablet")
	void InitializePermanentRoomDiagnosticTablet();

	/** Handles player interaction with [E] on objects, terminals, markers, and tablets */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Interaction")
	void HandleTrackedPlayerInteractWithE(AActor* InteractedActor, UPrimitiveComponent* InteractedComponent, FName ComponentName);

	/** Returns the Title of the current active quest (e.g., "Calibrate Digi_Wrench at Terminal_elctric") */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Quest")
	FText GetCurrentQuestTitleText() const;

	/** Returns the Instruction of what to do (e.g., "Take Digi_Wrench (Cyan Box) and go to Terminal_elctric Box & Press [E]!") */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Quest")
	FText GetCurrentQuestInstructionText() const;

	/** Returns normalized Problem-Solving Mastery (0.0 to 1.0) for UMG ProgressBar 'SetPercent' */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Mastery")
	float GetProblemSolvingMasteryPercent() const;

	/** Returns Problem-Solving Mastery text (e.g., "Mastery: 72 / 100") */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Mastery")
	FText GetProblemSolvingMasteryText() const;

	/** Returns dynamic real-time contextual control hints that teach the player exact keys based on what they are doing! */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Advice & Controls")
	FText GetCurrentContextualControlHintText() const;

	/** Returns latest Flight Director transmission / coaching text from the AI */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Advice & Controls")
	FText GetLatestFlightDirectorAdviceText() const;

	/** Returns Trainee Grade assigned autonomously by Meta AI */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Evaluation")
	FString GetAIGradeString() const { return CurrentAIGrade; }

	/** Returns Trainee Score assigned autonomously by Meta AI */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Evaluation")
	float GetAIFinalScore() const { return CurrentAIFinalScore; }

	/** Returns Flight Readiness status assigned autonomously by Meta AI */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Evaluation")
	FString GetAIFlightReadinessString() const { return CurrentAIFlightReadiness; }

	/** Returns detailed evaluation study written autonomously by Meta AI */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings|Evaluation")
	FString GetAIEvaluationSummaryString() const { return CurrentAIEvaluationSummary; }

	/**
	 * Single-Node HUD State Getter: Returns the complete Astronaut HUD state struct in one call!
	 * In your Widget Blueprint Event Tick or Timer, simply drop 'Get Astronaut HUD State', break the struct, and wire to your UI widgets!
	 */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings")
	FAstronautHUDState GetAstronautHUDState() const;

	/**
	 * GLOBAL STATIC HUD GETTER: Callable from ANY Widget Blueprint with 0 casting or actor references!
	 */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings", meta = (WorldContext = "WorldContextObject", DisplayName = "Get Astronaut HUD State (Global)"))
	static FAstronautHUDState GetAstronautHUDStateGlobal(const UObject* WorldContextObject);

	/**
	 * GLOBAL STATIC COMPONENT GETTER: Finds the active Meta AI Component from any Widget or Actor!
	 */
	UFUNCTION(BlueprintPure, Category = "Meta AI|UI Bindings", meta = (WorldContext = "WorldContextObject", DisplayName = "Get Meta AI Component (Global)"))
	static UMetaAIComponent* GetMetaAIComponentGlobal(const UObject* WorldContextObject);

	// =========================================================================
	// 12. Pre-Emergency Astronaut Orientation & Competency Training Protocol
	// =========================================================================

	/** If true, the AI Flight Director conducts a 4-step interactive zero-G orientation protocol before any threat or breakdown begins! */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meta AI|12. Pre-Emergency Orientation Protocol")
	bool bRequireOrientationTrainingBeforeThreats = true;

	/** Current phase of pre-emergency orientation */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Meta AI|12. Pre-Emergency Orientation Protocol")
	EAstronautTrainingPhase CurrentTrainingPhase = EAstronautTrainingPhase::NotStarted;

	/** Begins the 4-step astronaut orientation protocol */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Orientation & Training")
	void StartOrientationTraining();

	/** Advances the orientation training protocol to the specified next phase */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Orientation & Training")
	void AdvanceOrientationTraining(EAstronautTrainingPhase NextPhase);

	/** Certifies astronaut as 100% mission-ready and schedules the first live emergency breakdown */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Orientation & Training")
	void CompleteOrientationTrainingAndCertify();

	/** Allows skipping orientation training and immediately starting live emergency threats */
	UFUNCTION(BlueprintCallable, Category = "Meta AI|Orientation & Training")
	void SkipOrientationTrainingAndStartThreats();

	/** Returns the current orientation training phase */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Orientation & Training")
	EAstronautTrainingPhase GetCurrentTrainingPhase() const { return CurrentTrainingPhase; }

	/** Returns true if the astronaut is currently undergoing orientation training (before threats begin) */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Orientation & Training")
	bool IsInOrientationTraining() const;

	/** Returns formatted progress text for the orientation badge (e.g., "Orientation: Step 2 of 4") */
	UFUNCTION(BlueprintPure, Category = "Meta AI|Orientation & Training")
	FText GetTrainingStepProgressText() const;

private:
	/** Static/Session-wide protection state so even if multiple Actors have MetaAIComponent, they share the global safety cap! */
	static int32 GlobalSessionRequestCount;
	static double GlobalLastRequestTimestamp;
	static bool bGlobalRequestInFlight;

	int32 SessionRequestCount = 0;
	double LastRequestRealtimeSeconds = -1000.0;
	bool bIsRequestInFlight = false;
	bool bCircuitBreakerTripped = false;
	FString LastSentPrompt;
	double LastSentPromptTimestamp = -1000.0;

	/** In-memory cache mapping Prompt -> AI Response so repeated events cost 0 credits */
	UPROPERTY(Transient)
	TMap<FString, FString> SessionResponseCache;

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveHttpRequest;

	FString BuildFullSystemPromptWithPhysicsSchema() const;
	FString GenerateSmartFallbackPhysicsResponse() const;
	bool ApplySinglePhysicsCommandToActor(AActor* TargetActor, const TSharedPtr<class FJsonObject>& CmdObj, FString& OutAppliedSummary, TSharedPtr<class FJsonObject>& OutBeforeAfterLogObj);
	void FindMatchingActorsForTarget(const FString& TargetIdentifier, TArray<AActor*>& OutMatchingActors) const;

	void OnChatCompletionResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void OnModelsListResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void OnPhpDatabaseResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void SendJsonToPhpBackend(const TSharedPtr<class FJsonObject>& PayloadJson);
	void LogAIAdviceToPhpDatabase(const FString& AIMessage, const FString& RawAIJson);
	void BindToPlayerSimulationComponents();

	UFUNCTION()
	void HandleTrackedItemGrabOrSnap(AActor* ItemActor, AActor* SnapTargetActor, bool bWasSnapped);

	UFUNCTION()
	void HandleTrackedZeroGHandAction(bool bIsPullAction, const FVector& TargetPoint, const FVector& AppliedImpulse);

	UFUNCTION()
	void HandleTrackedWallInteraction(const FString& InteractionType, AActor* SurfaceActor, const FVector& WorldPoint, float MetricValue);



	void PrintScreenDebug(const FString& Message, FColor Color = FColor::Cyan, float Duration = 12.0f) const;

	// Mission Timer & Telemetry State synced with MySQL
	FString CurrentSessionUUID;
	float MissionStartTimeSeconds = 0.0f;
	float FinalMissionDurationSeconds = 0.0f;
	bool bIsMissionFinished = false;
	int32 SessionItemsGrabbedCount = 0;
	int32 SessionItemsDroppedCount = 0;
	int32 SessionItemsSnappedCount = 0;
	int32 SessionWallGrabsCount = 0;
	int32 SessionWallStopsCount = 0;
	int32 SessionWallLaunchesCount = 0;
	float SessionWallWalkDistanceMeters = 0.0f;
	FString LastGrabbedWallName = TEXT("None");
	FString LastReadablePhysicsSummary = TEXT("No physics changes applied.");
	TArray<TSharedPtr<class FJsonValue>> LastObjectEditsArray;

	// Spaceship Shake, Obstacle Spawner, Lighting & Repair State
	int32 SessionAISpawnedObstaclesCount = 0;
	int32 SessionAIShipShakesCount = 0;
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AActor>> SpawnedObstacleActors;
	float ActiveCameraShakeRemainingTime = 0.0f;
	float ActiveCameraShakeTotalDuration = 0.0f;
	float ActiveCameraShakeIntensity = 0.0f;
	float CameraShakePhaseAccumulator = 0.0f;
	float ShipLightAnimationTime = 0.0f;
	float TargetMissionTimeSeconds = 180.0f;
	int32 RequiredSnapsToFinishMission = 0;
	TArray<FTrackedShipLightState> TrackedShipLights;

	// 3D AI Game Master Quest Box + Floating Arrow + Tool Marker State
	UPROPERTY(Transient)
	TObjectPtr<AActor> ActiveQuestMarkerActor = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UBoxComponent> ActiveQuestBoxComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UArrowComponent> ActiveQuestArrowComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ActiveQuestArrowMeshComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> ActiveQuestTextComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> ActiveQuestBeaconLight = nullptr;

	// Step 1 Floating Cyan Tool Marker (Points at Digi_Wrench / required tool until grabbed!)
	UPROPERTY(Transient)
	TObjectPtr<AActor> ActiveToolMarkerActor = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UBoxComponent> ActiveToolBoxComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UArrowComponent> ActiveToolArrowComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ActiveToolArrowMeshComp = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> ActiveToolTextComp = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> DiscoveredLevelTools;
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> DiscoveredLevelTargets;

	/** Quests fetched from the MySQL 'quest_catalog' table on session start so AI can reuse uncompleted quests or avoid repeating mastered ones! */
	UPROPERTY(Transient)
	TArray<FSpaceQuestDefinition> CachedDatabaseQuests;

	/** Normalized quest keys (e.g. "terminal_elctric__digi_wrench") that the player has already completed in this session or mastered quickly in the DB! */
	TSet<FString> CompletedOrMasteredQuestKeys;

	FString CurrentQuestKey = TEXT("");
	float CurrentQuestStartTimeSeconds = 0.0f;

	TWeakObjectPtr<AActor> ActiveQuestTargetActor = nullptr;
	TWeakObjectPtr<AActor> ActiveQuestRequiredToolActor = nullptr;
	TWeakObjectPtr<AActor> LastBrokenQuestActor = nullptr;
	TWeakObjectPtr<AActor> LastUsedQuestToolActor = nullptr;

	/** History of recently broken target actors (ring buffer) to prevent back-to-back target repetition */
	UPROPERTY(Transient)
	mutable TArray<TWeakObjectPtr<AActor>> RecentlyBrokenQuestActors;

	/** History of recently used target system categories (e.g. "COOLANT_FLUID_PIPES", "ELECTRICAL_POWER_BUS", "AVIONICS_COMPUTER_SYSTEMS", "PROPULSION_REACTOR_CORE", "LIFE_SUPPORT_ATMOSPHERE") */
	UPROPERTY(Transient)
	mutable TArray<FString> RecentlyUsedTargetCategories;

	/** History of recently used target semantic names (e.g. "pipe", "terminal_elctric", "comm_terminal", "generator") */
	UPROPERTY(Transient)
	mutable TArray<FString> RecentlyUsedTargetNames;

	/** History of recently used tool actors to rotate between different tools */
	UPROPERTY(Transient)
	mutable TArray<TWeakObjectPtr<AActor>> RecentlyUsedQuestToolActors;

	/** History of recently used tool categories (e.g. "VALVE_WHEEL", "DIGI_WRENCH", "FIRE_EXTINGUISHER", "POWER_CELL_FUSE", "PLASMA_WELDER", "DIAGNOSTIC_TABLET") */
	UPROPERTY(Transient)
	mutable TArray<FString> RecentlyUsedToolCategories;

	/** Keys of quests that have already been assigned or buffered in the current session so they are not repeated */
	TSet<FString> SessionAssignedQuestKeys;

	static FString ClassifyTargetSystemCategory(AActor* InActor, const FString& InTargetName);
	static FString ClassifyToolCategory(AActor* InActor, const FString& InToolName);
	void RecordQuestTargetAndToolUsage(AActor* InTarget, const FString& InTargetName, AActor* InTool, const FString& InToolName);
	bool IsRecentlyUsedQuestTarget(AActor* InTarget, const FString& InTargetName) const;

	/** When true, ExecuteAIPhysicsResponseJson applies physics/lights only and ignores any 'ai_quest' block (used when replaying buffered AI JSON). */
	bool bSuppressAIQuestParsing = false;
	FString ActiveQuestRequiredItem = TEXT("");
	bool bHasActiveAIQuest = false;

	bool bIsAwaitingBreakdownAIResponse = false;

	float QuestMarkerAnimTime = 0.0f;
	float QuestCompletedFlashTimer = 0.0f;
	float TimeSinceLastBreakdownCheck = 0.0f;
	FTimerHandle NextEndlessQuestTimerHandle;

	void UpdateOxygenSimulation(float DeltaTime);
	void UpdateOrientationTraining(float DeltaTime);
	FString CachedFlightDirectorAdvice = TEXT("");

	float TrainingAccumulatedMoveTime = 0.0f;
	float TrainingAccumulatedDistanceTraversed = 0.0f;
	FVector TrainingLastPlayerLocation = FVector::ZeroVector;
	FTimerHandle ThreatActivationDelayTimerHandle;

	void DiscoverAndInitializeShipLights();
	void DiscoverAndInitializeLevelObstacles();
	void DiscoverAllLevelToolsAndTerminals();
	void FindAndCacheSpaceshipRoomBounds();
	void EnforceSpaceshipRoomBoundsOnAllToolsAndObjects(float DeltaTime);
	void UpdateAnimatedShipLights(float DeltaTime);
	void UpdateActive3DQuestMarker(float DeltaTime);
	void ApplyAutonomousAIRiskResponse(const FString& InRiskLevel, float InRiskScore);
	AActor* PickNextBreakableActorInShip() const;
	AActor* PickNextToolActorInShip(AActor* ForTargetActor = nullptr) const;


	void SpawnOrUpdate3DToolMarker(AActor* ToolActor, const FString& ToolName, const FString& TargetName);
	static FString MakeNormalizedQuestKey(const FString& InTargetName, const FString& InToolName, const FString& InTitle = TEXT(""));
	bool TryPickUncompletedManualOrDatabaseQuest(FSpaceQuestDefinition& OutQuestDef, AActor*& OutMatchedTarget, AActor*& OutMatchedTool);
	void SyncQuestToDatabaseCatalog(const FString& InQuestKey, const FString& InSource, const FString& InTitle, const FString& InInstruction, const FString& InTargetName, const FString& InToolName, bool bIsCompleted, float CompletionDurationSec = 0.0f);

	// Historical stats fetched from MySQL on session start so Meta AI knows past performance
	int32 HistoricalCompletedMissions = 0;
	float HistoricalBestTimeSeconds = 0.0f;
	float HistoricalAvgTimeSeconds = 0.0f;
	FString HistoricalLastAdvice = TEXT("");

	FCriticalSection FlightDirectorVoiceCriticalSection;
	TArray<FString> FlightDirectorVoiceQueue;
	bool bIsFlightDirectorSpeaking = false;
	bool bStopVoiceRequested = false;
	/** Incremented on every interrupt; the voice worker purges any sentence that was started under an older generation. */
	int32 FlightDirectorVoiceGeneration = 0;
	void EnqueueFlightDirectorSpokenText(const FString& InText);
	/** Immediately cuts the sentence currently being spoken and drops queued lines, so the next transmission plays at once. */
	void InterruptFlightDirectorVoice();
};
