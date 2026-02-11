<?php
header('Content-Type: application/json; charset=utf-8');

// Konfiguracja bazy danych
$host = 'localhost';
$db   = 'srv92298_wagi';
$user = 'srv92298_wagi';
$pass = 'gFePbTno7qkI'; // Zmień!
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

// Odbierz dane JSON
$input = file_get_contents('php://input');
$data = json_decode($input, true);

if (!$data) {
    http_response_code(400);
    echo json_encode(['status' => 'error', 'message' => 'Invalid JSON']);
    exit;
}

// Walidacja danych
if (!isset($data['gateway_id']) || !isset($data['measurements'])) {
    http_response_code(400);
    echo json_encode(['status' => 'error', 'message' => 'Missing required fields']);
    exit;
}

try {
    $pdo->beginTransaction();
    
    // Znajdź lub utwórz gateway
    $stmt = $pdo->prepare("
        INSERT INTO gateways (gateway_uid, firmware, battery, temp, updated_at) 
        VALUES (:uid, :firmware, :battery, :temp, NOW())
        ON DUPLICATE KEY UPDATE 
            firmware = :firmware, 
            battery = :battery, 
            temp = :temp, 
            updated_at = NOW()
    ");
    
    $stmt->execute([
        ':uid' => $data['gateway_id'],
        ':firmware' => $data['firmware'] ?? null,
        ':battery' => $data['battery'] ?? null,
        ':temp' => $data['temp'] ?? null
    ]);
    
    $gateway_id = $pdo->lastInsertId();
    if ($gateway_id == 0) {
        $gateway_id = $pdo->query("SELECT id FROM gateways WHERE gateway_uid = " . 
                                   $pdo->quote($data['gateway_id']))->fetchColumn();
    }
    
    $timestamp = $data['timestamp'] ?? date('Y-m-d H:i:s');
    
    // Przetwarzaj pomiary
    foreach ($data['measurements'] as $measurement) {
        // Znajdź lub utwórz urządzenie
        $stmt = $pdo->prepare("
            INSERT INTO devices (device_uid, gateway_id) 
            VALUES (:uid, :gateway_id)
            ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)
        ");
        
        $stmt->execute([
            ':uid' => $measurement['device_id'],
            ':gateway_id' => $gateway_id
        ]);
        
        $device_id = $pdo->lastInsertId();
        
        // Zapisz pomiar
        $stmt = $pdo->prepare("
            INSERT INTO measurements (device_id, weight, battery, measured_at) 
            VALUES (:device_id, :weight, :battery, :measured_at)
        ");
        
        $stmt->execute([
            ':device_id' => $device_id,
            ':weight' => $measurement['weight'] ?? null,
            ':battery' => $measurement['battery'] ?? null,
            ':measured_at' => $timestamp
        ]);
        
        // Jeśli są dane dryfu, zaktualizuj je
        if (isset($measurement['drift'])) {
            $drift = $measurement['drift'];
            
            $stmt = $pdo->prepare("
                CALL update_device_drift(
                    :device_uid,
                    :drift_avg,
                    :drift_last,
                    :syncs,
                    :days_wo_sync,
                    :min_drift,
                    :max_drift
                )
            ");
            
            $stmt->execute([
                ':device_uid' => $measurement['device_id'],
                ':drift_avg' => $drift['avg'] ?? 0,
                ':drift_last' => $drift['last'] ?? 0,
                ':syncs' => $drift['syncs'] ?? 0,
                ':days_wo_sync' => $drift['days_wo_sync'] ?? 0,
                ':min_drift' => $drift['min'] ?? 0,
                ':max_drift' => $drift['max'] ?? 0
            ]);
        }
    }
    
    $pdo->commit();
    
    http_response_code(200);
    echo json_encode([
        'status' => 'success', 
        'message' => 'Data saved successfully',
        'count' => count($data['measurements'])
    ]);
    
} catch (\PDOException $e) {
    $pdo->rollBack();
    http_response_code(500);
    echo json_encode(['status' => 'error', 'message' => 'Database error: ' . $e->getMessage()]);
}
?>