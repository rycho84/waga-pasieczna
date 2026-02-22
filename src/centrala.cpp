#define TINY_GSM_MODEM_SIM800
#define SIM800L_IP5306_VERSION_20200811
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <TinyGsmClient.h>
#include <Wire.h>
#include "RTClib.h"
#include <LittleFS.h>
#include "utilities.h"

// ================== PARAMETRY ==================
#define FIRMWARE_VERSION      "2.3-DEEPSLEEP"
#define MAX_SCALES_TOTAL      10
#define GPRS_MAX_RETRIES      3
#define GPRS_RETRY_DELAY_MS   10000UL

// ================== CYKL SNU ==================
#define LISTEN_SEC   20   // czas nasłuchu ESP-NOW [s]
#define SLEEP_SEC    40   // czas deep sleep [s]

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

// ================== MAGICZNE BAJTY ==================
#define MAGIC_DATA_WAGI     0x11
#define MAGIC_DISCOVERY     0xBB
#define MAGIC_DISC_RESPONSE 0xCC
#define MAGIC_TIME_SYNC     0xAA

// ================== STRUKTURY ==================
typedef struct {
    uint8_t  magic;
    float    waga;
    float    bateria;
    char     device_id[20];
    // ── Dane dryfu RTC ──────────────────────────────────
    int32_t  drift_ppm_avg;
    int32_t  drift_ppm_last;
    uint32_t drift_syncs;
    uint32_t drift_boots_since_eve;
    int32_t  drift_min;
    int32_t  drift_max;
} DaneWagi;

typedef struct {
    uint8_t magic;
    char    device_id[20];
} DiscoveryPacket;

typedef struct {
    uint8_t magic;
    char    gateway_id[20];
} DiscoveryResponse;

typedef struct {
    uint8_t  magic;
    uint32_t epoch;
} TimeSyncPacket;

// ================== PAMIĘĆ RTC (przeżywa deep sleep) ==================
RTC_DATA_ATTR static uint32_t bootCount = 0;

// ================== ZMIENNE GLOBALNE ==================
volatile bool noweDatane = false;
DaneWagi      odebraneData;
uint8_t       lastSenderMAC[6];
float         centralBatteryVoltage = 0.0;

// Dane wag zebrane w bieżącym cyklu nasłuchu
struct ScaleData {
    String   device_id;
    float    weight;
    float    battery;
    bool     received;
    // ── Dane dryfu ──────────────────────────────────────
    int32_t  drift_ppm_avg;
    int32_t  drift_ppm_last;
    uint32_t drift_syncs;
    uint32_t drift_boots_since_eve;
    int32_t  drift_min;
    int32_t  drift_max;
};
ScaleData scales[MAX_SCALES_TOTAL];
int       scaleCount = 0;

// ================== FORWARD DECLARATIONS ==================
void logMsg(const String& msg);
void logf(const char* fmt, ...);
void logRaw(const String& msg);

// ============================================================
// ================== WYSYŁANIE DO WAGI ==================
// ============================================================
bool sendToScale(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) return false;
    }
    return (esp_now_send(mac, data, len) == ESP_OK);
}

void sendDiscoveryResponse(const uint8_t* mac) {
    DiscoveryResponse resp;
    resp.magic = MAGIC_DISC_RESPONSE;
    strncpy(resp.gateway_id, GATEWAY_ID, sizeof(resp.gateway_id));
    if (sendToScale(mac, (uint8_t*)&resp, sizeof(resp))) {
        logf("📡 Discovery response → %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void sendTimeSyncToScale(const uint8_t* mac) {
    TimeSyncPacket tp;
    tp.magic = MAGIC_TIME_SYNC;
    tp.epoch = (uint32_t)rtc.now().unixtime();
    if (sendToScale(mac, (uint8_t*)&tp, sizeof(tp))) {
        logf("🕐 Sync czasu → %02X:%02X:%02X:%02X:%02X:%02X | epoch=%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tp.epoch);
    }
}

// ============================================================
// ================== ESP-NOW CALLBACK ==================
// ============================================================
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t magic = data[0];

    if (magic == MAGIC_DISCOVERY && len == sizeof(DiscoveryPacket)) {
        const DiscoveryPacket* dp = (const DiscoveryPacket*)data;
        Serial.println("\n╔═══════════════════════════════════╗");
        Serial.println("║  🔍 DISCOVERY OD WAGI            ║");
        Serial.printf( "║  ID:  %-28s║\n", dp->device_id);
        Serial.printf( "║  MAC: %02X:%02X:%02X:%02X:%02X:%02X          ║\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.println("╚═══════════════════════════════════╝");
        sendDiscoveryResponse(mac);
        return;
    }

    if (magic == MAGIC_DATA_WAGI && len == sizeof(DaneWagi)) {
        memcpy(&odebraneData, data, sizeof(DaneWagi));
        memcpy(lastSenderMAC, mac, 6);
        noweDatane = true;

        Serial.println("\n╔═══════════════════════════════════╗");
        Serial.println("║  📥 ODEBRANO DANE OD WAGI        ║");
        Serial.println("╠═══════════════════════════════════╣");
        Serial.printf( "║  ID:      %-24s║\n", odebraneData.device_id);
        Serial.printf( "║  Waga:    %.2f kg\n", odebraneData.waga);
        Serial.printf( "║  Bateria: %.2f V\n",  odebraneData.bateria);
        Serial.printf( "║  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.println("╚═══════════════════════════════════╝\n");

        sendTimeSyncToScale(mac);
        return;
    }
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
uint32_t logBootNum  = 0;

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
    String files[LOG_MAX_FILES + 10]; int fc = 0;
    File entry = dir.openNextFile();
    while (entry) {
        String name = String(entry.name()); entry.close();
        if (name.startsWith("boot_") && name.endsWith(".log") && fc < LOG_MAX_FILES + 9)
            files[fc++] = String(LOG_DIR) + "/" + name;
        entry = dir.openNextFile();
    }
    dir.close();
    for (int i = 0; i < fc-1; i++)
        for (int j = 0; j < fc-i-1; j++)
            if (files[j] > files[j+1]) { String t=files[j]; files[j]=files[j+1]; files[j+1]=t; }
    while (fc >= LOG_MAX_FILES) { LittleFS.remove(files[0]); for (int i=0;i<fc-1;i++) files[i]=files[i+1]; fc--; }
}

void log_init() {
    if (!LittleFS.begin(true)) { Serial.println(F("❌ LittleFS błąd!")); logReady=false; return; }
    if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);
    logBootNum = 0;
    if (LittleFS.exists(LOG_BOOT_KEY)) {
        File bf = LittleFS.open(LOG_BOOT_KEY,"r");
        if (bf) { logBootNum=bf.readString().toInt(); bf.close(); }
    }
    logBootNum++;
    File bf = LittleFS.open(LOG_BOOT_KEY,"w");
    if (bf) { bf.print(logBootNum); bf.close(); }
    rotateLogs();
    char fname[40];
    sprintf(fname,"%s/boot_%05u.log",LOG_DIR,logBootNum);
    logFilePath = String(fname);
    logFile = LittleFS.open(logFilePath,"a");  // "a" = append, log żyje przez wiele bootów
    logReady = (bool)logFile;
}

void log_close() { if (logFile) { logFile.flush(); logFile.close(); } }

// ============================================================
// ================== RTC ==================
// ============================================================
String nowStr() {
    DateTime now = rtc.now();
    char buf[20];
    sprintf(buf,"%04d-%02d-%02d %02d:%02d:%02d",
            now.year(),now.month(),now.day(),
            now.hour(),now.minute(),now.second());
    return String(buf);
}

void rtc_init() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!rtc.begin()) { logRaw(F("❌ DS3231 ERROR!")); while(1) delay(10); }
    if (rtc.lostPower()) {
        logMsg(F("⚠️ Ustawiam czas RTC..."));
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    rtc.disableAlarm(1); rtc.disableAlarm(2);
    rtc.clearAlarm(1);   rtc.clearAlarm(2);
}

// ================== BATERIA CENTRALI ==================
float readCentralBattery() {
    analogReadResolution(12);
    analogSetPinAttenuation(CENTRAL_BAT_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += analogRead(CENTRAL_BAT_ADC_PIN); delayMicroseconds(100); }
    return (sum / 16.0 / 4095.0) * 3.3 * 2.0;
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
    if (!modem.waitForNetwork(60000))      { logMsg(F("❌ GPRS: brak sieci")); return false; }
    if (!modem.gprsConnect(APN,USER,PASS)) { logMsg(F("❌ GPRS: błąd APN"));  return false; }
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
String buildJson(const String& timestamp) {
    String json = "{";
    json += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"gateway_battery\":" + String(centralBatteryVoltage, 2) + ",";
    json += "\"timestamp\":\"" + timestamp + "\",";
    json += "\"measurements\":[";
    bool first = true;
    for (int i = 0; i < scaleCount; i++) {
        if (!scales[i].received) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"device_id\":\""  + scales[i].device_id                      + "\"," 
              + "\"weight\":"           + String(scales[i].weight, 2)               + "," 
              + "\"battery\":"          + String(scales[i].battery, 2)              + "," 
              + "\"drift_ppm_avg\":"    + String(scales[i].drift_ppm_avg)           + "," 
              + "\"drift_ppm_last\":"   + String(scales[i].drift_ppm_last)          + "," 
              + "\"drift_syncs\":"      + String(scales[i].drift_syncs)             + "," 
              + "\"drift_boots\":"      + String(scales[i].drift_boots_since_eve)   + "," 
              + "\"drift_min\":"        + String(scales[i].drift_min)               + "," 
              + "\"drift_max\":"        + String(scales[i].drift_max)               + "}";
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
            gsmClient.println();
            gsmClient.print(payload);
            unsigned long t = millis();
            while (gsmClient.connected() && !gsmClient.available())
                if (millis()-t > 5000) break;
            String response = gsmClient.readString();
            gsmClient.stop();
            logMsg("🔍 HTTP: " + response.substring(0, 200));
            if (response.indexOf("200 OK") >= 0) { logMsg(F("✅ Dane wysłane")); return true; }
            logMsg(F("⚠️ Zła odpowiedź serwera"));
        }
        if (attempt < GPRS_MAX_RETRIES) {
            logf("⏳ Czekam %lus...", GPRS_RETRY_DELAY_MS/1000);
            delay(GPRS_RETRY_DELAY_MS);
        }
    }
    logMsg(F("❌ Wysyłanie nieudane"));
    return false;
}

// ============================================================
// ================== DEEP SLEEP ==================
// ============================================================
void goToSleep() {
    log_close();
    logf("💤 Deep sleep %ds", SLEEP_SEC);
    delay(100);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SEC * 1000000ULL);
    esp_deep_sleep_start();
}

// ============================================================
// ================== SETUP ==================
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    setupPMU();
    delay(300);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // RTC i log przy każdym wybudzeniu
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!rtc.begin()) { Serial.println("❌ DS3231 ERROR!"); while(1) delay(10); }
    if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

    log_init();
    bootCount++;

    // Pokaż nagłówek tylko przy pierwszym starcie (nie przy każdym wybudzeniu)
    if (bootCount == 1) {
        logRaw(F("\n╔════════════════════════════════╗"));
        logRaw(F("║  🐝 CENTRALA PASIECZNA        ║"));
        logRaw(F("║  Firmware: " FIRMWARE_VERSION "   ║"));
        logRaw(F("╚════════════════════════════════╝"));
    }

    logf("⏰ Boot #%u | %s", bootCount, nowStr().c_str());

    // ── ESP-NOW INIT ───────────────────────────────────────
    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        logMsg(F("❌ Błąd ESP-NOW init!"));
        goToSleep(); return;
    }
    esp_now_register_recv_cb(onDataRecv);

    // ── NASŁUCH PRZEZ LISTEN_SEC SEKUND ───────────────────
    logf("👂 Nasłuchuję %ds...", LISTEN_SEC);
    blinkLED(1, 50, 50);

    scaleCount = 0;
    noweDatane = false;
    String timestamp = nowStr();
    unsigned long listenStart = millis();

    while (millis() - listenStart < (uint32_t)LISTEN_SEC * 1000) {
        if (noweDatane) {
            noweDatane = false;

            bool juzMamy = false;
            for (int i = 0; i < scaleCount; i++) {
                if (scales[i].device_id == String(odebraneData.device_id)) {
                    scales[i].weight              = odebraneData.waga;
                    scales[i].battery             = odebraneData.bateria;
                    scales[i].received            = true;
                    scales[i].drift_ppm_avg       = odebraneData.drift_ppm_avg;
                    scales[i].drift_ppm_last      = odebraneData.drift_ppm_last;
                    scales[i].drift_syncs         = odebraneData.drift_syncs;
                    scales[i].drift_boots_since_eve = odebraneData.drift_boots_since_eve;
                    scales[i].drift_min           = odebraneData.drift_min;
                    scales[i].drift_max           = odebraneData.drift_max;
                    juzMamy = true;
                    logf("🔄 Aktualizacja: %s = %.2f kg",
                         odebraneData.device_id, odebraneData.waga);
                    break;
                }
            }
            if (!juzMamy && scaleCount < MAX_SCALES_TOTAL) {
                scales[scaleCount].device_id           = String(odebraneData.device_id);
                scales[scaleCount].weight              = odebraneData.waga;
                scales[scaleCount].battery             = odebraneData.bateria;
                scales[scaleCount].received            = true;
                scales[scaleCount].drift_ppm_avg       = odebraneData.drift_ppm_avg;
                scales[scaleCount].drift_ppm_last      = odebraneData.drift_ppm_last;
                scales[scaleCount].drift_syncs         = odebraneData.drift_syncs;
                scales[scaleCount].drift_boots_since_eve = odebraneData.drift_boots_since_eve;
                scales[scaleCount].drift_min           = odebraneData.drift_min;
                scales[scaleCount].drift_max           = odebraneData.drift_max;
                logf("✅ Nowa waga [%d]: %s = %.2f kg",
                     scaleCount + 1, odebraneData.device_id, odebraneData.waga);
                scaleCount++;
            }
            blinkLED(1, 30, 30);
        }
        delay(50);
    }

    // ── WYŚLIJ GPRS JEŚLI MAMY DANE, INACZEJ OD RAZU ŚPIJ ─
    if (scaleCount > 0) {
        logf("📶 Odebrano od %d wag – wysyłam GPRS", scaleCount);
        centralBatteryVoltage = readCentralBattery();
        if (initGPRS()) {
            String json = buildJson(timestamp);
            logRaw("📦 JSON: " + json);
            sendWithRetry(json);
            disconnectGPRS();
        }
    } else {
        logMsg(F("⚠️ Brak danych – idę spać"));
    }

    goToSleep();
}

void loop() {}