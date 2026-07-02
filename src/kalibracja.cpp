#include <Arduino.h>
#include "HX711.h"

#define HX711_DT_PIN   2
#define HX711_SCK_PIN  3
#define HX711_VCC_PIN  4
//#define HX711_GND_PIN  5

HX711 scale;

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(HX711_VCC_PIN, OUTPUT);
    //pinMode(HX711_GND_PIN, OUTPUT);
    //digitalWrite(HX711_GND_PIN, LOW);
    digitalWrite(HX711_VCC_PIN, HIGH);
    delay(2000);

    scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
    scale.power_up();
    delay(500);

    Serial.println();
    Serial.println("Kalibracja HX711");
    Serial.println("Usredniony surowy odczyt z 50 pomiarow bedzie wyswietlany po wybudzeniu HX711 z power_down.");
}

void loop() {
   // scale.power_up();
   digitalWrite(HX711_VCC_PIN, HIGH);
    delay(500);
    if (scale.is_ready()) {
        //scale.read_average(5);
        long rawReading = scale.get_units(5);
        Serial.println(rawReading);
    } else {
        Serial.println("HX711 nie odpowiada");
    }
   // scale.power_down();
   digitalWrite(HX711_VCC_PIN, LOW);
    delay(2000);
}
