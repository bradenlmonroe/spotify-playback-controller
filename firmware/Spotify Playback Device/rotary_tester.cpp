/*
 * ESP32-S3 Rotary Encoder + Button + RGB LED
 * SparkFun rotary encoder breakout (quadrature A/B, push button, RGB LED)
 *
 * Wiring:
 *   Encoder A   -> GPIO 16
 *   Encoder B   -> GPIO 15
 *   Button      -> GPIO 8
 *   LED R       -> GPIO 4
 *   LED G       -> GPIO 5
 *   LED B       -> GPIO 6
 *   VCC         -> 3.3V (through your filtering caps)
 *   GND         -> GND
 *
 * Decoding uses a full quadrature state table (robust against
 * contact bounce / partial detents) rather than naive edge counting.
 */

#include <Arduino.h>

// ---------- Pin definitions ----------
static const uint8_t PIN_ENC_A  = 16;
static const uint8_t PIN_ENC_B  = 17;
static const uint8_t PIN_BUTTON = 39;
static const uint8_t PIN_LED_R  = 42;
static const uint8_t PIN_LED_G  = 40;
static const uint8_t PIN_LED_B  = 41;

// ---------- Quadrature state machine ----------
// States
#define R_START      0x0
#define R_CW_FINAL   0x1
#define R_CW_BEGIN   0x2
#define R_CW_NEXT    0x3
#define R_CCW_BEGIN  0x4
#define R_CCW_FINAL  0x5
#define R_CCW_NEXT   0x6
// Direction flags OR'd into the returned state
#define DIR_CW  0x10
#define DIR_CCW 0x20

static const uint8_t stateTable[7][4] = {
  /* R_START     */ {R_START,    R_CW_BEGIN,  R_CCW_BEGIN, R_START},
  /* R_CW_FINAL  */ {R_CW_NEXT,  R_START,     R_CW_FINAL,  R_START | DIR_CW},
  /* R_CW_BEGIN  */ {R_CW_NEXT,  R_CW_BEGIN,  R_START,     R_START},
  /* R_CW_NEXT   */ {R_CW_NEXT,  R_CW_BEGIN,  R_CW_FINAL,  R_START},
  /* R_CCW_BEGIN */ {R_CCW_NEXT, R_START,     R_CCW_BEGIN, R_START},
  /* R_CCW_FINAL */ {R_CCW_NEXT, R_CCW_FINAL, R_START,     R_START | DIR_CCW},
  /* R_CCW_NEXT  */ {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START}
};

volatile uint8_t  encState        = R_START;
volatile int32_t  encoderPosition = 0;
volatile int8_t   encoderDirection = 0; // +1 CW, -1 CCW (last confirmed step)

void IRAM_ATTR encoderISR() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  uint8_t pinState = (b << 1) | a;
  encState = stateTable[encState & 0x0F][pinState];
  uint8_t dir = encState & 0x30;
  if (dir == DIR_CW) {
    encoderPosition++;
    encoderDirection = 1;
  } else if (dir == DIR_CCW) {
    encoderPosition--;
    encoderDirection = -1;
  }
}

// ---------- Button (interrupt + debounce) ----------
// NOTE: on this encoder, SW shares the same internal "+" net as the RGB LED's
// common anode. At rest, SW reads LOW; pressing the button pulls it toward
// whatever voltage is on "+". So we use an internal pulldown and trigger on
// RISING - the opposite of the usual pullup/FALLING button pattern.
volatile bool    buttonFlagPressed = false;
volatile uint32_t lastButtonISRTime = 0;

void IRAM_ATTR buttonISR() {
  uint32_t now = millis();
  if (now - lastButtonISRTime > 30) { // 30ms debounce window
    buttonFlagPressed = true;
    lastButtonISRTime = now;
  }
}

// ---------- RGB LED (LEDC PWM) ----------
// Your toolchain is on Arduino-ESP32 core 2.x, which uses the
// channel-based LEDC API: ledcSetup(channel,...) + ledcAttachPin(pin,channel)
// + ledcWrite(channel,duty). (Core 3.x replaced this with pin-based
// ledcAttach()/ledcWrite(pin,...) — not what's installed here.)
static const uint8_t LEDC_CH_R = 0;
static const uint8_t LEDC_CH_G = 1;
static const uint8_t LEDC_CH_B = 2;

void setupLED() {
  ledcSetup(LEDC_CH_R, 5000, 8); // 5 kHz, 8-bit duty
  ledcSetup(LEDC_CH_G, 5000, 8);
  ledcSetup(LEDC_CH_B, 5000, 8);
  ledcAttachPin(PIN_LED_R, LEDC_CH_R);
  ledcAttachPin(PIN_LED_G, LEDC_CH_G);
  ledcAttachPin(PIN_LED_B, LEDC_CH_B);
}

// This is a common-anode RGB LED (shared "+" pin), so each color pin turns
// that color ON when pulled LOW, not HIGH. setColor() still takes normal
// "brightness" values (0 = off, 255 = full on) - the inversion happens here
// so the rest of the code doesn't have to think about it.
void setColor(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(LEDC_CH_R, 255 - r);
  ledcWrite(LEDC_CH_G, 255 - g);
  ledcWrite(LEDC_CH_B, 255 - b);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  // INPUT_PULLUP is harmless even though you already have external
  // pull-ups/filtering on the breakout; it just reinforces the idle-high level.
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLDOWN); // SW idles LOW, goes HIGH when pressed

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), buttonISR, RISING);

  setupLED();
  setColor(0, 0, 40); // dim blue = idle

  Serial.println("Rotary encoder ready.");
}

// ---------- Loop ----------
int32_t lastReportedPosition = 0;

void loop() {
  noInterrupts();
  int32_t pos = encoderPosition;
  int8_t  dir = encoderDirection;
  interrupts();

  if (pos != lastReportedPosition) {
    Serial.printf("Position: %ld  (dir %s)\n", (long)pos, dir > 0 ? "CW" : "CCW");
    lastReportedPosition = pos;

    if (dir > 0) {
      setColor(0, 40, 0);  // green flash for CW
    } else {
      setColor(40, 0, 0);  // red flash for CCW
    }
  }

  if (buttonFlagPressed) {
    buttonFlagPressed = false;
    Serial.println("Button pressed!");
    setColor(40, 40, 40); // white flash
    delay(80);
    setColor(0, 0, 40);   // back to idle
  }
}
