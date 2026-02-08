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

// ================== PINY TTGO T-CALL ==================
#define MODEM_RST        5
#define MODEM_PWRKEY     4
#define MODEM_POWER_ON   23
#define MODEM_TX         27
#define MODEM_RX         26

// ================== RTC ==================
#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22
#define RTC_SQW_PIN 32
RTC_DS3231 rtc;

// ================== BLE UUID ==================
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charUUID   ("beb5483e-36e1-4688-b7f5-ea07361b26a8");

// ================== PARAMETRY ==================
#define MAX_SCAN_TIME_SEC     180
#define MAX_SCALES_TOTAL      10
#define MAX_SCALES_TO_READ     3

#define GPRS_MAX_RETRIES       3
#define GPRS_RETRY_DELAY_MS    60000UL

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

// ================== STRUKTURY ==================
struct ScaleData {
  String device_id;
  float weight;
  float battery;
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

// ================== RTC ==================
String nowStr() {
  DateTime now = rtc.now();
  char buf[20];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  return String(buf);
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
    int sep = val.indexOf(';');
    if (sep < 0) { pClient->disconnect(); return false; }

    scales[scaleCount].device_id = addr.toString().c_str();
    scales[scaleCount].weight   = val.substring(0, sep).toFloat();
    scales[scaleCount].battery  = val.substring(sep + 1).toFloat();
    scales[scaleCount].received = true;

    Serial.print("📥 Dane: ");
    Serial.println(val);
  }

  if (ch->canWrite()) {
    String timeCmd = "TIME:" + nowStr();
    ch->writeValue(timeCmd.c_str());
    Serial.print("⏱ Wysłano czas");
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
  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  pinMode(MODEM_POWER_ON, OUTPUT);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_POWER_ON, HIGH);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);

  if (!modem.restart()) return false;
  if (!modem.waitForNetwork(60000)) return false;
  if (!modem.gprsConnect(APN, USER, PASS)) return false;

  return true;
}

// ================== JSON ==================
String buildJson() {
  String json = "{";
  json += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
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
      gsmClient.println("POST " + String(PATH) + " HTTP/1.1");
      gsmClient.println("Host: " + String(SERVER));
      gsmClient.println("Content-Type: application/json");
      gsmClient.print("Content-Length: ");
      gsmClient.println(payload.length());
      gsmClient.println();
      gsmClient.print(payload);

      unsigned long t = millis();
      while (gsmClient.connected() && !gsmClient.available()) {
        if (millis() - t > 5000) break;
      }

      String response = gsmClient.readString();
      gsmClient.stop();

      if (response.indexOf("200 OK") >= 0) {
        Serial.println("✅ Dane wysłane poprawnie");
        return true;
      }

      Serial.println("⚠️ Zła odpowiedź serwera");
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

  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  rtc.begin();

  Serial.println("\n🐝 CENTRALA PASIECZNA v1.4 – GPRS RETRY");

  BLEDevice::init("Centrala");
  pClient = BLEDevice::createClient();

  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);

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

  if (scaleCount > 0 && initGPRS()) {
    String json = buildJson();
    Serial.println(json);
    sendWithRetry(json);
    modem.gprsDisconnect();
  }

  Serial.println("😴 Deep sleep");
  pinMode(RTC_SQW_PIN, INPUT_PULLUP);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_SQW_PIN, LOW);
  esp_deep_sleep_start();
}

void loop() {}
