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
#define FIRMWARE_VERSION "1.1"
#define SET_RTC_ON_COMPILE false
#define MAX_SCAN_TIME_SEC     60
#define MAX_SCALES_TOTAL      10
#define MAX_SCALES_TO_READ     3

#define GPRS_MAX_RETRIES       3
#define GPRS_RETRY_DELAY_MS    60000UL

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

// ================== HARMONOGRAM ==================
const int SCHEDULE_HOURS[] = {17, 17};      // Godziny: 6:xx i 20:xx
const int SCHEDULE_MINUTES[] = {50, 55};     // Minuty: x:06 i x:05
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
    Serial.println("⏱ Wysłano czas");
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
  json += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";  // Zmieniono nazwę
  json += "\"battery\":" + String(centralBatteryVoltage, 2) + ",";  // Zmieniono nazwę
  json += "\"temp\":" + String(centralTemperature, 2) + ",";  // DODANO
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

  // Inicjalizacja RTC
  rtc_init();

  Serial.println("\n╔════════════════════════════════╗");
  Serial.println("║  🐝 CENTRALA PASIECZNA        ║");
  Serial.print("║  Firmware: ");
  Serial.print(FIRMWARE_VERSION);
  Serial.println("                ║");
  Serial.println("╚════════════════════════════════╝");
  
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
