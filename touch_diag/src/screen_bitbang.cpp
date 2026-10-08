#include <Arduino.h>

static constexpr int PIN_MOSI = 2;
static constexpr int PIN_SCK = 41;
static constexpr int PIN_BL = 48;
static constexpr int PIN_DC = 47;
static constexpr int PIN_CS = 21;
static constexpr int PIN_TCS = 42;
static constexpr int PIN_RST = 20;

static inline void clockBit(bool value) {
  digitalWrite(PIN_MOSI, value ? HIGH : LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_SCK, HIGH);
  delayMicroseconds(4);
  digitalWrite(PIN_SCK, LOW);
}

static void writeByte(uint8_t value) {
  for (int bit = 7; bit >= 0; --bit) clockBit(value & (1U << bit));
}

static void sendCommand(uint8_t value, const uint8_t* args = nullptr,
                        size_t argCount = 0) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  writeByte(value);
  if (argCount) {
    digitalWrite(PIN_DC, HIGH);
    for (size_t i = 0; i < argCount; ++i) writeByte(args[i]);
  }
  digitalWrite(PIN_CS, HIGH);
}

static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t x[] = {uint8_t(x0 >> 8), uint8_t(x0),
                       uint8_t(x1 >> 8), uint8_t(x1)};
  const uint8_t y[] = {uint8_t(y0 >> 8), uint8_t(y0),
                       uint8_t(y1 >> 8), uint8_t(y1)};
  sendCommand(0x2A, x, sizeof(x));
  sendCommand(0x2B, y, sizeof(y));
}

static void fillBand(uint16_t x0, uint16_t x1, uint16_t color) {
  setWindow(x0, 0, x1, 239);
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, LOW);
  writeByte(0x2C);
  digitalWrite(PIN_DC, HIGH);
  const uint32_t pixels = uint32_t(x1 - x0 + 1) * 240UL;
  for (uint32_t i = 0; i < pixels; ++i) {
    writeByte(color >> 8);
    writeByte(color);
  }
  digitalWrite(PIN_CS, HIGH);
}

static void initST7789() {
  sendCommand(0x01);
  delay(200);
  sendCommand(0x11);
  delay(200);
  const uint8_t colorMode = 0x55;
  sendCommand(0x3A, &colorMode, 1);
  const uint8_t madctl = 0x68;
  sendCommand(0x36, &madctl, 1);
  sendCommand(0x20);
  sendCommand(0x13);
  sendCommand(0x29);
  delay(100);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_BL, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_TCS, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_SCK, LOW);
  digitalWrite(PIN_MOSI, LOW);
  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_BL, HIGH);
  digitalWrite(PIN_RST, LOW);
  delay(150);
  digitalWrite(PIN_RST, HIGH);
  delay(300);

  Serial.println("\nWUW ST7789 DIRECT GPIO TEST");
  Serial.println("RESET=20 MOSI=2 SCK=41 CS=21 DC=47 BL=48 TCS=42");
  initST7789();
  fillBand(0, 105, 0xF800);
  fillBand(106, 212, 0x07E0);
  fillBand(213, 319, 0x001F);
  Serial.println("RGB bands sent");
}

void loop() {
  delay(1000);
}
