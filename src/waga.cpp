#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "HX711.h"
#include <sys/time.h>
#include <time.h>
#include "soc/rtc.h"

// ================== WERSJA FIRMWARE ==================
#define FIRMWARE_VERSION "1.2-DRIFT"

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
const int WAKEUP_HOURS[] = {6, 20};    // Godziny wybudzenia: 6:00 i 20:00
const int WAKEUP_MINUTES[] = {0, 0};   // Minuty wybudzenia
const int WAKEUP_COUNT = 2;

// ================== STRUKTURA DRYFU RTC ==================
struct rtc_drift_data {
    int32_t drift_ppm_avg;           // Uśredniony dryf w ppm
    int32_t drift_ppm_last;          // Ostatni zmierzony dryf
    uint32_t last_sync_timestamp;    // Timestamp ostatniej synchronizacji
    uint64_t last_sync_rtc_ticks;    // RTC ticki przy synchronizacji
    uint32_t successful_syncs;       // Licznik udanych synchronizacji
    uint32_t days_without_sync;      // Dni bez synchronizacji
    int32_t max_drift_seen;          // Maksymalny zaobserwowany dryf
    int32_t min_drift_seen;          // Minimalny zaobserwowany dryf
    bool drift_initialized;          // Czy system dryfu jest zainicjowany
};

// ================== RTC DATA (zachowane w deep sleep) ==================
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool rtcInitialized = false;
RTC_DATA_ATTR rtc_drift_data driftData = {
    .drift_ppm_avg = 0,
    .drift_ppm_last = 0,
    .last_sync_timestamp = 0,
    .last_sync_rtc_ticks = 0,
    .successful_syncs = 0,
    .days_without_sync = 0,
    .max_drift_seen = -999999,
    .min_drift_seen = 999999,
    .drift_initialized = false
};

// ================== ZMIENNE ROBOCZE ==================
bool timeReceived = false;
bool deviceConnected = false;
float masa = 0.0;
float napiecie = 0.0;

HX711 scale;
BLECharacteristic *pCharacteristic = nullptr;

// ================== FUNKCJE RTC ==================
void setSystemTime(int year, int month, int day, int hour, int minute, int second) {
  struct tm timeinfo;
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = hour;
  timeinfo.tm_min = minute;
  timeinfo.tm_sec = second;
  
  time_t t = mktime(&timeinfo);
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  
  Serial.println("⏰ Czas systemowy zaktualizowany");
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "0000-00-00 00:00:00";
  }
  
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void getCurrentTimeComponents(int &hour, int &minute, int &second) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    second = timeinfo.tm_sec;
  } else {
    hour = minute = second = 0;
  }
}

// ================== FUNKCJE DRYFU RTC ==================

// Pobierz aktualne ticki RTC
uint64_t getRtcTicks() {
    return rtc_time_get();
}

// Pobierz częstotliwość RTC (Hz) - dla ESP32 to zazwyczaj ~150kHz
uint32_t getRtcFrequency() {
    return rtc_clk_slow_freq_get_hz();
}

// Zapisz punkt synchronizacji (wieczór)
void recordSyncPoint(uint32_t timestamp) {
    // Poczekaj chwilę na stabilizację po wybudzeniu
    delay(100);
    
    driftData.last_sync_timestamp = timestamp;
    driftData.last_sync_rtc_ticks = getRtcTicks();
    driftData.days_without_sync = 0;
    
    Serial.println("\n📍 PUNKT SYNCHRONIZACJI:");
    Serial.print("   Timestamp: ");
    Serial.println(timestamp);
    Serial.print("   RTC ticks: ");
    Serial.println((unsigned long)driftData.last_sync_rtc_ticks);
    Serial.print("   RTC freq: ");
    Serial.print(getRtcFrequency());
    Serial.println(" Hz");
}

// Oblicz i zaktualizuj dryf (poranek)
void calculateAndUpdateDrift(uint32_t current_timestamp) {
    if (!driftData.drift_initialized || driftData.last_sync_timestamp == 0) {
        Serial.println("⚠️ Brak punktu odniesienia - pomijam obliczanie dryfu");
        return;
    }
    
    // Poczekaj chwilę na stabilizację po wybudzeniu
    delay(100);
    
    uint64_t current_rtc_ticks = getRtcTicks();
    uint32_t rtc_freq = getRtcFrequency();
    
    // Rzeczywisty upływ czasu według centrali
    int32_t delta_real = current_timestamp - driftData.last_sync_timestamp;
    
    // Upływ czasu według RTC
    uint64_t delta_ticks = current_rtc_ticks - driftData.last_sync_rtc_ticks;
    float delta_rtc = (float)delta_ticks / (float)rtc_freq;
    
    // Oblicz dryf
    float drift = delta_rtc - (float)delta_real;
    int32_t drift_ppm = (int32_t)((drift / (float)delta_real) * 1000000.0);
    
    Serial.println("\n📊 ANALIZA DRYFU:");
    Serial.print("   Czas rzeczywisty: ");
    Serial.print(delta_real);
    Serial.println(" s");
    Serial.print("   Czas RTC: ");
    Serial.print(delta_rtc, 2);
    Serial.println(" s");
    Serial.print("   Różnica: ");
    Serial.print(drift, 2);
    Serial.println(" s");
    Serial.print("   Dryf: ");
    Serial.print(drift_ppm);
    Serial.println(" ppm");
    
    // Zabezpieczenie - clamp dryfu
    if (drift_ppm > 5000) {
        Serial.println("⚠️ Dryf > 5000 ppm - ograniczam do 5000");
        drift_ppm = 5000;
    }
    if (drift_ppm < -5000) {
        Serial.println("⚠️ Dryf < -5000 ppm - ograniczam do -5000");
        drift_ppm = -5000;
    }
    
    driftData.drift_ppm_last = drift_ppm;
    
    // Watchdog - jeśli dryf drastycznie się zmienił, zrób hard reset EMA
    if (driftData.successful_syncs > 3) {
        int32_t drift_delta = abs(drift_ppm - driftData.drift_ppm_avg);
        if (drift_delta > 2000) {
            Serial.println("⚠️ Drastyczna zmiana dryfu - hard reset EMA!");
            driftData.drift_ppm_avg = drift_ppm;
            driftData.successful_syncs = 1; // Reset licznika
        } else {
            // Normalna aktualizacja EMA
            // Cold start - szybsze uczenie
            float alpha = (driftData.successful_syncs < 10) ? 0.4 : 0.2;
            driftData.drift_ppm_avg = (int32_t)(
                driftData.drift_ppm_avg * (1.0 - alpha) + 
                drift_ppm * alpha
            );
        }
    } else {
        // Pierwsze pomiary - po prostu zapisz
        driftData.drift_ppm_avg = drift_ppm;
    }
    
    // Aktualizuj statystyki
    if (drift_ppm > driftData.max_drift_seen) {
        driftData.max_drift_seen = drift_ppm;
    }
    if (drift_ppm < driftData.min_drift_seen) {
        driftData.min_drift_seen = drift_ppm;
    }
    
    driftData.successful_syncs++;
    driftData.drift_initialized = true;
    
    Serial.println("\n✅ AKTUALIZACJA DRYFU:");
    Serial.print("   Drift avg: ");
    Serial.print(driftData.drift_ppm_avg);
    Serial.println(" ppm");
    Serial.print("   Sync count: ");
    Serial.println(driftData.successful_syncs);
    Serial.print("   Min/Max: ");
    Serial.print(driftData.min_drift_seen);
    Serial.print(" / ");
    Serial.print(driftData.max_drift_seen);
    Serial.println(" ppm");
}

// Oblicz skorygowany czas snu
long calculateCorrectedSleepTime(long planned_sleep_seconds) {
    if (!driftData.drift_initialized || driftData.successful_syncs < 2) {
        Serial.println("⚠️ Dryf nie zainicjowany - brak korekcji");
        return planned_sleep_seconds;
    }
    
    // Jeśli dawno nie było sync, nie koryguj (dryf nieprzewidywalny)
    if (driftData.days_without_sync > 2) {
        Serial.println("⚠️ Brak sync > 2 dni - brak korekcji");
        return planned_sleep_seconds;
    }
    
    // Oblicz korekcję
    float correction = (float)planned_sleep_seconds * (float)driftData.drift_ppm_avg / 1000000.0;
    long corrected_sleep = planned_sleep_seconds - (long)correction;
    
    Serial.println("\n🎯 KOREKCJA CZASU SNU:");
    Serial.print("   Planowany: ");
    Serial.print(planned_sleep_seconds);
    Serial.println(" s");
    Serial.print("   Korekcja: ");
    Serial.print(correction, 2);
    Serial.println(" s");
    Serial.print("   Skorygowany: ");
    Serial.print(corrected_sleep);
    Serial.println(" s");
    
    // Zabezpieczenie - maksymalna korekcja ±5%
    long max_correction = planned_sleep_seconds / 20; // 5%
    if (abs(corrected_sleep - planned_sleep_seconds) > max_correction) {
        Serial.println("⚠️ Korekcja > 5% - ograniczam");
        if (corrected_sleep > planned_sleep_seconds) {
            corrected_sleep = planned_sleep_seconds + max_correction;
        } else {
            corrected_sleep = planned_sleep_seconds - max_correction;
        }
    }
    
    return corrected_sleep;
}

// Generuj raport dryfu do wysłania
String getDriftReport() {
    char buffer[150];
    snprintf(buffer, sizeof(buffer), 
        "D:%d;%d;%d;%d;%d;%d",
        driftData.drift_ppm_avg,
        driftData.drift_ppm_last,
        driftData.successful_syncs,
        driftData.days_without_sync,
        driftData.min_drift_seen,
        driftData.max_drift_seen
    );
    return String(buffer);
}

// Oblicz ile sekund do następnego wybudzenia
long calculateSecondsToNextWakeup() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // Jeśli nie ma czasu, wybudź za 1 godzinę
    return 3600;
  }
  
  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int currentSecond = timeinfo.tm_sec;
  
  // Sprawdź każdy zaplanowany czas wybudzenia
  for (int i = 0; i < WAKEUP_COUNT; i++) {
    int wakeHour = WAKEUP_HOURS[i];
    int wakeMinute = WAKEUP_MINUTES[i];
    
    // Jeśli ten czas jest w przyszłości dzisiaj
    if (wakeHour > currentHour || 
        (wakeHour == currentHour && wakeMinute > currentMinute)) {
      
      // Oblicz sekundy do tego czasu
      int hoursUntil = wakeHour - currentHour;
      int minutesUntil = wakeMinute - currentMinute;
      int secondsUntil = -currentSecond;
      
      long totalSeconds = hoursUntil * 3600 + minutesUntil * 60 + secondsUntil;
      
      Serial.print("⏰ Następne wybudzenie za: ");
      Serial.print(totalSeconds / 3600);
      Serial.print("h ");
      Serial.print((totalSeconds % 3600) / 60);
      Serial.print("m ");
      Serial.print(totalSeconds % 60);
      Serial.println("s");
      
      return totalSeconds;
    }
  }
  
  // Wszystkie czasy dzisiaj minęły - następne jutro rano
  int wakeHour = WAKEUP_HOURS[0];
  int wakeMinute = WAKEUP_MINUTES[0];
  
  int hoursUntil = (24 - currentHour) + wakeHour;
  int minutesUntil = wakeMinute - currentMinute;
  int secondsUntil = -currentSecond;
  
  long totalSeconds = hoursUntil * 3600 + minutesUntil * 60 + secondsUntil;
  
  Serial.print("⏰ Następne wybudzenie jutro za: ");
  Serial.print(totalSeconds / 3600);
  Serial.print("h ");
  Serial.print((totalSeconds % 3600) / 60);
  Serial.print("m");
  Serial.println();
  
  return totalSeconds;
}

// ================== CALLBACK SERWERA BLE ==================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("✅ Centrala połączona!");
    }
    
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("❌ Centrala rozłączona");
    }
};

// ================== CALLBACK CHARAKTERYSTYKI BLE ==================
class MyCharCallbacks: public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* pCharacteristic) {
        Serial.println("📖 Centrala odczytuje dane");
    }
    
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string stdValue = pCharacteristic->getValue();
        String value = String(stdValue.c_str());
        
        if (value.length() > 0) {
            Serial.print("📥 Otrzymano: ");
            Serial.println(value);
            
            // Format: "TIME:1234567890"
            if (value.startsWith("TIME:")) {
                String timestampStr = value.substring(5);
                uint32_t timestamp = timestampStr.toInt();
                
                // Ustaw czas systemowy
                time_t t = (time_t)timestamp;
                struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                
                rtcInitialized = true;
                timeReceived = true;
                
                Serial.print("✅ Czas ustawiony: ");
                Serial.println(getCurrentTime());
                
                // Określ czy to poranek czy wieczór
                struct tm timeinfo;
                getLocalTime(&timeinfo);
                int hour = timeinfo.tm_hour;
                
                if (hour >= 5 && hour < 12) {
                    // PORANEK - oblicz dryf
                    Serial.println("\n🌅 SYNCHRONIZACJA PORANNA");
                    calculateAndUpdateDrift(timestamp);
                } else {
                    // WIECZÓR - zapisz punkt synchronizacji
                    Serial.println("\n🌆 SYNCHRONIZACJA WIECZORNA");
                    recordSyncPoint(timestamp);
                }
            }
        }
    }
};

// ================== FUNKCJA: Włącz zasilanie HX711 ==================
void hx711_power_on() {    
    digitalWrite(HX711_VCC_PIN, HIGH);
    Serial.println("⚡ HX711 ON");
    delay(500);
}

// ================== FUNKCJA: Wyłącz zasilanie HX711 ==================
void hx711_power_off() {    
    delay(10);
    digitalWrite(HX711_VCC_PIN, LOW);
    Serial.println("⚡ HX711 OFF");
}

// ================== FUNKCJA: Pomiar masy ==================
float read_weight() {
    Serial.println("⚖️ Ważenie...");
    hx711_power_on();
    
    float reading;
    if (scale.is_ready()) {
        reading = scale.get_units(5);
        Serial.print("   Raw: ");
        Serial.println(reading);
    } else {
        Serial.println("❌ HX711 błąd");
        hx711_power_off();
        return 0.0;
    }
    
    float weight = (reading - zero) / faktor;
    
    Serial.print("✅ Masa: ");
    Serial.print(weight, 2);
    Serial.println(" kg");
    
    hx711_power_off();
    return weight;
}

// ================== FUNKCJA: Pomiar napięcia baterii ==================
float readBatteryVoltage() {
  Serial.println("🔋 Pomiar baterii...");
  
  pinMode(BAT_GND_PIN, OUTPUT);
  digitalWrite(BAT_GND_PIN, LOW);
  delayMicroseconds(1000);

  uint32_t sum = 0;
  const int samples = 16;

  for (int i = 0; i < samples; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delayMicroseconds(50);
  }

  pinMode(BAT_GND_PIN, INPUT);

  float raw = sum / (float)samples;
  float v_adc = raw * 3.3 / 4095.0;
  float v_bat = v_adc * 2.0;

  Serial.print("✅ Napięcie: ");
  Serial.print(v_bat, 2);
  Serial.println(" V");

  return v_bat;
}

// ================== SETUP ==================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    bootCount++;
    
    Serial.println("\n╔════════════════════════════════╗");
    Serial.println("║    🐝 WAGA PASIECZNA          ║");
    Serial.print("║    Firmware: ");
    Serial.print(FIRMWARE_VERSION);
    Serial.println("        ║");
    Serial.println("╚════════════════════════════════╝");
    Serial.print("Boot #");
    Serial.println(bootCount);
    
    // Inkrementuj licznik dni bez sync
    if (rtcInitialized) {
        driftData.days_without_sync++;
    }
    
    // Konfiguruj strefy czasowe (UTC+1 dla Polski)
    setenv("TZ", "UTC0", 1);
    tzset();
    
    if (rtcInitialized) {
        Serial.print("⏰ Czas: ");
        Serial.println(getCurrentTime());
        
        // Pokaż status dryfu
        if (driftData.drift_initialized) {
            Serial.println("\n📊 STATUS DRYFU RTC:");
            Serial.print("   Drift avg: ");
            Serial.print(driftData.drift_ppm_avg);
            Serial.println(" ppm");
            Serial.print("   Last drift: ");
            Serial.print(driftData.drift_ppm_last);
            Serial.println(" ppm");
            Serial.print("   Syncs: ");
            Serial.println(driftData.successful_syncs);
            Serial.print("   Days w/o sync: ");
            Serial.println(driftData.days_without_sync);
        }
    } else {
        Serial.println("⚠️ RTC nie zainicjalizowany - czekam na synchronizację");
    }
    
    // Konfiguracja pinów
    pinMode(HX711_VCC_PIN, OUTPUT);
    pinMode(HX711_GND_PIN, OUTPUT);
    digitalWrite(HX711_GND_PIN, LOW);
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    
    pinMode(BAT_GND_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    // === POMIARY ===
    masa = read_weight();
    if (masa < 0 || masa > 500) {
        Serial.println("⚠️ Nieprawidłowy pomiar - reset do 0.00");
        masa = 0.0;
    }
    
    napiecie = readBatteryVoltage();

    // === PRZYGOTOWANIE DANYCH ===
    // Format: "masa;napiecie;drift_report"
    String daneDoWyslania = String(masa, 2) + ";" + String(napiecie, 2) + ";" + getDriftReport();
    Serial.print("\n📦 Dane BLE: ");
    Serial.println(daneDoWyslania);

    // === INICJALIZACJA BLE ===
    Serial.println("\n🔵 Uruchamiam BLE...");
    BLEDevice::init("Waga_Pasieka_1");
    
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    BLEService *pService = pServer->createService(SERVICE_UUID);
    
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE
    );
    
    pCharacteristic->setCallbacks(new MyCharCallbacks());
    pCharacteristic->setValue(daneDoWyslania.c_str());
    
    pService->start();
    
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    
    BLEDevice::startAdvertising();
    Serial.println("📡 Rozgłaszam BLE (max 60s)...\n");
    
    // === OCZEKIWANIE NA CENTRALĘ ===
    unsigned long startWait = millis();
    
    while (!timeReceived) {
        delay(500);
        
        if (millis() - startWait > 60000) {
            Serial.println("⏱️ TIMEOUT - brak centrali");
            break;
        }
        
        if ((millis() - startWait) % 10000 < 500) {
            Serial.print("⏳ Czekam... ");
            Serial.print((millis() - startWait) / 1000);
            Serial.println("s");
        }
    }
    
    // === PODSUMOWANIE ===
    if (timeReceived) {
        Serial.println("\n╔════════════════════════════════╗");
        Serial.println("║  ✅ SYNCHRONIZACJA OK         ║");
        Serial.println("╚════════════════════════════════╝");
    } else {
        Serial.println("\n╔════════════════════════════════╗");
        Serial.println("║  ⚠️ BRAK SYNCHRONIZACJI       ║");
        Serial.println("╚════════════════════════════════╝");
    }
    
    delay(1000);
    
    // === OBLICZ CZAS DO NASTĘPNEGO WYBUDZENIA ===
    long sleepSeconds;
    
    if (rtcInitialized) {
        long planned_sleep = calculateSecondsToNextWakeup();
        sleepSeconds = calculateCorrectedSleepTime(planned_sleep);
    } else {
        // Bez synchronizacji czasu - wybudź za 1h
        sleepSeconds = 3600;
        Serial.println("⚠️ Brak czasu - wybudzenie za 1h");
    }
    
    // Zabezpieczenie - minimum 60s, maksimum 24h
    if (sleepSeconds < 60) sleepSeconds = 60;
    if (sleepSeconds > 86400) sleepSeconds = 86400;
    
    Serial.print("\n💤 Deep sleep przez ");
    Serial.print(sleepSeconds);
    Serial.println(" sekund");
    
    // Pokaż przewidywany czas wybudzenia
    if (rtcInitialized) {
        time_t now;
        time(&now);
        time_t wake = now + sleepSeconds;
        struct tm* wake_time = localtime(&wake);
        char wake_buffer[20];
        strftime(wake_buffer, sizeof(wake_buffer), "%Y-%m-%d %H:%M:%S", wake_time);
        Serial.print("⏰ Przewidywane wybudzenie: ");
        Serial.println(wake_buffer);
    }
    
    Serial.println("═══════════════════════════════════\n");
    
    delay(1000);
    
    // === DEEP SLEEP ===
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {
    // Pusta - działanie w setup()
}