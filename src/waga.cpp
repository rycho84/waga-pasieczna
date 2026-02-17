#include <esp_now.h>
#include <WiFi.h>

// Adres MAC centrali (TTGO T-Call) - ZASTĄP RZECZYWISTYM!
uint8_t broadcastAddress[] = {0xFC, 0xB4, 0x67, 0x67, 0xAB, 0x18};

// Struktura nadawcza
typedef struct struct_message {
  char text[32];
} struct_message;

struct_message myData;

// Callback po wysłaniu - poprawiona sygnatura dla ESP32-C6
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  Serial.print("Status wysyłania: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Sukces" : "Porażka");
}

void setup() {
  Serial.begin(115200);
  Serial.println("Waga ESP-NOW uruchomiona");

  // Tryb STA
  WiFi.mode(WIFI_STA);

  // Inicjalizacja ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Błąd inicjalizacji ESP-NOW");
    return;
  }

  // Rejestracja callbacka wysyłania (z nową sygnaturą)
  esp_now_register_send_cb(OnDataSent);

  // Dodanie centrali jako peera
  esp_now_peer_info_t peerInfo;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Błąd dodawania peera");
    return;
  }
}

void loop() {
  // Symulacja odczytu z wagi co 5 sekund
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 5000) {
    lastSend = millis();
    
    // Przykładowa wartość (można zastąpić rzeczywistym odczytem)
    int weight = 34; // tu można wczytać z czujnika
    snprintf(myData.text, sizeof(myData.text), "w=%d", weight);

    // Wysłanie danych
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
    if (result == ESP_OK) {
      Serial.println("Wysłano: " + String(myData.text));
    } else {
      Serial.println("Błąd wysyłania");
    }
  }
}