#include <Arduino.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>

// Your CURRENT wiring
#define TFT_CS    4
#define TFT_RST   5
#define TFT_DC    6
#define TFT_MOSI  7
#define TFT_SCK   8

Arduino_DataBus *bus =
    new Arduino_HWSPI(TFT_DC, TFT_CS);

Arduino_GFX *gfx =
    new Arduino_ST7796(
        bus,
        TFT_RST,
        1,      // landscape
        false
    );

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("Starting");

    SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);

    Serial.println("SPI started");

    if (!gfx->begin(1000000)) {
        Serial.println("Display begin FAILED");
        while (true) {
            delay(1000);
        }
    }

    Serial.printf("Display initialized: %d x %d\n",
                  gfx->width(), gfx->height());

    Serial.println("RED");
    gfx->fillScreen(RED);
    delay(3000);

    Serial.println("GREEN");
    gfx->fillScreen(GREEN);
    delay(3000);

    Serial.println("BLUE");
    gfx->fillScreen(BLUE);
    delay(3000);

    Serial.println("WHITE");
    gfx->fillScreen(WHITE);
    delay(3000);

    Serial.println("BLACK");
    gfx->fillScreen(BLACK);
}

void loop()
{
}