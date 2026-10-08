#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>

static constexpr int PIN_MISO = 1;
static constexpr int PIN_MOSI = 2;
static constexpr int PIN_SCK  = 41;
static constexpr int PIN_TCS  = 42;
static constexpr int PIN_BL   = 48;
static constexpr int PIN_DC   = 47;
static constexpr int PIN_CS   = 21;
// TFT RESET is permanently tied to 3.3 V for this diagnostic.
static constexpr int PIN_RST  = -1;
static constexpr uint32_t LCD_HZ = 1000000;

class SweepDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus;
  lgfx::Panel_ST7789 st7789;
  lgfx::Panel_ILI9342 ili9342;
  lgfx::Panel_ILI9341 ili9341;
  lgfx::Panel_ST7796 st7796;

 public:
  SweepDisplay() {
    auto cfg = bus.config();
    cfg.spi_host = SPI2_HOST;
    cfg.spi_mode = 0;
    cfg.freq_write = LCD_HZ;
    cfg.freq_read = 1000000;
    cfg.spi_3wire = false;
    cfg.use_lock = true;
    cfg.dma_channel = 0;
    cfg.pin_sclk = PIN_SCK;
    cfg.pin_mosi = PIN_MOSI;
    cfg.pin_miso = PIN_MISO;
    cfg.pin_dc = PIN_DC;
    bus.config(cfg);
  }

  bool select(uint8_t kind, bool swapPins) {
    auto busCfg = bus.config();
    busCfg.pin_dc = swapPins ? PIN_CS : PIN_DC;
    bus.config(busCfg);

    lgfx::Panel_Device* panel = nullptr;
    switch (kind) {
      case 0: panel = &st7789; break;
      case 1: panel = &ili9342; break;
      case 2: panel = &ili9341; break;
      default: panel = &st7796; break;
    }
    auto panelCfg = panel->config();
    panelCfg.pin_cs = swapPins ? PIN_DC : PIN_CS;
    panelCfg.pin_rst = PIN_RST;
    panelCfg.pin_busy = -1;
    panelCfg.bus_shared = true;
    panelCfg.readable = false;
    panelCfg.invert = false;
    panelCfg.rgb_order = false;
    if (kind == 0) {
      panelCfg.panel_width = 240;
      panelCfg.panel_height = 320;
    }
    panel->config(panelCfg);
    panel->setBus(&bus);
    setPanel(panel);

    if (!init()) return false;
    writeCommand(0x01);
    delay(150);
    if (!init_without_reset(true)) return false;
    setRotation(1);
    return true;
  }
};

static SweepDisplay lcd;
static const char* const DRIVER[] = {"ST7789", "ILI9342", "ILI9341", "ST7796"};
static const uint16_t FRAME[] = {0xFFE0, 0x07FF, 0xF81F, 0xFD20};

static void paint(uint8_t kind, bool swapped) {
  lcd.fillScreen(0x0000);
  int w = lcd.width();
  int h = lcd.height();
  lcd.drawRect(0, 0, w, h, FRAME[kind]);
  lcd.drawRect(2, 2, w - 4, h - 4, FRAME[kind]);
  lcd.fillRect(12, 14, (w - 32) / 3, 42, 0xF800);
  lcd.fillRect(16 + (w - 32) / 3, 14, (w - 32) / 3, 42, 0x07E0);
  lcd.fillRect(20 + 2 * ((w - 32) / 3), 14, (w - 32) / 3, 42, 0x001F);
  lcd.setTextColor(0xFFFF, 0x0000);
  lcd.setTextSize(3);
  lcd.setCursor(16, 82);
  lcd.print(DRIVER[kind]);
  lcd.setTextSize(2);
  lcd.setCursor(16, 122);
  lcd.print(swapped ? "CS47 DC21" : "CS21 DC47");
  lcd.setCursor(16, 152);
  lcd.print("S3 SCREEN SWEEP");
  lcd.fillRect(w / 2 - 8, h - 38, 16, 16, 0xFFFF);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nWUW SCREEN CONTROLLER SWEEP");
  Serial.println("No camera, WiFi, SD, touch polling, or PSRAM");
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_TCS, OUTPUT);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_TCS, HIGH);
  digitalWrite(PIN_BL, LOW);
  delay(500);
  digitalWrite(PIN_BL, HIGH);

  Serial.println("[FIXED] ST7789, RESET=3V3, CS21/DC47, 1 MHz");
  bool ok = lcd.select(0, false);
  Serial.printf("[FIXED] init %s, heap=%u\n", ok ? "OK" : "FAILED",
                (unsigned)ESP.getFreeHeap());
  if (ok) paint(0, false);
}

void loop() {
  delay(1000);
}
