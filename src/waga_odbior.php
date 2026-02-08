<?php header("Content-Type: application/json"); 
// ====== DB ====== 
$host = "localhost"; 
$db = "srv92298_wagi"; 
$user = "srv92298_wagi"; 
// lub twój prawidłowy user 
$pass = "gFePbTno7qkI"; 
try { $conn = new mysqli($host, $user, $pass, $db); 
if ($conn->connect_error) { throw new Exception($conn->connect_error); } } catch (Exception $e) { http_response_code(500); exit(json_encode(["error" => "DB", "msg" => $e->getMessage()])); } // ====== ODBIERZ DANE ====== 
$rawInput = file_get_contents("php://input"); // Debug - zapisz co przyszło 
file_put_contents("/tmp/debug_input.txt", date("Y-m-d H:i:s") . "\n" . "Content-Type: " . ($_SERVER['CONTENT_TYPE'] ?? 'brak') . "\n" . "Content-Length: " . ($_SERVER['CONTENT_LENGTH'] ?? 'brak') . "\n" . "Raw input: " . $rawInput . "\n\n", FILE_APPEND ); // Spróbuj różnych metod odczytu 
if (empty($rawInput)) { $rawInput = file_get_contents('php://input'); } if (empty($rawInput) && isset($HTTP_RAW_POST_DATA)) { $rawInput = $HTTP_RAW_POST_DATA; } $data = json_decode($rawInput, true); if (!$data) { http_response_code(400); exit(json_encode([ "error" => "Invalid JSON", "received_length" => strlen($rawInput), "json_error" => json_last_error_msg() ])); } if (!isset($data['gateway_id']) || !isset($data['measurements'])) { http_response_code(400); exit(json_encode(["error" => "Missing fields"])); } $gateway = $conn->real_escape_string($data['gateway_id']); $measurements = $data['measurements']; 
// ====== GATEWAY ====== 
$conn->query("INSERT INTO gateways (gateway_uid, created_at) VALUES ('$gateway', NOW()) ON DUPLICATE KEY UPDATE gateway_uid = gateway_uid"); $res = $conn->query("SELECT id FROM gateways WHERE gateway_uid='$gateway'"); if (!$res || $res->num_rows == 0) { http_response_code(500); exit(json_encode(["error" => "Gateway insert failed"])); } $gateway_id = $res->fetch_assoc()['id']; 
// ====== MEASUREMENTS ====== 
$saved = 0; foreach ($measurements as $m) { if (!isset($m['device_id']) || !isset($m['weight']) || !isset($m['battery'])) { continue; } $device = $conn->real_escape_string($m['device_id']); $weight = floatval($m['weight']); $battery = floatval($m['battery']); 
// DEVICE 
$conn->query("INSERT INTO devices (device_uid, gateway_id, created_at) VALUES ('$device', $gateway_id, NOW()) ON DUPLICATE KEY UPDATE gateway_id = $gateway_id"); $res = $conn->query("SELECT id FROM devices WHERE device_uid='$device'"); if (!$res || $res->num_rows == 0) continue; $device_id = $res->fetch_assoc()['id']; 
// MEASUREMENT 
$result = $conn->query("INSERT INTO measurements (device_id, weight, battery, measured_at, created_at) VALUES ($device_id, $weight, $battery, NOW(), NOW())"); if ($result) $saved++; } echo json_encode(["status" => "OK", "saved" => $saved, "gateway_id" => $gateway]);