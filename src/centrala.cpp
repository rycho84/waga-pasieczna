#define TINY_GSM_MODEM_SIM800

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <TinyGsmClient.h>
#include <Wire.h>
#include "RTClib.h"
#include <LittleFS.h>

// ================== PARAMETRY ==================
#define FIRMWARE_VERSION      "2.0-ESPNOW"
#define MAX_SCALES_TOTAL      10
#define GPRS_MAX_RETRIES      3
#define GPRS_RETRY_DELAY_MS   10000UL

// ================== PINY TTGO T-CALL ==================
#define MODEM_RST        5
#define MODEM_PWRKEY     4
#define MODEM_POWER_ON   23
#define MODEM_TX         27
#define MODEM_RX         26

// ================== LED ==================
#define LED_PIN          25
#define LED_BLINK_MS     150

// ================== RTC ==================
#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22
RTC_DS3231 rtc;

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
const char GATEWAY_ID[] = "CENTRALA_01";

// ================== BATERIA CENTRALI ==================
#define CENTRAL_BAT_ADC_PIN 35
float centralBatteryVoltage = 0.0;

// ================== STRUKTURY ==================
// ★ Musi być identyczna jak w waga.cpp!
typedef struct {
    float waga;
    float bateria;
    char  device_id[20];
} DaneWagi;

struct ScaleData {
    String device_id;
    float  weight;
    float  battery;
    bool   received;
};

ScaleData scales[MAX_SCALES_TOTAL];
int       scaleCount = 0;

// ================== ESP-NOW ==================
volatile bool noweDatane = false;
DaneWagi      odebraneData;

// ★ Stara sygnatura dla espressif32@3.2.0
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    memcpy(&odebraneData, data, sizeof(odebraneData));
    noweDatane = true;

    Serial.println("\n╔═══════════════════════════════════╗");
    Serial.println("║  📥 ODEBRANO DANE OD WAGI        ║");
    Serial.println("╠═══════════════════════════════════╣");
    Serial.printf( "║  ID:          %s\n",   odebraneData.device_id);
    Serial.printf( "║  Waga:        %.2f kg\n", odebraneData.waga);
    Serial.printf( "║  Bateria:     %.2f V\n",  odebraneData.bateria);
    Serial.printf( "║  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.println("╚═══════════════════════════════════╝\n");
}

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
// ================== LOGOWANIE LittleFS ==================
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
            if (files[j] > files[j+1]) {
                String t = files[j]; files[j] = files[j+1]; files[j+1] = t;
            }

    while (fileCount >= LOG_MAX_FILES) {
        Serial.println("🗑 Usuwam stary log: " + files[0]);
        LittleFS.remove(files[0]);
        for (int i = 0; i < fileCount-1; i++) files[i] = files[i+1];
        fileCount--;
    }
}

void log_init() {
    if (!LittleFS.begin(true)) {
        Serial.println(F("❌ LittleFS: błąd!"));
        logReady = false; return;
    }
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
    if (!logFile) {
        Serial.println("❌ Błąd otwarcia logu");
        logReady = false; return;
    }
    logReady = true;
    Serial.println("📝 Log: " + logFilePath);
    Serial.printf("💾 LittleFS: %u / %u B\n",
                  (unsigned)LittleFS.usedBytes(),
                  (unsigned)LittleFS.totalBytes());
}

void log_close() {
    if (logFile) { logFile.flush(); logFile.close(); }
}

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
    if (!rtc.begin()) {
        logRaw(F("❌ DS3231 ERROR!"));
        while(1) delay(10);
    }
    if (rtc.lostPower()) {
        logMsg(F("⚠️ Ustawiam czas RTC..."));
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    rtc.disableAlarm(1); rtc.disableAlarm(2);
    rtc.clearAlarm(1);   rtc.clearAlarm(2);
    logMsg(F("✅ DS3231 OK"));
}

// ================== BATERIA CENTRALI ==================
float readCentralBattery() {
    analogReadResolution(12);
    analogSetPinAttenuation(CENTRAL_BAT_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(CENTRAL_BAT_ADC_PIN);
        delayMicroseconds(100);
    }
    float voltage = (sum / 16.0 / 4095.0) * 3.3 * 2.0;
    logf("🔋 Bateria centrali: %.2f V", voltage);
    return voltage;
}

// ================== GPRS ==================
bool gprsConnected = false;

bool initGPRS() {
    if (gprsConnected) return true;
    SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(3000);
    pinMode(MODEM_POWER_ON, OUTPUT);
    pinMode(MODEM_PWRKEY,   OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);
    digitalWrite(MODEM_PWRKEY,   HIGH);
    delay(1000);
    digitalWrite(MODEM_PWRKEY, LOW);

    if (!modem.restart())                  { logMsg(F("❌ GPRS: restart nieudany"));  return false; }
    if (!modem.waitForNetwork(60000))      { logMsg(F("❌ GPRS: brak sieci"));        return false; }
    if (!modem.gprsConnect(APN,USER,PASS)) { logMsg(F("❌ GPRS: błąd APN"));          return false; }

    logMsg(F("✅ GPRS połączony"));
    gprsConnected = true;
    return true;
}

void disconnectGPRS() {
    if (!gprsConnected) return;
    gsmClient.stop();
    modem.gprsDisconnect();
    delay(500);
    modem.poweroff();
    digitalWrite(MODEM_POWER_ON, LOW);
    gprsConnected = false;
    logMsg(F("📴 GPRS rozłączony"));
}

// ================== JSON ==================
String buildJson() {
    String json = "{";
    json += "\"gateway_id\":\""       + String(GATEWAY_ID)        + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION)   + "\",";
    json += "\"gateway_battery\":"    + String(centralBatteryVoltage, 2) + ",";
    json += "\"timestamp\":\""        + nowStr()                   + "\",";
    json += "\"measurements\":[";

    bool first = true;
    for (int i = 0; i < scaleCount; i++) {
        if (!scales[i].received) continue;
        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"device_id\":\"" + scales[i].device_id        + "\",";
        json += "\"weight\":"      + String(scales[i].weight, 2) + ",";
        json += "\"battery\":"     + String(scales[i].battery, 2);
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
            gsmClient.print(F("Content-Length: "));
            gsmClient.println(payload.length());
            gsmClient.println();
            gsmClient.print(payload);

            unsigned long t = millis();
            while (gsmClient.connected() && !gsmClient.available())
                if (millis() - t > 5000) break;

            String response = gsmClient.readString();
            gsmClient.stop();

            logMsg("🔍 Odpowiedź HTTP: " + response.substring(0, 300));

            if (response.indexOf("200 OK") >= 0) {
                logMsg(F("✅ Dane wysłane"));
                return true;
            }
            logMsg(F("⚠️ Zła odpowiedź serwera"));
        }
        if (attempt < GPRS_MAX_RETRIES) {
            logf("⏳ Czekam %lu s...", GPRS_RETRY_DELAY_MS / 1000);
            delay(GPRS_RETRY_DELAY_MS);
        }
    }
    logMsg(F("❌ Wysyłanie nieudane"));
    return false;
}

// ================== SETUP ==================
void setup() {
    Serial.begin(115200);
    delay(2000);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    log_init();
    rtc_init();

    logRaw(F("\n╔════════════════════════════════╗"));
    logRaw(F("║  🐝 CENTRALA PASIECZNA        ║"));
    logRaw(F("║  Firmware: " FIRMWARE_VERSION "           ║"));
    logRaw(F("╚════════════════════════════════╝"));
    logf("📅 Start: %s  (%.1f°C)  Boot #%u",
         nowStr().c_str(), rtc.getTemperature(), bootNumber);

    centralBatteryVoltage = readCentralBattery();

    // ================== ESP-NOW INIT ==================
    WiFi.mode(WIFI_STA);
    Serial.print("📍 MAC centrali: ");
    Serial.println(WiFi.macAddress());
    Serial.println("▶ Ten adres wpisz w kodzie wagi!\n");

    if (esp_now_init() != ESP_OK) {
        logMsg(F("❌ Błąd ESP-NOW init!"));
        return;
    }
    logMsg(F("✅ ESP-NOW zainicjalizowany"));

    esp_now_register_recv_cb(onDataRecv);
    logMsg(F("✅ Nasłuchuję na dane z wag..."));

    // ================== GŁÓWNA PĘTLA CYKLU ==================
    while (true) {
        scaleCount = 0;
        noweDatane = false;
        blinkLED(1);

        logMsg(F("⏳ Czekam na dane z wag (60s)..."));
        unsigned long startWait = millis();

        // Zbieraj dane przez 60 sekund
        while (millis() - startWait < 60000UL) {
            if (noweDatane) {
                noweDatane = false;

                // Sprawdź czy waga już zarejestrowana
                bool juzMamy = false;
                for (int i = 0; i < scaleCount; i++) {
                    if (scales[i].device_id == String(odebraneData.device_id)) {
                        // Aktualizuj istniejący wpis
                        scales[i].weight   = odebraneData.waga;
                        scales[i].battery  = odebraneData.bateria;
                        scales[i].received = true;
                        juzMamy = true;
                        logf("🔄 Aktualizacja danych: %s = %.2f kg",
                             odebraneData.device_id, odebraneData.waga);
                        break;
                    }
                }

                // Nowa waga
                if (!juzMamy && scaleCount < MAX_SCALES_TOTAL) {
                    scales[scaleCount].device_id = String(odebraneData.device_id);
                    scales[scaleCount].weight    = odebraneData.waga;
                    scales[scaleCount].battery   = odebraneData.bateria;
                    scales[scaleCount].received  = true;
                    logf("✅ Nowa waga [%d]: %s = %.2f kg",
                         scaleCount + 1,
                         odebraneData.device_id,
                         odebraneData.waga);
                    scaleCount++;
                }

                blinkLED(1, 50, 50);
            }
            delay(100);
        }

        // Wyślij jeśli mamy dane
        if (scaleCount > 0) {
            logf("📶 Zebrano dane od %d wag – wysyłam przez GPRS", scaleCount);

            if (initGPRS()) {
                String json = buildJson();
                logRaw("📦 JSON: " + json);
                sendWithRetry(json);
                disconnectGPRS();
            }
        } else {
            logMsg(F("⚠️ Brak danych od wag w tym cyklu"));
        }

        log_close();
        logFile  = LittleFS.open(logFilePath, "a");
        logReady = (bool)logFile;

        blinkLED(2);
        logMsg(F("😴 Przerwa 30s przed kolejnym cyklem"));
        delay(30000);
    }
}

void loop() {}
