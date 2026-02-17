//uint8_t centralaMAC[] = {0xFC, 0xB4, 0x67, 0x67, 0xAB, 0x18};
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "HX711.h"

// ================== WERSJA ==================
#define FIRMWARE_VERSION "2.0-ESPNOW"

// ================== PINY HX711 ==================
#define HX711_DT_PIN    20
#define HX711_SCK_PIN   21
#define HX711_VCC_PIN   22   // ← zmień jeśli inny pin
#define HX711_GND_PIN   19   // ← zmień jeśli inny pin

// ================== KALIBRACJA ==================
// ★ Skopiuj wartości ze swojego starego projektu!
const float zero   = -271000;    // ← wklej swoją wartość zero
const float faktor = -23000;    // ← wklej swój faktor

// ================== BATERIA ==================
#define BAT_ADC_PIN     4    // ← zmień jeśli inny pin

// ================== ESP-NOW ==================
// Adres MAC centrali - wpisz swój!
uint8_t centralaMAC[] = {0xFC, 0xB4, 0x67, 0x67, 0xAB, 0x18};

// Struktura danych - musi być identyczna w centrala.cpp!
typedef struct {
    float waga;
    float bateria;
    char  device_id[20];
} DaneWagi;

DaneWagi dane;
bool wyslanoPomyslnie = false;

// ================== OBIEKTY ==================
HX711 scale;

// ================== CALLBACK ESP-NOW ==================
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    wyslanoPomyslnie = (status == ESP_NOW_SEND_SUCCESS);
    Serial.print("📤 Status wysłania: ");
    Serial.println(wyslanoPomyslnie ? "✅ Sukces" : "❌ Błąd");
}

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
        Serial.println("❌ HX711 nie odpowiada!");
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
    
       delayMicroseconds(1000);
    
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(BAT_ADC_PIN);
        delayMicroseconds(50);
    }
        
    float v_bat = (sum / 16.0f) * 3.3f / 4095.0f * 2.0f;
    Serial.printf("✅ Napięcie: %.2f V\n", v_bat);
    return v_bat;
}

// ================== SETUP ==================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔════════════════════════════════╗");
    Serial.println("║    🐝 WAGA PASIECZNA          ║");
    Serial.printf( "║    Firmware: %-18s║\n", FIRMWARE_VERSION);
    Serial.println("╚════════════════════════════════╝");
    
    // Konfiguracja pinów HX711
    pinMode(HX711_VCC_PIN, OUTPUT);
    pinMode(HX711_GND_PIN, OUTPUT);
    digitalWrite(HX711_GND_PIN, LOW);
    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    
    // Konfiguracja ADC baterii    
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    
    // Pomiary
    float masa     = read_weight();
    float napiecie = readBatteryVoltage();
    
    // Walidacja wagi
    if (masa < 0 || masa > 500) {
        Serial.println("⚠️ Nieprawidłowy pomiar wagi – ustawiam 0.00");
        masa = 0.0;
    }
    
    Serial.printf("\n📦 Dane do wysłania: waga=%.2f kg, bat=%.2f V\n", masa, napiecie);
    
    // Wypełnij strukturę
    dane.waga    = masa;
    dane.bateria = napiecie;
    strncpy(dane.device_id, "Waga_01", sizeof(dane.device_id));
    
    // ================== ESP-NOW ==================
    WiFi.mode(WIFI_STA);
    Serial.print("\n📍 MAC wagi: ");
    Serial.println(WiFi.macAddress());
    
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Błąd ESP-NOW init!");
        return;
    }
    Serial.println("✅ ESP-NOW zainicjalizowany");
    
    esp_now_register_send_cb(onDataSent);
    
    // Dodaj centralę jako peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, centralaMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ Błąd dodawania peer!");
        return;
    }
    Serial.println("✅ Centrala dodana jako peer");
    
    // Wyślij dane (3 próby)
    for (int proba = 1; proba <= 3; proba++) {
        Serial.printf("\n📡 Wysyłanie (próba %d/3)...\n", proba);
        
        esp_err_t result = esp_now_send(centralaMAC, (uint8_t*)&dane, sizeof(dane));
        
        if (result != ESP_OK) {
            Serial.println("❌ Błąd wywołania esp_now_send!");
        } else {
            delay(500); // Poczekaj na callback
            if (wyslanoPomyslnie) break;
        }
        
        if (proba < 3) delay(1000);
    }
    
    if (wyslanoPomyslnie) {
        Serial.println("\n╔════════════════════════════════╗");
        Serial.println("║  ✅ DANE WYSŁANE POMYŚLNIE    ║");
        Serial.println("╚════════════════════════════════╝");
    } else {
        Serial.println("\n╔════════════════════════════════╗");
        Serial.println("║  ❌ NIE UDAŁO SIĘ WYSŁAĆ     ║");
        Serial.println("╚════════════════════════════════╝");
    }
    delay(5000);
    // Deep sleep do następnego pomiaru (co 30 minut)
    long sleepSeconds = 60;
    Serial.printf("\n💤 Deep sleep przez %ld s (%ld min)\n", sleepSeconds, sleepSeconds/60);
    delay(500);
    
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}