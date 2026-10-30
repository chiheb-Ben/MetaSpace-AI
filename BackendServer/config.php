<?php
/**
 * MetaSpace Zero-G Training Simulator - Database & Meta AI Configuration
 */

// Exact database credentials provided:
define('DB_HOST', 'localhost');
define('DB_USER', 'your_db_username');
define('DB_PASS', 'your_db_password');
define('DB_NAME', 'metaspace_db');

// Meta AI Server-Side Configuration (Optional server-side AI evaluation fallback)
define('META_API_URL', 'https://api.meta.ai/v1/chat/completions');
define('META_API_KEY', 'YOUR_META_MODEL_API_KEY_HERE');
define('META_MODEL',   'muse-spark-1.3');

/**
 * Establishes a PDO connection to MySQL and automatically initializes the required tables if they do not exist.
 */
function getDatabaseConnection(): PDO
{
    static $pdo = null;
    if ($pdo !== null) {
        return $pdo;
    }

    $options = [
        PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_EMULATE_PREPARES   => true,
    ];

    // Try trimmed DB name first ('metaspace_db'), then exact raw DB_NAME ('metaspace_db') if needed
    $cleanDbName = trim(DB_NAME);
    try {
        $dsn = "mysql:host=" . DB_HOST . ";dbname=" . $cleanDbName . ";charset=utf8mb4";
        $pdo = new PDO($dsn, DB_USER, DB_PASS, $options);
    } catch (PDOException $e) {
        $dsnRaw = "mysql:host=" . DB_HOST . ";dbname=" . DB_NAME . ";charset=utf8mb4";
        $pdo = new PDO($dsnRaw, DB_USER, DB_PASS, $options);
    }

    ensureDatabaseTablesExist($pdo);
    return $pdo;
}

/**
 * Automatically creates the 3 simulation tables if they don't exist yet in the database.
 */
function ensureDatabaseTablesExist(PDO $pdo): void
{
    $pdo->exec("
        CREATE TABLE IF NOT EXISTS `training_sessions` (
            `id` INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            `session_uuid` VARCHAR(64) NOT NULL UNIQUE,
            `trainee_name` VARCHAR(100) NOT NULL DEFAULT 'Astronaut_Trainee',
            `mission_name` TEXT NOT NULL,
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
    ");

    $pdo->exec("
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
    ");

    $pdo->exec("
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
    ");

    $pdo->exec("
        CREATE TABLE IF NOT EXISTS `ai_object_edit_logs` (
            `id` BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            `session_uuid` VARCHAR(64) NOT NULL,
            `elapsed_time_seconds` FLOAT NOT NULL DEFAULT 0.0,
            `object_name` VARCHAR(150) NOT NULL,
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
    ");
}
