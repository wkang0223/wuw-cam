/*
 * camtest.cpp — bare-metal hardware diagnostic for ESP32-CAM (OV3660)
 *
 * NO web server, NO WiFi, NO recording — just three questions, on repeat:
 *   1. Does anything ACK on the SCCB/I2C bus?   (sensor alive at all?)
 *   2. Does esp_camera_init() succeed with the ORIGINAL working config?
 *   3. Does the SD card mount?
 *
 * The scan repeats every 4 s, so you can gently press on the camera ribbon
 * connector / reseat the SD card and watch results change live.
 *
 * Build & flash:  pio run -e camtest -t upload
 */

#include "Arduino.h"
#include "Wire.h"
#include "esp_camera.h"
#include "SD_MMC.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

// OV sensors only answer on SCCB while XCLK is running — so we generate the
// clock ourselves with LEDC, exactly like the camera driver does.
static void xclkOn() {
  ledcSetup(0, 20000000, 1);       // 20 MHz, 1-bit resolution
  ledcAttachPin(XCLK_GPIO_NUM, 0);
  ledcWrite(0, 1);                 // 50% duty
}
static void xclkOff() {
  ledcDetachPin(XCLK_GPIO_NUM);
}

// Full driver init with the ORIGINAL working configuration (QXGA, q8, 2 fb).
static void tryCameraInit() {
  static camera_config_t cfg;      // static → zero-init → deterministic
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0       = Y2_GPIO_NUM;
  cfg.pin_d1       = Y3_GPIO_NUM;
  cfg.pin_d2       = Y4_GPIO_NUM;
  cfg.pin_d3       = Y5_GPIO_NUM;
  cfg.pin_d4       = Y6_GPIO_NUM;
  cfg.pin_d5       = Y7_GPIO_NUM;
  cfg.pin_d6       = Y8_GPIO_NUM;
  cfg.pin_d7       = Y9_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = FRAMESIZE_QXGA;
  cfg.jpeg_quality = 8;
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;

  xclkOff();                       // hand the pin to the driver
  esp_err_t err = esp_camera_init(&cfg);
  if (err == ESP_OK) {
    sensor_t* s = esp_camera_sensor_get();
    Serial.printf("[CAM] INIT OK — sensor PID 0x%02x\n", s ? s->id.PID : 0);
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      Serial.printf("[CAM] FRAME OK — %ux%u, %u KB\n", fb->width, fb->height, fb->len / 1024);
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[CAM] init OK but frame grab FAILED");
    }
    esp_camera_deinit();
  } else {
    Serial.printf("[CAM] INIT FAILED: 0x%x\n", err);
  }
  xclkOn();                        // take the clock back for the next scan
  delay(100);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(500);
  Serial.println("\n== CAMTEST — ESP32-CAM hardware diagnostic ==");
  Serial.printf("PSRAM: %s (%u bytes)\n", psramFound() ? "YES" : "NO", ESP.getFreePsram());

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);   // sensor powered
  xclkOn();
  delay(300);                          // sensor power-up settle
}

void loop() {
  static int pass = 0;
  pass++;
  Serial.printf("\n--- pass %d ---\n", pass);

  // 1) SCCB bus scan — the rawest possible "is the sensor alive" test
  Wire.begin(SIOD_GPIO_NUM, SIOC_GPIO_NUM, 100000);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      Serial.printf("[SCCB] ACK at 0x%02X %s\n", a,
                    a == 0x3C ? "← OV3660" : (a == 0x30 ? "← OV2640" : ""));
    }
  }
  if (!found)
    Serial.println("[SCCB] NO ACK — sensor is not answering (ribbon / sensor power)");
  Wire.end();

  // 2) Full driver init — only worth attempting if the bus scan saw the chip
  if (found) tryCameraInit();

  // 3) SD card — remount from scratch each pass so reinsertion shows up live
  SD_MMC.end();
  if (SD_MMC.begin("/sdcard", true) && SD_MMC.cardType() != CARD_NONE)
    Serial.printf("[SD] OK — %llu GB\n", SD_MMC.cardSize() / (1024ULL * 1024 * 1024));
  else
    Serial.println("[SD] FAIL — no card or bad contact");

  delay(4000);
}
