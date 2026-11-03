#include <Wire.h>
#include <BleMouse.h>
#include "esp_sleep.h"

#define SDA_PIN    8
#define SCL_PIN    9
#define TOUCH_PIN  4

// ---------- Tuning ----------
const bool  TOGGLE_MODE   = true;    // true = your module (output flips each touch)
const float SPEEDS[]      = {2.5, 5.0, 8.0, 12.0};   // slow ... very fast
const int   NUM_SPEEDS    = 4;
int         speedIndex    = 1;       // start at "medium"
const float DEADZONE      = 0.05;    // rad/s
const unsigned long TAP_WINDOW_MS = 350;
const unsigned long DEBOUNCE_MS   = 40;
const unsigned long IDLE_MS       = 60000;
const int   LOOP_DELAY_MS = 10;

BleMouse bleMouse("AirRing", "DIY", 100);

uint8_t mpuAddr = 0;
bool mpuOk = false;
float biasX = 0, biasZ = 0;
unsigned long lastActivity = 0;

// touch / gesture state
bool lastTouchState = false;
unsigned long lastChangeTime = 0;
int tapCount = 0;
unsigned long lastTapTime = 0;
bool dragging = false;

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool readGyro(float &gx, float &gz) {
  Wire.beginTransmission(mpuAddr);
  Wire.write(0x43);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)mpuAddr, 6) != 6) return false;
  int16_t rx = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();
  int16_t rz = (Wire.read() << 8) | Wire.read();
  const float k = 0.0174533f / 65.5f;
  gx = rx * k;
  gz = rz * k;
  return true;
}

bool initMPU() {
  for (uint8_t addr : {0x68, 0x69}) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      mpuAddr = addr;
      Wire.beginTransmission(addr);
      Wire.write(0x75);
      Wire.endTransmission(false);
      Wire.requestFrom((int)addr, 1);
      uint8_t id = Wire.read();
      Serial.printf("I2C device at 0x%02X, WHO_AM_I = 0x%02X\n", addr, id);
      mpuWrite(0x6B, 0x00);
      delay(100);
      mpuWrite(0x1B, 0x08);
      mpuWrite(0x1A, 0x04);
      return true;
    }
  }
  return false;
}

void calibrateGyro() {
  float sx = 0, sz = 0, gx, gz;
  int n = 0;
  for (int i = 0; i < 200; i++) {
    if (readGyro(gx, gz)) { sx += gx; sz += gz; n++; }
    delay(5);
  }
  if (n) { biasX = sx / n; biasZ = sz / n; }
}

void goToSleep() {
  Serial.println("Idle - going to deep sleep");
  if (dragging) { bleMouse.release(MOUSE_LEFT); dragging = false; }
  if (mpuOk) mpuWrite(0x6B, 0x40);
  // Wake on the opposite of the current level, so it works in toggle mode too
  bool high = digitalRead(TOUCH_PIN);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN,
      high ? ESP_GPIO_WAKEUP_GPIO_LOW : ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// Called once per detected tap
void onTap() {
  lastActivity = millis();
  if (dragging) {                       // any tap ends a drag
    bleMouse.release(MOUSE_LEFT);
    dragging = false;
    tapCount = 0;
    Serial.println("Drag released");
    return;
  }
  tapCount++;
  lastTapTime = millis();
}

// Decide what the finished tap sequence means
void handleTaps() {
  if (tapCount == 0 || dragging) return;
  if (millis() - lastTapTime < TAP_WINDOW_MS) return;

  switch (tapCount) {
    case 1:
      bleMouse.click(MOUSE_LEFT);
      Serial.println("Click");
      break;
    case 2:
      bleMouse.press(MOUSE_LEFT);
      dragging = true;
      Serial.println("Drag started - move to select, tap to release");
      break;
    case 3:
      bleMouse.click(MOUSE_LEFT);
      delay(60);
      bleMouse.click(MOUSE_LEFT);
      Serial.println("Double click");
      break;
    default:
      speedIndex = (speedIndex + 1) % NUM_SPEEDS;
      Serial.printf("Speed level %d (x%.1f)\n", speedIndex + 1, SPEEDS[speedIndex]);
      break;
  }
  tapCount = 0;
  lastActivity = millis();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Booting...");
  pinMode(TOUCH_PIN, INPUT);
  lastTouchState = digitalRead(TOUCH_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);

  mpuOk = initMPU();
  if (mpuOk) {
    delay(500);
    calibrateGyro();
    Serial.println("Gyro ready and calibrated");
  } else {
    Serial.println("NO I2C device found at 0x68/0x69 - wiring problem");
  }

  bleMouse.begin();
  Serial.println("BLE started, advertising as AirRing");
  lastActivity = millis();
}

void loop() {
  static unsigned long lastPrint = 0;
  bool doPrint = millis() - lastPrint > 1000;
  if (doPrint) lastPrint = millis();

  if (bleMouse.isConnected()) {
    // ----- Cursor movement -----
    if (mpuOk) {
      float gx, gz;
      if (readGyro(gx, gz)) {
        gx -= biasX;
        gz -= biasZ;
        if (fabs(gx) < DEADZONE) gx = 0;
        if (fabs(gz) < DEADZONE) gz = 0;

        float s = SPEEDS[speedIndex];
        int dx = constrain((int)(-gz * s), -127, 127);
        int dy = constrain((int)(-gx * s), -127, 127);

        if (dx || dy) {
          bleMouse.move(dx, dy);
          lastActivity = millis();
        }
      }
    }

    // ----- Touch tap detection -----
    bool touch = digitalRead(TOUCH_PIN);
    if (touch != lastTouchState && millis() - lastChangeTime > DEBOUNCE_MS) {
      lastChangeTime = millis();
      bool isTap = TOGGLE_MODE ? true : touch;   // momentary: only count the press
      lastTouchState = touch;
      if (isTap) onTap();
    }

    handleTaps();

    if (millis() - lastActivity > IDLE_MS) goToSleep();
  } else {
    lastTouchState = digitalRead(TOUCH_PIN);     // avoid false taps after pairing
    if (doPrint) Serial.println("Waiting for pairing...");
  }

  delay(LOOP_DELAY_MS);
}