#include <esp_now.h>
#include <WiFi.h>

typedef struct struct_message {
  char text[32];
} struct_message;

struct_message incomingData;

// Stara sygnatura callbacka – wymagana w platformie 3.2.0
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  memcpy(&incomingData, data, sizeof(incomingData));
  Serial.print("Odebrano: ");
  Serial.println(incomingData.text);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Centrala ESP-NOW uruchomiona");

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Błąd inicjalizacji ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {}