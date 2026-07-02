#include <Arduino.h>
#include <Wire.h>
#include "RTClib.h"

#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22

RTC_DS3231 rtc;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    if (!rtc.begin()) {
        Serial.println("❌ DS3231 nie znaleziony!");
        while (1) delay(10);
    }

    // ====================================================
    // USTAW DATĘ I GODZINĘ TUTAJ:
    //         rok    mies  dzień  godz  min  sek
    rtc.adjust(DateTime(2026,  2,   27,   20,  47,  0));
    // ====================================================

    DateTime now = rtc.now();
    Serial.printf("✅ Ustawiono: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
}

void loop() {
    DateTime now = rtc.now();
    Serial.printf("🕐 %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
    delay(1000);
}
