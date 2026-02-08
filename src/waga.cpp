#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "HX711.h"
#include <sys/time.h>
#include <time.h>

// ================== WERSJA FIRMWARE ==================
#define FIRMWARE_VERSION "1.1"

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

// ================== RTC DATA (zachowane w deep sleep) ==================
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool rtcInitialized = false;

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
        // POPRAWKA: użyj std::string zamiast String przez c_str()
        std::string stdValue = pCharacteristic->getValue();
        String value = String(stdValue.c_str());
        
        if (value.length() > 0) {
            Serial.print("📥 Otrzymano: ");
            Serial.println(value);
            
    // Format: "TIME:2026-02-08 18:30:15"
    if (value.startsWith("TIME:")) {
    String timestampStr = value.substring(5);
    uint32_t timestamp = timestampStr.toInt();
    
    // POPRAWKA: jawne rzutowanie typu
    time_t t = (time_t)timestamp;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    
    rtcInitialized = true;
    timeReceived = true;
    
    Serial.print("✅ Czas ustawiony: ");
    Serial.println(getCurrentTime());
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
    Serial.println("             ║");
    Serial.println("╚════════════════════════════════╝");
    Serial.print("Boot #");
    Serial.println(bootCount);
    
    // Konfiguruj strefy czasowe (UTC+1 dla Polski)
    setenv("TZ", "UTC0", 1);
    tzset();
    
    if (rtcInitialized) {
        Serial.print("⏰ Czas: ");
        Serial.println(getCurrentTime());
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
    String daneDoWyslania = String(masa, 2) + ";" + String(napiecie, 2);
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
        sleepSeconds = calculateSecondsToNextWakeup();
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
    Serial.println("═══════════════════════════════════\n");
    
    delay(1000);
    
    // === DEEP SLEEP ===
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {
    // Pusta - działanie w setup()
}
