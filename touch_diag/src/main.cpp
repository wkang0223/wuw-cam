#include <Arduino.h>
#include <SPI.h>

// Standalone ST7789 + XPT2046 diagnostic. No camera, SD, WiFi, or production
// firmware is linked into this build.
static constexpr int PIN_MISO = 1;   // SDO + T_DO (B3)
static constexpr int PIN_MOSI = 2;   // SDI + T_DIN (B2)
static constexpr int PIN_TCS  = 42;
static constexpr int PIN_SCK  = 41;  // SCK + T_CLK (B1)
static constexpr int PIN_BL   = 48;
static constexpr int PIN_DC   = 47;
static constexpr int PIN_LCS  = 21;

static constexpr uint32_t LCD_HZ   = 10000000;
static constexpr uint32_t TOUCH_HZ = 250000;
static constexpr int LCD_W = 320;
static constexpr int LCD_H = 240;

static SPIClass bus(FSPI);

static void lcdCommand(uint8_t command, const uint8_t* data = nullptr,
                       size_t length = 0) {
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_LCS, LOW);
  bus.beginTransaction(SPISettings(LCD_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_DC, LOW);
  bus.transfer(command);
  if (length) {
    digitalWrite(PIN_DC, HIGH);
    bus.writeBytes(data, length);
  }
  bus.endTransaction();
  digitalWrite(PIN_LCS, HIGH);
}

static void lcdWindow(int x0, int y0, int x1, int y1) {
  uint8_t x[] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
  uint8_t y[] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
  lcdCommand(0x2A, x, sizeof(x));
  lcdCommand(0x2B, y, sizeof(y));
  lcdCommand(0x2C);
}

static void lcdFillRect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_W) w = LCD_W - x;
  if (y + h > LCD_H) h = LCD_H - y;
  if (w <= 0 || h <= 0) return;

  lcdWindow(x, y, x + w - 1, y + h - 1);
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_LCS, LOW);
  digitalWrite(PIN_DC, HIGH);
  bus.beginTransaction(SPISettings(LCD_HZ, MSBFIRST, SPI_MODE0));
  uint8_t pixels[128];
  for (size_t i = 0; i < sizeof(pixels); i += 2) {
    pixels[i] = uint8_t(color >> 8);
    pixels[i + 1] = uint8_t(color);
  }
  size_t remaining = size_t(w) * size_t(h);
  while (remaining) {
    size_t count = remaining > 64 ? 64 : remaining;
    bus.writeBytes(pixels, count * 2);
    remaining -= count;
  }
  bus.endTransaction();
  digitalWrite(PIN_LCS, HIGH);
}

static void lcdInit() {
  digitalWrite(PIN_LCS, HIGH);
  digitalWrite(PIN_TCS, HIGH);
  delay(150);
  lcdCommand(0x01); // software reset
  delay(150);
  lcdCommand(0x11); // sleep out
  delay(120);
  const uint8_t colorMode = 0x55; // RGB565
  lcdCommand(0x3A, &colorMode, 1);
  const uint8_t madctl = 0x68; // landscape, BGR order for this ST7789 module
  lcdCommand(0x36, &madctl, 1);
  lcdCommand(0x20); // inversion off; measured correct for this ST7789 module
  lcdCommand(0x13); // normal display mode
  lcdCommand(0x29); // display on
  delay(20);

  lcdFillRect(0, 0, LCD_W, LCD_H, 0x0000);
  lcdFillRect(0, 0, LCD_W, 8, 0x07FF);
  lcdFillRect(0, LCD_H - 8, LCD_W, 8, 0x07FF);
  lcdFillRect(0, 8, 8, LCD_H - 16, 0x07FF);
  lcdFillRect(LCD_W - 8, 8, 8, LCD_H - 16, 0x07FF);
  // Reference bars: red, green, blue. These verify color order at a glance.
  lcdFillRect(108, 10, 32, 8, 0xF800);
  lcdFillRect(144, 10, 32, 8, 0x07E0);
  lcdFillRect(180, 10, 32, 8, 0x001F);
}

static uint16_t xptTransfer(uint8_t command) {
  bus.transfer(command);
  uint16_t value = uint16_t(bus.transfer(0x00)) << 8;
  value |= bus.transfer(0x00);
  return (value >> 3) & 0x0FFF;
}

static uint16_t xptRead(uint8_t command) {
  digitalWrite(PIN_LCS, HIGH);
  digitalWrite(PIN_TCS, LOW);
  bus.beginTransaction(SPISettings(TOUCH_HZ, MSBFIRST, SPI_MODE0));
  delayMicroseconds(20);
  // This board carries an HR2046-compatible clone: only the first conversion
  // after CS falls tracks position. A second conversion returns a plausible
  // but pressure-correlated value, which looks like bad calibration.
  uint16_t value = xptTransfer(command);
  bus.endTransaction();
  digitalWrite(PIN_TCS, HIGH);
  delayMicroseconds(60);
  return value;
}

struct TouchRaw {
  uint16_t x;
  uint16_t y;
  uint16_t z1;
  uint16_t z2;
};

static TouchRaw readTouch() {
  // Median of five rejects the occasional transition sample without buffers.
  uint16_t xs[5], ys[5], z1s[5], z2s[5];
  for (int i = 0; i < 5; ++i) {
    z1s[i] = xptRead(0xB1);
    xs[i]  = xptRead(0xD1);
    ys[i]  = xptRead(0x91);
    z2s[i] = xptRead(0xC1);
  }
  auto median = [](uint16_t* values) {
    for (int i = 1; i < 5; ++i) {
      uint16_t v = values[i];
      int j = i - 1;
      while (j >= 0 && values[j] > v) {
        values[j + 1] = values[j];
        --j;
      }
      values[j + 1] = v;
    }
    return values[2];
  };
  return {median(xs), median(ys), median(z1s), median(z2s)};
}

static bool looksStuck(const TouchRaw& t) {
  const bool allLow = t.x < 8 && t.y < 8 && t.z1 < 8 && t.z2 < 8;
  const bool allHigh = t.x > 4087 && t.y > 4087 && t.z1 > 4087 && t.z2 > 4087;
  return allLow || allHigh;
}

static bool isPressed(const TouchRaw& t) {
  return !looksStuck(t) && t.z1 > 80 && t.x > 100 && t.x < 4000 &&
         t.y > 100 && t.y < 4000;
}

struct Calibration {
  float x[3] = {};
  float y[3] = {};
  bool valid = false;
};

static constexpr int CAL_COUNT = 5;
static constexpr int CAL_FIT_COUNT = 4; // center is validation, not fit input
static constexpr uint16_t CAL_MIN_Z1 = 800;
static const int16_t targetX[CAL_COUNT] = {28, LCD_W - 29, LCD_W - 29, 28, LCD_W / 2};
static const int16_t targetY[CAL_COUNT] = {28, 28, LCD_H - 29, LCD_H - 29, LCD_H / 2};
static TouchRaw rawPoint[CAL_COUNT] = {};
static Calibration calibration;

static void drawTarget(int x, int y, uint16_t color) {
  lcdFillRect(x - 12, y - 2, 25, 5, color);
  lcdFillRect(x - 2, y - 12, 5, 25, color);
  lcdFillRect(x - 5, y - 5, 11, 11, color);
  lcdFillRect(x - 2, y - 2, 5, 5, 0x0000);
}

static bool solve3(float m[3][4], float out[3]) {
  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 3; ++row)
      if (fabsf(m[row][col]) > fabsf(m[pivot][col])) pivot = row;
    if (fabsf(m[pivot][col]) < 0.000001f) return false;
    if (pivot != col)
      for (int j = col; j < 4; ++j) {
        float tmp = m[col][j]; m[col][j] = m[pivot][j]; m[pivot][j] = tmp;
      }
    float divisor = m[col][col];
    for (int j = col; j < 4; ++j) m[col][j] /= divisor;
    for (int row = 0; row < 3; ++row) {
      if (row == col) continue;
      float factor = m[row][col];
      for (int j = col; j < 4; ++j) m[row][j] -= factor * m[col][j];
    }
  }
  for (int i = 0; i < 3; ++i) out[i] = m[i][3];
  return true;
}

static bool calculateCalibration() {
  auto distance2 = [](const TouchRaw& a, const TouchRaw& b) {
    int32_t dx = int32_t(a.x) - int32_t(b.x);
    int32_t dy = int32_t(a.y) - int32_t(b.y);
    return dx * dx + dy * dy;
  };
  const int32_t minEdge2 = 900 * 900;
  if (distance2(rawPoint[0], rawPoint[1]) < minEdge2 ||
      distance2(rawPoint[1], rawPoint[2]) < minEdge2 ||
      distance2(rawPoint[2], rawPoint[3]) < minEdge2 ||
      distance2(rawPoint[3], rawPoint[0]) < minEdge2) {
    Serial.println("CALIBRATION REJECTED - a corner was missed or repeated.");
    calibration.valid = false;
    return false;
  }

  // Least-squares affine transform: pixel = a*rawX + b*rawY + c.
  float normal[3][3] = {};
  float rhsX[3] = {};
  float rhsY[3] = {};
  for (int i = 0; i < CAL_FIT_COUNT; ++i) {
    float v[3] = {float(rawPoint[i].x), float(rawPoint[i].y), 1.0f};
    for (int row = 0; row < 3; ++row) {
      rhsX[row] += v[row] * targetX[i];
      rhsY[row] += v[row] * targetY[i];
      for (int col = 0; col < 3; ++col) normal[row][col] += v[row] * v[col];
    }
  }
  float mx[3][4], my[3][4];
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) mx[row][col] = my[row][col] = normal[row][col];
    mx[row][3] = rhsX[row];
    my[row][3] = rhsY[row];
  }
  if (!solve3(mx, calibration.x) || !solve3(my, calibration.y)) {
    Serial.println("CALIBRATION REJECTED - singular point geometry.");
    calibration.valid = false;
    return false;
  }

  float maxCornerError = 0.0f;
  for (int i = 0; i < CAL_FIT_COUNT; ++i) {
    float px = calibration.x[0] * rawPoint[i].x +
               calibration.x[1] * rawPoint[i].y + calibration.x[2];
    float py = calibration.y[0] * rawPoint[i].x +
               calibration.y[1] * rawPoint[i].y + calibration.y[2];
    float error = hypotf(px - targetX[i], py - targetY[i]);
    if (error > maxCornerError) maxCornerError = error;
  }
  float centerX = calibration.x[0] * rawPoint[4].x +
                  calibration.x[1] * rawPoint[4].y + calibration.x[2];
  float centerY = calibration.y[0] * rawPoint[4].x +
                  calibration.y[1] * rawPoint[4].y + calibration.y[2];
  float centerError = hypotf(centerX - targetX[4], centerY - targetY[4]);
  if (maxCornerError > 18.0f || centerError > 25.0f) {
    Serial.printf("CALIBRATION REJECTED - corner %.1fpx, center %.1fpx.\n",
                  maxCornerError, centerError);
    calibration.valid = false;
    return false;
  }
  calibration.valid = true;
  return true;
}

static void calibratedPixel(const TouchRaw& t, int& px, int& py) {
  px = lroundf(calibration.x[0] * t.x + calibration.x[1] * t.y + calibration.x[2]);
  py = lroundf(calibration.y[0] * t.x + calibration.y[1] * t.y + calibration.y[2]);
  px = constrain(px, 0, LCD_W - 1);
  py = constrain(py, 0, LCD_H - 1);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("WUW TOUCH DIAGNOSTIC");
  Serial.println("ST7789 + XPT2046, shared SPI, T_IRQ unused");
  Serial.println("SCK=41 MOSI=2 MISO=1 LCD_CS=21 T_CS=42 DC=47 BL=48");
  Serial.println("Touch SPI fixed at 250 kHz.");
  Serial.println("Press and HOLD each yellow target until it turns green.");

  pinMode(PIN_LCS, OUTPUT);
  pinMode(PIN_TCS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_LCS, HIGH);
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_BL, HIGH);
  bus.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  lcdInit();
  drawTarget(targetX[0], targetY[0], 0xFFE0);
}

void loop() {
  static uint32_t nextSample = 0;
  static uint32_t sampleCount = 0;
  static int point = 0;
  static bool waitForRelease = false;
  static uint32_t sumX = 0, sumY = 0, sumZ1 = 0, sumZ2 = 0;
  static uint8_t heldSamples = 0;
  static int oldX = -1;
  static int oldY = -1;
  if (millis() < nextSample) return;
  nextSample = millis() + 40;

  TouchRaw t = readTouch();
  bool down = isPressed(t);
  ++sampleCount;

  if (!calibration.valid) {
    if (waitForRelease) {
      if (!down) {
        waitForRelease = false;
        if (point < CAL_COUNT) drawTarget(targetX[point], targetY[point], 0xFFE0);
      }
    } else if (down && t.z1 >= CAL_MIN_Z1) {
      sumX += t.x; sumY += t.y; sumZ1 += t.z1; sumZ2 += t.z2;
      if (++heldSamples >= 12) {
        rawPoint[point] = {uint16_t(sumX / heldSamples), uint16_t(sumY / heldSamples),
                           uint16_t(sumZ1 / heldSamples), uint16_t(sumZ2 / heldSamples)};
        Serial.printf("point %d screen=%d,%d raw=%u,%u z1=%u z2=%u\n", point + 1,
                      targetX[point], targetY[point], rawPoint[point].x,
                      rawPoint[point].y, rawPoint[point].z1, rawPoint[point].z2);
        drawTarget(targetX[point], targetY[point], 0x07E0);
        ++point;
        heldSamples = 0; sumX = sumY = sumZ1 = sumZ2 = 0;
        waitForRelease = true;
        if (point == CAL_COUNT) {
          if (calculateCalibration()) {
            Serial.printf("CAL_X={%.8f, %.8f, %.4f}\n", calibration.x[0], calibration.x[1], calibration.x[2]);
            Serial.printf("CAL_Y={%.8f, %.8f, %.4f}\n", calibration.y[0], calibration.y[1], calibration.y[2]);
            int checkX, checkY;
            calibratedPixel(rawPoint[4], checkX, checkY);
            Serial.printf("CENTER CHECK expected=%d,%d measured=%d,%d error=%dpx\n",
                          targetX[4], targetY[4], checkX, checkY,
                          int(hypotf(float(checkX - targetX[4]), float(checkY - targetY[4]))));
            Serial.println("CALIBRATION COMPLETE - drag anywhere; green cursor should follow.");
            lcdFillRect(8, 8, LCD_W - 16, LCD_H - 16, 0x0000);
          } else {
            Serial.println("CALIBRATION FAILED - lift, then retry point 1.");
            lcdFillRect(0, 0, LCD_W, 8, 0xF800);
            lcdFillRect(0, LCD_H - 8, LCD_W, 8, 0xF800);
            lcdFillRect(0, 8, 8, LCD_H - 16, 0xF800);
            lcdFillRect(LCD_W - 8, 8, 8, LCD_H - 16, 0xF800);
            delay(600);
            lcdFillRect(0, 0, LCD_W, LCD_H, 0x0000);
            lcdFillRect(0, 0, LCD_W, 8, 0x07FF);
            lcdFillRect(0, LCD_H - 8, LCD_W, 8, 0x07FF);
            lcdFillRect(0, 8, 8, LCD_H - 16, 0x07FF);
            lcdFillRect(LCD_W - 8, 8, 8, LCD_H - 16, 0x07FF);
            point = 0;
          }
        }
      }
    } else {
      heldSamples = 0; sumX = sumY = sumZ1 = sumZ2 = 0;
    }
  } else if (down) {
    int px, py;
    calibratedPixel(t, px, py);
    if (oldX >= 0) {
      lcdFillRect(oldX - 5, oldY - 1, 11, 3, 0x0000);
      lcdFillRect(oldX - 1, oldY - 5, 3, 11, 0x0000);
    }
    lcdFillRect(px - 5, py - 1, 11, 3, 0x07E0);
    lcdFillRect(px - 1, py - 5, 3, 11, 0x07E0);
    oldX = px; oldY = py;
    Serial.printf("raw=%u,%u z1=%u -> pixel=%d,%d\n", t.x, t.y, t.z1, px, py);
  }

  if (looksStuck(t) || (sampleCount % 50 == 0 && !calibration.valid)) {
    Serial.printf("raw x=%4u y=%4u z1=%4u z2=%4u  %s  samples=%lu\n",
                  t.x, t.y, t.z1, t.z2,
                  looksStuck(t) ? "MISO_STUCK" : (down ? "TOUCH" : "idle"),
                  static_cast<unsigned long>(sampleCount));
  }
}
