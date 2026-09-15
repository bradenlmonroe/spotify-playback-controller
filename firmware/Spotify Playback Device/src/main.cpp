/*
 * ESP32-S3 + Elecrow 4" 480x320 ST7796 SPI TFT
 * Draws a simple box to confirm wiring/init before building real UI.
 *
 * Requires: "GFX Library for Arduino" by moononournation
 *   (Arduino IDE: Library Manager -> search "GFX Library for Arduino")
 *
 * Wiring:
 *   SCK    -> GPIO 14
 *   SDI/MOSI -> GPIO 13
 *   DC/RS  -> GPIO 12
 *   RESET  -> GPIO 11
 *   CS     -> GPIO 10
 *   LED (backlight control) -> GPIO 9  <-- PWM, not a fixed rail
 *   VCC    -> 5V
 *   GND    -> GND
 */

#include <Arduino_GFX_Library.h>

// ---------- Pin definitions ----------
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_DC   12
#define TFT_RST  11
#define TFT_CS   10
#define TFT_BACKLIGHT 9
// This display is write-only from the MCU's perspective, so MISO is unused.
#define TFT_MISO GFX_NOT_DEFINED

// ---------- Backlight PWM ----------
// The "LED" pin turned out to be a driver control input, not raw power -
// going to 5V blacked the screen out, which means it's logic-level and
// expects a PWM signal rather than a fixed voltage. This drives it from a
// GPIO instead so brightness is software-adjustable.
static const uint8_t BACKLIGHT_CH = 0;

void setBacklight(uint8_t brightness) { // 0-255
  ledcWrite(BACKLIGHT_CH, brightness);
}

// ---------- Bus + display objects ----------
// SPI_MODE0 is standard for ST7796. -1 lets the library pick a free HSPI/FSPI unit.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, -1 /* SPI host, auto */);

// 480x320 panel, no rotation, not IPS (adjust `false` to `true` and see which
// looks right if colors seem inverted/washed out on first boot).
Arduino_GFX *gfx = new Arduino_ST7796(
    bus, TFT_RST, 1 /* rotation: 0-3, 1 = landscape */, false /* IPS */,
    320, 480 /* native panel res before rotation */);

void setup() {
  Serial.begin(115200);
  delay(200);

  ledcSetup(BACKLIGHT_CH, 5000, 8); // 5 kHz, 8-bit duty
  ledcAttachPin(TFT_BACKLIGHT, BACKLIGHT_CH);
  setBacklight(255); // full brightness to start

  // Default SPI speed (up to 40MHz) can be too fast for long/loose jumper
  // wires and causes corrupted command bytes - showing up as streaks or
  // diagonal lines instead of clean shapes. Starting slow to confirm clean
  // communication, then you can raise this once it's working reliably.
  if (!gfx->begin(10000000)) { // 10 MHz
    Serial.println("Display init failed - check wiring/pins.");
    while (1) delay(1000);
  }

  // ---- Diagnostic: solid full-screen fills ----
  // If these come out clean (no streaks/lines), the panel is communicating
  // fine and the issue is narrower than a wiring/init problem. If these are
  // ALSO corrupted, it confirms something deeper (reset timing, a loose
  // DC/CS connection, etc.) rather than anything specific to drawing shapes.
  Serial.println("Filling RED...");
  gfx->fillScreen(RED);
  delay(1000);
  Serial.println("Filling GREEN...");
  gfx->fillScreen(GREEN);
  delay(1000);
  Serial.println("Filling BLUE...");
  gfx->fillScreen(BLUE);
  delay(1000);

  gfx->fillScreen(BLACK);

  // Draw a simple centered box: 200x120, white outline, 2px thick.
  int16_t boxW = 200, boxH = 120;
  int16_t x = (gfx->width()  - boxW) / 2;
  int16_t y = (gfx->height() - boxH) / 2;

  gfx->drawRect(x,     y,     boxW,     boxH,     WHITE);
  gfx->drawRect(x + 1, y + 1, boxW - 2, boxH - 2, WHITE);

  Serial.printf("Display up: %dx%d, box drawn at (%d,%d)\n",
                gfx->width(), gfx->height(), x, y);
}

void loop() {
  // Nothing yet - just holding the static box on screen.
}
