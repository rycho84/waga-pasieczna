#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>
#include "HX711.h"
#include <time.h>

// ================== WERSJA ==================
#define FIRMWARE_VERSION "2.2-DISCOVERY"


// ================== PINY HX711 ==================
#define HX711_DT_PIN    20
#define HX711_SCK_PIN   21
#define HX711_VCC_PIN   22
#define HX711_GND_PIN   19

// ================== KALIBRACJA ==================
const float zero   = -271000;
const float faktor = -23000;

// ================== BATERIA ==================
#define BAT_ADC_PIN     0

// ================== RESET KONFIGURACJI ==================
#ifdef BOARD_ESP32_CLASSIC
#define RESET_BTN_PIN   0    // GPIO0 = BOOT na klasycznym ESP32 / Wemos
#else
#define RESET_BTN_PIN   9    // GPIO9 = BOOT na FireBeetle ESP32-C6
#endif
#define RESET_HOLD_MS   3000

// ================== ALARMY ==================
#define ALARM_HOUR_1        6
#define ALARM_HOUR_2        20
#define MIN_SLEEP_SEC       1800UL  // min 30 min do alarmu – zapobiega podwójnemu wysłaniu
#define DEFAULT_SLEEP_SEC   3600UL

// ================== DISCOVERY ==================
#define DISCOVERY_TIMEOUT_MS  5000
#define DISCOVERY_RETRIES     5

// Magiczne bajty
#define MAGIC_DATA_WAGI     0x11
#define MAGIC_DISCOVERY     0xBB
#define MAGIC_DISC_RESPONSE 0xCC
#define MAGIC_TIME_SYNC     0xAA

uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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
    uint8_t  magic;      // MAGIC_TIME_SYNC — musi być PIERWSZY bajt!
    uint32_t epoch;
} TimeSyncPacket;

// ================== PAMIĘĆ RTC ==================
RTC_DATA_ATTR static time_t   savedEpoch        = 0;
RTC_DATA_ATTR static bool     timeValid         = false;
RTC_DATA_ATTR static uint32_t bootCount         = 0;
RTC_DATA_ATTR static uint8_t  rtcCentralaMAC[6] = {0};
RTC_DATA_ATTR static bool     rtcMACValid       = false;

// ── Dane dryfu RTC ────────────────────────────────────────
struct RtcDriftData {
    int32_t  drift_ppm_avg;
    int32_t  drift_ppm_last;
    uint32_t successful_syncs;
    uint32_t boots_since_evening;
    int32_t  max_drift_seen;
    int32_t  min_drift_seen;
    bool     drift_initialized;
    bool     evening_recorded;
    uint32_t sync_evening_ts;     // UNIX ts wieczornej sync
    uint32_t sleep_started_ts;    // kiedy zasnął
    uint32_t planned_wakeup_ts;   // kiedy planował wstać
};
RTC_DATA_ATTR static RtcDriftData drift = {
    0, 0, 0, 0, -999999, 999999, false, false, 0, 0, 0
};

// ── Tryb retry (deep sleep między próbami wysyłki) ────────
// 6 cykli × (10s aktywny + 20s sleep) = 3 minuty
#define SEND_WINDOW_SEC   20   // czas aktywnego wysyłania [s]
#define RETRY_SLEEP_SEC   30   // deep sleep między próbami [s]
#define RETRY_MAX_CYCLES  10   // max liczba prób

RTC_DATA_ATTR static uint8_t retryCycle = 0;  // licznik nieudanych prób
RTC_DATA_ATTR static float   rtcWaga       = 0.0;   // zapamiętany pomiar wagi
RTC_DATA_ATTR static float   rtcBateria    = 0.0;   // zapamiętane napięcie baterii
RTC_DATA_ATTR static char    rtcDeviceId[20] = {0}; // zapamiętany device_id

// ================== ZMIENNE GLOBALNE ==================
uint8_t  centralaMACbuf[6];
bool     macKnown          = false;

char         wagaMAC[20]        = "";   // wypełniany w setup() po WiFi.mode()
volatile bool wyslanoPomyslnie = false;
volatile bool discoveryDone    = false;
volatile bool timeSyncReceived = false;

DiscoveryResponse receivedDiscResp;
TimeSyncPacket    receivedTimeSync;

Preferences prefs;
HX711       scale;

// ============================================================
// ================== LOGIKA DRYFU ==================
// ============================================================

// Wieczór: zapisz punkt odniesienia
void recordEveningSyncPoint(uint32_t unix_ts) {
    drift.sync_evening_ts     = unix_ts;
    drift.boots_since_evening = 0;
    drift.evening_recorded    = true;
    Serial.printf("🌆 Punkt wieczorny zapisany: %u\n", unix_ts);
}

// Poranek: oblicz dryf na podstawie UNIX timestampów
void calculateDrift(uint32_t morning_unix_ts) {
    if (!drift.evening_recorded || drift.sync_evening_ts == 0) {
        Serial.println("⚠️ Brak punktu wieczornego – pomijam drift");
        return;
    }
    int32_t delta_real = (int32_t)(morning_unix_ts - drift.sync_evening_ts);
    if (delta_real < 3600 || delta_real > 50000) {
        Serial.printf("⚠️ Podejrzany delta_real=%d s – pomijam\n", delta_real);
        return;
    }
    if (drift.sleep_started_ts == 0 || drift.planned_wakeup_ts == 0) {
        Serial.println("⚠️ Brak danych sleep ts – pomijam drift");
        return;
    }
    int32_t delta_esp = (int32_t)(drift.planned_wakeup_ts - drift.sleep_started_ts);
    if (delta_esp < 60) return;

    float   drift_sec = (float)delta_esp - (float)delta_real;
    int32_t drift_ppm = (int32_t)((drift_sec / (float)delta_real) * 1000000.0f);
    drift_ppm = constrain(drift_ppm, -10000L, 10000L);

    Serial.printf("📊 Drift: real=%ds esp=%ds diff=%.1fs ppm=%d\n",
                  delta_real, delta_esp, drift_sec, drift_ppm);

    drift.drift_ppm_last = drift_ppm;

    if (!drift.drift_initialized || drift.successful_syncs == 0) {
        drift.drift_ppm_avg = drift_ppm;
    } else {
        if (abs(drift_ppm - drift.drift_ppm_avg) > 3000) {
            Serial.println("⚠️ Drastyczna zmiana dryfu – reset EMA");
            drift.drift_ppm_avg = drift_ppm;
        } else {
            float alpha = (drift.successful_syncs < 5) ? 0.5f : 0.25f;
            drift.drift_ppm_avg = (int32_t)(drift.drift_ppm_avg * (1.0f - alpha) + drift_ppm * alpha);
        }
    }

    if (drift_ppm > drift.max_drift_seen) drift.max_drift_seen = drift_ppm;
    if (drift_ppm < drift.min_drift_seen) drift.min_drift_seen = drift_ppm;

    drift.successful_syncs++;
    drift.drift_initialized = true;
    drift.evening_recorded  = false;

    Serial.printf("✅ Drift avg=%d ppm  syncs=%u\n",
                  drift.drift_ppm_avg, drift.successful_syncs);
}

// Skoryguj czas snu o zmierzony dryf
long applyDriftCorrection(long planned_sec) {
    if (!drift.drift_initialized || drift.successful_syncs < 1) return planned_sec;
    if (drift.boots_since_evening > 6) return planned_sec;

    float correction = (float)planned_sec * (float)drift.drift_ppm_avg / 1000000.0f;
    long  max_corr   = planned_sec / 10;
    long  corrected  = constrain((long)(planned_sec - correction),
                                  planned_sec - max_corr,
                                  planned_sec + max_corr);
    Serial.printf("🎯 Korekcja snu: %ld→%ld s (%.1fs, drift=%d ppm)\n",
                  planned_sec, corrected, correction, drift.drift_ppm_avg);
    return corrected;
}

// ================== FORWARD DECLARATIONS ==================
void goToSleep();

// ============================================================
// ================== CZAS ==================
// ============================================================
String epochToString(time_t t) {
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return String(buf);
}

uint64_t secondsUntilNextAlarm(time_t now) {
    struct tm t;
    localtime_r(&now, &t);
    int currentSecs = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    const int targets[] = { ALARM_HOUR_1 * 3600, ALARM_HOUR_2 * 3600 };
    int minDiff = 86400;
    for (int i = 0; i < 2; i++) {
        int diff = targets[i] - currentSecs;
        if (diff <= (int)MIN_SLEEP_SEC) diff += 86400;
        if (diff < minDiff) minDiff = diff;
    }
    return (uint64_t)minDiff;
}

// ============================================================
// ================== PREFERENCES ==================
// ============================================================
bool loadCentralaMAC(uint8_t* mac) {
    prefs.begin("waga_cfg", true);
    bool ok = prefs.isKey("c_mac");
    if (ok) prefs.getBytes("c_mac", mac, 6);
    prefs.end();
    return ok;
}

void saveCentralaMAC(const uint8_t* mac) {
    prefs.begin("waga_cfg", false);
    prefs.putBytes("c_mac", mac, 6);
    prefs.end();
    Serial.printf("💾 Zapisano MAC centrali: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void clearConfig() {
    prefs.begin("waga_cfg", false);
    prefs.clear();
    prefs.end();
    rtcMACValid = false;
    memset(rtcCentralaMAC, 0, 6);
    Serial.println("🗑 Konfiguracja wyczyszczona – discovery przy następnym starcie");
}

// ============================================================
// ================== ESP-NOW CALLBACKS ==================
// ============================================================

// Wspólna logika odbioru – wywoływana z obu wersji callbacka
void _handleRecv(const uint8_t *senderMAC, const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t magic = data[0];

    if (magic == MAGIC_DISC_RESPONSE && len == sizeof(DiscoveryResponse)) {
        memcpy(&receivedDiscResp, data, sizeof(DiscoveryResponse));
        memcpy(centralaMACbuf, senderMAC, 6);
        discoveryDone = true;
        Serial.printf("📡 Discovery OK! Centrala: %s  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      receivedDiscResp.gateway_id,
                      centralaMACbuf[0], centralaMACbuf[1], centralaMACbuf[2],
                      centralaMACbuf[3], centralaMACbuf[4], centralaMACbuf[5]);
    }
    else if (magic == MAGIC_TIME_SYNC && len == sizeof(TimeSyncPacket)) {
        memcpy(&receivedTimeSync, data, sizeof(TimeSyncPacket));
        uint32_t unix_ts = receivedTimeSync.epoch;

        // Walidacja timestamp
        if (unix_ts < 1700000000UL) {
            Serial.println("⚠️ Sync: timestamp za mały – odrzucam");
            return;
        }

        timeSyncReceived = true;
        Serial.printf("🕐 Sync czasu: %u (%s)\n",
                      unix_ts, epochToString((time_t)unix_ts).c_str());

        // Określ porę dnia i obsłuż drift
        struct tm t;
        time_t tt = (time_t)unix_ts;
        localtime_r(&tt, &t);
        int hour = t.tm_hour;

        if (hour >= 5 && hour < 14) {
            // Poranek – oblicz dryf
            calculateDrift(unix_ts);
        } else {
            // Wieczór/noc – zapisz punkt odniesienia
            recordEveningSyncPoint(unix_ts);
        }
    }
}

#ifdef BOARD_ESP32_CLASSIC
// ── Stary SDK: espressif32 <=6.x (ESP32, WEMOS itp.) ──────
void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    wyslanoPomyslnie = (status == ESP_NOW_SEND_SUCCESS);
    Serial.println(wyslanoPomyslnie ? "📤 ✅ Wysłano" : "📤 ❌ Błąd wysyłania");
}
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    _handleRecv(mac, data, len);
}
#else
// ── Nowy SDK: pioarduino (ESP32-C6, S3 itp.) ──────────────
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    wyslanoPomyslnie = (status == ESP_NOW_SEND_SUCCESS);
    Serial.println(wyslanoPomyslnie ? "📤 ✅ Wysłano" : "📤 ❌ Błąd wysyłania");
}
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    _handleRecv(recv_info->src_addr, data, len);
}
#endif

// ============================================================
// ================== DISCOVERY ==================
// ============================================================
bool runDiscovery() {
    Serial.println("\n🔍 Tryb DISCOVERY – szukam centrali...");

    if (!esp_now_is_peer_exist(broadcastMAC)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, broadcastMAC, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    DiscoveryPacket dp;
    dp.magic = MAGIC_DISCOVERY;
    strncpy(dp.device_id, wagaMAC, sizeof(dp.device_id));

    for (int attempt = 1; attempt <= DISCOVERY_RETRIES; attempt++) {
        Serial.printf("   Próba %d/%d\n", attempt, DISCOVERY_RETRIES);
        discoveryDone = false;
        esp_now_send(broadcastMAC, (uint8_t*)&dp, sizeof(dp));

        unsigned long t = millis();
        while (!discoveryDone && millis() - t < DISCOVERY_TIMEOUT_MS) delay(50);

        if (discoveryDone) {
            saveCentralaMAC(centralaMACbuf);
            memcpy(rtcCentralaMAC, centralaMACbuf, 6);
            rtcMACValid = true;
            return true;
        }
        Serial.println("   Brak odpowiedzi, ponawiam...");
        delay(500);
    }

    Serial.println("❌ Discovery nieudane – brak centrali w zasięgu");
    return false;
}

// ============================================================
// ================== HX711 ==================
// ============================================================
void hx711_power_on() { digitalWrite(HX711_VCC_PIN, HIGH); delay(500); }
void hx711_power_off() { delay(10); digitalWrite(HX711_VCC_PIN, LOW); }

float read_weight() {
    Serial.println("⚖️ Ważenie...");
    hx711_power_on();
    float reading = 0;
    if (scale.is_ready()) {
        reading = scale.get_units(5);
    } else {
        Serial.println("❌ HX711 nie odpowiada!");
        hx711_power_off();
        return 0.0;
    }
    float weight = (reading - zero) / faktor;
    Serial.printf("✅ Masa: %.2f kg\n", weight);
    hx711_power_off();
    return weight;
}

// ============================================================
// ================== BATERIA ==================
// ============================================================
float readBatteryVoltage() {
    delayMicroseconds(1000);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += analogRead(BAT_ADC_PIN); delayMicroseconds(50); }
    float v = (sum / 16.0f) * 3.3f / 4095.0f * 2.469f;  // kalibracja: *2.469 = *2.0 * (3.85/3.12)
    Serial.printf("🔋 Bateria: %.2f V\n", v);
    return v;
}

// ============================================================
// ================== SETUP ==================
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    bootCount++;

    // Inkrementuj licznik bootów od wieczornej sync (tylko prawdziwe pomiary, nie retry)
    if (drift.evening_recorded && retryCycle == 0)
        drift.boots_since_evening++;

    Serial.println("\n╔════════════════════════════════╗");
    Serial.println("║    🐝 WAGA PASIECZNA          ║");
    Serial.printf( "║    Firmware: %-18s║\n", FIRMWARE_VERSION);
    Serial.printf( "║    Boot #%-22u║\n", bootCount);
    Serial.println("╚════════════════════════════════╝");

    if (timeValid)
        Serial.printf("🕐 Czas: %s\n", epochToString(savedEpoch).c_str());
    else
        Serial.println("⚠️ Czas nieznany");

    // ── SPRAWDŹ PRZYCISK RESET KONFIGURACJI ────────────────
    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    if (digitalRead(RESET_BTN_PIN) == LOW) {
        Serial.printf("⚠️ Przycisk wciśnięty – trzymaj %ds aby zresetować...\n",
                      RESET_HOLD_MS / 1000);
        unsigned long held = millis();
        while (digitalRead(RESET_BTN_PIN) == LOW && millis() - held < RESET_HOLD_MS)
            delay(100);
        if (millis() - held >= RESET_HOLD_MS) {
            clearConfig();
            Serial.println("✅ Reset konfiguracji wykonany");
            delay(1000);
        } else {
            Serial.println("ℹ️ Za krótko – reset anulowany");
        }
    }

    // ── KONFIGURACJA SPRZĘTU ───────────────────────────────
    pinMode(HX711_VCC_PIN, OUTPUT);
    pinMode(HX711_GND_PIN, OUTPUT);
    digitalWrite(HX711_GND_PIN, LOW);
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    // ── POMIARY ────────────────────────────────────────────
    // Przy retry używamy danych z pamięci RTC (pomierzonych przy pierwszym starcie)
    float masa     = 0.0;
    float napiecie = 0.0;
    if (retryCycle > 0) {
        masa     = rtcWaga;
        napiecie = rtcBateria;
        Serial.printf("🔄 Retry %d/%d – dane z RTC: waga=%.2f kg\n",
                      retryCycle, RETRY_MAX_CYCLES, masa);
    } else {
        masa     = read_weight();
        napiecie = readBatteryVoltage();
        if (masa < 0 || masa > 500) { Serial.println("⚠️ Błędny pomiar – 0.00"); masa = 0.0; }
        Serial.printf("📦 Dane: waga=%.2f kg, bat=%.2f V\n", masa, napiecie);
        // Zapisz do RTC na wypadek retry
        rtcWaga    = masa;
        rtcBateria = napiecie;
        strncpy(rtcDeviceId, wagaMAC, sizeof(rtcDeviceId));
    }

    // ── ESP-NOW INIT ───────────────────────────────────────
    WiFi.mode(WIFI_STA);
    // Użyj MAC jako device_id (bez dwukropków, np. FC012CEC7C54)
    String macStr = WiFi.macAddress();
    macStr.replace(":", "");
    strncpy(wagaMAC, macStr.c_str(), sizeof(wagaMAC));
    Serial.print("📍 MAC wagi (device_id): "); Serial.println(wagaMAC);

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Błąd ESP-NOW init!");
        goToSleep(); return;
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    // ── USTAL MAC CENTRALI ─────────────────────────────────
    // Priorytet: pamięć RTC → Preferences → discovery
    if (rtcMACValid) {
        memcpy(centralaMACbuf, rtcCentralaMAC, 6);
        macKnown = true;
        Serial.printf("📋 MAC z RTC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      centralaMACbuf[0], centralaMACbuf[1], centralaMACbuf[2],
                      centralaMACbuf[3], centralaMACbuf[4], centralaMACbuf[5]);
    } else if (loadCentralaMAC(centralaMACbuf)) {
        macKnown = true;
        memcpy(rtcCentralaMAC, centralaMACbuf, 6);
        rtcMACValid = true;
        Serial.printf("📋 MAC z Preferences: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      centralaMACbuf[0], centralaMACbuf[1], centralaMACbuf[2],
                      centralaMACbuf[3], centralaMACbuf[4], centralaMACbuf[5]);
    } else {
        Serial.println("❓ Brak zapisanego MAC – uruchamiam discovery");
        macKnown = runDiscovery();
    }

    if (!macKnown) {
        Serial.println("❌ Nie znaleziono centrali – idę spać");
        goToSleep(); return;
    }

    // ── DODAJ CENTRALĘ JAKO PEER ───────────────────────────
    if (!esp_now_is_peer_exist(centralaMACbuf)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, centralaMACbuf, 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println("❌ Błąd dodawania peer!");
            goToSleep(); return;
        }
    }
    Serial.println("✅ Centrala dodana jako peer");

    // ── PRZYGOTUJ DANE DO WYSŁANIA ────────────────────────
    DaneWagi dane;
    dane.magic                = MAGIC_DATA_WAGI;
    dane.waga                 = masa;
    dane.bateria              = napiecie;
    strncpy(dane.device_id, wagaMAC, sizeof(dane.device_id));
    // Wypełnij dane dryfu
    dane.drift_ppm_avg        = drift.drift_ppm_avg;
    dane.drift_ppm_last       = drift.drift_ppm_last;
    dane.drift_syncs          = drift.successful_syncs;
    dane.drift_boots_since_eve = drift.boots_since_evening;
    dane.drift_min            = drift.min_drift_seen;
    dane.drift_max            = drift.max_drift_seen;

    // ── WYŚLIJ DANE (okno SEND_WINDOW_SEC sekund) ─────────
    Serial.printf("📡 Wysyłanie (okno %ds, próba %d/%d)...\n",
                  SEND_WINDOW_SEC, retryCycle + 1, RETRY_MAX_CYCLES);
    unsigned long sendStart = millis();
    int proba = 0;
    while (!wyslanoPomyslnie && millis() - sendStart < (uint32_t)SEND_WINDOW_SEC * 1000) {
        proba++;
        wyslanoPomyslnie = false;
        esp_now_send(centralaMACbuf, (uint8_t*)&dane, sizeof(dane));
        Serial.printf("   Próba %d | %lus\n", proba, (millis() - sendStart) / 1000);
        unsigned long tw = millis();
        while (!wyslanoPomyslnie && millis() - tw < 1000) delay(50);
        if (!wyslanoPomyslnie) delay(1000);
    }

    if (wyslanoPomyslnie) {
        // ── SUKCES → czekaj na sync, idź spać do alarmu ───
        retryCycle = 0;
        Serial.println("\n╔════════════════════════════════╗");
        Serial.println("║  ✅ DANE WYSŁANE POMYŚLNIE    ║");
        Serial.println("╚════════════════════════════════╝");

        Serial.println("⏳ Czekam na sync czasu (3s)...");
        unsigned long tw = millis();
        while (!timeSyncReceived && millis() - tw < 3000) delay(50);

        if (timeSyncReceived) {
            savedEpoch = (time_t)receivedTimeSync.epoch;
            timeValid  = true;
            Serial.printf("✅ Czas: %s\n", epochToString(savedEpoch).c_str());
        } else {
            Serial.println("⚠️ Brak sync czasu od centrali");
        }
        delay(200);
        goToSleep();

    } else {
        // ── NIEUDANA PRÓBA ─────────────────────────────────
        retryCycle++;
        if (retryCycle < RETRY_MAX_CYCLES) {
            Serial.printf("\n⚠️ Brak odpowiedzi – próba %d/%d, sleep %ds\n",
                          retryCycle, RETRY_MAX_CYCLES, RETRY_SLEEP_SEC);
            delay(100);
            esp_sleep_enable_timer_wakeup((uint64_t)RETRY_SLEEP_SEC * 1000000ULL);
            esp_deep_sleep_start();
        } else {
            retryCycle = 0;
            Serial.println("\n╔════════════════════════════════╗");
            Serial.println("║  ❌ WYSYŁANIE NIEUDANE        ║");
            Serial.println("╚════════════════════════════════╝");
            goToSleep();
        }
    }
}


// ============================================================
// ================== DEEP SLEEP ==================
// ============================================================

// Normalny sleep do następnego alarmu (6:00 / 20:00)
void goToSleep() {
    uint64_t sleepSec;

    if (timeValid) {
        long planned = (long)secondsUntilNextAlarm(savedEpoch);
        long corrected = applyDriftCorrection(planned);
        sleepSec = (uint64_t)constrain(corrected, 60L, 86400L);

        // Zapisz timestamps do obliczenia dryfu przy następnym wybudzeniu
        drift.sleep_started_ts  = (uint32_t)savedEpoch;
        drift.planned_wakeup_ts = (uint32_t)(savedEpoch + (time_t)sleepSec);

        time_t wakeTime = savedEpoch + (time_t)sleepSec;
        Serial.printf("⏰ Budzę się: %s\n", epochToString(wakeTime).c_str());
        Serial.printf("💤 Sleep: %llu s (%llu min)\n", sleepSec, sleepSec / 60);
        savedEpoch += (time_t)sleepSec;
    } else {
        sleepSec = DEFAULT_SLEEP_SEC;
        drift.sleep_started_ts  = 0;
        drift.planned_wakeup_ts = 0;
        Serial.printf("💤 Czas nieznany – sleep %lu s\n", sleepSec);
    }

    delay(200);
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}