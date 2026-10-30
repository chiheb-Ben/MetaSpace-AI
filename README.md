# MetaSpace: Autonomous Cyber-Physical Zero-G Space Station & Astronaut Telemetry System

[![Meta Global AI Hackathon](https://img.shields.io/badge/Meta_Global_AI_Hackathon-Push_the_Frontier-0284C7?style=for-the-badge&logo=meta)](https://meta.ai)
[![Unreal Engine](https://img.shields.io/badge/Unreal_Engine-5.7_C%2B%2B-0F172A?style=for-the-badge&logo=unrealengine)](https://unrealengine.com)
[![Meta Model API](https://img.shields.io/badge/Meta_Model_API-Muse_Spark_1.3-10B981?style=for-the-badge&logo=openai)](https://developers.facebook.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-F59E0B?style=for-the-badge)](LICENSE)

> Built for the **Meta Global AI Developer Hackathon 2026**  
> **Problem Statement Track:** *Push the Frontier* (Cyber-Physical AI, Robotics, Energy & Physical System Control)

---

## 🚀 Executive Summary

Most generative AI applications are passive text chatbots or document analyzers. **MetaSpace** breaks this paradigm by bringing Meta's reasoning and multimodal models into a **real-time Cyber-Physical 3D Simulation** in **Unreal Engine 5.7 C++**.

In MetaSpace, Meta AI serves as an **Autonomous Flight Director and Physical World Governor** presiding over an orbital space station. It directly perceives live astronaut telemetry, calculates real-time emergency risk scores, commands zero-G physical forces (Newtonian impulses, rotational drag, debris repulsion), controls station emergency lighting, broadcasts radio flight instructions, and evaluates astronaut flight readiness on a live cloud dashboard.

---

## 🌌 Why MetaSpace "Pushes the Frontier"

The official Hackathon *Push the Frontier* problem statement tasks builders with:
> *"Use Meta's models to tackle challenges in software, robotics, manufacturing, energy, materials and beyond. Give a machine a new capability, accelerate a scientific experiment, or unlock a better way to understand and control a physical system. Show us what becomes possible when these models meet the real world."*

MetaSpace directly delivers on this challenge:
1. **Physical Actuation over Text Output:** Meta AI does not merely output text; its structured JSON decisions directly actuate physical laws in the Unreal Engine world—firing thruster impulses, adjusting angular momentum, managing atmospheric pressure, and altering station lighting presets (`critical_red`, `repaired_green`, `caution_orange`).
2. **0.00ms Dual-Pipeline Background Precompute Engine:** Zero-latency physical gameplay. While an astronaut is actively resolving an emergency, Meta AI computes the next physical crisis and failure mode in a background thread, ensuring instant zero-stutter transitions.
3. **Multi-System Anti-Repetition Engine:** Classifies all spaceship hardware into 5 distinct engineering domains (*Coolant Fluids, Electrical Bus, Avionics Computing, Reactor Core, Life Support*) to autonomously cycle challenges across the entire station.
4. **Physical Consequences for Engineering Errors:** If an astronaut fails three consecutive engineering dilemma questions on the holographic tablet, the station physically ruptures: spawning explosive particle cascades (`P_Explosion_Big_A`), triggering camera shakes, sounding cabin alarms, and venting suit oxygen.
5. **Automated Cloud Telemetry & Debriefing:** Real-time metrics (tether slingshot velocity, wall anchoring latency, tool selection accuracy) are streamed to a dedicated PHP/MySQL cloud dashboard where Meta AI generates autonomous astronaut debriefs and issues Flight Readiness Certifications (`QUALIFIED_FLIGHT_READY`, `CONDITIONAL_TRAINING`, `GROUNDED_REMEDIAL`).
6. Full-Map Spatial Scanning & Emergent Task Generation:
Meta AI continuously scans the entire 3D environment in real-time to detect the types and locations of randomly scattered physical tools (e.g., screwdrivers, wrenches, fire extinguishers). Instead of relying on pre-scripted events, the AI dynamically invents specific engineering crises tailored strictly to the exact equipment currently distributed across the map. This demonstrates true spatial reasoning and the model's ability to drive emergent cyber-physical gameplay based on environmental variables.
---

## 🏛️ System Architecture

```text
  ┌────────────────────────────────────────────────────────────────────────┐
  │                           ASTRONAUT IN 3D WORLD                        │
  │     (6-DOF Zero-G Thrusters, Wall Latching, Tool Grab, [E] Repair)     │
  └───────────────────────────────┬────────────────────────────────────────┘
                                  │ 60Hz Telemetry Stream
                                  ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │                    UMetaAIComponent (Unreal Engine 5.7 C++)            │
  │  • Live Telemetry Engine (Wall Launches, Velocity, Snaps, Oxygen)      │
  │  • Anti-Repetition System Classifier (5 Engineering Categories)        │
  │  • Dual-Pipeline Background Precomputer (0.00ms Zero-Latency)          │
  │  • Asynchronous Multi-threaded SAPI / Meta Voice Interrupter           │
  └──────────────────┬─────────────────────────────────┬───────────────────┘
                     │ REST API (JSON)                 │ Real-Time Sync
                     ▼                                 ▼
  ┌───────────────────────────────────┐    ┌───────────────────────────────┐
  │      META MODEL API (LLM)         │    │      CLOUD DASHBOARD & DB     │
  │  • Meta Muse Spark 1.3            │    │  • Live Telemetry Monitoring  │
  │  • Autonomous Risk Scoring (0-100)│    │  • Flight Readiness Debrief   │
  │  • Physical Actuation Commands    │    │  • Global Leaderboard         │
  └───────────────────────────────────┘    └───────────────────────────────┘
```

---

## 🎮 Astronaut Controls

| Action | Key / Input | Description |
| :--- | :--- | :--- |
| **Zero-G Thrust** | `W / A / S / D` | Forward, backward, and lateral strafe thrusters |
| **Vertical Lift / Dive** | `SPACE` / `C` (or `L-CTRL`) | Ascend / descend in microgravity |
| **Thruster Burst** | `L-SHIFT` | High-speed zero-G boost propulsion |
| **Pitch & Yaw** | `MOUSE MOVE` | Full 360-degree rotational orientation |
| **Wall Tether & Anchor** | `HOLD [LMB]` | Anchor suit onto bulkheads and walls |
| **Slingshot Launch** | `RELEASE [LMB]` | Slingshot launch using stored Newtonian momentum |
| **Tool Handling** | `[RMB]` | Physically grab or release tools (Wrench, Battery, Extinguisher, Valve Wheel) |
| **System Repair** | `[E]` | Execute repair (must hold required tool near 3D marker) |
| **Diagnostic Tablet** | `[T]` or `[E]` | Open holographic tablet to solve aerospace dilemmas |

---

## 🛠️ Quickstart & Setup Guide

### Prerequisites
* **Unreal Engine 5.7** installed
* **Visual Studio 2022 / 2026** with C++ Game Development workload
* A **Meta Model API Key** (claimable through the Meta Hackathon participant portal)

### Installation
1. **Clone the repository:**
   ```bash
   git clone https://github.com/YourUsername/MetaSpace.git
   cd MetaSpace
   ```
2. **Generate Project Files:**
   * Right-click `MetaSpace.uproject` &rarr; select **Generate Visual Studio project files**.
3. **Configure your Meta API Key:**
   * Open the project in Unreal Editor or Visual Studio.
   * In `Source/MetaSpace/MetaAIComponent.h` (or via the Editor Details panel on `BP_MetaSpaceCharacter` &rarr; `MetaAIComponent`), set:
     ```cpp
     ApiKey = TEXT("YOUR_META_MODEL_API_KEY_HERE");
     ```
4. **Build and Run:**
   * Open `MetaSpace.sln` in Visual Studio and build `Win64 Development`, or press **Play** in the Unreal Editor.

---

## 📁 Repository Structure

```text
MetaSpace/
├── Config/                     # Engine and game configuration settings
├── Content/                    # 3D assets, hallway environment, tools, and audio
├── Docs/                       # Official Hackathon Submission Dossier (PDF)
│   └── MetaSpace_Hackathon_Submission_Dossier.pdf
├── Source/MetaSpace/           # C++ Game & AI Implementation
│   ├── MetaAIComponent.h/.cpp          # Meta AI Autonomous Director & Physical Governor
│   ├── SpaceFloatingComponent.h/.cpp   # 6-DOF Zero-G Movement & Wall Slingshot
│   ├── SpaceGrabSnapComponent.h/.cpp   # Tool Grab, Socket Snapping & [E] Validation
│   ├── SpaceTabletQuizComponent.h/.cpp # Diagnostic Tablet & Catastrophic Failure Logic
│   └── SpaceAIIdentityComponent.h/.cpp # Station Entity Classification
├── BackendServer/              # Cloud Telemetry Web App (PHP / MySQL)
│   ├── api.php                         # Telemetry ingest & Meta AI coaching endpoint
│   ├── dashboard.php                   # Real-time monitoring dashboard & debrief UI
│   └── config.example.php              # Sample database configuration
├── MetaSpace.uproject          # Unreal Engine Project descriptor
└── README.md                   # This documentation
```

---

## 🏆 Hackathon Compliance & Originality

* **Eligibility:** Built exclusively during the official hackathon building period (**October 27 – November 4, 2026**).
* **Model Utilization:** Deep native integration with **Meta Model API (Muse Spark 1.3 / Open Weights)**.
* **Strict Anti-Project Avoidance:** Not a generic chatbot, not a basic RAG system, not a medical or sports advisor. A novel, original aerospace cyber-physical simulation system.
* **Open Source:** Publicly licensed under the MIT License.

---

