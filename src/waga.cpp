// Zmiana: 5.3 - usunieto telemetrie dryfu, odblokowano cykliczna synchronizacje DS3231 i ograniczono retry poza oknem centrali.
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>
#include "HX711.h"
#include <Wire.h>
#include <RTClib.h>
#include <sys/time.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"

// ================== WERSJA ==================
#define FIRMWARE_VERSION "5.3-RESET-IRQ"

// ================== PINY HX711 ==================
#define HX711_DT_PIN    2
#define HX711_SCK_PIN   3
#define HX711_VCC_PIN   4
#define HX711_GND_PIN   5

// ================== PINY DS3231 ==================
// INT/SQW NIE jest podłączony – budzenie wyłącznie przez timer ESP
#define DS3231_SDA_PIN  19
#define DS3231_SCL_PIN  20
#define DS3231_VCC_PIN  5   // zasilanie DS3231 sterowane z GPIO

// ================== PIN RESET CENTRALI ==================
#define RESET_BTN_PIN   6
#define RESET_HOLD_MS   3000
#define LED_PIN         7
#define ONBOARD_LED_PIN 15

// ================== KALIBRACJA WAGI ==================
const float zero   = 1154100;
const float faktor = -21600;

// ================== BATERIA ==================
#define BAT_ADC_PIN     0

// ================== HARMONOGRAM ==================
#define PRESYNC_HOUR_1   5
#define PRESYNC_MIN_1   30
#define PRESYNC_HOUR_2  19
#define PRESYNC_MIN_2   30
#define SEND_HOUR_1      6
#define SEND_HOUR_2     20

// ================== ESP-NOW ==================
#define SEND_WINDOW_SEC      20
#define RETRY_SLEEP_SEC      30
#define RETRY_MAX_CYCLES_IN_WINDOW 3
#define RETRY_SLEEP_LONG_SEC 1800UL
#define RETRY_MAX_CYCLES     10
#define DEFAULT_SLEEP_SEC    3600UL
#define DISCOVERY_TIMEOUT_MS 5000
#define DISCOVERY_RETRIES    12

#define MAGIC_DATA_WAGI     0x11
#define MAGIC_DISCOVERY     0xBB
#define MAGIC_DISC_RESPONSE 0xCC
#define MAGIC_TIME_SYNC     0xAA

uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ================== TRYBY WYBUDZENIA ==================
#define WAKEUP_SEND    0
#define WAKEUP_PRESYNC 1

// ================== STRUKTURY ESP-NOW ==================
typedef struct {
    uint8_t  magic;
    float    waga;
    float    bateria;
    char     device_id[20];
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
RTC_DATA_ATTR static uint32_t bootCount         = 0;
RTC_DATA_ATTR static uint8_t  rtcCentralaMAC[6] = {0};
RTC_DATA_ATTR static bool     rtcMACValid       = false;
RTC_DATA_ATTR static uint8_t  wakeupMode        = WAKEUP_SEND;
RTC_DATA_ATTR static uint8_t  retryCycle        = 0;
RTC_DATA_ATTR static float    rtcWaga           = 0.0;
RTC_DATA_ATTR static float    rtcBateria        = 0.0;
RTC_DATA_ATTR static bool     ds3231Synced      = false;

// ================== ZMIENNE GLOBALNE ==================
char     wagaMAC[20]           = "";
uint8_t  centralaMACbuf[6];
bool     macKnown              = false;
volatile bool wyslanoPomyslnie = false;
volatile bool discoveryDone    = false;
volatile bool timeSyncReceived = false;

DiscoveryResponse receivedDiscResp;
TimeSyncPacket    receivedTimeSync;

Preferences prefs;
HX711       scale;
RTC_DS3231  rtc;

// ================== RESET BUTTON ISR ==================
volatile unsigned long resetPressedAt = 0;
volatile bool resetBtnHeld = false;
volatile bool resetInProgress = false;
TaskHandle_t resetTaskHandle = nullptr;

bool isResetButtonPressed() {
    return digitalRead(RESET_BTN_PIN) == LOW;
}

void IRAM_ATTR onResetBtn() {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (resetTaskHandle != nullptr) {
        vTaskNotifyGiveFromISR(resetTaskHandle, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
    }
}

// ================== FORWARD DECLARATIONS ==================
void handlePresyncWakeup();
void handleSendWakeup();
void goSleepFallback();
void sleepUntilTime(time_t now, uint8_t targetHour, uint8_t targetMin, uint8_t nextMode);
void sleepTimer(uint32_t sec, uint8_t nextMode);
bool handleResetHoldAtBoot(const char* source);

// ============================================================
// ================== HELPERS =================================
// ============================================================

String epochToString(time_t t) {
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return String(buf);
}

uint32_t secondsUntil(time_t now, uint8_t targetHour, uint8_t targetMin) {
    struct tm t;
    localtime_r(&now, &t);
    struct tm target  = t;
    target.tm_hour    = targetHour;
    target.tm_min     = targetMin;
    target.tm_sec     = 0;
    time_t targetTime = mktime(&target);
    if (targetTime <= now + 60) targetTime += 86400;
    return (uint32_t)(targetTime - now);
}

void setSystemTime(uint32_t epoch) {
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Serial.printf("Czas systemowy ustawiony: %s\n", epochToString((time_t)epoch).c_str());
}


// ============================================================
// ================== RESET PRZYCISK ==========================
// ============================================================

void doReset() {
    if (resetInProgress) return;
    resetInProgress = true;

    Serial.println(">>> RESET CENTRALI – czyszczę NVS i RTC RAM <<<");
    prefs.begin("waga_cfg", false);
    prefs.remove("c_mac");
    prefs.end();
    rtcMACValid      = false;
    ds3231Synced     = false;
    macKnown         = false;
    wakeupMode       = WAKEUP_SEND;
    retryCycle       = 0;
    discoveryDone    = false;
    timeSyncReceived = false;
    wyslanoPomyslnie = false;
    memset(rtcCentralaMAC, 0, sizeof(rtcCentralaMAC));
    memset(centralaMACbuf, 0, sizeof(centralaMACbuf));

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(2000));
    digitalWrite(LED_PIN, LOW);

    while (isResetButtonPressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP.restart();
}

void checkResetAnytime() {
    if (!resetBtnHeld && isResetButtonPressed()) {
        resetPressedAt = millis();
        resetBtnHeld   = true;
    }

    if (resetBtnHeld && !isResetButtonPressed()) {
        resetBtnHeld   = false;
        resetPressedAt = 0;
        return;
    }

    if (resetBtnHeld && resetPressedAt > 0 && millis() - resetPressedAt >= RESET_HOLD_MS) {
        doReset();
    }
}

bool handleResetHoldAtBoot(const char* source) {
    if (!isResetButtonPressed()) return false;

    Serial.printf("Reset GPIO aktywny (%s) - czekam %ums na dlugie przytrzymanie\n",
                  source, RESET_HOLD_MS);

    unsigned long startedAt = millis();
    while (isResetButtonPressed()) {
        if (millis() - startedAt >= RESET_HOLD_MS) {
            Serial.println("Wykryto dlugie przytrzymanie resetu");
            doReset();
            return true;
        }
        delay(20);
    }

    Serial.println("Krotkie nacisniecie resetu - tylko wybudzenie");
    return false;
}

void resetTask(void* pvParameters) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        checkResetAnytime();
    }
}

// ============================================================
// ================== DS3231 ==================================
// ============================================================

bool ds3231Init() {
    pinMode(DS3231_VCC_PIN, OUTPUT);
    digitalWrite(DS3231_VCC_PIN, HIGH);
    delay(100);
    Wire.begin(DS3231_SDA_PIN, DS3231_SCL_PIN);
    if (!rtc.begin(&Wire)) {
        Serial.println("DS3231: nie odpowiada!");
        Wire.end();
        digitalWrite(DS3231_VCC_PIN, LOW);
        return false;
    }
    return true;
}

void ds3231Off() {
    Wire.end();
    digitalWrite(DS3231_VCC_PIN, LOW);
    Serial.println("DS3231: zasilanie wyłączone");
}

time_t ds3231GetTime() {
    if (!ds3231Init()) return 0;
    if (rtc.lostPower()) {
        Serial.println("DS3231: lostPower=true – czas niewiarygodny, wymuszam ponowna synchronizacje");
        ds3231Synced = false;
        ds3231Off();
        return 0;
    }
    DateTime now = rtc.now();
    ds3231Off();

    if (now.year() < 2024) {
        Serial.println("DS3231: czas sprzed 2024 – niezainicjalizowany");
        return 0;
    }
    time_t t = now.unixtime();
    Serial.printf("DS3231: %s\n", epochToString(t).c_str());
    return t;
}

bool ds3231SetTime(uint32_t epoch) {
    if (!ds3231Init()) return false;
    rtc.adjust(DateTime(epoch));
    ds3231Off();
    Serial.printf("DS3231 ustawiony: %s\n", epochToString((time_t)epoch).c_str());
    return true;
}

// ============================================================
// ================== SLEEP ===================================
// ============================================================

void sleepTimer(uint32_t sec, uint8_t nextMode) {
    wakeupMode = nextMode;
    Serial.printf("Deep sleep %us (%.2fh) | nextMode: %s\n",
                  sec, sec / 3600.0f,
                  nextMode == WAKEUP_PRESYNC ? "PRESYNC" : "SEND");
    delay(200);
    esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);

    gpio_sleep_set_pull_mode((gpio_num_t)RESET_BTN_PIN, GPIO_PULLUP_ONLY);
    gpio_deep_sleep_wakeup_enable((gpio_num_t)RESET_BTN_PIN, GPIO_INTR_LOW_LEVEL);
    esp_err_t wakeErr = esp_deep_sleep_enable_gpio_wakeup(1ULL << RESET_BTN_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wakeErr != ESP_OK) {
        Serial.printf("GPIO wakeup setup failed: %d\n", (int)wakeErr);
    }

    esp_deep_sleep_start();
}

void sleepUntilTime(time_t now, uint8_t targetHour, uint8_t targetMin, uint8_t nextMode) {
    uint32_t sec = secondsUntil(now, targetHour, targetMin);
    Serial.printf("Sleep %us (~%.2fh) do %02d:%02d\n",
                  sec, sec / 3600.0f, targetHour, targetMin);
    sleepTimer(sec, nextMode);
}

// ============================================================
// ================== HARMONOGRAM ALARMÓW =====================
// ============================================================

void getNextPresyncAlarm(time_t now, uint8_t &hour, uint8_t &min) {
    struct tm t;
    localtime_r(&now, &t);
    int curMin = t.tm_hour * 60 + t.tm_min;
    int p1     = PRESYNC_HOUR_1 * 60 + PRESYNC_MIN_1;
    int p2     = PRESYNC_HOUR_2 * 60 + PRESYNC_MIN_2;

    if      (curMin + 5 < p1) { hour = PRESYNC_HOUR_1; min = PRESYNC_MIN_1; }
    else if (curMin + 5 < p2) { hour = PRESYNC_HOUR_2; min = PRESYNC_MIN_2; }
    else                      { hour = PRESYNC_HOUR_1; min = PRESYNC_MIN_1; }
}

void getSendTimeAfterPresync(time_t now, uint8_t &hour, uint8_t &min) {
    struct tm t;
    localtime_r(&now, &t);
    hour = (t.tm_hour < 12) ? SEND_HOUR_1 : SEND_HOUR_2;
    min  = 0;
}

void getNextScheduledWake(time_t now, uint8_t &hour, uint8_t &min, uint8_t &mode) {
    struct tm t;
    localtime_r(&now, &t);

    struct Slot {
        uint8_t hour;
        uint8_t min;
        uint8_t mode;
    };

    const Slot slots[] = {
        { PRESYNC_HOUR_1, PRESYNC_MIN_1, WAKEUP_PRESYNC },
        { SEND_HOUR_1,    0,             WAKEUP_SEND    },
        { PRESYNC_HOUR_2, PRESYNC_MIN_2, WAKEUP_PRESYNC },
        { SEND_HOUR_2,    0,             WAKEUP_SEND    }
    };

    time_t bestTime = 0;
    const Slot* bestSlot = nullptr;

    for (const Slot& slot : slots) {
        struct tm candidate = t;
        candidate.tm_hour = slot.hour;
        candidate.tm_min  = slot.min;
        candidate.tm_sec  = 0;

        time_t candidateTime = mktime(&candidate);
        if (candidateTime <= now + 60) {
            candidateTime += 86400;
        }

        if (bestSlot == nullptr || candidateTime < bestTime) {
            bestTime = candidateTime;
            bestSlot = &slot;
        }
    }

    hour = bestSlot->hour;
    min  = bestSlot->min;
    mode = bestSlot->mode;
}

// ============================================================
// ================== FALLBACK SLEEP ==========================
// ============================================================

void goSleepFallback() {
    if (ds3231Synced) {
        time_t now = ds3231GetTime();
        if (now > 0) {
            setSystemTime((uint32_t)now);
            uint8_t nextHour, nextMin, nextMode;
            getNextScheduledWake(now, nextHour, nextMin, nextMode);
            uint8_t pHour = nextHour, pMin = nextMin;
            Serial.printf("Fallback: następny presync o %02d:%02d\n", pHour, pMin);
            sleepUntilTime(now, nextHour, nextMin, nextMode);
            return;
        }
    }
    Serial.printf("Fallback: sleep %lus (1h), tryb SEND\n", DEFAULT_SLEEP_SEC);
    sleepTimer(DEFAULT_SLEEP_SEC, WAKEUP_SEND);
}

// ============================================================
// ================== PREFERENCES =============================
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
    Serial.printf("Zapisano MAC centrali: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ============================================================
// ================== ESP-NOW CALLBACKS =======================
// ============================================================

void _handleRecv(const uint8_t *senderMAC, const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t magic = data[0];

    if (magic == MAGIC_DISC_RESPONSE && len == sizeof(DiscoveryResponse)) {
        memcpy(&receivedDiscResp, data, sizeof(DiscoveryResponse));
        memcpy(centralaMACbuf, senderMAC, 6);
        discoveryDone = true;
        Serial.printf("Discovery OK! MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      centralaMACbuf[0], centralaMACbuf[1], centralaMACbuf[2],
                      centralaMACbuf[3], centralaMACbuf[4], centralaMACbuf[5]);
    }
    else if (magic == MAGIC_TIME_SYNC && len == sizeof(TimeSyncPacket)) {
        memcpy(&receivedTimeSync, data, sizeof(TimeSyncPacket));
        if (receivedTimeSync.epoch > 1700000000UL) {
            timeSyncReceived = true;
            Serial.printf("Sync czasu: %s\n",
                          epochToString((time_t)receivedTimeSync.epoch).c_str());
        }
    }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    wyslanoPomyslnie = (status == ESP_NOW_SEND_SUCCESS);
    Serial.println(wyslanoPomyslnie ? "Wysłano OK" : "Błąd wysyłania");
}

void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    _handleRecv(recv_info->src_addr, data, len);
}

// ============================================================
// ================== DISCOVERY ===============================
// ============================================================

bool runDiscovery() {
    Serial.println("Discovery – szukam centrali...");
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

    for (int i = 1; i <= DISCOVERY_RETRIES; i++) {
        Serial.printf("   Próba %d/%d\n", i, DISCOVERY_RETRIES);
        discoveryDone = false;
        esp_now_send(broadcastMAC, (uint8_t*)&dp, sizeof(dp));
        unsigned long t = millis();
        while (!discoveryDone && millis() - t < DISCOVERY_TIMEOUT_MS) {
            checkResetAnytime();
            delay(50);
        }
        if (discoveryDone) {
            saveCentralaMAC(centralaMACbuf);
            memcpy(rtcCentralaMAC, centralaMACbuf, 6);
            rtcMACValid = true;
            return true;
        }
        delay(500);
    }
    Serial.println("Discovery nieudane");
    return false;
}

// ============================================================
// ================== HX711 ===================================
// ============================================================

float read_weight() {
    Serial.println("Ważenie...");
    pinMode(HX711_VCC_PIN, OUTPUT);
    pinMode(HX711_GND_PIN, OUTPUT);
    digitalWrite(HX711_GND_PIN, LOW);
    digitalWrite(HX711_VCC_PIN, HIGH);
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    delay(500);

    float weight = 0.0;
    if (scale.is_ready()) {
        float reading = scale.get_units(5);
        weight = (reading - zero) / faktor;
        if (weight < 0 || weight > 500) weight = 0.0;
        Serial.printf("Masa: %.2f kg\n", weight);
    } else {
        Serial.println("HX711 nie odpowiada!");
    }
    delay(10);
    digitalWrite(HX711_VCC_PIN, LOW);
    return weight;
}

// ============================================================
// ================== BATERIA =================================
// ============================================================

float readBatteryVoltage() {
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += analogRead(BAT_ADC_PIN); delayMicroseconds(50); }
    float v = (sum / 16.0f) * 3.3f / 4095.0f * 2.469f;
    Serial.printf("Bateria: %.2f V\n", v);
    return v;
}

// ============================================================
// ================== PRESYNC WAKEUP ==========================
// ============================================================

void handlePresyncWakeup() {
    Serial.println("\n*** PRESYNC – korekcja czasu z DS3231 ***");
    checkResetAnytime();

    time_t now = ds3231GetTime();

    if (now == 0) {
        Serial.println("DS3231 niedostępny – fallback timer 1h");
        sleepTimer(DEFAULT_SLEEP_SEC, WAKEUP_SEND);
        return;
    }

    setSystemTime((uint32_t)now);

    uint8_t sendHour, sendMin;
    getSendTimeAfterPresync(now, sendHour, sendMin);

    Serial.printf("Presync OK → budzenie do wysłania o %02d:%02d\n", sendHour, sendMin);
    sleepUntilTime(now, sendHour, sendMin, WAKEUP_SEND);
}

// ============================================================
// ================== SEND WAKEUP =============================
// ============================================================

void handleSendWakeup() {
    Serial.println("\n*** SEND – pomiar i wysyłanie danych ***");
    checkResetAnytime();

    // ── POMIARY ──────────────────────────────────────────
    float masa, napiecie;
    if (retryCycle > 0) {
        masa     = rtcWaga;
        napiecie = rtcBateria;
        Serial.printf("Retry %d/%d | waga=%.2f kg\n",
                      retryCycle, RETRY_MAX_CYCLES, masa);
    } else {
        masa       = read_weight();
        napiecie   = readBatteryVoltage();
        rtcWaga    = masa;
        rtcBateria = napiecie;
    }

    // ── ESP-NOW INIT ──────────────────────────────────────
    WiFi.mode(WIFI_STA);
    String macStr = WiFi.macAddress();
    macStr.replace(":", "");
    strncpy(wagaMAC, macStr.c_str(), sizeof(wagaMAC));
    Serial.printf("MAC wagi: %s\n", wagaMAC);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        goSleepFallback();
        return;
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    // ── MAC CENTRALI ──────────────────────────────────────
    if (rtcMACValid) {
        memcpy(centralaMACbuf, rtcCentralaMAC, 6);
        macKnown = true;
        Serial.printf("MAC z RTC RAM: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      centralaMACbuf[0], centralaMACbuf[1], centralaMACbuf[2],
                      centralaMACbuf[3], centralaMACbuf[4], centralaMACbuf[5]);
    } else if (loadCentralaMAC(centralaMACbuf)) {
        macKnown = true;
        memcpy(rtcCentralaMAC, centralaMACbuf, 6);
        rtcMACValid = true;
        Serial.printf("MAC z NVS: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      centralaMACbuf[0], centralaMACbuf[1], centralaMACbuf[2],
                      centralaMACbuf[3], centralaMACbuf[4], centralaMACbuf[5]);
    } else {
        macKnown = runDiscovery();
    }

    if (!macKnown) {
        Serial.println("Brak centrali – idę spać");
        goSleepFallback();
        return;
    }

    // ── PEER ─────────────────────────────────────────────
    if (!esp_now_is_peer_exist(centralaMACbuf)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, centralaMACbuf, 6);
        peer.channel = 0;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println("Błąd dodawania peer");
            goSleepFallback();
            return;
        }
    }

    // ── PAKIET DANYCH ────────────────────────────────────
    DaneWagi dane = {};
    dane.magic   = MAGIC_DATA_WAGI;
    dane.waga    = masa;
    dane.bateria = napiecie;
    strncpy(dane.device_id, wagaMAC, sizeof(dane.device_id));

    // ── WYŚLIJ ───────────────────────────────────────────
    Serial.printf("Wysyłanie (okno %ds, próba %d/%d)...\n",
                  SEND_WINDOW_SEC, retryCycle + 1, RETRY_MAX_CYCLES);
    wyslanoPomyslnie = false;
    timeSyncReceived = false;
    unsigned long sendStart = millis();
    int proba = 0;
    while (millis() - sendStart < (uint32_t)SEND_WINDOW_SEC * 1000) {
        proba++;
        wyslanoPomyslnie = false;
        esp_err_t sendErr = esp_now_send(centralaMACbuf, (uint8_t*)&dane, sizeof(dane));
        if (sendErr != ESP_OK) {
            Serial.printf("esp_now_send error: %d\n", (int)sendErr);
        }
        Serial.printf("   Próba %d | %lus\n", proba, (millis() - sendStart) / 1000);
        unsigned long tw = millis();
        while (!wyslanoPomyslnie && millis() - tw < 1000) {
            checkResetAnytime();
            delay(50);
        }
        if (wyslanoPomyslnie) break;
        delay(1000);
    }

    if (wyslanoPomyslnie) {
        retryCycle = 0;
        Serial.println("DANE WYSŁANE POMYŚLNIE");

        uint32_t syncWait = ds3231Synced ? 4000 : 10000;
        Serial.printf("Czekam na sync czasu (%ums)...\n", syncWait);
        unsigned long tw = millis();
        while (!timeSyncReceived && millis() - tw < syncWait) {
            checkResetAnytime();
            delay(50);
        }

        if (timeSyncReceived) {
            uint32_t epoch = receivedTimeSync.epoch;
            setSystemTime(epoch);

            if (ds3231SetTime(epoch)) {
                if (!ds3231Synced) {
                    Serial.println("Pierwsza synchronizacja DS3231 zakonczona");
                } else {
                    Serial.println("DS3231 zaktualizowany czasem z centrali");
                }
                ds3231Synced = true;
            } else {
                Serial.println("Nie udalo sie zapisac czasu do DS3231");
            }

            uint8_t pHour, pMin;
            getNextScheduledWake((time_t)epoch, pHour, pMin, wakeupMode);
            Serial.printf("Następny presync o %02d:%02d\n", pHour, pMin);
            sleepUntilTime((time_t)epoch, pHour, pMin, wakeupMode);

        } else {
            Serial.println("Brak sync czasu od centrali");
            if (ds3231Synced) {
                time_t now = ds3231GetTime();
                if (now > 0) {
                    setSystemTime((uint32_t)now);
                    uint8_t pHour, pMin;
                    getNextScheduledWake(now, pHour, pMin, wakeupMode);
                    sleepUntilTime(now, pHour, pMin, wakeupMode);
                    return;
                }
            }
            goSleepFallback();
        }

    } else {
        // ── RETRY ────────────────────────────────────────
        retryCycle++;
        if (retryCycle < RETRY_MAX_CYCLES_IN_WINDOW) {
            Serial.printf("Brak odpowiedzi – szybka próba %d/%d, sleep %ds\n",
                          retryCycle, RETRY_MAX_CYCLES_IN_WINDOW, RETRY_SLEEP_SEC);
            sleepTimer(RETRY_SLEEP_SEC, WAKEUP_SEND);
        } else if (retryCycle < RETRY_MAX_CYCLES) {
            Serial.printf("Okno centrali prawdopodobnie minelo – próba %d/%d, sleep %lus\n",
                          retryCycle, RETRY_MAX_CYCLES, RETRY_SLEEP_LONG_SEC);
            sleepTimer(RETRY_SLEEP_LONG_SEC, WAKEUP_SEND);
        } else {
            retryCycle = 0;
            Serial.println("WYSYŁANIE NIEUDANE – wszystkie próby wyczerpane");
            goSleepFallback();
        }
    }
}

// ============================================================
// ================== SETUP ===================================
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    pinMode(DS3231_VCC_PIN, OUTPUT);
    digitalWrite(DS3231_VCC_PIN, LOW);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Interrupt na przycisku reset – aktywny przez cały cykl
   pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(RESET_BTN_PIN), onResetBtn, CHANGE);
    xTaskCreate(resetTask, "resetTask", 4096, NULL, 1, &resetTaskHandle);

    bootCount++;

    Serial.println("\n===================================");
    Serial.println("       WAGA PASIECZNA");
    Serial.printf( "   Firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf( "   Boot #%u\n", bootCount);
    Serial.println("===================================");

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool coldBoot = (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_GPIO);

    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        Serial.println(">>> Wybudzenie przez PIN RESET <<<");
    }

    Serial.printf("Wakeup: %d | mode: %s | DS3231synced: %s | retry: %d\n",
                  cause,
                  wakeupMode == WAKEUP_PRESYNC ? "PRESYNC" : "SEND",
                  ds3231Synced ? "TAK" : "NIE",
                  retryCycle);

    if (coldBoot) {
        Serial.println("Zimny start / reset – wymuszam tryb SEND");
        wakeupMode   = WAKEUP_SEND;
        retryCycle   = 0;
        rtcMACValid  = false;
        ds3231Synced = false;
    }

    // Sprawdz czy przycisk jest trzymany po wybudzeniu lub starcie.
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        if (handleResetHoldAtBoot("wakeup_gpio")) return;
    } else if (isResetButtonPressed()) {
        if (handleResetHoldAtBoot("startup")) return;
    }

    if (wakeupMode == WAKEUP_PRESYNC) {
        handlePresyncWakeup();
    } else {
        handleSendWakeup();
    }
}

void loop() {}
