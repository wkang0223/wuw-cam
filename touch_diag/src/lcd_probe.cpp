#include <Arduino.h>

static constexpr int PIN_MISO = 1;
static constexpr int PIN_MOSI = 2;
static constexpr int PIN_SCK = 41;
static constexpr int PIN_TCS = 42;
static constexpr int PIN_BL = 48;
static constexpr int PIN_DC = 47;
static constexpr int PIN_CS = 21;
static constexpr int PIN_RST = 20;

static void writeByte(uint8_t value) {
  for (int bit = 7; bit >= 0; --bit) {
    digitalWrite(PIN_MOSI, value & (1U << bit));
    delayMicroseconds(10);
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_SCK, LOW);
  }
}

static uint8_t readByte() {
  uint8_t value = 0;
  for (int bit = 0; bit < 8; ++bit) {
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(10);
    value = uint8_t((value << 1) | digitalRead(PIN_MISO));
    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(10);
  }
  return value;
}

static void readRegister(uint8_t reg, int dummyBytes, int count) {
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, LOW);
  writeByte(reg);
  digitalWrite(PIN_DC, HIGH);
  for (int i = 0; i < dummyBytes; ++i) (void)readByte();
  Serial.printf("reg 0x%02X:", reg);
  for (int i = 0; i < count; ++i) Serial.printf(" %02X", readByte());
  Serial.println();
  digitalWrite(PIN_CS, HIGH);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_MISO, INPUT_PULLUP);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_TCS, OUTPUT);
  pinMode(PIN_BL, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_SCK, LOW);
  digitalWrite(PIN_MOSI, LOW);
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_BL, HIGH);
  digitalWrite(PIN_RST, LOW);
  delay(150);
  digitalWrite(PIN_RST, HIGH);
  delay(300);

  Serial.println("\nWUW TFT READBACK PROBE");
  Serial.println("MISO=1 MOSI=2 SCK=41 CS=21 DC=47 RESET=20 TCS=42");
  readRegister(0x04, 1, 3);  // Read display ID.
  readRegister(0x09, 1, 4);  // Read display status.
  readRegister(0x0A, 1, 1);  // Read power mode.
  readRegister(0x0B, 1, 1);  // Read MADCTL.
  readRegister(0x0C, 1, 1);  // Read pixel format.
  readRegister(0x0F, 1, 1);  // Read self-diagnostic result.
  readRegister(0xD3, 1, 4);  // Common ILI controller ID register.
  Serial.println("Probe complete");
}

void loop() {
  delay(1000);
}
