#include <Arduino.h>

#define TFT_RST 11

void setup() {
    Serial.begin(115200);

    pinMode(TFT_RST, OUTPUT);
}

void loop() {
    Serial.println("RESET LOW");
    digitalWrite(TFT_RST, LOW);
    delay(3000);

    Serial.println("RESET HIGH");
    digitalWrite(TFT_RST, HIGH);
    delay(3000);
}