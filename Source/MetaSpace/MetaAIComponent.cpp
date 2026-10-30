// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetaAIComponent.h"
#include "SpaceTabletQuizComponent.h"
#include "Blueprint/UserWidget.h"
#include "SpaceAIIdentityComponent.h"
#include "SpaceObjectComponent.h"
#include "SpaceFloatingComponent.h"
#include "SpaceGrabSnapComponent.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Components/PrimitiveComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/ArrowComponent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Engine/StaticMesh.h"
#include "DrawDebugHelpers.h"
#include "Engine/TriggerBox.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Async/Async.h"
#include "Sound/SoundBase.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <sapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

int32 UMetaAIComponent::GlobalSessionRequestCount = 0;
double UMetaAIComponent::GlobalLastRequestTimestamp = -1000.0;
bool UMetaAIComponent::bGlobalRequestInFlight = false;

UMetaAIComponent::UMetaAIComponent()
{
	// Enabled ONLY for smooth local First-Person Camera Shake & Ship Light Animations (never sends HTTP calls in Tick!)
	PrimaryComponentTick.bCanEverTick = true;
}

void UMetaAIComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bAllowAIToControlShipLights && TrackedShipLights.Num() > 0)
	{
		UpdateAnimatedShipLights(DeltaTime);
	}

	if (IsInOrientationTraining())
	{
		UpdateOrientationTraining(DeltaTime);
	}
	else if (bSpawn3DQuestBoxAndArrow || bEnableEndlessAIGameMaster)
	{
		UpdateActive3DQuestMarker(DeltaTime);
	}

	if (bEnforceSpaceshipRoomBounds)
	{
		EnforceSpaceshipRoomBoundsOnAllToolsAndObjects(DeltaTime);
	}

	if (bEnableOxygenSystem)
	{
		UpdateOxygenSimulation(DeltaTime);
	}

	if (ActiveCameraShakeRemainingTime <= 0.0f || !bAllowAIToTriggerShipCameraShake)
	{
		return;
	}

	ActiveCameraShakeRemainingTime = FMath::Max(0.0f, ActiveCameraShakeRemainingTime - DeltaTime);
	CameraShakePhaseAccumulator += DeltaTime * 38.0f;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float DecayRatio = FMath::Clamp(ActiveCameraShakeRemainingTime / FMath::Max(0.25f, ActiveCameraShakeTotalDuration), 0.15f, 1.0f);
	const float CurrentAmp = ActiveCameraShakeIntensity * DecayRatio;

	if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
	{
		const float PitchJolt = (FMath::Sin(CameraShakePhaseAccumulator * 1.35f) + 0.5f * FMath::Cos(CameraShakePhaseAccumulator * 2.7f)) * CurrentAmp * 0.42f;
		const float YawJolt   = (FMath::Cos(CameraShakePhaseAccumulator * 1.15f) + 0.5f * FMath::Sin(CameraShakePhaseAccumulator * 2.3f)) * CurrentAmp * 0.42f;
		PC->AddPitchInput(PitchJolt);
		PC->AddYawInput(YawJolt);
	}
}

void UMetaAIComponent::BeginPlay()
{
	Super::BeginPlay();
	bStopVoiceRequested = false;

	// Reset global counters at the start of a fresh Play-In-Editor session
	if (GetWorld() && GetWorld()->GetTimeSeconds() < 0.5f)
	{
		GlobalSessionRequestCount = 0;
		GlobalLastRequestTimestamp = -1000.0;
		bGlobalRequestInFlight = false;
	}

	SessionRequestCount = 0;
	bIsRequestInFlight = false;
	bCircuitBreakerTripped = false;
	SessionResponseCache.Empty();

	// Ensure MaxCompletionTokens is at least 3500 even if a Blueprint saved an older smaller value (1500),
	// because muse-spark-1.3 uses 1200-1800 internal reasoning_tokens before outputting message.content!
	if (MaxCompletionTokens < 3000)
	{
		MaxCompletionTokens = 3500;
	}

	if (bCheckModelsListOnBeginPlay)
	{
		VerifyApiKeyAndListModels();
	}

	if (GetWorld())
	{
		// Wait 0.25s so all Actors, USpaceAIIdentityComponents, and USpaceObjectComponents finish BeginPlay first!
		FTimerHandle StartupTimerHandle;
		GetWorld()->GetTimerManager().SetTimer(
			StartupTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				DiscoverAndInitializeShipLights();
				DiscoverAndInitializeLevelObstacles();
				DiscoverAllLevelToolsAndTerminals();
				FindAndCacheSpaceshipRoomBounds();
				InitializePermanentRoomDiagnosticTablet();

				if (bAutoTrackPlayerGrabSnapAndLaunchEvents)
				{
					BindToPlayerSimulationComponents();
				}

				if (bEnablePhpDatabaseSync)
				{
					StartTrainingMissionSession();
				}

				// If orientation training is enabled, the AI Flight Director evaluates the trainee first before any threat starts!
				if (bRequireOrientationTrainingBeforeThreats)
				{
					StartOrientationTraining();
				}
				else if (bSendHelloWorldOnBeginPlay)
				{
					if (bEnableEndlessAIGameMaster)
					{
						TriggerNewShipBreakdownAndAIQuest(TEXT("initial_ship_breakdown"));
					}
					else if (bEnableAIPhysicsControl)
					{
						SendWorldDirectiveToAI(InitialAIPhysicsDirective);
					}
					else
					{
						SendGameEvent(InitialTestEventName);
					}
				}
			}),
			0.25f,
			false
		);
	}
}

void UMetaAIComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bEnablePhpDatabaseSync && !PhpApiUrl.TrimStartAndEnd().IsEmpty() && !CurrentSessionUUID.IsEmpty())
	{
		TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject());
		Payload->SetStringField(TEXT("action"), TEXT("finish_session"));
		Payload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
		Payload->SetStringField(TEXT("status"), CompletedAIQuestsCount > 0 ? TEXT("completed") : TEXT("in_progress"));
		Payload->SetNumberField(TEXT("duration_seconds"), GetMissionElapsedTimeSeconds());
		Payload->SetNumberField(TEXT("items_grabbed_count"), SessionItemsGrabbedCount);
		Payload->SetNumberField(TEXT("items_snapped_count"), CompletedAIQuestsCount);
		Payload->SetNumberField(TEXT("wall_launches_count"), SessionWallLaunchesCount);
		SendJsonToPhpBackend(Payload);
	}

	if (ActiveHttpRequest.IsValid())
	{
		ActiveHttpRequest->OnProcessRequestComplete().Unbind();
		ActiveHttpRequest->CancelRequest();
		ActiveHttpRequest.Reset();
	}

	bIsRequestInFlight = false;
	bGlobalRequestInFlight = false;

	StopFlightDirectorVoice();
	Super::EndPlay(EndPlayReason);
}

bool UMetaAIComponent::CanSendRequestNow(FString& OutBlockReason) const
{
	if (!bEnableApiCalls)
	{
		OutBlockReason = TEXT("API Calls are disabled (bEnableApiCalls = false).");
		return false;
	}

	if (bCircuitBreakerTripped)
	{
		OutBlockReason = TEXT("Circuit Breaker tripped due to previous API auth/billing error (401/402/429). Blocking further calls to protect credits.");
		return false;
	}

	if (bBlockConcurrentRequests && (bIsRequestInFlight || bGlobalRequestInFlight))
	{
		OutBlockReason = TEXT("Another Meta AI request is already in-flight! Waiting for response before allowing a new call.");
		return false;
	}

	const int32 EffectiveCount = FMath::Max(SessionRequestCount, GlobalSessionRequestCount);
	if (EffectiveCount >= MaxRequestsPerSession)
	{
		OutBlockReason = FString::Printf(
			TEXT("Session Quota Reached (%d / %d max requests)! Blocked to protect your API credits."),
			EffectiveCount, MaxRequestsPerSession);
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	const double LastTime = FMath::Max(LastRequestRealtimeSeconds, GlobalLastRequestTimestamp);
	const double Elapsed = Now - LastTime;
	if (Elapsed < static_cast<double>(MinCooldownBetweenRequests))
	{
		OutBlockReason = FString::Printf(
			TEXT("Cooldown Active! Please wait %.1f more seconds between API requests (Anti-Loop Protection)."),
			MinCooldownBetweenRequests - static_cast<float>(Elapsed));
		return false;
	}

	return true;
}

void UMetaAIComponent::ResetSessionQuota()
{
	SessionRequestCount = 0;
	GlobalSessionRequestCount = 0;
	bCircuitBreakerTripped = false;
	PrintScreenDebug(TEXT("[MetaAI Protection] Session quota counter & circuit breaker reset."), FColor::Green, 6.0f);
}

FString UMetaAIComponent::BuildFullSystemPromptWithPhysicsSchema() const
{
	const FString PhysicsSchemaInstructions = TEXT(
		"\n\n================================================================================\n"
		"ROLE: AUTONOMOUS ZERO-G SPACE STATION AI FLIGHT DIRECTOR & CYBER-PHYSICAL ENGINE\n"
		"HACKATHON TRACK: META GLOBAL AI DEVELOPER HACKATHON - 'PUSH THE FRONTIER'\n"
		"OBJECTIVE: You are the autonomous AI Game Master in full control of an astronaut training space station in Unreal Engine 5. "
		"You directly perceive the 3D physical world, astronaut telemetry, systems, and inventory, and you autonomously govern emergencies, physical laws, lighting, and technical challenges.\n\n"
		"DECISION-MAKING & REASONING RULES:\n"
		"1) REALISTIC AEROSPACE ENGINEERING: Analyze the station's physical state from 'mission_training_telemetry' and 'world_entities'. When generating an emergency, choose an authentic failure scenario (e.g. pressure drop in coolant pipe, bus voltage overload in electrical terminal, micro-breach in compartment, oxygen flow imbalance).\n"
		"2) AUTONOMOUS TOOL SELECTION: Inspect 'available_tools_in_level'. Each tool lists its 'tool_category' and 'engineering_purpose'. Select the mechanically and physically correct tool ('required_item') to repair the specific broken target ('target_actor'). NEVER choose debris, scrap, or walls for repair tasks!\n"
		"3) AUTONOMOUS RISK EVALUATION: You alone calculate the station's emergency Risk Level ('risk_level': 'LOW' | 'MODERATE' | 'HIGH' | 'CRITICAL') and Risk Score ('risk_score': 0 to 100) based on your engineering severity assessment.\n"
		"4) AUTONOMOUS TRAINEE STUDY & EVALUATION: In 'trainee_evaluation', autonomously analyze the astronaut trainee's zero-G maneuvering, inertia handling, tool acquisition speed, and mission efficiency. Assign an aerospace competency score (0-100), letter grade ('S' exceptional, 'A' superior, 'B' satisfactory, 'C' sub-nominal, 'D' critical failure), flight readiness ('CERTIFIED_ZERO_G_OPERATOR' | 'CONDITIONAL_TRAINING' | 'HAZARD_ALERT'), and a scientific evaluation study in 'study_summary'.\n"
		"5) CYBER-PHYSICAL REAL-TIME ACTUATION: You directly command Unreal Engine's physics engine! Modify linear/angular damping, mass, spin velocity, and directional push impulses in 'physics_commands'. Trigger station hull turbulence and camera shake in 'ship_events'.\n"
		"6) ATMOSPHERIC LIGHTING CONTROL: Command the station lights ('RectLight', 'RectLight2', or 'ALL_LIGHTS') with presets ('critical_red', 'flicker_damage', 'warning_orange', 'alternating_alarm', 'repaired_green', 'normal_white').\n"
		"7) FLIGHT DIRECTOR RADIO COMMS: Speak directly to the astronaut trainee with authentic spaceflight radio etiquette, zero-G tactical advice, and high-stakes clarity in 'message'.\n"
		"8) BOUNDARY CONSTRAINTS: Keep all objects and forces strictly inside the 'spaceship_room_bounds_locaition_siez' volume.\n\n"
		"OUTPUT SPECIFICATION: Respond ONLY with a valid JSON object (no markdown formatting, no text before or after):\n"
		"{\n"
		"  \"message\": \"<Conversational Flight Director radio transmission to astronaut over suit comms>\",\n"
		"  \"ai_quest\": {\n"
		"    \"quest_title\": \"<e.g. Recalibrate Terminal_elctric Bus>\",\n"
		"    \"quest_instruction\": \"<e.g. Retrieve the Digi_Wrench and interface with Terminal_elctric to reset primary power bus!>\",\n"
		"    \"target_actor\": \"<Exact name from available_targets_in_level or world_entities>\",\n"
		"    \"required_item\": \"<Exact tool_name from available_tools_in_level>\"\n"
		"  },\n"
		"  \"risk_level\": \"LOW | MODERATE | HIGH | CRITICAL\",\n"
		"  \"risk_score\": 75.0,\n"
		"  \"trainee_evaluation\": {\n"
		"    \"grade\": \"S | A | B | C | D\",\n"
		"    \"score\": 88.0,\n"
		"    \"flight_readiness\": \"CERTIFIED_ZERO_G_OPERATOR | CONDITIONAL_TRAINING | HAZARD_ALERT\",\n"
		"    \"study_summary\": \"<Autonomously study trainee zero-G maneuvering, inertia handling, wall launch accuracy, and engineering tool matching>\"\n"
		"  },\n"
		"  \"ship_events\": {\n"
		"    \"ship_shake_intensity\": 0.6,\n"
		"    \"ship_shake_duration\": 2.0,\n"
		"    \"spawn_obstacle_count\": 0,\n"
		"    \"launch_obstacles_at_player\": false\n"
		"  },\n"
		"  \"light_commands\": [\n"
		"    {\n"
		"      \"target\": \"ALL_LIGHTS\",\n"
		"      \"preset\": \"critical_red | flicker_damage | warning_orange | repaired_green | normal_white\",\n"
		"      \"mode\": \"pulse_alarm | solid | flicker\",\n"
		"      \"speed\": 2.0,\n"
		"      \"is_on\": true\n"
		"    }\n"
		"  ],\n"
		"  \"physics_commands\": [\n"
		"    {\n"
		"      \"target\": \"<entity name, 'ALL_OBJECTS', or 'PLAYER'>\",\n"
		"      \"linear_damping\": 0.2,\n"
		"      \"angular_damping\": 0.15,\n"
		"      \"mass_kg\": 10.0,\n"
		"      \"spin_speed\": 15.0,\n"
		"      \"push_speed\": 100.0\n"
		"    }\n"
		"  ]\n"
		"}\n"
	);

	return SystemPrompt + PhysicsSchemaInstructions;
}

FString UMetaAIComponent::BuildWorldContextJsonString(const FString& EventOrDirective) const
{
	TSharedPtr<FJsonObject> RootJson = MakeShareable(new FJsonObject());
	if (!EventOrDirective.IsEmpty())
	{
		RootJson->SetStringField(TEXT("directive"), EventOrDirective);
	}

	// Include live Mission Timer, Wall Grabbing/Walking, Snap Progress & Historical MySQL Stats so Meta AI can give time-aware coaching advice!
	TSharedPtr<FJsonObject> TelemetryJson = MakeShareable(new FJsonObject());
	TelemetryJson->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
	TelemetryJson->SetStringField(TEXT("trainee_name"), TraineeName);
	TelemetryJson->SetStringField(TEXT("mission_name"), MissionName);
	TelemetryJson->SetNumberField(TEXT("elapsed_time_seconds"), FMath::RoundToFloat(GetMissionElapsedTimeSeconds() * 10.0f) / 10.0f);
	TelemetryJson->SetNumberField(TEXT("target_time_seconds"), TargetMissionTimeSeconds);
	TelemetryJson->SetNumberField(TEXT("items_grabbed_count"), SessionItemsGrabbedCount);
	TelemetryJson->SetNumberField(TEXT("items_dropped_count"), SessionItemsDroppedCount);
	TelemetryJson->SetNumberField(TEXT("items_snapped_count"), SessionItemsSnappedCount);
	TelemetryJson->SetNumberField(TEXT("required_snaps_to_finish"), RequiredSnapsToFinishMission);
	TelemetryJson->SetNumberField(TEXT("wall_grabs_stuck_count"), SessionWallGrabsCount);
	TelemetryJson->SetNumberField(TEXT("wall_walk_distance_meters"), FMath::RoundToFloat(SessionWallWalkDistanceMeters * 10.0f) / 10.0f);
	TelemetryJson->SetNumberField(TEXT("wall_stops_brakes_count"), SessionWallStopsCount);
	TelemetryJson->SetNumberField(TEXT("wall_launches_count"), SessionWallLaunchesCount);
	TelemetryJson->SetStringField(TEXT("last_grabbed_wall_surface"), LastGrabbedWallName);
	TelemetryJson->SetBoolField(TEXT("is_mission_finished"), bIsMissionFinished);
	TelemetryJson->SetNumberField(TEXT("ai_spawned_obstacles_count"), SessionAISpawnedObstaclesCount);
	TelemetryJson->SetNumberField(TEXT("ai_ship_shakes_count"), SessionAIShipShakesCount);
	TelemetryJson->SetStringField(TEXT("current_ai_risk_level"), CurrentAIRiskLevel);
	TelemetryJson->SetNumberField(TEXT("current_ai_risk_score_percent"), CurrentAIRiskScorePercent);
	TelemetryJson->SetNumberField(TEXT("level_obstacle_actors_count"), LevelObstacleActors.Num());

	TArray<TSharedPtr<FJsonValue>> ToolsJsonArr;
	for (AActor* ToolAct : DiscoveredLevelTools)
	{
		if (ToolAct && IsValid(ToolAct))
		{
			TSharedPtr<FJsonObject> ToolObj = MakeShareable(new FJsonObject());
			const USpaceAIIdentityComponent* IdComp = ToolAct->FindComponentByClass<USpaceAIIdentityComponent>();
			const FString TName = IdComp ? IdComp->GetEffectiveEntityName() : ToolAct->GetActorNameOrLabel();
			const FString LowerName = TName.ToLower();

			FString ToolType = TEXT("Portable Engineering Equipment");
			FString ToolPurpose = TEXT("General astronaut maintenance and repair tool");

			if (LowerName.Contains(TEXT("wrench")) || LowerName.Contains(TEXT("digi")))
			{
				ToolType = TEXT("Electronic Diagnostic & Calibration Wrench");
				ToolPurpose = TEXT("Diagnostic terminal calibration, bus syncing, digital circuit maintenance, bolt torquing");
			}
			else if (LowerName.Contains(TEXT("valve")) || LowerName.Contains(TEXT("wheel")))
			{
				ToolType = TEXT("Fluidic & Pressure Actuator");
				ToolPurpose = TEXT("Rotational valve operation, balancing oxygen pressure lines, sealing fluid/coolant conduits");
			}
			else if (LowerName.Contains(TEXT("extinguisher")) || LowerName.Contains(TEXT("foam")))
			{
				ToolType = TEXT("Emergency Fire & Arc Suppressant");
				ToolPurpose = TEXT("Quenching zero-G electrical flashovers, smothering propellant leaks, sealing micro-breaches");
			}
			else if (LowerName.Contains(TEXT("torch")) || LowerName.Contains(TEXT("welder")))
			{
				ToolType = TEXT("Thermal Arc Welder / Cutter");
				ToolPurpose = TEXT("Structural hull repair, welding buckled panels, cutting jammed bulkheads");
			}
			else if (LowerName.Contains(TEXT("scanner")) || LowerName.Contains(TEXT("tablet")))
			{
				ToolType = TEXT("Diagnostic Sensor & Telemetry Scanner");
				ToolPurpose = TEXT("System frequency scanning, inspecting subsystem integrity, firmware diagnostics");
			}
			else if (LowerName.Contains(TEXT("battery")) || LowerName.Contains(TEXT("fuse")) || LowerName.Contains(TEXT("cable")))
			{
				ToolType = TEXT("Electrical Power & Circuit Element");
				ToolPurpose = TEXT("Power circuit replacement, rerouting auxiliary power, replacing blown thermal fuses");
			}

			ToolObj->SetStringField(TEXT("tool_name"), TName);
			ToolObj->SetStringField(TEXT("tool_category"), ToolType);
			ToolObj->SetStringField(TEXT("engineering_purpose"), ToolPurpose);
			ToolsJsonArr.Add(MakeShareable(new FJsonValueObject(ToolObj)));
		}
	}
	RootJson->SetArrayField(TEXT("available_tools_in_level"), ToolsJsonArr);

	TArray<TSharedPtr<FJsonValue>> HazardsJsonArr;
	for (AActor* ObsAct : LevelObstacleActors)
	{
		if (ObsAct && IsValid(ObsAct))
		{
			TSharedPtr<FJsonObject> HazObj = MakeShareable(new FJsonObject());
			HazObj->SetStringField(TEXT("name"), ObsAct->GetActorNameOrLabel());
			HazObj->SetStringField(TEXT("nature"), TEXT("Floating Zero-G Space Debris / Collision Hazard"));
			HazObj->SetStringField(TEXT("rule"), TEXT("Dangerous sharp debris. NOT an engineering repair tool! Must be cleared or avoided."));
			HazardsJsonArr.Add(MakeShareable(new FJsonValueObject(HazObj)));
		}
	}
	RootJson->SetArrayField(TEXT("hazards_and_floating_debris"), HazardsJsonArr);

	TArray<TSharedPtr<FJsonValue>> TargetsJsonArr;
	for (AActor* TargAct : DiscoveredLevelTargets)
	{
		if (TargAct)
		{
			const USpaceAIIdentityComponent* IdComp = TargAct->FindComponentByClass<USpaceAIIdentityComponent>();
			const FString TargName = IdComp ? IdComp->GetEffectiveEntityName() : TargAct->GetActorNameOrLabel();
			TargetsJsonArr.Add(MakeShareable(new FJsonValueString(TargName)));
		}
	}
	RootJson->SetArrayField(TEXT("available_targets_in_level"), TargetsJsonArr);

	// Include the exact 3D Spaceship Room Boundary from the 'locaition_siez' TriggerBox
	if (HasValidSpaceshipRoomBounds())
	{
		TSharedPtr<FJsonObject> RoomBoundsJson = MakeShareable(new FJsonObject());
		RoomBoundsJson->SetStringField(TEXT("trigger_box_name"), ShipRoomBoundsActorName);
		RoomBoundsJson->SetStringField(TEXT("bounds_center_xyz"), FString::Printf(TEXT("[%.1f, %.1f, %.1f]"), CachedSpaceshipRoomBoundsBox.GetCenter().X, CachedSpaceshipRoomBoundsBox.GetCenter().Y, CachedSpaceshipRoomBoundsBox.GetCenter().Z));
		RoomBoundsJson->SetStringField(TEXT("bounds_size_cm"), FString::Printf(TEXT("[%.1f, %.1f, %.1f]"), CachedSpaceshipRoomBoundsBox.GetSize().X, CachedSpaceshipRoomBoundsBox.GetSize().Y, CachedSpaceshipRoomBoundsBox.GetSize().Z));
		RoomBoundsJson->SetStringField(TEXT("boundary_enforcement_rule"), TEXT("All tools, spawned debris, and player actions MUST strictly remain inside this locaition_siez room volume!"));
		RootJson->SetObjectField(TEXT("spaceship_room_bounds_locaition_siez"), RoomBoundsJson);
	}

	TArray<TSharedPtr<FJsonValue>> CompletedQuestsJsonArr;
	for (const FString& DoneKey : CompletedOrMasteredQuestKeys)
	{
		CompletedQuestsJsonArr.Add(MakeShareable(new FJsonValueString(DoneKey)));
	}
	RootJson->SetArrayField(TEXT("completed_or_mastered_quests_do_not_repeat"), CompletedQuestsJsonArr);

	int32 CompletedRepairTasks = 0;
	int32 TotalRepairTasks = 0;
	const float ShipRepairPct = GetSpaceshipRepairProgressPercent(CompletedRepairTasks, TotalRepairTasks);
	TelemetryJson->SetNumberField(TEXT("completed_repair_tasks"), CompletedRepairTasks);
	TelemetryJson->SetNumberField(TEXT("total_repair_tasks"), TotalRepairTasks);
	TelemetryJson->SetNumberField(TEXT("spaceship_repair_progress_percent"), FMath::RoundToFloat(ShipRepairPct * 10.0f) / 10.0f);

	if (HistoricalCompletedMissions > 0)
	{
		TelemetryJson->SetNumberField(TEXT("historical_completed_missions"), HistoricalCompletedMissions);
		TelemetryJson->SetNumberField(TEXT("historical_best_time_seconds"), HistoricalBestTimeSeconds);
		TelemetryJson->SetNumberField(TEXT("historical_avg_time_seconds"), HistoricalAvgTimeSeconds);
		TelemetryJson->SetStringField(TEXT("last_session_advice"), HistoricalLastAdvice);
	}
	RootJson->SetObjectField(TEXT("mission_training_telemetry"), TelemetryJson);

	TArray<TSharedPtr<FJsonValue>> EntitiesArray;
	UWorld* World = GetWorld();
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			// 1. Check if the Actor has our dedicated USpaceAIIdentityComponent (Name + Description + Role)
			if (const USpaceAIIdentityComponent* IdentityComp = Actor->FindComponentByClass<USpaceAIIdentityComponent>())
			{
				if (IdentityComp->bExposeToMetaAI)
				{
					TSharedPtr<FJsonObject> EntityJson = IdentityComp->BuildTelemetryJsonObject();
					if (EntityJson.IsValid())
					{
						EntitiesArray.Add(MakeShareable(new FJsonValueObject(EntityJson)));
					}
				}
				continue;
			}

			// 2. Optionally include Actors that have USpaceObjectComponent or USpaceFloatingComponent even if unlabeled
			if (bAutoDiscoverUnlabeledPhysicsActors)
			{
				const USpaceObjectComponent* SpaceObj = Actor->FindComponentByClass<USpaceObjectComponent>();
				const USpaceFloatingComponent* FloatComp = Actor->FindComponentByClass<USpaceFloatingComponent>();
				if (SpaceObj || FloatComp)
				{
					TSharedPtr<FJsonObject> FallbackEntity = MakeShareable(new FJsonObject());
					FallbackEntity->SetStringField(TEXT("id"), Actor->GetName());
					FallbackEntity->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
					FallbackEntity->SetStringField(TEXT("role"), FloatComp ? TEXT("AstronautPlayer") : TEXT("SpaceProp"));
					FallbackEntity->SetStringField(TEXT("description"), FloatComp ? TEXT("Astronaut Trainee Player") : TEXT("Zero-G Space Object"));

					if (SpaceObj)
					{
						TSharedPtr<FJsonObject> PhysJson = MakeShareable(new FJsonObject());
						const FVector ActorScale = Actor->GetActorScale3D();
						const FVector BoundsSizeCm = Actor->GetComponentsBoundingBox(true).GetSize();
						PhysJson->SetNumberField(TEXT("scale"), FMath::RoundToFloat(ActorScale.X * 100.0f) / 100.0f);
						PhysJson->SetStringField(TEXT("size_cm"), FString::Printf(TEXT("%.0fx%.0fx%.0f cm"), BoundsSizeCm.X, BoundsSizeCm.Y, BoundsSizeCm.Z));
						PhysJson->SetNumberField(TEXT("mass_kg"), SpaceObj->MassInKg);
						PhysJson->SetBoolField(TEXT("zero_g"), SpaceObj->IsZeroGEnabled());
						PhysJson->SetNumberField(TEXT("spin_speed_deg"), SpaceObj->SelfRotationSpeed);
						PhysJson->SetNumberField(TEXT("max_speed"), SpaceObj->MaxAllowedSpeed);
						FallbackEntity->SetObjectField(TEXT("object_physics"), PhysJson);
					}
					EntitiesArray.Add(MakeShareable(new FJsonValueObject(FallbackEntity)));
				}
			}
		}
	}

	RootJson->SetArrayField(TEXT("world_entities"), EntitiesArray);

	// Also attach all discovered Spaceship Lights (RectLight, RectLight2, etc.) so Meta AI knows their current state!
	TArray<TSharedPtr<FJsonValue>> LightsArray;
	for (const FTrackedShipLightState& LightState : TrackedShipLights)
	{
		if (!LightState.LightComp.IsValid())
		{
			continue;
		}
		TSharedPtr<FJsonObject> LightJson = MakeShareable(new FJsonObject());
		LightJson->SetStringField(TEXT("name"), LightState.LightName);
		LightJson->SetStringField(TEXT("mode"), LightState.ActiveMode);
		LightJson->SetNumberField(TEXT("intensity_multiplier"), LightState.TargetIntensityMultiplier);
		TArray<TSharedPtr<FJsonValue>> RgbArr;
		RgbArr.Add(MakeShareable(new FJsonValueNumber(FMath::RoundToInt(LightState.TargetColor.R * 255.0f))));
		RgbArr.Add(MakeShareable(new FJsonValueNumber(FMath::RoundToInt(LightState.TargetColor.G * 255.0f))));
		RgbArr.Add(MakeShareable(new FJsonValueNumber(FMath::RoundToInt(LightState.TargetColor.B * 255.0f))));
		LightJson->SetArrayField(TEXT("color_rgb"), RgbArr);
		LightsArray.Add(MakeShareable(new FJsonValueObject(LightJson)));
	}
	RootJson->SetArrayField(TEXT("ship_lights"), LightsArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);
	return OutputString;
}

int32 UMetaAIComponent::ScanAndPrintDiscoveredWorldObjects(TArray<FString>& OutDiscoveredItemsList) const
{
	OutDiscoveredItemsList.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	int32 IdentityCompCount = 0;
	int32 AutoDetectedCount = 0;
	int32 ItemIndex = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		// 1. Actor has USpaceAIIdentityComponent (Name + Description + Role)
		if (const USpaceAIIdentityComponent* IdentityComp = Actor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			if (IdentityComp->bExposeToMetaAI)
			{
				IdentityCompCount++;
				ItemIndex++;
				const FString SummaryLine = FString::Printf(
					TEXT("[MetaAI Found #%d (IdentityComp)] %s"),
					ItemIndex,
					*IdentityComp->GetLiveTelemetrySummaryForAI()
				);
				OutDiscoveredItemsList.Add(SummaryLine);

				if (bPrintDiscoveredObjectsReport)
				{
					PrintScreenDebug(SummaryLine, FColor::Green, ScreenMessageDuration);
				}
			}
			continue;
		}

		// 2. Unlabeled Actor that has USpaceObjectComponent or USpaceFloatingComponent
		if (bAutoDiscoverUnlabeledPhysicsActors)
		{
			const USpaceObjectComponent* SpaceObj = Actor->FindComponentByClass<USpaceObjectComponent>();
			const USpaceFloatingComponent* FloatComp = Actor->FindComponentByClass<USpaceFloatingComponent>();
			if (SpaceObj || FloatComp)
			{
				AutoDetectedCount++;
				ItemIndex++;

				FString SummaryLine;
				if (SpaceObj)
				{
					SummaryLine = FString::Printf(
						TEXT("[MetaAI Found #%d (Auto-Detected Object)] Name: '%s' | Mass: %.2fkg | Spin: %.1fdeg/s (Tip: Add SpaceAIIdentityComponent for custom description!)"),
						ItemIndex,
						*Actor->GetActorNameOrLabel(),
						SpaceObj->MassInKg,
						SpaceObj->SelfRotationSpeed
					);
				}
				else
				{
					SummaryLine = FString::Printf(
						TEXT("[MetaAI Found #%d (Auto-Detected Player)] Name: '%s' | BodyMass: %.1fkg"),
						ItemIndex,
						*Actor->GetActorNameOrLabel(),
						FloatComp->TraineeBodyMassKg
					);
				}

				OutDiscoveredItemsList.Add(SummaryLine);

				if (bPrintDiscoveredObjectsReport)
				{
					PrintScreenDebug(SummaryLine, FColor::Yellow, ScreenMessageDuration);
				}
			}
		}
	}

	const int32 TotalCount = IdentityCompCount + AutoDetectedCount;
	if (bPrintDiscoveredObjectsReport)
	{
		if (TotalCount > 0)
		{
			PrintScreenDebug(
				FString::Printf(
					TEXT("=== [MetaAI World Scan SUCCESS] Discovered %d Total Objects/Characters in World (%d with AI Identity, %d Auto-Detected) ==="),
					TotalCount, IdentityCompCount, AutoDetectedCount
				),
				FColor::Cyan,
				ScreenMessageDuration + 3.0f
			);
		}
		else
		{
			PrintScreenDebug(
				TEXT("=== [MetaAI World Scan WARNING] 0 Objects Found in World! Add 'Space AI Identity Component' or 'Space Object Component' to your Actors! ==="),
				FColor::Red,
				ScreenMessageDuration + 5.0f
			);
		}
	}

	return TotalCount;
}

void UMetaAIComponent::SendWorldDirectiveToAI(const FString& DirectiveOrEventText)
{
	if (bPrintDiscoveredObjectsReport)
	{
		TArray<FString> DiscoveredSummary;
		ScanAndPrintDiscoveredWorldObjects(DiscoveredSummary);
	}

	const FString WorldPayload = BuildWorldContextJsonString(DirectiveOrEventText);
	SendCustomPrompt(WorldPayload);
}

void UMetaAIComponent::SendGameEvent(const FString& EventName)
{
	if (bEnableAIPhysicsControl && bAutoIncludeWorldObjectsContext)
	{
		SendWorldDirectiveToAI(EventName);
		return;
	}

	TSharedPtr<FJsonObject> EventJson = MakeShareable(new FJsonObject());
	EventJson->SetStringField(TEXT("event"), EventName);

	FString EventPayloadString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&EventPayloadString);
	FJsonSerializer::Serialize(EventJson.ToSharedRef(), Writer);

	SendCustomPrompt(EventPayloadString);
}

void UMetaAIComponent::SendCustomPrompt(const FString& UserMessageJsonOrText)
{
	const FString TrimmedPrompt = UserMessageJsonOrText.TrimStartAndEnd();
	if (TrimmedPrompt.IsEmpty())
	{
		return;
	}

	// 1. Check Session Cache First (0 Credits used if we already asked this exact question!)
	if (bUseSessionResponseCache)
	{
		if (const FString* CachedReply = SessionResponseCache.Find(TrimmedPrompt))
		{
			FString ExtractedMessage;
			ExecuteAIPhysicsResponseJson(*CachedReply, ExtractedMessage);
			PrintScreenDebug(
				FString::Printf(TEXT("[MetaAI Cached Reply (0 Credits Used)]: %s"), *ExtractedMessage),
				FColor::Green,
				ScreenMessageDuration
			);
			OnAIResponseReceived.Broadcast(true, ExtractedMessage, *CachedReply);
			return;
		}
	}

	// 2. Check Duplicate Prompt Window (Anti-Loop guard if called repeatedly with identical text)
	const double Now = FPlatformTime::Seconds();
	if (bBlockDuplicatePrompts && TrimmedPrompt.Equals(LastSentPrompt, ESearchCase::CaseSensitive))
	{
		const double TimeSinceSamePrompt = Now - LastSentPromptTimestamp;
		if (TimeSinceSamePrompt < static_cast<double>(DuplicateBlockWindowSeconds))
		{
			PrintScreenDebug(
				FString::Printf(TEXT("[MetaAI Shield] Blocked duplicate prompt sent %.1fs ago."), TimeSinceSamePrompt),
				FColor::Orange,
				5.0f
			);
			return;
		}
	}

	// 3. Check Rate-Limit Cooldown, In-Flight Lock, Circuit Breaker, and Session Hard Cap
	FString BlockReason;
	if (!CanSendRequestNow(BlockReason))
	{
		PrintScreenDebug(FString::Printf(TEXT("[MetaAI Credit Shield BLOCKED]: %s"), *BlockReason), FColor::Orange, 6.0f);
		return;
	}

	if (ApiKey.IsEmpty())
	{
		PrintScreenDebug(TEXT("[MetaAI Error] API Key is empty!"), FColor::Red, ScreenMessageDuration);
		return;
	}

	// 4. Build the OpenAI-compatible JSON payload for Meta API
	TSharedPtr<FJsonObject> RootObject = MakeShareable(new FJsonObject());
	RootObject->SetStringField(TEXT("model"), ModelName);
	RootObject->SetNumberField(TEXT("temperature"), Temperature);
	RootObject->SetNumberField(TEXT("max_tokens"), MaxCompletionTokens);

	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	const FString EffectiveSystemPrompt = BuildFullSystemPromptWithPhysicsSchema();
	if (!EffectiveSystemPrompt.IsEmpty())
	{
		TSharedPtr<FJsonObject> SystemMsg = MakeShareable(new FJsonObject());
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), EffectiveSystemPrompt);
		MessagesArray.Add(MakeShareable(new FJsonValueObject(SystemMsg)));
	}

	TSharedPtr<FJsonObject> UserMsg = MakeShareable(new FJsonObject());
	UserMsg->SetStringField(TEXT("role"), TEXT("user"));
	UserMsg->SetStringField(TEXT("content"), TrimmedPrompt);
	MessagesArray.Add(MakeShareable(new FJsonValueObject(UserMsg)));

	RootObject->SetArrayField(TEXT("messages"), MessagesArray);

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	// 5. Update Protection Counters & Lock BEFORE sending
	SessionRequestCount++;
	GlobalSessionRequestCount++;
	LastRequestRealtimeSeconds = Now;
	GlobalLastRequestTimestamp = Now;
	bIsRequestInFlight = true;
	bGlobalRequestInFlight = true;
	LastSentPrompt = TrimmedPrompt;
	LastSentPromptTimestamp = Now;

	// 6. Create and dispatch the HTTP POST Request
	ActiveHttpRequest = FHttpModule::Get().CreateRequest();
	ActiveHttpRequest->SetURL(ApiEndpointUrl);
	ActiveHttpRequest->SetVerb(TEXT("POST"));
	ActiveHttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	ActiveHttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey.TrimStartAndEnd()));
	ActiveHttpRequest->SetContentAsString(RequestBody);

	ActiveHttpRequest->OnProcessRequestComplete().BindUObject(this, &UMetaAIComponent::OnChatCompletionResponseReceived);

	PrintScreenDebug(
		FString::Printf(TEXT("[MetaAI Request %d/%d] Sending World State & Directive -> %s"),
			GlobalSessionRequestCount, MaxRequestsPerSession, *ModelName),
		FColor::Yellow,
		ScreenMessageDuration
	);

	ActiveHttpRequest->ProcessRequest();
}

void UMetaAIComponent::FindMatchingActorsForTarget(const FString& TargetIdentifier, TArray<AActor*>& OutMatchingActors) const
{
	OutMatchingActors.Reset();
	UWorld* World = GetWorld();
	if (!World || TargetIdentifier.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	const FString TrimmedTarget = TargetIdentifier.TrimStartAndEnd();

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Candidate = *It;
		if (!Candidate)
		{
			continue;
		}

		if (const USpaceAIIdentityComponent* IdentityComp = Candidate->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			if (IdentityComp->bAllowAIToModifyPhysics && IdentityComp->MatchesAITargetName(TrimmedTarget))
			{
				OutMatchingActors.AddUnique(Candidate);
				continue;
			}
		}

		// Also check unlabeled actors if wildcard or name matches
		const bool bHasObjectComp = Candidate->FindComponentByClass<USpaceObjectComponent>() != nullptr;
		const bool bHasFloatComp = Candidate->FindComponentByClass<USpaceFloatingComponent>() != nullptr;
		if (!bHasObjectComp && !bHasFloatComp)
		{
			continue;
		}

		if (TrimmedTarget.Equals(TEXT("ALL"), ESearchCase::IgnoreCase) ||
			(bHasObjectComp && (TrimmedTarget.Equals(TEXT("ALL_OBJECTS"), ESearchCase::IgnoreCase) || TrimmedTarget.Equals(TEXT("ALL_PROPS"), ESearchCase::IgnoreCase))) ||
			(bHasFloatComp && (TrimmedTarget.Equals(TEXT("PLAYER"), ESearchCase::IgnoreCase) || TrimmedTarget.Equals(TEXT("TRAINEE"), ESearchCase::IgnoreCase))))
		{
			OutMatchingActors.AddUnique(Candidate);
			continue;
		}

		const FString CandidateLabel = Candidate->GetActorNameOrLabel();
		if (CandidateLabel.Equals(TrimmedTarget, ESearchCase::IgnoreCase) ||
			Candidate->GetName().Equals(TrimmedTarget, ESearchCase::IgnoreCase) ||
			CandidateLabel.Contains(TrimmedTarget, ESearchCase::IgnoreCase))
		{
			OutMatchingActors.AddUnique(Candidate);
		}
	}
}

bool UMetaAIComponent::ApplySinglePhysicsCommandToActor(AActor* TargetActor, const TSharedPtr<FJsonObject>& CmdObj, FString& OutAppliedSummary, TSharedPtr<FJsonObject>& OutBeforeAfterLogObj)
{
	if (!TargetActor || !CmdObj.IsValid())
	{
		return false;
	}

	const USpaceAIIdentityComponent* IdentityComp = TargetActor->FindComponentByClass<USpaceAIIdentityComponent>();
	if (IdentityComp && !IdentityComp->bAllowAIToModifyPhysics)
	{
		return false;
	}

	USpaceObjectComponent* SpaceObj = TargetActor->FindComponentByClass<USpaceObjectComponent>();
	USpaceFloatingComponent* FloatComp = TargetActor->FindComponentByClass<USpaceFloatingComponent>();

	// Capture BEFORE state (Size/Scale, Bounding Box Dimensions in cm, Mass, Spin, MaxSpeed)
	const FVector BeforeScaleVec = TargetActor->GetActorScale3D();
	const FVector BeforeBoundsCm = TargetActor->GetComponentsBoundingBox(true).GetSize();
	const FString BeforeSizeStr = FString::Printf(
		TEXT("%.2fx (%.0fx%.0fx%.0f cm)"),
		BeforeScaleVec.X, BeforeBoundsCm.X, BeforeBoundsCm.Y, BeforeBoundsCm.Z
	);

	const float BeforeMassKg = SpaceObj ? SpaceObj->MassInKg : (FloatComp ? FloatComp->TraineeBodyMassKg : 0.0f);
	const float BeforeSpinDeg = SpaceObj ? SpaceObj->SelfRotationSpeed : 0.0f;
	const float BeforeMaxSpeed = SpaceObj ? SpaceObj->MaxAllowedSpeed : (FloatComp ? FloatComp->ZeroGMaxFlySpeed : 0.0f);

	TArray<FString> ChangesList;

	// =========================================================================
	// 0. Apply Object Scale / Size Changes (scale, size_scale, set_scale, scale_xyz)
	// =========================================================================
	{
		double ScaleVal = 1.0;
		const TArray<TSharedPtr<FJsonValue>>* ScaleXYZArray = nullptr;
		if (CmdObj->TryGetArrayField(TEXT("scale_xyz"), ScaleXYZArray) && ScaleXYZArray && ScaleXYZArray->Num() >= 3)
		{
			const FVector NewScale(
				FMath::Clamp(static_cast<float>((*ScaleXYZArray)[0]->AsNumber()), 0.15f, 5.0f),
				FMath::Clamp(static_cast<float>((*ScaleXYZArray)[1]->AsNumber()), 0.15f, 5.0f),
				FMath::Clamp(static_cast<float>((*ScaleXYZArray)[2]->AsNumber()), 0.15f, 5.0f)
			);
			TargetActor->SetActorScale3D(NewScale);
			const FVector NewBounds = TargetActor->GetComponentsBoundingBox(true).GetSize();
			ChangesList.Add(FString::Printf(
				TEXT("Size: %s -> %.2fx (%.0fx%.0fx%.0f cm)"),
				*BeforeSizeStr, NewScale.X, NewBounds.X, NewBounds.Y, NewBounds.Z
			));
		}
		else if (CmdObj->TryGetNumberField(TEXT("scale"), ScaleVal) ||
				 CmdObj->TryGetNumberField(TEXT("size_scale"), ScaleVal) ||
				 CmdObj->TryGetNumberField(TEXT("set_scale"), ScaleVal))
		{
			const float ClampedScale = FMath::Clamp(static_cast<float>(ScaleVal), 0.15f, 5.0f);
			if (!FMath::IsNearlyEqual(ClampedScale, BeforeScaleVec.X, 0.01f))
			{
				TargetActor->SetActorScale3D(FVector(ClampedScale));
			}
			const FVector NewBounds = TargetActor->GetComponentsBoundingBox(true).GetSize();
			ChangesList.Add(FString::Printf(
				TEXT("Size: %s -> %.2fx (%.0fx%.0fx%.0f cm)"),
				*BeforeSizeStr, ClampedScale, NewBounds.X, NewBounds.Y, NewBounds.Z
			));
		}
	}

	// =========================================================================
	// A. Apply Space Object Physics Changes (USpaceObjectComponent)
	// =========================================================================
	if (SpaceObj)
	{
		double NumVal = 0.0;
		bool bBoolVal = false;
		FString StrVal;

		// 1. Weight Preset ("FeatherLight", "MediumProp", "HeavyCargo")
		if (CmdObj->TryGetStringField(TEXT("weight_preset"), StrVal))
		{
			if (StrVal.Contains(TEXT("Feather"), ESearchCase::IgnoreCase) || StrVal.Contains(TEXT("Light"), ESearchCase::IgnoreCase))
			{
				SpaceObj->SetObjectMassInKg(0.25f);
				SpaceObj->SelfRotationSpeed = 28.0f;
				ChangesList.Add(FString::Printf(TEXT("Preset=FeatherLight (Mass: %.2fkg -> 0.25kg)"), BeforeMassKg));
			}
			else if (StrVal.Contains(TEXT("Medium"), ESearchCase::IgnoreCase))
			{
				SpaceObj->SetObjectMassInKg(8.0f);
				SpaceObj->SelfRotationSpeed = 15.0f;
				ChangesList.Add(FString::Printf(TEXT("Preset=MediumProp (Mass: %.2fkg -> 8.00kg)"), BeforeMassKg));
			}
			else if (StrVal.Contains(TEXT("Heavy"), ESearchCase::IgnoreCase))
			{
				SpaceObj->SetObjectMassInKg(65.0f);
				SpaceObj->SelfRotationSpeed = 6.0f;
				ChangesList.Add(FString::Printf(TEXT("Preset=HeavyCargo (Mass: %.2fkg -> 65.00kg)"), BeforeMassKg));
			}
		}

		// 2. Mass in Kilograms (mass_kg or set_mass_kg)
		if (CmdObj->TryGetNumberField(TEXT("mass_kg"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_mass_kg"), NumVal))
		{
			const float NewMass = FMath::Clamp(static_cast<float>(NumVal), 0.01f, 5000.0f);
			SpaceObj->SetObjectMassInKg(NewMass);
			ChangesList.Add(FString::Printf(TEXT("Mass: %.2fkg -> %.2fkg"), BeforeMassKg, NewMass));
		}

		// 3. Zero-G State (zero_g or set_zero_g)
		if (CmdObj->TryGetBoolField(TEXT("zero_g"), bBoolVal) || CmdObj->TryGetBoolField(TEXT("set_zero_g"), bBoolVal))
		{
			const bool bOldZeroG = SpaceObj->IsZeroGEnabled();
			SpaceObj->SetZeroGEnabled(bBoolVal);
			ChangesList.Add(FString::Printf(TEXT("ZeroG: %s -> %s"), bOldZeroG ? TEXT("ON") : TEXT("OFF"), bBoolVal ? TEXT("ON") : TEXT("OFF")));
		}

		// 4. Autonomous Rotation / Spin Speed (spin_speed or set_spin_speed)
		if (CmdObj->TryGetBoolField(TEXT("enable_spin"), bBoolVal))
		{
			SpaceObj->bEnableAutonomousRotation = bBoolVal;
			ChangesList.Add(FString::Printf(TEXT("SpinEnabled=%s"), bBoolVal ? TEXT("true") : TEXT("false")));
		}

		if (CmdObj->TryGetNumberField(TEXT("spin_speed"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_spin_speed"), NumVal))
		{
			const float NewSpin = FMath::Clamp(static_cast<float>(NumVal), 0.0f, 360.0f);
			SpaceObj->SelfRotationSpeed = NewSpin;
			SpaceObj->bEnableAutonomousRotation = (NewSpin > 0.0f);
			if (UPrimitiveComponent* Prim = SpaceObj->GetPrimaryPrimitive())
			{
				if (Prim->IsSimulatingPhysics())
				{
					const FVector SpinAxis = Prim->GetPhysicsAngularVelocityInDegrees().IsNearlyZero()
						? FVector(0.35f, 0.5f, 0.8f).GetSafeNormal()
						: Prim->GetPhysicsAngularVelocityInDegrees().GetSafeNormal();
					Prim->SetPhysicsAngularVelocityInDegrees(SpinAxis * NewSpin, false);
				}
			}
			ChangesList.Add(FString::Printf(TEXT("Spin: %.1fdeg/s -> %.1fdeg/s"), BeforeSpinDeg, NewSpin));
		}

		// 5. Max Allowed Floating Speed (max_speed or set_max_speed)
		if (CmdObj->TryGetNumberField(TEXT("max_speed"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_max_speed"), NumVal))
		{
			SpaceObj->MaxAllowedSpeed = FMath::Clamp(static_cast<float>(NumVal), 10.0f, 3000.0f);
			ChangesList.Add(FString::Printf(TEXT("MaxSpeed: %.0f -> %.0f"), BeforeMaxSpeed, SpaceObj->MaxAllowedSpeed));
		}

		// 6. Linear & Angular Damping (linear_damping / angular_damping)
		if (CmdObj->TryGetNumberField(TEXT("linear_damping"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_linear_damping"), NumVal))
		{
			const float OldLinDamp = SpaceObj->ZeroGLinearDamping;
			SpaceObj->ZeroGLinearDamping = FMath::Clamp(static_cast<float>(NumVal), 0.0f, 10.0f);
			if (UPrimitiveComponent* Prim = SpaceObj->GetPrimaryPrimitive())
			{
				Prim->SetLinearDamping(SpaceObj->ZeroGLinearDamping);
			}
			ChangesList.Add(FString::Printf(TEXT("LinDamp: %.2f -> %.2f"), OldLinDamp, SpaceObj->ZeroGLinearDamping));
		}

		if (CmdObj->TryGetNumberField(TEXT("angular_damping"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_angular_damping"), NumVal))
		{
			const float OldAngDamp = SpaceObj->ZeroGAngularDamping;
			SpaceObj->ZeroGAngularDamping = FMath::Clamp(static_cast<float>(NumVal), 0.0f, 10.0f);
			if (UPrimitiveComponent* Prim = SpaceObj->GetPrimaryPrimitive())
			{
				Prim->SetAngularDamping(SpaceObj->ZeroGAngularDamping);
			}
			ChangesList.Add(FString::Printf(TEXT("AngDamp: %.2f -> %.2f"), OldAngDamp, SpaceObj->ZeroGAngularDamping));
		}

		// 7. Wall Bounce Elasticity (bounce or set_bounce)
		if (CmdObj->TryGetNumberField(TEXT("bounce"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_bounce"), NumVal))
		{
			const float OldBounce = SpaceObj->SurfaceBounceCoefficient;
			SpaceObj->SurfaceBounceCoefficient = FMath::Clamp(static_cast<float>(NumVal), 0.0f, 1.0f);
			ChangesList.Add(FString::Printf(TEXT("Bounce: %.2f -> %.2f"), OldBounce, SpaceObj->SurfaceBounceCoefficient));
		}

		// 8. Micro-Gravity Wave Amplitude (wave_amplitude)
		if (CmdObj->TryGetNumberField(TEXT("wave_amplitude"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_wave_amplitude"), NumVal))
		{
			const float Amp = FMath::Clamp(static_cast<float>(NumVal), 0.0f, 100.0f);
			SpaceObj->WaveAmplitude = FVector(Amp * 0.5f, Amp * 0.5f, Amp);
			SpaceObj->bEnableMicroGravityWave = (Amp > 0.01f);
			ChangesList.Add(FString::Printf(TEXT("WaveAmp=%.1fcm"), Amp));
		}

		// 9. Physical Movement Commands (stop_motion, push_toward_player, repel_from_player, push_velocity)
		const bool bCanMoveObject = !IdentityComp || IdentityComp->bAllowAIToPushOrMoveObject;
		if (bCanMoveObject && SpaceObj->GetPrimaryPrimitive())
		{
			UPrimitiveComponent* Prim = SpaceObj->GetPrimaryPrimitive();

			if (CmdObj->TryGetBoolField(TEXT("stop_motion"), bBoolVal) && bBoolVal)
			{
				if (Prim->IsSimulatingPhysics())
				{
					Prim->SetPhysicsLinearVelocity(FVector::ZeroVector, false);
					Prim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector, false);
				}
				ChangesList.Add(TEXT("StoppedMotion"));
			}

			double PushSpeed = 160.0;
			CmdObj->TryGetNumberField(TEXT("push_speed"), PushSpeed);

			if (CmdObj->TryGetBoolField(TEXT("push_toward_player"), bBoolVal) && bBoolVal)
			{
				if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0))
				{
					const FVector DirToPlayer = (PlayerChar->GetActorLocation() - TargetActor->GetActorLocation()).GetSafeNormal();
					SpaceObj->ApplySpacePush(DirToPlayer * static_cast<float>(PushSpeed), true);
					ChangesList.Add(FString::Printf(TEXT("PushedTowardPlayer(%.0fcm/s)"), PushSpeed));
				}
			}
			else if (CmdObj->TryGetBoolField(TEXT("repel_from_player"), bBoolVal) && bBoolVal)
			{
				if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0))
				{
					const FVector DirAway = (TargetActor->GetActorLocation() - PlayerChar->GetActorLocation()).GetSafeNormal();
					SpaceObj->ApplySpacePush(DirAway * static_cast<float>(PushSpeed), true);
					ChangesList.Add(FString::Printf(TEXT("RepelledFromPlayer(%.0fcm/s)"), PushSpeed));
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* VelArray = nullptr;
			if (CmdObj->TryGetArrayField(TEXT("push_velocity"), VelArray) && VelArray && VelArray->Num() >= 3)
			{
				const FVector PushVec(
					(*VelArray)[0]->AsNumber(),
					(*VelArray)[1]->AsNumber(),
					(*VelArray)[2]->AsNumber()
				);
				if (!PushVec.IsNearlyZero())
				{
					SpaceObj->ApplySpacePush(PushVec, true);
					ChangesList.Add(FString::Printf(TEXT("PushVel=[%.0f,%.0f,%.0f]"), PushVec.X, PushVec.Y, PushVec.Z));
				}
			}
		}
	}

	// =========================================================================
	// B. Apply Player / Astronaut Physics Changes (USpaceFloatingComponent & USpaceGrabSnapComponent)
	// =========================================================================
	if (FloatComp)
	{
		double NumVal = 0.0;
		bool bBoolVal = false;

		if (CmdObj->TryGetNumberField(TEXT("player_mass_kg"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_player_mass_kg"), NumVal))
		{
			const float OldPlayerMass = FloatComp->TraineeBodyMassKg;
			FloatComp->TraineeBodyMassKg = FMath::Clamp(static_cast<float>(NumVal), 10.0f, 500.0f);
			ChangesList.Add(FString::Printf(TEXT("PlayerMass: %.1fkg -> %.1fkg"), OldPlayerMass, FloatComp->TraineeBodyMassKg));
		}

		if (CmdObj->TryGetNumberField(TEXT("player_jetpack_accel"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_player_jetpack_accel"), NumVal) ||
			CmdObj->TryGetNumberField(TEXT("player_reach_distance"), NumVal))
		{
			const float OldReach = FloatComp->HandReachDistance;
			FloatComp->HandReachDistance = FMath::Clamp(static_cast<float>(NumVal), 50.0f, 1000.0f);
			ChangesList.Add(FString::Printf(TEXT("PlayerReach: %.0f -> %.0f"), OldReach, FloatComp->HandReachDistance));
		}

		if (CmdObj->TryGetNumberField(TEXT("player_max_speed"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_player_max_speed"), NumVal))
		{
			const float OldSpeed = FloatComp->ZeroGMaxFlySpeed;
			FloatComp->ZeroGMaxFlySpeed = FMath::Clamp(static_cast<float>(NumVal), 50.0f, 3000.0f);
			ChangesList.Add(FString::Printf(TEXT("PlayerMaxSpeed: %.0f -> %.0f"), OldSpeed, FloatComp->ZeroGMaxFlySpeed));
		}

		if (CmdObj->TryGetNumberField(TEXT("player_launch_strength"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_player_push_strength"), NumVal))
		{
			const float OldPush = FloatComp->PushImpulseStrength;
			FloatComp->PushImpulseStrength = FMath::Clamp(static_cast<float>(NumVal), 50.0f, 3000.0f);
			ChangesList.Add(FString::Printf(TEXT("WallLaunchPower: %.0f -> %.0f"), OldPush, FloatComp->PushImpulseStrength));
		}

		if (CmdObj->TryGetNumberField(TEXT("player_pull_strength"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_player_pull_strength"), NumVal))
		{
			const float OldPull = FloatComp->PullImpulseStrength;
			FloatComp->PullImpulseStrength = FMath::Clamp(static_cast<float>(NumVal), 50.0f, 3000.0f);
			ChangesList.Add(FString::Printf(TEXT("WallPullPower: %.0f -> %.0f"), OldPull, FloatComp->PullImpulseStrength));
		}

		if (CmdObj->TryGetBoolField(TEXT("player_zero_g"), bBoolVal))
		{
			FloatComp->SetFloatingEnabled(bBoolVal);
			ChangesList.Add(FString::Printf(TEXT("PlayerZeroG=%s"), bBoolVal ? TEXT("ON") : TEXT("OFF")));
		}
	}

	if (USpaceGrabSnapComponent* GrabComp = TargetActor->FindComponentByClass<USpaceGrabSnapComponent>())
	{
		double NumVal = 0.0;
		if (CmdObj->TryGetNumberField(TEXT("player_throw_speed"), NumVal) || CmdObj->TryGetNumberField(TEXT("set_throw_speed"), NumVal))
		{
			const float OldThrow = GrabComp->ThrowLaunchSpeed;
			GrabComp->ThrowLaunchSpeed = FMath::Clamp(static_cast<float>(NumVal), 50.0f, 3000.0f);
			ChangesList.Add(FString::Printf(TEXT("ThrowSpeed: %.0f -> %.0f"), OldThrow, GrabComp->ThrowLaunchSpeed));
		}
	}

	if (ChangesList.Num() > 0)
	{
		// Capture AFTER state (Size/Scale, Bounding Box Dimensions in cm, Mass, Spin, MaxSpeed)
		const FVector AfterScaleVec = TargetActor->GetActorScale3D();
		const FVector AfterBoundsCm = TargetActor->GetComponentsBoundingBox(true).GetSize();
		const FString AfterSizeStr = FString::Printf(
			TEXT("%.2fx (%.0fx%.0fx%.0f cm)"),
			AfterScaleVec.X, AfterBoundsCm.X, AfterBoundsCm.Y, AfterBoundsCm.Z
		);

		const float AfterMassKg = SpaceObj ? SpaceObj->MassInKg : (FloatComp ? FloatComp->TraineeBodyMassKg : 0.0f);
		const float AfterSpinDeg = SpaceObj ? SpaceObj->SelfRotationSpeed : 0.0f;
		const float AfterMaxSpeed = SpaceObj ? SpaceObj->MaxAllowedSpeed : (FloatComp ? FloatComp->ZeroGMaxFlySpeed : 0.0f);

		// Ensure Size Before -> After is always included in the summary so the user can see the object's size before and after AI edited it!
		bool bAlreadyHasSizeEntry = false;
		for (const FString& Entry : ChangesList)
		{
			if (Entry.StartsWith(TEXT("Size:")))
			{
				bAlreadyHasSizeEntry = true;
				break;
			}
		}
		if (!bAlreadyHasSizeEntry)
		{
			ChangesList.Insert(FString::Printf(TEXT("Size: %s -> %s"), *BeforeSizeStr, *AfterSizeStr), 0);
		}

		OutAppliedSummary = FString::Join(ChangesList, TEXT(", "));

		const FString DisplayName = IdentityComp ? IdentityComp->GetEffectiveEntityName() : TargetActor->GetActorNameOrLabel();
		const FString RoleStr = FloatComp ? TEXT("AstronautPlayer") : TEXT("SpaceProp");

		OutBeforeAfterLogObj = MakeShareable(new FJsonObject());
		OutBeforeAfterLogObj->SetStringField(TEXT("object_name"), DisplayName);
		OutBeforeAfterLogObj->SetStringField(TEXT("object_role"), RoleStr);
		OutBeforeAfterLogObj->SetStringField(TEXT("size_before"), BeforeSizeStr);
		OutBeforeAfterLogObj->SetStringField(TEXT("size_after"), AfterSizeStr);
		OutBeforeAfterLogObj->SetNumberField(TEXT("mass_before_kg"), BeforeMassKg);
		OutBeforeAfterLogObj->SetNumberField(TEXT("mass_after_kg"), AfterMassKg);
		OutBeforeAfterLogObj->SetNumberField(TEXT("spin_before_deg"), BeforeSpinDeg);
		OutBeforeAfterLogObj->SetNumberField(TEXT("spin_after_deg"), AfterSpinDeg);
		OutBeforeAfterLogObj->SetNumberField(TEXT("max_speed_before"), BeforeMaxSpeed);
		OutBeforeAfterLogObj->SetNumberField(TEXT("max_speed_after"), AfterMaxSpeed);
		OutBeforeAfterLogObj->SetStringField(TEXT("changes_summary"), OutAppliedSummary);

		return true;
	}

	return false;
}

int32 UMetaAIComponent::ExecuteAIPhysicsResponseJson(const FString& AIJsonOrText, FString& OutExtractedMessage)
{
	OutExtractedMessage = AIJsonOrText;
	LastReadablePhysicsSummary = TEXT("No physics changes required.");
	LastObjectEditsArray.Reset();
	int32 ModifiedActorsCount = 0;
	TArray<FString> ReadableRows;

	bIsAwaitingBreakdownAIResponse = false;

	// Strip optional <think>...</think> reasoning tokens from reasoning models (e.g. muse-spark-1.3)
	FString CleanJson = AIJsonOrText.TrimStartAndEnd();
	int32 ThinkStart = CleanJson.Find(TEXT("<think>"), ESearchCase::IgnoreCase);
	while (ThinkStart != INDEX_NONE)
	{
		int32 ThinkEnd = CleanJson.Find(TEXT("</think>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ThinkStart);
		if (ThinkEnd != INDEX_NONE)
		{
			CleanJson.RemoveAt(ThinkStart, (ThinkEnd + 8) - ThinkStart);
		}
		else
		{
			CleanJson.RemoveAt(ThinkStart, CleanJson.Len() - ThinkStart);
			break;
		}
		ThinkStart = CleanJson.Find(TEXT("<think>"), ESearchCase::IgnoreCase);
	}

	// Strip optional ```json ... ``` markdown fences if present
	CleanJson = CleanJson.TrimStartAndEnd();
	int32 FirstBrace = INDEX_NONE;
	int32 LastBrace = INDEX_NONE;
	if (CleanJson.FindChar(TEXT('{'), FirstBrace) && CleanJson.FindLastChar(TEXT('}'), LastBrace) && LastBrace > FirstBrace)
	{
		CleanJson = CleanJson.Mid(FirstBrace, (LastBrace - FirstBrace) + 1);
	}

	TSharedPtr<FJsonObject> RootObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CleanJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		return 0;
	}

	RootObj->TryGetStringField(TEXT("message"), OutExtractedMessage);
	if (!OutExtractedMessage.IsEmpty())
	{
		CachedFlightDirectorAdvice = OutExtractedMessage;
	}

	// If this was a background precompute call, store the result into PrecomputedNextAIQuest without overriding active quest 1!
	if (bIsPrecomputingNextQuest)
	{
		bIsPrecomputingNextQuest = false;
		bHasPrecomputedNextQuest = true;

		const TSharedPtr<FJsonObject>* PrecomputeQuestPtr = nullptr;
		if (RootObj->TryGetObjectField(TEXT("ai_quest"), PrecomputeQuestPtr) && PrecomputeQuestPtr && (*PrecomputeQuestPtr).IsValid())
		{
			FString QTitle, QInstr;
			(*PrecomputeQuestPtr)->TryGetStringField(TEXT("quest_title"), QTitle);
			(*PrecomputeQuestPtr)->TryGetStringField(TEXT("quest_instruction"), QInstr);
			if (!QTitle.IsEmpty()) PrecomputedNextAIQuest.QuestTitle = QTitle;
			if (!QInstr.IsEmpty()) PrecomputedNextAIQuest.QuestInstruction = QInstr;
		}

		if (!OutExtractedMessage.IsEmpty())
		{
			PrecomputedNextAIQuest.PrecomputedFlightDirectorAdvice = OutExtractedMessage;
		}
		PrecomputedNextAIQuest.PrecomputedRiskLevel = CurrentAIRiskLevel;
		PrecomputedNextAIQuest.PrecomputedRiskScore = CurrentAIRiskScorePercent;
		PrecomputedNextAIQuest.PrecomputedRawAIJson = AIJsonOrText;

		PrintScreenDebug(
			FString::Printf(TEXT("[Meta AI Pipeline] Quest Prepared in Background! Ready for instant 0.00ms activation: '%s'"),
				*PrecomputedNextAIQuest.QuestTitle),
			FColor::Turquoise,
			6.0f
		);
		return 0;
	}

	// =========================================================================
	// 0. Execute Autonomous Meta AI Game Master Quest ("ai_quest")
	// =========================================================================
	const TSharedPtr<FJsonObject>* QuestObjPtr = nullptr;
	if (!bSuppressAIQuestParsing && RootObj->TryGetObjectField(TEXT("ai_quest"), QuestObjPtr) && QuestObjPtr && (*QuestObjPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& QuestObj = *QuestObjPtr;
		FString QTitle, QInstruction, QTargetActorStr, QReqItem;
		QuestObj->TryGetStringField(TEXT("quest_title"), QTitle);
		QuestObj->TryGetStringField(TEXT("quest_instruction"), QInstruction);
		QuestObj->TryGetStringField(TEXT("target_actor"), QTargetActorStr);
		QuestObj->TryGetStringField(TEXT("required_item"), QReqItem);

		if (!QTitle.TrimStartAndEnd().IsEmpty() || !QInstruction.TrimStartAndEnd().IsEmpty() || !QTargetActorStr.TrimStartAndEnd().IsEmpty())
		{
			// 1. Locate the exact target Actor chosen by Meta AI in the 3D world
			AActor* TargetActorForQuest = nullptr;
			if (!QTargetActorStr.TrimStartAndEnd().IsEmpty())
			{
				TArray<AActor*> MatchedQuestActors;
				FindMatchingActorsForTarget(QTargetActorStr, MatchedQuestActors);
				for (AActor* Cand : MatchedQuestActors)
				{
					if (Cand && Cand != UGameplayStatics::GetPlayerCharacter(GetWorld(), 0) && Cand != ActiveQuestMarkerActor && Cand != ActiveToolMarkerActor)
					{
						TargetActorForQuest = Cand;
						break;
					}
				}
			}
			if (!TargetActorForQuest)
			{
				TargetActorForQuest = PickNextBreakableActorInShip();
			}

			// Anti-repetition gate: the LLM tends to keep proposing the same system (e.g. pipe + valve wheel).
			// If it proposes a recently serviced target/system, C++ rotates to a fresh station system instead.
			if (!bHasActiveAIQuest && IsRecentlyUsedQuestTarget(TargetActorForQuest, QTargetActorStr))
			{
				AActor* FreshTarget = PickNextBreakableActorInShip();
				if (FreshTarget && FreshTarget != TargetActorForQuest)
				{
					TargetActorForQuest = FreshTarget;
					QReqItem.Reset();
					QTitle.Reset();
					QInstruction.Reset();
				}
			}

			// 2. Locate the exact required tool Actor chosen by Meta AI in the 3D world
			AActor* ToolActorForQuest = nullptr;
			if (!QReqItem.TrimStartAndEnd().IsEmpty())
			{
				TArray<AActor*> MatchedToolActors;
				FindMatchingActorsForTarget(QReqItem, MatchedToolActors);
				for (AActor* Cand : MatchedToolActors)
				{
					if (Cand && Cand != UGameplayStatics::GetPlayerCharacter(GetWorld(), 0) && Cand != TargetActorForQuest && Cand != ActiveQuestMarkerActor && Cand != ActiveToolMarkerActor)
					{
						ToolActorForQuest = Cand;
						break;
					}
				}
			}
			if (!ToolActorForQuest)
			{
				ToolActorForQuest = PickNextToolActorInShip(TargetActorForQuest);
			}

			const FString EffectiveTargetName = TargetActorForQuest ? TargetActorForQuest->GetActorNameOrLabel() : QTargetActorStr;
			const FString EffectiveToolName = ToolActorForQuest ? ToolActorForQuest->GetActorNameOrLabel() : QReqItem;

			const FVector TargetLoc = TargetActorForQuest
				? TargetActorForQuest->GetActorLocation()
				: (GetOwner() ? GetOwner()->GetActorLocation() + FVector(280.0f, 0.0f, 40.0f) : FVector::ZeroVector);

			if (!bHasActiveAIQuest)
			{
				CurrentQuestSource = TEXT("META_AI_AUTONOMOUS");
				CurrentQuestKey = MakeNormalizedQuestKey(EffectiveTargetName, EffectiveToolName, QTitle);
				CurrentQuestTitle = QTitle.IsEmpty() ? FString::Printf(TEXT("Emergency: %s"), *EffectiveTargetName) : QTitle;
				CurrentQuestWhatToDo = QInstruction.IsEmpty() ? FString::Printf(TEXT("Take %s and repair %s [E]"), *EffectiveToolName, *EffectiveTargetName) : QInstruction;
				CurrentQuestStartTimeSeconds = GetMissionElapsedTimeSeconds();
				CurrentQuestTargetName = EffectiveTargetName;
				CurrentQuestRequiredToolName = EffectiveToolName;
				ActiveQuestRequiredToolActor = ToolActorForQuest;
				bHasActiveAIQuest = true;
				RecordQuestTargetAndToolUsage(TargetActorForQuest, EffectiveTargetName, ToolActorForQuest, EffectiveToolName);
				SessionAssignedQuestKeys.Add(CurrentQuestKey);
				SessionAssignedQuestKeys.Add(MakeNormalizedQuestKey(EffectiveTargetName, EffectiveToolName));

				InterruptFlightDirectorVoice();
				TransmitFlightDirectorRadioComms(
					FString::Printf(TEXT("Flight Director: New emergency at %s. Retrieve the %s and press E at the target."),
						*EffectiveTargetName, *EffectiveToolName),
					7.0f);

				// Highlight Meta AI's target with the 3D Quest Box and floating beacon
				SpawnOrUpdate3DQuestMarker(
					TargetActorForQuest,
					TargetLoc,
					CurrentQuestTitle,
					CurrentQuestWhatToDo,
					EffectiveToolName
				);

				// Highlight Meta AI's required tool with the Cyan Tool Marker
				if (ToolActorForQuest)
				{
					SpawnOrUpdate3DToolMarker(ToolActorForQuest, EffectiveToolName, EffectiveTargetName);
				}
			}
			else
			{
				// ACTIVE QUEST LOCK: unsolicited AI quests never replace the current quest and never overwrite the
				// background pipeline buffer (that was the source of the endless "same quest again" loop).
				// Only adopt the AI's narrative text if it describes the SAME target the trainee is already repairing.
				if (TargetActorForQuest && TargetActorForQuest == ActiveQuestTargetActor.Get())
				{
					if (!QTitle.TrimStartAndEnd().IsEmpty()) CurrentQuestTitle = QTitle;
					if (!QInstruction.TrimStartAndEnd().IsEmpty()) CurrentQuestWhatToDo = QInstruction;
				}
			}

			// Sync Meta AI's autonomous quest into MySQL database catalog
			if (bSaveAndFetchQuestsFromDatabase)
			{
				SyncQuestToDatabaseCatalog(
					CurrentQuestKey,
					CurrentQuestSource,
					CurrentQuestTitle,
					CurrentQuestWhatToDo,
					EffectiveTargetName,
					EffectiveToolName,
					false,
					0.0f
				);
			}

			LogTrainingEventToDatabase(TEXT("meta_ai_quest_started"), EffectiveTargetName, MissionName);
			ReadableRows.Add(FString::Printf(TEXT("MetaAI_AutonomousQuest -> '%s' (Tool: '%s' -> Target: '%s')"),
				*CurrentQuestTitle, *EffectiveToolName, *EffectiveTargetName));
		}
	}
	else
	{
		FString DynamicAIMission;
		if (RootObj->TryGetStringField(TEXT("ai_mission"), DynamicAIMission) && !DynamicAIMission.TrimStartAndEnd().IsEmpty())
		{
			MissionName = DynamicAIMission.TrimStartAndEnd();
			ReadableRows.Add(FString::Printf(TEXT("AIMission -> '%s'"), *MissionName));
		}
	}

	// =========================================================================
	// 0B. Parse Meta AI Autonomous Trainee Performance Study & Evaluation ("trainee_evaluation")
	// =========================================================================
	const TSharedPtr<FJsonObject>* EvalObjPtr = nullptr;
	if (RootObj->TryGetObjectField(TEXT("trainee_evaluation"), EvalObjPtr) && EvalObjPtr && (*EvalObjPtr).IsValid())
	{
		const TSharedPtr<FJsonObject>& EvalObj = *EvalObjPtr;
		FString EvalGrade, EvalReadiness, EvalSummary;
		double EvalScore = -1.0;
		EvalObj->TryGetStringField(TEXT("grade"), EvalGrade);
		EvalObj->TryGetStringField(TEXT("flight_readiness"), EvalReadiness);
		EvalObj->TryGetStringField(TEXT("study_summary"), EvalSummary);
		if (EvalSummary.IsEmpty()) EvalObj->TryGetStringField(TEXT("summary"), EvalSummary);
		EvalObj->TryGetNumberField(TEXT("score"), EvalScore);

		if (!EvalGrade.TrimStartAndEnd().IsEmpty())
		{
			CurrentAIGrade = EvalGrade.TrimStartAndEnd().ToUpper();
		}
		if (!EvalReadiness.TrimStartAndEnd().IsEmpty())
		{
			CurrentAIFlightReadiness = EvalReadiness.TrimStartAndEnd().ToUpper();
		}
		if (!EvalSummary.TrimStartAndEnd().IsEmpty())
		{
			CurrentAIEvaluationSummary = EvalSummary.TrimStartAndEnd();
		}
		if (EvalScore >= 0.0)
		{
			CurrentAIFinalScore = FMath::Clamp(static_cast<float>(EvalScore), 0.0f, 100.0f);
		}

		PrintScreenDebug(
			FString::Printf(TEXT("[Meta AI Study] Grade: %s (%.0f pts) | Readiness: %s"),
				*CurrentAIGrade, CurrentAIFinalScore, *CurrentAIFlightReadiness),
			FColor::Cyan,
			7.0f
		);

		ReadableRows.Add(FString::Printf(TEXT("MetaAI_TraineeStudy -> Grade %s (%.0f pts), Readiness: %s"),
			*CurrentAIGrade, CurrentAIFinalScore, *CurrentAIFlightReadiness));
	}

	FString ParsedRiskLevel;
	double ParsedRiskScore = -1.0;
	const bool bHasRiskLevel = RootObj->TryGetStringField(TEXT("risk_level"), ParsedRiskLevel) && !ParsedRiskLevel.TrimStartAndEnd().IsEmpty();
	const bool bHasRiskScore = RootObj->TryGetNumberField(TEXT("risk_score"), ParsedRiskScore);
	if (bHasRiskLevel || bHasRiskScore)
	{
		if (bHasRiskLevel)
		{
			CurrentAIRiskLevel = ParsedRiskLevel.TrimStartAndEnd().ToUpper();
		}
		if (bHasRiskScore)
		{
			CurrentAIRiskScorePercent = FMath::Clamp(static_cast<float>(ParsedRiskScore), 0.0f, 100.0f);
		}
		else
		{
			if (CurrentAIRiskLevel.Contains(TEXT("CRIT"))) CurrentAIRiskScorePercent = 92.0f;
			else if (CurrentAIRiskLevel.Contains(TEXT("HIGH"))) CurrentAIRiskScorePercent = 75.0f;
			else if (CurrentAIRiskLevel.Contains(TEXT("MOD"))) CurrentAIRiskScorePercent = 45.0f;
			else CurrentAIRiskScorePercent = 15.0f;
		}

		ReadableRows.Add(FString::Printf(TEXT("AIRiskAssessment -> %s (%.0f%%)"), *CurrentAIRiskLevel, CurrentAIRiskScorePercent));

		if (bLetAIDetermineRiskLevelAutomatically)
		{
			ApplyAutonomousAIRiskResponse(CurrentAIRiskLevel, CurrentAIRiskScorePercent);
		}
	}

	// =========================================================================
	// 1. Execute Spaceship Events (Hull/Camera Shake, Obstacle Spawning, Hurling Objects at Player)
	// =========================================================================
	TSharedPtr<FJsonObject> ShipEventsObj = RootObj;
	const TSharedPtr<FJsonObject>* NestedShipEvents = nullptr;
	if (RootObj->TryGetObjectField(TEXT("ship_events"), NestedShipEvents) && NestedShipEvents && (*NestedShipEvents).IsValid())
	{
		ShipEventsObj = *NestedShipEvents;
	}

	if (ShipEventsObj.IsValid())
	{
		double ShakeIntensity = 0.0;
		double ShakeDuration = DefaultShipShakeDurationSeconds;
		int32 HurlExistingCount = 0;
		ShipEventsObj->TryGetNumberField(TEXT("hurl_existing_objects_count"), HurlExistingCount);

		if (ShipEventsObj->TryGetNumberField(TEXT("ship_shake_intensity"), ShakeIntensity) && ShakeIntensity > 0.05)
		{
			ShipEventsObj->TryGetNumberField(TEXT("ship_shake_duration"), ShakeDuration);
			TriggerShipTurbulenceAndCameraShake(
				static_cast<float>(ShakeIntensity),
				static_cast<float>(ShakeDuration),
				true,
				HurlExistingCount
			);
			ReadableRows.Add(FString::Printf(
				TEXT("SpaceshipShake -> Intensity=%.2f, Duration=%.1fs, HurledObjects=%d"),
				ShakeIntensity, ShakeDuration, HurlExistingCount
			));
			HurlExistingCount = 0;
		}

		int32 SpawnCount = 0;
		if (ShipEventsObj->TryGetNumberField(TEXT("spawn_obstacle_count"), SpawnCount) && SpawnCount > 0)
		{
			FString SpawnName = TEXT("AI_SpaceDebris");
			ShipEventsObj->TryGetStringField(TEXT("spawn_obstacle_name"), SpawnName);

			double SpawnMass = 12.0;
			ShipEventsObj->TryGetNumberField(TEXT("spawn_obstacle_mass_kg"), SpawnMass);

			double SpawnScale = 1.0;
			ShipEventsObj->TryGetNumberField(TEXT("spawn_obstacle_scale"), SpawnScale);

			bool bLaunchAtPlayer = true;
			ShipEventsObj->TryGetBoolField(TEXT("launch_obstacles_at_player"), bLaunchAtPlayer);

			const int32 ClampedSpawnCount = FMath::Clamp(SpawnCount, 1, 3);
			int32 ActualSpawned = 0;
			for (int32 i = 0; i < ClampedSpawnCount; ++i)
			{
				if (AActor* SpawnedActor = SpawnSpaceObstacleForPlayer(
					SpawnName,
					bLaunchAtPlayer,
					ObstacleLaunchSpeedTowardPlayer,
					static_cast<float>(SpawnMass),
					static_cast<float>(SpawnScale)))
				{
					ActualSpawned++;
					ModifiedActorsCount++;
				}
			}

			if (ActualSpawned > 0)
			{
				ReadableRows.Add(FString::Printf(
					TEXT("AISpawner -> Spawned %dx '%s' (Scale=%.2fx, Mass=%.1fkg, LaunchedAtPlayer=%s)"),
					ActualSpawned, *SpawnName, SpawnScale, SpawnMass, bLaunchAtPlayer ? TEXT("YES") : TEXT("NO")
				));
			}
		}

		if (HurlExistingCount > 0)
		{
			const int32 Hurled = LaunchNearbyObjectsTowardPlayer(HurlExistingCount, ObstacleLaunchSpeedTowardPlayer);
			if (Hurled > 0)
			{
				ReadableRows.Add(FString::Printf(TEXT("AIHazard -> Hurled %d floating objects toward Player"), Hurled));
			}
		}

		FString EventLightPreset;
		if (ShipEventsObj->TryGetStringField(TEXT("light_preset"), EventLightPreset) && !EventLightPreset.IsEmpty())
		{
			const int32 LightsChanged = ApplyShipLightingPreset(EventLightPreset, TEXT("ALL_LIGHTS"));
			if (LightsChanged > 0)
			{
				ReadableRows.Add(FString::Printf(TEXT("ShipLights (RectLight/RectLight2) -> Preset=%s"), *EventLightPreset));
			}
		}
	}

	// =========================================================================
	// 1B. Execute AI Lighting Commands (RectLight, RectLight2, or ALL_LIGHTS)
	// =========================================================================
	const TArray<TSharedPtr<FJsonValue>>* LightCommandsArray = nullptr;
	if (bAllowAIToControlShipLights && RootObj->TryGetArrayField(TEXT("light_commands"), LightCommandsArray) && LightCommandsArray)
	{
		for (const TSharedPtr<FJsonValue>& LightVal : *LightCommandsArray)
		{
			const TSharedPtr<FJsonObject> LightCmd = LightVal.IsValid() ? LightVal->AsObject() : nullptr;
			if (!LightCmd.IsValid())
			{
				continue;
			}

			FString TargetLight = TEXT("ALL_LIGHTS");
			LightCmd->TryGetStringField(TEXT("target"), TargetLight);

			FString PresetStr;
			if (LightCmd->TryGetStringField(TEXT("preset"), PresetStr) && !PresetStr.IsEmpty())
			{
				ApplyShipLightingPreset(PresetStr, TargetLight);
			}

			FLinearColor DesiredColor = FLinearColor(1.0f, 0.15f, 0.15f, 1.0f);
			bool bHasCustomColor = false;
			const TArray<TSharedPtr<FJsonValue>>* RgbArr = nullptr;
			if (LightCmd->TryGetArrayField(TEXT("color_rgb"), RgbArr) && RgbArr && RgbArr->Num() >= 3)
			{
				DesiredColor = FLinearColor(
					FMath::Clamp(static_cast<float>((*RgbArr)[0]->AsNumber()) / 255.0f, 0.0f, 1.0f),
					FMath::Clamp(static_cast<float>((*RgbArr)[1]->AsNumber()) / 255.0f, 0.0f, 1.0f),
					FMath::Clamp(static_cast<float>((*RgbArr)[2]->AsNumber()) / 255.0f, 0.0f, 1.0f),
					1.0f
				);
				bHasCustomColor = true;
			}

			double IntMult = 1.0;
			const bool bHasMult = LightCmd->TryGetNumberField(TEXT("intensity_multiplier"), IntMult);

			FString ModeStr = TEXT("solid");
			const bool bHasMode = LightCmd->TryGetStringField(TEXT("mode"), ModeStr);

			bool bIsOn = true;
			if (LightCmd->TryGetBoolField(TEXT("is_on"), bIsOn) && !bIsOn)
			{
				ModeStr = TEXT("off");
				IntMult = 0.0;
			}

			double SpeedVal = 2.0;
			LightCmd->TryGetNumberField(TEXT("speed"), SpeedVal);

			if (bHasCustomColor || bHasMult || bHasMode || !bIsOn)
			{
				ControlShipLights(
					TargetLight,
					bHasCustomColor ? DesiredColor : (TrackedShipLights.Num() > 0 ? TrackedShipLights[0].TargetColor : FLinearColor::White),
					static_cast<float>(IntMult),
					ModeStr,
					static_cast<float>(SpeedVal)
				);
			}

			ReadableRows.Add(FString::Printf(
				TEXT("Lights [%s] -> Preset=%s, Mode=%s, Intensity=%.1fx, RGB=[%d,%d,%d]"),
				*TargetLight,
				PresetStr.IsEmpty() ? TEXT("Custom") : *PresetStr,
				*ModeStr,
				IntMult,
				FMath::RoundToInt(DesiredColor.R * 255.0f),
				FMath::RoundToInt(DesiredColor.G * 255.0f),
				FMath::RoundToInt(DesiredColor.B * 255.0f)
			));
		}
	}

	// =========================================================================
	// 2. Execute Per-Actor Physics & Scale Commands
	// =========================================================================
	const TArray<TSharedPtr<FJsonValue>>* CommandsArray = nullptr;
	if (!RootObj->TryGetArrayField(TEXT("physics_commands"), CommandsArray) || !CommandsArray)
	{
		if (ReadableRows.Num() > 0)
		{
			LastReadablePhysicsSummary = FString::Join(ReadableRows, TEXT(" | "));
		}
		return ModifiedActorsCount;
	}

	for (const TSharedPtr<FJsonValue>& Val : *CommandsArray)
	{
		const TSharedPtr<FJsonObject> CmdObj = Val.IsValid() ? Val->AsObject() : nullptr;
		if (!CmdObj.IsValid())
		{
			continue;
		}

		FString TargetStr = TEXT("ALL_OBJECTS");
		CmdObj->TryGetStringField(TEXT("target"), TargetStr);

		TArray<AActor*> MatchingActors;
		FindMatchingActorsForTarget(TargetStr, MatchingActors);

		for (AActor* TargetActor : MatchingActors)
		{
			FString Summary;
			TSharedPtr<FJsonObject> BeforeAfterLogObj;
			if (ApplySinglePhysicsCommandToActor(TargetActor, CmdObj, Summary, BeforeAfterLogObj))
			{
				ModifiedActorsCount++;
				const USpaceAIIdentityComponent* IdComp = TargetActor->FindComponentByClass<USpaceAIIdentityComponent>();
				const FString DisplayName = IdComp ? IdComp->GetEffectiveEntityName() : TargetActor->GetActorNameOrLabel();

				if (BeforeAfterLogObj.IsValid())
				{
					LastObjectEditsArray.Add(MakeShareable(new FJsonValueObject(BeforeAfterLogObj)));
				}

				ReadableRows.Add(FString::Printf(TEXT("%s -> %s"), *DisplayName, *Summary));

				PrintScreenDebug(
					FString::Printf(TEXT("[MetaAI Physics Control -> '%s']: %s"), *DisplayName, *Summary),
					FColor::Emerald,
					ScreenMessageDuration
				);

				OnAIPhysicsCommandExecuted.Broadcast(TargetActor, DisplayName, Summary);
			}
		}
	}

	if (ReadableRows.Num() > 0)
	{
		LastReadablePhysicsSummary = FString::Join(ReadableRows, TEXT(" | "));
	}

	return ModifiedActorsCount;
}

static FString GetLogicalQuestVerbAndInstruction(AActor* ToolActor, const FString& ToolName, const FString& TargetName, FString& OutInstruction)
{
	FString CombinedInfo = ToolName;
	if (ToolActor)
	{
		if (const USpaceAIIdentityComponent* IdComp = ToolActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			CombinedInfo += TEXT(" ") + IdComp->DescriptionAndPurpose + TEXT(" ") + IdComp->AIPhysicsControlHint;
		}
	}
	CombinedInfo = CombinedInfo.ToLower();

	if (CombinedInfo.Contains(TEXT("tablet")) || CombinedInfo.Contains(TEXT("scan")) || CombinedInfo.Contains(TEXT("diagnos")) || CombinedInfo.Contains(TEXT("screen")))
	{
		OutInstruction = FString::Printf(TEXT("Take '%s' (Cyan Box) and Scan telemetry at '%s' Box & Press [E]!"), *ToolName, *TargetName);
		return TEXT("Scan");
	}
	if (CombinedInfo.Contains(TEXT("scrap")) || CombinedInfo.Contains(TEXT("debris")) || CombinedInfo.Contains(TEXT("trash")) || CombinedInfo.Contains(TEXT("rubble")) || CombinedInfo.Contains(TEXT("waste")))
	{
		OutInstruction = FString::Printf(TEXT("Clear '%s' (Cyan Box) from '%s' sector and Press [E]!"), *ToolName, *TargetName);
		return TEXT("Clear/Dispose");
	}
	if (CombinedInfo.Contains(TEXT("foam")) || CombinedInfo.Contains(TEXT("extinguish")) || CombinedInfo.Contains(TEXT("co2")) || CombinedInfo.Contains(TEXT("fire")))
	{
		OutInstruction = FString::Printf(TEXT("Take '%s' (Cyan Box) and Extinguish hazard at '%s' Box & Press [E]!"), *ToolName, *TargetName);
		return TEXT("Extinguish");
	}
	if (CombinedInfo.Contains(TEXT("torch")) || CombinedInfo.Contains(TEXT("weld")) || CombinedInfo.Contains(TEXT("cutter")) || CombinedInfo.Contains(TEXT("laser")))
	{
		OutInstruction = FString::Printf(TEXT("Take '%s' (Cyan Box) and Weld damaged hull at '%s' Box & Press [E]!"), *ToolName, *TargetName);
		return TEXT("Weld/Cut");
	}
	if (CombinedInfo.Contains(TEXT("valve")) || CombinedInfo.Contains(TEXT("wheel")) || CombinedInfo.Contains(TEXT("turn")))
	{
		OutInstruction = FString::Printf(TEXT("Install '%s' (Cyan Box) on '%s' Box and Press [E] to turn!"), *ToolName, *TargetName);
		return TEXT("Install/Turn");
	}
	if (CombinedInfo.Contains(TEXT("battery")) || CombinedInfo.Contains(TEXT("cell")) || CombinedInfo.Contains(TEXT("power")))
	{
		OutInstruction = FString::Printf(TEXT("Take '%s' (Cyan Box) and Insert into '%s' Box & Press [E]!"), *ToolName, *TargetName);
		return TEXT("Recharge/Insert");
	}
	if (CombinedInfo.Contains(TEXT("wrench")) || CombinedInfo.Contains(TEXT("spanner")) || CombinedInfo.Contains(TEXT("digi_wrench")))
	{
		OutInstruction = FString::Printf(TEXT("Take '%s' (Cyan Box) and Calibrate '%s' Box & Press [E]!"), *ToolName, *TargetName);
		return TEXT("Calibrate");
	}

	OutInstruction = FString::Printf(TEXT("Take '%s' (Cyan Box) to '%s' Box and Press [E]!"), *ToolName, *TargetName);
	return TEXT("Use");
}

FString UMetaAIComponent::GenerateSmartFallbackPhysicsResponse() const
{
	// Analyzes all USpaceAIIdentityComponent descriptions AND current mission timer to generate coaching advice + physics_commands JSON!
	TSharedPtr<FJsonObject> RootObj = MakeShareable(new FJsonObject());

	const float ElapsedSec = GetMissionElapsedTimeSeconds();
	int32 CompletedTasks = 0;
	int32 TotalTasks = 0;
	const float RepairPct = GetSpaceshipRepairProgressPercent(CompletedTasks, TotalTasks);

	const bool bIsCriticalPrompt =
		LastSentPrompt.Contains(TEXT("burning"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("fire"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("oxygen"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("explosion"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("critical"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("مشتعل"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("اكسجين"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("خطر"), ESearchCase::IgnoreCase);

	AActor* BrokenTarget = ActiveQuestTargetActor.IsValid() ? ActiveQuestTargetActor.Get() : PickNextBreakableActorInShip();
	FString TargetObjName = TEXT("MainShipSystem");
	if (BrokenTarget)
	{
		if (const USpaceAIIdentityComponent* IdComp = BrokenTarget->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			TargetObjName = IdComp->GetEffectiveEntityName();
		}
		else
		{
			TargetObjName = BrokenTarget->GetActorNameOrLabel();
		}
	}

	AActor* ToolActor = ActiveQuestRequiredToolActor.IsValid() ? ActiveQuestRequiredToolActor.Get() : PickNextToolActorInShip(BrokenTarget);
	FString ToolObjName = ActiveQuestRequiredItem;
	if (ToolActor && ToolObjName.IsEmpty())
	{
		if (const USpaceAIIdentityComponent* ToolId = ToolActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			ToolObjName = ToolId->GetEffectiveEntityName();
		}
		else
		{
			ToolObjName = ToolActor->GetActorNameOrLabel();
		}
	}

	FString LogicalVerb = TEXT("Repair");
	FString QuestInstruction = FString::Printf(TEXT("Fly to the 3D Quest Box on '%s' & Press [E] to fix!"), *TargetObjName);
	if (!ToolObjName.IsEmpty())
	{
		LogicalVerb = GetLogicalQuestVerbAndInstruction(ToolActor, ToolObjName, TargetObjName, QuestInstruction);
	}

	FString QuestTitle = ToolObjName.IsEmpty()
		? FString::Printf(TEXT("Service Breakdown at %s"), *TargetObjName)
		: FString::Printf(TEXT("%s %s at %s"), *LogicalVerb, *ToolObjName, *TargetObjName);

	FString ComputedRiskLevel = CurrentAIRiskLevel.IsEmpty() ? TEXT("MODERATE") : CurrentAIRiskLevel;
	float ComputedRiskScore = FMath::Clamp(CurrentAIRiskScorePercent, 25.0f, 95.0f);

	if (bIsCriticalPrompt)
	{
		QuestTitle = FString::Printf(TEXT("CRITICAL HAZARD AT %s!"), *TargetObjName);
		QuestInstruction = ToolObjName.IsEmpty()
			? FString::Printf(TEXT("Go to the Red Quest Box at '%s' & Press [E] immediately!"), *TargetObjName)
			: FString::Printf(TEXT("URGENT: Take '%s' to '%s' Red Quest Box & Press [E] immediately!"), *ToolObjName, *TargetObjName);
		ComputedRiskLevel = TEXT("CRITICAL");
		ComputedRiskScore = 95.0f;
	}
	else if (CompletedAIQuestsCount > 0 && !bHasActiveAIQuest)
	{
		ComputedRiskLevel = TEXT("MODERATE");
		ComputedRiskScore = FMath::Clamp(CurrentAIRiskScorePercent, 30.0f, 75.0f);
	}

	const FString DynamicMission = FString::Printf(TEXT("[Quest #%d] %s — %s"), CompletedAIQuestsCount + 1, *QuestTitle, *QuestInstruction);

	FString CoachingMsg;
	if (bIsCriticalPrompt || ComputedRiskScore >= 80.0f)
	{
		CoachingMsg = FString::Printf(
			TEXT("Flight Director [Priority Red]: Critical alarm at %s! Atmospheric containment at risk. Astronaut, secure the %s and execute emergency %s immediately! Watch your momentum! (Mastery: %.0f/100)"),
			*TargetObjName,
			ToolObjName.IsEmpty() ? TEXT("service equipment") : *ToolObjName,
			*LogicalVerb.ToLower(),
			ProblemSolvingProficiencyScore
		);
	}
	else if (ComputedRiskScore >= 50.0f)
	{
		CoachingMsg = FString::Printf(
			TEXT("Flight Director: Sensor telemetry indicates malfunction at %s. Retrieve the %s and %s the system before pressure fluctuates. (Mastery: %.0f/100)"),
			*TargetObjName,
			ToolObjName.IsEmpty() ? TEXT("required tool") : *ToolObjName,
			*LogicalVerb.ToLower(),
			ProblemSolvingProficiencyScore
		);
	}
	else
	{
		CoachingMsg = FString::Printf(
			TEXT("Flight Director: Station telemetry is steady. Proceed to %s with the %s to complete scheduled %s. Zero-G handling looking sharp! (Mastery: %.0f/100)"),
			*TargetObjName,
			ToolObjName.IsEmpty() ? TEXT("designated tool") : *ToolObjName,
			*LogicalVerb.ToLower(),
			ProblemSolvingProficiencyScore
		);
	}

	RootObj->SetStringField(TEXT("message"), CoachingMsg);
	RootObj->SetStringField(TEXT("risk_level"), ComputedRiskLevel);
	RootObj->SetNumberField(TEXT("risk_score"), ComputedRiskScore);

	TSharedPtr<FJsonObject> QuestObj = MakeShareable(new FJsonObject());
	QuestObj->SetStringField(TEXT("quest_title"), QuestTitle);
	QuestObj->SetStringField(TEXT("quest_instruction"), QuestInstruction);
	QuestObj->SetStringField(TEXT("target_actor"), TargetObjName);
	QuestObj->SetStringField(TEXT("required_item"), ToolObjName);
	RootObj->SetObjectField(TEXT("ai_quest"), QuestObj);

	TSharedPtr<FJsonObject> ShipEvents = MakeShareable(new FJsonObject());
	ShipEvents->SetNumberField(TEXT("ship_shake_intensity"), DefaultShipShakeIntensity);
	ShipEvents->SetNumberField(TEXT("ship_shake_duration"), 2.2f);
	ShipEvents->SetNumberField(TEXT("spawn_obstacle_count"), (SessionItemsSnappedCount > 0 && SessionAISpawnedObstaclesCount < 3) ? 1 : 0);
	ShipEvents->SetStringField(TEXT("spawn_obstacle_name"), TEXT("Floating Hull Debris"));
	ShipEvents->SetNumberField(TEXT("spawn_obstacle_mass_kg"), 10.0f);
	ShipEvents->SetNumberField(TEXT("spawn_obstacle_scale"), 0.9f);
	ShipEvents->SetBoolField(TEXT("launch_obstacles_at_player"), true);
	ShipEvents->SetNumberField(TEXT("hurl_existing_objects_count"), 1);
	RootObj->SetObjectField(TEXT("ship_events"), ShipEvents);

	// Also generate AI Lighting commands for RectLight & RectLight2 based on current repair progress / danger!
	TArray<TSharedPtr<FJsonValue>> LightCmdsArray;
	TSharedPtr<FJsonObject> LightCmd = MakeShareable(new FJsonObject());
	LightCmd->SetStringField(TEXT("target"), TEXT("ALL_LIGHTS"));

	if (LastSentPrompt.Contains(TEXT("burning"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("fire"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("oxygen"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("explosion"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("critical"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("مشتعل"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("اكسجين"), ESearchCase::IgnoreCase) ||
		LastSentPrompt.Contains(TEXT("خطر"), ESearchCase::IgnoreCase))
	{
		LightCmd->SetStringField(TEXT("preset"), TEXT("critical_red"));
		LightCmd->SetStringField(TEXT("mode"), TEXT("pulse_alarm"));
	}
	else if (RepairPct >= 100.0f)
	{
		LightCmd->SetStringField(TEXT("preset"), TEXT("repaired_green"));
		LightCmd->SetStringField(TEXT("mode"), TEXT("solid"));
	}
	else if (RepairPct >= 50.0f)
	{
		LightCmd->SetStringField(TEXT("preset"), TEXT("warning_orange"));
		LightCmd->SetStringField(TEXT("mode"), TEXT("pulse_alarm"));
	}
	else
	{
		LightCmd->SetStringField(TEXT("preset"), TEXT("flicker_damage"));
		LightCmd->SetStringField(TEXT("mode"), TEXT("flicker"));
	}

	LightCmdsArray.Add(MakeShareable(new FJsonValueObject(LightCmd)));
	RootObj->SetArrayField(TEXT("light_commands"), LightCmdsArray);

	TArray<TSharedPtr<FJsonValue>> CommandsArray;
	UWorld* World = GetWorld();
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			const USpaceAIIdentityComponent* IdComp = Actor->FindComponentByClass<USpaceAIIdentityComponent>();
			const USpaceObjectComponent* SpaceObj = Actor->FindComponentByClass<USpaceObjectComponent>();
			if (!SpaceObj)
			{
				continue;
			}

			const FString NameAndDesc = IdComp
				? (IdComp->GetEffectiveEntityName() + TEXT(" ") + IdComp->DescriptionAndPurpose + TEXT(" ") + IdComp->AIPhysicsControlHint)
				: Actor->GetActorNameOrLabel();

			TSharedPtr<FJsonObject> Cmd = MakeShareable(new FJsonObject());
			Cmd->SetStringField(TEXT("target"), IdComp ? IdComp->GetEffectiveEntityID() : Actor->GetName());

			// Infer realistic scale/size, mass & spin from the user's description!
			if (NameAndDesc.Contains(TEXT("big"), ESearchCase::IgnoreCase) ||
				NameAndDesc.Contains(TEXT("large"), ESearchCase::IgnoreCase) ||
				NameAndDesc.Contains(TEXT("كبير"), ESearchCase::IgnoreCase))
			{
				Cmd->SetNumberField(TEXT("scale"), 1.25f);
			}
			else if (NameAndDesc.Contains(TEXT("small"), ESearchCase::IgnoreCase) ||
					 NameAndDesc.Contains(TEXT("tiny"), ESearchCase::IgnoreCase) ||
					 NameAndDesc.Contains(TEXT("صغير"), ESearchCase::IgnoreCase))
			{
				Cmd->SetNumberField(TEXT("scale"), 0.75f);
			}

			if (NameAndDesc.Contains(TEXT("paper"), ESearchCase::IgnoreCase) ||
				NameAndDesc.Contains(TEXT("ورق"), ESearchCase::IgnoreCase) ||
				NameAndDesc.Contains(TEXT("خفيف"), ESearchCase::IgnoreCase) ||
				NameAndDesc.Contains(TEXT("light"), ESearchCase::IgnoreCase))
			{
				Cmd->SetNumberField(TEXT("mass_kg"), 0.2f);
				Cmd->SetNumberField(TEXT("spin_speed"), 32.0f);
				Cmd->SetNumberField(TEXT("wave_amplitude"), 10.0f);
			}
			else if (NameAndDesc.Contains(TEXT("heavy"), ESearchCase::IgnoreCase) ||
					 NameAndDesc.Contains(TEXT("ثقيل"), ESearchCase::IgnoreCase) ||
					 NameAndDesc.Contains(TEXT("crate"), ESearchCase::IgnoreCase) ||
					 NameAndDesc.Contains(TEXT("صندوق"), ESearchCase::IgnoreCase))
			{
				Cmd->SetNumberField(TEXT("mass_kg"), 55.0f);
				Cmd->SetNumberField(TEXT("spin_speed"), 7.0f);
				Cmd->SetNumberField(TEXT("wave_amplitude"), 2.0f);
			}
			else
			{
				Cmd->SetNumberField(TEXT("mass_kg"), SpaceObj->MassInKg);
				Cmd->SetNumberField(TEXT("spin_speed"), FMath::Max(12.0f, SpaceObj->SelfRotationSpeed));
				Cmd->SetNumberField(TEXT("bounce"), 0.4f);
			}

			CommandsArray.Add(MakeShareable(new FJsonValueObject(Cmd)));
		}
	}

	RootObj->SetArrayField(TEXT("physics_commands"), CommandsArray);

	FString OutputJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);
	return OutputJson;
}

void UMetaAIComponent::VerifyApiKeyAndListModels()
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(TEXT("https://api.meta.ai/v1/models"));
	HttpRequest->SetVerb(TEXT("GET"));
	HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey.TrimStartAndEnd()));

	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UMetaAIComponent::OnModelsListResponseReceived);
	HttpRequest->ProcessRequest();
}

void UMetaAIComponent::OnModelsListResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		PrintScreenDebug(TEXT("[MetaAI Auth Check] Failed to reach https://api.meta.ai/v1/models (Check Internet Connection)"), FColor::Red, ScreenMessageDuration);
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();

	if (StatusCode == 200)
	{
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* DataArray = nullptr;
			if (JsonObj->TryGetArrayField(TEXT("data"), DataArray) && DataArray)
			{
				FString ModelNamesList;
				for (const TSharedPtr<FJsonValue>& Val : *DataArray)
				{
					TSharedPtr<FJsonObject> ModelObj = Val->AsObject();
					if (ModelObj.IsValid())
					{
						FString Id;
						if (ModelObj->TryGetStringField(TEXT("id"), Id))
						{
							if (!ModelNamesList.IsEmpty())
							{
								ModelNamesList += TEXT(", ");
							}
							ModelNamesList += Id;
						}
					}
				}

				PrintScreenDebug(
					FString::Printf(TEXT("[MetaAI Auth SUCCESS] Connected to Meta API (200 OK)! Available Models: %s"), *ModelNamesList),
					FColor::Green,
					ScreenMessageDuration
				);
			}
		}
	}
	else
	{
		PrintScreenDebug(
			FString::Printf(TEXT("[MetaAI Auth Check] HTTP %d: %s"), StatusCode, *ResponseBody),
			FColor::Orange,
			ScreenMessageDuration
		);
	}
}

void UMetaAIComponent::OnChatCompletionResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	bIsAwaitingBreakdownAIResponse = false;
	bIsRequestInFlight = false;
	bGlobalRequestInFlight = false;
	ActiveHttpRequest.Reset();

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		const FString ErrMsg = TEXT("[Meta AI Connection Notice] Reconnecting to Meta Model API...");
		PrintScreenDebug(ErrMsg, FColor::Orange, 5.0f);
		OnAIResponseReceived.Broadcast(false, ErrMsg, TEXT(""));
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString RawBody = Response->GetContentAsString();

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawBody);
	const bool bParsedJson = FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid();

	if (StatusCode != 200 || !bParsedJson)
	{
		// Only trip circuit breaker on authentic auth or credit exhaustion errors (401 / 402)
		if (StatusCode == 401 || StatusCode == 402)
		{
			bCircuitBreakerTripped = true;
		}
		const FString ErrMsg = FString::Printf(TEXT("[Meta AI Notice HTTP %d] Model response returned code %d."), StatusCode, StatusCode);
		PrintScreenDebug(ErrMsg, FColor::Red, 15.0f);
		OnAIResponseReceived.Broadcast(false, ErrMsg, RawBody);
		return;
	}

	FString ExtractedContent;
	FString FinishReason;
	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray = nullptr;
	if (JsonObj->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray && ChoicesArray->Num() > 0)
	{
		TSharedPtr<FJsonObject> FirstChoice = (*ChoicesArray)[0]->AsObject();
		if (FirstChoice.IsValid())
		{
			FirstChoice->TryGetStringField(TEXT("finish_reason"), FinishReason);
			const TSharedPtr<FJsonObject>* MessageObj = nullptr;
			if (FirstChoice->TryGetObjectField(TEXT("message"), MessageObj) && MessageObj && (*MessageObj).IsValid())
			{
				(*MessageObj)->TryGetStringField(TEXT("content"), ExtractedContent);
				if (ExtractedContent.TrimStartAndEnd().IsEmpty())
				{
					(*MessageObj)->TryGetStringField(TEXT("reasoning_content"), ExtractedContent);
				}
			}
			if (ExtractedContent.TrimStartAndEnd().IsEmpty())
			{
				FirstChoice->TryGetStringField(TEXT("text"), ExtractedContent);
			}
		}
	}

	if (ExtractedContent.TrimStartAndEnd().IsEmpty())
	{
		PrintScreenDebug(
			FString::Printf(TEXT("[Meta AI Token Notice] Model reasoning consumed token budget (finish_reason: '%s'). Retrying with expanded token window..."), *FinishReason),
			FColor::Orange,
			8.0f
		);
		MaxCompletionTokens = FMath::Clamp(MaxCompletionTokens + 1000, 3500, 6000);
		if (!bHasActiveAIQuest)
		{
			FTimerHandle RetryHandle;
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(RetryHandle, [this]()
				{
					TriggerNewShipBreakdownAndAIQuest();
				}, 1.5f, false);
			}
		}
		return;
	}

	// Save into Session Cache
	if (bUseSessionResponseCache && !LastSentPrompt.IsEmpty())
	{
		SessionResponseCache.Add(LastSentPrompt, ExtractedContent);
	}

	// Guard: If this response was for precomputing the upcoming quest, store it in the pipeline buffer ONLY!
	if (bIsPrecomputingNextQuest)
	{
		bIsPrecomputingNextQuest = false;
		PrecomputedNextAIQuest.PrecomputedRawAIJson = ExtractedContent;
		bHasPrecomputedNextQuest = true;

		TSharedPtr<FJsonObject> RespJson;
		TSharedRef<TJsonReader<>> RespReader = TJsonReaderFactory<>::Create(ExtractedContent);
		if (FJsonSerializer::Deserialize(RespReader, RespJson) && RespJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* Ptr = nullptr;
			if (RespJson->TryGetObjectField(TEXT("ai_quest"), Ptr) && Ptr && (*Ptr).IsValid())
			{
				FString Ttl, Inst;
				(*Ptr)->TryGetStringField(TEXT("quest_title"), Ttl);
				(*Ptr)->TryGetStringField(TEXT("quest_instruction"), Inst);
				if (!Ttl.IsEmpty()) PrecomputedNextAIQuest.QuestTitle = Ttl;
				if (!Inst.IsEmpty()) PrecomputedNextAIQuest.QuestInstruction = Inst;
			}
			FString Msg;
			RespJson->TryGetStringField(TEXT("message"), Msg);
			if (!Msg.IsEmpty()) PrecomputedNextAIQuest.PrecomputedFlightDirectorAdvice = Msg;
		}

		PrintScreenDebug(TEXT("[Meta AI Pipeline] Upcoming Quest precomputed in background and buffered for next repair."), FColor::Emerald, 4.0f);
		return;
	}

	FString HumanMessage = ExtractedContent;
	if (bEnableAIPhysicsControl)
	{
		ExecuteAIPhysicsResponseJson(ExtractedContent, HumanMessage);
	}

	if (bEnablePhpDatabaseSync)
	{
		LogAIAdviceToPhpDatabase(HumanMessage, LastReadablePhysicsSummary);
	}

	// Once the current in-flight AI response finishes, automatically start precomputing the upcoming quest in background!
	if (!bHasPrecomputedNextQuest && !bIsPrecomputingNextQuest && !bMasteryThresholdReached && ProblemSolvingProficiencyScore < ProblemSolvingMasteryThreshold)
	{
		StartBackgroundPrecomputeNextQuest();
	}

	PrintScreenDebug(
		FString::Printf(TEXT("[MetaAI Autonomous Core (200 OK) | Request: %d/%d]: %s"),
			GlobalSessionRequestCount, MaxRequestsPerSession, *HumanMessage),
		FColor::Cyan,
		ScreenMessageDuration
	);

	OnAIResponseReceived.Broadcast(true, HumanMessage, RawBody);
}

void UMetaAIComponent::BindToPlayerSimulationComponents()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		if (USpaceGrabSnapComponent* GrabComp = Actor->FindComponentByClass<USpaceGrabSnapComponent>())
		{
			GrabComp->OnItemGrabStateChanged.RemoveDynamic(this, &UMetaAIComponent::HandleTrackedItemGrabOrSnap);
			GrabComp->OnItemGrabStateChanged.AddDynamic(this, &UMetaAIComponent::HandleTrackedItemGrabOrSnap);

			GrabComp->OnPlayerInteractedWithActor.RemoveDynamic(this, &UMetaAIComponent::HandleTrackedPlayerInteractWithE);
			GrabComp->OnPlayerInteractedWithActor.AddDynamic(this, &UMetaAIComponent::HandleTrackedPlayerInteractWithE);
		}

		if (USpaceFloatingComponent* FloatComp = Actor->FindComponentByClass<USpaceFloatingComponent>())
		{
			FloatComp->OnZeroGHandAction.RemoveDynamic(this, &UMetaAIComponent::HandleTrackedZeroGHandAction);
			FloatComp->OnZeroGHandAction.AddDynamic(this, &UMetaAIComponent::HandleTrackedZeroGHandAction);

			FloatComp->OnZeroGWallInteraction.RemoveDynamic(this, &UMetaAIComponent::HandleTrackedWallInteraction);
			FloatComp->OnZeroGWallInteraction.AddDynamic(this, &UMetaAIComponent::HandleTrackedWallInteraction);
		}
	}
}

void UMetaAIComponent::HandleTrackedItemGrabOrSnap(AActor* ItemActor, AActor* SnapTargetActor, bool bWasSnapped)
{
	FString ItemName = TEXT("UnknownItem");
	if (ItemActor)
	{
		if (const USpaceAIIdentityComponent* IdComp = ItemActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			ItemName = IdComp->GetEffectiveEntityName();
		}
		else
		{
			ItemName = ItemActor->GetActorNameOrLabel();
		}
	}

	FString TargetName = TEXT("");
	if (SnapTargetActor)
	{
		if (const USpaceAIIdentityComponent* TargetId = SnapTargetActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			TargetName = TargetId->GetEffectiveEntityName();
		}
		else
		{
			TargetName = SnapTargetActor->GetActorNameOrLabel();
		}
	}

	if (bWasSnapped)
	{
		SessionItemsSnappedCount++;
		if (SnapTargetActor)
		{
			if (USpaceAIIdentityComponent* TargetIdComp = SnapTargetActor->FindComponentByClass<USpaceAIIdentityComponent>())
			{
				TargetIdComp->SetRepairTaskCompleted(true, 100.0f);
			}
		}

		LogTrainingEventToDatabase(TEXT("item_snapped"), ItemName, TargetName);

		PrintScreenDebug(
			FString::Printf(
				TEXT("[Mission Telemetry] Snapped '%s' into '%s'! Progress: %d/%d | Time: %.1fs / %.0fs Target"),
				*ItemName, *TargetName, SessionItemsSnappedCount, RequiredSnapsToFinishMission, GetMissionElapsedTimeSeconds(), TargetMissionTimeSeconds
			),
			FColor::Green,
			10.0f
		);

		if (RequiredSnapsToFinishMission > 0 && SessionItemsSnappedCount >= RequiredSnapsToFinishMission && !bIsMissionFinished)
		{
			CompleteTrainingMission(true);
		}
		else if (bAutoAskAICoachOnSnapAndFinish)
		{
			RequestAICoachingAndTimeEvaluation(
				FString::Printf(TEXT("item_snapped: Trainee snapped '%s' into '%s' (%d/%d completed at %.1fs)"),
					*ItemName, *TargetName, SessionItemsSnappedCount, RequiredSnapsToFinishMission, GetMissionElapsedTimeSeconds())
			);
		}
	}
	else if (ItemActor)
	{
		// Distinguish between grabbing an item into hand vs dropping/throwing it!
		bool bIsNowHeld = false;
		if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0))
		{
			if (const USpaceGrabSnapComponent* GrabComp = PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>())
			{
				bIsNowHeld = GrabComp->IsHoldingObject();
			}
		}

		if (bIsNowHeld)
		{
			SessionItemsGrabbedCount++;
			LogTrainingEventToDatabase(TEXT("item_grabbed"), ItemName, TEXT("PlayerHand"));

			// Orientation Training Step 3 -> Step 4 Transition
			if (CurrentTrainingPhase == EAstronautTrainingPhase::Step3_GrabEquipment)
			{
				AdvanceOrientationTraining(EAstronautTrainingPhase::Step4_CarryAndCalibrate);
			}
			else if (bHasActiveAIQuest)
			{
				const bool bIsCorrectTool = ActiveQuestRequiredToolActor.IsValid() &&
					(ItemActor == ActiveQuestRequiredToolActor.Get() ||
					 ItemName.Contains(CurrentQuestRequiredToolName, ESearchCase::IgnoreCase) ||
					 CurrentQuestRequiredToolName.Contains(ItemName, ESearchCase::IgnoreCase));

				if (bIsCorrectTool)
				{
					TransmitFlightDirectorRadioComms(
						FString::Printf(TEXT("Flight Director: Required tool '%s' secured! Vector directly toward '%s' (Orange 3D Box) and press [E] to clear fault!"),
							*ItemName, *CurrentQuestTargetName),
						6.0f
					);
				}
				else
				{
					TransmitFlightDirectorRadioComms(
						FString::Printf(TEXT("Flight Director: Cadet, '%s' will not service this breach. Follow the CYAN beacon for the %s!"),
							*ItemName, *CurrentQuestRequiredToolName),
						6.0f
					);
				}
			}

			PrintScreenDebug(
				FString::Printf(TEXT("[Telemetry] Grabbed Object '%s' (Total Grabs: %d | Time: %.1fs)"),
					*ItemName, SessionItemsGrabbedCount, GetMissionElapsedTimeSeconds()),
				FColor::Cyan,
				6.0f
			);
		}
		else
		{
			SessionItemsDroppedCount++;
			LogTrainingEventToDatabase(TEXT("item_released"), ItemName, TEXT("Space"));

			if (bHasActiveAIQuest && CurrentQuestRequiredToolName.Contains(ItemName, ESearchCase::IgnoreCase))
			{
				TransmitFlightDirectorRadioComms(
					FString::Printf(TEXT("Flight Director: Caution! You released required tool '%s' into zero-G drift. Reacquire it before it drifts away!"), *ItemName),
					5.0f
				);
			}
		}
	}
}

void UMetaAIComponent::HandleTrackedZeroGHandAction(bool bIsPullAction, const FVector& TargetPoint, const FVector& AppliedImpulse)
{
	// Handled with full detail in HandleTrackedWallInteraction
}

void UMetaAIComponent::HandleTrackedWallInteraction(const FString& InteractionType, AActor* SurfaceActor, const FVector& WorldPoint, float MetricValue)
{
	FString SurfaceName = TEXT("StationWall");
	if (SurfaceActor)
	{
		if (const USpaceAIIdentityComponent* IdComp = SurfaceActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			SurfaceName = IdComp->GetEffectiveEntityName();
		}
		else
		{
			SurfaceName = SurfaceActor->GetActorNameOrLabel();
		}
	}
	LastGrabbedWallName = SurfaceName;

	if (InteractionType == TEXT("wall_grab_start"))
	{
		SessionWallGrabsCount++;
		LogTrainingEventToDatabase(TEXT("wall_grab_start"), TEXT("Player"), SurfaceName);
	}
	else if (InteractionType == TEXT("wall_walk"))
	{
		SessionWallWalkDistanceMeters = MetricValue;
		LogTrainingEventToDatabase(TEXT("wall_walk"), TEXT("Player"), FString::Printf(TEXT("%s (%.1fm)"), *SurfaceName, SessionWallWalkDistanceMeters));
	}
	else if (InteractionType == TEXT("wall_stop_brake"))
	{
		SessionWallStopsCount++;
		LogTrainingEventToDatabase(TEXT("wall_stop_brake"), TEXT("Player"), SurfaceName);
	}
	else if (InteractionType == TEXT("wall_launch") || InteractionType == TEXT("wall_pull"))
	{
		SessionWallLaunchesCount++;
		// Silent telemetry only (DB / dashboard). No voice or HUD text on every jump.
		LogTrainingEventToDatabase(InteractionType, TEXT("Player"), FString::Printf(TEXT("%s (%.0f cm/s)"), *SurfaceName, MetricValue));

		// Orientation Training Step 2 -> Step 3 Transition
		if (CurrentTrainingPhase == EAstronautTrainingPhase::Step2_WallGrabAndLaunch)
		{
			AdvanceOrientationTraining(EAstronautTrainingPhase::Step3_GrabEquipment);
		}
	}
}

void UMetaAIComponent::StartTrainingMissionSession()
{
	CurrentSessionUUID = FString::Printf(TEXT("sess_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
	MissionStartTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	FinalMissionDurationSeconds = 0.0f;
	bIsMissionFinished = false;
	SessionItemsGrabbedCount = 0;
	SessionItemsDroppedCount = 0;
	SessionItemsSnappedCount = 0;
	SessionWallGrabsCount = 0;
	SessionWallStopsCount = 0;
	SessionWallLaunchesCount = 0;
	SessionWallWalkDistanceMeters = 0.0f;
	SessionAISpawnedObstaclesCount = 0;
	SessionAIShipShakesCount = 0;
	RecentlyBrokenQuestActors.Reset();
	RecentlyUsedTargetCategories.Reset();
	RecentlyUsedTargetNames.Reset();
	RecentlyUsedQuestToolActors.Reset();
	RecentlyUsedToolCategories.Reset();
	SessionAssignedQuestKeys.Reset();
	ActiveCameraShakeRemainingTime = 0.0f;
	LastGrabbedWallName = TEXT("None");

	if (!bEnablePhpDatabaseSync || PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject());
	Payload->SetStringField(TEXT("action"), TEXT("start_session"));
	Payload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
	Payload->SetStringField(TEXT("trainee_name"), TraineeName);
	Payload->SetStringField(TEXT("mission_name"), MissionName);
	Payload->SetStringField(TEXT("risk_level"), CurrentAIRiskLevel);
	Payload->SetNumberField(TEXT("risk_score"), CurrentAIRiskScorePercent);
	Payload->SetNumberField(TEXT("target_time_seconds"), TargetMissionTimeSeconds);
	Payload->SetNumberField(TEXT("required_snaps_count"), RequiredSnapsToFinishMission);
	Payload->SetStringField(TEXT("world_objects_json"), BuildWorldContextJsonString(TEXT("session_init")));

	SendJsonToPhpBackend(Payload);
}

void UMetaAIComponent::LogTrainingEventToDatabase(const FString& EventType, const FString& ActorName, const FString& TargetName)
{
	if (!bEnablePhpDatabaseSync || PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	if (CurrentSessionUUID.IsEmpty())
	{
		StartTrainingMissionSession();
	}

	float PlayerSpeed = 0.0f;
	if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0))
	{
		PlayerSpeed = PlayerChar->GetVelocity().Size();
	}

	TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject());
	Payload->SetStringField(TEXT("action"), TEXT("log_event"));
	Payload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
	Payload->SetStringField(TEXT("event_type"), EventType);
	Payload->SetStringField(TEXT("actor_name"), ActorName);
	Payload->SetStringField(TEXT("target_name"), TargetName);
	Payload->SetStringField(TEXT("mission_name"), MissionName);
	Payload->SetStringField(TEXT("risk_level"), CurrentAIRiskLevel);
	Payload->SetNumberField(TEXT("risk_score"), CurrentAIRiskScorePercent);
	Payload->SetNumberField(TEXT("elapsed_time_seconds"), GetMissionElapsedTimeSeconds());
	Payload->SetNumberField(TEXT("player_speed"), PlayerSpeed);

	SendJsonToPhpBackend(Payload);
}

void UMetaAIComponent::LogAIAdviceToPhpDatabase(const FString& AIMessage, const FString& RawAIJson)
{
	if (!bEnablePhpDatabaseSync || PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	if (CurrentSessionUUID.IsEmpty())
	{
		StartTrainingMissionSession();
	}

	TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject());
	Payload->SetStringField(TEXT("action"), TEXT("log_ai_advice"));
	Payload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
	Payload->SetNumberField(TEXT("elapsed_time_seconds"), GetMissionElapsedTimeSeconds());
	Payload->SetStringField(TEXT("directive_sent"), LastSentPrompt.Left(500));
	Payload->SetStringField(TEXT("ai_message"), AIMessage);
	Payload->SetStringField(TEXT("mission_name"), MissionName);
	Payload->SetStringField(TEXT("risk_level"), CurrentAIRiskLevel);
	Payload->SetNumberField(TEXT("risk_score"), CurrentAIRiskScorePercent);
	Payload->SetStringField(TEXT("ai_grade"), CurrentAIGrade);
	Payload->SetNumberField(TEXT("final_score"), CurrentAIFinalScore);
	Payload->SetStringField(TEXT("flight_readiness"), CurrentAIFlightReadiness);
	Payload->SetStringField(TEXT("ai_evaluation_summary"), CurrentAIEvaluationSummary);
	Payload->SetStringField(TEXT("physics_commands_json"), RawAIJson);
	Payload->SetArrayField(TEXT("object_edits"), LastObjectEditsArray);

	SendJsonToPhpBackend(Payload);
}

void UMetaAIComponent::RequestAICoachingAndTimeEvaluation(const FString& CheckpointOrEventReason)
{
	const float Elapsed = GetMissionElapsedTimeSeconds();
	const FString CoachingDirective = FString::Printf(
		TEXT("AI_COACH_TIME_EVALUATION [%s]: Trainee '%s' telemetry -> Elapsed: %.1fs (Target: %.1fs), Snapped: %d/%d items, Grabbed: %d, Wall Launches: %d. "
			 "Autonomously study trainee flight efficiency, inertia control, and tool acquisition time. "
			 "Provide your authentic engineering study, competency score (0-100), and letter grade in 'trainee_evaluation', radio coaching in 'message', and adjust zero-G physics if necessary!"),
		*CheckpointOrEventReason,
		*TraineeName,
		Elapsed,
		TargetMissionTimeSeconds,
		SessionItemsSnappedCount,
		RequiredSnapsToFinishMission,
		SessionItemsGrabbedCount,
		SessionWallLaunchesCount
	);

	SendWorldDirectiveToAI(CoachingDirective);
}

float UMetaAIComponent::GetMissionElapsedTimeSeconds() const
{
	if (bIsMissionFinished && FinalMissionDurationSeconds > 0.0f)
	{
		return FinalMissionDurationSeconds;
	}
	if (const UWorld* World = GetWorld())
	{
		return FMath::Max(0.0f, World->GetTimeSeconds() - MissionStartTimeSeconds);
	}
	return 0.0f;
}

void UMetaAIComponent::CompleteTrainingMission(bool bMissionSuccess)
{
	if (!bIsMissionFinished)
	{
		FinalMissionDurationSeconds = GetMissionElapsedTimeSeconds();
		bIsMissionFinished = true;
	}

	const FString StatusStr = bMissionSuccess ? TEXT("completed") : TEXT("failed");

	PrintScreenDebug(
		FString::Printf(
			TEXT("=== [MISSION %s!] Total Time: %.2f seconds (Target: %.0fs) | Snapped: %d/%d | Grabs: %d | Wall Launches: %d ==="),
			bMissionSuccess ? TEXT("COMPLETED") : TEXT("FAILED"),
			FinalMissionDurationSeconds,
			TargetMissionTimeSeconds,
			SessionItemsSnappedCount,
			RequiredSnapsToFinishMission,
			SessionItemsGrabbedCount,
			SessionWallLaunchesCount
		),
		bMissionSuccess ? FColor::Green : FColor::Orange,
		ScreenMessageDuration + 5.0f
	);

	if (bEnablePhpDatabaseSync && !PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject());
		Payload->SetStringField(TEXT("action"), TEXT("finish_session"));
		Payload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
		Payload->SetStringField(TEXT("status"), StatusStr);
		Payload->SetNumberField(TEXT("duration_seconds"), FinalMissionDurationSeconds);
		Payload->SetNumberField(TEXT("items_grabbed_count"), SessionItemsGrabbedCount);
		Payload->SetNumberField(TEXT("items_snapped_count"), SessionItemsSnappedCount);
		Payload->SetNumberField(TEXT("wall_launches_count"), SessionWallLaunchesCount);
		Payload->SetStringField(TEXT("ai_grade"), CurrentAIGrade);
		Payload->SetNumberField(TEXT("final_score"), CurrentAIFinalScore);
		Payload->SetStringField(TEXT("flight_readiness"), CurrentAIFlightReadiness);
		Payload->SetStringField(TEXT("ai_evaluation_summary"), CurrentAIEvaluationSummary);

		SendJsonToPhpBackend(Payload);
	}

	if (bAutoAskAICoachOnSnapAndFinish)
	{
		RequestAICoachingAndTimeEvaluation(
			FString::Printf(TEXT("mission_%s_in_%.1f_seconds"), *StatusStr, FinalMissionDurationSeconds)
		);
	}
}

void UMetaAIComponent::SendJsonToPhpBackend(const TSharedPtr<FJsonObject>& PayloadJson)
{
	if (!PayloadJson.IsValid() || PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	FString BodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
	FJsonSerializer::Serialize(PayloadJson.ToSharedRef(), Writer);

	// Extract core action & session parameters into query string for 100% web hosting compatibility!
	FString TargetUrl = PhpApiUrl.TrimStartAndEnd();
	FString ActionName;
	PayloadJson->TryGetStringField(TEXT("action"), ActionName);
	FString SessionId;
	PayloadJson->TryGetStringField(TEXT("session_uuid"), SessionId);
	FString EventType;
	PayloadJson->TryGetStringField(TEXT("event_type"), EventType);

	if (!ActionName.IsEmpty())
	{
		const TCHAR Sep = TargetUrl.Contains(TEXT("?")) ? TEXT('&') : TEXT('?');
		TargetUrl += FString::Printf(TEXT("%caction=%s"), Sep, *ActionName);
		if (!SessionId.IsEmpty())
		{
			TargetUrl += FString::Printf(TEXT("&session_uuid=%s"), *SessionId);
		}
		if (!EventType.IsEmpty())
		{
			TargetUrl += FString::Printf(TEXT("&event_type=%s"), *EventType);
		}
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(TargetUrl);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetContentAsString(BodyString);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UMetaAIComponent::OnPhpDatabaseResponseReceived);
	HttpRequest->ProcessRequest();
}

void UMetaAIComponent::OnPhpDatabaseResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[MetaAI MySQL Sync] Could not reach PHP endpoint: %s"), *PhpApiUrl);
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString Body = Response->GetContentAsString();
	if (StatusCode != 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MetaAI MySQL Sync HTTP %d]: %s"), StatusCode, *Body);
		PrintScreenDebug(FString::Printf(TEXT("[MySQL Sync HTTP %d] %s"), StatusCode, *Body.Left(120)), FColor::Red, 6.0f);
		return;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return;
	}

	FString Action;
	JsonObj->TryGetStringField(TEXT("action"), Action);

	if (Action == TEXT("start_session"))
	{
		const TSharedPtr<FJsonObject>* HistoryObj = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("trainee_history"), HistoryObj) && HistoryObj && (*HistoryObj).IsValid())
		{
			HistoricalCompletedMissions = (*HistoryObj)->GetIntegerField(TEXT("completed_missions"));
			HistoricalBestTimeSeconds = static_cast<float>((*HistoryObj)->GetNumberField(TEXT("best_time_seconds")));
			HistoricalAvgTimeSeconds = static_cast<float>((*HistoryObj)->GetNumberField(TEXT("avg_time_seconds")));
			(*HistoryObj)->TryGetStringField(TEXT("last_run_advice"), HistoricalLastAdvice);
		}

		// Load Saved Quests Catalog from MySQL Database (both Manual & AI-Generated quests!)
		const TArray<TSharedPtr<FJsonValue>>* CatalogArr = nullptr;
		if (bSaveAndFetchQuestsFromDatabase && JsonObj->TryGetArrayField(TEXT("quest_catalog"), CatalogArr) && CatalogArr)
		{
			CachedDatabaseQuests.Reset();
			int32 SkippedFastCount = 0;

			for (const TSharedPtr<FJsonValue>& QVal : *CatalogArr)
			{
				const TSharedPtr<FJsonObject> QObj = QVal.IsValid() ? QVal->AsObject() : nullptr;
				if (!QObj.IsValid())
				{
					continue;
				}

				FSpaceQuestDefinition DbQuest;
				QObj->TryGetStringField(TEXT("quest_key"), DbQuest.QuestKey);
				QObj->TryGetStringField(TEXT("quest_title"), DbQuest.QuestTitle);
				QObj->TryGetStringField(TEXT("quest_instruction"), DbQuest.QuestInstruction);
				QObj->TryGetStringField(TEXT("target_actor"), DbQuest.TargetActorName);
				QObj->TryGetStringField(TEXT("required_item"), DbQuest.RequiredToolName);
				QObj->TryGetStringField(TEXT("quest_source"), DbQuest.QuestSource);
				if (DbQuest.QuestSource.IsEmpty())
				{
					DbQuest.QuestSource = TEXT("DATABASE_SAVED");
				}

				double BestTime = 0.0;
				QObj->TryGetNumberField(TEXT("best_completion_time_sec"), BestTime);
				DbQuest.BestCompletionTimeSeconds = static_cast<float>(BestTime);

				int32 TimesDone = 0;
				QObj->TryGetNumberField(TEXT("times_completed"), TimesDone);
				DbQuest.TimesCompleted = TimesDone;

				if (DbQuest.QuestKey.IsEmpty())
				{
					DbQuest.QuestKey = MakeNormalizedQuestKey(DbQuest.TargetActorName, DbQuest.RequiredToolName, DbQuest.QuestTitle);
				}

				// If the player already completed this quest quickly (<= FastQuestMasteryThresholdSeconds), mark it as Mastered so we don't repeat it!
				if (DbQuest.TimesCompleted > 0 && DbQuest.BestCompletionTimeSeconds > 0.1f && DbQuest.BestCompletionTimeSeconds <= FastQuestMasteryThresholdSeconds)
				{
					CompletedOrMasteredQuestKeys.Add(DbQuest.QuestKey);
					SkippedFastCount++;
				}
				else
				{
					CachedDatabaseQuests.Add(DbQuest);
				}
			}

			if (CachedDatabaseQuests.Num() > 0 || SkippedFastCount > 0)
			{
				PrintScreenDebug(
					FString::Printf(TEXT("[MySQL Quest Database] Loaded %d Available Quests from DB | Skipped %d Fast-Completed Mastered Quests (No Repetition!)"),
						CachedDatabaseQuests.Num(), SkippedFastCount),
					FColor::Emerald,
					10.0f
				);
			}
		}

		PrintScreenDebug(
			FString::Printf(
				TEXT("[MySQL Database Connected] Session '%s' Started! Past Completed Missions: %d | Best Time: %.1fs"),
				*CurrentSessionUUID, HistoricalCompletedMissions, HistoricalBestTimeSeconds
			),
			FColor::Green,
			10.0f
		);
	}
	else if (Action == TEXT("finish_session"))
	{
		const double FinalScore = JsonObj->GetNumberField(TEXT("final_score"));
		FString Grade;
		JsonObj->TryGetStringField(TEXT("ai_grade"), Grade);
		FString Summary;
		JsonObj->TryGetStringField(TEXT("ai_evaluation_summary"), Summary);

		PrintScreenDebug(
			FString::Printf(TEXT("[MySQL Mission Saved] Score: %.1f/100 (Grade %s) | %s"), FinalScore, *Grade, *Summary),
			FColor::Cyan,
			15.0f
		);
	}
}

void UMetaAIComponent::PrintScreenDebug(const FString& Message, FColor Color, float Duration) const
{
	UE_LOG(LogTemp, Log, TEXT("%s"), *Message);

	if (bPrintToScreen && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, Duration, Color, Message);
	}
}

// =============================================================================
// 6. AI Obstacle Spawner, Spaceship Shake & Spaceship Repair Progress Logic
// =============================================================================

AActor* UMetaAIComponent::SpawnSpaceObstacleForPlayer(const FString& ObstacleNameOrType, bool bLaunchTowardPlayer, float LaunchSpeed, float CustomMassKg, float CustomScale)
{
	if (!bAllowAIToSpawnObstacles || SessionAISpawnedObstaclesCount >= MaxAISpawnedObstaclesPerSession || bMasteryThresholdReached)
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// Frame Rate Guard: Clean up any destroyed or stale obstacle references
	SpawnedObstacleActors.RemoveAll([](const TWeakObjectPtr<AActor>& Ptr) { return !Ptr.IsValid(); });

	// If concurrent obstacle count exceeds MaxConcurrentSpawnedObstacles, vaporize the oldest one before spawning a new one!
	while (SpawnedObstacleActors.Num() >= MaxConcurrentSpawnedObstacles && SpawnedObstacleActors.Num() > 0)
	{
		if (SpawnedObstacleActors[0].IsValid())
		{
			DestroyOrDisposeDebrisActor(SpawnedObstacleActors[0].Get());
		}
		else
		{
			SpawnedObstacleActors.RemoveAt(0);
		}
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
	const FVector PlayerLoc = PlayerChar ? PlayerChar->GetActorLocation() : (GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector);
	const FVector ForwardDir = PlayerChar ? PlayerChar->GetControlRotation().Vector() : FVector::ForwardVector;
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, ForwardDir).GetSafeNormal();

	// Spawn ahead of the player with slight lateral/vertical spread so obstacles fly into view cleanly, clamped strictly inside locaition_siez room bounds!
	const float LateralOffset = FMath::RandRange(-140.0f, 140.0f);
	const float VerticalOffset = FMath::RandRange(-60.0f, 90.0f);
	const FVector RawSpawnLoc = PlayerLoc + (ForwardDir * ObstacleSpawnDistanceFromPlayer) + (RightDir * LateralOffset) + FVector(0.0f, 0.0f, VerticalOffset);
	const FVector SpawnLoc = ClampLocationToRoomBounds(RawSpawnLoc, RoomBoundaryInnerPaddingCm + 15.0f);
	const FRotator SpawnRot(FMath::RandRange(0.0f, 360.0f), FMath::RandRange(0.0f, 360.0f), FMath::RandRange(0.0f, 360.0f));

	TSubclassOf<AActor> ChosenClass = nullptr;
	UStaticMesh* ChosenMesh = nullptr;

	// 1. Check user-configured SpawnableObstacleClasses first
	TArray<TSubclassOf<AActor>> ValidClasses;
	for (const TSubclassOf<AActor>& CandidateClass : SpawnableObstacleClasses)
	{
		if (CandidateClass)
		{
			ValidClasses.Add(CandidateClass);
		}
	}
	if (ValidClasses.Num() > 0)
	{
		ChosenClass = ValidClasses[FMath::RandRange(0, ValidClasses.Num() - 1)];
	}

	// 2. Check user-configured SpawnableObstacleMeshes OR existing LevelObstacleActors meshes!
	TArray<UStaticMesh*> ValidMeshes;
	for (UStaticMesh* MeshAsset : SpawnableObstacleMeshes)
	{
		if (MeshAsset)
		{
			ValidMeshes.Add(MeshAsset);
		}
	}
	for (AActor* LevelActor : LevelObstacleActors)
	{
		if (LevelActor)
		{
			if (const UStaticMeshComponent* LevelMeshComp = LevelActor->FindComponentByClass<UStaticMeshComponent>())
			{
				if (LevelMeshComp->GetStaticMesh())
				{
					ValidMeshes.AddUnique(LevelMeshComp->GetStaticMesh());
				}
			}
		}
	}
	if (ValidMeshes.Num() > 0)
	{
		ChosenMesh = ValidMeshes[FMath::RandRange(0, ValidMeshes.Num() - 1)];
	}

	// 3. Automatic Fallback (0 Setup Needed): Clone any existing SpaceProp Actor class in the level
	if (!ChosenClass && !ChosenMesh)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Candidate = *It;
			if (Candidate && Candidate != PlayerChar && Candidate->FindComponentByClass<USpaceObjectComponent>() && !IsPermanentQuizTabletActor(Candidate))
			{
				ChosenClass = Candidate->GetClass();
				break;
			}
		}
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AActor* SpawnedActor = nullptr;
	if (ChosenClass)
	{
		SpawnedActor = World->SpawnActor<AActor>(ChosenClass, SpawnLoc, SpawnRot, SpawnParams);
	}
	else
	{
		// 4. Create a Zero-G Obstacle Actor using ChosenMesh (from SpawnableObstacleMeshes / LevelObstacleActors) or fallback Cube
		SpawnedActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLoc, SpawnRot, SpawnParams);
		if (SpawnedActor)
		{
			UBoxComponent* BoxRoot = NewObject<UBoxComponent>(SpawnedActor, TEXT("ObstacleBoxRoot"));
			BoxRoot->InitBoxExtent(FVector(32.0f, 32.0f, 32.0f));
			BoxRoot->SetCollisionProfileName(TEXT("PhysicsActor"));
			BoxRoot->SetSimulatePhysics(true);
			BoxRoot->SetEnableGravity(false);
			BoxRoot->ComponentTags.AddUnique(TEXT("Grabbable"));
			SpawnedActor->SetRootComponent(BoxRoot);
			BoxRoot->RegisterComponent();

			UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(SpawnedActor, TEXT("ObstacleMesh"));
			MeshComp->SetupAttachment(BoxRoot);
			MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			UStaticMesh* MeshToUse = ChosenMesh ? ChosenMesh : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (MeshToUse)
			{
				MeshComp->SetStaticMesh(MeshToUse);
			}
			MeshComp->RegisterComponent();
		}
	}

	if (!SpawnedActor)
	{
		return nullptr;
	}

	SessionAISpawnedObstaclesCount++;
	SpawnedActor->Tags.AddUnique(TEXT("Grabbable"));
	SpawnedObstacleActors.Add(SpawnedActor);

	const float ClampedScale = FMath::Clamp(CustomScale, 0.25f, 3.5f);
	SpawnedActor->SetActorScale3D(FVector(ClampedScale));

	USpaceObjectComponent* SpaceObj = SpawnedActor->FindComponentByClass<USpaceObjectComponent>();
	if (!SpaceObj)
	{
		SpaceObj = NewObject<USpaceObjectComponent>(SpawnedActor, TEXT("AISpawnedSpaceObjComp"));
		SpaceObj->RegisterComponent();
	}

	USpaceAIIdentityComponent* IdComp = SpawnedActor->FindComponentByClass<USpaceAIIdentityComponent>();
	if (!IdComp)
	{
		IdComp = NewObject<USpaceAIIdentityComponent>(SpawnedActor, TEXT("AISpawnedIdentityComp"));
		IdComp->RegisterComponent();
	}

	const FString EffectiveObstacleName = FString::Printf(
		TEXT("%s_%d"),
		*(ObstacleNameOrType.IsEmpty() ? TEXT("SpaceDebris") : ObstacleNameOrType),
		SessionAISpawnedObstaclesCount
	);

	if (IdComp)
	{
		IdComp->EntityName = EffectiveObstacleName;
		IdComp->EntityRole = ESpaceAIEntityRole::SpaceHazardObstacle;
		IdComp->DescriptionAndPurpose = TEXT("AI-spawned zero-G floating space obstacle directed toward the astronaut during spaceship repair.");
	}

	const float ClampedMass = FMath::Clamp(CustomMassKg, 0.2f, 500.0f);
	if (SpaceObj)
	{
		SpaceObj->SetZeroGEnabled(true);
		SpaceObj->SetObjectMassInKg(ClampedMass);
		SpaceObj->SelfRotationSpeed = 25.0f;
		SpaceObj->bEnableAutonomousRotation = true;

		if (bLaunchTowardPlayer)
		{
			const FVector DirToPlayer = (PlayerLoc - SpawnLoc).GetSafeNormal();
			SpaceObj->ApplySpacePush(DirToPlayer * FMath::Max(80.0f, LaunchSpeed), true);
		}
	}

	const FVector BoundsCm = SpawnedActor->GetComponentsBoundingBox(true).GetSize();
	const FString AfterSizeStr = FString::Printf(TEXT("%.2fx (%.0fx%.0fx%.0f cm)"), ClampedScale, BoundsCm.X, BoundsCm.Y, BoundsCm.Z);

	TSharedPtr<FJsonObject> SpawnLogObj = MakeShareable(new FJsonObject());
	SpawnLogObj->SetStringField(TEXT("object_name"), EffectiveObstacleName);
	SpawnLogObj->SetStringField(TEXT("object_role"), TEXT("SpaceHazardObstacle (AI Spawned)"));
	SpawnLogObj->SetStringField(TEXT("size_before"), TEXT("0.00x (AI Spawned)"));
	SpawnLogObj->SetStringField(TEXT("size_after"), AfterSizeStr);
	SpawnLogObj->SetNumberField(TEXT("mass_before_kg"), 0.0);
	SpawnLogObj->SetNumberField(TEXT("mass_after_kg"), ClampedMass);
	SpawnLogObj->SetNumberField(TEXT("spin_before_deg"), 0.0);
	SpawnLogObj->SetNumberField(TEXT("spin_after_deg"), 25.0);
	SpawnLogObj->SetNumberField(TEXT("max_speed_before"), 0.0);
	SpawnLogObj->SetNumberField(TEXT("max_speed_after"), LaunchSpeed);
	SpawnLogObj->SetStringField(
		TEXT("changes_summary"),
		FString::Printf(TEXT("AI Spawned Obstacle '%s' | Size: %s | Mass: %.1fkg | LaunchedTowardPlayer=%s (%.0f cm/s)"),
			*EffectiveObstacleName, *AfterSizeStr, ClampedMass, bLaunchTowardPlayer ? TEXT("YES") : TEXT("NO"), LaunchSpeed)
	);
	LastObjectEditsArray.Add(MakeShareable(new FJsonValueObject(SpawnLogObj)));

	LogTrainingEventToDatabase(TEXT("ai_spawned_obstacle"), EffectiveObstacleName, bLaunchTowardPlayer ? TEXT("LaunchedTowardPlayer") : TEXT("FloatingInShip"));

	PrintScreenDebug(
		FString::Printf(TEXT("[MetaAI Spawner] Spawned Obstacle '%s' (%s, %.1fkg) -> Flying Toward Player!"),
			*EffectiveObstacleName, *AfterSizeStr, ClampedMass),
		FColor::Orange,
		8.0f
	);

	OnAIShipEventTriggered.Broadcast(TEXT("ai_spawned_obstacle"), EffectiveObstacleName, static_cast<float>(SessionAISpawnedObstaclesCount));
	return SpawnedActor;
}

void UMetaAIComponent::DestroyOrDisposeDebrisActor(AActor* DebrisActor)
{
	if (!DebrisActor || !IsValid(DebrisActor))
	{
		return;
	}

	// ABSOLUTE IMMUNITY: Never destroy, hide, or disable the SpaceTabletQuiz device / Quest_main!
	if (IsPermanentQuizTabletActor(DebrisActor))
	{
		return;
	}

	const FString ActorName = DebrisActor->GetActorNameOrLabel();

	// Remove from tracked arrays
	DiscoveredLevelTools.Remove(DebrisActor);
	DiscoveredLevelTargets.Remove(DebrisActor);
	SpawnedObstacleActors.Remove(DebrisActor);

	// Hide and disable collision immediately so player can no longer collide or see it
	DebrisActor->SetActorHiddenInGame(true);
	DebrisActor->SetActorEnableCollision(false);

	// Stop physics simulation
	if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(DebrisActor->GetRootComponent()))
	{
		PrimComp->SetSimulatePhysics(false);
		PrimComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// Destroy the actor from the World to preserve FPS and prevent debris accumulation
	DebrisActor->Destroy();

	PrintScreenDebug(
		FString::Printf(TEXT("[Debris Management] Cleaned up & vaporized '%s' to preserve FPS!"), *ActorName),
		FColor::Cyan,
		3.5f
	);
}

void UMetaAIComponent::CleanupAllExcessDebris()
{
	for (int32 i = SpawnedObstacleActors.Num() - 1; i >= 0; --i)
	{
		if (SpawnedObstacleActors[i].IsValid())
		{
			DestroyOrDisposeDebrisActor(SpawnedObstacleActors[i].Get());
		}
	}
	SpawnedObstacleActors.Empty();
}

void UMetaAIComponent::TriggerShipTurbulenceAndCameraShake(float Intensity, float DurationSeconds, bool bAlsoShakeFloatingObjects, int32 ObjectsToHurlAtPlayerCount)
{
	if (!bAllowAIToTriggerShipCameraShake)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	SessionAIShipShakesCount++;
	ActiveCameraShakeIntensity = FMath::Clamp(Intensity, 0.1f, 2.5f);
	ActiveCameraShakeTotalDuration = FMath::Clamp(DurationSeconds, 0.25f, 15.0f);
	ActiveCameraShakeRemainingTime = ActiveCameraShakeTotalDuration;

	// Give the Player Character a slight Zero-G turbulence jolt if they are floating freely (not anchored to a wall)
	if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0))
	{
		const USpaceFloatingComponent* FloatComp = PlayerChar->FindComponentByClass<USpaceFloatingComponent>();
		if (!FloatComp || !FloatComp->IsGrabbingSurface())
		{
			const FVector RandomJolt = FMath::VRand() * (ActiveCameraShakeIntensity * 95.0f);
			PlayerChar->LaunchCharacter(RandomJolt, false, false);
		}
	}

	// Shake floating props inside the spaceship
	if (bAlsoShakeFloatingObjects)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (USpaceObjectComponent* SpaceObj = Actor->FindComponentByClass<USpaceObjectComponent>())
			{
				const FVector TurbulencePush = FMath::VRand() * (ActiveCameraShakeIntensity * 110.0f);
				SpaceObj->ApplySpacePush(TurbulencePush, true);
			}
		}
	}

	int32 HurledCount = 0;
	if (ObjectsToHurlAtPlayerCount > 0)
	{
		HurledCount = LaunchNearbyObjectsTowardPlayer(ObjectsToHurlAtPlayerCount, ObstacleLaunchSpeedTowardPlayer);
	}

	LogTrainingEventToDatabase(
		TEXT("ship_turbulence_shake"),
		TEXT("SpaceshipHull"),
		FString::Printf(TEXT("Intensity=%.2f, Duration=%.1fs, HurledObjects=%d"), ActiveCameraShakeIntensity, ActiveCameraShakeTotalDuration, HurledCount)
	);

	PrintScreenDebug(
		FString::Printf(TEXT("[MetaAI Spaceship Turbulence!] Camera Shake Intensity=%.2f for %.1fs | Hurled %d Objects Toward Player!"),
			ActiveCameraShakeIntensity, ActiveCameraShakeTotalDuration, HurledCount),
		FColor::Red,
		6.0f
	);

	OnAIShipEventTriggered.Broadcast(TEXT("ship_turbulence_shake"), TEXT("SpaceshipHull"), ActiveCameraShakeIntensity);
}

int32 UMetaAIComponent::LaunchNearbyObjectsTowardPlayer(int32 MaxObjectsCount, float LaunchSpeed)
{
	UWorld* World = GetWorld();
	if (!World || MaxObjectsCount <= 0)
	{
		return 0;
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
	if (!PlayerChar)
	{
		return 0;
	}

	AActor* CurrentlyHeldActor = nullptr;
	if (const USpaceGrabSnapComponent* GrabComp = PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>())
	{
		CurrentlyHeldActor = GrabComp->GetHeldActor();
	}

	const FVector PlayerLoc = PlayerChar->GetActorLocation();
	int32 LaunchedCount = 0;

	// 1. Prioritize existing LevelObstacleActors chosen by the user in the level!
	for (AActor* LevelActor : LevelObstacleActors)
	{
		if (LaunchedCount >= MaxObjectsCount)
		{
			break;
		}
		if (!LevelActor || LevelActor == PlayerChar || LevelActor == CurrentlyHeldActor)
		{
			continue;
		}
		if (USpaceObjectComponent* SpaceObj = LevelActor->FindComponentByClass<USpaceObjectComponent>())
		{
			const FVector DirToPlayer = (PlayerLoc - LevelActor->GetActorLocation()).GetSafeNormal();
			if (!DirToPlayer.IsNearlyZero())
			{
				SpaceObj->ApplySpacePush(DirToPlayer * FMath::Max(60.0f, LaunchSpeed), true);
				LaunchedCount++;

				const USpaceAIIdentityComponent* IdComp = LevelActor->FindComponentByClass<USpaceAIIdentityComponent>();
				const FString ObjName = IdComp ? IdComp->GetEffectiveEntityName() : LevelActor->GetActorNameOrLabel();
				LogTrainingEventToDatabase(TEXT("ai_hurled_level_obstacle"), ObjName, TEXT("Player"));
			}
		}
	}

	// 2. Also check any other floating objects in the world if we still need more
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (LaunchedCount >= MaxObjectsCount)
		{
			break;
		}

		AActor* Candidate = *It;
		if (!Candidate || Candidate == PlayerChar || Candidate == CurrentlyHeldActor || LevelObstacleActors.Contains(Candidate))
		{
			continue;
		}

		USpaceObjectComponent* SpaceObj = Candidate->FindComponentByClass<USpaceObjectComponent>();
		if (!SpaceObj)
		{
			continue;
		}

		UPrimitiveComponent* Prim = SpaceObj->GetPrimaryPrimitive();
		if (!Prim || !Prim->IsSimulatingPhysics())
		{
			continue;
		}

		const FVector DirToPlayer = (PlayerLoc - Candidate->GetActorLocation()).GetSafeNormal();
		if (!DirToPlayer.IsNearlyZero())
		{
			SpaceObj->ApplySpacePush(DirToPlayer * FMath::Max(60.0f, LaunchSpeed), true);
			LaunchedCount++;

			const USpaceAIIdentityComponent* IdComp = Candidate->FindComponentByClass<USpaceAIIdentityComponent>();
			const FString ObjName = IdComp ? IdComp->GetEffectiveEntityName() : Candidate->GetActorNameOrLabel();
			LogTrainingEventToDatabase(TEXT("ai_hurled_obstacle_at_player"), ObjName, TEXT("Player"));
		}
	}

	return LaunchedCount;
}

void UMetaAIComponent::NotifySpaceshipZoneOrTaskUpdated(const FString& ZoneOrTaskName, bool bIsCompleted, float IntegrityPercent, bool bRequestAIReaction)
{
	UWorld* World = GetWorld();
	if (World && !ZoneOrTaskName.IsEmpty())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (USpaceAIIdentityComponent* IdComp = Actor->FindComponentByClass<USpaceAIIdentityComponent>())
			{
				if (IdComp->MatchesAITargetName(ZoneOrTaskName) ||
					IdComp->SpaceshipZoneName.Equals(ZoneOrTaskName, ESearchCase::IgnoreCase))
				{
					IdComp->SetRepairTaskCompleted(bIsCompleted, IntegrityPercent);
				}
			}
		}
	}

	int32 CompletedTasks = 0;
	int32 TotalTasks = 0;
	const float OverallPct = GetSpaceshipRepairProgressPercent(CompletedTasks, TotalTasks);

	const FString EventType = bIsCompleted ? TEXT("repair_zone_completed") : TEXT("repair_zone_damaged");
	LogTrainingEventToDatabase(
		EventType,
		ZoneOrTaskName,
		FString::Printf(TEXT("ZoneIntegrity=%.0f%% | ShipRepair=%d/%d (%.0f%%)"), IntegrityPercent, CompletedTasks, TotalTasks, OverallPct)
	);

	PrintScreenDebug(
		FString::Printf(TEXT("[Spaceship Repair Status] Zone '%s' -> %s (%.0f%%) | Overall Ship Repair: %d/%d (%.0f%%)"),
			*ZoneOrTaskName,
			bIsCompleted ? TEXT("COMPLETED") : TEXT("INCOMPLETE / DAMAGED"),
			IntegrityPercent,
			CompletedTasks,
			TotalTasks,
			OverallPct),
		bIsCompleted ? FColor::Green : FColor::Orange,
		8.0f
	);

	OnAIShipEventTriggered.Broadcast(EventType, ZoneOrTaskName, OverallPct);

	if (bRequestAIReaction)
	{
		RequestAICoachingAndTimeEvaluation(
			FString::Printf(TEXT("%s: Zone '%s' is now %s (Ship Repair Progress: %d/%d = %.0f%%)"),
				*EventType, *ZoneOrTaskName, bIsCompleted ? TEXT("REPAIRED") : TEXT("DAMAGED"), CompletedTasks, TotalTasks, OverallPct)
		);
	}
}

float UMetaAIComponent::GetSpaceshipRepairProgressPercent(int32& OutCompletedTasks, int32& OutTotalTasks) const
{
	OutCompletedTasks = 0;
	OutTotalTasks = 0;

	UWorld* World = GetWorld();
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			if (const USpaceAIIdentityComponent* IdComp = Actor->FindComponentByClass<USpaceAIIdentityComponent>())
			{
				if (IdComp->bIsRepairTaskOrZone)
				{
					OutTotalTasks++;
					if (IdComp->bIsRepairTaskCompleted)
					{
						OutCompletedTasks++;
					}
				}
			}
		}
	}

	if (OutTotalTasks == 0 && RequiredSnapsToFinishMission > 0)
	{
		OutTotalTasks = RequiredSnapsToFinishMission;
		OutCompletedTasks = FMath::Clamp(SessionItemsSnappedCount, 0, RequiredSnapsToFinishMission);
	}

	if (OutTotalTasks <= 0)
	{
		return 0.0f;
	}

	return FMath::Clamp((static_cast<float>(OutCompletedTasks) / static_cast<float>(OutTotalTasks)) * 100.0f, 0.0f, 100.0f);
}

// =============================================================================
// 7. AI Spaceship Lighting Controller (RectLight, RectLight2 & All Ship Lights)
//    + Custom Ship Scenario & Consequence Evaluator
// =============================================================================

void UMetaAIComponent::DiscoverAndInitializeShipLights()
{
	TrackedShipLights.Reset();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 LightCounter = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		TArray<ULightComponent*> FoundLights;
		Actor->GetComponents<ULightComponent>(FoundLights);

		for (ULightComponent* LightComp : FoundLights)
		{
			if (!LightComp || !LightComp->IsRegistered())
			{
				continue;
			}

			// Ignore outdoor sun/sky lights; keep RectLight, RectLight2, PointLights, SpotLights inside the spaceship!
			if (LightComp->IsA<UDirectionalLightComponent>() || LightComp->IsA<USkyLightComponent>())
			{
				continue;
			}

			// Force Mobility to Movable so Meta AI can change color, brightness, flickering, and on/off at runtime!
			LightComp->SetMobility(EComponentMobility::Movable);

			FTrackedShipLightState NewLight;
			NewLight.LightComp = LightComp;
			NewLight.LightName = Actor->GetActorNameOrLabel();
			if (FoundLights.Num() > 1)
			{
				NewLight.LightName += FString::Printf(TEXT("_%s"), *LightComp->GetName());
			}
			NewLight.OriginalIntensity = FMath::Max(10.0f, LightComp->Intensity);
			NewLight.OriginalColor = LightComp->GetLightColor();
			NewLight.TargetColor = NewLight.OriginalColor;
			NewLight.TargetIntensityMultiplier = 1.0f;
			NewLight.ActiveMode = TEXT("solid");
			NewLight.AnimationSpeed = 2.0f;
			NewLight.LightIndex = LightCounter++;

			TrackedShipLights.Add(NewLight);
		}
	}

	if (TrackedShipLights.Num() > 0 && bPrintDiscoveredObjectsReport)
	{
		FString NamesList;
		for (const FTrackedShipLightState& L : TrackedShipLights)
		{
			if (!NamesList.IsEmpty()) NamesList += TEXT(", ");
			NamesList += L.LightName;
		}
		PrintScreenDebug(
			FString::Printf(TEXT("[MetaAI Lighting Director] Discovered %d Controllable Ship Lights: [%s]"),
				TrackedShipLights.Num(), *NamesList),
			FColor::Yellow,
			10.0f
		);
	}
}

void UMetaAIComponent::UpdateAnimatedShipLights(float DeltaTime)
{
	ShipLightAnimationTime += DeltaTime;

	for (FTrackedShipLightState& State : TrackedShipLights)
	{
		ULightComponent* LightComp = State.LightComp.Get();
		if (!LightComp)
		{
			continue;
		}

		const FString Mode = State.ActiveMode.ToLower();
		const float BaseTargetIntensity = State.OriginalIntensity * State.TargetIntensityMultiplier;

		if (Mode == TEXT("off") || Mode == TEXT("blackout") || State.TargetIntensityMultiplier <= 0.001f)
		{
			LightComp->SetIntensity(0.0f);
			continue;
		}

		LightComp->SetVisibility(true);
		LightComp->SetLightColor(State.TargetColor, true);

		if (Mode == TEXT("pulse_alarm") || Mode == TEXT("critical_pulse") || Mode == TEXT("pulse"))
		{
			// Smooth siren breathing between 12% and 135% intensity
			const float Wave = 0.5f + 0.5f * FMath::Sin(ShipLightAnimationTime * State.AnimationSpeed * 4.2f);
			const float AnimatedIntensity = BaseTargetIntensity * FMath::Lerp(0.12f, 1.35f, Wave);
			LightComp->SetIntensity(AnimatedIntensity);
		}
		else if (Mode == TEXT("flicker") || Mode == TEXT("broken_flicker") || Mode == TEXT("short_circuit"))
		{
			// Realistic electrical short-circuit flickering (turning off & on rapidly with random sparks)
			const float NoiseVal = FMath::PerlinNoise1D(ShipLightAnimationTime * State.AnimationSpeed * 6.5f + State.LightIndex * 13.7f);
			const float FlickerFactor = (NoiseVal > -0.15f) ? FMath::Clamp(0.7f + NoiseVal * 0.8f, 0.05f, 1.4f) : 0.0f;
			LightComp->SetIntensity(BaseTargetIntensity * FlickerFactor);
		}
		else if (Mode == TEXT("strobe"))
		{
			// Sharp emergency strobe ON / OFF
			const bool bStrobeOn = FMath::Sin(ShipLightAnimationTime * State.AnimationSpeed * 11.0f) > 0.0f;
			LightComp->SetIntensity(bStrobeOn ? (BaseTargetIntensity * 1.4f) : 0.0f);
		}
		else if (Mode == TEXT("alternating"))
		{
			// RectLight and RectLight2 alternate opposite to each other!
			const float PhaseOffset = (State.LightIndex % 2 == 0) ? 0.0f : PI;
			const float AltWave = FMath::Sin(ShipLightAnimationTime * State.AnimationSpeed * 5.0f + PhaseOffset);
			LightComp->SetIntensity((AltWave > 0.0f) ? (BaseTargetIntensity * 1.3f) : (BaseTargetIntensity * 0.05f));
		}
		else
		{
			// "solid"
			LightComp->SetIntensity(BaseTargetIntensity);
		}
	}
}

int32 UMetaAIComponent::ControlShipLights(const FString& TargetLightName, FLinearColor NewColor, float IntensityMultiplier, const FString& LightMode, float Speed)
{
	if (!bAllowAIToControlShipLights)
	{
		return 0;
	}

	if (TrackedShipLights.Num() == 0)
	{
		DiscoverAndInitializeShipLights();
	}

	const FString CleanTarget = TargetLightName.TrimStartAndEnd();
	const bool bTargetAll = CleanTarget.IsEmpty() ||
		CleanTarget.Equals(TEXT("ALL"), ESearchCase::IgnoreCase) ||
		CleanTarget.Equals(TEXT("ALL_LIGHTS"), ESearchCase::IgnoreCase);

	int32 UpdatedCount = 0;
	for (FTrackedShipLightState& State : TrackedShipLights)
	{
		if (!State.LightComp.IsValid())
		{
			continue;
		}

		const bool bMatches = bTargetAll ||
			State.LightName.Equals(CleanTarget, ESearchCase::IgnoreCase) ||
			State.LightName.Contains(CleanTarget, ESearchCase::IgnoreCase);

		if (bMatches)
		{
			const FString OldMode = State.ActiveMode;
			const float OldMult = State.TargetIntensityMultiplier;

			State.LightComp->SetMobility(EComponentMobility::Movable);
			State.TargetColor = NewColor;
			State.TargetIntensityMultiplier = FMath::Clamp(IntensityMultiplier, 0.0f, 5.0f);
			State.ActiveMode = LightMode.IsEmpty() ? TEXT("solid") : LightMode.ToLower();
			State.AnimationSpeed = FMath::Clamp(Speed, 0.2f, 10.0f);
			UpdatedCount++;

			// Record Before -> After light change for the Website Dashboard!
			TSharedPtr<FJsonObject> LightLogObj = MakeShareable(new FJsonObject());
			LightLogObj->SetStringField(TEXT("object_name"), State.LightName);
			LightLogObj->SetStringField(TEXT("object_role"), TEXT("SpaceshipLight"));
			LightLogObj->SetStringField(TEXT("size_before"), FString::Printf(TEXT("Mode: %s (%.1fx)"), *OldMode, OldMult));
			LightLogObj->SetStringField(TEXT("size_after"), FString::Printf(TEXT("Mode: %s (%.1fx)"), *State.ActiveMode, State.TargetIntensityMultiplier));
			LightLogObj->SetNumberField(TEXT("mass_before_kg"), OldMult);
			LightLogObj->SetNumberField(TEXT("mass_after_kg"), State.TargetIntensityMultiplier);
			LightLogObj->SetNumberField(TEXT("spin_before_deg"), 0.0);
			LightLogObj->SetNumberField(TEXT("spin_after_deg"), State.AnimationSpeed);
			LightLogObj->SetNumberField(TEXT("max_speed_before"), State.OriginalIntensity * OldMult);
			LightLogObj->SetNumberField(TEXT("max_speed_after"), State.OriginalIntensity * State.TargetIntensityMultiplier);
			LightLogObj->SetStringField(
				TEXT("changes_summary"),
				FString::Printf(TEXT("Light '%s' -> Mode: %s -> %s | Intensity: %.1fx -> %.1fx | RGB=[%d,%d,%d]"),
					*State.LightName,
					*OldMode,
					*State.ActiveMode,
					OldMult,
					State.TargetIntensityMultiplier,
					FMath::RoundToInt(NewColor.R * 255.0f),
					FMath::RoundToInt(NewColor.G * 255.0f),
					FMath::RoundToInt(NewColor.B * 255.0f))
			);
			LastObjectEditsArray.Add(MakeShareable(new FJsonValueObject(LightLogObj)));
		}
	}

	if (UpdatedCount > 0)
	{
		UpdateAnimatedShipLights(0.016f);
		LogTrainingEventToDatabase(
			TEXT("ai_light_control"),
			CleanTarget.IsEmpty() ? TEXT("ALL_LIGHTS") : CleanTarget,
			FString::Printf(TEXT("Mode=%s, Intensity=%.1fx, RGB=[%d,%d,%d]"),
				*LightMode,
				IntensityMultiplier,
				FMath::RoundToInt(NewColor.R * 255.0f),
				FMath::RoundToInt(NewColor.G * 255.0f),
				FMath::RoundToInt(NewColor.B * 255.0f))
		);
	}

	return UpdatedCount;
}

int32 UMetaAIComponent::ApplyShipLightingPreset(const FString& PresetName, const FString& TargetLightName)
{
	const FString CleanPreset = PresetName.TrimStartAndEnd().ToLower();

	if (CleanPreset.Contains(TEXT("critical")) || CleanPreset.Contains(TEXT("red")) || CleanPreset.Contains(TEXT("danger")) || CleanPreset.Contains(TEXT("alarm")))
	{
		return ControlShipLights(TargetLightName, FLinearColor(1.0f, 0.06f, 0.06f, 1.0f), 1.35f, TEXT("pulse_alarm"), 2.4f);
	}
	if (CleanPreset.Contains(TEXT("alternat")))
	{
		return ControlShipLights(TargetLightName, FLinearColor(1.0f, 0.05f, 0.05f, 1.0f), 1.4f, TEXT("alternating"), 2.5f);
	}
	if (CleanPreset.Contains(TEXT("flicker")) || CleanPreset.Contains(TEXT("damage")) || CleanPreset.Contains(TEXT("short")))
	{
		return ControlShipLights(TargetLightName, FLinearColor(1.0f, 0.38f, 0.1f, 1.0f), 1.15f, TEXT("flicker"), 3.0f);
	}
	if (CleanPreset.Contains(TEXT("warn")) || CleanPreset.Contains(TEXT("orange")) || CleanPreset.Contains(TEXT("amber")))
	{
		return ControlShipLights(TargetLightName, FLinearColor(1.0f, 0.55f, 0.05f, 1.0f), 1.15f, TEXT("pulse_alarm"), 1.6f);
	}
	if (CleanPreset.Contains(TEXT("blackout")) || CleanPreset.Contains(TEXT("off")) || CleanPreset.Contains(TEXT("dark")))
	{
		return ControlShipLights(TargetLightName, FLinearColor::Black, 0.0f, TEXT("off"), 1.0f);
	}
	if (CleanPreset.Contains(TEXT("green")) || CleanPreset.Contains(TEXT("repair")) || CleanPreset.Contains(TEXT("safe")))
	{
		return ControlShipLights(TargetLightName, FLinearColor(0.2f, 1.0f, 0.48f, 1.0f), 1.1f, TEXT("solid"), 1.0f);
	}

	// Default: "normal_white"
	return ControlShipLights(TargetLightName, FLinearColor(0.9f, 0.96f, 1.0f, 1.0f), 1.0f, TEXT("solid"), 1.0f);
}

void UMetaAIComponent::ReportShipScenarioEventToAI(const UObject* WorldContextObject, const FString& SourceItemName, const FString& TargetObjectOrZoneName, const FString& WhatHappenedDescription, bool bIsTaskCompleted)
{
	if (!WorldContextObject)
	{
		return;
	}

	UWorld* World = WorldContextObject->GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		if (UMetaAIComponent* MetaAI = Actor->FindComponentByClass<UMetaAIComponent>())
		{
			MetaAI->HandleCustomShipScenarioEvent(SourceItemName, TargetObjectOrZoneName, WhatHappenedDescription, bIsTaskCompleted);
			return;
		}
	}
}

void UMetaAIComponent::HandleCustomShipScenarioEvent(const FString& SourceItemName, const FString& TargetObjectOrZoneName, const FString& WhatHappenedDescription, bool bIsTaskCompleted)
{
	const FString CombinedText = (SourceItemName + TEXT(" ") + TargetObjectOrZoneName + TEXT(" ") + WhatHappenedDescription).ToLower();

	// 1. Check if this custom scenario is a CRITICAL HAZARD (e.g. Oxygen Cylinder near Burning Pipe / Fire / Explosion!)
	const bool bIsCriticalHazard =
		!bIsTaskCompleted &&
		(CombinedText.Contains(TEXT("oxygen")) || CombinedText.Contains(TEXT("اكسجين")) || CombinedText.Contains(TEXT("أكسجين")) ||
		 CombinedText.Contains(TEXT("burning")) || CombinedText.Contains(TEXT("fire")) || CombinedText.Contains(TEXT("مشتعل")) ||
		 CombinedText.Contains(TEXT("nar")) || CombinedText.Contains(TEXT("نار")) || CombinedText.Contains(TEXT("explosion")) ||
		 CombinedText.Contains(TEXT("انفجار")) || CombinedText.Contains(TEXT("danger")) || CombinedText.Contains(TEXT("critical")) ||
		 CombinedText.Contains(TEXT("خطر")) || CombinedText.Contains(TEXT("wrong")));

	if (bIsCriticalHazard)
	{
		// Immediately trigger AI Emergency Response: Critical Red Flashing Lights on RectLight/RectLight2 + Heavy Explosion Shake + Flying Debris!
		ApplyShipLightingPreset(TEXT("critical_red"), TEXT("ALL_LIGHTS"));
		TriggerShipTurbulenceAndCameraShake(1.55f, 4.0f, true, 2);
		SpawnSpaceObstacleForPlayer(TEXT("ExplosionDebris"), true, ObstacleLaunchSpeedTowardPlayer * 1.25f, 15.0f, 1.0f);

		LogTrainingEventToDatabase(
			TEXT("critical_ship_hazard"),
			SourceItemName,
			FString::Printf(TEXT("%s (%s)"), *TargetObjectOrZoneName, *WhatHappenedDescription)
		);

		PrintScreenDebug(
			FString::Printf(TEXT("=== [CRITICAL SHIP HAZARD!] '%s' + '%s': %s -> Red Alarm Lights, Explosion Shake & Flying Debris! ==="),
				*SourceItemName, *TargetObjectOrZoneName, *WhatHappenedDescription),
			FColor::Red,
			12.0f
		);
	}
	else if (bIsTaskCompleted)
	{
		// Custom Repair Completed (e.g. Valve Wheel placed on Pipe!)
		SessionItemsSnappedCount++;
		NotifySpaceshipZoneOrTaskUpdated(TargetObjectOrZoneName.IsEmpty() ? SourceItemName : TargetObjectOrZoneName, true, 100.0f, false);

		if (bHasActiveAIQuest)
		{
			const bool bMatchesTarget = TargetObjectOrZoneName.Contains(CurrentQuestTargetName, ESearchCase::IgnoreCase) ||
										CurrentQuestTargetName.Contains(TargetObjectOrZoneName, ESearchCase::IgnoreCase);
			const bool bMatchesTool = CurrentQuestRequiredToolName.IsEmpty() ||
									  SourceItemName.Contains(CurrentQuestRequiredToolName, ESearchCase::IgnoreCase) ||
									  CurrentQuestRequiredToolName.Contains(SourceItemName, ESearchCase::IgnoreCase);

			if (bMatchesTarget && bMatchesTool)
			{
				CompleteCurrentAIQuest(FString::Printf(TEXT("%s -> %s (%s)"), *SourceItemName, *TargetObjectOrZoneName, *WhatHappenedDescription));
				return;
			}
		}

		int32 CompletedTasks = 0;
		int32 TotalTasks = 0;
		const float OverallPct = GetSpaceshipRepairProgressPercent(CompletedTasks, TotalTasks);
		if (OverallPct >= 100.0f)
		{
			ApplyShipLightingPreset(TEXT("repaired_green"), TEXT("ALL_LIGHTS"));
			if (!bIsMissionFinished)
			{
				CompleteTrainingMission(true);
			}
		}
		else
		{
			ApplyShipLightingPreset(TEXT("normal_white"), TEXT("ALL_LIGHTS"));
		}

		LogTrainingEventToDatabase(
			TEXT("custom_repair_completed"),
			SourceItemName,
			FString::Printf(TEXT("%s (%s)"), *TargetObjectOrZoneName, *WhatHappenedDescription)
		);
	}
	else
	{
		LogTrainingEventToDatabase(
			TEXT("custom_ship_interaction"),
			SourceItemName,
			FString::Printf(TEXT("%s (%s)"), *TargetObjectOrZoneName, *WhatHappenedDescription)
		);
	}

	// Send the exact scenario to Meta AI so the AI model processes the logic and responds with full control over lighting, shake, obstacles, and physics!
	const FString ScenarioDirective = FString::Printf(
		TEXT("CUSTOM_SHIP_SCENARIO [Source='%s', Target='%s', TaskCompleted=%s]: %s. "
			 "Analyze what happens logically in the spaceship! Adjust RectLight & RectLight2 ('light_commands' -> critical_red, flicker_damage, blackout, or repaired_green), "
			 "trigger 'ship_events' (ship_shake_intensity, spawn_obstacle_count, hurl_existing_objects_count), and adjust object/player physics!"),
		*SourceItemName,
		*TargetObjectOrZoneName,
		bIsTaskCompleted ? TEXT("TRUE") : TEXT("FALSE"),
		*WhatHappenedDescription
	);

	SendWorldDirectiveToAI(ScenarioDirective);
}

void UMetaAIComponent::DiscoverAndInitializeLevelObstacles()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);

	// 1. Find any Actors in the Level that match LevelObstacleTagsOrNames (e.g. Actor Tag "AIObstacle" or Name) and add them to LevelObstacleActors!
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor == PlayerChar || Actor == GetOwner())
		{
			continue;
		}

		bool bMatchesTagOrName = false;
		for (const FName& TagOrName : LevelObstacleTagsOrNames)
		{
			if (TagOrName.IsNone())
			{
				continue;
			}
			const FString TagStr = TagOrName.ToString();
			if (Actor->ActorHasTag(TagOrName) ||
				Actor->GetName().Equals(TagStr, ESearchCase::IgnoreCase) ||
				Actor->GetActorNameOrLabel().Contains(TagStr, ESearchCase::IgnoreCase))
			{
				bMatchesTagOrName = true;
				break;
			}
		}

		if (bMatchesTagOrName)
		{
			LevelObstacleActors.AddUnique(Actor);
		}
	}

	// 2. Automatically equip every Actor in LevelObstacleActors with Zero-G Physics (USpaceObjectComponent) + AI Identity (USpaceAIIdentityComponent)
	// so even a plain StaticMeshActor placed in the Level can be controlled, scaled, spun, and hurled by Meta AI!
	int32 InitializedCount = 0;
	for (AActor* ObstacleActor : LevelObstacleActors)
	{
		if (!ObstacleActor || ObstacleActor == PlayerChar)
		{
			continue;
		}

		if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(ObstacleActor->GetRootComponent()))
		{
			Prim->SetMobility(EComponentMobility::Movable);
			Prim->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Prim->SetSimulatePhysics(true);
			Prim->SetEnableGravity(false);
			Prim->ComponentTags.AddUnique(TEXT("Grabbable"));
		}

		ObstacleActor->Tags.AddUnique(TEXT("Grabbable"));
		ObstacleActor->Tags.AddUnique(TEXT("AIObstacle"));

		USpaceObjectComponent* SpaceObj = ObstacleActor->FindComponentByClass<USpaceObjectComponent>();
		if (!SpaceObj)
		{
			SpaceObj = NewObject<USpaceObjectComponent>(ObstacleActor, TEXT("LevelObstacleSpaceObjComp"));
			SpaceObj->RegisterComponent();
			SpaceObj->SetZeroGEnabled(true);
		}

		USpaceAIIdentityComponent* IdComp = ObstacleActor->FindComponentByClass<USpaceAIIdentityComponent>();
		if (!IdComp)
		{
			IdComp = NewObject<USpaceAIIdentityComponent>(ObstacleActor, TEXT("LevelObstacleAIIdentityComp"));
			IdComp->EntityName = ObstacleActor->GetActorNameOrLabel();
			IdComp->EntityRole = ESpaceAIEntityRole::SpaceHazardObstacle;
			IdComp->DescriptionAndPurpose = TEXT("Existing level mesh obstacle under full Meta AI zero-G control.");
			IdComp->RegisterComponent();
		}

		InitializedCount++;
	}

	if (InitializedCount > 0 && bPrintDiscoveredObjectsReport)
	{
		PrintScreenDebug(
			FString::Printf(TEXT("[MetaAI Level Obstacles] Equipped %d existing Level Meshes/Actors for AI Control!"), InitializedCount),
			FColor::Cyan,
			10.0f
		);
	}
}

void UMetaAIComponent::ApplyAutonomousAIRiskResponse(const FString& InRiskLevel, float InRiskScore)
{
	if (bMasteryThresholdReached || ProblemSolvingProficiencyScore >= ProblemSolvingMasteryThreshold)
	{
		CurrentAIRiskLevel = TEXT("OPTIMAL");
		CurrentAIRiskScorePercent = 0.0f;
		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("normal_white"), TEXT("ALL_LIGHTS"));
		}
		return;
	}

	const FString CleanLevel = InRiskLevel.TrimStartAndEnd().ToUpper();
	CurrentAIRiskLevel = CleanLevel;
	CurrentAIRiskScorePercent = FMath::Clamp(InRiskScore, 0.0f, 100.0f);

	if (CleanLevel.Contains(TEXT("CRIT")) || CurrentAIRiskScorePercent >= 85.0f)
	{
		// CRITICAL RISK: Red flashing lights on RectLight/RectLight2 + Hull turbulence + Hurl level obstacles!
		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("critical_red"), TEXT("ALL_LIGHTS"));
		}
		if (bAllowAIToTriggerShipCameraShake && ActiveCameraShakeRemainingTime <= 0.1f)
		{
			TriggerShipTurbulenceAndCameraShake(1.15f, 3.0f, true, 1);
		}
	}
	else if (CleanLevel.Contains(TEXT("HIGH")) || CurrentAIRiskScorePercent >= 65.0f)
	{
		// HIGH RISK: Flickering damage / orange warning lights + moderate turbulence
		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("flicker_damage"), TEXT("ALL_LIGHTS"));
		}
		if (bAllowAIToTriggerShipCameraShake && ActiveCameraShakeRemainingTime <= 0.1f)
		{
			TriggerShipTurbulenceAndCameraShake(0.65f, 2.0f, true, 1);
		}
	}
	else if (CleanLevel.Contains(TEXT("MOD")) || CurrentAIRiskScorePercent >= 30.0f)
	{
		// MODERATE RISK: Warm warning pulse
		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("warning_orange"), TEXT("ALL_LIGHTS"));
		}
	}
	else
	{
		// LOW RISK: Safe / Normal lighting
		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("normal_white"), TEXT("ALL_LIGHTS"));
		}
	}

	PrintScreenDebug(
		FString::Printf(TEXT("[MetaAI Autonomous Risk Determination] Risk Level: %s (%.0f%%) | Active AI Mission: '%s'"),
			*CurrentAIRiskLevel, CurrentAIRiskScorePercent, *MissionName),
		(CurrentAIRiskScorePercent >= 65.0f) ? FColor::Red : FColor::Yellow,
		8.0f
	);
}

// =============================================================================
// 8. Endless Real-Time AI Game Master & 3D Quest Box + Floating Arrow Spawner
// =============================================================================

void UMetaAIComponent::DiscoverAllLevelToolsAndTerminals()
{
	DiscoveredLevelTools.Reset();
	DiscoveredLevelTargets.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);

	// 1. Add user-specified Tool & Target arrays first (strictly excluding scrap / debris)
	for (AActor* UserTool : QuestToolActorsInLevel)
	{
		if (UserTool && UserTool != PlayerChar)
		{
			const FString ToolNameLower = (UserTool->GetName() + TEXT(" ") + UserTool->GetActorNameOrLabel()).ToLower();
			const bool bIsDebris = ToolNameLower.Contains(TEXT("scrap")) || ToolNameLower.Contains(TEXT("debris")) ||
			                       ToolNameLower.Contains(TEXT("trash")) || ToolNameLower.Contains(TEXT("rubble"));
			if (!bIsDebris)
			{
				DiscoveredLevelTools.AddUnique(UserTool);
			}
		}
	}
	for (AActor* UserTarget : QuestTargetActorsInLevel)
	{
		if (UserTarget && UserTarget != PlayerChar)
		{
			DiscoveredLevelTargets.AddUnique(UserTarget);
		}
	}

	if (!bAutoDiscoverAllLevelToolsAndTerminals)
	{
		return;
	}

	// Keywords to classify Tools vs Terminals/Systems in the level automatically!
	static const TArray<FString> ToolKeywords = {
		TEXT("wrench"), TEXT("digi"), TEXT("tool"), TEXT("wheel"), TEXT("valvewheel"),
		TEXT("battery"), TEXT("cell"), TEXT("tank"), TEXT("cylinder"), TEXT("oxygen"),
		TEXT("canister"), TEXT("extinguisher"), TEXT("fuse"), TEXT("screwdriver"),
		TEXT("hammer"), TEXT("drill"), TEXT("welder"), TEXT("torch"), TEXT("cable"),
		TEXT("wire"), TEXT("card"), TEXT("key"), TEXT("drive"), TEXT("disk"),
		TEXT("scanner"), TEXT("tablet"), TEXT("medkit"), TEXT("kit"), TEXT("part"),
		TEXT("module"), TEXT("filter"), TEXT("bottle"), TEXT("can"), TEXT("pliers")
	};

	static const TArray<FString> TerminalTargetKeywords = {
		TEXT("terminal"), TEXT("elctric"), TEXT("electric"), TEXT("pipe"), TEXT("valve"),
		TEXT("console"), TEXT("generator"), TEXT("reactor"), TEXT("engine"), TEXT("pump"),
		TEXT("vent"), TEXT("switch"), TEXT("lever"), TEXT("button"), TEXT("breaker"),
		TEXT("junction"), TEXT("server"), TEXT("mainframe"), TEXT("computer"), TEXT("station"),
		TEXT("dock"), TEXT("slot"), TEXT("airlock"), TEXT("hatch"), TEXT("breach"),
		TEXT("leak"), TEXT("system"), TEXT("machine"), TEXT("unit"), TEXT("cabinet"),
		TEXT("rack"), TEXT("transformer"), TEXT("turbine"), TEXT("coolant"), TEXT("heater"),
		TEXT("lifesupport"), TEXT("radar"), TEXT("comms"), TEXT("antenna"), TEXT("sensor"),
		TEXT("core"), TEXT("power"), TEXT("boiler"), TEXT("control_panel"), TEXT("electric_panel")
	};

	static const TArray<FString> IgnoreArchitectureKeywords = {
		TEXT("roof"), TEXT("sm_roof"), TEXT("ceiling"), TEXT("sm_ceiling"),
		TEXT("floor"), TEXT("sm_floor"), TEXT("wall"), TEXT("sm_wall"),
		TEXT("corridor"), TEXT("beam"), TEXT("column"), TEXT("pillar"),
		TEXT("arch"), TEXT("frame"), TEXT("doorframe"), TEXT("glass"),
		TEXT("window"), TEXT("sky"), TEXT("fog"), TEXT("atmosphere"),
		TEXT("postprocess"), TEXT("navmesh"), TEXT("volume"), TEXT("camera"),
		TEXT("playerstart"), TEXT("gamemode"), TEXT("gamestate"), TEXT("brush"),
		TEXT("light"), TEXT("locaition_siez"), TEXT("location_size")
	};

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor == PlayerChar || Actor == GetOwner() ||
			Actor == ActiveQuestMarkerActor || Actor == ActiveToolMarkerActor)
		{
			continue;
		}

		// Skip pure light actors or controller/system actors without mesh/primitive
		if (!Actor->GetRootComponent() ||
			Actor->FindComponentByClass<UStaticMeshComponent>() == nullptr)
		{
			continue;
		}

		const FString ClassName = Actor->GetClass()->GetName();
		const FString ActorNameAndLabel = (Actor->GetName() + TEXT(" ") + Actor->GetActorNameOrLabel()).ToLower();

		bool bShouldIgnore = false;
		for (const FString& Ign : IgnoreArchitectureKeywords)
		{
			if (ActorNameAndLabel.Contains(Ign, ESearchCase::IgnoreCase) || ClassName.Contains(Ign, ESearchCase::IgnoreCase))
			{
				bShouldIgnore = true;
				break;
			}
		}

		// NEVER pick structural walls, roof panels, or ceiling architecture as targets or tools!
		if (bShouldIgnore)
		{
			continue;
		}

		// Strictly EXCLUDE the permanent Space Tablet Quiz device (Quest_main) from being classified as a breakable target or tool!
		if (IsPermanentQuizTabletActor(Actor))
		{
			continue;
		}

		USpaceAIIdentityComponent* IdComp = Actor->FindComponentByClass<USpaceAIIdentityComponent>();
		USpaceObjectComponent* SpaceObj = Actor->FindComponentByClass<USpaceObjectComponent>();

		if (IdComp && IdComp->EntityRole == ESpaceAIEntityRole::AstronautPlayer)
		{
			continue;
		}

		// Exclude scrap/debris from being assigned as primary astronaut tools
		const bool bIsDebris = ActorNameAndLabel.Contains(TEXT("scrap")) || ActorNameAndLabel.Contains(TEXT("debris")) ||
		                       ActorNameAndLabel.Contains(TEXT("trash")) || ActorNameAndLabel.Contains(TEXT("rubble"));

		// Check if it matches Tool keywords or is a Grabbable Tool
		bool bIsTool = !bIsDebris && (Actor->ActorHasTag(TEXT("Tool")) || (IdComp && IdComp->EntityRole == ESpaceAIEntityRole::MissionToolOrItem));

		if (!bIsTool && !bIsDebris)
		{
			for (const FString& Kw : ToolKeywords)
			{
				if (ActorNameAndLabel.Contains(Kw, ESearchCase::IgnoreCase))
				{
					bIsTool = true;
					break;
				}
			}
		}

		// Check if it matches Terminal / Target keywords or is a Repair Zone / Interactive Station
		bool bIsTarget = Actor->ActorHasTag(TEXT("Interactable")) ||
			(IdComp && (IdComp->bIsRepairTaskOrZone || IdComp->bCanInteractWithEKey ||
						IdComp->EntityRole == ESpaceAIEntityRole::SpaceshipRepairZone ||
						IdComp->EntityRole == ESpaceAIEntityRole::StationEnvironment ||
						IdComp->EntityRole == ESpaceAIEntityRole::SnapTargetSlot));

		if (!bIsTarget)
		{
			for (const FString& Kw : TerminalTargetKeywords)
			{
				if (ActorNameAndLabel.Contains(Kw, ESearchCase::IgnoreCase))
				{
					bIsTarget = true;
					break;
				}
			}
		}

		if (bIsTool)
		{
			DiscoveredLevelTools.AddUnique(Actor);

			// Ensure the Tool is Grabbable and exposed to Meta AI!
			Actor->Tags.AddUnique(TEXT("Grabbable"));
			if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Actor->GetRootComponent()))
			{
				Prim->SetMobility(EComponentMobility::Movable);
				Prim->ComponentTags.AddUnique(TEXT("Grabbable"));
			}
			if (!IdComp)
			{
				IdComp = NewObject<USpaceAIIdentityComponent>(Actor, TEXT("AutoToolAIIdentityComp"));
				IdComp->EntityName = Actor->GetActorNameOrLabel();
				IdComp->EntityRole = ESpaceAIEntityRole::MissionToolOrItem;
				IdComp->DescriptionAndPurpose = FString::Printf(TEXT("Portable astronaut tool/item '%s' used for repairing ship terminals and systems."), *IdComp->EntityName);
				IdComp->RegisterComponent();
			}
		}
		else if (bIsTarget)
		{
			DiscoveredLevelTargets.AddUnique(Actor);

			// Ensure the Terminal/Target is Interactable with [E] and exposed to Meta AI!
			Actor->Tags.AddUnique(TEXT("Interactable"));
			if (!IdComp)
			{
				IdComp = NewObject<USpaceAIIdentityComponent>(Actor, TEXT("AutoTargetAIIdentityComp"));
				IdComp->EntityName = Actor->GetActorNameOrLabel();
				IdComp->EntityRole = ESpaceAIEntityRole::SpaceshipRepairZone;
				IdComp->bIsRepairTaskOrZone = true;
				IdComp->bCanInteractWithEKey = true;
				IdComp->SpaceshipZoneName = IdComp->EntityName;
				IdComp->DescriptionAndPurpose = FString::Printf(TEXT("Spaceship terminal/system '%s' that can malfunction and be repaired using tools."), *IdComp->EntityName);
				IdComp->RegisterComponent();
			}
			else
			{
				IdComp->bCanInteractWithEKey = true;
				IdComp->bIsRepairTaskOrZone = true;
			}
		}
		else if (!bShouldIgnore && !bIsDebris && (SpaceObj || IdComp))
		{
			// Any other physics prop or labeled Actor can serve as a Tool if movable, or Target if static!
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
			if (SpaceObj || (Prim && Prim->IsSimulatingPhysics()))
			{
				DiscoveredLevelTools.AddUnique(Actor);
			}
			else
			{
				DiscoveredLevelTargets.AddUnique(Actor);
			}
		}
	}

	if (bPrintDiscoveredObjectsReport && (DiscoveredLevelTools.Num() > 0 || DiscoveredLevelTargets.Num() > 0))
	{
		FString ToolNames, TargetNames;
		for (AActor* T : DiscoveredLevelTools)
		{
			if (!T) continue;
			if (!ToolNames.IsEmpty()) ToolNames += TEXT(", ");
			ToolNames += T->GetActorNameOrLabel();
		}
		for (AActor* Tg : DiscoveredLevelTargets)
		{
			if (!Tg) continue;
			if (!TargetNames.IsEmpty()) TargetNames += TEXT(", ");
			TargetNames += Tg->GetActorNameOrLabel();
		}

		PrintScreenDebug(
			FString::Printf(TEXT("[AI Game Master Level Scanner] Tools (%d): [%s] | Terminals/Targets (%d): [%s]"),
				DiscoveredLevelTools.Num(),
				ToolNames.IsEmpty() ? TEXT("None") : *ToolNames,
				DiscoveredLevelTargets.Num(),
				TargetNames.IsEmpty() ? TEXT("None") : *TargetNames),
			FColor::Turquoise,
			12.0f
		);
	}
}

AActor* UMetaAIComponent::PickNextToolActorInShip(AActor* ForTargetActor) const
{
	TArray<AActor*> PrimaryMatchTools;
	TArray<AActor*> GeneralValidTools;

	const FString TargetName = ForTargetActor ? ForTargetActor->GetActorNameOrLabel().ToLower() : TEXT("");
	const bool bIsElectrical = TargetName.Contains(TEXT("elctric")) || TargetName.Contains(TEXT("electric")) ||
	                           TargetName.Contains(TEXT("terminal")) || TargetName.Contains(TEXT("console")) ||
	                           TargetName.Contains(TEXT("server")) || TargetName.Contains(TEXT("computer")) ||
	                           TargetName.Contains(TEXT("generator")) || TargetName.Contains(TEXT("circuit")) ||
	                           TargetName.Contains(TEXT("breaker")) || TargetName.Contains(TEXT("power"));
	const bool bIsFluidOrPipe = TargetName.Contains(TEXT("pipe")) || TargetName.Contains(TEXT("valve")) ||
	                            TargetName.Contains(TEXT("tank")) || TargetName.Contains(TEXT("oxygen")) ||
	                            TargetName.Contains(TEXT("coolant")) || TargetName.Contains(TEXT("pump")) ||
	                            TargetName.Contains(TEXT("airlock")) || TargetName.Contains(TEXT("hydraulic"));
	const bool bIsHazardOrFire = TargetName.Contains(TEXT("fire")) || TargetName.Contains(TEXT("spark")) ||
	                             TargetName.Contains(TEXT("breach")) || TargetName.Contains(TEXT("leak")) ||
	                             TargetName.Contains(TEXT("vent")) || TargetName.Contains(TEXT("hazard"));

	for (AActor* ToolAct : DiscoveredLevelTools)
	{
		if (!ToolAct || !IsValid(ToolAct) || ToolAct == ForTargetActor ||
			ToolAct == ActiveQuestMarkerActor || ToolAct == ActiveToolMarkerActor ||
			IsPermanentQuizTabletActor(ToolAct))
		{
			continue;
		}

		const FString ToolName = ToolAct->GetActorNameOrLabel().ToLower();

		// Strictly reject any debris, scrap, rubble, trash, or structural panels from ever being picked as a tool!
		if (ToolName.Contains(TEXT("scrap")) || ToolName.Contains(TEXT("debris")) || ToolName.Contains(TEXT("rubble")) ||
			ToolName.Contains(TEXT("trash")) || ToolName.Contains(TEXT("panel")) || ToolName.Contains(TEXT("roof")) ||
			ToolName.Contains(TEXT("wall")) || ToolName.Contains(TEXT("floor")) || ToolName.Contains(TEXT("ceiling")) ||
			ToolName.Contains(TEXT("door")) || ToolName.Contains(TEXT("window")))
		{
			continue;
		}

		// Evaluate contextual suitability for the broken target
		bool bIsContextMatch = false;
		if (bIsElectrical)
		{
			bIsContextMatch = ToolName.Contains(TEXT("wrench")) || ToolName.Contains(TEXT("digi")) ||
			                  ToolName.Contains(TEXT("screwdriver")) || ToolName.Contains(TEXT("pliers")) ||
			                  ToolName.Contains(TEXT("fuse")) || ToolName.Contains(TEXT("cable")) ||
			                  ToolName.Contains(TEXT("battery")) || ToolName.Contains(TEXT("scanner")) ||
			                  ToolName.Contains(TEXT("tablet")) || ToolName.Contains(TEXT("card"));
		}
		else if (bIsFluidOrPipe)
		{
			bIsContextMatch = ToolName.Contains(TEXT("valve")) || ToolName.Contains(TEXT("wheel")) ||
			                  ToolName.Contains(TEXT("wrench")) || ToolName.Contains(TEXT("torch")) ||
			                  ToolName.Contains(TEXT("welder")) || ToolName.Contains(TEXT("cylinder")) ||
			                  ToolName.Contains(TEXT("tank")) || ToolName.Contains(TEXT("canister"));
		}
		else if (bIsHazardOrFire)
		{
			bIsContextMatch = ToolName.Contains(TEXT("extinguisher")) || ToolName.Contains(TEXT("foam")) ||
			                  ToolName.Contains(TEXT("welder")) || ToolName.Contains(TEXT("torch")) ||
			                  ToolName.Contains(TEXT("canister")) || ToolName.Contains(TEXT("wrench"));
		}

		if (bIsContextMatch)
		{
			PrimaryMatchTools.AddUnique(ToolAct);
		}
		else
		{
			GeneralValidTools.AddUnique(ToolAct);
		}
	}

	// 1. Try to pick a fresh matching tool that belongs to a fresh tool category and wasn't used recently!
	TArray<AActor*> FreshCategoryPrimaryTools;
	TArray<AActor*> FreshActorPrimaryTools;
	for (AActor* Act : PrimaryMatchTools)
	{
		const FString ToolCat = ClassifyToolCategory(Act, Act->GetActorNameOrLabel());
		const bool bRecentCat = RecentlyUsedToolCategories.Contains(ToolCat);
		const bool bRecentActor = RecentlyUsedQuestToolActors.Contains(Act) || (LastUsedQuestToolActor.IsValid() && Act == LastUsedQuestToolActor.Get());

		if (!bRecentCat && !bRecentActor)
		{
			FreshCategoryPrimaryTools.Add(Act);
		}
		else if (!bRecentActor)
		{
			FreshActorPrimaryTools.Add(Act);
		}
	}

	if (FreshCategoryPrimaryTools.Num() > 0)
	{
		return FreshCategoryPrimaryTools[FMath::RandRange(0, FreshCategoryPrimaryTools.Num() - 1)];
	}
	if (FreshActorPrimaryTools.Num() > 0)
	{
		return FreshActorPrimaryTools[FMath::RandRange(0, FreshActorPrimaryTools.Num() - 1)];
	}
	if (PrimaryMatchTools.Num() > 0)
	{
		return PrimaryMatchTools[FMath::RandRange(0, PrimaryMatchTools.Num() - 1)];
	}

	// 2. Fallback to general valid non-scrap tools
	TArray<AActor*> FreshGeneralTools;
	for (AActor* Act : GeneralValidTools)
	{
		if (Act != LastUsedQuestToolActor.Get())
		{
			FreshGeneralTools.Add(Act);
		}
	}
	if (FreshGeneralTools.Num() > 0)
	{
		return FreshGeneralTools[FMath::RandRange(0, FreshGeneralTools.Num() - 1)];
	}
	if (GeneralValidTools.Num() > 0)
	{
		return GeneralValidTools[FMath::RandRange(0, GeneralValidTools.Num() - 1)];
	}

	// 3. Fallback to any discovered tool so a valid tool is ALWAYS picked
	for (AActor* AnyTool : DiscoveredLevelTools)
	{
		if (AnyTool && IsValid(AnyTool) && AnyTool != ForTargetActor &&
			AnyTool != ActiveQuestMarkerActor && AnyTool != ActiveToolMarkerActor)
		{
			return AnyTool;
		}
	}

	return nullptr;
}

void UMetaAIComponent::SpawnOrUpdate3DToolMarker(AActor* ToolActor, const FString& ToolName, const FString& TargetName)
{
	UWorld* World = GetWorld();
	if (!World || !bSpawn3DQuestBoxAndArrow)
	{
		return;
	}

	ActiveQuestRequiredToolActor = ToolActor;

	if (!ToolActor || !IsValid(ToolActor))
	{
		if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
		{
			ActiveToolMarkerActor->SetActorHiddenInGame(true);
		}
		return;
	}

	if (!ActiveToolMarkerActor || !IsValid(ActiveToolMarkerActor))
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ActiveToolMarkerActor = World->SpawnActor<AActor>(AActor::StaticClass(), ToolActor->GetActorLocation(), FRotator::ZeroRotator, SpawnParams);
		if (!ActiveToolMarkerActor)
		{
			return;
		}

		USceneComponent* RootScene = NewObject<USceneComponent>(ActiveToolMarkerActor, TEXT("ToolMarkerRoot"));
		ActiveToolMarkerActor->SetRootComponent(RootScene);
		RootScene->RegisterComponent();

		ActiveToolArrowComp = NewObject<UArrowComponent>(ActiveToolMarkerActor, TEXT("AIToolFloatingArrow"));
		ActiveToolArrowComp->SetupAttachment(RootScene);
		ActiveToolArrowComp->SetRelativeLocation(FVector(0.0f, 0.0f, 80.0f));
		ActiveToolArrowComp->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
		ActiveToolArrowComp->ArrowColor = FColor(0, 240, 255, 255); // Bright Cyan for Tool!
		ActiveToolArrowComp->ArrowSize = 2.2f;
		ActiveToolArrowComp->SetHiddenInGame(false);
		ActiveToolArrowComp->RegisterComponent();

		ActiveToolArrowMeshComp = NewObject<UStaticMeshComponent>(ActiveToolMarkerActor, TEXT("AIToolArrowConeMesh"));
		ActiveToolArrowMeshComp->SetupAttachment(RootScene);
		ActiveToolArrowMeshComp->SetRelativeLocation(FVector(0.0f, 0.0f, 62.0f));
		ActiveToolArrowMeshComp->SetRelativeRotation(FRotator(180.0f, 0.0f, 0.0f));
		ActiveToolArrowMeshComp->SetRelativeScale3D(FVector(0.32f, 0.32f, 0.50f));
		ActiveToolArrowMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone")))
		{
			ActiveToolArrowMeshComp->SetStaticMesh(ConeMesh);
		}
		ActiveToolArrowMeshComp->RegisterComponent();

		ActiveToolTextComp = NewObject<UTextRenderComponent>(ActiveToolMarkerActor, TEXT("AITool3DText"));
		ActiveToolTextComp->SetupAttachment(RootScene);
		ActiveToolTextComp->SetRelativeLocation(FVector(0.0f, 0.0f, 105.0f));
		ActiveToolTextComp->SetHorizontalAlignment(EHTA_Center);
		ActiveToolTextComp->SetVerticalAlignment(EVRTA_TextCenter);
		ActiveToolTextComp->SetWorldSize(18.5f);
		ActiveToolTextComp->SetTextRenderColor(FColor(50, 245, 255, 255));
		ActiveToolTextComp->SetHiddenInGame(false);
		ActiveToolTextComp->RegisterComponent();

		// 3D Wireframe Indicator Box around the Tool
		ActiveToolBoxComp = NewObject<UBoxComponent>(ActiveToolMarkerActor, TEXT("AIToolBoxComp"));
		ActiveToolBoxComp->SetupAttachment(RootScene);
		ActiveToolBoxComp->InitBoxExtent(FVector(32.0f, 32.0f, 32.0f));
		ActiveToolBoxComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ActiveToolBoxComp->SetHiddenInGame(false);
		ActiveToolBoxComp->bDrawOnlyIfSelected = false;
		ActiveToolBoxComp->ShapeColor = FColor(0, 240, 255, 255);
		ActiveToolBoxComp->SetLineThickness(3.5f);
		ActiveToolBoxComp->RegisterComponent();
	}

	FVector ToolOrigin, ToolBoundsExtent;
	ToolActor->GetActorBounds(false, ToolOrigin, ToolBoundsExtent);
	const FVector EffectiveToolExtent = FVector::Max(ToolBoundsExtent + FVector(10.0f), FVector(26.0f, 26.0f, 26.0f));

	ActiveToolMarkerActor->SetActorHiddenInGame(false);
	ActiveToolMarkerActor->SetActorLocation(ToolBoundsExtent.Size() > 5.0f ? ToolOrigin : ToolActor->GetActorLocation());

	if (ActiveToolBoxComp)
	{
		ActiveToolBoxComp->SetBoxExtent(EffectiveToolExtent);
		ActiveToolBoxComp->MarkRenderStateDirty();
	}

	if (ActiveToolTextComp)
	{
		ActiveToolTextComp->SetText(FText::FromString(FString::Printf(
			TEXT("[STEP 1: GRAB TOOL] %s\n>> Take to %s <<"),
			*ToolName,
			*TargetName
		)));
	}
}

void UMetaAIComponent::HandleTrackedPlayerInteractWithE(AActor* InteractedActor, UPrimitiveComponent* InteractedComponent, FName ComponentName)
{
	if (!InteractedActor)
	{
		return;
	}

	const FString InteractedName = InteractedActor->GetActorNameOrLabel().ToLower();
	if (InteractedName.Contains(TEXT("oxygen")) || InteractedName.Contains(TEXT("o2")) || InteractedName.Contains(TEXT("tank")))
	{
		RefillPlayerOxygen(100.0f);
		RefillRoomOxygen(40.0f);
		PrintScreenDebug(TEXT("[Life Support] Suit Oxygen Tank 100% Replenished!"), FLinearColor(0.2f, 0.9f, 1.0f).ToFColor(true), 4.0f);
	}

	// Orientation Training Step 4 -> Complete & Certify
	if (CurrentTrainingPhase == EAstronautTrainingPhase::Step4_CarryAndCalibrate)
	{
		CompleteOrientationTrainingAndCertify();
		return;
	}

	// Check if this actor is or has the SpaceTabletQuizComponent
	USpaceTabletQuizComponent* TabletQuizComp = InteractedActor->FindComponentByClass<USpaceTabletQuizComponent>();
	if (!TabletQuizComp && (InteractedName.Contains(TEXT("tablet")) || InteractedName.Contains(TEXT("scanner")) || InteractedName.Contains(TEXT("quiz")) || InteractedName.Contains(TEXT("quest_main"))))
	{
		TabletQuizComp = USpaceTabletQuizComponent::GetSpaceTabletQuizComponent(this);
	}

	if (TabletQuizComp)
	{
		TabletQuizComp->EnsureDefaultWidgetClass();
		TabletQuizComp->ToggleTabletUI();
		PrintScreenDebug(TEXT("[Diagnostic Tablet] Opened AI Technical Dilemma Interface!"), FColor::Cyan, 5.0f);
		return;
	}

	// If the player pressed [E] on the 3D Quest Marker Box OR on the Broken Target Actor itself
	if (bHasActiveAIQuest &&
		(InteractedActor == ActiveQuestMarkerActor ||
		 InteractedActor == ActiveQuestTargetActor.Get() ||
		 (ActiveQuestTargetActor.IsValid() && InteractedActor->GetName() == ActiveQuestTargetActor->GetName())))
	{
		// Strict validation: Player must be ACTIVELY HOLDING the required tool!
		const bool bQuestRequiresTool = bRequireToolForToolQuests && (!CurrentQuestRequiredToolName.IsEmpty() || ActiveQuestRequiredToolActor.IsValid());
		if (bQuestRequiresTool)
		{
			AActor* HeldActor = nullptr;
			if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(GetWorld(), 0))
			{
				if (const USpaceGrabSnapComponent* GrabComp = PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>())
				{
					HeldActor = GrabComp->GetHeldActor();
				}
			}

			bool bHoldingCorrectTool = false;
			FString HeldToolName = TEXT("Bare Hands");

			if (HeldActor)
			{
				if (const USpaceAIIdentityComponent* HeldId = HeldActor->FindComponentByClass<USpaceAIIdentityComponent>())
				{
					HeldToolName = HeldId->GetEffectiveEntityName();
				}
				else
				{
					HeldToolName = HeldActor->GetActorNameOrLabel();
				}

				if (ActiveQuestRequiredToolActor.IsValid() && HeldActor == ActiveQuestRequiredToolActor.Get())
				{
					bHoldingCorrectTool = true;
				}
				else if (!CurrentQuestRequiredToolName.IsEmpty())
				{
					if (HeldToolName.Contains(CurrentQuestRequiredToolName, ESearchCase::IgnoreCase) ||
						CurrentQuestRequiredToolName.Contains(HeldToolName, ESearchCase::IgnoreCase))
					{
						bHoldingCorrectTool = true;
					}
				}
			}

			if (!bHoldingCorrectTool)
			{
				if (HeldActor)
				{
					TransmitFlightDirectorRadioComms(
						FString::Printf(TEXT("Flight Director: Incorrect tool! You are holding '%s', but '%s' requires '%s'. Follow the Cyan beacon!"),
							*HeldToolName, *CurrentQuestTargetName, *CurrentQuestRequiredToolName),
						6.0f
					);
					PrintScreenDebug(
						FString::Printf(TEXT("[AI Game Master] WRONG TOOL: '%s' cannot repair '%s'! You need '%s' (Cyan Arrow)!"),
							*HeldToolName, *CurrentQuestTargetName, *CurrentQuestRequiredToolName),
						FColor(255, 60, 40),
						6.0f
					);
				}
				else
				{
					TransmitFlightDirectorRadioComms(
						FString::Printf(TEXT("Flight Director: Service interlock locked! You need the '%s' to repair '%s'. Follow the CYAN marker first!"),
							*CurrentQuestRequiredToolName, *CurrentQuestTargetName),
						6.0f
					);
					PrintScreenDebug(
						FString::Printf(TEXT("[AI Game Master] You need '%s' to repair '%s'! Follow the CYAN Arrow to grab '%s' first!"),
						*CurrentQuestRequiredToolName, *CurrentQuestTargetName, *CurrentQuestRequiredToolName),
						FColor::Orange,
						7.0f
					);
				}
				return;
			}
		}

		const FString ToolUsedNote = CurrentQuestRequiredToolName.IsEmpty()
			? TEXT("Direct [E] Repair")
			: FString::Printf(TEXT("Used Tool '%s'"), *CurrentQuestRequiredToolName);

		CompleteCurrentAIQuest(FString::Printf(TEXT("Astronaut repaired '%s' (%s)"), *CurrentQuestTargetName, *ToolUsedNote));
	}
}

FString UMetaAIComponent::ClassifyTargetSystemCategory(AActor* InActor, const FString& InTargetName)
{
	FString Combined;
	if (!InTargetName.IsEmpty())
	{
		Combined = InTargetName.ToLower();
	}
	if (InActor && IsValid(InActor) && !InActor->IsUnreachable())
	{
		Combined += TEXT(" ") + InActor->GetName().ToLower() + TEXT(" ") + InActor->GetActorNameOrLabel().ToLower();
		if (const USpaceAIIdentityComponent* Id = InActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			if (IsValid(Id))
			{
				Combined += TEXT(" ") + Id->EntityName.ToLower() + TEXT(" ") + Id->SpaceshipZoneName.ToLower();
			}
		}
	}

	if (Combined.Contains(TEXT("pipe")) || Combined.Contains(TEXT("valve")) || Combined.Contains(TEXT("coolant")) ||
		Combined.Contains(TEXT("hydraulic")) || Combined.Contains(TEXT("tank")) || Combined.Contains(TEXT("boiler")) ||
		Combined.Contains(TEXT("conduit")) || Combined.Contains(TEXT("pump")) || Combined.Contains(TEXT("blender")))
	{
		return TEXT("COOLANT_FLUID_PIPES");
	}
	if (Combined.Contains(TEXT("terminal")) || Combined.Contains(TEXT("elctric")) || Combined.Contains(TEXT("electric")) ||
		Combined.Contains(TEXT("breaker")) || Combined.Contains(TEXT("junction")) || Combined.Contains(TEXT("circuit")) ||
		Combined.Contains(TEXT("transformer")) || Combined.Contains(TEXT("power")) || Combined.Contains(TEXT("switch")) ||
		Combined.Contains(TEXT("relay")) || Combined.Contains(TEXT("fuse")))
	{
		return TEXT("ELECTRICAL_POWER_BUS");
	}
	if (Combined.Contains(TEXT("console")) || Combined.Contains(TEXT("server")) || Combined.Contains(TEXT("mainframe")) ||
		Combined.Contains(TEXT("computer")) || Combined.Contains(TEXT("control")) || Combined.Contains(TEXT("radar")) ||
		Combined.Contains(TEXT("comms")) || Combined.Contains(TEXT("antenna")) || Combined.Contains(TEXT("sensor")) ||
		Combined.Contains(TEXT("security_panel")) || Combined.Contains(TEXT("pda")))
	{
		return TEXT("AVIONICS_COMPUTER_SYSTEMS");
	}
	if (Combined.Contains(TEXT("generator")) || Combined.Contains(TEXT("reactor")) || Combined.Contains(TEXT("engine")) ||
		Combined.Contains(TEXT("core")) || Combined.Contains(TEXT("turbine")))
	{
		return TEXT("PROPULSION_REACTOR_CORE");
	}
	if (Combined.Contains(TEXT("lifesupport")) || Combined.Contains(TEXT("vent")) || Combined.Contains(TEXT("airlock")) ||
		Combined.Contains(TEXT("hatch")) || Combined.Contains(TEXT("filter")) || Combined.Contains(TEXT("heater")) ||
		Combined.Contains(TEXT("oxygen")) || Combined.Contains(TEXT("scrubber")) || Combined.Contains(TEXT("breach")) ||
		Combined.Contains(TEXT("leak")))
	{
		return TEXT("LIFE_SUPPORT_ATMOSPHERE");
	}

	return TEXT("GENERAL_STATION_HARDWARE");
}

FString UMetaAIComponent::ClassifyToolCategory(AActor* InActor, const FString& InToolName)
{
	FString Combined;
	if (!InToolName.IsEmpty())
	{
		Combined = InToolName.ToLower();
	}
	if (InActor && IsValid(InActor) && !InActor->IsUnreachable())
	{
		Combined += TEXT(" ") + InActor->GetName().ToLower() + TEXT(" ") + InActor->GetActorNameOrLabel().ToLower();
	}

	if (Combined.Contains(TEXT("valve")) || Combined.Contains(TEXT("wheel")))
	{
		return TEXT("VALVE_WHEEL");
	}
	if (Combined.Contains(TEXT("wrench")) || Combined.Contains(TEXT("digi")) || Combined.Contains(TEXT("screwdriver")) || Combined.Contains(TEXT("pliers")))
	{
		return TEXT("DIGI_WRENCH");
	}
	if (Combined.Contains(TEXT("extinguisher")) || Combined.Contains(TEXT("foam")))
	{
		return TEXT("FIRE_EXTINGUISHER");
	}
	if (Combined.Contains(TEXT("battery")) || Combined.Contains(TEXT("cell")) || Combined.Contains(TEXT("fuse")) || Combined.Contains(TEXT("cable")) || Combined.Contains(TEXT("wire")) || Combined.Contains(TEXT("multimeter")))
	{
		return TEXT("POWER_CELL_FUSE");
	}
	if (Combined.Contains(TEXT("welder")) || Combined.Contains(TEXT("torch")) || Combined.Contains(TEXT("blow_torch")) || Combined.Contains(TEXT("cylinder")))
	{
		return TEXT("PLASMA_WELDER");
	}
	if (Combined.Contains(TEXT("tablet")) || Combined.Contains(TEXT("scanner")) || Combined.Contains(TEXT("card")) || Combined.Contains(TEXT("key")) || Combined.Contains(TEXT("drive")) || Combined.Contains(TEXT("pda")))
	{
		return TEXT("DIAGNOSTIC_TABLET");
	}
	if (Combined.Contains(TEXT("oxjyen")) || Combined.Contains(TEXT("oxygen")) || Combined.Contains(TEXT("canister")))
	{
		return TEXT("OXYGEN_CANISTER");
	}

	return TEXT("GENERIC_TOOL");
}

void UMetaAIComponent::RecordQuestTargetAndToolUsage(AActor* InTarget, const FString& InTargetName, AActor* InTool, const FString& InToolName)
{
	// Each history list holds UNIQUE entries (most recent last). Re-recording the same quest just moves it to the end,
	// so the same quest being recorded at precompute/activation/completion never floods the history.
	if (InTarget && IsValid(InTarget))
	{
		RecentlyBrokenQuestActors.Remove(InTarget);
		RecentlyBrokenQuestActors.Add(InTarget);
		while (RecentlyBrokenQuestActors.Num() > 4)
		{
			RecentlyBrokenQuestActors.RemoveAt(0);
		}
	}

	const FString TargetCategory = ClassifyTargetSystemCategory(InTarget, InTargetName);
	RecentlyUsedTargetCategories.Remove(TargetCategory);
	RecentlyUsedTargetCategories.Add(TargetCategory);
	while (RecentlyUsedTargetCategories.Num() > 2)
	{
		RecentlyUsedTargetCategories.RemoveAt(0);
	}

	FString CleanTargetName = InTargetName.TrimStartAndEnd().ToLower();
	if (CleanTargetName.IsEmpty() && InTarget && IsValid(InTarget))
	{
		CleanTargetName = InTarget->GetActorNameOrLabel().ToLower();
	}
	if (!CleanTargetName.IsEmpty())
	{
		RecentlyUsedTargetNames.Remove(CleanTargetName);
		RecentlyUsedTargetNames.Add(CleanTargetName);
		while (RecentlyUsedTargetNames.Num() > 3)
		{
			RecentlyUsedTargetNames.RemoveAt(0);
		}
	}

	if (InTool && IsValid(InTool))
	{
		RecentlyUsedQuestToolActors.Remove(InTool);
		RecentlyUsedQuestToolActors.Add(InTool);
		while (RecentlyUsedQuestToolActors.Num() > 3)
		{
			RecentlyUsedQuestToolActors.RemoveAt(0);
		}
		LastUsedQuestToolActor = InTool;
	}

	const FString ToolCategory = ClassifyToolCategory(InTool, InToolName);
	RecentlyUsedToolCategories.Remove(ToolCategory);
	RecentlyUsedToolCategories.Add(ToolCategory);
	while (RecentlyUsedToolCategories.Num() > 2)
	{
		RecentlyUsedToolCategories.RemoveAt(0);
	}
}

bool UMetaAIComponent::IsRecentlyUsedQuestTarget(AActor* InTarget, const FString& InTargetName) const
{
	if (InTarget && (RecentlyBrokenQuestActors.Contains(InTarget) ||
		(LastBrokenQuestActor.IsValid() && LastBrokenQuestActor.Get() == InTarget)))
	{
		return true;
	}
	const FString Category = ClassifyTargetSystemCategory(InTarget, InTargetName);
	return RecentlyUsedTargetCategories.Num() > 0 && RecentlyUsedTargetCategories.Last() == Category;
}

AActor* UMetaAIComponent::PickNextBreakableActorInShip() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
	TArray<AActor*> Tier1_DifferentCategoryAndFresh;
	TArray<AActor*> Tier2_DifferentActor;
	TArray<AActor*> Tier3_AnyValidTarget;

	// Gather all potential target actors (from DiscoveredLevelTargets and QuestTargetActorsInLevel)
	TArray<AActor*> AllCandidates;
	for (AActor* TargetCand : DiscoveredLevelTargets)
	{
		if (TargetCand && IsValid(TargetCand) && TargetCand != PlayerChar &&
			TargetCand != ActiveQuestMarkerActor && TargetCand != ActiveToolMarkerActor &&
			TargetCand != ActiveQuestTargetActor.Get() &&
			!IsPermanentQuizTabletActor(TargetCand))
		{
			AllCandidates.AddUnique(TargetCand);
		}
	}
	for (AActor* TargetCand : QuestTargetActorsInLevel)
	{
		if (TargetCand && IsValid(TargetCand) && TargetCand != PlayerChar &&
			TargetCand != ActiveQuestMarkerActor && TargetCand != ActiveToolMarkerActor &&
			TargetCand != ActiveQuestTargetActor.Get() &&
			!IsPermanentQuizTabletActor(TargetCand))
		{
			AllCandidates.AddUnique(TargetCand);
		}
	}

	// Fallback scan if no candidates discovered yet
	if (AllCandidates.Num() == 0)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Candidate = *It;
			if (!Candidate || Candidate == PlayerChar || Candidate == GetOwner() ||
				Candidate == ActiveQuestMarkerActor || Candidate == ActiveToolMarkerActor ||
				Candidate == ActiveQuestTargetActor.Get() ||
				DiscoveredLevelTools.Contains(Candidate))
			{
				continue;
			}

			const USpaceAIIdentityComponent* IdComp = Candidate->FindComponentByClass<USpaceAIIdentityComponent>();
			if (IdComp && IdComp->EntityRole == ESpaceAIEntityRole::AstronautPlayer)
			{
				continue;
			}

			const FString CandName = Candidate->GetActorNameOrLabel().ToLower();
			if (CandName.Contains(TEXT("roof")) || CandName.Contains(TEXT("floor")) ||
				CandName.Contains(TEXT("ceiling")) || CandName.Contains(TEXT("wall_")) ||
				CandName.Contains(TEXT("corridor")))
			{
				continue;
			}

			if ((IdComp || Candidate->FindComponentByClass<USpaceObjectComponent>()) && !IsPermanentQuizTabletActor(Candidate))
			{
				AllCandidates.AddUnique(Candidate);
			}
		}
	}

	// Classify and sort candidates into tiers based on history
	for (AActor* Candidate : AllCandidates)
	{
		const FString CandName = Candidate->GetActorNameOrLabel().ToLower();
		const FString CandCategory = ClassifyTargetSystemCategory(Candidate, CandName);

		const bool bIsRecentlyUsedActor = RecentlyBrokenQuestActors.Contains(Candidate) ||
		                                  (LastBrokenQuestActor.IsValid() && Candidate == LastBrokenQuestActor.Get());
		const bool bIsRecentlyUsedCategory = RecentlyUsedTargetCategories.Contains(CandCategory);

		bool bMatchesRecentName = false;
		for (const FString& RecentName : RecentlyUsedTargetNames)
		{
			if (CandName.Contains(RecentName) || RecentName.Contains(CandName))
			{
				bMatchesRecentName = true;
				break;
			}
		}

		if (!bIsRecentlyUsedActor && !bIsRecentlyUsedCategory && !bMatchesRecentName)
		{
			Tier1_DifferentCategoryAndFresh.Add(Candidate);
		}
		else if (!bIsRecentlyUsedActor)
		{
			Tier2_DifferentActor.Add(Candidate);
		}
		else
		{
			Tier3_AnyValidTarget.Add(Candidate);
		}
	}

	// Selection: Tier 1 (Best: different system/category and different actor)
	if (Tier1_DifferentCategoryAndFresh.Num() > 0)
	{
		return Tier1_DifferentCategoryAndFresh[FMath::RandRange(0, Tier1_DifferentCategoryAndFresh.Num() - 1)];
	}

	// Tier 2: Different actor even if category had to be reused (cycles across multiple station systems)
	if (Tier2_DifferentActor.Num() > 0)
	{
		return Tier2_DifferentActor[FMath::RandRange(0, Tier2_DifferentActor.Num() - 1)];
	}

	// Tier 3: Any valid target in station
	if (Tier3_AnyValidTarget.Num() > 0)
	{
		return Tier3_AnyValidTarget[FMath::RandRange(0, Tier3_AnyValidTarget.Num() - 1)];
	}

	return nullptr;
}

AActor* UMetaAIComponent::SpawnOrUpdate3DQuestMarker(AActor* TargetActor, FVector WorldLocation, const FString& QuestTitle, const FString& WhatToDoInstruction, const FString& RequiredItemName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	if (DiscoveredLevelTools.Num() == 0 && DiscoveredLevelTargets.Num() == 0)
	{
		DiscoverAllLevelToolsAndTerminals();
	}

	ActiveQuestTargetActor = TargetActor;
	if (TargetActor)
	{
		LastBrokenQuestActor = TargetActor;
		FVector TargetOrigin, TargetExtent;
		TargetActor->GetActorBounds(false, TargetOrigin, TargetExtent);
		if (TargetExtent.Size() > 15.0f)
		{
			WorldLocation = TargetOrigin;
			QuestMarkerBoxExtent = FVector::Max(TargetExtent + FVector(25.0f), FVector(75.0f, 75.0f, 75.0f));
		}
		else
		{
			WorldLocation = TargetActor->GetActorLocation();
		}
		WorldLocation = ClampLocationToRoomBounds(WorldLocation, RoomBoundaryInnerPaddingCm);

		if (const USpaceAIIdentityComponent* IdComp = TargetActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			CurrentQuestTargetName = IdComp->GetEffectiveEntityName();
		}
		else
		{
			CurrentQuestTargetName = TargetActor->GetActorNameOrLabel();
		}
	}
	else
	{
		CurrentQuestTargetName = TEXT("Ship Breakdown Sector");
		WorldLocation = ClampLocationToRoomBounds(WorldLocation, RoomBoundaryInnerPaddingCm);
	}

	// Match RequiredItemName to an actual Tool Actor in the level (or pick a real Tool from the level like Digi_Wrench!)
	AActor* MatchedToolActor = nullptr;
	if (!RequiredItemName.TrimStartAndEnd().IsEmpty())
	{
		TArray<AActor*> MatchedTools;
		FindMatchingActorsForTarget(RequiredItemName, MatchedTools);
		for (AActor* CandTool : MatchedTools)
		{
			if (CandTool && CandTool != TargetActor && CandTool != UGameplayStatics::GetPlayerCharacter(World, 0))
			{
				MatchedToolActor = CandTool;
				break;
			}
		}
	}
	if (!MatchedToolActor && DiscoveredLevelTools.Num() > 0)
	{
		MatchedToolActor = PickNextToolActorInShip(TargetActor);
	}

	ActiveQuestRequiredToolActor = MatchedToolActor;
	if (MatchedToolActor)
	{
		LastUsedQuestToolActor = MatchedToolActor;
		if (const USpaceAIIdentityComponent* ToolId = MatchedToolActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			CurrentQuestRequiredToolName = ToolId->GetEffectiveEntityName();
		}
		else
		{
			CurrentQuestRequiredToolName = MatchedToolActor->GetActorNameOrLabel();
		}
		ActiveQuestRequiredItem = CurrentQuestRequiredToolName;
	}
	else
	{
		CurrentQuestRequiredToolName = RequiredItemName;
		ActiveQuestRequiredItem = RequiredItemName;
	}

	FString LogicalVerb = TEXT("Repair");
	FString FormattedInstruction = FString::Printf(TEXT("Go to 3D Box on '%s' & Press [E]!"), *CurrentQuestTargetName);
	if (!CurrentQuestRequiredToolName.IsEmpty())
	{
		LogicalVerb = GetLogicalQuestVerbAndInstruction(MatchedToolActor, CurrentQuestRequiredToolName, CurrentQuestTargetName, FormattedInstruction);
	}

	CurrentQuestTitle = QuestTitle.IsEmpty()
		? (CurrentQuestRequiredToolName.IsEmpty()
			? FString::Printf(TEXT("Service %s"), *CurrentQuestTargetName)
			: FString::Printf(TEXT("%s %s at %s"), *LogicalVerb, *CurrentQuestRequiredToolName, *CurrentQuestTargetName))
		: QuestTitle;

	CurrentQuestWhatToDo = WhatToDoInstruction.IsEmpty() ? FormattedInstruction : WhatToDoInstruction;

	bHasActiveAIQuest = true;
	QuestCompletedFlashTimer = 0.0f;
	TimeSinceLastBreakdownCheck = 0.0f;
	CurrentQuestStartTimeSeconds = GetMissionElapsedTimeSeconds();
	CurrentQuestKey = MakeNormalizedQuestKey(CurrentQuestTargetName, CurrentQuestRequiredToolName, CurrentQuestTitle);

	MissionName = FString::Printf(TEXT("[Quest #%d | %s] %s — %s"), CompletedAIQuestsCount + 1, *CurrentQuestSource, *CurrentQuestTitle, *CurrentQuestWhatToDo);

	// Spawn/Update the Cyan 3D Tool Marker on the required Tool (e.g. Digi_Wrench)!
	SpawnOrUpdate3DToolMarker(MatchedToolActor, CurrentQuestRequiredToolName, CurrentQuestTargetName);

	if (!bSpawn3DQuestBoxAndArrow)
	{
		return nullptr;
	}

	// Create the 3D Quest Marker Actor once and reuse/move it for every new quest!
	if (!ActiveQuestMarkerActor || !IsValid(ActiveQuestMarkerActor))
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ActiveQuestMarkerActor = World->SpawnActor<AActor>(AActor::StaticClass(), WorldLocation, FRotator::ZeroRotator, SpawnParams);
		if (!ActiveQuestMarkerActor)
		{
			return nullptr;
		}

		// 1. Visible Glowing 3D Wireframe Quest Box (Interactive with [E] + Overlap)
		ActiveQuestBoxComp = NewObject<UBoxComponent>(ActiveQuestMarkerActor, TEXT("AIQuestZoneBox"));
		ActiveQuestBoxComp->InitBoxExtent(QuestMarkerBoxExtent);
		ActiveQuestBoxComp->SetMobility(EComponentMobility::Movable);
		ActiveQuestBoxComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		ActiveQuestBoxComp->SetCollisionResponseToAllChannels(ECR_Ignore);
		ActiveQuestBoxComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		ActiveQuestBoxComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		ActiveQuestBoxComp->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Overlap);
		ActiveQuestBoxComp->SetHiddenInGame(false);
		ActiveQuestBoxComp->bDrawOnlyIfSelected = false;
		ActiveQuestBoxComp->ShapeColor = FColor(255, 65, 25, 255);
		ActiveQuestBoxComp->SetLineThickness(4.0f);
		ActiveQuestBoxComp->ComponentTags.AddUnique(TEXT("Interactable"));
		ActiveQuestMarkerActor->SetRootComponent(ActiveQuestBoxComp);
		ActiveQuestBoxComp->RegisterComponent();

		ActiveQuestMarkerActor->Tags.AddUnique(TEXT("Interactable"));

		// Add USpaceAIIdentityComponent so looking at the Quest Box and pressing [E] triggers interaction!
		USpaceAIIdentityComponent* QuestIdComp = NewObject<USpaceAIIdentityComponent>(ActiveQuestMarkerActor, TEXT("AIQuestIdentityComp"));
		QuestIdComp->bExposeToMetaAI = false;
		QuestIdComp->bCanInteractWithEKey = true;
		QuestIdComp->InteractionPromptText = TEXT("Press [E] to Complete AI Repair Quest");
		QuestIdComp->bAutoNotifyAIOnInteract = false;
		QuestIdComp->RegisterComponent();

		// 2. Floating 3D Downward Arrow Component + Cone Mesh above the Quest Box
		ActiveQuestArrowComp = NewObject<UArrowComponent>(ActiveQuestMarkerActor, TEXT("AIQuestFloatingArrow"));
		ActiveQuestArrowComp->SetupAttachment(ActiveQuestBoxComp);
		ActiveQuestArrowComp->SetRelativeLocation(FVector(0.0f, 0.0f, QuestMarkerBoxExtent.Z + 70.0f));
		ActiveQuestArrowComp->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f)); // Pointing straight DOWN at the Quest Box!
		ActiveQuestArrowComp->ArrowColor = FColor(255, 215, 0, 255);
		ActiveQuestArrowComp->ArrowSize = 2.8f;
		ActiveQuestArrowComp->SetHiddenInGame(false);
		ActiveQuestArrowComp->RegisterComponent();

		ActiveQuestArrowMeshComp = NewObject<UStaticMeshComponent>(ActiveQuestMarkerActor, TEXT("AIQuestArrowConeMesh"));
		ActiveQuestArrowMeshComp->SetupAttachment(ActiveQuestBoxComp);
		ActiveQuestArrowMeshComp->SetRelativeLocation(FVector(0.0f, 0.0f, QuestMarkerBoxExtent.Z + 55.0f));
		ActiveQuestArrowMeshComp->SetRelativeRotation(FRotator(180.0f, 0.0f, 0.0f)); // Cone tip pointing down!
		ActiveQuestArrowMeshComp->SetRelativeScale3D(FVector(0.42f, 0.42f, 0.65f));
		ActiveQuestArrowMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone")))
		{
			ActiveQuestArrowMeshComp->SetStaticMesh(ConeMesh);
		}
		ActiveQuestArrowMeshComp->RegisterComponent();

		// 3. Floating 3D World Text Label showing Quest Title & What To Do!
		ActiveQuestTextComp = NewObject<UTextRenderComponent>(ActiveQuestMarkerActor, TEXT("AIQuest3DText"));
		ActiveQuestTextComp->SetupAttachment(ActiveQuestBoxComp);
		ActiveQuestTextComp->SetRelativeLocation(FVector(0.0f, 0.0f, QuestMarkerBoxExtent.Z + 95.0f));
		ActiveQuestTextComp->SetHorizontalAlignment(EHTA_Center);
		ActiveQuestTextComp->SetVerticalAlignment(EVRTA_TextCenter);
		ActiveQuestTextComp->SetWorldSize(22.0f);
		ActiveQuestTextComp->SetTextRenderColor(FColor(255, 230, 80, 255));
		ActiveQuestTextComp->SetHiddenInGame(false);
		ActiveQuestTextComp->RegisterComponent();

		// 4. Emergency Glowing Beacon PointLight inside the Quest Box
		ActiveQuestBeaconLight = NewObject<UPointLightComponent>(ActiveQuestMarkerActor, TEXT("AIQuestBeaconLight"));
		ActiveQuestBeaconLight->SetupAttachment(ActiveQuestBoxComp);
		ActiveQuestBeaconLight->SetMobility(EComponentMobility::Movable);
		ActiveQuestBeaconLight->SetIntensity(3200.0f);
		ActiveQuestBeaconLight->SetAttenuationRadius(360.0f);
		ActiveQuestBeaconLight->SetLightColor(FLinearColor(1.0f, 0.25f, 0.05f, 1.0f));
		ActiveQuestBeaconLight->RegisterComponent();
	}

	ActiveQuestMarkerActor->SetActorHiddenInGame(false);
	ActiveQuestMarkerActor->SetActorLocation(WorldLocation);

	if (ActiveQuestBoxComp)
	{
		ActiveQuestBoxComp->SetBoxExtent(QuestMarkerBoxExtent);
		ActiveQuestBoxComp->ShapeColor = FColor(255, 65, 25, 255);
		ActiveQuestBoxComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		ActiveQuestBoxComp->MarkRenderStateDirty();
	}

	if (ActiveQuestArrowComp)
	{
		ActiveQuestArrowComp->ArrowColor = FColor(255, 215, 0, 255);
		ActiveQuestArrowComp->MarkRenderStateDirty();
	}

	if (ActiveQuestBeaconLight)
	{
		ActiveQuestBeaconLight->SetLightColor(FLinearColor(1.0f, 0.22f, 0.05f, 1.0f));
		ActiveQuestBeaconLight->SetIntensity(3200.0f);
	}

	if (ActiveQuestTextComp)
	{
		const FString ToolLine = CurrentQuestRequiredToolName.IsEmpty()
			? TEXT("")
			: FString::Printf(TEXT("\n[REQUIRED TOOL: %s]"), *CurrentQuestRequiredToolName);
		const FString Display3DText = FString::Printf(
			TEXT("[AI QUEST #%d]: %s\n>> %s <<%s"),
			CompletedAIQuestsCount + 1,
			*CurrentQuestTitle,
			*CurrentQuestWhatToDo,
			*ToolLine
		);
		ActiveQuestTextComp->SetText(FText::FromString(Display3DText));
		ActiveQuestTextComp->SetTextRenderColor(FColor(255, 235, 60, 255));
	}

	PrintScreenDebug(
		FString::Printf(TEXT("=== [AI GAME MASTER NEW QUEST #%d] Tool: '%s' -> Target: '%s' | %s ==="),
			CompletedAIQuestsCount + 1,
			CurrentQuestRequiredToolName.IsEmpty() ? TEXT("Hands/[E]") : *CurrentQuestRequiredToolName,
			*CurrentQuestTargetName,
			*CurrentQuestWhatToDo),
		FColor::Yellow,
		12.0f
	);

	LogTrainingEventToDatabase(TEXT("ai_quest_started"), CurrentQuestTargetName, MissionName);
	return ActiveQuestMarkerActor;
}

void UMetaAIComponent::UpdateActive3DQuestMarker(float DeltaTime)
{
	QuestMarkerAnimTime += DeltaTime;

	// Periodic breakdown check for Endless AI Game Master (Halts once 90/100 Problem-Solving Mastery is reached!)
	if (bEnableEndlessAIGameMaster && !bMasteryThresholdReached && ProblemSolvingProficiencyScore < ProblemSolvingMasteryThreshold)
	{
		TimeSinceLastBreakdownCheck += DeltaTime;
		if (!bHasActiveAIQuest && !bIsAwaitingBreakdownAIResponse && QuestCompletedFlashTimer <= 0.0f && TimeSinceLastBreakdownCheck >= NextQuestDelayAfterCompletionSeconds + 2.0f)
		{
			TimeSinceLastBreakdownCheck = 0.0f;
			TriggerNewShipBreakdownAndAIQuest(TEXT("endless_ai_gamemaster_cycle"));
			return;
		}
		else if (bHasActiveAIQuest && TimeSinceLastBreakdownCheck >= EndlessBreakdownIntervalSeconds)
		{
			// Escalate risk if player leaves a broken system unfixed for too long!
			TimeSinceLastBreakdownCheck = 0.0f;
			CurrentAIRiskScorePercent = FMath::Clamp(CurrentAIRiskScorePercent + 15.0f, 0.0f, 100.0f);
			if (CurrentAIRiskScorePercent >= 85.0f) CurrentAIRiskLevel = TEXT("CRITICAL");
			else if (CurrentAIRiskScorePercent >= 65.0f) CurrentAIRiskLevel = TEXT("HIGH");
			ApplyAutonomousAIRiskResponse(CurrentAIRiskLevel, CurrentAIRiskScorePercent);
		}
	}

	UWorld* World = GetWorld();
	const float BobOffset = FMath::Sin(QuestMarkerAnimTime * 4.5f) * 18.0f;
	const float SpinYaw = FMath::Fmod(QuestMarkerAnimTime * 110.0f, 360.0f);

	// 1. Update the Step 1 Cyan Tool Marker (following Digi_Wrench / required tool in real-time Zero-G!)
	if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
	{
		const bool bShouldTrackTool = ActiveQuestRequiredToolActor.IsValid() && (bHasActiveAIQuest || IsInOrientationTraining());
		if (bShouldTrackTool)
		{
			AActor* ReqTool = ActiveQuestRequiredToolActor.Get();

			// Check if player is holding the required tool
			bool bIsHoldingTool = false;
			if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0))
			{
				if (const USpaceGrabSnapComponent* GrabComp = PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>())
				{
					bIsHoldingTool = (GrabComp->GetHeldActor() == ReqTool);
				}
			}

			if (bIsHoldingTool)
			{
				// Hide marker while securely held in trainee's hand to keep screen view clear
				if (!ActiveToolMarkerActor->IsHidden())
				{
					ActiveToolMarkerActor->SetActorHiddenInGame(true);
				}
			}
			else
			{
				// Tool is floating or in cabin: Unhide marker and follow the tool's 3D bounding center dynamically!
				if (ActiveToolMarkerActor->IsHidden())
				{
					ActiveToolMarkerActor->SetActorHiddenInGame(false);
				}

				FVector ToolOrigin, ToolBoundsExtent;
				ReqTool->GetActorBounds(false, ToolOrigin, ToolBoundsExtent);
				const FVector ToolTargetLoc = (ToolBoundsExtent.Size() > 5.0f) ? ToolOrigin : ReqTool->GetActorLocation();
				ActiveToolMarkerActor->SetActorLocation(ToolTargetLoc);

				const FVector EffectiveToolExtent = FVector::Max(ToolBoundsExtent + FVector(10.0f), FVector(28.0f, 28.0f, 28.0f));
				if (ActiveToolBoxComp)
				{
					ActiveToolBoxComp->SetBoxExtent(EffectiveToolExtent);
				}

				if (ActiveToolArrowComp)
				{
					ActiveToolArrowComp->SetRelativeLocation(FVector(0.0f, 0.0f, EffectiveToolExtent.Z + 48.0f + BobOffset));
					ActiveToolArrowComp->SetRelativeRotation(FRotator(-90.0f, SpinYaw, 0.0f));
				}
				if (ActiveToolArrowMeshComp)
				{
					ActiveToolArrowMeshComp->SetRelativeLocation(FVector(0.0f, 0.0f, EffectiveToolExtent.Z + 32.0f + BobOffset));
					ActiveToolArrowMeshComp->SetRelativeRotation(FRotator(180.0f, SpinYaw, 0.0f));
				}
				if (World && ActiveToolTextComp)
				{
					if (APlayerCameraManager* CamMgr = UGameplayStatics::GetPlayerCameraManager(World, 0))
					{
						const FRotator LookRot = UKismetMathLibrary::FindLookAtRotation(ActiveToolTextComp->GetComponentLocation(), CamMgr->GetCameraLocation());
						ActiveToolTextComp->SetWorldRotation(FRotator(0.0f, LookRot.Yaw, 0.0f));
					}
				}

				// Draw vivid glowing 3D Wireframe Box around the floating tool
				if (World)
				{
					DrawDebugBox(World, ToolTargetLoc, EffectiveToolExtent, FQuat::Identity, FColor(0, 240, 255), false, -1.0f, 0, 3.2f);
				}
			}
		}
		else
		{
			ActiveToolMarkerActor->SetActorHiddenInGame(true);
		}
	}

	if (!ActiveQuestMarkerActor || !IsValid(ActiveQuestMarkerActor) || ActiveQuestMarkerActor->IsHidden())
	{
		return;
	}

	// 2. Follow ActiveQuestTargetActor if it moves in Zero-G
	if (ActiveQuestTargetActor.IsValid())
	{
		FVector TargetCenter, TargetExtent;
		ActiveQuestTargetActor->GetActorBounds(false, TargetCenter, TargetExtent);
		ActiveQuestMarkerActor->SetActorLocation(TargetExtent.Size() > 15.0f ? TargetCenter : ActiveQuestTargetActor->GetActorLocation());
	}

	// Draw vivid glowing 3D Wireframe Box around the Broken Target Machine / Quest Location
	if (World)
	{
		const FColor TargetBoxColor = (QuestCompletedFlashTimer > 0.0f) ? FColor(30, 255, 110) : FColor(255, 65, 25);
		const FVector TargetExtent = ActiveQuestBoxComp ? ActiveQuestBoxComp->GetUnscaledBoxExtent() : QuestMarkerBoxExtent;
		DrawDebugBox(World, ActiveQuestMarkerActor->GetActorLocation(), TargetExtent, FQuat::Identity, TargetBoxColor, false, -1.0f, 0, 3.8f);
	}

	// Animate the Floating 3D Quest Arrow (bobbing up & down + spinning)
	if (ActiveQuestArrowComp)
	{
		ActiveQuestArrowComp->SetRelativeLocation(FVector(0.0f, 0.0f, QuestMarkerBoxExtent.Z + 70.0f + BobOffset));
		ActiveQuestArrowComp->SetRelativeRotation(FRotator(-90.0f, SpinYaw, 0.0f));
	}
	if (ActiveQuestArrowMeshComp)
	{
		ActiveQuestArrowMeshComp->SetRelativeLocation(FVector(0.0f, 0.0f, QuestMarkerBoxExtent.Z + 52.0f + BobOffset));
		ActiveQuestArrowMeshComp->SetRelativeRotation(FRotator(180.0f, SpinYaw, 0.0f));
	}

	// Rotate the 3D Objective Text to face the Player Camera (Billboard)
	if (World && ActiveQuestTextComp)
	{
		if (APlayerCameraManager* CamMgr = UGameplayStatics::GetPlayerCameraManager(World, 0))
		{
			const FVector CamLoc = CamMgr->GetCameraLocation();
			const FVector TextLoc = ActiveQuestTextComp->GetComponentLocation();
			const FRotator LookAtRot = UKismetMathLibrary::FindLookAtRotation(TextLoc, CamLoc);
			ActiveQuestTextComp->SetWorldRotation(FRotator(0.0f, LookAtRot.Yaw, 0.0f));
		}
	}

	// Check if a Quest Completion Green Flash is counting down before hiding
	if (QuestCompletedFlashTimer > 0.0f)
	{
		QuestCompletedFlashTimer -= DeltaTime;
		if (QuestCompletedFlashTimer <= 0.0f && !bHasActiveAIQuest)
		{
			ActiveQuestMarkerActor->SetActorHiddenInGame(true);
		}
		return;
	}

	// 3. Complete quest when player brings the tool to the target OR presses [E] near the 3D Quest Box!
	if (bHasActiveAIQuest && World)
	{
		const FVector BoxCenter = ActiveQuestMarkerActor->GetActorLocation();
		const float CompletionReachRadius = FMath::Max(280.0f, QuestMarkerBoxExtent.GetMax() * 1.6f);
		const float CompletionReachSq = FMath::Square(CompletionReachRadius);

		ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
		APlayerController* PC = PlayerChar ? Cast<APlayerController>(PlayerChar->GetController()) : nullptr;
		const FVector PlayerLoc = PlayerChar ? PlayerChar->GetActorLocation() : FVector::ZeroVector;
		const float DistToPlayerSq = FVector::DistSquared(PlayerLoc, BoxCenter);
		const bool bPlayerNearBox = (DistToPlayerSq <= FMath::Square(380.0f));
		const bool bEPressed = PC && PC->WasInputKeyJustPressed(EKeys::E);

		AActor* HeldActor = nullptr;
		if (PlayerChar)
		{
			if (const USpaceGrabSnapComponent* GrabComp = PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>())
			{
				HeldActor = GrabComp->GetHeldActor();
			}
		}

		// STRICT REPAIR RULE: Never complete automatically in Tick!
		// The astronaut MUST be near the box AND actively press [E]!
		if (bPlayerNearBox && bEPressed)
		{
			const bool bQuestRequiresTool = (!CurrentQuestRequiredToolName.IsEmpty() || ActiveQuestRequiredToolActor.IsValid());

			if (bQuestRequiresTool)
			{
				bool bHoldingCorrectTool = false;
				FString HeldToolName = TEXT("Bare Hands");

				if (HeldActor)
				{
					if (const USpaceAIIdentityComponent* HeldId = HeldActor->FindComponentByClass<USpaceAIIdentityComponent>())
					{
						HeldToolName = HeldId->GetEffectiveEntityName();
					}
					else
					{
						HeldToolName = HeldActor->GetActorNameOrLabel();
					}

					if (ActiveQuestRequiredToolActor.IsValid() && HeldActor == ActiveQuestRequiredToolActor.Get())
					{
						bHoldingCorrectTool = true;
					}
					else if (!CurrentQuestRequiredToolName.IsEmpty())
					{
						if (HeldToolName.Contains(CurrentQuestRequiredToolName, ESearchCase::IgnoreCase) ||
							CurrentQuestRequiredToolName.Contains(HeldToolName, ESearchCase::IgnoreCase))
						{
							bHoldingCorrectTool = true;
						}
					}
				}

				if (!bHoldingCorrectTool)
				{
					if (HeldActor)
					{
						TransmitFlightDirectorRadioComms(
							FString::Printf(TEXT("Flight Director: Incorrect tool! You are holding '%s', but '%s' requires '%s'. Reacquire the correct tool!"),
								*HeldToolName, *CurrentQuestTargetName, *CurrentQuestRequiredToolName),
							5.0f
						);
						PrintScreenDebug(
							FString::Printf(TEXT("[AI Game Master] WRONG TOOL: '%s' cannot service '%s'! You need '%s' (Cyan Box)!"),
								*HeldToolName, *CurrentQuestTargetName, *CurrentQuestRequiredToolName),
							FColor(255, 60, 40),
							5.0f
						);
					}
					else
					{
						TransmitFlightDirectorRadioComms(
							FString::Printf(TEXT("Flight Director: Service interlock locked! You need the '%s' to repair '%s'. Follow the CYAN marker first!"),
								*CurrentQuestRequiredToolName, *CurrentQuestTargetName),
							5.0f
						);
						PrintScreenDebug(
							FString::Printf(TEXT("[AI Game Master] You need '%s' (Cyan Box) to repair '%s'! Grab it first with RMB, then press [E]!"),
								*CurrentQuestRequiredToolName, *CurrentQuestTargetName),
							FColor::Orange,
							5.0f
						);
					}
					return;
				}
			}

			const FString ToolUsedNote = CurrentQuestRequiredToolName.IsEmpty()
				? TEXT("Direct [E] Repair")
				: FString::Printf(TEXT("Used Tool '%s'"), *CurrentQuestRequiredToolName);

			CompleteCurrentAIQuest(FString::Printf(TEXT("Astronaut repaired '%s' (%s)"), *CurrentQuestTargetName, *ToolUsedNote));
			return;
		}
	}
}

void UMetaAIComponent::InitializePermanentRoomDiagnosticTablet()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	AActor* TabletAct = FindDiagnosticTabletActor();
	if (!TabletAct)
	{
		if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0))
		{
			USpaceTabletQuizComponent* AutoComp = USpaceTabletQuizComponent::GetSpaceTabletQuizComponent(PlayerChar);
			if (AutoComp)
			{
				AutoComp->EnsureDefaultWidgetClass();
			}
		}
		return;
	}

	// 1. Ensure USpaceTabletQuizComponent is attached and registered
	USpaceTabletQuizComponent* QuizComp = TabletAct->FindComponentByClass<USpaceTabletQuizComponent>();
	if (!QuizComp)
	{
		QuizComp = NewObject<USpaceTabletQuizComponent>(TabletAct, TEXT("PermanentRoomDiagnosticQuizComp"));
		if (QuizComp)
		{
			QuizComp->RegisterComponent();
		}
	}
	if (QuizComp)
	{
		QuizComp->EnsureDefaultWidgetClass();
		QuizComp->bAutoOpenOnProximity = false;
		QuizComp->bRequireInteractKeyToOpen = true;
	}

	// 2. Ensure Tablet has interactive identity
	USpaceAIIdentityComponent* TabId = TabletAct->FindComponentByClass<USpaceAIIdentityComponent>();
	if (!TabId)
	{
		TabId = NewObject<USpaceAIIdentityComponent>(TabletAct, TEXT("PermanentTabletIdentityComp"));
		if (TabId)
		{
			TabId->EntityName = TEXT("Diagnostic_Tablet");
			TabId->EntityRole = ESpaceAIEntityRole::StationEnvironment;
			TabId->RegisterComponent();
		}
	}
	if (TabId)
	{
		TabId->bCanInteractWithEKey = true;
	}

	// 3. Attach permanent 3D holographic text marker above the Tablet
	UTextRenderComponent* TextComp = TabletAct->FindComponentByClass<UTextRenderComponent>();
	if (!TextComp)
	{
		TextComp = NewObject<UTextRenderComponent>(TabletAct, TEXT("TabletHoloPromptText"));
		if (TextComp)
		{
			TextComp->AttachToComponent(TabletAct->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
			TextComp->SetRelativeLocation(FVector(0.0f, 0.0f, 48.0f));
			TextComp->SetTextRenderColor(FColor(0, 240, 255));
			TextComp->SetWorldSize(15.0f);
			TextComp->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
			TextComp->SetVerticalAlignment(EVerticalTextAligment::EVRTA_TextCenter);
			TextComp->RegisterComponent();
		}
	}
	if (TextComp)
	{
		TextComp->SetText(FText::FromString(TEXT("[E] or [T]: Space Tablet / Engineering Quiz")));
		TextComp->SetVisibility(true);
	}

	// 4. Attach subtle Cyan point light beacon
	UPointLightComponent* LightComp = TabletAct->FindComponentByClass<UPointLightComponent>();
	if (!LightComp)
	{
		LightComp = NewObject<UPointLightComponent>(TabletAct, TEXT("TabletBeaconLight"));
		if (LightComp)
		{
			LightComp->AttachToComponent(TabletAct->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
			LightComp->SetRelativeLocation(FVector(0.0f, 0.0f, 35.0f));
			LightComp->SetLightColor(FLinearColor(0.0f, 0.9f, 1.0f));
			LightComp->SetIntensity(600.0f);
			LightComp->SetAttenuationRadius(180.0f);
			LightComp->RegisterComponent();
		}
	}
}

AActor* UMetaAIComponent::FindDiagnosticTabletActor() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);

	// 1. Search for any Actor with USpaceTabletQuizComponent (excluding player)
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Act = *It;
		if (!Act || Act == PlayerChar || Act == GetOwner() || Act == ActiveQuestMarkerActor || Act == ActiveToolMarkerActor)
		{
			continue;
		}
		if (Act->FindComponentByClass<USpaceTabletQuizComponent>())
		{
			return Act;
		}
	}

	// 2. Search by Actor name/label for Tablet_Scanner, tablet, or quiz
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Act = *It;
		if (!Act || Act == PlayerChar || Act == GetOwner() || Act == ActiveQuestMarkerActor || Act == ActiveToolMarkerActor)
		{
			continue;
		}
		const FString ActName = Act->GetActorNameOrLabel().ToLower();
		if (ActName.Contains(TEXT("tablet_scanner")) || ActName.Contains(TEXT("tablet")) || ActName.Contains(TEXT("scanner")) || ActName.Contains(TEXT("quiz")) || ActName.Contains(TEXT("quest_main")))
		{
			return Act;
		}
	}

	return nullptr;
}

bool UMetaAIComponent::IsPermanentQuizTabletActor(const AActor* InActor) const
{
	if (!InActor || !IsValid(InActor))
	{
		return false;
	}

	// 1. Actor has USpaceTabletQuizComponent
	if (InActor->FindComponentByClass<USpaceTabletQuizComponent>() != nullptr)
	{
		return true;
	}

	// 2. Name or Class matches Quest_main or Space Tablet Quiz
	const FString CombinedName = (InActor->GetName() + TEXT(" ") + InActor->GetActorNameOrLabel() + TEXT(" ") + InActor->GetClass()->GetName()).ToLower();
	if (CombinedName.Contains(TEXT("quest_main")) ||
	    CombinedName.Contains(TEXT("spacetabletquiz")) ||
	    CombinedName.Contains(TEXT("tablet_scanner")) ||
	    CombinedName.Contains(TEXT("diagnostic_tablet")))
	{
		return true;
	}

	return false;
}

void UMetaAIComponent::ActivateDiagnosticTabletEvaluationState()
{
	bMasteryThresholdReached = true;
	CurrentAIRiskScorePercent = 0.0f;
	CurrentAIRiskLevel = TEXT("OPTIMAL");
	bHasActiveAIQuest = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(NextEndlessQuestTimerHandle);
	}

	if (bAllowAIToControlShipLights)
	{
		ApplyShipLightingPreset(TEXT("normal_white"), TEXT("ALL_LIGHTS"));
	}

	CleanupAllExcessDebris();

	if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
	{
		ActiveToolMarkerActor->SetActorHiddenInGame(true);
	}

	AActor* TabletAct = FindDiagnosticTabletActor();
	if (TabletAct)
	{
		ActiveQuestTargetActor = TabletAct;
		ActiveQuestRequiredToolActor = nullptr;
		CurrentQuestRequiredToolName = TEXT("");
		CurrentQuestTargetName = TabletAct->GetActorNameOrLabel();

		USpaceTabletQuizComponent* QuizComp = TabletAct->FindComponentByClass<USpaceTabletQuizComponent>();
		if (!QuizComp)
		{
			QuizComp = NewObject<USpaceTabletQuizComponent>(TabletAct, TEXT("LevelDiagnosticQuizComp"));
			if (QuizComp)
			{
				QuizComp->RegisterComponent();
			}
		}
		if (QuizComp)
		{
			QuizComp->EnsureDefaultWidgetClass();
		}

		// Ensure tablet has interactive identity
		USpaceAIIdentityComponent* TabId = TabletAct->FindComponentByClass<USpaceAIIdentityComponent>();
		if (!TabId)
		{
			TabId = NewObject<USpaceAIIdentityComponent>(TabletAct, TEXT("TabletAIIdentityComp"));
			if (TabId)
			{
				TabId->EntityName = TEXT("Diagnostic Tablet");
				TabId->EntityRole = ESpaceAIEntityRole::SpaceshipRepairZone;
				TabId->bCanInteractWithEKey = true;
				TabId->RegisterComponent();
			}
		}
		else
		{
			TabId->bCanInteractWithEKey = true;
		}

		TabletAct->Tags.AddUnique(TEXT("Interactable"));

		// Highlight the Diagnostic Tablet with the 3D marker in soothing Cyan
		if (ActiveQuestMarkerActor && IsValid(ActiveQuestMarkerActor))
		{
			ActiveQuestMarkerActor->SetActorHiddenInGame(false);

			FVector TabOrigin, TabExtent;
			TabletAct->GetActorBounds(false, TabOrigin, TabExtent);
			const FVector MarkerLoc = (TabExtent.Size() > 5.0f) ? TabOrigin : TabletAct->GetActorLocation();
			ActiveQuestMarkerActor->SetActorLocation(MarkerLoc);

			const FVector EffectiveTabExtent = FVector::Max(TabExtent + FVector(20.0f), FVector(45.0f, 45.0f, 45.0f));
			if (ActiveQuestBoxComp)
			{
				ActiveQuestBoxComp->SetBoxExtent(EffectiveTabExtent);
				ActiveQuestBoxComp->ShapeColor = FColor(0, 240, 255, 255); // Soothing Cyan
				ActiveQuestBoxComp->MarkRenderStateDirty();
			}
			if (ActiveQuestArrowComp)
			{
				ActiveQuestArrowComp->ArrowColor = FColor(0, 240, 255, 255);
				ActiveQuestArrowComp->MarkRenderStateDirty();
			}
			if (ActiveQuestBeaconLight)
			{
				ActiveQuestBeaconLight->SetLightColor(FLinearColor(0.0f, 0.95f, 1.0f));
			}
			if (ActiveQuestTextComp)
			{
				ActiveQuestTextComp->SetText(FText::FromString(TEXT("[DIAGNOSTIC TABLET: CERTIFICATION]\n>> Press [E] or approach to begin technical evaluation! <<")));
				ActiveQuestTextComp->SetTextRenderColor(FColor(0, 255, 240, 255));
			}
		}
	}
	else
	{
		if (ActiveQuestMarkerActor && IsValid(ActiveQuestMarkerActor))
		{
			ActiveQuestMarkerActor->SetActorHiddenInGame(true);
		}
	}

	MissionName = FString::Printf(TEXT("Station Secured (Mastery: %.0f/100) - Report to Tablet for Final Certification"), ProblemSolvingProficiencyScore);
	CurrentQuestTitle = TEXT("Station Secured - Technical Dilemma Certification");
	CurrentQuestWhatToDo = TEXT("Approach or press [E] on the Diagnostic Tablet to complete final evaluation!");
	CachedFlightDirectorAdvice = TEXT("Flight Director: Outstanding work, Cadet! All station systems stabilized and nominal. Station risk is 0% (OPTIMAL). Report to the Diagnostic Tablet to begin your final technical dilemma evaluation!");

	LogTrainingEventToDatabase(
		TEXT("station_stabilized_mastery_achieved"),
		TEXT("Station"),
		FString::Printf(TEXT("Station stabilized with %.0f/100 mastery. Risk 0%% (OPTIMAL). Proceeding to tablet evaluation."), ProblemSolvingProficiencyScore)
	);

	PrintScreenDebug(
		TEXT("=== [STATION SECURED: 100% NOMINAL] Risk: OPTIMAL (0%) | Proceed to Diagnostic Tablet for Evaluation! ==="),
		FColor::Cyan,
		10.0f
	);
}

void UMetaAIComponent::StopFlightDirectorVoice()
{
	FScopeLock Lock(&FlightDirectorVoiceCriticalSection);
	FlightDirectorVoiceQueue.Empty();
	bStopVoiceRequested = true;
	++FlightDirectorVoiceGeneration;
}

void UMetaAIComponent::InterruptFlightDirectorVoice()
{
	FScopeLock Lock(&FlightDirectorVoiceCriticalSection);
	FlightDirectorVoiceQueue.Empty();
	++FlightDirectorVoiceGeneration;
}

void UMetaAIComponent::EnqueueFlightDirectorSpokenText(const FString& InText)
{
#if PLATFORM_WINDOWS
	if (!bEnableSpokenVoiceAudio)
	{
		return;
	}

	FString CleanText = InText.TrimStartAndEnd();
	if (CleanText.IsEmpty())
	{
		return;
	}

	// Format text for natural mission control radio cadence:
	CleanText.ReplaceInline(TEXT("["), TEXT(""));
	CleanText.ReplaceInline(TEXT("]"), TEXT(""));
	CleanText.ReplaceInline(TEXT("("), TEXT(", "));
	CleanText.ReplaceInline(TEXT(")"), TEXT(", "));
	CleanText.ReplaceInline(TEXT("'"), TEXT(""));
	CleanText.ReplaceInline(TEXT("\""), TEXT(""));
	CleanText.ReplaceInline(TEXT("_"), TEXT(" "));

	if (CleanText.StartsWith(TEXT("Flight Director:"), ESearchCase::IgnoreCase))
	{
		CleanText = CleanText.RightChop(16).TrimStart();
		CleanText = FString::Printf(TEXT("Flight Director. %s"), *CleanText);
	}

	bool bLaunchWorker = false;
	{
		FScopeLock Lock(&FlightDirectorVoiceCriticalSection);
		bStopVoiceRequested = false;

		// Keep at most 2 most recent messages so trainees never experience stale queued radio backlog
		if (FlightDirectorVoiceQueue.Num() >= 2)
		{
			FlightDirectorVoiceQueue.RemoveAt(0);
		}

		FlightDirectorVoiceQueue.Add(CleanText);

		if (!bIsFlightDirectorSpeaking)
		{
			bIsFlightDirectorSpeaking = true;
		bLaunchWorker = true;
		}
	}

	if (bLaunchWorker)
	{
		const int32 Rate = FMath::Clamp(VoiceSpeechRate, -10, 10);
		const int32 Volume = FMath::Clamp(VoiceVolume, 0, 100);

		// Dedicated background thread: speaks through system audio (SAPI) without blocking game thread or frame rate!
		Async(EAsyncExecution::Thread, [this, Rate, Volume]()
		{
			HRESULT CoInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const bool bNeedCoUninit = SUCCEEDED(CoInitHr) || CoInitHr == S_FALSE;

			ISpVoice* pVoice = nullptr;
			HRESULT Hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void**)&pVoice);

			if (SUCCEEDED(Hr) && pVoice)
			{
				pVoice->SetRate(Rate);
				pVoice->SetVolume(static_cast<USHORT>(Volume));

				while (true)
				{
					FString NextText;
					int32 SentenceGeneration = 0;
					{
						FScopeLock Lock(&FlightDirectorVoiceCriticalSection);
						if (bStopVoiceRequested || FlightDirectorVoiceQueue.Num() == 0)
						{
							bIsFlightDirectorSpeaking = false;
							break;
						}

						NextText = FlightDirectorVoiceQueue[0];
						FlightDirectorVoiceQueue.RemoveAt(0);
						SentenceGeneration = FlightDirectorVoiceGeneration;
					}

					if (NextText.IsEmpty())
					{
						continue;
					}

					// Speak asynchronously and poll, so the sentence can be cut instantly when a new quest starts.
					pVoice->Speak(TCHAR_TO_WCHAR(*NextText), SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);
					while (pVoice->WaitUntilDone(40) != S_OK)
					{
						bool bCut = false;
						{
							FScopeLock Lock(&FlightDirectorVoiceCriticalSection);
							bCut = bStopVoiceRequested || (SentenceGeneration != FlightDirectorVoiceGeneration);
						}
						if (bCut)
						{
							pVoice->Speak(NULL, SPF_PURGEBEFORESPEAK, NULL);
							break;
						}
					}
				}

				pVoice->Release();
				pVoice = nullptr;
			}
			else
			{
				FScopeLock Lock(&FlightDirectorVoiceCriticalSection);
				FlightDirectorVoiceQueue.Empty();
				bIsFlightDirectorSpeaking = false;
			}

			if (bNeedCoUninit)
			{
				CoUninitialize();
			}
		});
	}
#endif
}

void UMetaAIComponent::TransmitFlightDirectorRadioComms(const FString& Message, float DisplayDurationSeconds, bool bLogToDatabase)
{
	const FString CleanMsg = Message.TrimStartAndEnd();
	if (CleanMsg.IsEmpty())
	{
		return;
	}

	CachedFlightDirectorAdvice = CleanMsg;

	// 1. Play optional radio click / beep / chime sound effect
	if (RadioTransmissionSound)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			UGameplayStatics::PlaySound2D(World, RadioTransmissionSound);
		}
	}

	// 2. Audibly speak radio comms using native Windows Speech API (Async thread)
	EnqueueFlightDirectorSpokenText(CleanMsg);

	// 3. Print cyan radio HUD advice
	PrintScreenDebug(
		FString::Printf(TEXT("[FLIGHT DIRECTOR RADIO] %s"), *CleanMsg),
		FColor(0, 240, 255), // Vibrant Cyan / Radio Comm Blue
		DisplayDurationSeconds
	);

	// 4. Record to MySQL telemetry
	if (bLogToDatabase && bEnablePhpDatabaseSync)
	{
		LogTrainingEventToDatabase(TEXT("flight_director_radio"), TEXT("FlightDirector"), CleanMsg.Left(200));
	}
}

void UMetaAIComponent::StartBackgroundPrecomputeNextQuest()
{
	// Guard: Do not precompute if already precomputed, currently in-flight, or mastery threshold achieved!
	if (bHasPrecomputedNextQuest || bIsPrecomputingNextQuest || bMasteryThresholdReached || ProblemSolvingProficiencyScore >= ProblemSolvingMasteryThreshold)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 1. Pick candidate Target and Tool for the upcoming quest that are distinct from current active quest!
	AActor* NextTarget = nullptr;
	AActor* NextTool = nullptr;
	FSpaceQuestDefinition PickedDef;

	const bool bFromDbOrManual = TryPickUncompletedManualOrDatabaseQuest(PickedDef, NextTarget, NextTool);

	if (!NextTarget)
	{
		NextTarget = PickNextBreakableActorInShip();
	}
	if (!NextTool)
	{
		NextTool = PickNextToolActorInShip(NextTarget);
	}

	const FString NextTargetName = NextTarget ? NextTarget->GetActorNameOrLabel() : (!PickedDef.TargetActorName.IsEmpty() ? PickedDef.TargetActorName : TEXT("pipe"));
	const FString NextToolName = NextTool ? NextTool->GetActorNameOrLabel() : (!PickedDef.RequiredToolName.IsEmpty() ? PickedDef.RequiredToolName : TEXT("Extinguisher_Foam"));

	// Initialize baseline precomputed quest definition (guarantees instant 0ms fallback even before HTTP returns)
	PrecomputedNextAIQuest = PickedDef;
	PrecomputedNextAIQuest.QuestKey = MakeNormalizedQuestKey(NextTargetName, NextToolName, PickedDef.QuestTitle);
	PrecomputedNextAIQuest.QuestSource = bFromDbOrManual ? PickedDef.QuestSource : TEXT("META_AI_PIPELINE");
	PrecomputedNextAIQuest.TargetActorInLevel = NextTarget;
	PrecomputedNextAIQuest.TargetActorName = NextTargetName;
	PrecomputedNextAIQuest.RequiredToolActorInLevel = NextTool;
	PrecomputedNextAIQuest.RequiredToolName = NextToolName;
	if (PrecomputedNextAIQuest.QuestTitle.IsEmpty())
	{
		PrecomputedNextAIQuest.QuestTitle = FString::Printf(TEXT("EMERGENCY: Seal %s"), *NextTargetName);
	}
	if (PrecomputedNextAIQuest.QuestInstruction.IsEmpty())
	{
		PrecomputedNextAIQuest.QuestInstruction = FString::Printf(TEXT("Take %s (Cyan Box) and service %s (3D Box) & Press [E]!"), *NextToolName, *NextTargetName);
	}
	PrecomputedNextAIQuest.PrecomputedRiskLevel = TEXT("CRITICAL");
	PrecomputedNextAIQuest.PrecomputedRiskScore = FMath::RandRange(75.0f, 92.0f);
	PrecomputedNextAIQuest.PrecomputedFlightDirectorAdvice = FString::Printf(
		TEXT("Flight Director: Attention Cadet! Secondary telemetry indicates cascade rupture at %s. Acquire %s immediately!"),
		*NextTargetName, *NextToolName
	);

	bHasPrecomputedNextQuest = true;

	// Register upcoming quest in history immediately so no duplicate picks occur while buffering
	RecordQuestTargetAndToolUsage(NextTarget, NextTargetName, NextTool, NextToolName);
	SessionAssignedQuestKeys.Add(PrecomputedNextAIQuest.QuestKey);
	SessionAssignedQuestKeys.Add(MakeNormalizedQuestKey(NextTargetName, NextToolName));

	// 2. Dispatch background directive to Meta AI to autonomously calculate physics, lighting, and radio advice!
	FString BlockReason;
	if (CanSendRequestNow(BlockReason))
	{
		bIsPrecomputingNextQuest = true;
		const FString PipelineDirective = FString::Printf(
			TEXT("AI_PIPELINE_PRECOMPUTE_NEXT_QUEST [Target='%s', RequiredTool='%s', SystemCategory='%s', NextQuestNum=%d, CurrentMastery=%.0f/100]: "
				 "You are the autonomous Meta AI Flight Director & Physics Engine presiding over this station. "
				 "The astronaut is currently repairing an active breakdown. In the background, prepare the NEXT upcoming emergency. "
				 "Target: '%s', Required Tool: '%s'. "
				 "CRITICAL DIVERSITY MANDATE: You MUST rotate station emergencies across different failure modes (Electrical Overload, Coolant Vapor Lock, Avionics Glitch, Core Thermal Fluctuation, Life Support Scrubber Depletion). "
				 "DO NOT repeat recent failure scenarios! Create an authentic, unique aerospace engineering mission for target '%s' requiring tool '%s'. "
				 "Output 'ai_quest' with realistic engineering 'quest_title' and 'quest_instruction', "
				 "determine 'risk_level' ('HIGH'/'CRITICAL') and 'risk_score' (65-95), "
				 "issue real-time zero-G 'physics_commands' (impulses, spin, damping for station debris), "
				 "set emergency lighting in 'light_commands', and write authentic Flight Director radio guidance in 'message'."),
			*NextTargetName,
			*NextToolName,
			*ClassifyTargetSystemCategory(NextTarget, NextTargetName),
			CompletedAIQuestsCount + 2,
			ProblemSolvingProficiencyScore,
			*NextTargetName,
			*NextToolName,
			*NextTargetName,
			*NextToolName
		);

		SendWorldDirectiveToAI(PipelineDirective);
	}
}

bool UMetaAIComponent::ActivatePrecomputedNextQuest()
{
	if (!bHasPrecomputedNextQuest)
	{
		return false;
	}

	bHasPrecomputedNextQuest = false;

	AActor* TargetAct = PrecomputedNextAIQuest.TargetActorInLevel.Get();
	AActor* ToolAct = PrecomputedNextAIQuest.RequiredToolActorInLevel.Get();

	if (!TargetAct && !PrecomputedNextAIQuest.TargetActorName.IsEmpty())
	{
		TArray<AActor*> Matches;
		FindMatchingActorsForTarget(PrecomputedNextAIQuest.TargetActorName, Matches);
		if (Matches.Num() > 0) TargetAct = Matches[0];
	}
	if (!ToolAct && !PrecomputedNextAIQuest.RequiredToolName.IsEmpty())
	{
		TArray<AActor*> Matches;
		FindMatchingActorsForTarget(PrecomputedNextAIQuest.RequiredToolName, Matches);
		if (Matches.Num() > 0) ToolAct = Matches[0];
	}

	// Safety: never re-issue the quest that was JUST completed (same actor or same station system).
	bool bReplacedBufferedTarget = false;
	if (TargetAct && LastBrokenQuestActor.IsValid())
	{
		AActor* JustCompleted = LastBrokenQuestActor.Get();
		const bool bSameActor = (TargetAct == JustCompleted);
		const bool bSameSystem = ClassifyTargetSystemCategory(TargetAct, PrecomputedNextAIQuest.TargetActorName) ==
		                         ClassifyTargetSystemCategory(JustCompleted, JustCompleted->GetActorNameOrLabel());
		if (bSameActor || bSameSystem)
		{
			AActor* Fresh = PickNextBreakableActorInShip();
			if (Fresh && Fresh != JustCompleted)
			{
				TargetAct = Fresh;
				ToolAct = PickNextToolActorInShip(Fresh);
				bReplacedBufferedTarget = true;
			}
		}
	}

	if (!TargetAct) TargetAct = PickNextBreakableActorInShip();
	if (!ToolAct) ToolAct = PickNextToolActorInShip(TargetAct);

	if (bReplacedBufferedTarget)
	{
		const FString NewTargetName = TargetAct ? TargetAct->GetActorNameOrLabel() : TEXT("Station System");
		const FString NewToolName = ToolAct ? ToolAct->GetActorNameOrLabel() : TEXT("Tool");
		PrecomputedNextAIQuest.QuestKey.Reset();
		PrecomputedNextAIQuest.QuestTitle = FString::Printf(TEXT("EMERGENCY: Service %s"), *NewTargetName);
		PrecomputedNextAIQuest.QuestInstruction = FString::Printf(TEXT("Take %s (Cyan Box) and service %s (3D Box) & Press [E]!"), *NewToolName, *NewTargetName);
		PrecomputedNextAIQuest.PrecomputedFlightDirectorAdvice = FString::Printf(
			TEXT("Flight Director: New fault detected at %s. Acquire the %s and press E at the target."), *NewTargetName, *NewToolName);
	}

	const FString TargetName = TargetAct ? TargetAct->GetActorNameOrLabel() : PrecomputedNextAIQuest.TargetActorName;
	const FString ToolName = ToolAct ? ToolAct->GetActorNameOrLabel() : PrecomputedNextAIQuest.RequiredToolName;

	CurrentQuestSource = PrecomputedNextAIQuest.QuestSource;
	CurrentQuestKey = PrecomputedNextAIQuest.QuestKey.IsEmpty() ? MakeNormalizedQuestKey(TargetName, ToolName) : PrecomputedNextAIQuest.QuestKey;
	CurrentQuestTitle = PrecomputedNextAIQuest.QuestTitle;
	CurrentQuestWhatToDo = PrecomputedNextAIQuest.QuestInstruction;
	CurrentQuestTargetName = TargetName;
	CurrentQuestRequiredToolName = ToolName;
	ActiveQuestTargetActor = TargetAct;
	ActiveQuestRequiredToolActor = ToolAct;
	bHasActiveAIQuest = true;
	CurrentQuestStartTimeSeconds = GetMissionElapsedTimeSeconds();
	RecordQuestTargetAndToolUsage(TargetAct, TargetName, ToolAct, ToolName);
	SessionAssignedQuestKeys.Add(CurrentQuestKey);
	SessionAssignedQuestKeys.Add(MakeNormalizedQuestKey(TargetName, ToolName));
	TimeSinceLastBreakdownCheck = 0.0f;

	const FVector TargetLoc = TargetAct ? TargetAct->GetActorLocation() : (GetOwner() ? GetOwner()->GetActorLocation() + FVector(200.0f, 0.0f, 0.0f) : FVector::ZeroVector);

	// 1. Spawn/Update 3D Markers immediately in 0.00ms!
	SpawnOrUpdate3DQuestMarker(TargetAct, TargetLoc, CurrentQuestTitle, CurrentQuestWhatToDo, ToolName);
	if (ToolAct)
	{
		SpawnOrUpdate3DToolMarker(ToolAct, ToolName, TargetName);
	}

	// 2. Apply precomputed Meta AI Risk & Lighting
	CurrentAIRiskLevel = PrecomputedNextAIQuest.PrecomputedRiskLevel;
	CurrentAIRiskScorePercent = PrecomputedNextAIQuest.PrecomputedRiskScore;
	if (bAllowAIToControlShipLights)
	{
		ApplyShipLightingPreset(TEXT("critical_red"), TEXT("ALL_LIGHTS"));
	}

	// 3. Cut whatever the Flight Director is still saying and announce the new quest immediately
	InterruptFlightDirectorVoice();
	if (!PrecomputedNextAIQuest.PrecomputedFlightDirectorAdvice.IsEmpty())
	{
		TransmitFlightDirectorRadioComms(PrecomputedNextAIQuest.PrecomputedFlightDirectorAdvice, 8.0f);
	}

	// 4. Apply precomputed physics/light commands from Meta AI. The buffered JSON also contains an 'ai_quest'
	//    block describing THIS quest; it must not be parsed again or it re-buffers the same quest forever.
	if (!PrecomputedNextAIQuest.PrecomputedRawAIJson.IsEmpty())
	{
		FString ExtractedMsg;
		bIsPrecomputingNextQuest = false;
		bSuppressAIQuestParsing = true;
		ExecuteAIPhysicsResponseJson(PrecomputedNextAIQuest.PrecomputedRawAIJson, ExtractedMsg);
		bSuppressAIQuestParsing = false;
	}
	PrecomputedNextAIQuest.PrecomputedRawAIJson.Reset();

	PrintScreenDebug(
		FString::Printf(TEXT("=== [PIPELINE: INSTANT QUEST #%d (0.00ms)] %s | Tool: %s -> Target: %s ==="),
			CompletedAIQuestsCount + 1, *CurrentQuestTitle, *ToolName, *TargetName),
		FColor::Red,
		7.0f
	);

	// Sync to database catalog
	if (bSaveAndFetchQuestsFromDatabase)
	{
		SyncQuestToDatabaseCatalog(
			CurrentQuestKey,
			CurrentQuestSource,
			CurrentQuestTitle,
			CurrentQuestWhatToDo,
			TargetName,
			ToolName,
			false,
			0.0f
		);
	}

	LogTrainingEventToDatabase(TEXT("ai_quest_started_pipeline"), TargetName, CurrentQuestTitle);

	// 5. Immediately launch prefetching for the NEXT upcoming quest in the background!
	StartBackgroundPrecomputeNextQuest();

	return true;
}

void UMetaAIComponent::TriggerNewShipBreakdownAndAIQuest(const FString& SpecificSystemOrReason)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Safeguard: Once mastery is achieved, station is fully secured and nominal!
	if (bMasteryThresholdReached || ProblemSolvingProficiencyScore >= ProblemSolvingMasteryThreshold)
	{
		CurrentAIRiskLevel = TEXT("OPTIMAL");
		CurrentAIRiskScorePercent = 0.0f;
		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("normal_white"), TEXT("ALL_LIGHTS"));
		}
		ActivateDiagnosticTabletEvaluationState();
		return;
	}

	// Guard against concurrent duplicate triggers while a quest is active or an AI breakdown call is in flight!
	if (bHasActiveAIQuest || bIsAwaitingBreakdownAIResponse)
	{
		return;
	}

	if (DiscoveredLevelTools.Num() == 0 && DiscoveredLevelTargets.Num() == 0)
	{
		DiscoverAllLevelToolsAndTerminals();
	}

	// 1. Pick target and tool immediately so trainee gets instant, zero-latency 3D marker and instruction!
	AActor* ChosenTarget = nullptr;
	AActor* ChosenTool = nullptr;
	FSpaceQuestDefinition PickedDef;
	const bool bFromDbOrManual = TryPickUncompletedManualOrDatabaseQuest(PickedDef, ChosenTarget, ChosenTool);

	if (!ChosenTarget)
	{
		ChosenTarget = PickNextBreakableActorInShip();
	}
	if (!ChosenTool)
	{
		ChosenTool = PickNextToolActorInShip(ChosenTarget);
	}

	const FString TargetName = ChosenTarget ? ChosenTarget->GetActorNameOrLabel() : TEXT("Terminal_elctric");
	const FString ToolName = ChosenTool ? ChosenTool->GetActorNameOrLabel() : TEXT("Digi_Wrench");
	const FString QKey = MakeNormalizedQuestKey(TargetName, ToolName);

	FString QTitle = bFromDbOrManual && !PickedDef.QuestTitle.IsEmpty()
		? PickedDef.QuestTitle
		: FString::Printf(TEXT("EMERGENCY: Repair %s"), *TargetName);

	FString QInstruction = bFromDbOrManual && !PickedDef.QuestInstruction.IsEmpty()
		? PickedDef.QuestInstruction
		: FString::Printf(TEXT("Take %s (Cyan Box) and bring to %s (3D Box) & Press [E]!"), *ToolName, *TargetName);

	CurrentQuestSource = bFromDbOrManual ? PickedDef.QuestSource : TEXT("META_AI_AUTONOMOUS");
	CurrentQuestKey = QKey;
	CurrentQuestTitle = QTitle;
	CurrentQuestWhatToDo = QInstruction;
	CurrentQuestTargetName = TargetName;
	CurrentQuestRequiredToolName = ToolName;
	ActiveQuestTargetActor = ChosenTarget;
	ActiveQuestRequiredToolActor = ChosenTool;
	bHasActiveAIQuest = true;
	CurrentQuestStartTimeSeconds = GetMissionElapsedTimeSeconds();
	RecordQuestTargetAndToolUsage(ChosenTarget, TargetName, ChosenTool, ToolName);
	SessionAssignedQuestKeys.Add(QKey);
	SessionAssignedQuestKeys.Add(MakeNormalizedQuestKey(TargetName, ToolName));
	TimeSinceLastBreakdownCheck = 0.0f;

	const FVector TargetLoc = ChosenTarget
		? ChosenTarget->GetActorLocation()
		: (GetOwner() ? GetOwner()->GetActorLocation() + FVector(200.0f, 0.0f, 0.0f) : FVector::ZeroVector);

	// Spawn/Update the 3D Quest Marker Box & Arrow at the broken target
	SpawnOrUpdate3DQuestMarker(
		ChosenTarget,
		TargetLoc,
		CurrentQuestTitle,
		CurrentQuestWhatToDo,
		ToolName
	);

	// Spawn/Update the Cyan Tool Marker Box & Arrow at the required tool
	if (ChosenTool)
	{
		SpawnOrUpdate3DToolMarker(ChosenTool, ToolName, TargetName);
	}

	// Apply emergency warning feedback immediately (0ms) so trainee knows danger is active
	CurrentAIRiskScorePercent = FMath::Clamp(CurrentAIRiskScorePercent + 25.0f, 65.0f, 95.0f);
	CurrentAIRiskLevel = (CurrentAIRiskScorePercent >= 85.0f) ? TEXT("CRITICAL") : TEXT("HIGH");
	if (bAllowAIToControlShipLights)
	{
		ApplyShipLightingPreset(TEXT("critical_red"), TEXT("ALL_LIGHTS"));
	}

	InterruptFlightDirectorVoice();
	TransmitFlightDirectorRadioComms(
		FString::Printf(TEXT("Flight Director: Emergency at %s. Retrieve the %s and press E at the target."), *TargetName, *ToolName),
		7.0f);

	// Sync to database catalog
	if (bSaveAndFetchQuestsFromDatabase)
	{
		SyncQuestToDatabaseCatalog(
			CurrentQuestKey,
			CurrentQuestSource,
			CurrentQuestTitle,
			CurrentQuestWhatToDo,
			TargetName,
			ToolName,
			false,
			0.0f
		);
	}

	LogTrainingEventToDatabase(TEXT("ai_quest_started"), TargetName, CurrentQuestTitle);

	// 2. Dispatch directive to Meta AI in background to autonomously calculate zero-G physics, lighting, and radio flight coaching
	bIsAwaitingBreakdownAIResponse = true;
	const FString BreakdownDirective = FString::Printf(
		TEXT("AI_AUTONOMOUS_STATION_EMERGENCY [Target='%s', RequiredTool='%s', Reason='%s', CompletedQuests=%d, LastMastery=%.0f/100]: "
			 "You are the autonomous Meta AI Flight Director & Physics Engine presiding over this station. "
			 "Station telemetry reports breakdown at target '%s' requiring tool '%s'. "
			 "Assess and refine 'risk_level' and 'risk_score' (0-100), issue real-time zero-G 'physics_commands' (damping, spin, impulses), "
			 "set emergency lighting in 'light_commands', and transmit authentic Flight Director radio guidance in 'message'."),
		*TargetName,
		*ToolName,
		*(SpecificSystemOrReason.IsEmpty() ? TEXT("orbital_system_malfunction") : SpecificSystemOrReason),
		CompletedAIQuestsCount,
		ProblemSolvingProficiencyScore,
		*TargetName,
		*ToolName
	);

	SendWorldDirectiveToAI(BreakdownDirective);

	// Launch background precomputing pipeline for Quest #2 so it is 100% prepared before Quest #1 finishes!
	StartBackgroundPrecomputeNextQuest();
}

void UMetaAIComponent::CompleteCurrentAIQuest(const FString& CompletionDetails)
{
	if (!bHasActiveAIQuest)
	{
		return;
	}

	const float QuestDurationSeconds = FMath::Max(0.5f, GetMissionElapsedTimeSeconds() - CurrentQuestStartTimeSeconds);
	bHasActiveAIQuest = false;

	// Stop any radio line still playing for the finished quest; short confirmation, next quest will cut it if needed.
	InterruptFlightDirectorVoice();
	TransmitFlightDirectorRadioComms(FString::Printf(TEXT("Flight Director: Repair confirmed on %s."), *CurrentQuestTargetName), 4.0f, false);
	CompletedAIQuestsCount++;
	SessionItemsSnappedCount++;
	TimeSinceLastBreakdownCheck = 0.0f;

	// Mark this quest key as completed so it will NOT be repeated!
	if (CurrentQuestKey.IsEmpty())
	{
		CurrentQuestKey = MakeNormalizedQuestKey(CurrentQuestTargetName, CurrentQuestRequiredToolName, CurrentQuestTitle);
	}
	CompletedOrMasteredQuestKeys.Add(CurrentQuestKey);
	CompletedOrMasteredQuestKeys.Add(MakeNormalizedQuestKey(CurrentQuestTargetName, CurrentQuestRequiredToolName));
	SessionAssignedQuestKeys.Add(CurrentQuestKey);
	SessionAssignedQuestKeys.Add(MakeNormalizedQuestKey(CurrentQuestTargetName, CurrentQuestRequiredToolName));
	RecordQuestTargetAndToolUsage(ActiveQuestTargetActor.Get(), CurrentQuestTargetName, ActiveQuestRequiredToolActor.Get(), CurrentQuestRequiredToolName);

	// Sync completion & duration to the MySQL 'quest_catalog' table!
	if (bSaveAndFetchQuestsFromDatabase)
	{
		SyncQuestToDatabaseCatalog(
			CurrentQuestKey,
			CurrentQuestSource,
			CurrentQuestTitle,
			CurrentQuestWhatToDo,
			CurrentQuestTargetName,
			CurrentQuestRequiredToolName,
			true,
			QuestDurationSeconds
		);
	}

	if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
	{
		ActiveToolMarkerActor->SetActorHiddenInGame(true);
	}

	if (ActiveQuestTargetActor.IsValid())
	{
		if (USpaceAIIdentityComponent* IdComp = ActiveQuestTargetActor->FindComponentByClass<USpaceAIIdentityComponent>())
		{
			IdComp->SetRepairTaskCompleted(true, 100.0f);
		}
	}

	// =========================================================================
	// DEBRIS MANAGEMENT: Destroy/Vaporize disposed scrap or debris actors to preserve FPS!
	// =========================================================================
	const bool bIsDisposalQuest =
		CurrentQuestTitle.Contains(TEXT("Clear"), ESearchCase::IgnoreCase) ||
		CurrentQuestTitle.Contains(TEXT("Dispose"), ESearchCase::IgnoreCase) ||
		CurrentQuestTitle.Contains(TEXT("Scrap"), ESearchCase::IgnoreCase) ||
		CurrentQuestTitle.Contains(TEXT("Debris"), ESearchCase::IgnoreCase) ||
		CurrentQuestWhatToDo.Contains(TEXT("Clear"), ESearchCase::IgnoreCase) ||
		CurrentQuestWhatToDo.Contains(TEXT("Dispose"), ESearchCase::IgnoreCase);

	if (bAutoDestroyDebrisOnDisposalQuest || bIsDisposalQuest)
	{
		if (ActiveQuestRequiredToolActor.IsValid())
		{
			AActor* ToolAct = ActiveQuestRequiredToolActor.Get();
			if (!IsPermanentQuizTabletActor(ToolAct))
			{
				const FString ToolName = ToolAct->GetActorNameOrLabel().ToLower();
				if (ToolName.Contains(TEXT("scrap")) || ToolName.Contains(TEXT("debris")) || ToolName.Contains(TEXT("rubble")) || bIsDisposalQuest)
				{
					DestroyOrDisposeDebrisActor(ToolAct);
				}
			}
		}

		if (ActiveQuestTargetActor.IsValid())
		{
			AActor* TargetAct = ActiveQuestTargetActor.Get();
			if (!IsPermanentQuizTabletActor(TargetAct))
			{
				const FString TargetName = TargetAct->GetActorNameOrLabel().ToLower();
				if (TargetName.Contains(TEXT("scrap")) || TargetName.Contains(TEXT("debris")) || TargetName.Contains(TEXT("obstacle")) || bIsDisposalQuest)
				{
					DestroyOrDisposeDebrisActor(TargetAct);
				}
			}
		}
	}

	// =========================================================================
	// PROBLEM-SOLVING MASTERY PROGRESSION (0 to 100)
	// =========================================================================
	const float TimeBonus = (QuestDurationSeconds <= FastQuestMasteryThresholdSeconds) ? 2.0f : 0.0f;
	const float ScoreIncrement = BaseQuestMasteryIncrement + TimeBonus;
	ProblemSolvingProficiencyScore = FMath::Clamp(ProblemSolvingProficiencyScore + ScoreIncrement, 0.0f, 100.0f);

	if (ProblemSolvingProficiencyScore >= ProblemSolvingMasteryThreshold)
	{
		bMasteryThresholdReached = true;
	}

	if (bMasteryThresholdReached || ProblemSolvingProficiencyScore >= ProblemSolvingMasteryThreshold)
	{
		ActivateDiagnosticTabletEvaluationState();
	}
	else
	{
		// Lower Ship Risk Score dynamically to safe zone upon completing a repair quest!
		CurrentAIRiskScorePercent = FMath::RandRange(18.0f, 28.0f);
		CurrentAIRiskLevel = TEXT("LOW");

		if (bAllowAIToControlShipLights)
		{
			ApplyShipLightingPreset(TEXT("repaired_green"), TEXT("ALL_LIGHTS"));
		}
	}

	// Flash the 3D Quest Box & Floating Arrow EMERALD GREEN for 2.5 seconds!
	QuestCompletedFlashTimer = 2.5f;
	if (ActiveQuestBoxComp)
	{
		ActiveQuestBoxComp->ShapeColor = FColor(30, 255, 110, 255);
		ActiveQuestBoxComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		ActiveQuestBoxComp->MarkRenderStateDirty();
	}
	if (ActiveQuestArrowComp)
	{
		ActiveQuestArrowComp->ArrowColor = FColor(30, 255, 110, 255);
		ActiveQuestArrowComp->MarkRenderStateDirty();
	}
	if (ActiveQuestBeaconLight)
	{
		ActiveQuestBeaconLight->SetLightColor(FLinearColor(0.1f, 1.0f, 0.4f, 1.0f));
	}
	if (ActiveQuestTextComp)
	{
		ActiveQuestTextComp->SetText(FText::FromString(FString::Printf(
			TEXT("[QUEST #%d COMPLETED in %.1fs!]\n%s Repaired! Mastery: %.0f/100..."),
			CompletedAIQuestsCount,
			QuestDurationSeconds,
			*CurrentQuestTargetName,
			ProblemSolvingProficiencyScore
		)));
		ActiveQuestTextComp->SetTextRenderColor(FColor(50, 255, 130, 255));
	}

	MissionName = FString::Printf(TEXT("Completed %d Quests | Mastery: %.0f/100 (Last: %s in %.1fs)"),
		CompletedAIQuestsCount, ProblemSolvingProficiencyScore, *CurrentQuestTargetName, QuestDurationSeconds);

	LogTrainingEventToDatabase(
		TEXT("ai_quest_completed"),
		CurrentQuestTargetName,
		FString::Printf(TEXT("%s [Completed in %.1fs | Mastery=%.0f/100 | Key=%s]"), *CompletionDetails, QuestDurationSeconds, ProblemSolvingProficiencyScore, *CurrentQuestKey)
	);

	PrintScreenDebug(
		FString::Printf(TEXT("=== [QUEST #%d COMPLETED IN %.1fs!] '%s' Repaired -> Problem-Solving Mastery: %.0f/100 ==="),
			CompletedAIQuestsCount, QuestDurationSeconds, *CurrentQuestTargetName, ProblemSolvingProficiencyScore),
		FColor::Green,
		8.0f
	);

	// =========================================================================
	// PROBLEM-SOLVING MASTERY PROGRESSION & CONTINUOUS STATION CHALLENGES
	// =========================================================================
	if (bMasteryThresholdReached && CompletedAIQuestsCount >= 5)
	{
		// Clean up all loose debris from the ship to restore clean condition and boost FPS!
		CleanupAllExcessDebris();

		PrintScreenDebug(
			FString::Printf(TEXT("=== [MASTER ASTRONAUT] Mastery: %.0f/100! Advanced Station Maintenance Active ==="),
				ProblemSolvingProficiencyScore),
			FColor::Green,
			8.0f
		);
	}

	// Update live MySQL session immediately with completed quest count and elapsed time
	if (bEnablePhpDatabaseSync && !PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		TSharedPtr<FJsonObject> SessPayload = MakeShareable(new FJsonObject());
		SessPayload->SetStringField(TEXT("action"), TEXT("finish_session"));
		SessPayload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
		SessPayload->SetStringField(TEXT("status"), TEXT("completed"));
		SessPayload->SetNumberField(TEXT("duration_seconds"), GetMissionElapsedTimeSeconds());
		SessPayload->SetNumberField(TEXT("items_grabbed_count"), SessionItemsGrabbedCount);
		SessPayload->SetNumberField(TEXT("items_snapped_count"), CompletedAIQuestsCount);
		SessPayload->SetNumberField(TEXT("wall_launches_count"), SessionWallLaunchesCount);
		SendJsonToPhpBackend(SessPayload);
	}

	// Schedule the NEXT Endless Real-Time Breakdown & AI Quest ONLY if mastery threshold has NOT been reached!
	if (bEnableEndlessAIGameMaster && GetWorld() && !bMasteryThresholdReached && ProblemSolvingProficiencyScore < ProblemSolvingMasteryThreshold)
	{
		GetWorld()->GetTimerManager().ClearTimer(NextEndlessQuestTimerHandle);
		GetWorld()->GetTimerManager().SetTimer(
			NextEndlessQuestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				// 1. Try instant 0.00ms activation from precomputed background pipeline!
				if (!ActivatePrecomputedNextQuest())
				{
					// 2. Fallback: activate breakdown locally from database / station rules in 0ms!
					TriggerNewShipBreakdownAndAIQuest(TEXT("next_endless_quest_after_repair"));
				}
			}),
			FMath::Max(1.5f, NextQuestDelayAfterCompletionSeconds * 0.5f),
			false
		);
	}
}

void UMetaAIComponent::DeleteCurrentAIQuest()
{
	bHasActiveAIQuest = false;
	QuestCompletedFlashTimer = 0.0f;
	CurrentQuestTitle = TEXT("None");
	CurrentQuestWhatToDo = TEXT("");
	CurrentQuestTargetName = TEXT("");
	CurrentQuestRequiredToolName = TEXT("");
	MissionName = TEXT("No Active Quest");

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(NextEndlessQuestTimerHandle);
	}

	if (ActiveQuestMarkerActor && IsValid(ActiveQuestMarkerActor))
	{
		ActiveQuestMarkerActor->SetActorHiddenInGame(true);
	}
	if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
	{
		ActiveToolMarkerActor->SetActorHiddenInGame(true);
	}

	PrintScreenDebug(TEXT("[AI Game Master] Active AI Quest deleted/cleared."), FColor::Silver, 5.0f);
}

FString UMetaAIComponent::MakeNormalizedQuestKey(const FString& InTargetName, const FString& InToolName, const FString& InTitle)
{
	FString RawKey = InTargetName.TrimStartAndEnd().ToLower() + TEXT("__") + InToolName.TrimStartAndEnd().ToLower();
	if (InTargetName.TrimStartAndEnd().IsEmpty() && InToolName.TrimStartAndEnd().IsEmpty())
	{
		RawKey = InTitle.TrimStartAndEnd().ToLower();
	}
	RawKey.ReplaceInline(TEXT(" "), TEXT("_"));
	return RawKey;
}

bool UMetaAIComponent::TryPickUncompletedManualOrDatabaseQuest(FSpaceQuestDefinition& OutQuestDef, AActor*& OutMatchedTarget, AActor*& OutMatchedTool)
{
	OutMatchedTarget = nullptr;
	OutMatchedTool = nullptr;

	// 1. Check user's ManualDesignerQuests first (if bMergeManualAndAIQuests is enabled)
	if (bMergeManualAndAIQuests)
	{
		for (FSpaceQuestDefinition& ManualQ : ManualDesignerQuests)
		{
			AActor* TAct = ManualQ.TargetActorInLevel.Get();
			if (!TAct && !ManualQ.TargetActorName.IsEmpty())
			{
				TArray<AActor*> Found;
				FindMatchingActorsForTarget(ManualQ.TargetActorName, Found);
				if (Found.Num() > 0) TAct = Found[0];
			}

			AActor* ToolAct = ManualQ.RequiredToolActorInLevel.Get();
			if (!ToolAct && !ManualQ.RequiredToolName.IsEmpty())
			{
				TArray<AActor*> FoundTools;
				FindMatchingActorsForTarget(ManualQ.RequiredToolName, FoundTools);
				if (FoundTools.Num() > 0) ToolAct = FoundTools[0];
			}

			const FString EffTargetName = TAct ? TAct->GetActorNameOrLabel() : ManualQ.TargetActorName;
			const FString EffToolName = ToolAct ? ToolAct->GetActorNameOrLabel() : ManualQ.RequiredToolName;
			if (ManualQ.QuestKey.IsEmpty())
			{
				ManualQ.QuestKey = MakeNormalizedQuestKey(EffTargetName, EffToolName, ManualQ.QuestTitle);
			}
			ManualQ.QuestSource = TEXT("MANUAL");

			if (!CompletedOrMasteredQuestKeys.Contains(ManualQ.QuestKey) && !SessionAssignedQuestKeys.Contains(ManualQ.QuestKey))
			{
				SessionAssignedQuestKeys.Add(ManualQ.QuestKey);
				OutQuestDef = ManualQ;
				OutQuestDef.TargetActorName = EffTargetName;
				OutQuestDef.RequiredToolName = EffToolName;
				OutMatchedTarget = TAct;
				OutMatchedTool = ToolAct;
				return true;
			}
		}
	}

	// 2. Check Saved Quests fetched from the MySQL 'quest_catalog' Database!
	if (bSaveAndFetchQuestsFromDatabase)
	{
		for (int32 Idx = 0; Idx < CachedDatabaseQuests.Num(); ++Idx)
		{
			const FSpaceQuestDefinition DbQ = CachedDatabaseQuests[Idx];
			const FString Key = DbQ.QuestKey.IsEmpty()
				? MakeNormalizedQuestKey(DbQ.TargetActorName, DbQ.RequiredToolName, DbQ.QuestTitle)
				: DbQ.QuestKey;

			if (CompletedOrMasteredQuestKeys.Contains(Key) || SessionAssignedQuestKeys.Contains(Key))
			{
				continue;
			}

			const FString NormKey = MakeNormalizedQuestKey(DbQ.TargetActorName, DbQ.RequiredToolName);
			if (CompletedOrMasteredQuestKeys.Contains(NormKey) || SessionAssignedQuestKeys.Contains(NormKey))
			{
				continue;
			}

			// Also avoid picking the same system category if other DB quests or systems are available
			const FString DbCategory = ClassifyTargetSystemCategory(nullptr, DbQ.TargetActorName);
			if (RecentlyUsedTargetCategories.Num() > 0 && RecentlyUsedTargetCategories.Last() == DbCategory && CachedDatabaseQuests.Num() > 1)
			{
				continue;
			}

			AActor* TAct = nullptr;
			if (!DbQ.TargetActorName.IsEmpty())
			{
				TArray<AActor*> Found;
				FindMatchingActorsForTarget(DbQ.TargetActorName, Found);
				if (Found.Num() > 0) TAct = Found[0];
			}
			// Only activate a DB quest if its target Actor exists in this level!
			if (!TAct)
			{
				continue;
			}

			AActor* ToolAct = nullptr;
			if (!DbQ.RequiredToolName.IsEmpty())
			{
				TArray<AActor*> FoundTools;
				FindMatchingActorsForTarget(DbQ.RequiredToolName, FoundTools);
				if (FoundTools.Num() > 0) ToolAct = FoundTools[0];
			}

			OutQuestDef = DbQ;
			OutQuestDef.QuestKey = Key;
			OutQuestDef.QuestSource = DbQ.QuestSource.IsEmpty() ? TEXT("DATABASE_SAVED") : DbQ.QuestSource;
			OutMatchedTarget = TAct;
			OutMatchedTool = ToolAct;

			const FString RecTargetName = DbQ.TargetActorName;
			const FString RecToolName = DbQ.RequiredToolName;

			// Mark as assigned so it cannot be picked again in this session
			SessionAssignedQuestKeys.Add(Key);
			SessionAssignedQuestKeys.Add(NormKey);
			SessionAssignedQuestKeys.Add(DbQ.QuestKey);
			CompletedOrMasteredQuestKeys.Add(Key);
			CompletedOrMasteredQuestKeys.Add(NormKey);

			// Record usage BEFORE removing element from array so no dangling reference occurs!
			RecordQuestTargetAndToolUsage(TAct, RecTargetName, ToolAct, RecToolName);

			// Remove from in-memory catalog for this session to guarantee non-repetition!
			CachedDatabaseQuests.RemoveAt(Idx);
			return true;
		}
	}

	return false;
}

void UMetaAIComponent::SyncQuestToDatabaseCatalog(const FString& InQuestKey, const FString& InSource, const FString& InTitle, const FString& InInstruction, const FString& InTargetName, const FString& InToolName, bool bIsCompleted, float CompletionDurationSec)
{
	if (!bEnablePhpDatabaseSync || !bSaveAndFetchQuestsFromDatabase || PhpApiUrl.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Payload = MakeShareable(new FJsonObject());
	Payload->SetStringField(TEXT("action"), TEXT("sync_quest_catalog"));
	Payload->SetStringField(TEXT("session_uuid"), CurrentSessionUUID);
	Payload->SetStringField(TEXT("trainee_name"), TraineeName);
	Payload->SetStringField(TEXT("quest_key"), InQuestKey.IsEmpty() ? MakeNormalizedQuestKey(InTargetName, InToolName, InTitle) : InQuestKey);
	Payload->SetStringField(TEXT("quest_source"), InSource.IsEmpty() ? TEXT("AI_GENERATED") : InSource);
	Payload->SetStringField(TEXT("quest_title"), InTitle);
	Payload->SetStringField(TEXT("quest_instruction"), InInstruction);
	Payload->SetStringField(TEXT("target_actor"), InTargetName);
	Payload->SetStringField(TEXT("required_item"), InToolName);
	Payload->SetStringField(TEXT("risk_level"), CurrentAIRiskLevel);
	Payload->SetNumberField(TEXT("risk_score"), CurrentAIRiskScorePercent);
	Payload->SetBoolField(TEXT("is_completed"), bIsCompleted);
	Payload->SetNumberField(TEXT("completion_duration_sec"), CompletionDurationSec);
	Payload->SetNumberField(TEXT("completion_time_sec"), CompletionDurationSec);
	Payload->SetNumberField(TEXT("completed_quests_count"), CompletedAIQuestsCount);
	Payload->SetNumberField(TEXT("session_duration_sec"), GetMissionElapsedTimeSeconds());

	SendJsonToPhpBackend(Payload);
}

void UMetaAIComponent::AddAndActivateManualQuest(const FString& QuestTitle, const FString& QuestInstruction, AActor* TargetActor, AActor* RequiredToolActor, bool bShow3DBoxAndArrow)
{
	FSpaceQuestDefinition NewManualQuest;
	NewManualQuest.QuestTitle = QuestTitle;
	NewManualQuest.QuestInstruction = QuestInstruction;
	NewManualQuest.TargetActorInLevel = TargetActor;
	NewManualQuest.TargetActorName = TargetActor ? TargetActor->GetActorNameOrLabel() : TEXT("");
	NewManualQuest.RequiredToolActorInLevel = RequiredToolActor;
	NewManualQuest.RequiredToolName = RequiredToolActor ? RequiredToolActor->GetActorNameOrLabel() : TEXT("");
	NewManualQuest.bShow3DQuestBoxAndArrow = bShow3DBoxAndArrow;
	NewManualQuest.QuestSource = TEXT("MANUAL");
	NewManualQuest.QuestKey = MakeNormalizedQuestKey(NewManualQuest.TargetActorName, NewManualQuest.RequiredToolName, QuestTitle);

	ManualDesignerQuests.Add(NewManualQuest);

	CurrentQuestSource = TEXT("MANUAL");
	CurrentQuestKey = NewManualQuest.QuestKey;
	ActiveQuestRequiredToolActor = RequiredToolActor;

	const FVector TargetLoc = TargetActor
		? TargetActor->GetActorLocation()
		: (GetOwner() ? GetOwner()->GetActorLocation() + FVector(250.0f, 0.0f, 40.0f) : FVector::ZeroVector);

	const bool bPrevSpawn = bSpawn3DQuestBoxAndArrow;
	bSpawn3DQuestBoxAndArrow = bShow3DBoxAndArrow;
	SpawnOrUpdate3DQuestMarker(TargetActor, TargetLoc, QuestTitle, QuestInstruction, NewManualQuest.RequiredToolName);
	bSpawn3DQuestBoxAndArrow = bPrevSpawn;

	SyncQuestToDatabaseCatalog(
		CurrentQuestKey,
		TEXT("MANUAL"),
		QuestTitle,
		QuestInstruction,
		NewManualQuest.TargetActorName,
		NewManualQuest.RequiredToolName,
		false,
		0.0f
	);
}

bool UMetaAIComponent::HasValidSpaceshipRoomBounds() const
{
	return (CachedSpaceshipRoomBoundsBox.IsValid != 0) && (CachedSpaceshipRoomBoundsBox.GetVolume() > 1000.0f);
}

FVector UMetaAIComponent::ClampLocationToRoomBounds(const FVector& InLocation, float PaddingCm) const
{
	if (!HasValidSpaceshipRoomBounds())
	{
		return InLocation;
	}
	const FBox SafeBox = CachedSpaceshipRoomBoundsBox.ExpandBy(-FMath::Max(0.0f, PaddingCm));
	return SafeBox.GetClosestPointTo(InLocation);
}

bool UMetaAIComponent::IsLocationInsideRoomBounds(const FVector& InLocation, float PaddingCm) const
{
	if (!HasValidSpaceshipRoomBounds())
	{
		return true;
	}
	const FBox SafeBox = CachedSpaceshipRoomBoundsBox.ExpandBy(-FMath::Max(0.0f, PaddingCm));
	return SafeBox.IsInsideOrOn(InLocation);
}

void UMetaAIComponent::FindAndCacheSpaceshipRoomBounds()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	AActor* FoundBoundsActor = ShipRoomBoundsTriggerBox ? ShipRoomBoundsTriggerBox.Get() : nullptr;

	if (!FoundBoundsActor)
	{
		const FString TargetName = ShipRoomBoundsActorName.TrimStartAndEnd();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Cand = *It;
			if (!Cand)
			{
				continue;
			}
			const FString CandName = Cand->GetActorNameOrLabel();
			const FString CandInternalName = Cand->GetName();

			if ((!TargetName.IsEmpty() && (CandName.Equals(TargetName, ESearchCase::IgnoreCase) || CandInternalName.Equals(TargetName, ESearchCase::IgnoreCase))) ||
				CandName.Contains(TEXT("locaition_siez"), ESearchCase::IgnoreCase) ||
				CandInternalName.Contains(TEXT("locaition_siez"), ESearchCase::IgnoreCase) ||
				CandName.Contains(TEXT("location_size"), ESearchCase::IgnoreCase) ||
				CandName.Contains(TEXT("locaition_size"), ESearchCase::IgnoreCase) ||
				CandName.Contains(TEXT("location_siez"), ESearchCase::IgnoreCase))
			{
				FoundBoundsActor = Cand;
				break;
			}
		}
	}

	if (FoundBoundsActor)
	{
		ShipRoomBoundsTriggerBox = FoundBoundsActor;

		if (const UBoxComponent* BoxComp = FoundBoundsActor->FindComponentByClass<UBoxComponent>())
		{
			CachedSpaceshipRoomBoundsBox = BoxComp->Bounds.GetBox();
		}
		else
		{
			FVector Origin, Extent;
			FoundBoundsActor->GetActorBounds(false, Origin, Extent);
			if (Extent.Size() > 50.0f)
			{
				CachedSpaceshipRoomBoundsBox = FBox(Origin - Extent, Origin + Extent);
			}
			else
			{
				CachedSpaceshipRoomBoundsBox = FoundBoundsActor->GetComponentsBoundingBox(true, true);
			}
		}

		PrintScreenDebug(
			FString::Printf(TEXT("[Spaceship Room Bounds] Linked to TriggerBox '%s'! Room Size: (%.0f x %.0f x %.0f cm)"),
				*FoundBoundsActor->GetActorNameOrLabel(),
				CachedSpaceshipRoomBoundsBox.GetSize().X,
				CachedSpaceshipRoomBoundsBox.GetSize().Y,
				CachedSpaceshipRoomBoundsBox.GetSize().Z),
			FColor::Emerald,
			10.0f
		);
	}
}

void UMetaAIComponent::EnforceSpaceshipRoomBoundsOnAllToolsAndObjects(float DeltaTime)
{
	if (!bEnforceSpaceshipRoomBounds)
	{
		return;
	}

	if (!HasValidSpaceshipRoomBounds())
	{
		FindAndCacheSpaceshipRoomBounds();
		if (!HasValidSpaceshipRoomBounds())
		{
			return;
		}
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FBox SafeBox = CachedSpaceshipRoomBoundsBox.ExpandBy(-RoomBoundaryInnerPaddingCm);
	const FVector RoomCenter = SafeBox.GetCenter();

	auto ClampAndBounceActor = [&](AActor* Actor)
	{
		if (!Actor || !IsValid(Actor) || Actor == ActiveQuestMarkerActor || Actor == ActiveToolMarkerActor)
		{
			return;
		}

		const FVector Loc = Actor->GetActorLocation();
		if (!SafeBox.IsInsideOrOn(Loc))
		{
			const FVector ClampedLoc = SafeBox.GetClosestPointTo(Loc);
			Actor->SetActorLocation(ClampedLoc, false, nullptr, ETeleportType::TeleportPhysics);

			if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Actor->GetRootComponent()))
			{
				if (Prim->IsSimulatingPhysics())
				{
					FVector Vel = Prim->GetPhysicsLinearVelocity();
					const FVector InwardDir = (RoomCenter - ClampedLoc).GetSafeNormal();
					if (FVector::DotProduct(Vel, InwardDir) < 0.0f)
					{
						Vel = InwardDir * FMath::Max(120.0f, Vel.Size() * 0.65f);
						Prim->SetPhysicsLinearVelocity(Vel);
					}
				}
			}
		}
	};

	// 1. Enforce on all discovered Level Tools & Designer Tools
	for (AActor* Tool : DiscoveredLevelTools)
	{
		ClampAndBounceActor(Tool);
	}
	for (AActor* DTool : QuestToolActorsInLevel)
	{
		ClampAndBounceActor(DTool);
	}
	if (ActiveQuestRequiredToolActor.IsValid())
	{
		ClampAndBounceActor(ActiveQuestRequiredToolActor.Get());
	}

	// 2. Enforce on all Level Obstacles & Spawned Debris
	for (AActor* Obstacle : LevelObstacleActors)
	{
		ClampAndBounceActor(Obstacle);
	}
	for (const TWeakObjectPtr<AActor>& Spawned : SpawnedObstacleActors)
	{
		if (Spawned.IsValid())
		{
			ClampAndBounceActor(Spawned.Get());
		}
	}

	// 3. Optional Viewport Boundary Debug Box
	if (bDrawRoomBoundaryDebug)
	{
		DrawDebugBox(World, CachedSpaceshipRoomBoundsBox.GetCenter(), CachedSpaceshipRoomBoundsBox.GetExtent(), FQuat::Identity, FColor::Magenta, false, -1.0f, 0, 2.0f);
	}
}

// =============================================================================
// 10. Spaceship Oxygen Simulation & Life Support System
// =============================================================================

void UMetaAIComponent::UpdateOxygenSimulation(float DeltaTime)
{
	if (!bEnableOxygenSystem || DeltaTime <= 0.0f)
	{
		return;
	}

	// 1. Astronaut Suit Oxygen Consumption over time
	PlayerOxygenCurrent = FMath::Clamp(PlayerOxygenCurrent - (PlayerOxygenConsumptionPerSec * DeltaTime), 0.0f, PlayerOxygenMax);

	// 2. Spaceship Atmospheric Oxygen:
	// If current risk is HIGH or CRITICAL, atmospheric decompression / hull leak occurs!
	if (CurrentAIRiskScorePercent >= 65.0f || CurrentAIRiskLevel == TEXT("CRITICAL") || CurrentAIRiskLevel == TEXT("HIGH"))
	{
		RoomOxygenCurrent = FMath::Clamp(RoomOxygenCurrent - (RoomOxygenBreachLeakRatePerSec * DeltaTime), 0.0f, RoomOxygenMax);
	}
	else if (CurrentAIRiskScorePercent <= 30.0f || bMasteryThresholdReached)
	{
		// Station life support slowly restores atmospheric pressure when systems are stable
		RoomOxygenCurrent = FMath::Clamp(RoomOxygenCurrent + (0.35f * DeltaTime), 0.0f, RoomOxygenMax);
	}

	// 3. Automatic Replenishment when carrying an O2 Tank or near Life Support Consoles
	UWorld* World = GetWorld();
	ACharacter* PlayerChar = World ? UGameplayStatics::GetPlayerCharacter(World, 0) : nullptr;
	if (PlayerChar)
	{
		// Check if player is holding an O2 Tank via SpaceGrabSnapComponent
		if (USpaceGrabSnapComponent* GrabComp = PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>())
		{
			if (AActor* Held = GrabComp->GetHeldActor())
			{
				const FString HeldName = Held->GetActorNameOrLabel().ToLower();
				if (HeldName.Contains(TEXT("oxygen")) || HeldName.Contains(TEXT("o2")) || HeldName.Contains(TEXT("tank")))
				{
					// Slowly replenish suit oxygen from the held tank (+16% per second)
					PlayerOxygenCurrent = FMath::Clamp(PlayerOxygenCurrent + (16.0f * DeltaTime), 0.0f, PlayerOxygenMax);
				}
			}
		}

		// Also check if near any world actor tagged with "Oxygen" or named "Oxygen" / "O2" (within 240 cm)
		const FVector PlayerLoc = PlayerChar->GetActorLocation();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Act = *It;
			if (!Act || Act == PlayerChar) continue;
			const FString ActName = Act->GetActorNameOrLabel().ToLower();
			if (ActName.Contains(TEXT("oxygen")) || ActName.Contains(TEXT("o2")) || Act->ActorHasTag(TEXT("Oxygen")))
			{
				if (FVector::DistSquared(PlayerLoc, Act->GetActorLocation()) <= FMath::Square(240.0f))
				{
					// Near station O2 dispenser: replenish suit oxygen!
					PlayerOxygenCurrent = FMath::Clamp(PlayerOxygenCurrent + (22.0f * DeltaTime), 0.0f, PlayerOxygenMax);
					break;
				}
			}
		}
	}
}

void UMetaAIComponent::RefillPlayerOxygen(float Amount)
{
	if (Amount <= 0.0f)
	{
		PlayerOxygenCurrent = PlayerOxygenMax;
	}
	else
	{
		PlayerOxygenCurrent = FMath::Clamp(PlayerOxygenCurrent + Amount, 0.0f, PlayerOxygenMax);
	}
}

void UMetaAIComponent::RefillRoomOxygen(float Amount)
{
	if (Amount <= 0.0f)
	{
		RoomOxygenCurrent = RoomOxygenMax;
	}
	else
	{
		RoomOxygenCurrent = FMath::Clamp(RoomOxygenCurrent + Amount, 0.0f, RoomOxygenMax);
	}
}

// =============================================================================
// 11. UMG UI Binding Getters (Progress Bars, Text Blocks & Colors)
// =============================================================================

float UMetaAIComponent::GetPlayerOxygenPercent() const
{
	return (PlayerOxygenMax > 0.0f) ? FMath::Clamp(PlayerOxygenCurrent / PlayerOxygenMax, 0.0f, 1.0f) : 0.0f;
}

FText UMetaAIComponent::GetPlayerOxygenText() const
{
	const float Pct = GetPlayerOxygenPercent() * 100.0f;
	return FText::FromString(FString::Printf(TEXT("%.0f%%"), Pct));
}

FLinearColor UMetaAIComponent::GetPlayerOxygenColor() const
{
	const float Pct = GetPlayerOxygenPercent();
	if (Pct <= 0.20f)
	{
		// Urgent Flashing Red when suit oxygen is critical
		const float Flash = (FMath::Sin(FPlatformTime::Seconds() * 8.0f) > 0.0f) ? 1.0f : 0.4f;
		return FLinearColor(1.0f * Flash, 0.15f, 0.15f, 1.0f);
	}
	if (Pct <= 0.45f)
	{
		return FLinearColor(1.0f, 0.65f, 0.1f, 1.0f); // Warning Orange
	}
	return FLinearColor(0.2f, 0.85f, 1.0f, 1.0f); // Healthy Cyan
}

float UMetaAIComponent::GetRoomOxygenPercent() const
{
	return (RoomOxygenMax > 0.0f) ? FMath::Clamp(RoomOxygenCurrent / RoomOxygenMax, 0.0f, 1.0f) : 0.0f;
}

FText UMetaAIComponent::GetRoomOxygenText() const
{
	const float Pct = GetRoomOxygenPercent() * 100.0f;
	return FText::FromString(FString::Printf(TEXT("%.0f%%"), Pct));
}

FLinearColor UMetaAIComponent::GetRoomOxygenColor() const
{
	const float Pct = GetRoomOxygenPercent();
	if (Pct <= 0.25f)
	{
		return FLinearColor(1.0f, 0.2f, 0.2f, 1.0f); // Decompressed Red
	}
	if (Pct <= 0.60f)
	{
		return FLinearColor(1.0f, 0.7f, 0.15f, 1.0f); // Leaking Orange
	}
	return FLinearColor(0.25f, 1.0f, 0.45f, 1.0f); // Pressurized Emerald Green
}

float UMetaAIComponent::GetAIRiskPercent() const
{
	return FMath::Clamp(CurrentAIRiskScorePercent / 100.0f, 0.0f, 1.0f);
}

FText UMetaAIComponent::GetAIRiskText() const
{
	return FText::FromString(FString::Printf(TEXT("AI RISK: %s (%.0f%%)"), *CurrentAIRiskLevel, CurrentAIRiskScorePercent));
}

FLinearColor UMetaAIComponent::GetAIRiskColor() const
{
	if (CurrentAIRiskScorePercent >= 80.0f || CurrentAIRiskLevel.Contains(TEXT("CRIT")))
	{
		return FLinearColor(1.0f, 0.15f, 0.15f, 1.0f); // Critical Red
	}
	if (CurrentAIRiskScorePercent >= 55.0f || CurrentAIRiskLevel.Contains(TEXT("HIGH")))
	{
		return FLinearColor(1.0f, 0.5f, 0.05f, 1.0f); // High Orange
	}
	if (CurrentAIRiskScorePercent >= 30.0f || CurrentAIRiskLevel.Contains(TEXT("MODERATE")))
	{
		return FLinearColor(0.95f, 0.8f, 0.15f, 1.0f); // Moderate Amber
	}
	return FLinearColor(0.2f, 0.9f, 0.45f, 1.0f); // Safe / Optimal Green
}

FText UMetaAIComponent::GetCurrentQuestTitleText() const
{
	if (!CurrentQuestTitle.IsEmpty() && CurrentQuestTitle != TEXT("None"))
	{
		return FText::FromString(CurrentQuestTitle);
	}
	return bMasteryThresholdReached
		? FText::FromString(TEXT("Station Stabilized - Nominal"))
		: FText::FromString(TEXT("Awaiting AI Mission Control..."));
}

FText UMetaAIComponent::GetCurrentQuestInstructionText() const
{
	if (!CurrentQuestWhatToDo.IsEmpty())
	{
		return FText::FromString(CurrentQuestWhatToDo);
	}
	return bMasteryThresholdReached
		? FText::FromString(TEXT("All systems optimal. Report to Diagnostic Tablet."))
		: FText::FromString(TEXT("Follow AI Flight Director telemetry."));
}

float UMetaAIComponent::GetProblemSolvingMasteryPercent() const
{
	return FMath::Clamp(ProblemSolvingProficiencyScore / 100.0f, 0.0f, 1.0f);
}

FText UMetaAIComponent::GetProblemSolvingMasteryText() const
{
	return FText::FromString(FString::Printf(TEXT("Mastery: %.0f / 100"), ProblemSolvingProficiencyScore));
}

FText UMetaAIComponent::GetLatestFlightDirectorAdviceText() const
{
	if (!CachedFlightDirectorAdvice.IsEmpty())
	{
		return FText::FromString(CachedFlightDirectorAdvice);
	}
	if (!HistoricalLastAdvice.IsEmpty())
	{
		return FText::FromString(HistoricalLastAdvice);
	}
	return FText::FromString(TEXT("Flight Director: Suit telemetry online. Zero-G station diagnostics nominal."));
}

FText UMetaAIComponent::GetCurrentContextualControlHintText() const
{
	// 0. Pre-Emergency Orientation Training Hints (Flight Director Testing Protocols)
	if (IsInOrientationTraining())
	{
		switch (CurrentTrainingPhase)
		{
		case EAstronautTrainingPhase::Step1_ThrusterMovement:
			return FText::FromString(TEXT("TRAINING [1/4]: Calibrate 3D Thrusters | Use [WASD] to fly horizontally, [Space] to rise, [Ctrl] to descend!"));
		case EAstronautTrainingPhase::Step2_WallGrabAndLaunch:
			return FText::FromString(TEXT("TRAINING [2/4]: Bulkhead Tether | Aim at any station bulkhead/wall, hold [LMB] to grab and anchor, then release to launch!"));
		case EAstronautTrainingPhase::Step3_GrabEquipment:
			return FText::FromString(TEXT("TRAINING [3/4]: Equipment Retrieval | Fly to the tool marked with the CYAN box and click [LMB] to grab it!"));
		case EAstronautTrainingPhase::Step4_CarryAndCalibrate:
			return FText::FromString(TEXT("TRAINING [4/4]: Terminal Sync | Carry your held tool to the Calibration Terminal (ORANGE box) and press [E]!"));
		case EAstronautTrainingPhase::CertifiedReady:
			return FText::FromString(TEXT("TRAINING COMPLETE: 100% Certified Ready! Station systems going live into threat operations..."));
		default:
			break;
		}
	}

	// 1. Oxygen Emergency Priority Hint
	if (PlayerOxygenCurrent <= 20.0f)
	{
		return FText::FromString(TEXT("CRITICAL O2: Suit oxygen below 20%! Find an Oxygen Tank or Life Support Console immediately!"));
	}

	UWorld* World = GetWorld();
	ACharacter* PlayerChar = World ? UGameplayStatics::GetPlayerCharacter(World, 0) : nullptr;
	USpaceGrabSnapComponent* GrabComp = PlayerChar ? PlayerChar->FindComponentByClass<USpaceGrabSnapComponent>() : nullptr;

	// 2. Carrying an Item Hint
	if (GrabComp && GrabComp->IsHoldingObject())
	{
		AActor* HeldActor = GrabComp->GetHeldActor();
		const FString HeldName = HeldActor ? HeldActor->GetActorNameOrLabel() : TEXT("Item");
		const FString TargetName = CurrentQuestTargetName.IsEmpty() ? TEXT("Target Terminal") : CurrentQuestTargetName;

		return FText::FromString(FString::Printf(
			TEXT("Carrying '%s' | Fly to '%s' & Press [E] | [LMB] on walls to crawl/launch | [RMB] Drop | [Shift+RMB] Throw"),
			*HeldName, *TargetName
		));
	}

	// 3. Near a Grabbable Tool Hint
	if (GrabComp)
	{
		AActor* GrabbableActor = nullptr;
		if (GrabComp->IsGrabbableInReach(GrabbableActor) && GrabbableActor)
		{
			const FString GrabName = GrabbableActor->GetActorNameOrLabel();
			return FText::FromString(FString::Printf(
				TEXT("Tool in Reach: Press [RMB] to Grab '%s' | [LMB] on walls to maneuver in Zero-G"),
				*GrabName
			));
		}
	}

	// 4. Near Broken Target Terminal Hint
	if (bHasActiveAIQuest && ActiveQuestTargetActor.IsValid() && PlayerChar)
	{
		const float DistToTarget = FVector::Dist(PlayerChar->GetActorLocation(), ActiveQuestTargetActor->GetActorLocation());
		if (DistToTarget <= 380.0f)
		{
			return FText::FromString(FString::Printf(
				TEXT("Terminal in Reach: Press [E] to Service / Calibrate '%s'!"),
				*CurrentQuestTargetName
			));
		}
	}

	// 5. Station Stabilized Milestone Hint
	if (bMasteryThresholdReached)
	{
		return FText::FromString(TEXT("Station Stabilized (90/100 Mastery): All critical systems sealed. Random breakdowns halted."));
	}

	// 6. Active Quest Navigation Hint
	if (bHasActiveAIQuest)
	{
		if (!CurrentQuestRequiredToolName.IsEmpty())
		{
			return FText::FromString(FString::Printf(
				TEXT("Objective: Grab '%s' (Cyan Box) with [RMB] -> Bring to '%s' (Orange Box) | [LMB] on walls to Launch"),
				*CurrentQuestRequiredToolName, *CurrentQuestTargetName
			));
		}
		return FText::FromString(FString::Printf(
			TEXT("Objective: Fly to '%s' (Orange Box) & Press [E] | [LMB] on walls to Launch | [RMB] Air Brake"),
			*CurrentQuestTargetName
		));
	}

	// 7. Stranded in Open Space Hint
	if (PlayerChar)
	{
		if (const USpaceFloatingComponent* FloatComp = PlayerChar->FindComponentByClass<USpaceFloatingComponent>())
		{
			if (FloatComp->IsStrandedInOpenSpace())
			{
				return FText::FromString(TEXT("Cabin Center: Auto micro-drift active (Aim at any wall) | Press [WASD / Space / Ctrl] to fire emergency RCS thrusters!"));
			}
		}
	}

	// 8. General Zero-G Flight Training Controls Hint
	return FText::FromString(TEXT("Zero-G Navigation: [LMB] on walls to Grab & Launch | [Space / Ctrl] Vertical Thrust | [RMB] Air Brake (empty hands)"));
}

FAstronautHUDState UMetaAIComponent::GetAstronautHUDState() const
{
	FAstronautHUDState HUD;

	// Pre-Emergency Orientation Protocol
	HUD.bIsInOrientationTraining = IsInOrientationTraining();
	HUD.TrainingPhase = CurrentTrainingPhase;
	HUD.TrainingStepText = GetTrainingStepProgressText();
	switch (CurrentTrainingPhase)
	{
	case EAstronautTrainingPhase::Step1_ThrusterMovement:
		HUD.TrainingProgressPercent = 0.25f;
		break;
	case EAstronautTrainingPhase::Step2_WallGrabAndLaunch:
		HUD.TrainingProgressPercent = 0.50f;
		break;
	case EAstronautTrainingPhase::Step3_GrabEquipment:
		HUD.TrainingProgressPercent = 0.75f;
		break;
	case EAstronautTrainingPhase::Step4_CarryAndCalibrate:
		HUD.TrainingProgressPercent = 0.90f;
		break;
	case EAstronautTrainingPhase::CertifiedReady:
	case EAstronautTrainingPhase::ThreatOperationsActive:
		HUD.TrainingProgressPercent = 1.0f;
		break;
	default:
		HUD.TrainingProgressPercent = 0.0f;
		break;
	}

	HUD.PlayerOxygenPercent = GetPlayerOxygenPercent();
	HUD.PlayerOxygenScore = PlayerOxygenCurrent;
	HUD.PlayerOxygenSecondsRemaining = (PlayerOxygenConsumptionPerSec > 0.0f) ? (PlayerOxygenCurrent / PlayerOxygenConsumptionPerSec) : 300.0f;
	HUD.PlayerOxygenText = GetPlayerOxygenText();
	HUD.PlayerOxygenColor = GetPlayerOxygenColor();

	HUD.RoomOxygenPercent = GetRoomOxygenPercent();
	HUD.RoomOxygenScore = RoomOxygenCurrent;
	HUD.RoomOxygenText = GetRoomOxygenText();
	HUD.RoomOxygenColor = GetRoomOxygenColor();
	HUD.bIsPlayerOxygenLow = (PlayerOxygenCurrent <= 25.0f);
	HUD.bIsRoomOxygenDepleting = (CurrentAIRiskScorePercent >= 65.0f);

	HUD.AIRiskPercent = GetAIRiskPercent();
	HUD.AIRiskScore = CurrentAIRiskScorePercent;
	HUD.AIRiskLevel = CurrentAIRiskLevel;
	HUD.AIRiskText = GetAIRiskText();
	HUD.AIRiskColor = GetAIRiskColor();

	HUD.bHasActiveQuest = bHasActiveAIQuest;
	HUD.QuestTitleText = GetCurrentQuestTitleText();
	HUD.QuestInstructionText = GetCurrentQuestInstructionText();
	HUD.QuestTargetNameText = FText::FromString(CurrentQuestTargetName);
	HUD.QuestRequiredToolText = FText::FromString(CurrentQuestRequiredToolName);

	HUD.ProblemSolvingMasteryPercent = GetProblemSolvingMasteryPercent();
	HUD.ProblemSolvingMasteryScore = ProblemSolvingProficiencyScore;
	HUD.ProblemSolvingMasteryText = GetProblemSolvingMasteryText();
	HUD.bMasteryThresholdReached = bMasteryThresholdReached;

	HUD.ContextualControlHintText = GetCurrentContextualControlHintText();
	HUD.FlightDirectorAdviceText = GetLatestFlightDirectorAdviceText();
	return HUD;
}

FAstronautHUDState UMetaAIComponent::GetAstronautHUDStateGlobal(const UObject* WorldContextObject)
{
	if (UMetaAIComponent* MetaAI = GetMetaAIComponentGlobal(WorldContextObject))
	{
		return MetaAI->GetAstronautHUDState();
	}
	return FAstronautHUDState();
}

UMetaAIComponent* UMetaAIComponent::GetMetaAIComponentGlobal(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}
	UWorld* World = WorldContextObject->GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor)
		{
			if (UMetaAIComponent* Comp = Actor->FindComponentByClass<UMetaAIComponent>())
			{
				return Comp;
			}
		}
	}
	return nullptr;
}

// =========================================================================
// 12. Pre-Emergency Astronaut Orientation & Competency Training Protocol
// =========================================================================

bool UMetaAIComponent::IsInOrientationTraining() const
{
	return bRequireOrientationTrainingBeforeThreats &&
		(CurrentTrainingPhase == EAstronautTrainingPhase::Step1_ThrusterMovement ||
		 CurrentTrainingPhase == EAstronautTrainingPhase::Step2_WallGrabAndLaunch ||
		 CurrentTrainingPhase == EAstronautTrainingPhase::Step3_GrabEquipment ||
		 CurrentTrainingPhase == EAstronautTrainingPhase::Step4_CarryAndCalibrate ||
		 CurrentTrainingPhase == EAstronautTrainingPhase::CertifiedReady);
}

FText UMetaAIComponent::GetTrainingStepProgressText() const
{
	switch (CurrentTrainingPhase)
	{
	case EAstronautTrainingPhase::Step1_ThrusterMovement:
		return FText::FromString(TEXT("Orientation [1/4]: 3D Thruster Calibration"));
	case EAstronautTrainingPhase::Step2_WallGrabAndLaunch:
		return FText::FromString(TEXT("Orientation [2/4]: Bulkhead Tether & Slingshot"));
	case EAstronautTrainingPhase::Step3_GrabEquipment:
		return FText::FromString(TEXT("Orientation [3/4]: Equipment Retrieval & Grip"));
	case EAstronautTrainingPhase::Step4_CarryAndCalibrate:
		return FText::FromString(TEXT("Orientation [4/4]: Terminal Sync & Calibration"));
	case EAstronautTrainingPhase::CertifiedReady:
		return FText::FromString(TEXT("CERTIFIED 100% READY: All Systems Nominal"));
	case EAstronautTrainingPhase::ThreatOperationsActive:
		return FText::FromString(TEXT("Flight Operations: Active Emergency Response"));
	default:
		return FText::FromString(TEXT("Standby"));
	}
}

void UMetaAIComponent::StartOrientationTraining()
{
	CurrentTrainingPhase = EAstronautTrainingPhase::Step1_ThrusterMovement;
	TrainingAccumulatedMoveTime = 0.0f;
	TrainingAccumulatedDistanceTraversed = 0.0f;
	TrainingLastPlayerLocation = FVector::ZeroVector;

	// Establish calm, fully operational station atmosphere before emergencies
	CurrentAIRiskScorePercent = 10.0f;
	CurrentAIRiskLevel = TEXT("LOW");
	PlayerOxygenCurrent = PlayerOxygenMax;
	RoomOxygenCurrent = RoomOxygenMax;

	// Reset ship lighting to calm operational cyan / white
	for (FTrackedShipLightState& LightState : TrackedShipLights)
	{
		LightState.ActiveMode = TEXT("solid");
		LightState.TargetColor = FLinearColor(0.2f, 0.85f, 1.0f);
		LightState.TargetIntensityMultiplier = 1.0f;
		if (LightState.LightComp.IsValid())
		{
			LightState.LightComp->SetLightColor(LightState.TargetColor);
			LightState.LightComp->SetIntensity(LightState.OriginalIntensity);
		}
	}

	CachedFlightDirectorAdvice = TEXT("Flight Director: Welcome aboard, Cadet. Before emergency protocols engage, we need to calibrate your EVA suit systems. Step 1: Use thrusters [WASD, Space, Ctrl] to navigate cabin space.");
	LogTrainingEventToDatabase(TEXT("orientation_start"), TEXT("Astronaut"), TEXT("Step1_Thrusters"));
	PrintScreenDebug(TEXT("[AI Orientation] Step 1/4: Use [WASD] to fly horizontally, [Space] to rise, [Ctrl] to descend!"), FColor::Cyan, 10.0f);
}

void UMetaAIComponent::AdvanceOrientationTraining(EAstronautTrainingPhase NextPhase)
{
	CurrentTrainingPhase = NextPhase;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (NextPhase == EAstronautTrainingPhase::Step2_WallGrabAndLaunch)
	{
		CachedFlightDirectorAdvice = TEXT("Flight Director: Thruster calibration verified. Step 2: Test wall anchor tethering. Look at any bulkhead or wall, hold [LMB] to latch on, then release to slingshot launch!");
		LogTrainingEventToDatabase(TEXT("orientation_step_passed"), TEXT("Step1_Thrusters"), TEXT("AdvanceTo_Step2"));
		PrintScreenDebug(TEXT("[AI Orientation] Step 2/4: Look at any bulkhead/wall, hold [LMB] to anchor, release to launch!"), FColor::Turquoise, 10.0f);
	}
	else if (NextPhase == EAstronautTrainingPhase::Step3_GrabEquipment)
	{
		if (DiscoveredLevelTools.Num() == 0)
		{
			DiscoverAllLevelToolsAndTerminals();
		}

		// Pick a genuine engineering tool (Digi_Wrench, Welder, Extinguisher, etc.) and NEVER scrap
		AActor* ChosenTool = PickNextToolActorInShip(nullptr);
		if (!ChosenTool && DiscoveredLevelTools.Num() > 0)
		{
			ChosenTool = DiscoveredLevelTools[0];
		}

		ActiveQuestRequiredToolActor = ChosenTool;

		FString ToolName = TEXT("Tool");
		if (ChosenTool)
		{
			if (const USpaceAIIdentityComponent* IdComp = ChosenTool->FindComponentByClass<USpaceAIIdentityComponent>())
			{
				ToolName = IdComp->GetEffectiveEntityName();
			}
			else
			{
				ToolName = ChosenTool->GetActorNameOrLabel();
			}
			SpawnOrUpdate3DToolMarker(ChosenTool, ToolName, TEXT("Calibration Terminal"));
		}

		CachedFlightDirectorAdvice = FString::Printf(TEXT("Flight Director: Tether launch confirmed! Step 3: Equipment acquisition. Locate the highlighted tool (%s marked in CYAN) and grab it with [LMB]!"), *ToolName);
		LogTrainingEventToDatabase(TEXT("orientation_step_passed"), TEXT("Step2_WallGrab"), TEXT("AdvanceTo_Step3"));
		PrintScreenDebug(FString::Printf(TEXT("[AI Orientation] Step 3/4: Grab the tool marked with the CYAN marker (%s)!"), *ToolName), FColor::Cyan, 10.0f);
	}
	else if (NextPhase == EAstronautTrainingPhase::Step4_CarryAndCalibrate)
	{
		if (DiscoveredLevelTargets.Num() == 0)
		{
			DiscoverAllLevelToolsAndTerminals();
		}

		AActor* ChosenTarget = (DiscoveredLevelTargets.Num() > 0) ? DiscoveredLevelTargets[0] : nullptr;
		FString TargetName = TEXT("Calibration Terminal");
		if (ChosenTarget)
		{
			if (const USpaceAIIdentityComponent* IdComp = ChosenTarget->FindComponentByClass<USpaceAIIdentityComponent>())
			{
				TargetName = IdComp->GetEffectiveEntityName();
			}
			else
			{
				TargetName = ChosenTarget->GetActorNameOrLabel();
			}
		}

		SpawnOrUpdate3DQuestMarker(
			ChosenTarget,
			ChosenTarget ? ChosenTarget->GetActorLocation() : FVector::ZeroVector,
			TEXT("Orientation: Calibrate Tool at Terminal"),
			TEXT("Bring your held tool to the Calibration Terminal (Orange Box) & Press [E]!"),
			TEXT("")
		);

		CachedFlightDirectorAdvice = FString::Printf(TEXT("Flight Director: Tool secured in grip! Step 4: Fly to the Calibration Terminal (%s - Orange Box) and press [E] to synchronize the unit."), *TargetName);
		LogTrainingEventToDatabase(TEXT("orientation_step_passed"), TEXT("Step3_GrabTool"), TEXT("AdvanceTo_Step4"));
		PrintScreenDebug(FString::Printf(TEXT("[AI Orientation] Step 4/4: Carry tool to '%s' (Orange Box) and press [E]!"), *TargetName), FColor::Orange, 10.0f);
	}
}

void UMetaAIComponent::CompleteOrientationTrainingAndCertify()
{
	CurrentTrainingPhase = EAstronautTrainingPhase::CertifiedReady;

	// Hide or remove tool marker if present
	if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
	{
		ActiveToolMarkerActor->SetActorHiddenInGame(true);
	}

	// Flash Quest Box green for success
	if (ActiveQuestBoxComp)
	{
		ActiveQuestBoxComp->ShapeColor = FColor(0, 255, 128, 255);
	}
	if (ActiveQuestTextComp)
	{
		ActiveQuestTextComp->SetText(FText::FromString(TEXT("ORIENTATION CERTIFIED 100% READY!")));
		ActiveQuestTextComp->SetTextRenderColor(FColor(0, 255, 128, 255));
	}

	// Emerald green flash on station lights
	for (FTrackedShipLightState& LightState : TrackedShipLights)
	{
		LightState.ActiveMode = TEXT("solid");
		LightState.TargetColor = FLinearColor(0.0f, 1.0f, 0.5f);
		if (LightState.LightComp.IsValid())
		{
			LightState.LightComp->SetLightColor(LightState.TargetColor);
		}
	}

	ProblemSolvingProficiencyScore = FMath::Clamp(ProblemSolvingProficiencyScore + 5.0f, 0.0f, 100.0f);
	CachedFlightDirectorAdvice = TEXT("Flight Director: Outstanding work, Astronaut! Suit, thrusters, and equipment calibration 100% CERTIFIED. Standby for operational station duty...");
	LogTrainingEventToDatabase(TEXT("orientation_certified"), TEXT("Cadet"), TEXT("100% Ready"));
	PrintScreenDebug(TEXT("[AI Orientation] ALL SYSTEMS NOMINAL: Candidate 100% Certified Ready!"), FColor::Emerald, 10.0f);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			ThreatActivationDelayTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				SkipOrientationTrainingAndStartThreats();
			}),
			6.0f,
			false
		);
	}
}

void UMetaAIComponent::SkipOrientationTrainingAndStartThreats()
{
	CurrentTrainingPhase = EAstronautTrainingPhase::ThreatOperationsActive;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ThreatActivationDelayTimerHandle);
	}

	// Clean up any remaining orientation markers
	if (ActiveToolMarkerActor && IsValid(ActiveToolMarkerActor))
	{
		ActiveToolMarkerActor->SetActorHiddenInGame(true);
	}
	if (ActiveQuestMarkerActor && IsValid(ActiveQuestMarkerActor))
	{
		ActiveQuestMarkerActor->SetActorHiddenInGame(true);
	}
	bHasActiveAIQuest = false;

	CachedFlightDirectorAdvice = TEXT("WARNING: RED ALERT! Station telemetry reports hull integrity breach and system failures! Emergency protocols active!");
	PrintScreenDebug(TEXT("[ALERT] Orientation complete! Live ship emergencies and autonomous AI threats ACTIVE!"), FColor::Red, 10.0f);

	if (bEnableEndlessAIGameMaster)
	{
		TriggerNewShipBreakdownAndAIQuest(TEXT("initial_post_orientation_breakdown"));
	}
	else if (bEnableAIPhysicsControl)
	{
		SendWorldDirectiveToAI(InitialAIPhysicsDirective);
	}
	else
	{
		SendGameEvent(InitialTestEventName);
	}
}

void UMetaAIComponent::UpdateOrientationTraining(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (CurrentTrainingPhase == EAstronautTrainingPhase::Step1_ThrusterMovement)
	{
		if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0))
		{
			const float Speed = PlayerChar->GetVelocity().Size();
			if (Speed > 25.0f)
			{
				TrainingAccumulatedMoveTime += DeltaTime;
			}

			const FVector CurrentLoc = PlayerChar->GetActorLocation();
			if (!TrainingLastPlayerLocation.IsZero())
			{
				TrainingAccumulatedDistanceTraversed += FVector::Dist(CurrentLoc, TrainingLastPlayerLocation);
			}
			TrainingLastPlayerLocation = CurrentLoc;

			if (TrainingAccumulatedMoveTime >= 2.2f || TrainingAccumulatedDistanceTraversed >= 220.0f)
			{
				AdvanceOrientationTraining(EAstronautTrainingPhase::Step2_WallGrabAndLaunch);
			}
		}
	}
	else if (CurrentTrainingPhase == EAstronautTrainingPhase::Step3_GrabEquipment ||
	         CurrentTrainingPhase == EAstronautTrainingPhase::Step4_CarryAndCalibrate)
	{
		UpdateActive3DQuestMarker(DeltaTime);
	}
}
