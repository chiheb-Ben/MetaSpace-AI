<?php
/**
 * MetaSpace Zero-G Astronaut Training Simulator - JSON REST API
 * Connects Unreal Engine 5.7 & Meta AI to the MySQL Database (spacesumlater).
 */

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type, Authorization');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

require_once __DIR__ . '/config.php';

/**
 * Converts any raw JSON AI message into clean, human-readable text.
 */
function formatCleanAIMessage(?string $raw): string
{
    if ($raw === null || trim($raw) === '') {
        return 'AI Coach monitoring trainee performance...';
    }
    $trimmed = trim($raw);
    if ($trimmed[0] === '{' || $trimmed[0] === '[') {
        $data = json_decode($trimmed, true);
        if (is_array($data)) {
            if (!empty($data['message']) && is_string($data['message'])) {
                return $data['message'];
            }
            if (isset($data['choices'][0]['message']['content'])) {
                $content = $data['choices'][0]['message']['content'];
                if (is_string($content) && trim($content) !== '') {
                    $inner = json_decode($content, true);
                    if (is_array($inner) && !empty($inner['message'])) {
                        return (string)$inner['message'];
                    }
                    return $content;
                }
            }
            if (isset($data['choices'][0]['finish_reason']) && $data['choices'][0]['finish_reason'] === 'length') {
                return 'Meta AI Coach: Station zero-G telemetry analyzed. Use wall-grab launches to reach target slots faster!';
            }
        }
    }
    return $trimmed;
}

/**
 * Converts any raw JSON physics_commands into clean, easy-to-read row text.
 */
function formatCleanPhysicsRows($raw): string
{
    if ($raw === null || $raw === '') {
        return 'Standard Zero-G Calibration';
    }
    if (is_string($raw)) {
        $trimmed = trim($raw);
        if ($trimmed === '') {
            return 'Standard Zero-G Calibration';
        }
        if ($trimmed[0] !== '{' && $trimmed[0] !== '[') {
            return $trimmed;
        }
        $decoded = json_decode($trimmed, true);
    } else {
        $decoded = $raw;
    }

    if (!is_array($decoded)) {
        return is_string($raw) ? $raw : 'Standard Zero-G Calibration';
    }

    // If wrapped in OpenAI chatcmpl response
    if (isset($decoded['choices'][0]['message']['content']) && is_string($decoded['choices'][0]['message']['content'])) {
        $inner = json_decode($decoded['choices'][0]['message']['content'], true);
        if (is_array($inner)) {
            $decoded = $inner;
        }
    }

    $rows = [];
    if (!empty($decoded['ai_quest']) && is_array($decoded['ai_quest'])) {
        $q = $decoded['ai_quest'];
        $qTitle  = $q['quest_title'] ?? 'Ship Breakdown';
        $qTarget = $q['target_actor'] ?? 'ShipSystem';
        $qInstr  = $q['quest_instruction'] ?? 'Press [E] at 3D Quest Box';
        $rows[]  = '[AI Game Master Quest] -> ' . $qTitle . ' @ ' . $qTarget . ' (' . $qInstr . ')';
    } elseif (!empty($decoded['ai_mission'])) {
        $rows[] = '[AI Dynamic Mission] -> ' . $decoded['ai_mission'];
    }
    if (!empty($decoded['risk_level'])) {
        $rScore = isset($decoded['risk_score']) ? ' (' . round((float)$decoded['risk_score']) . '%)' : '';
        $rows[] = '[AI Risk Assessment] -> ' . strtoupper((string)$decoded['risk_level']) . $rScore;
    }
    if (!empty($decoded['ship_events']) && is_array($decoded['ship_events'])) {
        $se = $decoded['ship_events'];
        $seParts = [];
        if (!empty($se['ship_shake_intensity']))      $seParts[] = 'Camera/Ship Shake: ' . $se['ship_shake_intensity'] . ' (' . ($se['ship_shake_duration'] ?? 2.5) . 's)';
        if (!empty($se['spawn_obstacle_count']))      $seParts[] = 'Spawned Obstacles: ' . $se['spawn_obstacle_count'] . 'x ' . ($se['spawn_obstacle_name'] ?? 'SpaceDebris');
        if (!empty($se['hurl_existing_objects_count'])) $seParts[] = 'Hurled at Player: ' . $se['hurl_existing_objects_count'] . ' objects';
        if (!empty($seParts)) {
            $rows[] = '[Spaceship AI Events] -> ' . implode(' | ', $seParts);
        }
    }

    if (!empty($decoded['light_commands']) && is_array($decoded['light_commands'])) {
        foreach ($decoded['light_commands'] as $lcmd) {
            if (!is_array($lcmd)) continue;
            $lTarget = $lcmd['target'] ?? 'ALL_LIGHTS';
            $lParts  = [];
            if (!empty($lcmd['preset']))               $lParts[] = 'Preset: ' . $lcmd['preset'];
            if (!empty($lcmd['mode']))                 $lParts[] = 'Mode: ' . $lcmd['mode'];
            if (isset($lcmd['intensity_multiplier']))  $lParts[] = 'Intensity: ' . $lcmd['intensity_multiplier'] . 'x';
            if (!empty($lParts)) {
                $rows[] = '[Lights: ' . $lTarget . '] -> ' . implode(' | ', $lParts);
            }
        }
    }

    $commands = $decoded['physics_commands'] ?? (isset($decoded[0]) ? $decoded : []);
    if ((empty($commands) || !is_array($commands)) && empty($rows)) {
        return 'Standard Zero-G Calibration (No Physics Override)';
    }

    foreach ($commands as $cmd) {
        if (!is_array($cmd)) continue;
        $target = $cmd['target'] ?? 'Object';
        $parts  = [];
        if (isset($cmd['scale']))                  $parts[] = 'Size/Scale: ' . $cmd['scale'] . 'x';
        if (isset($cmd['size_scale']))             $parts[] = 'Size/Scale: ' . $cmd['size_scale'] . 'x';
        if (isset($cmd['mass_kg']))                $parts[] = 'Mass: ' . $cmd['mass_kg'] . ' kg';
        if (isset($cmd['spin_speed']))             $parts[] = 'Spin: ' . $cmd['spin_speed'] . ' deg/s';
        if (isset($cmd['max_speed']))              $parts[] = 'Max Speed: ' . $cmd['max_speed'] . ' cm/s';
        if (isset($cmd['zero_g']))                 $parts[] = 'Zero-G: ' . ($cmd['zero_g'] ? 'ON' : 'OFF');
        if (isset($cmd['bounce']))                 $parts[] = 'Bounce: ' . $cmd['bounce'];
        if (isset($cmd['wave_amplitude']))         $parts[] = 'Wave: ' . $cmd['wave_amplitude'] . ' cm';
        if (!empty($cmd['push_toward_player']))    $parts[] = 'Action: Pushed Toward Player';
        if (!empty($cmd['repel_from_player']))     $parts[] = 'Action: Repelled From Player';
        if (isset($cmd['player_max_speed']))       $parts[] = 'Player Max Speed: ' . $cmd['player_max_speed'];
        if (isset($cmd['player_launch_strength'])) $parts[] = 'Wall Launch Power: ' . $cmd['player_launch_strength'];

        if (!empty($parts)) {
            $rows[] = '[' . $target . '] -> ' . implode(' | ', $parts);
        }
    }

    return !empty($rows) ? implode(' || ', $rows) : 'Standard Zero-G Calibration';
}

try {
    $pdo = getDatabaseConnection();
    $pdo->setAttribute(PDO::ATTR_EMULATE_PREPARES, true);
    
    // Auto-expand all table columns to prevent String data truncated errors (1406)
    try {
        $pdo->exec("ALTER TABLE `training_sessions` MODIFY COLUMN `mission_name` TEXT NOT NULL");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_sessions` MODIFY COLUMN `trainee_name` VARCHAR(190) NOT NULL DEFAULT 'Astronaut_Trainee'");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_telemetry_events` MODIFY COLUMN `target_name` TEXT NULL");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_telemetry_events` MODIFY COLUMN `actor_name` TEXT NULL");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `ai_object_edit_logs` MODIFY COLUMN `object_name` VARCHAR(255) NOT NULL");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `ai_object_edit_logs` MODIFY COLUMN `size_before` TEXT NOT NULL");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `ai_object_edit_logs` MODIFY COLUMN `size_after` TEXT NOT NULL");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `ai_object_edit_logs` MODIFY COLUMN `changes_summary` TEXT NULL");
    } catch (Exception $ignore) {}

    // Auto-ensure risk_level & risk_score columns exist on training_sessions
    try {
        $pdo->exec("ALTER TABLE `training_sessions` ADD COLUMN `risk_level` VARCHAR(32) NOT NULL DEFAULT 'MODERATE'");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_sessions` ADD COLUMN `risk_score` FLOAT NOT NULL DEFAULT 35.0");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_sessions` ADD COLUMN `flight_readiness` VARCHAR(64) NOT NULL DEFAULT 'IN_EVALUATION'");
    } catch (Exception $ignore) {}

    // Auto-create `quest_catalog` table to store merged Manual & AI-Generated quests and prevent repeating fast-mastered quests
    try {
        $pdo->exec("
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
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
        ");
    } catch (Exception $ignore) {}
} catch ( Exception $e ) {
    http_response_code(500);
    echo json_encode([
        'ok'      => false,
        'error'   => 'Database connection failed',
        'message' => $e->getMessage()
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

// Read incoming JSON body (or GET/POST form parameters as fallback)
$rawBody = file_get_contents('php://input');
$input   = [];
if (!empty($rawBody)) {
    $decoded = json_decode($rawBody, true);
    if (is_array($decoded)) {
        $input = $decoded;
    }
}
$input = array_merge($_GET, $_POST, $input);

$action = isset($input['action']) ? trim((string)$input['action']) : 'status';

try {
    switch ($action) {

        // ====================================================================
        // 1. START TRAINING SESSION
        // ====================================================================
        case 'start_session': {
            $sessionUuid   = !empty($input['session_uuid']) ? trim($input['session_uuid']) : ('sess_' . bin2hex(random_bytes(8)));
            $traineeName   = !empty($input['trainee_name']) ? mb_substr(trim($input['trainee_name']), 0, 190) : 'Astronaut_Trainee';
            $missionName   = !empty($input['mission_name']) ? mb_substr(trim($input['mission_name']), 0, 500) : 'Awaiting AI Game Master Quest...';
            $riskLevel     = !empty($input['risk_level']) ? strtoupper(trim($input['risk_level'])) : 'MODERATE';
            $riskScore     = isset($input['risk_score']) ? max(0.0, min(100.0, (float)$input['risk_score'])) : 35.0;
            $targetTime    = isset($input['target_time_seconds']) ? (float)$input['target_time_seconds'] : 180.0;
            $requiredSnaps = isset($input['required_snaps_count']) ? (int)$input['required_snaps_count'] : 0;
            $worldJson     = isset($input['world_objects_json'])
                ? (is_string($input['world_objects_json']) ? $input['world_objects_json'] : json_encode($input['world_objects_json'], JSON_UNESCAPED_UNICODE))
                : null;

            $stmt = $pdo->prepare("
                INSERT INTO `training_sessions`
                    (`session_uuid`, `trainee_name`, `mission_name`, `risk_level`, `risk_score`, `status`, `target_time_seconds`, `required_snaps_count`, `world_objects_json`)
                VALUES
                    (:uuid, :trainee, :mission, :rlevel, :rscore, 'in_progress', :target_time, :req_snaps, :world_json)
                ON DUPLICATE KEY UPDATE
                    `status` = 'in_progress',
                    `risk_level` = VALUES(`risk_level`),
                    `risk_score` = VALUES(`risk_score`),
                    `world_objects_json` = VALUES(`world_objects_json`)
            ");
            $stmt->execute([
                ':uuid'        => $sessionUuid,
                ':trainee'     => $traineeName,
                ':mission'     => $missionName,
                ':rlevel'      => $riskLevel,
                ':rscore'      => $riskScore,
                ':target_time' => $targetTime,
                ':req_snaps'   => $requiredSnaps,
                ':world_json'  => $worldJson
            ]);

            // Fetch trainee's historical performance from past completed sessions so Meta AI can compare times!
            $histStmt = $pdo->prepare("
                SELECT
                    COUNT(*) AS completed_missions,
                    MIN(`duration_seconds`) AS best_time_seconds,
                    AVG(`duration_seconds`) AS avg_time_seconds,
                    AVG(`final_score`) AS avg_score
                FROM `training_sessions`
                WHERE `trainee_name` = :trainee AND `status` = 'completed' AND `duration_seconds` > 0
            ");
            $histStmt->execute([':trainee' => $traineeName]);
            $history = $histStmt->fetch() ?: [
                'completed_missions' => 0,
                'best_time_seconds'  => null,
                'avg_time_seconds'   => null,
                'avg_score'          => null
            ];

            // Also get the last AI coaching advice given to this trainee
            $lastAdviceStmt = $pdo->prepare("
                SELECT `ai_coaching_advice`, `duration_seconds`, `ai_grade`
                FROM `training_sessions`
                WHERE `trainee_name` = :trainee AND `ai_coaching_advice` IS NOT NULL AND `ai_coaching_advice` != ''
                ORDER BY `id` DESC LIMIT 1
            ");
            $lastAdviceStmt->execute([':trainee' => $traineeName]);
            $lastRun = $lastAdviceStmt->fetch() ?: null;

            // Fetch all saved quests from `quest_catalog` so Unreal Engine can reuse uncompleted quests and skip fast-mastered quests!
            $questCatalogRows = [];
            try {
                $qStmt = $pdo->query("SELECT * FROM `quest_catalog` ORDER BY `times_completed` ASC, `id` ASC");
                $questCatalogRows = $qStmt->fetchAll() ?: [];
            } catch (Exception $ignore) {}

            echo json_encode([
                'ok'                  => true,
                'action'              => 'start_session',
                'session_uuid'        => $sessionUuid,
                'trainee_name'        => $traineeName,
                'mission_name'        => $missionName,
                'target_time_seconds' => $targetTime,
                'required_snaps'      => $requiredSnaps,
                'trainee_history'     => [
                    'completed_missions' => (int)($history['completed_missions'] ?? 0),
                    'best_time_seconds'  => $history['best_time_seconds'] !== null ? round((float)$history['best_time_seconds'], 2) : 0.0,
                    'avg_time_seconds'   => $history['avg_time_seconds'] !== null ? round((float)$history['avg_time_seconds'], 2) : 0.0,
                    'avg_score'          => $history['avg_score'] !== null ? round((float)$history['avg_score'], 1) : 0.0,
                    'last_run_advice'    => $lastRun ? $lastRun['ai_coaching_advice'] : 'First recorded training session.'
                ],
                'quest_catalog'       => $questCatalogRows
            ], JSON_UNESCAPED_UNICODE);
            break;
        }

        // ====================================================================
        // 2. LOG REAL-TIME TRAINING EVENT (Grab, Drop, Snap, Wall Launch, Timer)
        // ====================================================================
        case 'log_event': {
            $sessionUuid = !empty($input['session_uuid']) ? trim($input['session_uuid']) : 'default_session';
            $eventType   = !empty($input['event_type']) ? trim($input['event_type']) : 'generic_event';
            $actorName   = isset($input['actor_name']) ? mb_substr(trim((string)$input['actor_name']), 0, 500) : null;
            $targetName  = isset($input['target_name']) ? mb_substr(trim((string)$input['target_name']), 0, 500) : null;
            $elapsedSec  = isset($input['elapsed_time_seconds']) ? (float)$input['elapsed_time_seconds'] : 0.0;
            $playerSpeed = isset($input['player_speed']) ? (float)$input['player_speed'] : 0.0;
            $detailsJson = isset($input['details_json'])
                ? (is_string($input['details_json']) ? $input['details_json'] : json_encode($input['details_json'], JSON_UNESCAPED_UNICODE))
                : null;

            $stmt = $pdo->prepare("
                INSERT INTO `training_telemetry_events`
                    (`session_uuid`, `event_type`, `actor_name`, `target_name`, `elapsed_time_seconds`, `player_speed`, `details_json`)
                VALUES
                    (:uuid, :etype, :actor, :target, :elapsed, :speed, :details)
            ");
            $stmt->execute([
                ':uuid'    => $sessionUuid,
                ':etype'   => $eventType,
                ':actor'   => $actorName,
                ':target'  => $targetName,
                ':elapsed' => $elapsedSec,
                ':speed'   => $playerSpeed,
                ':details' => $detailsJson
            ]);

            // Update session counters & live elapsed time (supports SnapPoint, Custom Repairs, and AI Game Master Quests!)
            $grabInc   = ($eventType === 'item_grabbed') ? 1 : 0;
            $snapInc   = in_array($eventType, ['item_snapped', 'custom_repair_completed', 'repair_zone_completed', 'ai_quest_completed'], true) ? 1 : 0;
            $launchInc = in_array($eventType, ['wall_launch', 'wall_pull', 'wall_grab_start', 'wall_walk', 'wall_stop_brake'], true) ? 1 : 0;
            $evMission = !empty($input['mission_name']) ? trim((string)$input['mission_name']) : '';
            $evRiskLvl = !empty($input['risk_level']) ? strtoupper(trim((string)$input['risk_level'])) : '';
            $evRiskScr = isset($input['risk_score']) ? max(0.0, min(100.0, (float)$input['risk_score'])) : -1.0;

            $upd = $pdo->prepare("
                UPDATE `training_sessions`
                SET
                    `duration_seconds`    = GREATEST(`duration_seconds`, :elapsed),
                    `items_grabbed_count` = `items_grabbed_count` + :grab_inc,
                    `items_snapped_count` = `items_snapped_count` + :snap_inc,
                    `wall_launches_count` = `wall_launches_count` + :launch_inc,
                    `mission_name`        = COALESCE(NULLIF(:mission, ''), `mission_name`),
                    `risk_level`          = COALESCE(NULLIF(:rlevel, ''), `risk_level`),
                    `risk_score`          = IF(:rscore >= 0, :rscore, `risk_score`)
                WHERE `session_uuid` = :uuid
            ");
            $upd->execute([
                ':elapsed'    => $elapsedSec,
                ':grab_inc'   => $grabInc,
                ':snap_inc'   => $snapInc,
                ':launch_inc' => $launchInc,
                ':mission'    => $evMission,
                ':rlevel'     => $evRiskLvl,
                ':rscore'     => $evRiskScr,
                ':uuid'       => $sessionUuid
            ]);

            // Fetch current session state to report progress & time analysis
            $sessStmt = $pdo->prepare("SELECT * FROM `training_sessions` WHERE `session_uuid` = :uuid LIMIT 1");
            $sessStmt->execute([':uuid' => $sessionUuid]);
            $session = $sessStmt->fetch();

            $targetTime    = $session ? (float)$session['target_time_seconds'] : 120.0;
            $snappedCount  = $session ? (int)$session['items_snapped_count'] : $snapInc;
            $requiredSnaps = $session ? max(1, (int)$session['required_snaps_count']) : 3;
            $progressRatio = min(1.0, $snappedCount / $requiredSnaps);
            $expectedTimeAtThisProgress = $targetTime * max(0.15, $progressRatio);
            $paceStatus = ($elapsedSec <= $expectedTimeAtThisProgress) ? 'FAST_ON_TRACK' : 'TAKING_LONG_NEEDS_AI_HELP';

            echo json_encode([
                'ok'                   => true,
                'action'               => 'log_event',
                'session_uuid'         => $sessionUuid,
                'event_type'           => $eventType,
                'elapsed_time_seconds' => round($elapsedSec, 2),
                'target_time_seconds'  => round($targetTime, 2),
                'items_snapped_count'  => $snappedCount,
                'required_snaps_count' => $requiredSnaps,
                'remaining_snaps'      => max(0, $requiredSnaps - $snappedCount),
                'pace_status'          => $paceStatus
            ], JSON_UNESCAPED_UNICODE);
            break;
        }

        // ====================================================================
        // 3. LOG META AI COACHING ADVICE, PHYSICS COMMANDS & BEFORE/AFTER OBJECT SIZE LOGS
        // ====================================================================
        case 'log_ai_advice': {
            $sessionUuid = !empty($input['session_uuid']) ? trim($input['session_uuid']) : 'default_session';
            $elapsedSec  = isset($input['elapsed_time_seconds']) ? (float)$input['elapsed_time_seconds'] : 0.0;
            $directive   = isset($input['directive_sent']) ? trim((string)$input['directive_sent']) : null;
            $rawAiMsg    = isset($input['ai_message']) ? trim((string)$input['ai_message']) : '';
            $rawPhys     = $input['physics_commands_json'] ?? null;
            $objectEdits = $input['object_edits'] ?? [];
            $aiMission   = isset($input['mission_name']) ? trim((string)$input['mission_name']) : '';
            $riskLevel   = isset($input['risk_level']) ? trim((string)$input['risk_level']) : '';
            $riskScore   = isset($input['risk_score']) ? (float)$input['risk_score'] : -1.0;

            $aiMessage   = formatCleanAIMessage($rawAiMsg);
            $physicsText = formatCleanPhysicsRows($rawPhys ?: $rawAiMsg);

            $stmt = $pdo->prepare("
                INSERT INTO `ai_coaching_and_physics_logs`
                    (`session_uuid`, `elapsed_time_seconds`, `directive_sent`, `ai_message`, `physics_commands_json`)
                VALUES
                    (:uuid, :elapsed, :directive, :msg, :phys)
            ");
            $stmt->execute([
                ':uuid'      => $sessionUuid,
                ':elapsed'   => $elapsedSec,
                ':directive' => $directive,
                ':msg'       => $aiMessage,
                ':phys'      => $physicsText
            ]);

            // Save Before & After Object Size / Physics logs into `ai_object_edit_logs`
            if (is_string($objectEdits)) {
                $decodedEdits = json_decode($objectEdits, true);
                $objectEdits  = is_array($decodedEdits) ? $decodedEdits : [];
            }

            // Fallback: if object_edits was not sent (e.g. older build), synthesize Before/After from world_objects_json!
            if (empty($objectEdits) || !is_array($objectEdits)) {
                $objectEdits = [];
                $wStmt = $pdo->prepare("SELECT `world_objects_json` FROM `training_sessions` WHERE `session_uuid` = :uuid LIMIT 1");
                $wStmt->execute([':uuid' => $sessionUuid]);
                $wRow = $wStmt->fetch();
                if ($wRow && !empty($wRow['world_objects_json'])) {
                    $wData = json_decode($wRow['world_objects_json'], true);
                    $entities = $wData['world_entities'] ?? [];
                    foreach ($entities as $ent) {
                        if (!is_array($ent)) continue;
                        $objName = $ent['name'] ?? ($ent['id'] ?? 'SpaceObject');
                        $objRole = $ent['role'] ?? 'SpaceProp';
                        $phys    = $ent['object_physics'] ?? ($ent['player_physics'] ?? []);
                        $scaleBefore = isset($phys['scale']) ? (float)$phys['scale'] : 1.0;
                        $sizeCm      = $phys['size_cm'] ?? '50x50x50 cm';
                        $sizeBefore  = number_format($scaleBefore, 2) . 'x (' . $sizeCm . ')';
                        $massBefore  = (float)($phys['mass_kg'] ?? ($phys['body_mass_kg'] ?? 10.0));
                        $spinBefore  = (float)($phys['spin_speed_deg'] ?? 0.0);
                        $speedBefore = (float)($phys['max_speed'] ?? ($phys['max_fly_speed'] ?? 600.0));

                        // Infer AI adjustments
                        $massAfter  = $massBefore;
                        $spinAfter  = max(12.0, $spinBefore);
                        $speedAfter = $speedBefore;
                        $desc       = strtolower(($ent['description'] ?? '') . ' ' . $objName);
                        if (strpos($desc, 'light') !== false || strpos($desc, 'paper') !== false) {
                            $massAfter = 0.25;
                            $spinAfter = 28.0;
                        } elseif (strpos($desc, 'heavy') !== false || strpos($desc, 'crate') !== false) {
                            $massAfter = 55.0;
                            $spinAfter = 7.0;
                        }

                        $objectEdits[] = [
                            'object_name'      => $objName,
                            'object_role'      => $objRole,
                            'size_before'      => $sizeBefore,
                            'size_after'       => $sizeBefore,
                            'mass_before_kg'   => $massBefore,
                            'mass_after_kg'    => $massAfter,
                            'spin_before_deg'  => $spinBefore,
                            'spin_after_deg'   => $spinAfter,
                            'max_speed_before' => $speedBefore,
                            'max_speed_after'  => $speedAfter,
                            'changes_summary'  => sprintf(
                                "Size: %s -> %s, Mass: %.2fkg -> %.2fkg, Spin: %.1fdeg/s -> %.1fdeg/s",
                                $sizeBefore, $sizeBefore, $massBefore, $massAfter, $spinBefore, $spinAfter
                            )
                        ];
                    }
                }
            }

            if (!empty($objectEdits) && is_array($objectEdits)) {
                $editStmt = $pdo->prepare("
                    INSERT INTO `ai_object_edit_logs`
                        (`session_uuid`, `elapsed_time_seconds`, `object_name`, `object_role`,
                         `size_before`, `size_after`, `mass_before_kg`, `mass_after_kg`,
                         `spin_before_deg`, `spin_after_deg`, `max_speed_before`, `max_speed_after`, `changes_summary`)
                    VALUES
                        (:uuid, :elapsed, :oname, :orole,
                         :sbefore, :safter, :mbefore, :mafter,
                         :spbefore, :spafter, :vbefore, :vafter, :summary)
                ");
                foreach ($objectEdits as $edit) {
                    if (!is_array($edit)) continue;
                    $editStmt->execute([
                        ':uuid'     => $sessionUuid,
                        ':elapsed'  => $elapsedSec,
                        ':oname'    => (string)($edit['object_name'] ?? 'SpaceObject'),
                        ':orole'    => (string)($edit['object_role'] ?? 'SpaceProp'),
                        ':sbefore'  => (string)($edit['size_before'] ?? '1.00x (50x50x50 cm)'),
                        ':safter'   => (string)($edit['size_after'] ?? '1.00x (50x50x50 cm)'),
                        ':mbefore'  => (float)($edit['mass_before_kg'] ?? 0.0),
                        ':mafter'   => (float)($edit['mass_after_kg'] ?? 0.0),
                        ':spbefore' => (float)($edit['spin_before_deg'] ?? 0.0),
                        ':spafter'  => (float)($edit['spin_after_deg'] ?? 0.0),
                        ':vbefore'  => (float)($edit['max_speed_before'] ?? 0.0),
                        ':vafter'   => (float)($edit['max_speed_after'] ?? 0.0),
                        ':summary'  => (string)($edit['changes_summary'] ?? '')
                    ]);
                }
            }

            // Extract Meta AI's autonomous evaluation & study if included in response
            $aiEval = null;
            if (isset($input['trainee_evaluation']) && is_array($input['trainee_evaluation'])) {
                $aiEval = $input['trainee_evaluation'];
            } elseif (is_array($rawPhys) && !empty($rawPhys['trainee_evaluation'])) {
                $aiEval = $rawPhys['trainee_evaluation'];
            } elseif (is_string($rawPhys) && ($rawPhys[0] === '{' || $rawPhys[0] === '[')) {
                $decodedP = json_decode($rawPhys, true);
                if (is_array($decodedP)) {
                    if (isset($decodedP['choices'][0]['message']['content'])) {
                        $inner = json_decode($decodedP['choices'][0]['message']['content'], true);
                        if (is_array($inner) && !empty($inner['trainee_evaluation'])) {
                            $aiEval = $inner['trainee_evaluation'];
                        }
                    } elseif (!empty($decodedP['trainee_evaluation'])) {
                        $aiEval = $decodedP['trainee_evaluation'];
                    }
                }
            }
            if (!$aiEval && is_string($rawAiMsg) && ($rawAiMsg[0] === '{' || $rawAiMsg[0] === '[')) {
                $decodedMsg = json_decode($rawAiMsg, true);
                if (is_array($decodedMsg) && !empty($decodedMsg['trainee_evaluation'])) {
                    $aiEval = $decodedMsg['trainee_evaluation'];
                }
            }

            $evalGrade     = !empty($input['ai_grade']) && $input['ai_grade'] !== 'PENDING' ? strtoupper(trim($input['ai_grade'])) : null;
            $evalScore     = isset($input['final_score']) && (float)$input['final_score'] > 0 ? (float)$input['final_score'] : null;
            $evalReadiness = !empty($input['flight_readiness']) && $input['flight_readiness'] !== 'IN_EVALUATION' ? strtoupper(trim($input['flight_readiness'])) : null;
            $evalSummary   = !empty($input['ai_evaluation_summary']) ? trim((string)$input['ai_evaluation_summary']) : null;

            if (is_array($aiEval)) {
                if (!empty($aiEval['grade']))            $evalGrade     = strtoupper(trim((string)$aiEval['grade']));
                if (isset($aiEval['score']))             $evalScore     = (float)$aiEval['score'];
                if (!empty($aiEval['flight_readiness'])) $evalReadiness = strtoupper(trim((string)$aiEval['flight_readiness']));
                if (!empty($aiEval['study_summary']))    $evalSummary   = trim((string)$aiEval['study_summary']);
                elseif (!empty($aiEval['summary']))      $evalSummary   = trim((string)$aiEval['summary']);
            }

            if (!empty($aiMessage) || $evalGrade !== null || $evalSummary !== null) {
                $upd = $pdo->prepare("
                    UPDATE `training_sessions`
                    SET `ai_coaching_advice`    = COALESCE(NULLIF(:msg, ''), `ai_coaching_advice`),
                        `mission_name`          = COALESCE(NULLIF(:mission, ''), `mission_name`),
                        `risk_level`            = COALESCE(NULLIF(:rlevel, ''), `risk_level`),
                        `risk_score`            = IF(:rscore >= 0, :rscore, `risk_score`),
                        `duration_seconds`      = GREATEST(`duration_seconds`, :elapsed),
                        `ai_grade`              = COALESCE(:egrade, `ai_grade`),
                        `final_score`           = COALESCE(:escore, `final_score`),
                        `flight_readiness`      = COALESCE(:ereadiness, `flight_readiness`),
                        `ai_evaluation_summary` = COALESCE(:esummary, `ai_evaluation_summary`)
                    WHERE `session_uuid` = :uuid
                ");
                $upd->execute([
                    ':msg'        => $aiMessage,
                    ':mission'    => $aiMission,
                    ':rlevel'     => strtoupper($riskLevel),
                    ':rscore'     => $riskScore,
                    ':elapsed'    => $elapsedSec,
                    ':egrade'     => $evalGrade,
                    ':escore'     => $evalScore,
                    ':ereadiness' => $evalReadiness,
                    ':esummary'   => $evalSummary,
                    ':uuid'       => $sessionUuid
                ]);
            }

            echo json_encode([
                'ok'                 => true,
                'action'             => 'log_ai_advice',
                'session_uuid'       => $sessionUuid,
                'saved_advice'       => $aiMessage,
                'logged_object_edits'=> count($objectEdits)
            ], JSON_UNESCAPED_UNICODE);
            break;
        }

        // ====================================================================
        // 4. FINISH MISSION & RECORD COMPLETION TIME + AI EVALUATION
        // ====================================================================
        case 'finish_session': {
            $sessionUuid   = !empty($input['session_uuid']) ? trim($input['session_uuid']) : 'default_session';
            $status        = !empty($input['status']) ? trim($input['status']) : 'completed';
            $durationSec   = isset($input['duration_seconds']) ? (float)$input['duration_seconds'] : 0.0;
            $aiAdvice      = isset($input['ai_coaching_advice']) ? trim((string)$input['ai_coaching_advice']) : null;
            $aiSummary     = isset($input['ai_evaluation_summary']) ? trim((string)$input['ai_evaluation_summary']) : null;

            $sessStmt = $pdo->prepare("SELECT * FROM `training_sessions` WHERE `session_uuid` = :uuid LIMIT 1");
            $sessStmt->execute([':uuid' => $sessionUuid]);
            $session = $sessStmt->fetch();

            $targetTime    = $session ? (float)$session['target_time_seconds'] : 120.0;
            $snappedCount  = isset($input['items_snapped_count']) ? (int)$input['items_snapped_count'] : ($session ? (int)$session['items_snapped_count'] : 0);
            $grabbedCount  = isset($input['items_grabbed_count']) ? (int)$input['items_grabbed_count'] : ($session ? (int)$session['items_grabbed_count'] : 0);
            $launchesCount = isset($input['wall_launches_count']) ? (int)$input['wall_launches_count'] : ($session ? (int)$session['wall_launches_count'] : 0);
            $requiredSnaps = $session ? max(1, (int)$session['required_snaps_count']) : 3;

            if ($durationSec <= 0.0 && $session) {
                $durationSec = (float)$session['duration_seconds'];
            }

            // 100% Meta AI Autonomy: Prioritize Meta AI's autonomous score & grade if provided!
            $finalScore = null;
            if (isset($input['final_score']) && (float)$input['final_score'] > 0.0) {
                $finalScore = (float)$input['final_score'];
            } elseif ($session && (float)$session['final_score'] > 0.0) {
                $finalScore = (float)$session['final_score'];
            }

            $grade = null;
            if (!empty($input['ai_grade']) && $input['ai_grade'] !== 'PENDING') {
                $grade = strtoupper(trim((string)$input['ai_grade']));
            } elseif ($session && !empty($session['ai_grade']) && $session['ai_grade'] !== 'PENDING') {
                $grade = strtoupper(trim((string)$session['ai_grade']));
            }

            $flightReadiness = !empty($input['flight_readiness'])
                ? strtoupper(trim((string)$input['flight_readiness']))
                : (!empty($session['flight_readiness']) ? $session['flight_readiness'] : 'CERTIFIED_ZERO_G_OPERATOR');

            if (empty($aiSummary) && $session && !empty($session['ai_evaluation_summary'])) {
                $aiSummary = $session['ai_evaluation_summary'];
            }

            // Only fallback to formula if Meta AI has not evaluated the session yet
            if ($finalScore === null) {
                $completionPoints = min(60.0, ($snappedCount / $requiredSnaps) * 60.0);
                $timeRatio        = ($durationSec > 0.0) ? ($targetTime / $durationSec) : 1.0;
                $timePoints       = max(0.0, min(40.0, $timeRatio * 35.0));
                $finalScore       = round($completionPoints + $timePoints, 1);
            }

            if ($grade === null) {
                if ($finalScore >= 90.0)      $grade = 'S';
                elseif ($finalScore >= 80.0)  $grade = 'A';
                elseif ($finalScore >= 65.0)  $grade = 'B';
                elseif ($finalScore >= 50.0)  $grade = 'C';
                else                          $grade = 'D';
            }

            if (empty($aiSummary)) {
                $aiSummary = sprintf(
                    "Meta AI Debrief: Mission %s in %.1f seconds (Target: %.1fs). Snapped %d/%d items, %d grabs, %d wall launches. Final Score: %.1f/100 (Grade %s). Flight Readiness: %s.",
                    $status, $durationSec, $targetTime, $snappedCount, $requiredSnaps, $grabbedCount, $launchesCount, $finalScore, $grade, $flightReadiness
                );
            }

            $upd = $pdo->prepare("
                UPDATE `training_sessions`
                SET
                    `status`                = :status,
                    `duration_seconds`      = :duration,
                    `items_grabbed_count`   = :grabs,
                    `items_snapped_count`   = :snaps,
                    `wall_launches_count`   = :launches,
                    `final_score`           = :score,
                    `ai_grade`              = :grade,
                    `flight_readiness`      = :readiness,
                    `ai_coaching_advice`    = COALESCE(:advice, `ai_coaching_advice`),
                    `ai_evaluation_summary` = :summary,
                    `finished_at`           = CURRENT_TIMESTAMP
                WHERE `session_uuid` = :uuid
            ");
            $upd->execute([
                ':status'    => $status,
                ':duration'  => $durationSec,
                ':grabs'     => $grabbedCount,
                ':snaps'     => $snappedCount,
                ':launches'  => $launchesCount,
                ':score'     => $finalScore,
                ':grade'     => $grade,
                ':readiness' => $flightReadiness,
                ':advice'    => $aiAdvice,
                ':summary'   => $aiSummary,
                ':uuid'      => $sessionUuid
            ]);

            echo json_encode([
                'ok'                    => true,
                'action'                => 'finish_session',
                'session_uuid'          => $sessionUuid,
                'status'                => $status,
                'duration_seconds'      => round($durationSec, 2),
                'target_time_seconds'   => round($targetTime, 2),
                'final_score'           => $finalScore,
                'ai_grade'              => $grade,
                'items_snapped_count'   => $snappedCount,
                'required_snaps_count'  => $requiredSnaps,
                'ai_evaluation_summary' => $aiSummary,
                'ai_coaching_advice'    => $aiAdvice
            ], JSON_UNESCAPED_UNICODE);
            break;
        }

        // ====================================================================
        // 5. SYNC / SAVE MANUAL OR AI-GENERATED QUEST INTO `quest_catalog` DATABASE
        // ====================================================================
        case 'sync_quest_catalog': {
            $questTitle   = !empty($input['quest_title']) ? trim((string)$input['quest_title']) : 'Spaceship Repair Quest';
            $questInstr   = !empty($input['quest_instruction']) ? trim((string)$input['quest_instruction']) : 'Bring required tool and press [E]';
            $targetActor  = !empty($input['target_actor']) ? trim((string)$input['target_actor']) : 'ShipSystem';
            $requiredItem = isset($input['required_item']) ? trim((string)$input['required_item']) : '';
            $questSource  = !empty($input['quest_source']) ? strtoupper(trim((string)$input['quest_source'])) : 'AI_GENERATED';
            $traineeName  = !empty($input['trainee_name']) ? trim((string)$input['trainee_name']) : 'Astronaut_Trainee';
            $isCompleted  = !empty($input['is_completed']);

            // Accept both key names: completion_time_sec and completion_duration_sec
            $compTimeSec = 0.0;
            if (isset($input['completion_time_sec']) && (float)$input['completion_time_sec'] > 0.0) {
                $compTimeSec = (float)$input['completion_time_sec'];
            } elseif (isset($input['completion_duration_sec']) && (float)$input['completion_duration_sec'] > 0.0) {
                $compTimeSec = (float)$input['completion_duration_sec'];
            }

            $sessionDurationSec = isset($input['session_duration_sec']) ? max(0.0, (float)$input['session_duration_sec']) : $compTimeSec;
            $completedQuestsCnt = isset($input['completed_quests_count']) ? (int)$input['completed_quests_count'] : ($isCompleted ? 1 : 0);

            $questKey = !empty($input['quest_key'])
                ? strtolower(trim((string)$input['quest_key']))
                : strtolower($targetActor . '__' . $requiredItem);

            // Check if quest already exists in catalog
            $chk = $pdo->prepare("SELECT * FROM `quest_catalog` WHERE `quest_key` = :qkey LIMIT 1");
            $chk->execute([':qkey' => $questKey]);
            $existing = $chk->fetch();

            if (!$existing) {
                $ins = $pdo->prepare("
                    INSERT INTO `quest_catalog`
                        (`quest_key`, `quest_source`, `quest_title`, `quest_instruction`, `target_actor`, `required_item`,
                         `times_assigned`, `times_completed`, `best_completion_time_sec`, `avg_completion_time_sec`, `last_trainee`)
                    VALUES
                        (:qkey, :qsrc, :qtitle, :qinstr, :target, :reqitem,
                         1, :tcomp, :best_sec, :avg_sec, :trainee)
                ");
                $ins->execute([
                    ':qkey'     => $questKey,
                    ':qsrc'     => $questSource,
                    ':qtitle'   => $questTitle,
                    ':qinstr'   => $questInstr,
                    ':target'   => $targetActor,
                    ':reqitem'  => $requiredItem,
                    ':tcomp'    => $isCompleted ? 1 : 0,
                    ':best_sec' => ($isCompleted && $compTimeSec > 0.0) ? $compTimeSec : 0.0,
                    ':avg_sec'  => ($isCompleted && $compTimeSec > 0.0) ? $compTimeSec : 0.0,
                    ':trainee'  => $traineeName
                ]);
            } else {
                $oldCompleted = (int)$existing['times_completed'];
                $oldBest      = (float)$existing['best_completion_time_sec'];
                $oldAvg       = (float)$existing['avg_completion_time_sec'];

                $newCompleted = $isCompleted ? ($oldCompleted + 1) : $oldCompleted;
                $newBest      = $oldBest;
                $newAvg       = $oldAvg;

                if ($isCompleted && $compTimeSec > 0.0) {
                    $newBest = ($oldBest <= 0.0) ? $compTimeSec : min($oldBest, $compTimeSec);
                    $newAvg  = ($oldCompleted <= 0)
                        ? $compTimeSec
                        : (($oldAvg * $oldCompleted) + $compTimeSec) / $newCompleted;
                }

                $updQ = $pdo->prepare("
                    UPDATE `quest_catalog`
                    SET
                        `quest_title`              = IF(:qtitle != '', :qtitle, `quest_title`),
                        `quest_instruction`        = IF(:qinstr != '', :qinstr, `quest_instruction`),
                        `quest_source`             = IF(`quest_source` = 'MANUAL', 'MANUAL', :qsrc),
                        `times_assigned`           = `times_assigned` + :assign_inc,
                        `times_completed`          = :tcomp,
                        `best_completion_time_sec` = :best_sec,
                        `avg_completion_time_sec`  = :avg_sec,
                        `last_trainee`             = :trainee
                    WHERE `quest_key` = :qkey
                ");
                $updQ->execute([
                    ':qtitle'     => $questTitle,
                    ':qinstr'     => $questInstr,
                    ':qsrc'       => $questSource,
                    ':assign_inc' => $isCompleted ? 0 : 1,
                    ':tcomp'      => $newCompleted,
                    ':best_sec'   => round($newBest, 2),
                    ':avg_sec'    => round($newAvg, 2),
                    ':trainee'    => $traineeName,
                    ':qkey'       => $questKey
                ]);
            }

            // Also update the active training session with duration, fixed count, status, risk level immediately!
            $sessionUuid = !empty($input['session_uuid']) ? trim((string)$input['session_uuid']) : '';
            $riskLevel   = !empty($input['risk_level']) ? strtoupper(trim((string)$input['risk_level'])) : '';
            $riskScore   = isset($input['risk_score']) ? (float)$input['risk_score'] : -1.0;
            if (!empty($sessionUuid)) {
                $fullMissionDesc = $questTitle . ' (' . $questInstr . ')';
                $completedSql = $isCompleted ? ", `status` = 'completed'" : "";
                $pdo->prepare("
                    UPDATE `training_sessions`
                    SET `mission_name`        = IF(:m != '', :m, `mission_name`),
                        `risk_level`          = IF(:rl != '', :rl, `risk_level`),
                        `risk_score`          = IF(:rs >= 0, :rs, `risk_score`),
                        `duration_seconds`    = GREATEST(`duration_seconds`, :dur_sec),
                        `items_snapped_count` = GREATEST(`items_snapped_count`, :comp_cnt)
                        $completedSql
                    WHERE `session_uuid` = :u
                ")->execute([
                    ':m'        => $fullMissionDesc,
                    ':rl'       => $riskLevel,
                    ':rs'       => $riskScore,
                    ':dur_sec'  => round($sessionDurationSec, 2),
                    ':comp_cnt' => $completedQuestsCnt,
                    ':u'        => $sessionUuid
                ]);
            }

            echo json_encode([
                'ok'                  => true,
                'action'              => 'sync_quest_catalog',
                'quest_key'           => $questKey,
                'quest_source'        => $questSource,
                'is_completed'        => $isCompleted,
                'completion_time_sec' => round($compTimeSec, 2)
            ], JSON_UNESCAPED_UNICODE);
            break;
        }

        // ====================================================================
        // 6. GET SESSION SUMMARY & HISTORICAL LEADERBOARD FOR META AI
        // ====================================================================
        case 'get_training_summary':
        case 'status':
        default: {
            $sessionUuid = !empty($input['session_uuid']) ? trim($input['session_uuid']) : '';

            $currentSession = null;
            $recentEvents   = [];
            if (!empty($sessionUuid)) {
                $stmt = $pdo->prepare("SELECT * FROM `training_sessions` WHERE `session_uuid` = :uuid LIMIT 1");
                $stmt->execute([':uuid' => $sessionUuid]);
                $currentSession = $stmt->fetch() ?: null;

                $evStmt = $pdo->prepare("
                    SELECT `event_type`, `actor_name`, `target_name`, `elapsed_time_seconds`, `player_speed`, `created_at`
                    FROM `training_telemetry_events`
                    WHERE `session_uuid` = :uuid
                    ORDER BY `id` DESC LIMIT 15
                ");
                $evStmt->execute([':uuid' => $sessionUuid]);
                $recentEvents = $evStmt->fetchAll();
            }

            $recentSessionsStmt = $pdo->query("
                SELECT `session_uuid`, `trainee_name`, `mission_name`, `status`, `duration_seconds`,
                       `target_time_seconds`, `items_snapped_count`, `required_snaps_count`,
                       `wall_launches_count`, `final_score`, `ai_grade`, `ai_coaching_advice`, `started_at`
                FROM `training_sessions`
                ORDER BY `id` DESC LIMIT 10
            ");
            $recentSessions = $recentSessionsStmt->fetchAll();

            echo json_encode([
                'ok'              => true,
                'database'        => trim(DB_NAME),
                'current_session' => $currentSession,
                'recent_events'   => $recentEvents,
                'recent_sessions' => $recentSessions
            ], JSON_UNESCAPED_UNICODE);
            break;
        }
    }
} catch (Exception $e) {
    http_response_code(500);
    echo json_encode([
        'ok'      => false,
        'error'   => 'Server execution error',
        'message' => $e->getMessage()
    ], JSON_UNESCAPED_UNICODE);
}
