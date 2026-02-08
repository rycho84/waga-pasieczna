<?php
header("Content-Type: application/json");

// ====== DB ======
$host = "localhost";
$db = "srv92298_wagi";
$user = "srv92298_wagi";
$pass = "gFePbTno7qkI";

try {
    $conn = new mysqli($host, $user, $pass, $db);
    if ($conn->connect_error) {
        throw new Exception($conn->connect_error);
    }
} catch (Exception $e) {
    http_response_code(500);
    exit(json_encode(["error" => "DB", "msg" => $e->getMessage()]));
}

// ====== ODBIERZ DANE ======
$rawInput = file_get_contents("php://input");

// Debug
file_put_contents("/tmp/debug_input.txt",
    date("Y-m-d H:i:s") . "\n" .
    "Raw input: " . $rawInput . "\n\n",
    FILE_APPEND
);

$data = json_decode($rawInput, true);

if (!$data) {
    http_response_code(400);
    exit(json_encode([
        "error" => "Invalid JSON",
        "json_error" => json_last_error_msg()
    ]));
}

if (!isset($data['gateway_id']) || !isset($data['measurements'])) {
    http_response_code(400);
    exit(json_encode(["error" => "Missing required fields"]));
}

$gateway = $conn->real_escape_string($data['gateway_id']);
$firmware = isset($data['firmware']) ? $conn->real_escape_string($data['firmware']) : null;
$battery = isset($data['battery']) ? floatval($data['battery']) : null;
$temp = isset($data['temp']) ? floatval($data['temp']) : null;
$measurements = $data['measurements'];

// ====== GATEWAY ======
// Najpierw wstaw/znajdź gateway
$conn->query("INSERT INTO gateways (gateway_uid, created_at) 
              VALUES ('$gateway', NOW()) 
              ON DUPLICATE KEY UPDATE gateway_uid = gateway_uid");

$res = $conn->query("SELECT id FROM gateways WHERE gateway_uid='$gateway'");
if (!$res || $res->num_rows == 0) {
    http_response_code(500);
    exit(json_encode(["error" => "Gateway insert failed"]));
}
$gateway_id = $res->fetch_assoc()['id'];

// Zaktualizuj dane centrali
$updates = [];
if ($firmware !== null) {
    $updates[] = "firmware = '$firmware'";
}
if ($battery !== null) {
    $updates[] = "battery = $battery";
}
if ($temp !== null) {
    $updates[] = "temp = $temp";
}
$updates[] = "updated_at = NOW()";

if (!empty($updates)) {
    $updateSql = "UPDATE gateways SET " . implode(", ", $updates) . " WHERE id = $gateway_id";
    $conn->query($updateSql);
}

// ====== MEASUREMENTS ======
$saved = 0;
foreach ($measurements as $m) {
    if (!isset($m['device_id']) || !isset($m['weight']) || !isset($m['battery'])) {
        continue;
    }
    
    $device = $conn->real_escape_string($m['device_id']);
    $weight = floatval($m['weight']);
    $deviceBattery = floatval($m['battery']);
    
    // DEVICE
    $conn->query("INSERT INTO devices (device_uid, gateway_id, created_at) 
                  VALUES ('$device', $gateway_id, NOW()) 
                  ON DUPLICATE KEY UPDATE gateway_id = $gateway_id");
    
    $res = $conn->query("SELECT id FROM devices WHERE device_uid='$device'");
    if (!$res || $res->num_rows == 0) continue;
    $device_id = $res->fetch_assoc()['id'];
    
    // MEASUREMENT
    $result = $conn->query("INSERT INTO measurements (device_id, weight, battery, measured_at, created_at) 
                           VALUES ($device_id, $weight, $deviceBattery, NOW(), NOW())");
    if ($result) $saved++;
}

// Debug
file_put_contents("/tmp/debug_processed.txt",
    date("Y-m-d H:i:s") . "\n" .
    "Gateway: $gateway\n" .
    "Firmware: " . ($firmware ?? 'NULL') . "\n" .
    "Battery: " . ($battery ?? 'NULL') . "\n" .
    "Temp: " . ($temp ?? 'NULL') . "\n" .
    "Gateway ID: $gateway_id\n\n",
    FILE_APPEND
);

echo json_encode([
    "status" => "OK",
    "saved" => $saved,
    "gateway_id" => $gateway,
    "firmware" => $firmware,
    "battery" => $battery,
    "temp" => $temp
]);