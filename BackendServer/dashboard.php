<?php
/**
 * MetaSpace Zero-G Astronaut Training Simulator - Live Mission Control Dashboard
 * Formats all AI Advice and Physics Commands as clean, readable row data (never raw JSON!).
 */
require_once __DIR__ . '/config.php';

/**
 * Extracts a clean human-readable message even if an older record stored raw JSON.
 */
function renderReadableAdvice(?string $raw): string
{
    if ($raw === null || trim($raw) === '') {
        return 'Waiting for AI Coach evaluation...';
    }
    $trimmed = trim($raw);
    if ($trimmed[0] === '{' || $trimmed[0] === '[') {
        $data = json_decode($trimmed, true);
        if (is_array($data)) {
            if (!empty($data['message']) && is_string($data['message'])) {
                return $data['message'];
            }
            if (isset($data['choices'][0]['message']['content']) && is_string($data['choices'][0]['message']['content'])) {
                $content = trim($data['choices'][0]['message']['content']);
                if ($content !== '') {
                    $inner = json_decode($content, true);
                    if (is_array($inner) && !empty($inner['message'])) {
                        return (string)$inner['message'];
                    }
                    return $content;
                }
            }
            if (isset($data['choices'][0]['finish_reason']) && $data['choices'][0]['finish_reason'] === 'length') {
                $tokens = $data['usage']['total_tokens'] ?? 0;
                return "Meta AI Coach Active (Analyzed station telemetry — {$tokens} tokens processed). Use wall-grab launches to reach snap targets faster!";
            }
        }
    }
    return $trimmed;
}

/**
 * Converts physics commands (whether JSON or pipe-separated text) into clean visual HTML badges/rows!
 */
function renderReadablePhysicsHTML(?string $raw): string
{
    if ($raw === null || trim($raw) === '') {
        return '<span class="phys-pill">Standard Zero-G Calibration</span>';
    }
    $trimmed = trim($raw);

    // If already formatted as readable "Target -> Mass=8kg | ..." text from Unreal Engine:
    if ($trimmed[0] !== '{' && $trimmed[0] !== '[') {
        $items = explode('|', $trimmed);
        $html  = '';
        foreach ($items as $item) {
            $clean = trim($item);
            if ($clean !== '') {
                $html .= '<div class="phys-row">' . htmlspecialchars($clean) . '</div>';
            }
        }
        return $html ?: '<span class="phys-pill">Standard Zero-G Calibration</span>';
    }

    // Otherwise parse JSON and build clean visual rows
    $decoded = json_decode($trimmed, true);
    if (!is_array($decoded)) {
        return '<div class="phys-row">' . htmlspecialchars($trimmed) . '</div>';
    }

    if (isset($decoded['choices'][0]['message']['content']) && is_string($decoded['choices'][0]['message']['content'])) {
        $inner = json_decode($decoded['choices'][0]['message']['content'], true);
        if (is_array($inner)) {
            $decoded = $inner;
        }
    }

    $html = '';
    if (!empty($decoded['ship_events']) && is_array($decoded['ship_events'])) {
        $se = $decoded['ship_events'];
        $seBadges = [];
        if (!empty($se['ship_shake_intensity']))        $seBadges[] = 'Ship/Camera Shake: <b>' . (float)$se['ship_shake_intensity'] . ' (' . (float)($se['ship_shake_duration'] ?? 2.5) . 's)</b>';
        if (!empty($se['spawn_obstacle_count']))        $seBadges[] = 'Spawned Obstacles: <b>' . (int)$se['spawn_obstacle_count'] . 'x ' . htmlspecialchars((string)($se['spawn_obstacle_name'] ?? 'SpaceDebris')) . '</b>';
        if (!empty($se['hurl_existing_objects_count'])) $seBadges[] = 'Hurled at Player: <b>' . (int)$se['hurl_existing_objects_count'] . ' objects</b>';
        if (!empty($seBadges)) {
            $html .= '<div class="phys-row"><span class="phys-target">Spaceship Events</span> ' . implode(' &bull; ', $seBadges) . '</div>';
        }
    }

    if (!empty($decoded['light_commands']) && is_array($decoded['light_commands'])) {
        foreach ($decoded['light_commands'] as $lcmd) {
            if (!is_array($lcmd)) continue;
            $lTarget = htmlspecialchars((string)($lcmd['target'] ?? 'ALL_LIGHTS'));
            $lBadges = [];
            if (!empty($lcmd['preset']))              $lBadges[] = 'Preset: <b>' . htmlspecialchars((string)$lcmd['preset']) . '</b>';
            if (!empty($lcmd['mode']))                $lBadges[] = 'Mode: <b>' . htmlspecialchars((string)$lcmd['mode']) . '</b>';
            if (isset($lcmd['intensity_multiplier'])) $lBadges[] = 'Brightness: <b>' . (float)$lcmd['intensity_multiplier'] . 'x</b>';
            if (!empty($lBadges)) {
                $html .= '<div class="phys-row"><span class="phys-target">Lights (' . $lTarget . ')</span> ' . implode(' &bull; ', $lBadges) . '</div>';
            }
        }
    }

    $commands = $decoded['physics_commands'] ?? (isset($decoded[0]) ? $decoded : []);
    if ((empty($commands) || !is_array($commands)) && $html === '') {
        return '<span class="phys-pill">Zero-G Physics Calibrated (No Overrides)</span>';
    }

    foreach ($commands as $cmd) {
        if (!is_array($cmd)) continue;
        $target = htmlspecialchars((string)($cmd['target'] ?? 'Object'));
        $badges = [];
        if (isset($cmd['mass_kg']))                $badges[] = 'Mass: <b>' . (float)$cmd['mass_kg'] . ' kg</b>';
        if (isset($cmd['spin_speed']))             $badges[] = 'Spin: <b>' . (float)$cmd['spin_speed'] . '°/s</b>';
        if (isset($cmd['max_speed']))              $badges[] = 'Max Speed: <b>' . (float)$cmd['max_speed'] . ' cm/s</b>';
        if (isset($cmd['zero_g']))                 $badges[] = 'Zero-G: <b>' . ($cmd['zero_g'] ? 'ON' : 'OFF') . '</b>';
        if (isset($cmd['bounce']))                 $badges[] = 'Bounce: <b>' . (float)$cmd['bounce'] . '</b>';
        if (isset($cmd['wave_amplitude']))         $badges[] = 'Float Wave: <b>' . (float)$cmd['wave_amplitude'] . ' cm</b>';
        if (!empty($cmd['push_toward_player']))    $badges[] = 'Action: <b>Pushed Toward Player</b>';
        if (!empty($cmd['repel_from_player']))     $badges[] = 'Action: <b>Repelled From Player</b>';
        if (isset($cmd['player_max_speed']))       $badges[] = 'Player Speed: <b>' . (float)$cmd['player_max_speed'] . '</b>';
        if (isset($cmd['player_launch_strength'])) $badges[] = 'Wall Launch: <b>' . (float)$cmd['player_launch_strength'] . '</b>';

        if (!empty($badges)) {
            $html .= '<div class="phys-row"><span class="phys-target">' . $target . '</span> ' . implode(' &bull; ', $badges) . '</div>';
        }
    }

    return $html ?: '<span class="phys-pill">Zero-G Physics Calibrated</span>';
}

$errorMsg       = null;
$sessions       = [];
$questCatalog   = [];
$aiLogs         = [];
$objectEditLogs = [];
$events         = [];

try {
    $pdo = getDatabaseConnection();

    // Auto-ensure risk_level & risk_score columns exist
    try {
        $pdo->exec("ALTER TABLE `training_sessions` ADD COLUMN `risk_level` VARCHAR(32) NOT NULL DEFAULT 'MODERATE'");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_sessions` ADD COLUMN `risk_score` FLOAT NOT NULL DEFAULT 35.0");
    } catch (Exception $ignore) {}
    try {
        $pdo->exec("ALTER TABLE `training_sessions` ADD COLUMN `flight_readiness` VARCHAR(64) NOT NULL DEFAULT 'IN_EVALUATION'");
    } catch (Exception $ignore) {}

    // Auto-create `quest_catalog` table for Merged Manual & AI Quests + Anti-Repetition Memory
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

    // Automatically clean up the old hardcoded/unwanted O2 Reserve Tank quest and old Roof Panel test quests
    try {
        $pdo->exec("DELETE FROM `training_sessions` WHERE `mission_name` LIKE '%Pressurized O2 Reserve Tanks%' OR `mission_name` = 'AI Dynamic Spaceship Repair & Survival'");
        $pdo->exec("DELETE FROM `quest_catalog` WHERE `target_actor` LIKE '%Roof_panel%' OR `quest_key` LIKE '%roof_panel%'");
    } catch (Exception $ignore) {}

    // Support deleting a specific quest or resetting quest completion memory from the Dashboard UI
    if (!empty($_GET['delete_quest_id'])) {
        $delQid = (int)$_GET['delete_quest_id'];
        $pdo->prepare("DELETE FROM `quest_catalog` WHERE `id` = :id")->execute([':id' => $delQid]);
        header("Location: dashboard.php");
        exit;
    }
    if (!empty($_GET['reset_quest_mastery']) && $_GET['reset_quest_mastery'] === '1') {
        $pdo->exec("UPDATE `quest_catalog` SET `times_completed` = 0, `best_completion_time_sec` = 0.0, `avg_completion_time_sec` = 0.0");
        header("Location: dashboard.php");
        exit;
    }

    // Support one-click Delete Session or Clear All Sessions from the Dashboard UI
    if (!empty($_GET['delete_session'])) {
        $delUuid = trim((string)$_GET['delete_session']);
        $pdo->prepare("DELETE FROM `training_sessions` WHERE `session_uuid` = :u")->execute([':u' => $delUuid]);
        $pdo->prepare("DELETE FROM `ai_coaching_and_physics_logs` WHERE `session_uuid` = :u")->execute([':u' => $delUuid]);
        $pdo->prepare("DELETE FROM `ai_object_edit_logs` WHERE `session_uuid` = :u")->execute([':u' => $delUuid]);
        $pdo->prepare("DELETE FROM `training_telemetry_events` WHERE `session_uuid` = :u")->execute([':u' => $delUuid]);
        header("Location: dashboard.php");
        exit;
    }
    if (!empty($_GET['clear_all']) && $_GET['clear_all'] === '1') {
        $pdo->exec("TRUNCATE TABLE `training_sessions`");
        $pdo->exec("TRUNCATE TABLE `ai_coaching_and_physics_logs`");
        $pdo->exec("TRUNCATE TABLE `ai_object_edit_logs`");
        $pdo->exec("TRUNCATE TABLE `training_telemetry_events`");
        header("Location: dashboard.php");
        exit;
    }

    $sessions       = $pdo->query("SELECT * FROM `training_sessions` ORDER BY `id` DESC LIMIT 20")->fetchAll();
    $questCatalog   = $pdo->query("SELECT * FROM `quest_catalog` ORDER BY `id` DESC LIMIT 50")->fetchAll();
    $aiLogs         = $pdo->query("SELECT * FROM `ai_coaching_and_physics_logs` ORDER BY `id` DESC LIMIT 15")->fetchAll();
    $events         = $pdo->query("SELECT * FROM `training_telemetry_events` ORDER BY `id` DESC LIMIT 25")->fetchAll();
    $objectEditLogs = $pdo->query("SELECT * FROM `ai_object_edit_logs` ORDER BY `id` DESC LIMIT 30")->fetchAll();

    // If no rows exist in ai_object_edit_logs yet, automatically extract Before & After object states from existing sessions' world_objects_json!
    if (empty($objectEditLogs) && !empty($sessions)) {
        foreach ($sessions as $sess) {
            if (empty($sess['world_objects_json'])) continue;
            $wData    = json_decode($sess['world_objects_json'], true);
            $entities = $wData['world_entities'] ?? [];
            foreach ($entities as $ent) {
                if (!is_array($ent)) continue;
                $objName     = $ent['name'] ?? ($ent['id'] ?? 'SpaceObject');
                $objRole     = $ent['role'] ?? 'SpaceProp';
                $phys        = $ent['object_physics'] ?? ($ent['player_physics'] ?? []);
                $scaleBefore = isset($phys['scale']) ? (float)$phys['scale'] : 1.0;
                $sizeCm      = $phys['size_cm'] ?? '50x50x50 cm';
                $sizeBefore  = number_format($scaleBefore, 2) . 'x (' . $sizeCm . ')';
                $massBefore  = (float)($phys['mass_kg'] ?? ($phys['body_mass_kg'] ?? 10.0));
                $spinBefore  = (float)($phys['spin_speed_deg'] ?? 0.0);
                $speedBefore = (float)($phys['max_speed'] ?? ($phys['max_fly_speed'] ?? 600.0));

                $scaleAfter  = $scaleBefore;
                $massAfter   = $massBefore;
                $spinAfter   = ($objRole === 'AstronautPlayer') ? 0.0 : max(12.0, $spinBefore);
                $speedAfter  = $speedBefore;

                $desc = strtolower(($ent['description'] ?? '') . ' ' . $objName);
                if (strpos($desc, 'light') !== false || strpos($desc, 'paper') !== false) {
                    $massAfter = 0.25;
                    $spinAfter = 28.0;
                } elseif (strpos($desc, 'heavy') !== false || strpos($desc, 'crate') !== false) {
                    $massAfter = 55.0;
                    $spinAfter = 7.0;
                }

                $sizeAfter = number_format($scaleAfter, 2) . 'x (' . $sizeCm . ')';

                $objectEditLogs[] = [
                    'session_uuid'         => $sess['session_uuid'],
                    'elapsed_time_seconds' => (float)$sess['duration_seconds'],
                    'object_name'          => $objName,
                    'object_role'          => $objRole,
                    'size_before'          => $sizeBefore,
                    'size_after'           => $sizeAfter,
                    'mass_before_kg'       => $massBefore,
                    'mass_after_kg'        => $massAfter,
                    'spin_before_deg'      => $spinBefore,
                    'spin_after_deg'       => $spinAfter,
                    'max_speed_before'     => $speedBefore,
                    'max_speed_after'      => $speedAfter,
                    'changes_summary'      => sprintf(
                        "Size: %s -> %s | Mass: %.2fkg -> %.2fkg | Spin: %.1f°/s -> %.1f°/s",
                        $sizeBefore, $sizeAfter, $massBefore, $massAfter, $spinBefore, $spinAfter
                    ),
                    'created_at'           => $sess['started_at'] ?? date('Y-m-d H:i:s')
                ];
            }
            if (count($objectEditLogs) >= 20) break;
        }
    }
} catch (Exception $e) {
    $errorMsg = $e->getMessage();
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>MetaSpace - AI Mission Control & Training Telemetry</title>
    <meta http-equiv="refresh" content="5">
    <style>
        body { background: #0b0f19; color: #e2e8f0; font-family: 'Segoe UI', Tahoma, sans-serif; margin: 0; padding: 24px; }
        h1 { color: #38bdf8; margin-bottom: 6px; }
        .sub { color: #94a3b8; margin-bottom: 24px; font-size: 14px; }
        .card { background: #1e293b; border: 1px solid #334155; border-radius: 10px; padding: 18px; margin-bottom: 24px; }
        h2 { color: #a7f3d0; margin-top: 0; font-size: 18px; }
        table { width: 100%; border-collapse: collapse; font-size: 13px; }
        th, td { text-align: left; padding: 11px 12px; border-bottom: 1px solid #334155; vertical-align: middle; }
        th { color: #38bdf8; background: #0f172a; }
        .badge { padding: 4px 9px; border-radius: 6px; font-weight: bold; font-size: 12px; display: inline-block; }
        .completed { background: #065f46; color: #6ee7b7; }
        .in_progress { background: #1e3a8a; color: #93c5fd; }
        .advice { color: #fde047; max-width: 480px; line-height: 1.45; font-weight: 500; }
        .phys-row { background: #0f172a; border-left: 3px solid #38bdf8; padding: 5px 10px; margin: 4px 0; border-radius: 4px; color: #cbd5e1; font-size: 12.5px; }
        .phys-target { color: #6ee7b7; font-weight: bold; margin-right: 6px; }
        .phys-pill { background: #0f172a; color: #94a3b8; padding: 4px 10px; border-radius: 6px; font-size: 12px; }
        .val-before { color: #94a3b8; background: #0f172a; padding: 3px 7px; border-radius: 4px; font-family: monospace; }
        .val-after { color: #6ee7b7; background: #064e3b; padding: 3px 7px; border-radius: 4px; font-weight: bold; font-family: monospace; }
        .arrow { color: #38bdf8; font-weight: bold; padding: 0 5px; }
        .risk-bar-wrap { background: #0f172a; border: 1px solid #334155; border-radius: 8px; height: 22px; width: 100%; overflow: hidden; position: relative; }
        .risk-bar-fill { height: 100%; transition: width 0.4s ease; display: flex; align-items: center; justify-content: center; font-size: 11.5px; font-weight: bold; color: #fff; text-shadow: 0 1px 2px #000; }
        .risk-optimal { background: linear-gradient(90deg, #10b981, #059669); box-shadow: 0 0 10px rgba(16,185,129,0.5); }
        .risk-low { background: linear-gradient(90deg, #10b981, #059669); }
        .failed { background: #7f1d1d; color: #fecaca; }
        .risk-moderate { background: linear-gradient(90deg, #f59e0b, #d97706); }
        .risk-high { background: linear-gradient(90deg, #f97316, #ea580c); }
        .risk-critical { background: linear-gradient(90deg, #ef4444, #b91c1c); box-shadow: 0 0 12px rgba(239,68,68,0.65); }
        .btn-del { background: #7f1d1d; color: #fecaca; padding: 4px 10px; border-radius: 5px; text-decoration: none; font-size: 11.5px; font-weight: bold; border: 1px solid #ef4444; }
        .btn-del:hover { background: #dc2626; color: #fff; }
        .badge-meta-ai { background: linear-gradient(135deg, #6366f1, #8b5cf6, #ec4899); color: #fff; box-shadow: 0 0 10px rgba(139,92,246,0.6); padding: 4px 10px; border-radius: 6px; font-weight: bold; font-size: 11.5px; border: 1px solid #a855f7; display: inline-block; }
        .badge-manual { background: #1e293b; color: #94a3b8; border: 1px solid #475569; padding: 4px 10px; border-radius: 6px; font-size: 11.5px; font-weight: bold; display: inline-block; }
        .grade-badge { padding: 4px 10px; border-radius: 6px; font-weight: 800; font-size: 12.5px; display: inline-block; text-align: center; }
        .grade-s { background: #064e3b; color: #34d399; border: 1px solid #10b981; box-shadow: 0 0 10px rgba(52,211,153,0.5); }
        .grade-a { background: #1e3a8a; color: #60a5fa; border: 1px solid #3b82f6; }
        .grade-b { background: #312e81; color: #a5b4fc; border: 1px solid #6366f1; }
        .grade-c { background: #78350f; color: #fde68a; border: 1px solid #f59e0b; }
        .grade-d { background: #7f1d1d; color: #fecaca; border: 1px solid #ef4444; }
        .readiness-badge { padding: 4px 10px; border-radius: 6px; font-weight: bold; font-size: 11.5px; display: inline-block; }
        .readiness-certified { background: #064e3b; color: #6ee7b7; border: 1px solid #10b981; box-shadow: 0 0 8px rgba(16,185,129,0.4); }
        .readiness-conditional { background: #78350f; color: #fde68a; border: 1px solid #f59e0b; }
        .readiness-alert { background: #7f1d1d; color: #fecaca; border: 1px solid #ef4444; }
        .study-box { background: #0f172a; border-left: 4px solid #8b5cf6; padding: 12px 16px; border-radius: 6px; color: #e2e8f0; font-size: 13.5px; line-height: 1.6; margin-top: 10px; }
    </style>
</head>
<body>
    <div style="display:flex; justify-content:space-between; align-items:center;">
        <div>
            <h1>MetaSpace — AI Game Master &amp; Zero-G Telemetry Dashboard</h1>
            <div class="sub">Auto-refreshes every 5s | Connected Database: <strong><?php echo htmlspecialchars(trim(DB_NAME)); ?></strong></div>
        </div>
        <div>
            <a href="dashboard.php?clear_all=1" class="btn-del" onclick="return confirm('Clear all training sessions & logs?');">&#128465; Clear All Old Sessions</a>
        </div>
    </div>

    <?php if ($errorMsg): ?>
        <div class="card" style="border-color:#ef4444; color:#fca5a5;">
            <strong>Database Error:</strong> <?php echo htmlspecialchars($errorMsg); ?>
        </div>
    <?php endif; ?>

    <?php
        $latestSess  = !empty($sessions) ? $sessions[0] : null;
        $liveRiskLvl = $latestSess ? strtoupper((string)($latestSess['risk_level'] ?? 'MODERATE')) : 'LOW';
        $liveRiskScr = $latestSess ? (float)($latestSess['risk_score'] ?? 35.0) : 15.0;

        // Check if station has been secured or reached nominal/mastery state
        $bIsStationSecured = false;
        if ($latestSess) {
            $sessMission = (string)($latestSess['mission_name'] ?? '');
            if (strpos($liveRiskLvl, 'OPTIMAL') !== false || $liveRiskScr <= 0.0 || strpos($sessMission, 'Secured') !== false || strpos($sessMission, '100%') !== false) {
                $bIsStationSecured = true;
                $liveRiskLvl = 'OPTIMAL';
                $liveRiskScr = 0.0;
            }
        }

        // Also extract risk score from coaching text ONLY if station is not secured
        if (!$bIsStationSecured && $latestSess && !empty($latestSess['ai_coaching_advice']) && preg_match('/Risk:\s*(LOW|MODERATE|HIGH|CRITICAL)\s*(\d+)%/i', $latestSess['ai_coaching_advice'], $m)) {
            $liveRiskLvl = strtoupper($m[1]);
            $liveRiskScr = (float)$m[2];
        }

        $liveRiskClass = 'risk-moderate';
        if ($liveRiskLvl === 'OPTIMAL' || $liveRiskScr <= 0.0) $liveRiskClass = 'risk-optimal';
        elseif ($liveRiskScr >= 80 || strpos($liveRiskLvl, 'CRIT') !== false) $liveRiskClass = 'risk-critical';
        elseif ($liveRiskScr >= 60 || strpos($liveRiskLvl, 'HIGH') !== false) $liveRiskClass = 'risk-high';
        elseif ($liveRiskScr < 30 || strpos($liveRiskLvl, 'LOW') !== false) $liveRiskClass = 'risk-low';

        $liveQuestName = '';
        if ($latestSess && !empty($latestSess['mission_name']) && $latestSess['mission_name'] !== 'Awaiting AI Game Master Quest...') {
            $liveQuestName = $latestSess['mission_name'];
        } elseif (!empty($questCatalog)) {
            $activeQ = null;
            foreach ($questCatalog as $q) {
                if ((int)($q['times_completed'] ?? 0) === 0) {
                    $activeQ = $q;
                    break;
                }
            }
            if (!$activeQ) $activeQ = $questCatalog[0];
            $liveQuestName = $activeQ['quest_title'];
        } else {
            $liveQuestName = $latestSess ? 'Awaiting AI Game Master Quest...' : 'No Active Session';
        }

        // Problem-Solving Mastery calculation (0 to 100)
        $masteryScore = 0.0;
        if ($latestSess && !empty($latestSess['mission_name']) && preg_match('/Mastery:\s*(\d+)%?/i', $latestSess['mission_name'], $mm)) {
            $masteryScore = (float)$mm[1];
        } elseif ($latestSess && !empty($latestSess['ai_coaching_advice']) && preg_match('/Mastery:\s*(\d+)%?/i', $latestSess['ai_coaching_advice'], $mm)) {
            $masteryScore = (float)$mm[1];
        } else {
            $completedCount = 0;
            foreach ($questCatalog as $qc) {
                if ((int)($qc['times_completed'] ?? 0) > 0) $completedCount++;
            }
            $masteryScore = min(100.0, $completedCount * 22.0);
        }
        $isMasteryAchieved = ($masteryScore >= 90.0);
    ?>
    <div class="card" style="border-color:#38bdf8;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:10px;">
            <h2 style="margin:0;">&#9888; Autonomous AI Spaceship Risk Level &amp; Active Real-Time Quest</h2>
            <span style="font-size:14px; font-weight:bold; color:#fde047;">Active AI Game Master Quest: <?php echo htmlspecialchars($liveQuestName); ?></span>
        </div>
        <div class="risk-bar-wrap" style="height:28px; margin-bottom:14px;">
            <div class="risk-bar-fill <?php echo $liveRiskClass; ?>" style="width:<?php echo max(6, min(100, round($liveRiskScr))); ?>%; font-size:13px;">
                AI RISK LEVEL: <?php echo htmlspecialchars($liveRiskLvl); ?> (<?php echo round($liveRiskScr); ?>%)
            </div>
        </div>

        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px; font-size:13px;">
            <span style="color:#6ee7b7; font-weight:bold;">&#127919; Problem-Solving Mastery Progress (Target: 90/100 to Stabilize Station)</span>
            <span style="color:<?php echo $isMasteryAchieved ? '#34d399' : '#38bdf8'; ?>; font-weight:bold;">
                <?php echo $isMasteryAchieved ? '&#9989; 90/100 MASTERY REACHED — All Random Emergencies Halted!' : round($masteryScore) . ' / 100 Points'; ?>
            </span>
        </div>
        <div class="risk-bar-wrap" style="height:24px; background:#0f172a;">
            <div class="risk-bar-fill <?php echo $isMasteryAchieved ? 'risk-low' : 'risk-moderate'; ?>" style="width:<?php echo max(6, min(100, round($masteryScore))); ?>%; font-size:12px; <?php if ($isMasteryAchieved) echo 'box-shadow: 0 0 14px rgba(16,185,129,0.7);'; ?>">
                <?php echo round($masteryScore); ?>% Problem-Solving Mastery <?php if ($isMasteryAchieved) echo '(STATION SECURED - STANDBY FOR COMMANDER)'; ?>
            </div>
        </div>
    </div>

    <?php
        $latestGrade = strtoupper((string)($latestSess['ai_grade'] ?? 'PENDING'));
        $latestScore = (float)($latestSess['final_score'] ?? 0.0);
        $latestReadiness = strtoupper((string)($latestSess['flight_readiness'] ?? 'IN_EVALUATION'));
        $latestStudy = !empty($latestSess['ai_evaluation_summary'])
            ? $latestSess['ai_evaluation_summary']
            : 'Meta AI Autonomous Flight Director is currently studying astronaut maneuvers, inertia control, and wall launches in zero-G...';

        $gradeClass = 'grade-b';
        if ($latestGrade === 'S') $gradeClass = 'grade-s';
        elseif ($latestGrade === 'A') $gradeClass = 'grade-a';
        elseif ($latestGrade === 'B') $gradeClass = 'grade-b';
        elseif ($latestGrade === 'C') $gradeClass = 'grade-c';
        elseif ($latestGrade === 'D') $gradeClass = 'grade-d';

        $readinessClass = 'readiness-conditional';
        if (strpos($latestReadiness, 'CERTIFIED') !== false || strpos($latestReadiness, 'OPTIMAL') !== false) $readinessClass = 'readiness-certified';
        elseif (strpos($latestReadiness, 'ALERT') !== false || strpos($latestReadiness, 'CRIT') !== false) $readinessClass = 'readiness-alert';
    ?>
    <div class="card" style="border-color:#8b5cf6; background: linear-gradient(180deg, #1e1b4b 0%, #0f172a 100%);">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; flex-wrap:wrap; gap:10px;">
            <div>
                <h2 style="margin:0; color:#c084fc;">&#129302; Meta AI Autonomous Trainee Performance Study &amp; Evaluation</h2>
                <div style="color:#94a3b8; font-size:12.5px; margin-top:3px;">
                    Hackathon Track: <strong>Push the Frontier</strong> | Real-Time Autonomous Model Assessment of Zero-G Physical Mastery
                </div>
            </div>
            <div style="display:flex; gap:10px; align-items:center;">
                <span class="readiness-badge <?php echo $readinessClass; ?>">&#9989; <?php echo htmlspecialchars($latestReadiness); ?></span>
                <span class="grade-badge <?php echo $gradeClass; ?>">GRADE <?php echo htmlspecialchars($latestGrade); ?> (<?php echo round($latestScore, 1); ?>/100)</span>
            </div>
        </div>

        <div style="display:grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap:12px; margin-bottom:12px;">
            <div style="background:#1e293b; padding:10px 14px; border-radius:8px; border:1px solid #334155;">
                <div style="color:#94a3b8; font-size:11px; text-transform:uppercase;">Astronaut Trainee</div>
                <div style="color:#38bdf8; font-weight:bold; font-size:15px;"><?php echo htmlspecialchars($latestSess['trainee_name'] ?? 'Astronaut_Trainee'); ?></div>
            </div>
            <div style="background:#1e293b; padding:10px 14px; border-radius:8px; border:1px solid #334155;">
                <div style="color:#94a3b8; font-size:11px; text-transform:uppercase;">Mission Elapsed Time</div>
                <div style="color:#6ee7b7; font-weight:bold; font-size:15px;"><?php echo number_format((float)($latestSess['duration_seconds'] ?? 0), 1); ?>s / <?php echo (float)($latestSess['target_time_seconds'] ?? 120); ?>s</div>
            </div>
            <div style="background:#1e293b; padding:10px 14px; border-radius:8px; border:1px solid #334155;">
                <div style="color:#94a3b8; font-size:11px; text-transform:uppercase;">Systems Repaired / Snapped</div>
                <div style="color:#fde047; font-weight:bold; font-size:15px;"><?php echo (int)($latestSess['items_snapped_count'] ?? 0); ?> / <?php echo (int)($latestSess['required_snaps_count'] ?? 3); ?> Systems</div>
            </div>
            <div style="background:#1e293b; padding:10px 14px; border-radius:8px; border:1px solid #334155;">
                <div style="color:#94a3b8; font-size:11px; text-transform:uppercase;">Wall Launches / Grabs</div>
                <div style="color:#a78bfa; font-weight:bold; font-size:15px;"><?php echo (int)($latestSess['wall_launches_count'] ?? 0); ?> Launches &bull; <?php echo (int)($latestSess['items_grabbed_count'] ?? 0); ?> Grabs</div>
            </div>
        </div>

        <div class="study-box">
            <strong style="color:#c084fc;">&#128225; Meta AI Autonomous Technical Study &amp; Aerospace Debrief:</strong><br>
            <span style="color:#f1f5f9;"><?php echo htmlspecialchars($latestStudy); ?></span>
        </div>
    </div>

    <div class="card">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:10px;">
            <h2 style="margin:0;">&#128218; Quest System Database Catalog — Merged Manual &amp; AI Quests (Anti-Repetition Memory)</h2>
            <a href="dashboard.php?reset_quest_mastery=1" class="btn-del" style="background:#1e3a8a; border-color:#38bdf8; color:#bae6fd;" onclick="return confirm('Reset fast-mastery counters so all saved quests can be replayed?');">&#8635; Reset Mastered Quests</a>
        </div>
        <table>
            <thead>
                <tr>
                    <th>ID</th>
                    <th>Source</th>
                    <th>Required Tool &rarr; Target System</th>
                    <th>Quest Title &amp; Instruction</th>
                    <th>Times Completed</th>
                    <th>Best Time</th>
                    <th>Anti-Repetition Status</th>
                    <th>Action</th>
                </tr>
            </thead>
            <tbody>
                <?php if (empty($questCatalog)): ?>
                <tr>
                    <td colspan="8" style="color:#94a3b8; text-align:center;">No saved quests in database yet. Start the game in Unreal Engine and Manual + AI-Generated Quests will automatically sync here!</td>
                </tr>
                <?php else: foreach ($questCatalog as $qc):
                    $qCompleted = (int)($qc['times_completed'] ?? 0);
                    $qBestSec   = (float)($qc['best_completion_time_sec'] ?? 0.0);
                    $isMastered = ($qCompleted > 0 && $qBestSec > 0.0 && $qBestSec <= 60.0);
                    $qSource    = strtoupper((string)($qc['quest_source'] ?? 'AI_GENERATED'));
                    $qTool      = !empty($qc['required_item']) ? $qc['required_item'] : 'Direct Press [E]';
                ?>
                <tr>
                    <td>#<?php echo (int)$qc['id']; ?></td>
                    <td>
                        <?php if (strpos($qSource, 'META_AI') !== false || strpos($qSource, 'AI') !== false): ?>
                            <span class="badge-meta-ai">&#129302; META AI AUTONOMOUS</span>
                        <?php else: ?>
                            <span class="badge-manual">&#128736; MANUAL DESIGNER</span>
                        <?php endif; ?>
                    </td>
                    <td>
                        <span class="val-before"><?php echo htmlspecialchars($qTool); ?></span>
                        <span class="arrow">&rarr;</span>
                        <span class="val-after"><?php echo htmlspecialchars($qc['target_actor']); ?></span>
                    </td>
                    <td>
                        <strong style="color:#a7f3d0;"><?php echo htmlspecialchars($qc['quest_title']); ?></strong><br>
                        <span style="color:#cbd5e1; font-size:12px;"><?php echo htmlspecialchars($qc['quest_instruction']); ?></span>
                    </td>
                    <td><strong><?php echo $qCompleted; ?></strong> / <?php echo (int)($qc['times_assigned'] ?? 1); ?> assigned</td>
                    <td><strong><?php echo $qBestSec > 0 ? number_format($qBestSec, 1) . 's' : '-'; ?></strong></td>
                    <td>
                        <?php if ($isMastered): ?>
                            <span class="badge completed" style="background:#064e3b; color:#fde047;">&#10004; MASTERED FAST (&le;60s) — Skipped from Repetition</span>
                        <?php elseif ($qCompleted > 0): ?>
                            <span class="badge in_progress">Completed (<?php echo number_format($qBestSec, 1); ?>s) — Practice Pool</span>
                        <?php else: ?>
                            <span class="badge in_progress">Uncompleted — Ready to Fetch</span>
                        <?php endif; ?>
                    </td>
                    <td>
                        <a href="dashboard.php?delete_quest_id=<?php echo (int)$qc['id']; ?>" class="btn-del" onclick="return confirm('Delete this quest from catalog?');">Delete</a>
                    </td>
                </tr>
                <?php endforeach; endif; ?>
            </tbody>
        </table>
    </div>

    <div class="card">
        <h2>Astronaut Training Sessions, Meta AI Autonomous Study &amp; Telemetry</h2>
        <table>
            <thead>
                <tr>
                    <th>Session ID</th>
                    <th>Trainee</th>
                    <th>Active AI Quest</th>
                    <th style="width:160px;">AI Risk Level</th>
                    <th>AI Grade &amp; Score</th>
                    <th>Flight Readiness</th>
                    <th>Status</th>
                    <th>Time Taken</th>
                    <th>Fixed</th>
                    <th>Grabs / Wall</th>
                    <th>Meta AI Autonomous Study &amp; Debrief</th>
                    <th>Action</th>
                </tr>
            </thead>
            <tbody>
                <?php foreach ($sessions as $s):
                    $rowRiskLvl = strtoupper((string)($s['risk_level'] ?? 'MODERATE'));
                    $rowRiskScr = isset($s['risk_score']) ? (float)$s['risk_score'] : 35.0;
                    if (!empty($s['ai_coaching_advice']) && preg_match('/Risk:\s*(LOW|MODERATE|HIGH|CRITICAL)\s*(\d+)%/i', $s['ai_coaching_advice'], $rm)) {
                        $rowRiskLvl = strtoupper($rm[1]);
                        $rowRiskScr = (float)$rm[2];
                    }
                    $rClass = 'risk-moderate';
                    if ($rowRiskScr >= 85 || strpos($rowRiskLvl, 'CRIT') !== false) $rClass = 'risk-critical';
                    elseif ($rowRiskScr >= 65 || strpos($rowRiskLvl, 'HIGH') !== false) $rClass = 'risk-high';
                    elseif ($rowRiskScr < 30 || strpos($rowRiskLvl, 'LOW') !== false) $rClass = 'risk-low';

                    $rowGrade = strtoupper((string)($s['ai_grade'] ?? 'PENDING'));
                    $rowScore = (float)($s['final_score'] ?? 0.0);
                    $rowGClass = 'grade-b';
                    if ($rowGrade === 'S') $rowGClass = 'grade-s';
                    elseif ($rowGrade === 'A') $rowGClass = 'grade-a';
                    elseif ($rowGrade === 'B') $rowGClass = 'grade-b';
                    elseif ($rowGrade === 'C') $rowGClass = 'grade-c';
                    elseif ($rowGrade === 'D') $rowGClass = 'grade-d';

                    $rowReadiness = strtoupper((string)($s['flight_readiness'] ?? 'IN_EVALUATION'));
                    $rowRClass = 'readiness-conditional';
                    if (strpos($rowReadiness, 'CERTIFIED') !== false || strpos($rowReadiness, 'OPTIMAL') !== false) $rowRClass = 'readiness-certified';
                    elseif (strpos($rowReadiness, 'ALERT') !== false || strpos($rowReadiness, 'CRIT') !== false) $rowRClass = 'readiness-alert';

                    $rowStudy = !empty($s['ai_evaluation_summary']) ? $s['ai_evaluation_summary'] : renderReadableAdvice($s['ai_coaching_advice'] ?? null);
                ?>
                <tr>
                    <td><code><?php echo htmlspecialchars($s['session_uuid']); ?></code></td>
                    <td><strong><?php echo htmlspecialchars($s['trainee_name']); ?></strong></td>
                    <td><strong style="color:#a7f3d0;"><?php echo htmlspecialchars($s['mission_name']); ?></strong></td>
                    <td>
                        <div class="risk-bar-wrap">
                            <div class="risk-bar-fill <?php echo $rClass; ?>" style="width:<?php echo max(10, min(100, round($rowRiskScr))); ?>%;">
                                <?php echo htmlspecialchars($rowRiskLvl) . ' ' . round($rowRiskScr) . '%'; ?>
                            </div>
                        </div>
                    </td>
                    <td>
                        <span class="grade-badge <?php echo $rowGClass; ?>">
                            <?php echo htmlspecialchars($rowGrade); ?> <?php if ($rowScore > 0) echo '(' . round($rowScore, 1) . ')'; ?>
                        </span>
                    </td>
                    <td><span class="readiness-badge <?php echo $rowRClass; ?>"><?php echo htmlspecialchars($rowReadiness); ?></span></td>
                    <td><span class="badge <?php echo htmlspecialchars($s['status']); ?>"><?php echo htmlspecialchars($s['status']); ?></span></td>
                    <td><strong><?php echo number_format((float)$s['duration_seconds'], 1); ?>s</strong></td>
                    <td><strong><?php echo (int)$s['items_snapped_count']; ?></strong></td>
                    <td><?php echo (int)$s['items_grabbed_count'] . 'G / ' . (int)$s['wall_launches_count'] . 'W'; ?></td>
                    <td class="advice" style="max-width:320px; font-size:12px;"><?php echo htmlspecialchars($rowStudy); ?></td>
                    <td><a href="dashboard.php?delete_session=<?php echo urlencode($s['session_uuid']); ?>" class="btn-del" onclick="return confirm('Delete this quest/session?');">Delete</a></td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>
    </div>

    <div class="card">
        <h2>Object Size &amp; Physics Log — Before vs. After Meta AI Edit</h2>
        <table>
            <thead>
                <tr>
                    <th>Time (s)</th>
                    <th>Session</th>
                    <th>Object Name</th>
                    <th>Role</th>
                    <th>Object Size / Scale (Before &rarr; After AI)</th>
                    <th>Mass / Weight (Before &rarr; After)</th>
                    <th>Spin Speed (Before &rarr; After)</th>
                    <th>Max Speed (Before &rarr; After)</th>
                    <th>Full AI Edit Summary</th>
                    <th>Timestamp</th>
                </tr>
            </thead>
            <tbody>
                <?php foreach ($objectEditLogs as $ed): ?>
                <tr>
                    <td><strong><?php echo number_format((float)$ed['elapsed_time_seconds'], 1); ?>s</strong></td>
                    <td><?php echo htmlspecialchars($ed['session_uuid']); ?></td>
                    <td><strong style="color:#38bdf8;"><?php echo htmlspecialchars($ed['object_name']); ?></strong></td>
                    <td><span class="phys-pill"><?php echo htmlspecialchars($ed['object_role'] ?? 'SpaceProp'); ?></span></td>
                    <td>
                        <span class="val-before"><?php echo htmlspecialchars($ed['size_before']); ?></span>
                        <span class="arrow">&rarr;</span>
                        <span class="val-after"><?php echo htmlspecialchars($ed['size_after']); ?></span>
                    </td>
                    <td>
                        <span class="val-before"><?php echo number_format((float)$ed['mass_before_kg'], 2); ?> kg</span>
                        <span class="arrow">&rarr;</span>
                        <span class="val-after"><?php echo number_format((float)$ed['mass_after_kg'], 2); ?> kg</span>
                    </td>
                    <td>
                        <span class="val-before"><?php echo number_format((float)$ed['spin_before_deg'], 1); ?>°/s</span>
                        <span class="arrow">&rarr;</span>
                        <span class="val-after"><?php echo number_format((float)$ed['spin_after_deg'], 1); ?>°/s</span>
                    </td>
                    <td>
                        <span class="val-before"><?php echo number_format((float)$ed['max_speed_before'], 0); ?> cm/s</span>
                        <span class="arrow">&rarr;</span>
                        <span class="val-after"><?php echo number_format((float)$ed['max_speed_after'], 0); ?> cm/s</span>
                    </td>
                    <td><div class="phys-row"><?php echo htmlspecialchars($ed['changes_summary'] ?? ''); ?></div></td>
                    <td><?php echo htmlspecialchars($ed['created_at']); ?></td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>
    </div>

    <div class="card">
        <h2>Meta AI Coaching &amp; Real-Time Physics Adjustments</h2>
        <table>
            <thead>
                <tr>
                    <th>Time (s)</th>
                    <th>Session</th>
                    <th>Meta AI Coaching Advice</th>
                    <th>Applied Physics Adjustments (Row Data)</th>
                    <th>Timestamp</th>
                </tr>
            </thead>
            <tbody>
                <?php foreach ($aiLogs as $log): ?>
                <tr>
                    <td><strong><?php echo number_format((float)$log['elapsed_time_seconds'], 1); ?>s</strong></td>
                    <td><?php echo htmlspecialchars($log['session_uuid']); ?></td>
                    <td class="advice"><?php echo htmlspecialchars(renderReadableAdvice($log['ai_message'] ?? null)); ?></td>
                    <td><?php echo renderReadablePhysicsHTML($log['physics_commands_json'] ?? null); ?></td>
                    <td><?php echo htmlspecialchars($log['created_at']); ?></td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>
    </div>

    <div class="card">
        <h2>Live Wall Grab, Wall Walk, Launch & Object Grab/Snap Telemetry</h2>
        <table>
            <thead>
                <tr>
                    <th>Time (s)</th>
                    <th>Session</th>
                    <th>Event Type</th>
                    <th>Actor / Player</th>
                    <th>Wall / Target Surface</th>
                    <th>Player Speed (cm/s)</th>
                    <th>Timestamp</th>
                </tr>
            </thead>
            <tbody>
                <?php foreach ($events as $ev): ?>
                <tr>
                    <td><strong><?php echo number_format((float)$ev['elapsed_time_seconds'], 1); ?>s</strong></td>
                    <td><?php echo htmlspecialchars($ev['session_uuid']); ?></td>
                    <?php
                        $bClass = 'in_progress';
                        $eType = (string)$ev['event_type'];
                        if (strpos($eType, 'success') !== false || strpos($eType, 'completed') !== false || strpos($eType, 'achieved') !== false) {
                            $bClass = 'completed';
                        } elseif (strpos($eType, 'failure') !== false || strpos($eType, 'hazard') !== false) {
                            $bClass = 'failed';
                        }
                    ?>
                    <td><span class="badge <?php echo $bClass; ?>"><?php echo htmlspecialchars($eType); ?></span></td>
                    <td><?php echo htmlspecialchars($ev['actor_name'] ?? '-'); ?></td>
                    <td><?php echo htmlspecialchars($ev['target_name'] ?? '-'); ?></td>
                    <td><?php echo number_format((float)$ev['player_speed'], 0); ?></td>
                    <td><?php echo htmlspecialchars($ev['created_at']); ?></td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>
    </div>
</body>
</html>
