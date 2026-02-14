#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "HX711.h"
#include <sys/time.h>
#include <time.h>
// ★ USUNIĘTO: #include "soc/rtc.h" – rtc_time_get() NIE działa przez deep sleep
//    Zamiast tego używamy esp_timer_get_time() który jest ciągły przez light sleep
//    ale dla deep sleep jedyną metodą jest porównanie UNIX timestampów z centrali

// ================== WERSJA FIRMWARE ==================
#define FIRMWARE_VERSION "1.3-DRIFT"

// ================== PINY HX711 ==================
#define HX711_DT_PIN    5
#define HX711_SCK_PIN   17
#define HX711_VCC_PIN   16
#define HX711_GND_PIN   18

// ================== KALIBRACJA HX711 ==================
float faktor = -23000;
float zero = -271000;

// ================== PINY BATERII ==================
const int BAT_ADC_PIN = 26;
const int BAT_GND_PIN = 33;

// ================== BLE UUID ==================
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ================== HARMONOGRAM WYBUDZANIA ==================
const int WAKEUP_HOURS[]   = {6, 20};
const int WAKEUP_MINUTES[] = {0,  0};
const int WAKEUP_COUNT     = 2;
const int MIN_SLEEP_MINUTES = 30;

// ================== STRUKTURA DRYFU RTC ==================
// ★ KLUCZOWA ZMIANA: zamiast rtc_time_get() (ticki resetu) używamy
//    UNIX timestampów z centrali – są to absolutne wartości czasu rzeczywistego.
//    Dryf = różnica między tym ile ESP32 MYŚLAŁ że minęło (deep sleep timer)
//           a ile NAPRAWDĘ minęło (wg DS3231 centrali).
struct rtc_drift_data {
    int32_t  drift_ppm_avg;          // Uśredniony dryf [ppm]
    int32_t  drift_ppm_last;         // Ostatni zmierzony dryf [ppm]
    uint32_t sync_evening_ts;        // UNIX timestamp wieczornej synchronizacji
    uint32_t sync_evening_esp_ts;    // ★ Czas ESP32 (millis/1000 od boot) przy wieczornej sync
                                     //   – NIE używamy, zamiast tego porównujemy UNIX-y
    uint32_t successful_syncs;       // Licznik udanych par (wieczór+poranek)
    uint32_t boots_since_evening;    // ★ Liczba bootów od wieczornej sync (nie dni!)
    int32_t  max_drift_seen;
    int32_t  min_drift_seen;
    bool     drift_initialized;      // Czy mamy już pierwszy pomiar dryfu
    bool     evening_recorded;       // ★ Czy mamy punkt wieczorny czekający na ranek
};

// ================== RTC DATA (zachowane przez deep sleep) ==================
RTC_DATA_ATTR int            bootCount      = 0;
RTC_DATA_ATTR bool           rtcInitialized = false;
RTC_DATA_ATTR rtc_drift_data driftData      = {
    .drift_ppm_avg       = 0,
    .drift_ppm_last      = 0,
    .sync_evening_ts     = 0,
    .sync_evening_esp_ts = 0,
    .successful_syncs    = 0,
    .boots_since_evening = 0,
    .max_drift_seen      = -999999,
    .min_drift_seen      =  999999,
    .drift_initialized   = false,
    .evening_recorded    = false
};

// ★ Osobno przechowujemy planowany czas budzenia – żeby obliczyć dryf timera
RTC_DATA_ATTR uint32_t planned_wakeup_ts   = 0;  // Kiedy MIAŁ się obudzić (UNIX)
RTC_DATA_ATTR uint32_t sleep_started_ts    = 0;  // Kiedy zasnął (UNIX)

// ================== ZMIENNE ROBOCZE ==================
bool  timeReceived     = false;
bool  deviceConnected  = false;
float masa             = 0.0;
float napiecie         = 0.0;

HX711             scale;
BLECharacteristic *pCharacteristic = nullptr;

// ================== FUNKCJE CZASU ==================
String getCurrentTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "0000-00-00 00:00:00";
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

// ================== LOGIKA DRYFU ==================

// ★ WIECZÓR: zapamiętaj UNIX timestamp synchronizacji
void recordEveningSyncPoint(uint32_t unix_ts) {
    driftData.sync_evening_ts     = unix_ts;
    driftData.boots_since_evening = 0;
    driftData.evening_recorded    = true;

    Serial.println("\n📍 PUNKT SYNCHRONIZACJI WIECZORNEJ:");
    Serial.printf("   UNIX ts: %u\n", unix_ts);
    Serial.printf("   Czas: %s\n", getCurrentTime().c_str());
}

// ★ PORANEK: oblicz dryf na podstawie UNIX timestampów
//   delta_real = czas wg DS3231 centrali (prawdziwy)
//   delta_esp  = czas wg deep sleep timera ESP32
//   Dryf = ile ESP32 się myli na milion sekund
void calculateDriftFromTimestamps(uint32_t morning_unix_ts) {
    if (!driftData.evening_recorded || driftData.sync_evening_ts == 0) {
        Serial.println("⚠️ Brak punktu wieczornego – pomijam obliczanie dryfu");
        return;
    }

    // Rzeczywisty upływ czasu wg DS3231 (przez centralę)
    int32_t delta_real = (int32_t)(morning_unix_ts - driftData.sync_evening_ts);

    if (delta_real < 3600 || delta_real > 50000) {
        Serial.printf("⚠️ Podejrzany delta_real=%d s – pomijam\n", delta_real);
        return;
    }

    // Planowany czas snu: sleep_started_ts → planned_wakeup_ts
    // To jest ile ESP32 MYŚLAŁ że minie
    if (sleep_started_ts == 0 || planned_wakeup_ts == 0) {
        Serial.println("⚠️ Brak danych sleep_started_ts / planned_wakeup_ts – pomijam");
        return;
    }

    int32_t delta_esp = (int32_t)(planned_wakeup_ts - sleep_started_ts);

    if (delta_esp < 60) {
        Serial.println("⚠️ delta_esp < 60s – dane nieprawidłowe");
        return;
    }

    // ★ Dryf: ile sekund ESP32 się spóźnił/pośpieszył na delta_real sekund rzeczywistych
    float drift_sec = (float)delta_esp - (float)delta_real;
    int32_t drift_ppm = (int32_t)((drift_sec / (float)delta_real) * 1000000.0f);

    Serial.println("\n📊 ANALIZA DRYFU:");
    Serial.printf("   Czas rzeczywisty (DS3231): %d s\n", delta_real);
    Serial.printf("   Czas ESP32 deep sleep:     %d s\n", delta_esp);
    Serial.printf("   Różnica: %.2f s\n", drift_sec);
    Serial.printf("   Dryf: %d ppm\n", drift_ppm);

    // Clamp
    drift_ppm = constrain(drift_ppm, -10000L, 10000L);
    driftData.drift_ppm_last = drift_ppm;

    // Aktualizuj EMA
    if (!driftData.drift_initialized || driftData.successful_syncs == 0) {
        driftData.drift_ppm_avg = drift_ppm;
    } else {
        // Wykryj drastyczną zmianę
        if (abs(drift_ppm - driftData.drift_ppm_avg) > 3000) {
            Serial.println("⚠️ Drastyczna zmiana dryfu – reset EMA");
            driftData.drift_ppm_avg = drift_ppm;
        } else {
            float alpha = (driftData.successful_syncs < 5) ? 0.5f : 0.25f;
            driftData.drift_ppm_avg = (int32_t)(
                driftData.drift_ppm_avg * (1.0f - alpha) + drift_ppm * alpha
            );
        }
    }

    if (drift_ppm > driftData.max_drift_seen) driftData.max_drift_seen = drift_ppm;
    if (drift_ppm < driftData.min_drift_seen) driftData.min_drift_seen = drift_ppm;

    driftData.successful_syncs++;
    driftData.drift_initialized  = true;
    driftData.evening_recorded   = false; // ★ Zresetuj – czekamy na następny wieczór

    Serial.println("\n✅ AKTUALIZACJA DRYFU:");
    Serial.printf("   Drift avg: %d ppm\n", driftData.drift_ppm_avg);
    Serial.printf("   Sync count: %u\n",    driftData.successful_syncs);
    Serial.printf("   Min/Max: %d / %d ppm\n", driftData.min_drift_seen, driftData.max_drift_seen);
}

// ★ Oblicz skorygowany czas snu
//   Jeśli ESP32 śpi ZA DŁUGO (drift_ppm > 0) → skróć czas snu
//   Jeśli ESP32 śpi ZA KRÓTKO (drift_ppm < 0) → wydłuż czas snu
long calculateCorrectedSleepTime(long planned_sleep_seconds) {
    if (!driftData.drift_initialized || driftData.successful_syncs < 1) {
        Serial.println("⚠️ Dryf nie zainicjowany – brak korekcji");
        return planned_sleep_seconds;
    }

    if (driftData.boots_since_evening > 6) {
        Serial.println("⚠️ Za dużo bootów bez sync wieczornej – brak korekcji");
        return planned_sleep_seconds;
    }

    // Korekcja: jeśli ESP śpi 1% za długo, skróć czas o 1%
    float correction_sec = (float)planned_sleep_seconds *
                           (float)driftData.drift_ppm_avg / 1000000.0f;
    long corrected = planned_sleep_seconds - (long)correction_sec;

    // ★ Bez ograniczenia 5% – przy dryfie 1667ppm na 36000s korekcja = 60s = 0.17%
    //   Ograniczamy tylko do rozsądnego maximum: 10%
    long max_corr = planned_sleep_seconds / 10;
    corrected = constrain(corrected,
                          planned_sleep_seconds - max_corr,
                          planned_sleep_seconds + max_corr);

    Serial.printf("\n🎯 KOREKCJA SNU: %ld s → %ld s (korekta %.1f s, drift=%d ppm)\n",
                  planned_sleep_seconds, corrected, correction_sec,
                  driftData.drift_ppm_avg);

    return corrected;
}

// ★ Raport dryfu do wysłania przez BLE → centrala → PHP
String getDriftReport() {
    char buf[160];
    snprintf(buf, sizeof(buf),
        "D:%d;%d;%u;%u;%d;%d",
        driftData.drift_ppm_avg,
        driftData.drift_ppm_last,
        driftData.successful_syncs,
        driftData.boots_since_evening,
        driftData.min_drift_seen,
        driftData.max_drift_seen
    );
    return String(buf);
}

// ================== OBLICZANIE NASTĘPNEGO WYBUDZENIA ==================
long calculateSecondsToNextWakeup() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return 3600;

    int ch = timeinfo.tm_hour;
    int cm = timeinfo.tm_min;
    int cs = timeinfo.tm_sec;

    Serial.printf("\n⏰ Obecny czas: %02d:%02d:%02d\n", ch, cm, cs);

    for (int i = 0; i < WAKEUP_COUNT; i++) {
        int wh = WAKEUP_HOURS[i];
        int wm = WAKEUP_MINUTES[i];

        if (wh > ch || (wh == ch && wm > cm)) {
            long secs = (wh - ch) * 3600L + (wm - cm) * 60L - cs;
            if (secs >= MIN_SLEEP_MINUTES * 60L) {
                Serial.printf("✅ Następne wybudzenie: %02d:%02d (za %ld s)\n", wh, wm, secs);
                return secs;
            }
            Serial.printf("⚠️ Slot %02d:%02d za blisko – pomijam\n", wh, wm);
        }
    }

    // Jutro rano
    int wh = WAKEUP_HOURS[0];
    int wm = WAKEUP_MINUTES[0];
    long secs = (24 - ch + wh) * 3600L + (wm - cm) * 60L - cs;
    Serial.printf("✅ Następne wybudzenie JUTRO: %02d:%02d (za %ld s)\n", wh, wm, secs);
    return secs;
}

// ================== BLE CALLBACKS ==================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s)    { deviceConnected = true;  Serial.println("✅ Centrala połączona!"); }
    void onDisconnect(BLEServer* s) { deviceConnected = false; Serial.println("❌ Centrala rozłączona"); }
};

class MyCharCallbacks : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* c) { Serial.println("📖 Centrala odczytuje dane"); }

    void onWrite(BLECharacteristic* c) {
        std::string sv = c->getValue();
        String value = String(sv.c_str());
        if (value.length() == 0) return;

        Serial.print("📥 Otrzymano: ");
        Serial.println(value);

        if (value.startsWith("TIME:")) {
            uint32_t unix_ts = (uint32_t)value.substring(5).toInt();
            if (unix_ts < 1700000000UL) {
                Serial.println("❌ Timestamp za mały – odrzucam");
                return;
            }

            // Ustaw czas systemowy
            struct timeval tv = { .tv_sec = (time_t)unix_ts, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            rtcInitialized = true;
            timeReceived   = true;

            Serial.printf("✅ Czas ustawiony: %s\n", getCurrentTime().c_str());

            // Określ pora dnia
            struct tm timeinfo;
            getLocalTime(&timeinfo);
            int hour = timeinfo.tm_hour;

            if (hour >= 5 && hour < 14) {
                // ★ PORANEK – oblicz dryf
                Serial.println("\n🌅 SYNCHRONIZACJA PORANNA");
                calculateDriftFromTimestamps(unix_ts);
            } else {
                // ★ WIECZÓR / NOC – zapisz punkt odniesienia
                Serial.println("\n🌆 SYNCHRONIZACJA WIECZORNA");
                recordEveningSyncPoint(unix_ts);
            }
        }
    }
};

// ================== HX711 ==================
void hx711_power_on() {
    digitalWrite(HX711_VCC_PIN, HIGH);
    Serial.println("⚡ HX711 ON");
    delay(500);
}

void hx711_power_off() {
    delay(10);
    digitalWrite(HX711_VCC_PIN, LOW);
    Serial.println("⚡ HX711 OFF");
}

float read_weight() {
    Serial.println("⚖️ Ważenie...");
    hx711_power_on();
    float reading = 0;
    if (scale.is_ready()) {
        reading = scale.get_units(5);
        Serial.printf("   Raw: %.2f\n", reading);
    } else {
        Serial.println("❌ HX711 błąd");
        hx711_power_off();
        return 0.0;
    }
    float weight = (reading - zero) / faktor;
    Serial.printf("✅ Masa: %.2f kg\n", weight);
    hx711_power_off();
    return weight;
}

// ================== BATERIA ==================
float readBatteryVoltage() {
    Serial.println("🔋 Pomiar baterii...");
    pinMode(BAT_GND_PIN, OUTPUT);
    digitalWrite(BAT_GND_PIN, LOW);
    delayMicroseconds(1000);

    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) { sum += analogRead(BAT_ADC_PIN); delayMicroseconds(50); }
    pinMode(BAT_GND_PIN, INPUT);

    float v_bat = (sum / 16.0f) * 3.3f / 4095.0f * 2.0f;
    Serial.printf("✅ Napięcie: %.2f V\n", v_bat);
    return v_bat;
}

// ================== SETUP ==================
void setup() {
    Serial.begin(115200);
    delay(1000);

    bootCount++;
    // ★ Inkrementuj licznik bootów od wieczornej synchronizacji
    if (driftData.evening_recorded) driftData.boots_since_evening++;

    Serial.println("\n╔════════════════════════════════╗");
    Serial.println("║    🐝 WAGA PASIECZNA          ║");
    Serial.printf( "║    Firmware: %-18s║\n", FIRMWARE_VERSION);
    Serial.println("╚════════════════════════════════╝");
    Serial.printf("Boot #%d\n", bootCount);

    // Strefa czasowa UTC (centrala wysyła UTC, nie lokalny czas)
    setenv("TZ", "UTC0", 1);
    tzset();

    if (rtcInitialized) {
        Serial.printf("⏰ Czas: %s\n", getCurrentTime().c_str());
        if (driftData.drift_initialized) {
            Serial.printf("📊 Drift avg=%d ppm  last=%d ppm  syncs=%u  boots_since_eve=%u\n",
                driftData.drift_ppm_avg, driftData.drift_ppm_last,
                driftData.successful_syncs, driftData.boots_since_evening);
        }
    } else {
        Serial.println("⚠️ RTC nie zainicjalizowany – czekam na synchronizację");
    }

    // Konfiguracja pinów
    pinMode(HX711_VCC_PIN, OUTPUT);
    pinMode(HX711_GND_PIN, OUTPUT);
    digitalWrite(HX711_GND_PIN, LOW);
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    pinMode(BAT_GND_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    // Pomiary
    masa = read_weight();
    if (masa < 0 || masa > 500) { Serial.println("⚠️ Nieprawidłowy pomiar – reset do 0.00"); masa = 0.0; }
    napiecie = readBatteryVoltage();

    // Dane BLE: "masa;napiecie;D:avg;last;syncs;boots;min;max"
    String daneDoWyslania = String(masa, 2) + ";" + String(napiecie, 2) + ";" + getDriftReport();
    Serial.printf("\n📦 Dane BLE: %s\n", daneDoWyslania.c_str());

    // BLE
    Serial.println("\n🔵 Uruchamiam BLE...");
    BLEDevice::init("Waga_Pasieka_1");
    BLEServer    *pServer  = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService   *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    pCharacteristic->setCallbacks(new MyCharCallbacks());
    pCharacteristic->setValue(daneDoWyslania.c_str());
    pService->start();

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.println("📡 Rozgłaszam BLE (max 60s)...\n");

    // Czekaj na centralę
    unsigned long startWait = millis();
    while (!timeReceived) {
        delay(500);
        if (millis() - startWait > 60000) { Serial.println("⏱️ TIMEOUT – brak centrali"); break; }
        if ((millis() - startWait) % 10000 < 500)
            Serial.printf("⏳ Czekam... %lus\n", (millis() - startWait) / 1000);
    }

    Serial.println(timeReceived ?
        "\n╔════════════════════════════════╗\n║  ✅ SYNCHRONIZACJA OK         ║\n╚════════════════════════════════╝" :
        "\n╔════════════════════════════════╗\n║  ⚠️  BRAK SYNCHRONIZACJI      ║\n╚════════════════════════════════╝");

    delay(500);

    // Oblicz czas snu
    long sleepSeconds;
    if (rtcInitialized) {
        long planned = calculateSecondsToNextWakeup();
        sleepSeconds = calculateCorrectedSleepTime(planned);

        // ★ Zapisz kiedy zasnęliśmy i kiedy planujemy wstać
        time_t now_ts;
        time(&now_ts);
        sleep_started_ts  = (uint32_t)now_ts;
        planned_wakeup_ts = (uint32_t)(now_ts + sleepSeconds);
    } else {
        sleepSeconds     = 3600;
        sleep_started_ts = 0;
        planned_wakeup_ts = 0;
        Serial.println("⚠️ Brak czasu – wybudzenie za 1h");
    }

    sleepSeconds = constrain(sleepSeconds, 60L, 86400L);

    Serial.printf("\n💤 Deep sleep przez %ld s\n", sleepSeconds);
    if (rtcInitialized) {
        time_t wake = (time_t)planned_wakeup_ts;
        struct tm *wt = localtime(&wake);
        char wb[20]; strftime(wb, sizeof(wb), "%Y-%m-%d %H:%M:%S", wt);
        Serial.printf("⏰ Przewidywane wybudzenie: %s\n", wb);
    }
    Serial.println("═══════════════════════════════════\n");
    delay(500);

    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}
