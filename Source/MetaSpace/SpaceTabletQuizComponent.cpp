// Copyright Epic Games, Inc. All Rights Reserved.

#include "SpaceTabletQuizComponent.h"
#include "MetaAIComponent.h"
#include "TimerManager.h"
#include "Blueprint/UserWidget.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Particles/ParticleSystem.h" 

USpaceTabletQuizComponent::USpaceTabletQuizComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.15f; // Check proximity ~6.6 times per second to save CPU!

	SystemPrompt = TEXT("You are the autonomous AI Flight Director and Space Systems Engineer of an active orbital space station. Your task is to test the astronaut trainee with deeply authentic, scientifically accurate spaceflight dilemmas involving zero-gravity physics, orbital mechanics, ECLSS (Environmental Control and Life Support), hypergolic propellants, and vacuum engineering.\n\nScenarios must reflect authentic space missions (e.g. Apollo 13 CO2 scrubbing, microgravity coolant vapor lock, Clohessy-Wiltshire orbital burns, spacesuit depressurization, reaction control thruster torque, cryogenic boil-off).\n\nYou must output ONLY a valid JSON object in the exact format below, with no markdown or extra commentary:\n{\n  \"question\": \"[2-sentence emergency scenario or technical dilemma rooted in real space physics]\",\n  \"options\": [\n    \"[Option 1: Plausible amateur intuition that violates orbital mechanics or zero-g physics]\",\n    \"[Option 2: The scientifically correct NASA/ESA engineering or physics protocol]\",\n    \"[Option 3: Another dangerous or incorrect space action]\"\n  ],\n  \"correct_index\": 1,\n  \"success_message\": \"[Scientific explanation citing the physics law or space protocol (e.g. Newton's 3rd Law, Orbital Phase)]\",\n  \"failure_message\": \"[Critical warning explaining the physical disaster caused by the error]\",\n  \"punishment_event\": \"[One of: 'Hazard_Explosion', 'Spawn_Debris', 'Ship_Shake', 'Oxygen_Leak']\"\n}\nNote: Randomize the 'correct_index' (0, 1, or 2) evenly across questions.");
}

void USpaceTabletQuizComponent::BeginPlay()
{
	Super::BeginPlay();

	// 0. Auto-load default Widget class if unset
	EnsureDefaultWidgetClass();

	if (!CatastrophicExplosionParticle)
	{
		CatastrophicExplosionParticle = LoadObject<UParticleSystem>(
			nullptr,
			TEXT("/Game/Realistic_Starter_VFX_Pack_Vol2/Particles/Explosion/P_Explosion_Big_A.P_Explosion_Big_A")
		);
	}

	// 1. Immediately initialize with a high-stakes realistic Zero-G technical physics dilemma
	// so that options and question are ALWAYS populated from frame 0 (never blank)!
	LoadRandomFallbackDilemma();

	// Note: Frame 0 HTTP calls are avoided to prevent concurrency and rate limit collisions with MetaAIComponent.
	// Fresh AI dilemmas are fetched when the tablet UI is opened or when final certification begins.
}

void USpaceTabletQuizComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ActiveHttpRequest.IsValid())
	{
		ActiveHttpRequest->OnProcessRequestComplete().Unbind();
		ActiveHttpRequest->CancelRequest();
		ActiveHttpRequest.Reset();
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(NextQuestionTimerHandle);
		World->GetTimerManager().ClearTimer(AutoCloseTimerHandle);
	}

	if (ActiveWidgetInstance)
	{
		ActiveWidgetInstance->RemoveFromParent();
		ActiveWidgetInstance = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void USpaceTabletQuizComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor)
	{
		return;
	}

	ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0);
	if (!PlayerChar)
	{
		return;
	}

	const float DistanceToPlayer = FVector::Dist(OwnerActor->GetActorLocation(), PlayerChar->GetActorLocation());

	// 1. Proximity Open
	if (!bIsUIVisible && bAutoOpenOnProximity && !bRequireInteractKeyToOpen)
	{
		if (DistanceToPlayer <= ProximityOpenDistance)
		{
			bOpenedViaProximity = true;
			OpenTabletUI();
		}
	}
	// 2. Proximity Close when drifting away (Only if opened via proximity, NOT if opened via T key or remote toggle!)
	else if (bIsUIVisible)
	{
		if (DistanceToPlayer > ProximityCloseDistance && bOpenedViaProximity)
		{
			CloseTabletUI();
		}
	}
}

void USpaceTabletQuizComponent::EnsureDefaultWidgetClass()
{
	if (!TabletWidgetClass)
	{
		TabletWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI_Contact/Ai_Talk.Ai_Talk_C"));
		if (!TabletWidgetClass)
		{
			TabletWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI_Contact/Operaitor_ui.Operaitor_ui_C"));
		}
	}
}

void USpaceTabletQuizComponent::OpenTabletUI()
{
	if (bIsUIVisible)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	EnsureDefaultWidgetClass();

	bIsUIVisible = true;

	// Spawn or unhide UMG widget if class is configured
	if (TabletWidgetClass)
	{
		if (!ActiveWidgetInstance || !IsValid(ActiveWidgetInstance) || !ActiveWidgetInstance->IsInViewport())
		{
			ActiveWidgetInstance = CreateWidget<UUserWidget>(World, TabletWidgetClass);
			if (ActiveWidgetInstance)
			{
				ActiveWidgetInstance->AddToViewport(50);
			}
		}
		else
		{
			ActiveWidgetInstance->SetVisibility(ESlateVisibility::Visible);
		}
	}

	// Switch input mode, freeze player movement/look controls, 6-DOF thrusters, wall grab, and enable mouse cursor in C++
	if (bManageInputModeAndCursor)
	{
		UMetaAIComponent::LockPlayerInputForUI(this, true, true);

		if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
		{
			FInputModeGameAndUI InputMode;
			InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			InputMode.SetHideCursorDuringCapture(false);
			if (ActiveWidgetInstance)
			{
				InputMode.SetWidgetToFocus(ActiveWidgetInstance->TakeWidget());
			}
			PC->SetInputMode(InputMode);
		}
	}

	OnTabletVisibilityChanged.Broadcast(true);

	// Broadcast the current dilemma so newly created or opened widgets immediately receive question & options!
	if (CurrentDilemma.Options.Num() >= 3)
	{
		OnQuestionReady.Broadcast(CurrentDilemma.Question, CurrentDilemma.Options);
	}

	// If no active dilemma or already answered, fetch a fresh one
	if (CurrentDilemma.Options.Num() < 3 || bHasAnsweredCurrentQuestion)
	{
		RequestNewQuestionFromAI();
	}
}

void USpaceTabletQuizComponent::CloseTabletUI()
{
	UWorld* World = GetWorld();
	if (World)
	{
		World->GetTimerManager().ClearTimer(AutoCloseTimerHandle);
	}

	if (!bIsUIVisible)
	{
		// Fallback: Ensure player controls are completely unlocked even if UI was closed externally
		if (bManageInputModeAndCursor)
		{
			UMetaAIComponent::LockPlayerInputForUI(this, false, false);
		}
		return;
	}

	bIsUIVisible = false;
	bOpenedViaProximity = false;

	if (ActiveWidgetInstance && IsValid(ActiveWidgetInstance))
	{
		ActiveWidgetInstance->SetVisibility(ESlateVisibility::Collapsed);
	}

	// Fully restore pure Game-Only input mode, 6-DOF thrusters, wall grab, and hide mouse cursor in C++
	if (bManageInputModeAndCursor)
	{
		UMetaAIComponent::LockPlayerInputForUI(this, false, false);
	}

	OnTabletVisibilityChanged.Broadcast(false);
}

void USpaceTabletQuizComponent::ToggleTabletUI()
{
	if (bIsUIVisible)
	{
		CloseTabletUI();
	}
	else
	{
		bOpenedViaProximity = false; // Opened manually via key or toggle: bypass proximity close!
		OpenTabletUI();
	}
}

void USpaceTabletQuizComponent::CloseTabletAndRestoreControls()
{
	CloseTabletUI();
	UMetaAIComponent::LockPlayerInputForUI(this, false, false);
}

void USpaceTabletQuizComponent::ForceRestorePlayerControls(const UObject* WorldContextObject)
{
	UMetaAIComponent::LockPlayerInputForUI(WorldContextObject, false, false);
}

void USpaceTabletQuizComponent::RequestNewQuestionFromAI()
{
	bHasAnsweredCurrentQuestion = false;
	LastFeedbackMessage = TEXT("");
	LastSubmittedIndex = -1;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(NextQuestionTimerHandle);
	}

	if (bFetchDilemmasFromMetaAI && !ApiKey.TrimStartAndEnd().IsEmpty())
	{
		SendChatCompletionRequest();
	}
	else
	{
		LoadRandomFallbackDilemma();
	}
}

void USpaceTabletQuizComponent::SendChatCompletionRequest()
{
	if (bIsWaitingForAIResponse)
	{
		return;
	}

	bIsWaitingForAIResponse = true;

	// Build JSON request body for Meta AI chat completion
	TSharedPtr<FJsonObject> RootObj = MakeShareable(new FJsonObject());
	RootObj->SetStringField(TEXT("model"), ModelName);
	RootObj->SetNumberField(TEXT("temperature"), 0.7);
	RootObj->SetNumberField(TEXT("max_tokens"), 3000);

	TArray<TSharedPtr<FJsonValue>> MessagesArray;

	// System message
	TSharedPtr<FJsonObject> SystemMsgObj = MakeShareable(new FJsonObject());
	SystemMsgObj->SetStringField(TEXT("role"), TEXT("system"));
	SystemMsgObj->SetStringField(TEXT("content"), SystemPrompt);
	MessagesArray.Add(MakeShareable(new FJsonValueObject(SystemMsgObj)));

	// User message
	TSharedPtr<FJsonObject> UserMsgObj = MakeShareable(new FJsonObject());
	UserMsgObj->SetStringField(TEXT("role"), TEXT("user"));
	UserMsgObj->SetStringField(TEXT("content"), TEXT("Generate the next high-stakes situational zero-G dilemma."));
	MessagesArray.Add(MakeShareable(new FJsonValueObject(UserMsgObj)));

	RootObj->SetArrayField(TEXT("messages"), MessagesArray);

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

	ActiveHttpRequest = FHttpModule::Get().CreateRequest();
	ActiveHttpRequest->SetURL(ApiEndpointUrl);
	ActiveHttpRequest->SetVerb(TEXT("POST"));
	ActiveHttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	ActiveHttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey.TrimStartAndEnd()));
	ActiveHttpRequest->SetContentAsString(RequestBody);
	ActiveHttpRequest->OnProcessRequestComplete().BindUObject(this, &USpaceTabletQuizComponent::OnChatCompletionResponseReceived);
	ActiveHttpRequest->ProcessRequest();
}

void USpaceTabletQuizComponent::OnChatCompletionResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	bIsWaitingForAIResponse = false;

	if (!bConnectedSuccessfully || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
	{
		// Fallback to offline engineering technical dilemmas so options are NEVER broken or blank!
		if (CurrentDilemma.Options.Num() < 3)
		{
			LoadRandomFallbackDilemma();
		}
		return;
	}

	const FString ResponseString = Response->GetContentAsString();
	TSharedPtr<FJsonObject> JsonResponse;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);

	if (!FJsonSerializer::Deserialize(Reader, JsonResponse) || !JsonResponse.IsValid())
	{
		if (CurrentDilemma.Options.Num() < 3)
		{
			LoadRandomFallbackDilemma();
		}
		return;
	}

	// Extract message content from choices[0].message.content or reasoning_content or text
	const TArray<TSharedPtr<FJsonValue>>* ChoicesArray;
	if (JsonResponse->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray->Num() > 0)
	{
		const TSharedPtr<FJsonObject> FirstChoice = (*ChoicesArray)[0]->AsObject();
		if (FirstChoice.IsValid())
		{
			FString AIContent;
			const TSharedPtr<FJsonObject> MsgObj = FirstChoice->GetObjectField(TEXT("message"));
			if (MsgObj.IsValid())
			{
				MsgObj->TryGetStringField(TEXT("content"), AIContent);
				if (AIContent.TrimStartAndEnd().IsEmpty())
				{
					MsgObj->TryGetStringField(TEXT("reasoning_content"), AIContent);
				}
			}
			if (AIContent.TrimStartAndEnd().IsEmpty())
			{
				FirstChoice->TryGetStringField(TEXT("text"), AIContent);
			}

			FSpaceQuizDilemma ParsedDilemma;
			if (ParseDilemmaFromJson(AIContent, ParsedDilemma))
			{
				CurrentDilemma = ParsedDilemma;
				OnQuestionReady.Broadcast(CurrentDilemma.Question, CurrentDilemma.Options);
				return;
			}
		}
	}

	// If AI output had unexpected structure, retain or refresh fallback physics dilemma
	if (CurrentDilemma.Options.Num() < 3)
	{
		LoadRandomFallbackDilemma();
	}
}

bool USpaceTabletQuizComponent::ParseDilemmaFromJson(const FString& JsonString, FSpaceQuizDilemma& OutDilemma)
{
	FString CleanJson = JsonString.TrimStartAndEnd();

	// Strip markdown ```json code fences if present
	if (CleanJson.StartsWith(TEXT("```json")))
	{
		CleanJson.RemoveFromStart(TEXT("```json"));
	}
	else if (CleanJson.StartsWith(TEXT("```")))
	{
		CleanJson.RemoveFromStart(TEXT("```"));
	}
	if (CleanJson.EndsWith(TEXT("```")))
	{
		CleanJson.RemoveFromEnd(TEXT("```"));
	}
	CleanJson = CleanJson.TrimStartAndEnd();

	// Locate JSON object bounds '{' and '}'
	const int32 FirstBrace = CleanJson.Find(TEXT("{"));
	const int32 LastBrace = CleanJson.Find(TEXT("}"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (FirstBrace != INDEX_NONE && LastBrace != INDEX_NONE && LastBrace > FirstBrace)
	{
		CleanJson = CleanJson.Mid(FirstBrace, LastBrace - FirstBrace + 1);
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CleanJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return false;
	}

	OutDilemma.Question = JsonObj->GetStringField(TEXT("question"));

	const TArray<TSharedPtr<FJsonValue>>* OptionsArray;
	if (JsonObj->TryGetArrayField(TEXT("options"), OptionsArray) && OptionsArray->Num() >= 3)
	{
		OutDilemma.Options.Empty();
		for (int32 i = 0; i < 3; ++i)
		{
			OutDilemma.Options.Add((*OptionsArray)[i]->AsString());
		}
	}
	else
	{
		return false;
	}

	OutDilemma.CorrectIndex = FMath::Clamp(JsonObj->GetIntegerField(TEXT("correct_index")), 0, 2);
	OutDilemma.SuccessMessage = JsonObj->GetStringField(TEXT("success_message"));
	OutDilemma.FailureMessage = JsonObj->GetStringField(TEXT("failure_message"));
	OutDilemma.PunishmentEvent = JsonObj->GetStringField(TEXT("punishment_event"));

	return !OutDilemma.Question.IsEmpty() && OutDilemma.Options.Num() == 3;
}

void USpaceTabletQuizComponent::LoadRandomFallbackDilemma()
{
	TArray<FSpaceQuizDilemma> FallbackPool;

	// Dilemma 1: Cryogenic Jet Recoil
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("A ruptured cryogenic cooling manifold is venting a 180N recoil jet into the transit corridor. You are floating directly in its approach vector without wall contact.");
		D.Options.Add(TEXT("Throw your heavy 12kg magnetic toolbag in the direction of the jet to impart an equal-and-opposite momentum pushing yourself safely clear."));
		D.Options.Add(TEXT("Attempt to paddle through the cabin atmosphere with your hands to swim toward the nearest bulkhead."));
		D.Options.Add(TEXT("Hurl your wrench away from the jet to accelerate yourself forward past it."));
		D.CorrectIndex = 0;
		D.SuccessMessage = TEXT("Newton's 3rd Law verified: Conservation of momentum from throwing the heavy toolbag safely propelled your body away from the hazard!");
		D.FailureMessage = TEXT("Catastrophic error: Viscous drag is negligible in cabin air; throwing away from the jet hurled you directly into the cryogenic plume!");
		D.PunishmentEvent = TEXT("Hazard_Explosion");
		FallbackPool.Add(D);
	}

	// Dilemma 2: High-RPM Spinning Turbine Lock
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("A 50kg atmospheric scrubber fan is spinning uncontrollably at 400 RPM following a power surge. You must lock its rotor to prevent motor burnout.");
		D.Options.Add(TEXT("Grab the outer rim directly with both hands while free-floating without anchoring to a station bulkhead."));
		D.Options.Add(TEXT("Anchor your magnetic boots firmly to the station frame first before engaging the rim friction brake."));
		D.Options.Add(TEXT("Pry the rotor hub perpendicular to the rotation axis with a steel prybar to force gyroscopic precession."));
		D.CorrectIndex = 1;
		D.SuccessMessage = TEXT("Torque reaction absorbed: Anchoring to station frame prevented angular momentum transfer to your body!");
		D.FailureMessage = TEXT("Severe injury: Without frame anchoring, rotor angular momentum spun your entire body violently in reverse!");
		D.PunishmentEvent = TEXT("Ship_Shake");
		FallbackPool.Add(D);
	}

	// Dilemma 3: High-Voltage Electrical Panel Arc
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("An electrical distribution bus in the life support corridor has developed a 600V sustained arc, emitting dense conductive smoke in cabin atmosphere.");
		D.Options.Add(TEXT("Discharge the liquid foam canister directly into the open electrical cabinet without isolating power."));
		D.Options.Add(TEXT("Isolate the main sub-station circuit breaker first, then deploy the non-conductive Halon/CO2 extinguisher."));
		D.Options.Add(TEXT("Vent the module atmosphere to vacuum while remaining inside without suit integrity seal check."));
		D.CorrectIndex = 1;
		D.SuccessMessage = TEXT("Standard electrical protocol: Power isolation prevented reignition and secondary shock hazard!");
		D.FailureMessage = TEXT("Electrocution explosion: Liquid conductive extinguishing agent intensified the arc blast!");
		D.PunishmentEvent = TEXT("Hazard_Explosion");
		FallbackPool.Add(D);
	}

	// Dilemma 4: Micro-Meteoroid Hull Breach
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("A 2cm micro-meteoroid hull puncture has caused module decompression at 500 L/min. Cabin atmospheric pressure is dropping rapidly.");
		D.Options.Add(TEXT("Apply a magnetic silicone seal patch against the interior hull surface over the leak."));
		D.Options.Add(TEXT("Attempt to weld the high-stress puncture from outside while cabin air exhausts outward."));
		D.Options.Add(TEXT("Cover the puncture loosely with your suit glove to seal it by manual hand grip alone."));
		D.CorrectIndex = 0;
		D.SuccessMessage = TEXT("Pressure differential utilized: Interior cabin air pressure locks the seal patch firmly against the hull frame!");
		D.FailureMessage = TEXT("Decompression blowout: Outward exhaust velocity tore the patch away and accelerated cabin pressure drop!");
		D.PunishmentEvent = TEXT("Oxygen_Leak");
		FallbackPool.Add(D);
	}

	// Dilemma 5: Unsecured High-Pressure Cylinder
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("An unstrapped 85kg oxygen cylinder is floating down the corridor at 3 m/s on a direct collision course with the delicate avionics terminal.");
		D.Options.Add(TEXT("Position your body directly in front of the terminal to absorb the impact."));
		D.Options.Add(TEXT("Deploy an energy-absorbing tether line with a dampener to decelerate the cylinder gradually along the bulkhead."));
		D.Options.Add(TEXT("Strike the cylinder valve head with a prybar to deflect its trajectory."));
		D.CorrectIndex = 1;
		D.SuccessMessage = TEXT("Kinetic energy dissipated: Gradual tether deceleration arrested the cylinder without structural damage!");
		D.FailureMessage = TEXT("Catastrophic valve fracture: Impact broke the brass valve stem, turning the 300-bar tank into an unguided rocket!");
		D.PunishmentEvent = TEXT("Spawn_Debris");
		FallbackPool.Add(D);
	}

	// Dilemma 6: Apollo 13 CO2 Scrubber Adapter (ECLSS Chemistry)
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("Cabin pCO2 is rising above 7.5 mmHg due to saturated lithium hydroxide canisters. Spare canisters in storage are square, but the primary scrubber socket is round.");
		D.Options.Add(TEXT("Construct an airtight adapter using plastic flight cue cards, spacesuit hoses, and gray duct tape to force cabin air through the square canisters."));
		D.Options.Add(TEXT("Flush the entire air circulation loop with liquid coolant to dissolve the carbon dioxide."));
		D.Options.Add(TEXT("Turn off all cabin circulation fans so heavy CO2 settles down to the module floor."));
		D.CorrectIndex = 0;
		D.SuccessMessage = TEXT("Authentic NASA Apollo 13 protocol verified: The improvised cardboard/hose adapter successfully routed airflow through the LiOH beds!");
		D.FailureMessage = TEXT("Fatal hypercapnia: Zero-G prevents atmospheric gas settling; CO2 pooled around crew sleeping berths!");
		D.PunishmentEvent = TEXT("Oxygen_Leak");
		FallbackPool.Add(D);
	}

	// Dilemma 7: Microgravity Boiling Vapor Lock (Zero-G Thermodynamics)
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("In zero-gravity, heat pipes lack natural buoyant convection. Boiling vapor bubbles are clinging to hot internal pipe walls, causing severe thermal runaway in the avionics cooler.");
		D.Options.Add(TEXT("Hit the pipe manifold with a heavy maintenance wrench to shake bubbles loose."));
		D.Options.Add(TEXT("Engage the centrifugal vortex separator pump to generate artificial radial acceleration and strip vapor bubbles from the pipe walls."));
		D.Options.Add(TEXT("Open the emergency radiator line directly to vacuum while running at maximum power."));
		D.CorrectIndex = 1;
		D.SuccessMessage = TEXT("Centrifugal phase separation verified: Artificial radial acceleration restored liquid contact with the boiler wall!");
		D.FailureMessage = TEXT("Thermal burnout: Radiator freeze occurred in vacuum and pipe impact fractured the titanium cooling line!");
		D.PunishmentEvent = TEXT("Hazard_Explosion");
		FallbackPool.Add(D);
	}

	// Dilemma 8: Orbital Rendezvous Relative Motion (Clohessy-Wiltshire Physics)
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("An uncrewed cargo pod is floating 100 meters directly ahead in the orbital velocity vector. Station telemetry indicates you are slowly falling behind. What burn should you execute?");
		D.Options.Add(TEXT("Fire forward prograde thrusters to burn directly toward the cargo ship to speed up."));
		D.Options.Add(TEXT("Fire retrograde thrusters to slightly lower orbital altitude, thereby increasing angular orbital velocity to catch up from below."));
		D.Options.Add(TEXT("Fire yaw thrusters laterally to cancel Earth's gravitational pull."));
		D.CorrectIndex = 1;
		D.SuccessMessage = TEXT("Orbital mechanics verified (Kepler's 3rd Law): Lowering the orbital radius shortened your orbital period, allowing you to catch up cleanly from below!");
		D.FailureMessage = TEXT("Orbital mechanics violation: Prograde burn raised your orbital apogee, increasing your period and causing you to drift further behind into a higher orbit!");
		D.PunishmentEvent = TEXT("Ship_Shake");
		FallbackPool.Add(D);
	}

	// Dilemma 9: Solar Particle Event Radiation Shelter
	{
		FSpaceQuizDilemma D;
		D.Question = TEXT("Solar sensors detect an incoming X-class proton storm reaching the station in 12 minutes. The module outer skin is 3mm aluminum.");
		D.Options.Add(TEXT("Retreat to the central station storm shelter surrounded by hydrogen-rich water storage tanks and dense polyethylene shielding."));
		D.Options.Add(TEXT("Stand behind a single thin lead sheet at the cupola window to block high-energy protons."));
		D.Options.Add(TEXT("Increase module cabin atmospheric pressure to 300 kPa to absorb incoming cosmic rays."));
		D.CorrectIndex = 0;
		D.SuccessMessage = TEXT("Radiation physics verified: Light nuclei (Hydrogen) absorb energetic cosmic protons efficiently without producing lethal secondary Bremsstrahlung scattering!");
		D.FailureMessage = TEXT("Secondary radiation hazard: Heavy lead atoms interacted with relativistic protons, generating intense secondary Bremsstrahlung gamma radiation!");
		D.PunishmentEvent = TEXT("Ship_Shake");
		FallbackPool.Add(D);
	}

	const int32 RandIndex = FMath::RandRange(0, FallbackPool.Num() - 1);
	CurrentDilemma = FallbackPool[RandIndex];
	OnQuestionReady.Broadcast(CurrentDilemma.Question, CurrentDilemma.Options);
}

bool USpaceTabletQuizComponent::SubmitOptionChoice(int32 OptionIndex)
{
	if (bHasAnsweredCurrentQuestion || CurrentDilemma.Options.Num() < 3)
	{
		return bLastAnswerWasCorrect;
	}

	bHasAnsweredCurrentQuestion = true;
	LastSubmittedIndex = OptionIndex;
	QuestionsAnsweredCount++;

	const bool bIsCorrect = (OptionIndex == CurrentDilemma.CorrectIndex);
	bLastAnswerWasCorrect = bIsCorrect;

	if (bIsCorrect)
	{
		ConsecutiveFailureCount = 0;
		QuestionsCorrectCount++;
		LastFeedbackMessage = CurrentDilemma.SuccessMessage;
		SyncMasteryToMetaAI(true);
	}
	else
	{
		ConsecutiveFailureCount++;
		LastFeedbackMessage = CurrentDilemma.FailureMessage;
		SyncMasteryToMetaAI(false);

		if (bExecutePunishmentConsequences)
		{
			ExecutePunishment(CurrentDilemma.PunishmentEvent);

			// If 3 consecutive wrong choices were submitted, trigger catastrophic station explosion!
			if (ConsecutiveFailureCount >= FailureThresholdForExplosion)
			{
				TriggerCatastrophicExplosionPunishment();
			}
		}
	}

	OnAnswerEvaluated.Broadcast(
		bIsCorrect,
		OptionIndex,
		CurrentDilemma.CorrectIndex,
		LastFeedbackMessage,
		CurrentDilemma.PunishmentEvent
	);

	// Auto-close tablet and restore player controls if configured
	if (bAutoCloseOnAnswerEvaluated)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				AutoCloseTimerHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					CloseTabletUI();
				}),
				FMath::Max(0.5f, AutoCloseDelayAfterAnswerSeconds),
				false
			);
		}
	}

	// Schedule next question
	if (AutoNextQuestionDelaySeconds > 0.0f)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				NextQuestionTimerHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					RequestNewQuestionFromAI();
				}),
				AutoNextQuestionDelaySeconds,
				false
			);
		}
	}

	return bIsCorrect;
}

FText USpaceTabletQuizComponent::GetCurrentQuestionText() const
{
	if (bIsWaitingForAIResponse)
	{
		return FText::FromString(TEXT("COMM LINK ACTIVE: Receiving tactical telemetry and scenario from station AI..."));
	}

	if (bHasAnsweredCurrentQuestion)
	{
		const FString Prefix = bLastAnswerWasCorrect
			? TEXT("[STATUS: OPTIMAL APPLICATION]\n")
			: TEXT("[STATUS: CATASTROPHIC ACTION]\n");
		return FText::FromString(Prefix + LastFeedbackMessage);
	}

	return FText::FromString(CurrentDilemma.Question);
}

FText USpaceTabletQuizComponent::GetOptionText(int32 OptionIndex) const
{
	if (CurrentDilemma.Options.IsValidIndex(OptionIndex))
	{
		return FText::FromString(CurrentDilemma.Options[OptionIndex]);
	}
	return FText::GetEmpty();
}

float USpaceTabletQuizComponent::GetSuccessRatePercent() const
{
	if (QuestionsAnsweredCount == 0)
	{
		return 100.0f;
	}
	return ((float)QuestionsCorrectCount / (float)QuestionsAnsweredCount) * 100.0f;
}

void USpaceTabletQuizComponent::TriggerCatastrophicExplosionPunishment()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!CatastrophicExplosionParticle)
	{
		CatastrophicExplosionParticle = LoadObject<UParticleSystem>(
			nullptr,
			TEXT("/Game/Realistic_Starter_VFX_Pack_Vol2/Particles/Explosion/P_Explosion_Big_A.P_Explosion_Big_A")
		);
	}

	AActor* OwnerActor = GetOwner();
	FVector SpawnLoc = OwnerActor ? OwnerActor->GetActorLocation() : FVector::ZeroVector;
	if (OwnerActor)
	{
		SpawnLoc += OwnerActor->GetActorForwardVector() * 60.0f + FVector(0.0f, 0.0f, 30.0f);
	}

	if (CatastrophicExplosionParticle)
	{
		UGameplayStatics::SpawnEmitterAtLocation(
			World,
			CatastrophicExplosionParticle,
			SpawnLoc,
			FRotator::ZeroRotator,
			FVector(1.6f),
			true
		);
	}

	if (UMetaAIComponent* MetaAI = UMetaAIComponent::GetMetaAIComponentGlobal(this))
	{
		MetaAI->TriggerShipTurbulenceAndCameraShake(2.2f, 5.0f, true, 3);
		MetaAI->RefillRoomOxygen(-30.0f);
		MetaAI->TransmitFlightDirectorRadioComms(
			TEXT("Flight Director: CRITICAL ALERT! 3 consecutive engineering failures triggered a catastrophic station breach explosion! Evacuate hazard zone!"),
			8.0f
		);
		MetaAI->ApplyShipLightingPreset(TEXT("critical_red"), TEXT("ALL_LIGHTS"));
	}

	ConsecutiveFailureCount = 0;
}

void USpaceTabletQuizComponent::ExecutePunishment(const FString& PunishmentEvent)
{
	UMetaAIComponent* MetaAI = UMetaAIComponent::GetMetaAIComponentGlobal(this);
	if (!MetaAI)
	{
		return;
	}

	if (PunishmentEvent.Contains(TEXT("Explosion")) || PunishmentEvent.Contains(TEXT("Hazard_Explosion")))
	{
		MetaAI->TriggerShipTurbulenceAndCameraShake(1.4f, 3.8f, true, 2);
		MetaAI->RefillRoomOxygen(-15.0f);
	}
	else if (PunishmentEvent.Contains(TEXT("Debris")) || PunishmentEvent.Contains(TEXT("Spawn_Debris")))
	{
		MetaAI->SpawnSpaceObstacleForPlayer(TEXT("SpaceDebris"), true, 340.0f);
		MetaAI->TriggerShipTurbulenceAndCameraShake(0.7f, 2.0f, true, 1);
	}
	else if (PunishmentEvent.Contains(TEXT("Oxygen")) || PunishmentEvent.Contains(TEXT("Oxygen_Leak")))
	{
		MetaAI->RefillRoomOxygen(-25.0f);
	}
	else if (PunishmentEvent.Contains(TEXT("Shake")) || PunishmentEvent.Contains(TEXT("Ship_Shake")))
	{
		MetaAI->TriggerShipTurbulenceAndCameraShake(1.1f, 4.0f, true, 1);
	}
}

void USpaceTabletQuizComponent::SyncMasteryToMetaAI(bool bCorrect)
{
	if (UMetaAIComponent* MetaAI = UMetaAIComponent::GetMetaAIComponentGlobal(this))
	{
		const FString ChosenText = GetOptionText(LastSubmittedIndex).ToString();
		if (bCorrect)
		{
			MetaAI->ProblemSolvingProficiencyScore = FMath::Clamp(MetaAI->ProblemSolvingProficiencyScore + 8.0f, 0.0f, 100.0f);
			MetaAI->LogTrainingEventToDatabase(
				TEXT("tablet_quiz_success"),
				TEXT("DiagnosticTablet"),
				FString::Printf(TEXT("SUCCESS: Option %d '%s' | %s"), LastSubmittedIndex + 1, *ChosenText.Left(80), *CurrentDilemma.SuccessMessage.Left(200))
			);
		}
		else
		{
			MetaAI->ProblemSolvingProficiencyScore = FMath::Clamp(MetaAI->ProblemSolvingProficiencyScore - 10.0f, 0.0f, 100.0f);
			MetaAI->LogTrainingEventToDatabase(
				TEXT("tablet_quiz_failure"),
				TEXT("DiagnosticTablet"),
				FString::Printf(TEXT("FAIL: Option %d '%s' -> Event: %s | %s"), LastSubmittedIndex + 1, *ChosenText.Left(80), *CurrentDilemma.PunishmentEvent, *CurrentDilemma.FailureMessage.Left(200))
			);
		}
	}
}

USpaceTabletQuizComponent* USpaceTabletQuizComponent::GetSpaceTabletQuizComponent(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	UWorld* World = WorldContextObject->GetWorld();
	if (!World && GEngine)
	{
		World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	}
	if (!World)
	{
		return nullptr;
	}

	USpaceTabletQuizComponent* FoundFallbackComp = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor)
		{
			if (USpaceTabletQuizComponent* Comp = Actor->FindComponentByClass<USpaceTabletQuizComponent>())
			{
				const FString ActName = Actor->GetActorNameOrLabel().ToLower();
				if (ActName.Contains(TEXT("tablet")) || ActName.Contains(TEXT("scanner")) || ActName.Contains(TEXT("quiz")))
				{
					return Comp;
				}
				if (!FoundFallbackComp)
				{
					FoundFallbackComp = Comp;
				}
			}
		}
	}

	if (FoundFallbackComp)
	{
		return FoundFallbackComp;
	}

	// Dynamic Fallback 1: Attach to any Tablet actor in the level
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		const FString ActName = Actor->GetActorNameOrLabel().ToLower();
		if (ActName.Contains(TEXT("tablet")) || ActName.Contains(TEXT("scanner")) || ActName.Contains(TEXT("quiz")))
		{
			USpaceTabletQuizComponent* NewComp = NewObject<USpaceTabletQuizComponent>(Actor, TEXT("AutoAttachedTabletQuizComp"));
			if (NewComp)
			{
				NewComp->TabletWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI_Contact/Ai_Talk.Ai_Talk_C"));
				NewComp->RegisterComponent();
				return NewComp;
			}
		}
	}

	// Dynamic Fallback 2: If no Actor in the level has the component yet, auto-attach one to Player Character!
	if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(World, 0))
	{
		USpaceTabletQuizComponent* AutoComp = NewObject<USpaceTabletQuizComponent>(PlayerChar, TEXT("AutoSpaceTabletQuizComp"));
		if (AutoComp)
		{
			AutoComp->TabletWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI_Contact/Ai_Talk.Ai_Talk_C"));
			AutoComp->RegisterComponent();
			return AutoComp;
		}
	}

	return nullptr;
}
