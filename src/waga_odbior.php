<?php
// Zmiana: usunieto nieuzywana obsluge telemetrii dryfu z raportow wag.
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
