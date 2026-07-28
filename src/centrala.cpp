#define TINY_GSM_MODEM_SIM800
#define SIM800L_IP5306_VERSION_20200811
// Zmiana: dodano sygnalizacje LED po wyniku wysylki GPRS.
// Zmiana: 3.2 - usunieto telemetrie dryfu, dodano bezpieczne wylaczanie modemu i adaptacyjny nasluch.
// Zmiana: 3.3 - dodano tymczasowa obsluge starych pakietow wag z polami dryfu.
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <TinyGsmClient.h>
#include <Wire.h>
#include "RTClib.h"
#include <LittleFS.h>
#include "utilities.h"

// ================== PARAMETRY ==================
#define FIRMWARE_VERSION      "3.3-RTCWAKE"
#define MAX_SCALES_TOTAL      10
#define EXPECTED_SCALES       1        // zakończ nasłuch gdy zebrano tyle unikalnych wag
#define GPRS_MAX_RETRIES      3
#define GPRS_RETRY_DELAY_MS   10000UL
static const uint32_t GPRS_NETWORK_TIMEOUT_MS = 30000UL;
static const uint8_t  GPRS_MIN_USABLE_CSQ = 5;
static const bool USTAW_ZEGAR = false; // ustaw true, aby przy starcie wymusic zapis czasu kompilacji do DS3231

// ================== CYKL NASŁUCHU ==================
#define LISTEN_SEC            180      // pelne okno nasluchu ESP-NOW [s]
#define LISTEN_SEC_MISS_1     120      // okno po pierwszym pustym cyklu
#define LISTEN_SEC_MISS_MORE  60       // okno po kolejnych pustych cyklach
#define LISTEN_FULL_EVERY_MISSES 4     // co tyle pustych cykli pelne okno odzyskiwania
#define LOG_DUMP_WINDOW_MS    1000UL

// ================== GODZINY WYBUDZENIA (DS3231) ==================
// Dwa alarmy na dobę: 6:00 i 20:00
static const uint8_t WAKE_HOURS[]   = { 6, 20 };
static const uint8_t WAKE_MINUTES[] = { 0, 0 };
static const int     WAKE_COUNT     = 2;

// ================== PIN INT DS3231 → ESP32 ==================
// Podłącz SQW/INT z DS3231 do tego pinu przez rezystor pull-up 10kΩ do 3.3V
// Sygnał aktywny LOW → budzimy ESP32 stanem 0
// RTC_INT_PIN MUSI być GPIO z obsługą ext0: 0,2,4,12-15,25-27,32-39
#define RTC_INT_PIN   32     // <-- zmień jeśli używasz innego GPIO

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
const char PATH[]   = "/waga/waga_odbior.php";
const char GATEWAY_ID[] = "CENTRALA_05";

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
} DaneWagi;

typedef struct {
    uint8_t  magic;
    float    waga;
    float    bateria;
    char     device_id[20];
    int32_t  drift_ppm_avg;
    int32_t  drift_ppm_last;
    uint32_t drift_syncs;
    uint32_t drift_boots_since_eve;
    int32_t  drift_min;
    int32_t  drift_max;
} DaneWagiLegacy;

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
RTC_DATA_ATTR static uint8_t  lastWakeSlot = 3;   // 1=06:00, 2=20:00, 3=inne/restart
RTC_DATA_ATTR static uint8_t  emptyListenStreak = 0;

// ================== ZMIENNE GLOBALNE ==================
volatile bool noweDatane = false;
DaneWagi      odebraneData;
uint8_t       lastSenderMAC[6];
float         centralBatteryVoltage = 0.0;
int           centralSignalDBm      = 0;
float centralTemperature    = 0.0;

struct ScaleData {
    String   device_id;
    float    weight;
    float    battery;
    bool     received;
};
ScaleData scales[MAX_SCALES_TOTAL];
int       scaleCount = 0;

// ================== FORWARD DECLARATIONS ==================
void logMsg(const String& msg);
void logf(const char* fmt, ...);
void logRaw(const String& msg);
void log_close();
void goToSleep();

// ============================================================
// ================== OBLICZ I USTAW NASTĘPNY ALARM RTC =======
// ============================================================
/**
 * Wybiera najbliższy z WAKE_HOURS/WAKE_MINUTES, ustawia Alarm1
 * DS3231 i zwraca minuty do alarmu (do logów).
 *
 * DS3231_A1_Hour = alarm gdy godzina, minuta i sekunda pasują.
 * Pin SQW/INT przechodzi w LOW gdy alarm się wyzwoli i pozostaje
 * LOW do momentu wywołania rtc.clearAlarm(1).
 */
int setNextRTCAlarm() {
    DateTime now = rtc.now();
    int nowMin = now.hour() * 60 + now.minute();

    int bestDiff   = -1;
    int bestHour   = WAKE_HOURS[0];
    int bestMinute = WAKE_MINUTES[0];
    uint8_t bestSlot = 3;

    for (int i = 0; i < WAKE_COUNT; i++) {
        int alarmMin = WAKE_HOURS[i] * 60 + WAKE_MINUTES[i];
        int diff     = alarmMin - nowMin;
        if (diff <= 0) diff += 24 * 60;          // już minął dziś → jutro
        if (bestDiff < 0 || diff < bestDiff) {
            bestDiff   = diff;
            bestHour   = WAKE_HOURS[i];
            bestMinute = WAKE_MINUTES[i];
            bestSlot   = (i == 0) ? 1 : 2;
        }
    }

    // Wyczyść stare alarmy (zwalnia linię INT jeśli aktywna)
    rtc.disableAlarm(1);
    rtc.disableAlarm(2);
    rtc.clearAlarm(1);
    rtc.clearAlarm(2);

    // Alarm1: wybudzenie gdy godzina i minuta pasują (sekundy = 0)
    DateTime alarmTime(now.year(), now.month(), now.day(),
                       bestHour, bestMinute, 0);
    rtc.setAlarm1(alarmTime, DS3231_A1_Hour);
    lastWakeSlot = bestSlot;

    logf("⏰ Alarm RTC ustawiony na %02d:%02d (za %d min)", bestHour, bestMinute, bestDiff);
    return bestDiff;
}

// ============================================================
// ================== DEEP SLEEP ==============================
// ============================================================
void goToSleep() {
    setNextRTCAlarm();

    // Budzenie przez EXT0: pin RTC_INT_PIN, poziom LOW (alarm aktywny LOW)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)RTC_INT_PIN, 0);

    logMsg(F("💤 Wchodzę w deep sleep – budzenie przez INT DS3231"));
    log_close();
    delay(200);
    esp_deep_sleep_start();
}

void deepSleepWithoutRTC(uint32_t sec) {
    Serial.printf("Deep sleep timer %us bez DS3231\n", sec);
    delay(200);
    esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
    esp_deep_sleep_start();
}

// ============================================================
// ================== WYSYŁANIE DO WAGI =======================
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
    if (sendToScale(mac, (uint8_t*)&resp, sizeof(resp)))
        logf("📡 Discovery response → %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void sendTimeSyncToScale(const uint8_t* mac) {
    TimeSyncPacket tp;
    tp.magic = MAGIC_TIME_SYNC;
    tp.epoch = (uint32_t)rtc.now().unixtime();
    if (sendToScale(mac, (uint8_t*)&tp, sizeof(tp)))
        logf("🕐 Sync czasu → %02X:%02X:%02X:%02X:%02X:%02X | epoch=%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tp.epoch);
}

// ============================================================
// ================== ESP-NOW CALLBACK ========================
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

    if (magic == MAGIC_DATA_WAGI &&
        (len == sizeof(DaneWagi) || len == sizeof(DaneWagiLegacy))) {
        if (len == sizeof(DaneWagiLegacy)) {
            const DaneWagiLegacy* legacy = (const DaneWagiLegacy*)data;
            odebraneData.magic = legacy->magic;
            odebraneData.waga = legacy->waga;
            odebraneData.bateria = legacy->bateria;
            strncpy(odebraneData.device_id, legacy->device_id, sizeof(odebraneData.device_id));
            odebraneData.device_id[sizeof(odebraneData.device_id) - 1] = '\0';
        } else {
            memcpy(&odebraneData, data, sizeof(DaneWagi));
        }
        memcpy(lastSenderMAC, mac, 6);
        noweDatane = true;
        Serial.println("\n╔═══════════════════════════════════╗");
        Serial.println("║  📥 ODEBRANO DANE OD WAGI        ║");
        Serial.println("╠═══════════════════════════════════╣");
        Serial.printf( "║  ID:      %-24s║\n", odebraneData.device_id);
        Serial.printf( "║  Waga:    %.2f kg\n",  odebraneData.waga);
        Serial.printf( "║  Bateria: %.2f V\n",   odebraneData.bateria);
        Serial.printf( "║  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.println("╚═══════════════════════════════════╝\n");
        sendTimeSyncToScale(mac);
        return;
    }
}

// ============================================================
// ================== LED =====================================
// ============================================================
void blinkLED(uint8_t count, uint16_t onMs = LED_BLINK_MS, uint16_t offMs = LED_BLINK_MS) {
    for (uint8_t i = 0; i < count; i++) {
        digitalWrite(LED_PIN, HIGH); delay(onMs);
        digitalWrite(LED_PIN, LOW);
        if (i < count - 1) delay(offMs);
    }
}

void signalSendResult(bool success) {
    if (success) {
        blinkLED(3, 150, 150);
        return;
    }

    digitalWrite(LED_PIN, HIGH);
    delay(2000);
    digitalWrite(LED_PIN, LOW);
}

// ============================================================
// ================== LOGOWANIE LittleFS ======================
// ============================================================
#define LOG_DIR       "/logs"
#define LOG_BOOT_KEY  "/logs/bootcnt"
#define LOG_MAX_FILES 100

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
    logFile = LittleFS.open(logFilePath,"a");
    logReady = (bool)logFile;
}

void log_close() { if (logFile) { logFile.flush(); logFile.close(); } }

// ============================================================
// ================== RTC HELPERS =============================
// ============================================================
String nowStr() {
    DateTime now = rtc.now();
    char buf[20];
    sprintf(buf,"%04d-%02d-%02d %02d:%02d:%02d",
            now.year(),now.month(),now.day(),
            now.hour(),now.minute(),now.second());
    return String(buf);
}

// ============================================================
// ================== BATERIA CENTRALI ========================
// ============================================================
float readCentralBattery() {
    analogReadResolution(12);
    analogSetPinAttenuation(CENTRAL_BAT_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += analogRead(CENTRAL_BAT_ADC_PIN); delayMicroseconds(100); }
    return (sum / 16.0 / 4095.0) * 3.3 * 2.0;
}

// ============================================================
// ================== GPRS ====================================
// ============================================================
bool gprsConnected = false;

void powerOffGPRSModem() {
    gsmClient.stop();
    if (gprsConnected) {
        modem.gprsDisconnect();
        delay(500);
    }
    modem.poweroff();
    digitalWrite(MODEM_POWER_ON, LOW);
    digitalWrite(MODEM_PWRKEY, HIGH);
    gprsConnected = false;
}

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
    if (!modem.waitForNetwork(GPRS_NETWORK_TIMEOUT_MS)) {
        logMsg(F("❌ GPRS: brak sieci"));
        powerOffGPRSModem();
        return false;
    }
    if (!modem.gprsConnect(APN,USER,PASS)) {
        logMsg(F("❌ GPRS: błąd APN"));
        powerOffGPRSModem();
        return false;
    }
    logMsg(F("✅ GPRS połączony"));
    gprsConnected = true;
    return true;
}

void disconnectGPRS() {
    powerOffGPRSModem();
    logMsg(F("📴 GPRS rozłączony"));
}

// ============================================================
// ================== JSON ====================================
// ============================================================
String buildJson(const String& timestamp) {
    String json = "{";
    json += "\"gateway_id\":\"" + String(GATEWAY_ID) + "\",";
    json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"gateway_battery\":" + String(centralBatteryVoltage, 2) + ",";
    json += "\"gateway_signal\":"      + String(centralSignalDBm)      + ",";
    json += "\"gateway_temperature\":" + String(centralTemperature, 1) + ",";
    json += "\"w\":" + String(lastWakeSlot) + ",";
    json += "\"timestamp\":\"" + timestamp + "\",";
    json += "\"measurements\":[";
    bool first = true;
    for (int i = 0; i < scaleCount; i++) {
        if (!scales[i].received) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"device_id\":\""  + scales[i].device_id        + "\","
              + "\"weight\":"       + String(scales[i].weight, 2)  + ","
              + "\"battery\":"      + String(scales[i].battery, 2) + "}";
    }
    json += "]}";
    return json;
}

// ============================================================
// ================== HTTP SEND + RETRY =======================
// ============================================================
bool sendWithRetry(const String& payload) {
    for (int attempt = 1; attempt <= GPRS_MAX_RETRIES; attempt++) {
        logf("📡 GPRS próba %d/%d", attempt, GPRS_MAX_RETRIES);
        int csq = modem.getSignalQuality();
        if (csq != 99 && csq < GPRS_MIN_USABLE_CSQ) {
            logf("⚠️ Zbyt słaby sygnał GSM (CSQ=%d), pomijam dalsze próby", csq);
            return false;
        }
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
// ================== PĘTLA NASŁUCHU ESP-NOW ==================
// ============================================================
/**
 * Nasłuchuje przez max LISTEN_SEC sekund LUB do zebrania
 * EXPECTED_SCALES unikalnych wag — co nastąpi pierwsze.
 */
uint16_t currentListenWindowSec() {
    if (emptyListenStreak == 0) return LISTEN_SEC;
    if (emptyListenStreak % LISTEN_FULL_EVERY_MISSES == 0) return LISTEN_SEC;
    if (emptyListenStreak == 1) return LISTEN_SEC_MISS_1;
    return LISTEN_SEC_MISS_MORE;
}

void listenForScales() {
    scaleCount = 0;
    noweDatane = false;
    unsigned long listenStart = millis();
    uint16_t listenSec = currentListenWindowSec();

    logf("👂 Nasłuchuję max %us | cel: %d wag | puste cykle=%u",
         listenSec, EXPECTED_SCALES, emptyListenStreak);
    blinkLED(1, 50, 50);

    while (millis() - listenStart < (uint32_t)listenSec * 1000) {

        // Warunek wyjścia: zebrano oczekiwaną liczbę wag
        if (scaleCount >= EXPECTED_SCALES) {
            logf("🎯 Zebrano %d/%d wag – kończę nasłuch wcześniej (%.1fs)",
                 scaleCount, EXPECTED_SCALES,
                 (millis() - listenStart) / 1000.0);
            break;
        }

        if (noweDatane) {
            noweDatane = false;

            bool juzMamy = false;
            for (int i = 0; i < scaleCount; i++) {
                if (scales[i].device_id == String(odebraneData.device_id)) {
                    // Aktualizuj istniejącą wagę
                    scales[i].weight               = odebraneData.waga;
                    scales[i].battery              = odebraneData.bateria;
                    scales[i].received             = true;
                    juzMamy = true;
                    logf("🔄 Aktualizacja: %s = %.2f kg", odebraneData.device_id, odebraneData.waga);
                    break;
                }
            }
            if (!juzMamy && scaleCount < MAX_SCALES_TOTAL) {
                int idx = scaleCount;
                scales[idx].device_id              = String(odebraneData.device_id);
                scales[idx].weight                 = odebraneData.waga;
                scales[idx].battery                = odebraneData.bateria;
                scales[idx].received               = true;
                scaleCount++;
                logf("✅ Nowa waga [%d/%d]: %s = %.2f kg",
                     scaleCount, EXPECTED_SCALES,
                     odebraneData.device_id, odebraneData.waga);
            }
            blinkLED(1, 30, 30);
        }
        delay(50);
    }

    if (scaleCount > 0) {
        emptyListenStreak = 0;
    } else if (emptyListenStreak < 250) {
        emptyListenStreak++;
    }

    logf("📊 Nasłuch zakończony: %d wag, czas=%.1fs",
         scaleCount, (millis() - listenStart) / 1000.0);
}

// ============================================================
// ================== SETUP ===================================
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    setupPMU();
    delay(300);

    pinMode(LED_PIN,     OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Pin INT DS3231 – wejście z pull-up wewnętrznym ESP32
    // (zewnętrzny rezystor pull-up 10kΩ do 3.3V też zalecany)
    pinMode(RTC_INT_PIN, INPUT_PULLUP);

    // ── RTC INIT ──────────────────────────────────────────
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    if (!rtc.begin()) {
        Serial.println("❌ DS3231 ERROR!");
        deepSleepWithoutRTC(3600UL);
    }
    if (USTAW_ZEGAR) {
        Serial.println("?? Wymuszam ustawienie RTC czasem kompilacji");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    } else if (rtc.lostPower()) {
        Serial.println("⚠️ RTC stracił zasilanie – ustawiam czas kompilacji");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // WAŻNE: wyczyść flagę alarmu DS3231 zaraz po przebudzeniu
    // aby zwolnić linię INT (która jest active-low i trzymana LOW
    // dopóki alarm nie zostanie skasowany)
    rtc.clearAlarm(1);
    rtc.clearAlarm(2);
    rtc.disableAlarm(2);  // Alarm2 nieużywany

    // ── LOG INIT ──────────────────────────────────────────
    log_init();
    bootCount++;

    Serial.println("Wyślij 'L' w ciągu 1s aby zobaczyć logi...");
    unsigned long t = millis();
    while (millis() - t < LOG_DUMP_WINDOW_MS) {
        if (Serial.available() && Serial.read() == 'L') {
            File dir = LittleFS.open("/logs");
            File f = dir.openNextFile();
            while (f) {
                Serial.println("=== " + String(f.name()) + " ===");
                while (f.available()) Serial.write(f.read());
                f.close();
                f = dir.openNextFile();
            }
        }
    }
    // Powód wybudzenia
    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();

    if (bootCount == 1 || wakeReason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        logRaw(F("\n╔════════════════════════════════╗"));
        logRaw(F("║  🐝 CENTRALA PASIECZNA        ║"));
        logRaw(F("║  Firmware: " FIRMWARE_VERSION "     ║"));
        logRaw(F("╚════════════════════════════════╝"));
        logMsg(F("🔌 Pierwsze uruchomienie / reset sprzętowy"));
        lastWakeSlot = 3;
    }

    if (wakeReason == ESP_SLEEP_WAKEUP_EXT0) {
        logMsg(F("⏰ Wybudzenie przez INT DS3231 (alarm RTC)"));
    } else {
        lastWakeSlot = 3;
    }

    logf("⏰ Boot #%u | %s | w=%u", bootCount, nowStr().c_str(), lastWakeSlot);

    // ── ESP-NOW INIT ───────────────────────────────────────
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        logMsg(F("❌ Błąd ESP-NOW init!"));
        goToSleep();
        return;
    }
    esp_now_register_recv_cb(onDataRecv);

    // ── NASŁUCH: 2 minuty LUB EXPECTED_SCALES wag ─────────
    String timestamp = nowStr();
    listenForScales();
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);

    // ── WYŚLIJ GPRS ZAWSZE (dane wag lub samo napięcie centrali) ──
    centralBatteryVoltage = readCentralBattery();
    centralTemperature    = rtc.getTemperature();
    logf("🌡️ Temperatura RTC: %.1f°C", centralTemperature);
    if (scaleCount > 0) {
        logf("📶 Odebrano od %d wag – wysyłam GPRS", scaleCount);
    } else {
        logMsg(F("⚠️ Brak danych od wag – wysyłam samo napięcie centrali"));
    }
    if (initGPRS()) {
        int csq = modem.getSignalQuality();
        centralSignalDBm = (csq == 99) ? 0 : csq;
        logf("📶 Zasięg GSM: CSQ=%d", centralSignalDBm);
        String json = buildJson(timestamp);
        logRaw("📦 JSON: " + json);
        bool sendOk = sendWithRetry(json);
        disconnectGPRS();
        signalSendResult(sendOk);
    } else {
        signalSendResult(false);
    }

    // ── DEEP SLEEP DO NASTĘPNEGO ALARMU DS3231 ─────────────
    goToSleep();
}

void loop() {}
