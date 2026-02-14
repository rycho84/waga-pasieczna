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
#include <LittleFS.h>

// ================== PARAMETRY ==================
#define FIRMWARE_VERSION      "1.7.4"
#define MAX_SCAN_TIME_SEC     10
#define MAX_SCALES_TOTAL      10
#define MAX_SCALES_TO_READ    3
#define SLEEP_BETWEEN_SEC     30

#define GPRS_MAX_RETRIES      3
#define GPRS_RETRY_DELAY_MS   10000UL

// ================== PINY TTGO T-CALL ==================
#define MODEM_RST        5
#define MODEM_PWRKEY     4
#define MODEM_POWER_ON   23
#define MODEM_TX         27
#define MODEM_RX         26

// ================== LED STATUSU ==================
#define LED_PIN          25
#define LED_BLINK_MS     150

// ================== RTC ==================
#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22
RTC_DS3231 rtc;

// ================== BLE UUID ==================
static BLEUUID serviceUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
static BLEUUID charUUID   ("beb5483e-36e1-4688-b7f5-ea07361b26a8");

// ================== IDENTYFIKACJA ==================
const char GATEWAY_ID[] = "CENTRALA_01";

// ================== GPRS ==================
HardwareSerial SerialAT(1);
TinyGsm        modem(SerialAT);
TinyGsmClient  gsmClient(modem);

const char APN[]    = "internet";
const char USER[]   = "";
const char PASS[]   = "";
const char SERVER[] = "srv92298.seohost.com.pl";
const int  PORT     = 80;
const char PATH[]   = "/waga_odbior.php";

// ================== BATERIA CENTRALI ==================
#define CENTRAL_BAT_ADC_PIN 35
float centralBatteryVoltage = 0.0;

// ================== STRUKTURY ==================
// ★ Dodano pola dryfu
struct DriftData {
    int32_t  avg;
    int32_t  last;
    uint32_t syncs;
    uint32_t boots_since_eve;
    int32_t  min_seen;
    int32_t  max_seen;
    bool     valid;
};

struct ScaleData {
    String    device_id;
    float     weight;
    float     battery;
    DriftData drift;
    bool      received;
};

ScaleData scales[MAX_SCALES_TOTAL];
int scaleCount = 0;

// ================== PAMIĘĆ OBSŁUŻONYCH WAG ==================
#define MAX_HANDLED_SCALES 10
String handledScales[MAX_HANDLED_SCALES];
int    handledCount = 0;

// ================== BLE GLOBAL ==================
BLEScan*   pScan    = nullptr;
BLEClient* pClient  = nullptr;

volatile bool scaleDetected = false;
BLEAddress    detectedAddress("");

// ============================================================
// ================== LED ==================
// ============================================================
void blinkLED(uint8_t count, uint16_t onMs = LED_BLINK_MS, uint16_t offMs = LED_BLINK_MS) {
    for (uint8_t i = 0; i < count; i++) {
        digitalWrite(LED_PIN, HIGH); delay(onMs);
        digitalWrite(LED_PIN, LOW);
        if (i < count - 1) delay(offMs);
    }
}

// ============================================================
// ================== SYSTEM LOGOWANIA LittleFS ==================
// ============================================================
#define LOG_DIR       "/logs"
#define LOG_BOOT_KEY  "/logs/bootcnt"
#define LOG_MAX_FILES 10

File     logFile;
String   logFilePath = "";
bool     logReady    = false;
uint32_t bootNumber  = 0;

void _logWrite(const String& line) {
    if (!logReady || !logFile) return;
    logFile.println(line); logFile.flush();
}

void logMsg(const String& msg) {
    DateTime now = rtc.now();
    char ts[24];
    sprintf(ts, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            now.year(), now.month(), now.day(),
            now.hour(), now.minute(), now.second());
    String line = String(ts) + msg;
    Serial.println(line); _logWrite(line);
}

void logRaw(const String& msg) { Serial.println(msg); _logWrite(msg); }

void logf(const char* fmt, ...) {
    char buf[256];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    logMsg(String(buf));
}

void printAllPreviousLogs() {
    Serial.println(F("\n╔══════════════════════════════════════════════╗"));
    Serial.println(F("║          POPRZEDNIE LOGI STARTOWE           ║"));
    Serial.println(F("╚══════════════════════════════════════════════╝"));

    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) {
        Serial.println(F("  (brak katalogu logów)"));
        return;
    }

    String files[LOG_MAX_FILES + 5]; int fileCount = 0;
    File entry = dir.openNextFile();
    while (entry) {
        String name = String(entry.name()); entry.close();
        if (name.startsWith("boot_") && name.endsWith(".log") &&
            fileCount < (int)(sizeof(files)/sizeof(files[0]))) {
            String fp = String(LOG_DIR) + "/" + name;
            if (fp != logFilePath) files[fileCount++] = fp;
        }
        entry = dir.openNextFile();
    }
    dir.close();

    for (int i = 0; i < fileCount-1; i++)
        for (int j = 0; j < fileCount-i-1; j++)
            if (files[j] > files[j+1]) { String t=files[j]; files[j]=files[j+1]; files[j+1]=t; }

    if (fileCount == 0) Serial.println(F("  (brak poprzednich logów)"));
    for (int i = 0; i < fileCount; i++) {
        Serial.println("\n──────── " + files[i] + " ────────");
        File f = LittleFS.open(files[i], "r");
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n'); line.trim();
                if (line.length() > 0) Serial.println(line);
            }
            f.close();
        }
    }
    Serial.println(F("\n╔══════════════════════════════════════════════╗"));
    Serial.println(F("║           KONIEC POPRZEDNICH LOGÓW          ║"));
    Serial.println(F("╚══════════════════════════════════════════════╝\n"));
}

void rotateLogs() {
    File dir = LittleFS.open(LOG_DIR);
    if (!dir || !dir.isDirectory()) return;
    String files[LOG_MAX_FILES + 10]; int fileCount = 0;
    File entry = dir.openNextFile();
    while (entry) {
        String name = String(entry.name()); entry.close();
        if (name.startsWith("boot_") && name.endsWith(".log") &&
            fileCount < (int)(sizeof(files)/sizeof(files[0])))
            files[fileCount++] = String(LOG_DIR) + "/" + name;
        entry = dir.openNextFile();
    }
    dir.close();

    for (int i = 0; i < fileCount-1; i++)
        for (int j = 0; j < fileCount-i-1; j++)
            if (files[j] > files[j+1]) { String t=files[j]; files[j]=files[j+1]; files[j+1]=t; }

    while (fileCount >= LOG_MAX_FILES) {
        Serial.println("🗑 Usuwam stary log: " + files[0]);
        LittleFS.remove(files[0]);
        for (int i = 0; i < fileCount-1; i++) files[i] = files[i+1];
        fileCount--;
    }
}

void log_init() {
    if (!LittleFS.begin(true)) { Serial.println(F("❌ LittleFS: błąd!")); logReady=false; return; }
    Serial.println(F("✅ LittleFS zamontowany"));
    if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);

    bootNumber = 0;
    if (LittleFS.exists(LOG_BOOT_KEY)) {
        File bf = LittleFS.open(LOG_BOOT_KEY, "r");
        if (bf) { bootNumber = bf.readString().toInt(); bf.close(); }
    }
    bootNumber++;
    File bf = LittleFS.open(LOG_BOOT_KEY, "w");
    if (bf) { bf.print(bootNumber); bf.close(); }

    rotateLogs();
    char fname[40];
    sprintf(fname, "%s/boot_%05u.log", LOG_DIR, bootNumber);
    logFilePath = String(fname);
    logFile = LittleFS.open(logFilePath, "w");
    if (!logFile) { Serial.println("❌ Błąd otwarcia logu"); logReady=false; return; }
    logReady = true;
    Serial.println("📝 Log: " + logFilePath);
    Serial.printf("💾 LittleFS: %u / %u B\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

void log_close() { if (logFile) { logFile.flush(); logFile.close(); } }

// ============================================================
// ================== RTC ==================
// ============================================================
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
    if (!rtc.begin()) { logRaw(F("❌ DS3231 ERROR!")); while(1) delay(10); }
    if (rtc.lostPower()) { logMsg(F("⚠️ Ustawiam czas RTC...")); rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); }
    rtc.disableAlarm(1); rtc.disableAlarm(2);
    rtc.clearAlarm(1);   rtc.clearAlarm(2);
    logMsg(F("✅ DS3231 OK"));
}

// ================== BATERIA CENTRALI ==================
float readCentralBattery() {
    analogReadResolution(12);
    analogSetPinAttenuation(CENTRAL_BAT_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += analogRead(CENTRAL_BAT_ADC_PIN); delayMicroseconds(100); }
    float voltage = (sum / 16.0 / 4095.0) * 3.3 * 2.0;
    logf("🔋 Bateria centrali: %.2f V", voltage);
    return voltage;
}

// ================== MAC HANDLING ==================
bool alreadyHandled(BLEAddress addr) {
    String s = addr.toString().c_str();
    for (int i = 0; i < handledCount; i++) if (handledScales[i] == s) return true;
    return false;
}
void markHandled(BLEAddress addr) {
    if (handledCount < MAX_HANDLED_SCALES) handledScales[handledCount++] = addr.toString().c_str();
}

// ★ Parsuj raport dryfu z formatu "D:avg;last;syncs;boots;min;max"
DriftData parseDrift(const String& driftStr) {
    DriftData d = {0, 0, 0, 0, 0, 0, false};
    if (!driftStr.startsWith("D:")) return d;

    String s = driftStr.substring(2); // usuń "D:"
    int idx = 0;
    long vals[6] = {0};
    int valIdx = 0;

    while (valIdx < 6) {
        int sep = s.indexOf(';', idx);
        String token = (sep < 0) ? s.substring(idx) : s.substring(idx, sep);
        vals[valIdx++] = token.toInt();
        if (sep < 0) break;
        idx = sep + 1;
    }

    if (valIdx == 6) {
        d.avg            = (int32_t)vals[0];
        d.last           = (int32_t)vals[1];
        d.syncs          = (uint32_t)vals[2];
        d.boots_since_eve = (uint32_t)vals[3];
        d.min_seen       = (int32_t)vals[4];
        d.max_seen       = (int32_t)vals[5];
        d.valid          = true;
    }
    return d;
}

// ================== OBSŁUGA WAGI ==================
bool processScale(BLEAddress addr) {
    logMsg("🔗 Łączenie z wagą: " + String(addr.toString().c_str()));

    if (!pClient->connect(addr)) { logMsg(F("❌ Błąd połączenia BLE")); return false; }

    auto srv = pClient->getService(serviceUUID);
    if (!srv) { pClient->disconnect(); logMsg(F("❌ Brak serwisu BLE")); return false; }

    auto ch = srv->getCharacteristic(charUUID);
    if (!ch) { pClient->disconnect(); logMsg(F("❌ Brak charakterystyki BLE")); return false; }

    if (ch->canRead()) {
        String val = ch->readValue().c_str();
        logMsg("📥 Dane surowe: " + val);

        // Format: "weight;battery;D:avg;last;syncs;boots;min;max"
        int sep1 = val.indexOf(';');
        if (sep1 < 0) { pClient->disconnect(); logMsg("❌ Zły format (brak sep1)"); return false; }

        int sep2 = val.indexOf(';', sep1 + 1);
        if (sep2 < 0) { pClient->disconnect(); logMsg("❌ Zły format (brak sep2)"); return false; }

        scales[scaleCount].device_id = addr.toString().c_str();
        scales[scaleCount].weight    = val.substring(0, sep1).toFloat();
        scales[scaleCount].battery   = val.substring(sep1 + 1, sep2).toFloat();
        scales[scaleCount].received  = true;

        // ★ Parsuj dane dryfu (trzecia część)
        String driftPart = val.substring(sep2 + 1);
        scales[scaleCount].drift = parseDrift(driftPart);

        if (scales[scaleCount].drift.valid) {
            logf("📊 Dryf: avg=%d last=%d syncs=%u",
                 scales[scaleCount].drift.avg,
                 scales[scaleCount].drift.last,
                 scales[scaleCount].drift.syncs);
        } else {
            logMsg(F("⚠️ Brak danych dryfu lub błędny format"));
        }
    }

    if (ch->canWrite()) {
        // Wyślij UNIX timestamp (nie string daty!)
        DateTime now = rtc.now();
        uint32_t unixTs = now.unixtime();
        String timeCmd = "TIME:" + String(unixTs);
        ch->writeValue(timeCmd.c_str());
        logf("⏱ Wysłano czas do wagi: %lu", (unsigned long)unixTs);
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
            scaleDetected   = true;
            pScan->stop();
            delay(100);
        }
    }
};

// ★ Bezpieczny start skanowania BLE
void bleScanStart() {
    pScan->stop(); delay(100);
    pScan->clearResults(); delay(100);
    scaleDetected = false;
    pScan->start(MAX_SCAN_TIME_SEC, false);
}

// ================== GPRS ==================
bool gprsConnected = false;

bool initGPRS() {
    if (gprsConnected) return true;
    SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(3000);
    pinMode(MODEM_POWER_ON, OUTPUT); pinMode(MODEM_PWRKEY, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH); digitalWrite(MODEM_PWRKEY, HIGH);
    delay(1000); digitalWrite(MODEM_PWRKEY, LOW);
    if (!modem.restart())               { logMsg(F("❌ GPRS: restart nieudany"));  return false; }
    if (!modem.waitForNetwork(60000))   { logMsg(F("❌ GPRS: brak sieci"));        return false; }
    if (!modem.gprsConnect(APN,USER,PASS)) { logMsg(F("❌ GPRS: błąd APN"));      return false; }
    logMsg(F("✅ GPRS połączony")); gprsConnected = true; return true;
}

void disconnectGPRS() {
    if (!gprsConnected) return;
    gsmClient.stop(); modem.gprsDisconnect(); delay(500);
    modem.poweroff(); digitalWrite(MODEM_POWER_ON, LOW);
    gprsConnected = false;
    logMsg(F("📴 GPRS rozłączony"));
}

// ================== JSON ==================
// ★ Dodano pole "drift" do każdego pomiaru
String buildJson() {
    String json = "{";
    json += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"gateway_battery\":" + String(centralBatteryVoltage, 2) + ",";
    json += "\"timestamp\":\"" + nowStr() + "\",";
    json += "\"measurements\":[";

    bool first = true;
    for (int i = 0; i < scaleCount; i++) {
        if (!scales[i].received) continue;
        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"device_id\":\"" + scales[i].device_id + "\",";
        json += "\"weight\":"  + String(scales[i].weight, 2) + ",";
        json += "\"battery\":" + String(scales[i].battery, 2);

        // ★ Dołącz dane dryfu jeśli dostępne
        if (scales[i].drift.valid) {
            json += ",\"drift\":{";
            json += "\"avg\":"            + String(scales[i].drift.avg)            + ",";
            json += "\"last\":"           + String(scales[i].drift.last)           + ",";
            json += "\"syncs\":"          + String(scales[i].drift.syncs)          + ",";
            json += "\"boots_since_eve\":" + String(scales[i].drift.boots_since_eve) + ",";
            json += "\"min\":"            + String(scales[i].drift.min_seen)       + ",";
            json += "\"max\":"            + String(scales[i].drift.max_seen);
            json += "}";
        }
        json += "}";
    }
    json += "]}";
    return json;
}

// ================== SEND + RETRY ==================
bool sendWithRetry(const String& payload) {
    for (int attempt = 1; attempt <= GPRS_MAX_RETRIES; attempt++) {
        logf("📡 GPRS próba %d/%d", attempt, GPRS_MAX_RETRIES);
        if (!gsmClient.connect(SERVER, PORT)) {
            logMsg(F("❌ Brak połączenia z serwerem"));
        } else {
            gsmClient.println("POST " + String(PATH) + " HTTP/1.1");
            gsmClient.println("Host: " + String(SERVER));
            gsmClient.println(F("Content-Type: application/json"));
            gsmClient.print(F("Content-Length: ")); gsmClient.println(payload.length());
            gsmClient.println(); gsmClient.print(payload);

            unsigned long t = millis();
            while (gsmClient.connected() && !gsmClient.available())
                if (millis() - t > 5000) break;

            String response = gsmClient.readString();
            gsmClient.stop();

            // ★ Loguj pełną odpowiedź HTTP – widać kod błędu i treść PHP
            logMsg("🔍 Odpowiedź HTTP: " + response.substring(0, 300));

            if (response.indexOf("200 OK") >= 0) { logMsg(F("✅ Dane wysłane")); return true; }
            logMsg(F("⚠️ Zła odpowiedź serwera"));
        }
        if (attempt < GPRS_MAX_RETRIES) {
            logf("⏳ Czekam %lu s...", GPRS_RETRY_DELAY_MS / 1000);
            delay(GPRS_RETRY_DELAY_MS);
        }
    }
    logMsg(F("❌ Wysyłanie nieudane")); return false;
}

// ================== RESET DANYCH CYKLU ==================
void resetCycleData() {
    for (int i = 0; i < scaleCount; i++) scales[i] = {};
    scaleCount = 0; handledCount = 0; scaleDetected = false;
}

// ================== JEDEN CYKL PRACY ==================
void runCycle() {
    resetCycleData();
    blinkLED(1);
    logMsg(F("💡 LED: start skanowania BLE"));
    bleScanStart();
    unsigned long scanStart = millis();

    while ((millis() - scanStart) < (MAX_SCAN_TIME_SEC * 1000UL) &&
           scaleCount < MAX_SCALES_TO_READ) {
        if (scaleDetected) {
            scaleDetected = false;
            processScale(detectedAddress);
            if (scaleCount < MAX_SCALES_TO_READ) {
                delay(300);
                pScan->stop(); delay(100);
                pScan->clearResults(); delay(100);
                pScan->start(MAX_SCAN_TIME_SEC, false);
            }
        }
        delay(50);
    }
    pScan->stop(); delay(100); pScan->clearResults();

    if (scaleCount > 0) {
        logf("📶 Znaleziono %d wag – wysyłam", scaleCount);
        if (initGPRS()) {
            String json = buildJson();
            logRaw(json.c_str());
            sendWithRetry(json);
            disconnectGPRS();
        }
    }
}

// ================== SETUP ==================
void setup() {
    Serial.begin(115200);
    delay(2000);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    log_init();
    printAllPreviousLogs();
    rtc_init();

    logRaw(F("\n╔════════════════════════════════╗"));
    logRaw(F("║  🐝 CENTRALA PASIECZNA        ║"));
    logRaw(F("║  Firmware: " FIRMWARE_VERSION "                ║"));
    logRaw(F("╚════════════════════════════════╝"));
    logf("📅 Start: %s  (%.1f°C)  Boot #%u",
         nowStr().c_str(), rtc.getTemperature(), bootNumber);

    centralBatteryVoltage = readCentralBattery();

    BLEDevice::init("Centrala");
    pClient = BLEDevice::createClient();
    pScan   = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);

    while (true) {
        runCycle();
        log_close();
        logFile  = LittleFS.open(logFilePath, "a");
        logReady = (bool)logFile;

        blinkLED(2);
        logMsg(F("💡 LED: wchodzę w uśpienie"));
        delay(SLEEP_BETWEEN_SEC * 1000UL);
    }
}

void loop() {}
