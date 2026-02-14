<?php
header('Content-Type: application/json; charset=utf-8');

$host    = 'localhost';
$db      = 'srv92298_wagi';
$user    = 'srv92298_wagi';
$pass    = 'gFePbTno7qkI';
$charset = 'utf8mb4';

$dsn = "mysql:host=$host;dbname=$db;charset=$charset";
$options = [
    PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    PDO::ATTR_EMULATE_PREPARES   => false,
];

try {
    $pdo = new PDO($dsn, $user, $pass, $options);
} catch (\PDOException $e) {
    http_response_code(500);
    echo json_encode(['status' => 'error', 'message' => 'Database connection failed']);
    exit;
}

$input = file_get_contents('php://input');
$data  = json_decode($input, true);

if (!$data) {
    http_response_code(400);
    echo json_encode(['status' => 'error', 'message' => 'Invalid JSON']);
    exit;
}

if (!isset($data['gateway_id']) || !isset($data['measurements'])) {
    http_response_code(400);
    echo json_encode(['status' => 'error', 'message' => 'Missing required fields']);
    exit;
}

try {
    $pdo->beginTransaction();

    // Poprawione nazwy pol z centrali: firmware_version i gateway_battery
  $stmt = $pdo->prepare("
    INSERT INTO gateways (gateway_uid, firmware, battery, updated_at)
    VALUES (:uid, :firmware, :battery, NOW())
    ON DUPLICATE KEY UPDATE
        firmware   = VALUES(firmware),
        battery    = VALUES(battery),
        updated_at = NOW()
");
$stmt->execute([
    ':uid'      => $data['gateway_id'],
    ':firmware' => $data['firmware_version'] ?? null,
    ':battery'  => $data['gateway_battery']  ?? null,
]);

    $gateway_id = $pdo->lastInsertId();
    if ($gateway_id == 0) {
        $gateway_id = $pdo->query(
            "SELECT id FROM gateways WHERE gateway_uid = " . $pdo->quote($data['gateway_id'])
        )->fetchColumn();
    }

    $timestamp = $data['timestamp'] ?? date('Y-m-d H:i:s');

    foreach ($data['measurements'] as $measurement) {

        $stmt = $pdo->prepare("
            INSERT INTO devices (device_uid, gateway_id)
            VALUES (:uid, :gateway_id)
            ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)
        ");
        $stmt->execute([
            ':uid'        => $measurement['device_id'],
            ':gateway_id' => $gateway_id
        ]);
        $device_id = $pdo->lastInsertId();

        $stmt = $pdo->prepare("
            INSERT INTO measurements (device_id, weight, battery, measured_at)
            VALUES (:device_id, :weight, :battery, :measured_at)
        ");
        $stmt->execute([
            ':device_id'   => $device_id,
            ':weight'      => $measurement['weight']  ?? null,
            ':battery'     => $measurement['battery'] ?? null,
            ':measured_at' => $timestamp
        ]);

        // Zapis dryfu do tabeli device_drift_stats
        if (isset($measurement['drift']) && is_array($measurement['drift'])) {
            $drift = $measurement['drift'];
            $syncs = intval($drift['syncs'] ?? 0);

            if ($syncs > 0) {
                $drift_avg  = intval($drift['avg']             ?? 0);
                $drift_last = intval($drift['last']            ?? 0);
                $days_wo    = intval($drift['boots_since_eve'] ?? 0);
                $min_drift  = intval($drift['min']             ?? 0);
                $max_drift  = intval($drift['max']             ?? 0);

                $stmt = $pdo->prepare("
                    INSERT INTO device_drift_stats (
                        device_id, drift_ppm_avg, drift_ppm_last,
                        successful_syncs, days_without_sync,
                        min_drift_seen, max_drift_seen
                    )
                    VALUES (
                        :device_id, :drift_avg, :drift_last,
                        :syncs, :days_wo,
                        :min_drift, :max_drift
                    )
                    ON DUPLICATE KEY UPDATE
    drift_ppm_avg     = VALUES(drift_ppm_avg),
    drift_ppm_last    = VALUES(drift_ppm_last),
    successful_syncs  = VALUES(successful_syncs),
    days_without_sync = VALUES(days_without_sync),
    min_drift_seen    = VALUES(min_drift_seen),
    max_drift_seen    = VALUES(max_drift_seen)
                ");
                $stmt->execute([
                    ':device_id' => $device_id,
                    ':drift_avg' => $drift_avg,
                    ':drift_last'=> $drift_last,
                    ':syncs'     => $syncs,
                    ':days_wo'   => $days_wo,
                    ':min_drift' => $min_drift,
                    ':max_drift' => $max_drift,
                ]);

                // Zaktualizuj drift_quality w tabeli devices
                $abs_avg = abs($drift_avg);
                if      ($abs_avg < 500)  $quality = 'excellent';
                elseif  ($abs_avg < 1000) $quality = 'good';
                elseif  ($abs_avg < 2000) $quality = 'fair';
                else                      $quality = 'poor';

                $pdo->prepare("UPDATE devices SET drift_quality = :q WHERE id = :id")
                    ->execute([':q' => $quality, ':id' => $device_id]);
            }
        }
    }

    $pdo->commit();

    http_response_code(200);
    echo json_encode([
        'status'  => 'success',
        'message' => 'Data saved successfully',
        'count'   => count($data['measurements'])
    ]);

} catch (\PDOException $e) {
    $pdo->rollBack();
    http_response_code(500);
    echo json_encode(['status' => 'error', 'message' => 'DB error: ' . $e->getMessage()]);
}
?>
