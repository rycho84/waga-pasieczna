#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "HX711.h"

// === PINY HX711 ===
#define HX711_DT_PIN    5   // Data pin
#define HX711_SCK_PIN   17    // Clock pin  
#define HX711_VCC_PIN   16   // Zasilanie (kontrolowane programowo)
#define HX711_GND_PIN   18
float faktor = -23000;
float zero = -271000;
// === PINY BATERII (z drugiego kodu) ===
const int BAT_ADC_PIN = 26;   // GPIO26 – ADC
const int BAT_GND_PIN = 33;   // GPIO33 – udawana masa dzielnika

// === BLE UUID ===
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

     

// === ZMIENNE RTC (zachowane przez deep sleep) ===
RTC_DATA_ATTR long sleepDuration = 180;  // Domyślnie 180s

// === ZMIENNE ROBOCZE ===
bool timeReceived = false;
bool deviceConnected = false;
float masa = 0.0;
float napiecie = 0.0;

HX711 scale;
BLECharacteristic *pCharacteristic = nullptr;

// === CALLBACK SERWERA BLE ===
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("WAGA: Centrala się połączyła!");
    }
    
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("WAGA: Centrala się rozłączyła.");
    }
};

// === CALLBACK CHARAKTERYSTYKI BLE ===
class MyCharCallbacks: public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic* pCharacteristic) {
        Serial.println("WAGA: Centrala odczytuje dane!");
    }
    
    void onWrite(BLECharacteristic *pCharacteristic) {
        String value = pCharacteristic->getValue().c_str();
        if (value.length() > 0) {
            sleepDuration = value.toInt();
            timeReceived = true;
            Serial.print("WAGA: Otrzymano nowy czas snu: ");
            Serial.print(sleepDuration);
            Serial.println(" sekund");
        }
    }
};

// === FUNKCJA: Włącz zasilanie HX711 ===
void hx711_power_on() {    
    digitalWrite(HX711_VCC_PIN, HIGH);
    Serial.println("WAGA: HX711 zasilanie ON");
    delay(500);  // Daj czas na stabilizację zasilania
}

// === FUNKCJA: Wyłącz zasilanie HX711 ===
void hx711_power_off() {    
    delay(10);
    digitalWrite(HX711_VCC_PIN, LOW);  // Potem odcięcie zasilania   
    Serial.println("WAGA: HX711 zasilanie OFF");
}

// === FUNKCJA: Pomiar masy ===
float read_weight() {
    Serial.println("WAGA: Inicjalizacja HX711...");      
    hx711_power_on();
    float reading;
    if (scale.is_ready()) {
        reading = scale.get_units(5);	
        Serial.print("HX711 reading: ");
        Serial.println(reading);
    } else {
        Serial.println("HX711 not found.");
    }
    
    float weight = (reading - zero)/faktor;
    
    Serial.print("WAGA: Zmierzona masa: ");
    Serial.print(weight, 2);
    Serial.println(" kg");
    
    hx711_power_off();
    
    return weight;
}

// === FUNKCJA: Pomiar napięcia baterii (zaimportowana) ===
float readBatteryVoltage() {
  Serial.println("BAT: Rozpoczynam pomiar napięcia...");
  
  // 1. Włącz dzielnik (podaj GND na rezystor)
  pinMode(BAT_GND_PIN, OUTPUT);
  digitalWrite(BAT_GND_PIN, LOW);

  delayMicroseconds(1000); // stabilizacja

  // 2. Uśrednianie
  uint32_t sum = 0;
  const int samples = 16;

  for (int i = 0; i < samples; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delayMicroseconds(50);
  }

  // 3. Wyłącz dzielnik (oszczędzanie energii)
  pinMode(BAT_GND_PIN, INPUT);

  float raw = sum / (float)samples;
  // Przeliczenie: (raw / 4095) * 3.3V * 2 (dzielnik 1:1)
  float v_adc = raw * 3.3 / 4095.0;
  float v_bat = v_adc * 2.0;   

  Serial.print("BAT: Napięcie: ");
  Serial.print(v_bat, 2);
  Serial.println(" V");

  return v_bat;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    pinMode(HX711_VCC_PIN, OUTPUT);
    pinMode(HX711_GND_PIN, OUTPUT);  
    digitalWrite(HX711_GND_PIN, LOW);  
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    Serial.println("\n=== SYSTEM START ===");
    
    // === KONFIGURACJA ADC DLA BATERII ===
    // Dzielnik domyślnie WYŁĄCZONY
    pinMode(BAT_GND_PIN, INPUT); 
    analogReadResolution(12);                  
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    // === POMIAR MASY ===
    masa = read_weight();
    if (masa < 0 || masa > 500) {
        Serial.println("WAGA: UWAGA - Nieprawidłowy pomiar wagi, używam 0.00");
        masa = 0.0;
    }
    
    // === POMIAR BATERII ===
    napiecie = readBatteryVoltage();

    // === PRZYGOTOWANIE DANYCH ===
    // Format: "WAGA;NAPIĘCIE" np. "12.50;4.15"
    String daneDoWyslania = String(masa, 2) + ";" + String(napiecie, 2);
    Serial.print("BLE: Dane do wysłania: ");
    Serial.println(daneDoWyslania);

    // === INICJALIZACJA BLE ===
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
    
    // Ustawienie wartości charakterystyki
    pCharacteristic->setValue(daneDoWyslania.c_str());
    
    pService->start();
    Serial.println("BLE: Serwis uruchomiony");
    
    // === ROZGŁASZANIE BLE ===
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    
    BLEDevice::startAdvertising();
    Serial.println("BLE: Rozgłaszam się! Czekam na centralę (max 60s)\n");
    
    // === OCZEKIWANIE NA CENTRALĘ ===
    unsigned long startWait = millis();
    
    while (!timeReceived) {
        delay(500);
        
        // Timeout 60s
        if (millis() - startWait > 60000) {
            Serial.println("SYSTEM: TIMEOUT - nie doczekano się centrali");
            break;
        }
        
        // Info co 10s
        if ((millis() - startWait) % 10000 < 500) {
            Serial.print("BLE: Czekam... (");
            Serial.print((millis() - startWait) / 1000);
            Serial.println("s)");
        }
    }
    
    // === PODSUMOWANIE ===
    if (timeReceived) {
        Serial.println("\n=== SYSTEM: ✓ Sukces! ✓ ===");
    } else {
        Serial.println("\n=== SYSTEM: Timeout - używam domyślnego czasu ===");
    }
    
    Serial.print("SYSTEM: Idę spać na ");
    Serial.print(sleepDuration);
    Serial.println(" sekund...\n");
    
    delay(1000);
    
    // === DEEP SLEEP ===
    esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
    Serial.println("SYSTEM: Dobranoc! 😴");
    esp_deep_sleep_start();
}

void loop() {
    // Pusta pętla - działanie w setup()
}