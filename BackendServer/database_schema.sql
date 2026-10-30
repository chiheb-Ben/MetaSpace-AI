-- ============================================================================
-- MetaSpace Zero-G Astronaut Training Simulator - MySQL Database Schema
-- Database Name: spacesumlater
-- ============================================================================

CREATE DATABASE IF NOT EXISTS `spacesumlater`
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE `spacesumlater`;

-- ----------------------------------------------------------------------------
-- 1. Table: training_sessions
-- Stores each training simulation run, how long it took (duration_seconds),
-- actions performed, performance score, and Meta AI's coaching advice & grade.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `training_sessions` (
    `id` INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    `session_uuid` VARCHAR(64) NOT NULL UNIQUE,
    `trainee_name` VARCHAR(190) NOT NULL DEFAULT 'Astronaut_Trainee',
    `mission_name` TEXT NOT NULL,
    `risk_level` VARCHAR(32) NOT NULL DEFAULT 'MODERATE',
    `risk_score` FLOAT NOT NULL DEFAULT 35.0,
    `flight_readiness` VARCHAR(64) NOT NULL DEFAULT 'IN_EVALUATION',
    `status` ENUM('in_progress', 'completed', 'failed', 'aborted') NOT NULL DEFAULT 'in_progress',
    `duration_seconds` FLOAT NOT NULL DEFAULT 0.0,
    `target_time_seconds` FLOAT NOT NULL DEFAULT 120.0,
    `items_grabbed_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `items_snapped_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `required_snaps_count` INT UNSIGNED NOT NULL DEFAULT 3,
    `wall_launches_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `final_score` FLOAT NOT NULL DEFAULT 0.0,
    `ai_grade` VARCHAR(16) DEFAULT 'PENDING',
    `ai_coaching_advice` TEXT DEFAULT NULL,
    `ai_evaluation_summary` TEXT DEFAULT NULL,
    `world_objects_json` LONGTEXT DEFAULT NULL,
    `started_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `finished_at` DATETIME DEFAULT NULL,
    INDEX `idx_trainee` (`trainee_name`),
    INDEX `idx_status` (`status`),
    INDEX `idx_started_at` (`started_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------------------------------------------------------
-- 2. Table: training_telemetry_events
-- Stores every step/action during the simulation (item grabbed, item dropped,
-- item snapped, wall launch, elapsed time at that exact moment, etc.).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `training_telemetry_events` (
    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    `session_uuid` VARCHAR(64) NOT NULL,
    `event_type` VARCHAR(64) NOT NULL,
    `actor_name` TEXT DEFAULT NULL,
    `target_name` TEXT DEFAULT NULL,
    `elapsed_time_seconds` FLOAT NOT NULL DEFAULT 0.0,
    `player_speed` FLOAT NOT NULL DEFAULT 0.0,
    `details_json` TEXT DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX `idx_session_uuid` (`session_uuid`),
    INDEX `idx_event_type` (`event_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------------------------------------------------------
-- 3. Table: ai_coaching_and_physics_logs
-- Stores every Meta AI coaching message, time analysis, and physics command
-- executed during the simulation.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `ai_coaching_and_physics_logs` (
    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    `session_uuid` VARCHAR(64) NOT NULL,
    `elapsed_time_seconds` FLOAT NOT NULL DEFAULT 0.0,
    `directive_sent` TEXT DEFAULT NULL,
    `ai_message` TEXT DEFAULT NULL,
    `physics_commands_json` LONGTEXT DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX `idx_ai_session` (`session_uuid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------------------------------------------------------
-- 4. Table: ai_object_edit_logs
-- Stores the exact BEFORE and AFTER size/scale, mass, spin, and max speed of
-- every object edited by Meta AI during the simulation.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `ai_object_edit_logs` (
    `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    `session_uuid` VARCHAR(64) NOT NULL,
    `elapsed_time_seconds` FLOAT NOT NULL DEFAULT 0.0,
    `object_name` VARCHAR(255) NOT NULL,
    `object_role` VARCHAR(64) DEFAULT 'SpaceProp',
    `size_before` TEXT NOT NULL,
    `size_after` TEXT NOT NULL,
    `mass_before_kg` FLOAT NOT NULL DEFAULT 0.0,
    `mass_after_kg` FLOAT NOT NULL DEFAULT 0.0,
    `spin_before_deg` FLOAT NOT NULL DEFAULT 0.0,
    `spin_after_deg` FLOAT NOT NULL DEFAULT 0.0,
    `max_speed_before` FLOAT NOT NULL DEFAULT 0.0,
    `max_speed_after` FLOAT NOT NULL DEFAULT 0.0,
    `changes_summary` TEXT DEFAULT NULL,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX `idx_edit_session` (`session_uuid`),
    INDEX `idx_edit_object` (`object_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------------------------------------------------------
-- 5. Table: quest_catalog
-- Stores all active and completed AI & Manual quests, preventing repetition
-- and tracking trainee mastery times.
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `quest_catalog` (
    `id` INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    `quest_key` VARCHAR(190) NOT NULL UNIQUE,
    `quest_source` VARCHAR(32) NOT NULL DEFAULT 'AI_GENERATED',
    `quest_title` VARCHAR(255) NOT NULL,
    `quest_instruction` TEXT NOT NULL,
    `target_actor` VARCHAR(190) NOT NULL,
    `required_item` VARCHAR(190) NOT NULL DEFAULT '',
    `times_assigned` INT UNSIGNED NOT NULL DEFAULT 1,
    `times_completed` INT UNSIGNED NOT NULL DEFAULT 0,
    `best_completion_time_sec` FLOAT NOT NULL DEFAULT 0.0,
    `avg_completion_time_sec` FLOAT NOT NULL DEFAULT 0.0,
    `last_trainee` VARCHAR(120) DEFAULT NULL,
    `created_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
