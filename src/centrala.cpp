// ================== MODEM ==================
#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <TinyGsmClient.h>
#include <Wire.h>
#include "RTClib.h"

// ================== PARAMETRY ==================
#define FIRMWARE_VERSION "1.3.0"
#define SET_RTC_ON_COMPILE false  // Zmień na true aby ustawić czas przy kompilacji
#define MAX_SCAN_TIME_SEC     240
#define MAX_SCALES_TOTAL      10
#define MAX_SCALES_TO_READ     1

#define GPRS_MAX_RETRIES       3
#define GPRS_RETRY_DELAY_MS    60000UL

// ================== PINY TTGO T-CALL ==================
#define MODEM_RST        5
#define MODEM_PWRKEY     4
#define MODEM_POWER_ON   23
#define MODEM_TX         27
#define MODEM_RX         26
#define LED_PIN          25 

// ================== RTC ==================
#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22
#define RTC_SQW_PIN 32
RTC_DS3231 rtc;

// ================== HARMONOGRAM ==================
const int SCHEDULE_HOURS[] = {5, 19};      // Godziny: 6:xx i 20:xx
const int SCHEDULE_MINUTES[] = {58, 58};     // Minuty: x:06 i x:05
const int SCHEDULE_COUNT = 2;

// ================== BLE UUID ==================
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charUUID   ("beb5483e-36e1-4688-b7f5-ea07361b26a8");

// ================== IDENTYFIKACJA ==================
const char GATEWAY_ID[] = "CENTRALA_01";

// ================== GPRS ==================
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);

const char APN[]  = "internet";
const char USER[] = "";
const char PASS[] = "";
const char SERVER[] = "srv92298.seohost.com.pl";
const int  PORT = 80;
const char PATH[] = "/waga_odbior.php";

// ================== BATERIA CENTRALI ==================
#define CENTRAL_BAT_ADC_PIN 35  // GPIO35 - ADC dla TTGO T-Call
float centralBatteryVoltage = 0.0;
float centralTemperature = 0.0;  // Temperatura z DS3231

// ================== STRUKTURY ==================
struct DriftData {
  int32_t drift_ppm_avg;
  int32_t drift_ppm_last;
  uint32_t successful_syncs;
  uint32_t days_without_sync;
  int32_t min_drift_seen;
  int32_t max_drift_seen;
  bool has_drift_data;
};

struct ScaleData {
  String device_id;
  float weight;
  float battery;
  DriftData drift;
  bool received;
};

ScaleData scales[MAX_SCALES_TOTAL];
int scaleCount = 0;

// ================== PAMIĘĆ OBSŁUŻONYCH WAG ==================
#define MAX_HANDLED_SCALES 10
String handledScales[MAX_HANDLED_SCALES];
int handledCount = 0;

// ================== BLE GLOBAL ==================
BLEScan* pScan;
BLEClient* pClient;

// ================== FLAGI ==================
volatile bool scaleDetected = false;
BLEAddress detectedAddress("");

// ================== FUNKCJE RTC ==================
String nowStr() {
  DateTime now = rtc.now();
  char buf[20];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  return String(buf);
}

void rtc_init() {
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  
  if (!rtc.begin()) {
    Serial.println("❌ DS3231 ERROR!");
    while (1) delay(10);
  }
  
  #if SET_RTC_ON_COMPILE
    Serial.println("⚙️ Ustawiam czas z kompilacji...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  #else
    if (rtc.lostPower()) {
      Serial.println("⚠️ RTC stracił zasilanie - wymagane ustawienie czasu!");
      Serial.println("⚠️ Ustaw SET_RTC_ON_COMPILE na true i przekompiluj");
    }
  #endif
  
  rtc.disableAlarm(1);
  rtc.disableAlarm(2);
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  
  Serial.println("✅ DS3231 OK");
}

DateTime getNextScheduledTime(DateTime now) {
  int currentHour = now.hour();
  int currentMinute = now.minute();
  
  for (int i = 0; i < SCHEDULE_COUNT; i++) {
    int scheduleHour = SCHEDULE_HOURS[i];
    int scheduleMinute = SCHEDULE_MINUTES[i];
    
    // Sprawdź czy ta godzina jeszcze nie minęła
    if (scheduleHour > currentHour || 
        (scheduleHour == currentHour && scheduleMinute > currentMinute)) {
      return DateTime(now.year(), now.month(), now.day(), 
                     scheduleHour, scheduleMinute, 0);
    }
  }
  
  // Wszystkie dzisiejsze alarmy minęły - ustaw na jutro pierwszy
  DateTime tomorrow = now + TimeSpan(1, 0, 0, 0);
  return DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), 
                 SCHEDULE_HOURS[0], SCHEDULE_MINUTES[0], 0);
}

void rtc_set_alarm() {
  DateTime now = rtc.now();
  DateTime nextAlarm = getNextScheduledTime(now);
  
  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║   NASTĘPNE WYBUDZENIE        ║");
  Serial.println("╚══════════════════════════════╝");
  
  char buffer[20];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  Serial.print("⏰ Teraz:      ");
  Serial.println(buffer);
  
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          nextAlarm.year(), nextAlarm.month(), nextAlarm.day(),
          nextAlarm.hour(), nextAlarm.minute(), nextAlarm.second());
  Serial.print("⏰ Następny:   ");
  Serial.println(buffer);
  Serial.println("╚══════════════════════════════╝\n");
  
  rtc.setAlarm1(nextAlarm, DS3231_A1_Minute);  // Dopasowanie do minuty
  rtc.clearAlarm(1);
}

// ================== BATERIA CENTRALI ==================
float readCentralBattery() {
  analogReadResolution(12);
  analogSetPinAttenuation(CENTRAL_BAT_ADC_PIN, ADC_11db);
  
  uint32_t sum = 0;
  const int samples = 16;
  
  for (int i = 0; i < samples; i++) {
    sum += analogRead(CENTRAL_BAT_ADC_PIN);
    delayMicroseconds(100);
  }
  
  float raw = sum / (float)samples;
  // TTGO T-Call ma dzielnik 1:2, więc napięcie = (raw/4095)*3.3*2
  float voltage = (raw / 4095.0) * 3.3 * 2.0;
  
  Serial.print("🔋 Bateria centrali: ");
  Serial.print(voltage, 2);
  Serial.println(" V");
  
  return voltage;
}

// ================== MAC HANDLING ==================
bool alreadyHandled(BLEAddress addr) {
  String addrStr = addr.toString().c_str();
  for (int i = 0; i < handledCount; i++) {
    if (handledScales[i] == addrStr) return true;
  }
  return false;
}

void markHandled(BLEAddress addr) {
  if (handledCount < MAX_HANDLED_SCALES) {
    handledScales[handledCount++] = addr.toString().c_str();
  }
}

// ================== PARSOWANIE DRYFU ==================
DriftData parseDriftData(String driftStr) {
  DriftData drift;
  drift.has_drift_data = false;
  
  // Format: "D:drift_avg;drift_last;syncs;days_wo_sync;min;max"
  if (!driftStr.startsWith("D:")) {
    return drift;
  }
  
  driftStr = driftStr.substring(2); // Usuń "D:"
  
  int idx = 0;
  String parts[6];
  int partCount = 0;
  
  // Rozdziel po średnikach
  while (driftStr.length() > 0 && partCount < 6) {
    int sep = driftStr.indexOf(';');
    if (sep >= 0) {
      parts[partCount++] = driftStr.substring(0, sep);
      driftStr = driftStr.substring(sep + 1);
    } else {
      parts[partCount++] = driftStr;
      break;
    }
  }
  
  if (partCount >= 6) {
    drift.drift_ppm_avg = parts[0].toInt();
    drift.drift_ppm_last = parts[1].toInt();
    drift.successful_syncs = parts[2].toInt();
    drift.days_without_sync = parts[3].toInt();
    drift.min_drift_seen = parts[4].toInt();
    drift.max_drift_seen = parts[5].toInt();
    drift.has_drift_data = true;
    
    Serial.println("📊 DANE DRYFU:");
    Serial.print("   Avg: ");
    Serial.print(drift.drift_ppm_avg);
    Serial.println(" ppm");
    Serial.print("   Last: ");
    Serial.print(drift.drift_ppm_last);
    Serial.println(" ppm");
    Serial.print("   Syncs: ");
    Serial.println(drift.successful_syncs);
    Serial.print("   Days w/o sync: ");
    Serial.println(drift.days_without_sync);
  }
  
  return drift;
}

// ================== OBSŁUGA WAGI ==================
bool processScale(BLEAddress addr) {
  Serial.print("🔗 Łączenie z wagą: ");
  Serial.println(addr.toString().c_str());

  if (!pClient->connect(addr)) return false;

  auto srv = pClient->getService(serviceUUID);
  if (!srv) { pClient->disconnect(); return false; }

  auto ch = srv->getCharacteristic(charUUID);
  if (!ch) { pClient->disconnect(); return false; }

  if (ch->canRead()) {
    String val = ch->readValue().c_str();
    Serial.print("📥 Dane surowe: ");
    Serial.println(val);
    
    // Format: "masa;napiecie;D:drift_data"
    // lub stary format: "masa;napiecie"
    
    int firstSep = val.indexOf(';');
    if (firstSep < 0) { pClient->disconnect(); return false; }
    
    int secondSep = val.indexOf(';', firstSep + 1);
    
    scales[scaleCount].device_id = addr.toString().c_str();
    scales[scaleCount].weight   = val.substring(0, firstSep).toFloat();
    
    if (secondSep >= 0) {
      // Nowy format z dryfem
      scales[scaleCount].battery  = val.substring(firstSep + 1, secondSep).toFloat();
      String driftStr = val.substring(secondSep + 1);
      scales[scaleCount].drift = parseDriftData(driftStr);
    } else {
      // Stary format bez dryfu
      scales[scaleCount].battery  = val.substring(firstSep + 1).toFloat();
      scales[scaleCount].drift.has_drift_data = false;
    }
    
    scales[scaleCount].received = true;

    Serial.print("✅ Masa: ");
    Serial.print(scales[scaleCount].weight, 2);
    Serial.print(" kg, Bateria: ");
    Serial.print(scales[scaleCount].battery, 2);
    Serial.println(" V");
  }

  if (ch->canWrite()) {
    DateTime now = rtc.now();
    uint32_t timestamp = now.unixtime();
    String timeCmd = "TIME:" + String(timestamp);
    ch->writeValue(timeCmd.c_str());
    Serial.print("⏱ Wysłano czas: ");
    Serial.println(nowStr());
  }

  pClient->disconnect();
  delay(300);

  markHandled(addr);
  scaleCount++;
  return true;
}

// ================== CALLBACK BLE ==================
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (scaleCount >= MAX_SCALES_TO_READ) return;
    if (alreadyHandled(dev.getAddress())) return;

    if (dev.haveServiceUUID() && dev.isAdvertisingService(serviceUUID)) {
      detectedAddress = dev.getAddress();
      scaleDetected = true;
      pScan->stop();
      delay(100);
    }
  }
};

// ================== GPRS INIT ==================
bool initGPRS() {
  Serial.println("📶 Inicjalizacja modemu...");
  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  pinMode(MODEM_POWER_ON, OUTPUT);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_POWER_ON, HIGH);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);

  Serial.println("🔄 Restart modemu...");
  if (!modem.restart()) {
    Serial.println("❌ BŁĄD: modem.restart() nie powiódł się");
    return false;
  }
  Serial.println("✅ Modem OK");

  String modemInfo = modem.getModemInfo();
  Serial.print("ℹ️ Modem info: ");
  Serial.println(modemInfo);

  Serial.println("📶 Czekam na sieć (60s)...");
  if (!modem.waitForNetwork(60000)) {
    Serial.println("❌ BŁĄD: Brak sieci GSM");
    Serial.print("   Stan sieci: ");
    Serial.println(modem.getRegistrationStatus());
    return false;
  }
  Serial.println("✅ Sieć GSM OK");

  Serial.print("📶 Siła sygnału: ");
  Serial.println(modem.getSignalQuality());

  Serial.println("🌐 Łączę z GPRS...");
  Serial.print("   APN: ");
  Serial.println(APN);
  if (!modem.gprsConnect(APN, USER, PASS)) {
    Serial.println("❌ BŁĄD: gprsConnect() nie powiódł się");
    return false;
  }
  Serial.println("✅ GPRS połączony");

  Serial.print("🌐 Lokalny IP: ");
  Serial.println(modem.localIP());

  Serial.println("🔌 Łączę z serwerem...");
  Serial.print("   Serwer: ");
  Serial.print(SERVER);
  Serial.print(":");
  Serial.println(PORT);

  return true;
}

// ================== JSON ==================
String buildJson() {
  String json = "{";
  json += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
  json += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"battery\":" + String(centralBatteryVoltage, 2) + ",";
  json += "\"temp\":" + String(centralTemperature, 2) + ",";
  json += "\"timestamp\":\"" + nowStr() + "\",";
  json += "\"measurements\":[";

  bool first = true;
  for (int i = 0; i < scaleCount; i++) {
    if (!scales[i].received) continue;
    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"device_id\":\"" + scales[i].device_id + "\",";
    json += "\"weight\":" + String(scales[i].weight, 2) + ",";
    json += "\"battery\":" + String(scales[i].battery, 2);
    
    // Dodaj dane dryfu jeśli dostępne
    if (scales[i].drift.has_drift_data) {
      json += ",\"drift\":{";
      json += "\"avg\":" + String(scales[i].drift.drift_ppm_avg) + ",";
      json += "\"last\":" + String(scales[i].drift.drift_ppm_last) + ",";
      json += "\"syncs\":" + String(scales[i].drift.successful_syncs) + ",";
      json += "\"days_wo_sync\":" + String(scales[i].drift.days_without_sync) + ",";
      json += "\"min\":" + String(scales[i].drift.min_drift_seen) + ",";
      json += "\"max\":" + String(scales[i].drift.max_drift_seen);
      json += "}";
    }
    
    json += "}";
  }
  json += "]}";
  return json;
}

// ================== SEND + RETRY ==================
bool sendWithRetry(String payload) {
  for (int attempt = 1; attempt <= GPRS_MAX_RETRIES; attempt++) {
    Serial.printf("📡 GPRS próba %d/%d\n", attempt, GPRS_MAX_RETRIES);

    if (!gsmClient.connect(SERVER, PORT)) {
      Serial.println("❌ Brak połączenia z serwerem");
    } else {
      gsmClient.println("POST " + String(PATH) + " HTTP/1.0");
      gsmClient.println("Host: " + String(SERVER));
      gsmClient.println("Content-Type: application/json");
      gsmClient.println("Connection: close");
      gsmClient.print("Content-Length: ");
      gsmClient.println(payload.length());
      gsmClient.println();
      gsmClient.print(payload);
      gsmClient.flush();

      unsigned long t = millis();
while (gsmClient.connected() && !gsmClient.available()) {
  if (millis() - t > 15000) {
    Serial.println("⏱ Timeout oczekiwania na odpowiedź");
    break;
  }
  delay(100);
}

String statusLine = "";
String response = "";
bool firstLine = true;

unsigned long readStart = millis();
while (gsmClient.available() ||
       (gsmClient.connected() && millis() - readStart < 5000)) {
  if (gsmClient.available()) {
    String line = gsmClient.readStringUntil('\n');
    if (firstLine) {
      statusLine = line;
      firstLine = false;
      Serial.print("📨 Status HTTP: ");
      Serial.println(statusLine);
    }
    response += line;
  }
}

gsmClient.stop();

if (statusLine.indexOf("200") >= 0) {
  Serial.println("✅ Dane wysłane poprawnie");
  return true;
}

Serial.print("⚠️ Zła odpowiedź: ");
Serial.println(statusLine);
Serial.println("Pełna odpowiedź serwera:");
Serial.println(response);
    }

    if (attempt < GPRS_MAX_RETRIES) {
      Serial.println("⏳ Czekam 60 s przed ponowną próbą");
      delay(GPRS_RETRY_DELAY_MS);
    }
  }
  return false;
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(2000);

  // Inicjalizacja RTC
  rtc_init();

  Serial.println("\n╔════════════════════════════════╗");
  Serial.println("║  🐝 CENTRALA PASIECZNA        ║");
  Serial.print("║  Firmware: ");
  Serial.print(FIRMWARE_VERSION);
  Serial.println("                ║");
  Serial.println("╚════════════════════════════════╝");
  
  // Miganie diody LED 3 razy
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
  }

  DateTime now = rtc.now();
  Serial.print("📅 Czas: ");
  Serial.print(nowStr());
  Serial.print(" (");
  Serial.print(rtc.getTemperature());
  Serial.println("°C)");
  
  // Pomiar baterii centrali
  centralBatteryVoltage = readCentralBattery();
  
  // Odczyt temperatury z DS3231
  centralTemperature = rtc.getTemperature();
  Serial.print("🌡️ Temperatura DS3231: ");
  Serial.print(centralTemperature, 2);
  Serial.println("°C");

  // Inicjalizacja BLE
  BLEDevice::init("Centrala");
  pClient = BLEDevice::createClient();

  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);

  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║   SKANOWANIE BLE             ║");
  Serial.println("╚══════════════════════════════╝");

  unsigned long scanStart = millis();
  pScan->start(MAX_SCAN_TIME_SEC, false);

  while ((millis() - scanStart) < (MAX_SCAN_TIME_SEC * 1000UL) &&
         scaleCount < MAX_SCALES_TO_READ) {
    if (scaleDetected) {
      scaleDetected = false;
      processScale(detectedAddress);
      if (scaleCount < MAX_SCALES_TO_READ) {
        delay(300);
        pScan->start(MAX_SCAN_TIME_SEC, false);
      }
    }
    delay(50);
  }

  pScan->stop();

  Serial.print("\n✅ Znaleziono: ");
  Serial.print(scaleCount);
  Serial.println(" wag\n");

  // Wysyłanie przez GPRS
  if (scaleCount > 0 && initGPRS()) {
    String json = buildJson();
    Serial.println("\n📤 JSON do wysłania:");
    Serial.println(json);
    sendWithRetry(json);
    modem.gprsDisconnect();
  }

  // Ustaw alarm na następne skanowanie
  rtc_set_alarm();

  Serial.println("😴 Deep sleep - wybudzenie przez RTC alarm");
  pinMode(RTC_SQW_PIN, INPUT_PULLUP);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_SQW_PIN, LOW);

  delay(2000);
  esp_deep_sleep_start();
}

void loop() {}