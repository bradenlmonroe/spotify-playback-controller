/*
 * ESP32-S3 + SparkFun Rotary Encoder Breakout (A/B quadrature, button, RGB LED)
 *
 * WIRING (corrected — see note about GPIO6 conflict):
 *   Encoder A      -> GPIO 6
 *   Encoder B      -> GPIO 15
 *   Button         -> GPIO 8
 *   RGB Red        -> GPIO 4
 *   RGB Green      -> GPIO 5
 *   RGB Blue       -> GPIO 16
 *
 * NOTE: Your original wiring had encoder pin A AND one RGB channel both on
 * GPIO 6. That's a conflict — pick one signal per pin. This code assumes
 * the table above. If you rewire differently, just update the #defines.
 *
 * Behavior:
 *   - Turning the knob increments/decrements a counter (quadrature decoded
 *     in an interrupt, so it won't miss steps even during fast spins).
 *   - Pressing the button toggles the RGB LED on/off.
 *   - While on, the RGB LED shows a hue that shifts based on the encoder
 *     position, just as a visual demo — swap fillColorFromPosition() for
 *     whatever behavior you actually want.
 */

#include <Arduino.h>

// ---------- Pin definitions ----------
#define PIN_ENC_A     6
#define PIN_ENC_B     15
#define PIN_BUTTON    8
#define PIN_RGB_R     4
#define PIN_RGB_G     5
#define PIN_RGB_B     16

// ---------- Encoder polarity / behavior ----------
// If turning the knob counts the wrong direction, swap PIN_ENC_A/PIN_ENC_B
// above (no code changes needed), or flip the sign in the ISR table below.

// ---------- PWM (LEDC) config for RGB ----------
#define PWM_FREQ        5000
#define PWM_RESOLUTION  8      // 8-bit -> 0-255
#define PWM_CH_R        0
#define PWM_CH_G        1
#define PWM_CH_B        2

// If your RGB LED is COMMON ANODE, set this to true so 0=full brightness,
// 255=off gets inverted correctly. Common cathode (most common) -> false.
#define RGB_COMMON_ANODE false

// ---------- Encoder state ----------
volatile int32_t encoderPosition = 0;
volatile uint8_t lastEncState = 0;

// Quadrature transition table: index = (prevAB << 2) | currAB
// Valid single-step transitions give +1 / -1, everything else (bounce /
// double-step) gives 0 so we don't miscount.
static const int8_t QEM[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

void IRAM_ATTR encoderISR() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  uint8_t currState = (a << 1) | b;
  uint8_t index = (lastEncState << 2) | currState;
  encoderPosition += QEM[index];
  lastEncState = currState;
}

// ---------- Button state (debounced) ----------
volatile bool buttonFlagPressed = false;
uint32_t lastButtonISRTime = 0;
bool ledOn = false;

void IRAM_ATTR buttonISR() {
  uint32_t now = millis();
  // simple debounce: ignore edges within 50ms of the last accepted one
  if (now - lastButtonISRTime > 50) {
    buttonFlagPressed = true;
    lastButtonISRTime = now;
  }
}

// ---------- RGB helpers ----------
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (RGB_COMMON_ANODE) {
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
  }
  ledcWrite(PWM_CH_R, r);
  ledcWrite(PWM_CH_G, g);
  ledcWrite(PWM_CH_B, b);
}

// Simple HSV -> RGB, hue in [0,255]
void hueToRGB(uint8_t hue, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = hue / 43;
  uint8_t remainder = (hue - (region * 43)) * 6;

  uint8_t p = 0;
  uint8_t q = 255 - remainder;
  uint8_t t = remainder;

  switch (region) {
    case 0:  r = 255; g = t;   b = p;   break;
    case 1:  r = q;   g = 255; b = p;   break;
    case 2:  r = p;   g = 255; b = t;   break;
    case 3:  r = p;   g = q;   b = 255; break;
    case 4:  r = t;   g = p;   b = 255; break;
    default: r = 255; g = p;   b = q;   break;
  }
}

void updateLEDFromPosition() {
  if (!ledOn) {
    setRGB(0, 0, 0);
    return;
  }
  uint8_t hue = (uint8_t)(encoderPosition & 0xFF); // wraps every 256 steps
  uint8_t r, g, b;
  hueToRGB(hue, r, g, b);
  setRGB(r, g, b);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  lastEncState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), buttonISR, FALLING);

  ledcSetup(PWM_CH_R, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(PWM_CH_G, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(PWM_CH_B, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PIN_RGB_R, PWM_CH_R);
  ledcAttachPin(PIN_RGB_G, PWM_CH_G);
  ledcAttachPin(PIN_RGB_B, PWM_CH_B);

  setRGB(0, 0, 0); // start off

  Serial.println("Rotary encoder + button + RGB ready.");
}

// ---------- Loop ----------
int32_t lastReportedPosition = 0;

void loop() {
  // Handle button press (toggle LED on/off)
  if (buttonFlagPressed) {
    buttonFlagPressed = false;
    ledOn = !ledOn;
    updateLEDFromPosition();
    Serial.printf("Button pressed. LED %s\n", ledOn ? "ON" : "OFF");
  }

  // Report + react to encoder movement
  int32_t pos;
  noInterrupts();
  pos = encoderPosition;
  interrupts();

  if (pos != lastReportedPosition) {
    lastReportedPosition = pos;
    Serial.printf("Encoder position: %ld\n", (long)pos);
    updateLEDFromPosition();
  }

  delay(5); // small idle delay; all real work happens in ISRs above
}
